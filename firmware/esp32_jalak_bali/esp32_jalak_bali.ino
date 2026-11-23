#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_camera.h"
#include "mbedtls/base64.h"

const char* WIFI_SSID = "Lemott_2.4G";
const char* WIFI_PASS = "lemot321";

const char* MQTT_HOST      = "w13913d1.ala.us-east-1.emqxsl.com";
const int   MQTT_PORT      = 8883;
const char* MQTT_CLIENT_ID = "esp32s3-jalak-bali-01";
const char* MQTT_USER      = "SmartBrooder";
const char* MQTT_PASS      = "12345678";

const char* TOPIC_TELEMETRY   = "SmartBrooder/jalak/telemetry";
const char* TOPIC_PHOTO       = "SmartBrooder/jalak/photo";
const char* TOPIC_CMD_RELAY   = "SmartBrooder/jalak/cmd/relay";
const char* TOPIC_CMD_MODE    = "SmartBrooder/jalak/cmd/mode";
const char* TOPIC_CMD_LIMITS  = "SmartBrooder/jalak/cmd/limits";
const char* TOPIC_CMD_OFF     = "SmartBrooder/jalak/cmd/all_off";
const char* TOPIC_CMD_CAPTURE = "SmartBrooder/jalak/cmd/capture";

#define DHTPIN 14
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// OLED SSD1306 128x32 (I2C SDA=19, SCL=20, Alamat 0x3C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define OLED_ADDR 0x3C
#define OLED_SDA 19
#define OLED_SCL 20
TwoWire oledWire = TwoWire(0);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &oledWire, OLED_RESET);

// Rotary Encoder (CLK=1, DT=2, SW=3)
#define ENC_CLK 1
#define ENC_DT  2
#define ENC_SW  3

const int RELAY_PINS[4] = {41, 42, 47, 48};
const char* RELAY_TYPES[4] = {"heat", "heat", "fan", "fan"};
const bool RELAY_ACTIVE_LOW = true;
bool relayState[4] = {false, false, false, false};

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

bool autoMode = true;
float minTemp = 32.5;
float maxTemp = 33.5;

enum EncMenuState { ENC_IDLE = 0, ENC_SET_MAX = 1, ENC_SET_MIN = 2 };
EncMenuState encMenuState = ENC_IDLE;
int lastEncClk = HIGH;
int lastEncSw = HIGH;
unsigned long lastSwMs = 0;

float cachedTemp = NAN;
float cachedHum = NAN;
unsigned long lastDhtReadMs = 0;
unsigned long lastPubMs = 0;
unsigned long lastOledMs = 0;
unsigned long lastWifiCheckMs = 0;
bool cameraReady = false;
bool oledReady = false;
volatile bool captureRequested = false;

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

void writeRelayPin(int idx, bool on) {
  if (idx < 0 || idx > 3) return;
  relayState[idx] = on;
  uint8_t level = RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(RELAY_PINS[idx], level);
}

void setAllRelaysOff() {
  for (int i = 0; i < 4; i++) {
    writeRelayPin(i, false);
  }
}

void updateOled() {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Baris 1 (y = 2): Mode/Menu & Suhu (maks ~21 karakter)
  display.setCursor(0, 2);
  if (encMenuState == ENC_SET_MAX) {
    display.printf("SET ATAS : %.1fC", maxTemp);
  } else if (encMenuState == ENC_SET_MIN) {
    display.printf("SET BAWAH: %.1fC", minTemp);
  } else if (isnan(cachedTemp)) {
    display.printf("%s Suhu:--.-C", autoMode ? "OTOMATIS" : "MANUAL");
  } else {
    display.printf("%s Suhu:%.1fC", autoMode ? "OTOMATIS" : "MANUAL", cachedTemp);
  }

  // Baris 2 (y = 18): Lembap, Relay & Status WiFi/MQTT (maks ~21 karakter)
  display.setCursor(0, 18);
  if (encMenuState == ENC_SET_MAX) {
    display.print("Putar:+/- Tekan:Bawah");
  } else if (encMenuState == ENC_SET_MIN) {
    display.print("Putar:+/- Tekan:OK");
  } else {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool mqttOk = mqtt.connected();
    char rlyStr[5];
    snprintf(rlyStr, sizeof(rlyStr), "%d%d%d%d",
             relayState[0] ? 1 : 0,
             relayState[1] ? 1 : 0,
             relayState[2] ? 1 : 0,
             relayState[3] ? 1 : 0);

    if (isnan(cachedHum)) {
      display.printf("R:%s W:%s M:%s",
                     rlyStr,
                     wifiOk ? "YA" : "TDK",
                     mqttOk ? "YA" : "TDK");
    } else {
      display.printf("L:%.0f%% R:%s W:%s M:%s",
                     cachedHum,
                     rlyStr,
                     wifiOk ? "YA" : "TDK",
                     mqttOk ? "YA" : "TDK");
    }
  }

  display.display();
}

void publishTelemetry() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  if (isnan(cachedTemp)) doc["temp"] = nullptr;
  else doc["temp"] = serialized(String(cachedTemp, 1));

  if (isnan(cachedHum)) doc["hum"] = nullptr;
  else doc["hum"] = serialized(String(cachedHum, 1));

  doc["auto"] = autoMode;
  doc["minTemp"] = serialized(String(minTemp, 1));
  doc["maxTemp"] = serialized(String(maxTemp, 1));
  doc["encMenu"] = (encMenuState != ENC_IDLE);

  JsonArray rArr = doc["relays"].to<JsonArray>();
  for (int i = 0; i < 4; i++) {
    rArr.add(relayState[i] ? 1 : 0);
  }

  char payload[280];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(TOPIC_TELEMETRY, payload, true);
}

void applyAutoControl() {
  if (!autoMode) return;
  if (isnan(cachedTemp)) {
    setAllRelaysOff();
    return;
  }
  for (int i = 0; i < 4; i++) {
    bool wantOn = false;
    if (cachedTemp < minTemp && strcmp(RELAY_TYPES[i], "heat") == 0) {
      wantOn = true;
    } else if (cachedTemp > maxTemp && strcmp(RELAY_TYPES[i], "fan") == 0) {
      wantOn = true;
    }
    writeRelayPin(i, wantOn);
  }
}

void handleRotaryEncoder() {
  unsigned long now = millis();
  int swVal = digitalRead(ENC_SW);
  if (lastEncSw == HIGH && swVal == LOW && (now - lastSwMs > 250)) {
    lastSwMs = now;
    if (encMenuState == ENC_IDLE) {
      encMenuState = ENC_SET_MAX;
      autoMode = false;
    } else if (encMenuState == ENC_SET_MAX) {
      encMenuState = ENC_SET_MIN;
    } else {
      encMenuState = ENC_IDLE;
      autoMode = true;
      applyAutoControl();
      publishTelemetry();
    }
    updateOled();
  }
  lastEncSw = swVal;

  int clkVal = digitalRead(ENC_CLK);
  if (clkVal != lastEncClk && clkVal == LOW) {
    bool clockwise = (digitalRead(ENC_DT) != clkVal);
    if (encMenuState == ENC_SET_MAX) {
      if (clockwise) {
        maxTemp = roundf((maxTemp + 0.1f) * 10.0f) / 10.0f;
      } else {
        maxTemp = roundf((maxTemp - 0.1f) * 10.0f) / 10.0f;
        if (maxTemp <= minTemp) maxTemp = roundf((minTemp + 0.1f) * 10.0f) / 10.0f;
      }
      updateOled();
      publishTelemetry();
    } else if (encMenuState == ENC_SET_MIN) {
      if (clockwise) {
        minTemp = roundf((minTemp + 0.1f) * 10.0f) / 10.0f;
        if (minTemp >= maxTemp) minTemp = roundf((maxTemp - 0.1f) * 10.0f) / 10.0f;
      } else {
        minTemp = roundf((minTemp - 0.1f) * 10.0f) / 10.0f;
      }
      updateOled();
      publishTelemetry();
    }
  }
  lastEncClk = clkVal;
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 16000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 16;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 20;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  return (esp_camera_init(&config) == ESP_OK);
}

void sendCapturedPhotoMqtt() {
  if (!cameraReady || !mqtt.connected()) {
    Serial.println("Kamera atau MQTT belum siap untuk ambil foto");
    return;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Gagal mengambil frame kamera");
    return;
  }

  size_t b64Len = 0;
  mbedtls_base64_encode(NULL, 0, &b64Len, fb->buf, fb->len);
  unsigned char *b64Buf = (unsigned char *)malloc(b64Len + 1);
  if (!b64Buf) {
    esp_camera_fb_return(fb);
    return;
  }

  if (mbedtls_base64_encode(b64Buf, b64Len + 1, &b64Len, fb->buf, fb->len) != 0) {
    free(b64Buf);
    esp_camera_fb_return(fb);
    return;
  }
  b64Buf[b64Len] = '\0';
  esp_camera_fb_return(fb);

  const size_t CHUNK_SIZE = 2048;
  int totalChunks = (int)((b64Len + CHUNK_SIZE - 1) / CHUNK_SIZE);
  unsigned long shotId = millis();

  for (int i = 0; i < totalChunks; i++) {
    size_t startIdx = (size_t)i * CHUNK_SIZE;
    size_t len = b64Len - startIdx;
    if (len > CHUNK_SIZE) len = CHUNK_SIZE;

    String partData((const char *)(b64Buf + startIdx), len);
    JsonDocument pkt;
    pkt["id"] = shotId;
    pkt["idx"] = i;
    pkt["total"] = totalChunks;
    pkt["data"] = partData;

    String out;
    serializeJson(pkt, out);
    mqtt.publish(TOPIC_PHOTO, out.c_str(), false);
    mqtt.loop();
    delay(15);
  }

  free(b64Buf);
  Serial.printf("Foto terkirim via MQTT (%d bagian)\n", totalChunks);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  String t = String(topic);
  if (t == TOPIC_CMD_CAPTURE) {
    Serial.println("Perintah ambil foto diterima");
    captureRequested = true;
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, msg)) return;

  if (t == TOPIC_CMD_RELAY) {
    int ch = doc["ch"] | 0;
    int st = doc["state"] | 0;
    if (ch >= 1 && ch <= 4) {
      autoMode = false;
      encMenuState = ENC_IDLE;
      writeRelayPin(ch - 1, st != 0);
      publishTelemetry();
    }
  } else if (t == TOPIC_CMD_MODE) {
    autoMode = doc["auto"] | true;
    if (autoMode) encMenuState = ENC_IDLE;
    if (autoMode) applyAutoControl();
    publishTelemetry();
  } else if (t == TOPIC_CMD_LIMITS) {
    if (!doc["min"].isNull()) minTemp = doc["min"].as<float>();
    if (!doc["max"].isNull()) maxTemp = doc["max"].as<float>();
    if (autoMode) applyAutoControl();
    publishTelemetry();
  } else if (t == TOPIC_CMD_OFF) {
    autoMode = false;
    encMenuState = ENC_IDLE;
    setAllRelaysOff();
    publishTelemetry();
  }
  updateOled();
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWifiCheckMs < 5000) return;
  lastWifiCheckMs = now;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 3000) return;
  lastAttempt = millis();

  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.println("EMQX MQTT terhubung");
    mqtt.subscribe(TOPIC_CMD_RELAY, 1);
    mqtt.subscribe(TOPIC_CMD_MODE, 1);
    mqtt.subscribe(TOPIC_CMD_LIMITS, 1);
    mqtt.subscribe(TOPIC_CMD_OFF, 1);
    mqtt.subscribe(TOPIC_CMD_CAPTURE, 1);
    publishTelemetry();
  }
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  lastEncClk = digitalRead(ENC_CLK);
  lastEncSw = digitalRead(ENC_SW);

  for (int i = 0; i < 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    writeRelayPin(i, false);
  }

  cameraReady = initCamera();
  if (!cameraReady) Serial.println("Kamera gagal diinisialisasi");

  oledWire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledReady) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 4);
    display.println("Brooder Jalak Bali");
    display.setCursor(0, 18);
    display.println("Ready (128x32)");
    display.display();
  } else {
    Serial.println("OLED SSD1306 gagal diinisialisasi");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  espClient.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(4096);
  mqtt.setCallback(mqttCallback);
  updateOled();
}

void loop() {
  ensureWiFi();
  ensureMqtt();
  mqtt.loop();
  handleRotaryEncoder();

  if (captureRequested) {
    captureRequested = false;
    sendCapturedPhotoMqtt();
  }

  unsigned long now = millis();
  if (now - lastDhtReadMs >= 2000) {
    lastDhtReadMs = now;
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(t) && !isnan(h)) {
      cachedTemp = t;
      cachedHum = h;
    } else {
      cachedTemp = NAN;
      cachedHum = NAN;
    }
    applyAutoControl();
  }

  if (now - lastOledMs >= 1000) {
    lastOledMs = now;
    updateOled();
  }

  if (now - lastPubMs >= 2500) {
    lastPubMs = now;
    publishTelemetry();
  }
}