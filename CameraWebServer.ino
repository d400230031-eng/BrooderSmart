/*
  ==========================================
       BROODER CONTROL SYSTEM
       ESP32-S3 N16R8 + OV3660
  ==========================================

  SENSOR:
  DHT22       GPIO14

  RELAY NC:
  KIPAS 1     GPIO41
  KIPAS 2     GPIO42
  LAMPU 1     GPIO47
  LAMPU 2     GPIO48

  OLED:
  SDA         GPIO1
  SCL         GPIO2

  TOMBOL:
  UP          GPIO39
  DOWN        GPIO40
  MENU        GPIO21

  LIBRARY:
  DHT sensor library by Adafruit
  Adafruit Unified Sensor
  Adafruit GFX Library
  Adafruit SSD1306

  FILE TAMBAHAN:
  board_config.h (pin kamera sesuai board)
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "DHT.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "board_config.h"

// ==========================================
// WIFI DAN SUPABASE
// ==========================================

const char *WIFI_SSID = "Lemott_2.4G";
const char *WIFI_PASSWORD = "lemot321";

const char *SUPABASE_CAMERA_URL =
"https://ezowvgtxgqvyqbzqbcwa.supabase.co/functions/v1/brooder-camera";

const char *DEVICE_TOKEN = "brooderr1333AAA$";

// ==========================================
// PIN ESP32-S3
// ==========================================

#define DHT_PIN 14

#define FAN1_PIN 41
#define FAN2_PIN 42

#define LAMP1_PIN 47
#define LAMP2_PIN 48

#define BUTTON_UP_PIN 39
#define BUTTON_DOWN_PIN 40
#define BUTTON_MENU_PIN 21

#define OLED_SDA_PIN 1
#define OLED_SCL_PIN 2

// ==========================================
// KONFIGURASI HARDWARE
// ==========================================

#define DHT_TYPE DHT22

#define OLED_WIDTH 128
#define OLED_HEIGHT 32
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

// Sesuaikan setelah menguji polaritas modul relay.
#define RELAY_ACTIVE_LEVEL LOW

// Semua output menggunakan terminal NC.
#define RELAYS_USE_NC true

// ==========================================
// PENGATURAN SUHU
// ==========================================

float temperatureSetpoint = 32.0;

const float SETPOINT_MIN = 25.0;
const float SETPOINT_MAX = 38.0;
const float SETPOINT_STEP = 0.5;

// Histeresis suhu.

const float LAMP_ON_BELOW = 0.5;
const float LAMP_OFF_ABOVE = 0.2;

const float FAN1_ON_ABOVE = 1.0;
const float FAN1_OFF_BELOW = 0.3;

const float FAN2_ON_ABOVE = 2.0;
const float FAN2_OFF_BELOW = 1.0;

// ==========================================
// TIMER
// ==========================================

const unsigned long SENSOR_INTERVAL_MS = 2500;
const unsigned long DISPLAY_INTERVAL_MS = 1000;

const unsigned long CAMERA_UPLOAD_INTERVAL_MS = 60000;

const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;

// ==========================================
// OBJEK
// ==========================================

DHT dht(DHT_PIN, DHT_TYPE);

Adafruit_SSD1306 display(
  OLED_WIDTH,
  OLED_HEIGHT,
  &Wire,
  OLED_RESET
);

// ==========================================
// STATUS SISTEM
// ==========================================

bool oledAvailable = false;
bool cameraAvailable = false;

bool autoMode = true;

bool fan1On = false;
bool fan2On = false;

bool lamp1On = false;
bool lamp2On = false;

float temperatureC = NAN;
float humidityPct = NAN;

// TIMER

unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastCameraUpload = 0;
unsigned long lastWiFiAttempt = 0;

// TOMBOL

uint8_t selectedRelay = 0;

bool previousMenu = HIGH;
bool previousUp = HIGH;
bool previousDown = HIGH;

unsigned long menuPressedAt = 0;
unsigned long lastButtonAction = 0;

bool menuLongPressHandled = false;

const char *relayNames[] = {
  "KIPAS 1",
  "KIPAS 2",
  "LAMPU 1",
  "LAMPU 2"
};

// ==========================================
// KONTROL RELAY NC
// ==========================================

void writeLoad(uint8_t pin, bool loadOn) {

  // NC:
  // Beban ON  = coil relay OFF
  // Beban OFF = coil relay ON

  bool coilEnergized =
    RELAYS_USE_NC ? !loadOn : loadOn;

  uint8_t inactiveLevel =
    (RELAY_ACTIVE_LEVEL == LOW) ? HIGH : LOW;

  digitalWrite(
    pin,
    coilEnergized ? RELAY_ACTIVE_LEVEL : inactiveLevel
  );
}

// ==========================================
// OUTPUT RELAY
// ==========================================

void applyRelays() {

  writeLoad(FAN1_PIN, fan1On);
  writeLoad(FAN2_PIN, fan2On);

  writeLoad(LAMP1_PIN, lamp1On);
  writeLoad(LAMP2_PIN, lamp2On);

}

// ==========================================
// MATIKAN SEMUA BEBAN
// ==========================================

void allLoadsOff() {

  fan1On = false;
  fan2On = false;

  lamp1On = false;
  lamp2On = false;

  applyRelays();

}

// ==========================================
// KONTROL RELAY MANUAL
// ==========================================

void setSelectedRelay(bool on) {

  switch (selectedRelay) {

    case 0:
      fan1On = on;
      break;

    case 1:
      fan2On = on;
      break;

    case 2:
      lamp1On = on;
      break;

    case 3:
      lamp2On = on;
      break;

  }

  applyRelays();

}

// ==========================================
// KONTROL OTOMATIS SUHU
// ==========================================

void runAutomaticControl() {

  if (!isfinite(temperatureC)) {

    allLoadsOff();

    return;

  }

  float lampOnAt =
    temperatureSetpoint - LAMP_ON_BELOW;

  float lampOffAt =
    temperatureSetpoint + LAMP_OFF_ABOVE;

  float fan1OnAt =
    temperatureSetpoint + FAN1_ON_ABOVE;

  float fan1OffAt =
    temperatureSetpoint + FAN1_OFF_BELOW;

  float fan2OnAt =
    temperatureSetpoint + FAN2_ON_ABOVE;

  float fan2OffAt =
    temperatureSetpoint + FAN2_OFF_BELOW;

  // LAMPU

  if (temperatureC < lampOnAt) {

    lamp1On = true;
    lamp2On = true;

  }

  else if (temperatureC > lampOffAt) {

    lamp1On = false;
    lamp2On = false;

  }

  // KIPAS 1

  if (temperatureC > fan1OnAt) {

    fan1On = true;

  }

  else if (temperatureC < fan1OffAt) {

    fan1On = false;

  }

  // KIPAS 2

  if (temperatureC > fan2OnAt) {

    fan2On = true;

  }

  else if (temperatureC < fan2OffAt) {

    fan2On = false;

  }

  // PENCEGAHAN PEMANASAN DAN PENDINGINAN BERSAMAAN

  if (fan1On || fan2On) {

    lamp1On = false;
    lamp2On = false;

  }

  applyRelays();

}

// ==========================================
// MEMBACA SENSOR DHT22
// ==========================================

void readSensors() {

  float h = dht.readHumidity();

  float t = dht.readTemperature();

  if (isfinite(t) && isfinite(h)) {

    temperatureC = t;
    humidityPct = h;

    Serial.printf(
      "DHT22: %.1f C | %.1f %% RH\n",
      t,
      h
    );

  }

  else {

    temperatureC = NAN;
    humidityPct = NAN;

    Serial.println(
      "ERROR: pembacaan DHT22 gagal."
    );

  }

  if (autoMode) {

    runAutomaticControl();

  }

}

// ==========================================
// TOMBOL UP DOWN MENU
// ==========================================

void handleButtons() {

  unsigned long now = millis();

  bool menu =
    digitalRead(BUTTON_MENU_PIN);

  bool up =
    digitalRead(BUTTON_UP_PIN);

  bool down =
    digitalRead(BUTTON_DOWN_PIN);

  // MENU DITEKAN

  if (previousMenu == HIGH && menu == LOW) {

    menuPressedAt = now;

    menuLongPressHandled = false;

  }

  // TAHAN MENU 1,8 DETIK
  // GANTI MODE AUTO / MANUAL

  if (
    menu == LOW &&
    !menuLongPressHandled &&
    now - menuPressedAt >= 1800
  ) {

    autoMode = !autoMode;

    menuLongPressHandled = true;

    if (autoMode) {

      runAutomaticControl();

    }

    else {

      allLoadsOff();

    }

    Serial.printf(
      "MODE: %s\n",
      autoMode ? "AUTO" : "MANUAL"
    );

  }

  // MENU SINGKAT
  // PILIH RELAY MANUAL

  if (
    previousMenu == LOW &&
    menu == HIGH &&
    !menuLongPressHandled &&
    now - lastButtonAction > 180
  ) {

    if (!autoMode) {

      selectedRelay =
        (selectedRelay + 1) % 4;

      Serial.printf(
        "PILIHAN: %s\n",
        relayNames[selectedRelay]
      );

    }

    lastButtonAction = now;

  }

  // TOMBOL UP

  if (
    previousUp == HIGH &&
    up == LOW &&
    now - lastButtonAction > 180
  ) {

    if (autoMode) {

      temperatureSetpoint =
        min(
          SETPOINT_MAX,
          temperatureSetpoint + SETPOINT_STEP
        );

      runAutomaticControl();

      Serial.printf(
        "SETPOINT: %.1f C\n",
        temperatureSetpoint
      );

    }

    else {

      setSelectedRelay(true);

      Serial.printf(
        "%s ON\n",
        relayNames[selectedRelay]
      );

    }

    lastButtonAction = now;

  }

  // TOMBOL DOWN

  if (
    previousDown == HIGH &&
    down == LOW &&
    now - lastButtonAction > 180
  ) {

    if (autoMode) {

      temperatureSetpoint =
        max(
          SETPOINT_MIN,
          temperatureSetpoint - SETPOINT_STEP
        );

      runAutomaticControl();

      Serial.printf(
        "SETPOINT: %.1f C\n",
        temperatureSetpoint
      );

    }

    else {

      setSelectedRelay(false);

      Serial.printf(
        "%s OFF\n",
        relayNames[selectedRelay]
      );

    }

    lastButtonAction = now;

  }

  previousMenu = menu;
  previousUp = up;
  previousDown = down;

}

// ==========================================
// TAMPILAN OLED
// ==========================================

void updateDisplay() {

  if (!oledAvailable) return;

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);

  display.setCursor(0, 0);

  display.println("BROODER CONTROL");

  display.drawLine(
    0, 10, 127, 10, SSD1306_WHITE
  );

  // SUHU

  display.setCursor(0, 14);

  display.print("Temp: ");

  if (isfinite(temperatureC)) {

    display.printf("%.1f C", temperatureC);

  }

  else {

    display.print("--.- C");

  }

  // KELEMBAPAN

  display.setCursor(0, 25);

  display.print("RH: ");

  if (isfinite(humidityPct)) {

    display.printf("%.1f %%", humidityPct);

  }

  else {

    display.print("--.- %");

  }

  // SETPOINT DAN MODE

  display.setCursor(0, 36);

  display.printf(
    "SP: %.1f C  %s",
    temperatureSetpoint,
    autoMode ? "AUTO" : "MANUAL"
  );

  // KIPAS

  display.setCursor(0, 48);

  display.printf(
    "F1:%s F2:%s",
    fan1On ? "ON" : "OFF",
    fan2On ? "ON" : "OFF"
  );

  // LAMPU

  display.setCursor(0, 57);

  display.printf(
    "L1:%s L2:%s",
    lamp1On ? "ON" : "OFF",
    lamp2On ? "ON" : "OFF"
  );

  display.display();

}

// ==========================================
// WIFI
// ==========================================

void connectWiFi() {

  if (WiFi.status() == WL_CONNECTED) {

    return;

  }

  unsigned long now = millis();

  if (
    now - lastWiFiAttempt <
    WIFI_RETRY_INTERVAL_MS
  ) {

    return;

  }

  lastWiFiAttempt = now;

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

}

// ==========================================
// STATUS WIFI
// ==========================================

void printWiFiStatus() {

  if (WiFi.status() == WL_CONNECTED) {

    Serial.print("WiFi connected. IP: ");

    Serial.println(WiFi.localIP());

  }

  else {

    Serial.println(
      "WiFi belum terhubung."
    );

  }

}

// ==========================================
// INISIALISASI KAMERA OV3660
// ==========================================

bool initializeCamera() {

  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;

  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = FRAMESIZE_VGA;

  config.jpeg_quality = 12;

  config.fb_count =
    psramFound() ? 2 : 1;

  config.grab_mode =
    psramFound()
    ? CAMERA_GRAB_LATEST
    : CAMERA_GRAB_WHEN_EMPTY;

  config.fb_location =
    psramFound()
    ? CAMERA_FB_IN_PSRAM
    : CAMERA_FB_IN_DRAM;

  esp_err_t err =
    esp_camera_init(&config);

  if (err != ESP_OK) {

    Serial.printf(
      "Camera init gagal: 0x%x\n",
      err
    );

    return false;

  }

  sensor_t *sensor =
    esp_camera_sensor_get();

  if (
    sensor &&
    sensor->id.PID == OV3660_PID
  ) {

    sensor->set_vflip(sensor, 1);

    sensor->set_brightness(sensor, 1);

    sensor->set_saturation(sensor, -2);

  }

  Serial.println("Kamera siap.");

  return true;

}

// ==========================================
// UPLOAD FOTO KE SUPABASE
// ==========================================

void uploadCameraImage() {

  if (
    !cameraAvailable ||
    WiFi.status() != WL_CONNECTED
  ) {

    return;

  }

  if (
    String(DEVICE_TOKEN).startsWith("GANTI_") ||
    String(SUPABASE_CAMERA_URL)
      .indexOf("YOUR_PROJECT_REF") >= 0
  ) {

    Serial.println(
      "Isi URL Supabase dan DEVICE_TOKEN terlebih dahulu."
    );

    return;

  }

  camera_fb_t *fb =
    esp_camera_fb_get();

  if (!fb) {

    Serial.println(
      "Gagal mengambil foto."
    );

    return;

  }

  WiFiClientSecure client;

  // Hanya untuk pengujian awal.
  // Untuk produksi gunakan verifikasi sertifikat CA.

  client.setInsecure();

  HTTPClient https;

  https.setTimeout(20000);

  if (
    !https.begin(
      client,
      SUPABASE_CAMERA_URL
    )
  ) {

    Serial.println(
      "Gagal membuka endpoint Supabase."
    );

    esp_camera_fb_return(fb);

    return;

  }

  https.addHeader(
    "Content-Type",
    "image/jpeg"
  );

  https.addHeader(
    "x-device-token",
    DEVICE_TOKEN
  );

  Serial.printf(
    "Upload foto: %u bytes\n",
    fb->len
  );

  int httpCode =
    https.POST(
      fb->buf,
      fb->len
    );

  if (httpCode > 0) {

    Serial.printf(
      "HTTP status: %d\n",
      httpCode
    );

    Serial.println(
      https.getString()
    );

    if (
      httpCode >= 200 &&
      httpCode < 300
    ) {

      Serial.println(
        "UPLOAD FOTO BERHASIL."
      );

    }

    else {

      Serial.println(
        "UPLOAD DITOLAK SERVER."
      );

    }

  }

  else {

    Serial.printf(
      "HTTP error: %s\n",
      https.errorToString(httpCode).c_str()
    );

  }

  https.end();

  esp_camera_fb_return(fb);

}

// ==========================================
// SETUP
// ==========================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();

  Serial.println(
    "=== BROODER CONTROL SYSTEM ==="
  );

  // RELAY

  pinMode(FAN1_PIN, OUTPUT);
  pinMode(FAN2_PIN, OUTPUT);

  pinMode(LAMP1_PIN, OUTPUT);
  pinMode(LAMP2_PIN, OUTPUT);

  allLoadsOff();

  // TOMBOL

  pinMode(
    BUTTON_UP_PIN,
    INPUT_PULLUP
  );

  pinMode(
    BUTTON_DOWN_PIN,
    INPUT_PULLUP
  );

  pinMode(
    BUTTON_MENU_PIN,
    INPUT_PULLUP
  );

  // OLED

  Wire.begin(
    OLED_SDA_PIN,
    OLED_SCL_PIN
  );

  oledAvailable =
    display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDRESS
    );

  if (oledAvailable) {

    display.clearDisplay();

    display.setTextSize(1);

    display.setTextColor(
      SSD1306_WHITE
    );

    display.setCursor(0, 0);

    display.println(
      "BROODER STARTING..."
    );

    display.display();

  }

  else {

    Serial.println(
      "OLED tidak terdeteksi."
    );

  }

  // SENSOR DHT22

  dht.begin();

  delay(2000);

  readSensors();

  // KAMERA

  cameraAvailable =
    initializeCamera();

  // WIFI

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  lastWiFiAttempt = millis();

  Serial.print(
    "Menghubungkan WiFi"
  );

  unsigned long wifiStart =
    millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - wifiStart < 15000
  ) {

    delay(300);

    Serial.print(".");

  }

  Serial.println();

  printWiFiStatus();

  if (autoMode) {

    runAutomaticControl();

  }

  lastSensorRead = millis();

  lastDisplayUpdate = 0;

  lastCameraUpload =
    millis() - CAMERA_UPLOAD_INTERVAL_MS;

  Serial.println(
    "Sistem siap."
  );

  Serial.println(
    "AUTO: UP/DOWN ubah setpoint 0.5 C."
  );

  Serial.println(
    "Tahan MENU 1.8 detik: AUTO <-> MANUAL."
  );

  Serial.println(
    "MANUAL: MENU pilih relay, UP=ON, DOWN=OFF."
  );

}

// ==========================================
// LOOP UTAMA
// ==========================================

void loop() {

  unsigned long now =
    millis();

  // WIFI

  connectWiFi();

  // TOMBOL

  handleButtons();

  // SENSOR

  if (
    now - lastSensorRead >=
    SENSOR_INTERVAL_MS
  ) {

    lastSensorRead = now;

    readSensors();

  }

  // OLED

  if (
    now - lastDisplayUpdate >=
    DISPLAY_INTERVAL_MS
  ) {

    lastDisplayUpdate = now;

    updateDisplay();

  }

  // KAMERA DAN SUPABASE

  if (
    now - lastCameraUpload >=
    CAMERA_UPLOAD_INTERVAL_MS
  ) {

    lastCameraUpload = now;

    uploadCameraImage();

  }

  delay(10);

}