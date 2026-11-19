#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_camera.h"
#include "mbedtls/base64.h"

const char* PRIMARY_WIFI_SSID = "Aryo Sidiq";
const char* PRIMARY_WIFI_PASS = "cukitooo";
const char* AP_SSID           = "BrooderJalak-Setup";
const char* AP_PASS           = "12345678";

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

bool encMenuActive = false;
int lastEncClk = HIGH;
int lastEncSw = HIGH;
unsigned long lastSwMs = 0;

float cachedTemp = NAN;
float cachedHum = NAN;
unsigned long lastDhtReadMs = 0;
unsigned long lastPubMs = 0;
unsigned long lastOledMs = 0;
unsigned long attemptStartMs = 0;
unsigned long lastPrimaryRetryMs = 0;
bool cameraReady = false;
bool oledReady = false;
bool portalActive = false;
bool dnsStarted = false;
bool tryingPrimary = true;
bool pendingFallbackWifi = false;
volatile bool captureRequested = false;

String fallbackWifiSsid = "";
String fallbackWifiPass = "";

Preferences prefs;
WebServer webServer(80);
DNSServer dnsServer;
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

String htmlEscape(const String& in) {
  String out = in;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

String buildSetupHtml(const String& notice = "") {
  String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String statusStr = (WiFi.status() == WL_CONNECTED)
    ? ("Terhubung ke " + htmlEscape(WiFi.SSID()) + " (" + ipStr + ")")
    : ("Aryo Sidiq tidak ditemukan | Portal AP: " + String(AP_SSID) + " (" + ipStr + ")");

  String html = "<!DOCTYPE html><html lang='id'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>Pengaturan WiFi BrooderSmart</title>";
  html += "<style>body{font-family:system-ui,sans-serif;background:#0f172a;color:#f8fafc;padding:20px;margin:0}";
  html += ".box{max-width:440px;margin:0 auto;background:#1e293b;border:1px solid #334155;border-radius:14px;padding:20px}";
  html += "h2{margin:0 0 8px;font-size:1.2rem;color:#38bdf8}.sub{font-size:.85rem;color:#94a3b8;margin-bottom:16px}";
  html += "label{display:block;font-size:.82rem;color:#cbd5e1;margin:12px 0 4px}";
  html += "input{width:100%;box-sizing:border-box;padding:10px 12px;border-radius:10px;border:1px solid #475569;background:#0f172a;color:#f8fafc;font-size:.95rem}";
  html += "button{width:100%;margin-top:16px;padding:11px;border:0;border-radius:10px;background:#38bdf8;color:#0f172a;font-weight:700;font-size:.95rem;cursor:pointer}";
  html += ".msg{background:rgba(34,197,94,.15);border:1px solid #22c55e;color:#bbf7d0;padding:10px;border-radius:10px;margin-bottom:14px;font-size:.85rem}</style></head><body><div class='box'>";
  html += "<h2>Pengaturan WiFi Alternatif</h2>";
  html += "<div class='sub'>Utama: <strong>Aryo Sidiq</strong> | " + statusStr + "</div>";
  if (notice.length() > 0) {
    html += "<div class='msg'>" + htmlEscape(notice) + "</div>";
  }
  html += "<form method='POST' action='/save'>";
  html += "<label for='ssid'>Nama WiFi Alternatif (SSID)</label>";
  html += "<input id='ssid' name='ssid' type='text' required value='" + htmlEscape(fallbackWifiSsid) + "' placeholder='Masukkan SSID WiFi'>";
  html += "<label for='pass'>Password WiFi Alternatif</label>";
  html += "<input id='pass' name='pass' type='password' value='" + htmlEscape(fallbackWifiPass) + "' placeholder='Kosongkan jika open WiFi'>";
  html += "<button type='submit'>Simpan &amp; Hubungkan</button></form></div></body></html>";
  return html;
}

void startSetupPortal() {
  if (portalActive) return;
  WiFi.mode(WIFI_AP_STA);
  delay(80);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(80);
  if (!dnsStarted) {
    dnsServer.start(53, "*", WiFi.softAPIP());
    dnsStarted = true;
  }
  portalActive = true;
  Serial.println("Aryo Sidiq tidak ditemukan/gagal, aktifkan portal setup");
  Serial.print("AP aktif: ");
  Serial.print(AP_SSID);
  Serial.print(" -> http://");
  Serial.println(WiFi.softAPIP());
}

void startStationAttempt(const char* ssid, const char* pass, bool primary, bool initialBoot = false) {
  tryingPrimary = primary;
  if (!initialBoot) {
    WiFi.disconnect(false);
    delay(80);
  }
  if (pass && strlen(pass) > 0) {
    WiFi.begin(ssid, pass);
  } else {
    WiFi.begin(ssid);
  }
  attemptStartMs = millis();
  Serial.print("Mencoba WiFi ");
  Serial.print(primary ? "utama: " : "alternatif: ");
  Serial.println(ssid);
}

void setupWebRoutes() {
  webServer.on("/", HTTP_GET, []() {
    webServer.send(200, "text/html", buildSetupHtml());
  });
  webServer.on("/save", HTTP_POST, []() {
    String newSsid = webServer.arg("ssid");
    String newPass = webServer.arg("pass");
    newSsid.trim();
    if (newSsid.length() == 0) {
      webServer.send(400, "text/html", buildSetupHtml("SSID tidak boleh kosong."));
      return;
    }
    fallbackWifiSsid = newSsid;
    fallbackWifiPass = newPass;
    prefs.begin("brooder_wifi", false);
    prefs.putString("alt_ssid", fallbackWifiSsid);
    prefs.putString("alt_pass", fallbackWifiPass);
    prefs.end();

    webServer.send(200, "text/html", buildSetupHtml("Tersimpan! Mencoba menghubungkan ke " + newSsid + "..."));
    pendingFallbackWifi = true;
  });
  webServer.onNotFound([]() {
    if (WiFi.status() != WL_CONNECTED) {
      webServer.send(200, "text/html", buildSetupHtml());
    } else {
      webServer.send(404, "text/plain", "Not found");
    }
  });
  webServer.begin();
}

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
  display.setCursor(0, 0);

  if (encMenuActive) {
    display.printf("SET L:%.1f H:%.1f\n", minTemp, maxTemp);
  } else if (isnan(cachedTemp) || isnan(cachedHum)) {
    display.printf("%s T:--.-C H:--%%\n", autoMode ? "AUTO" : "MAN");
  } else {
    display.printf("%s T:%.1fC H:%.0f%%\n", autoMode ? "AUTO" : "MAN", cachedTemp, cachedHum);
  }

  display.setCursor(0, 12);
  display.printf("L:%.1f H:%.1f R:%d%d%d%d\n",
                 minTemp, maxTemp,
                 relayState[0] ? 1 : 0,
                 relayState[1] ? 1 : 0,
                 relayState[2] ? 1 : 0,
                 relayState[3] ? 1 : 0);

  display.setCursor(0, 24);
  if (encMenuActive) {
    display.print("R:+H L:-L SW:OK");
  } else {
    display.printf("W:%s M:%s C:%s",
                 WiFi.status() == WL_CONNECTED ? "OK" : (portalActive ? "AP" : "--"),
                 mqtt.connected() ? "OK" : "--",
                 cameraReady ? "OK" : "ERR");
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
  doc["encMenu"] = encMenuActive;

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
    if (!encMenuActive) {
      encMenuActive = true;
      autoMode = false;
    } else {
      encMenuActive = false;
      autoMode = true;
      applyAutoControl();
      publishTelemetry();
    }
    updateOled();
  }
  lastEncSw = swVal;

  int clkVal = digitalRead(ENC_CLK);
  if (clkVal != lastEncClk && clkVal == LOW) {
    if (encMenuActive) {
      if (digitalRead(ENC_DT) != clkVal) {
        maxTemp = roundf((maxTemp + 0.1f) * 10.0f) / 10.0f;
        if (maxTemp <= minTemp) maxTemp = roundf((minTemp + 0.1f) * 10.0f) / 10.0f;
      } else {
        minTemp = roundf((minTemp - 0.1f) * 10.0f) / 10.0f;
        if (minTemp >= maxTemp) minTemp = roundf((maxTemp - 0.1f) * 10.0f) / 10.0f;
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
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 18;
  config.fb_count     = 1;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

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
      encMenuActive = false;
      writeRelayPin(ch - 1, st != 0);
      publishTelemetry();
    }
  } else if (t == TOPIC_CMD_MODE) {
    autoMode = doc["auto"] | true;
    if (autoMode) encMenuActive = false;
    if (autoMode) applyAutoControl();
    publishTelemetry();
  } else if (t == TOPIC_CMD_LIMITS) {
    if (!doc["min"].isNull()) minTemp = doc["min"].as<float>();
    if (!doc["max"].isNull()) maxTemp = doc["max"].as<float>();
    if (autoMode) applyAutoControl();
    publishTelemetry();
  } else if (t == TOPIC_CMD_OFF) {
    autoMode = false;
    encMenuActive = false;
    setAllRelaysOff();
    publishTelemetry();
  }
  updateOled();
}

void ensureWiFi() {
  if (pendingFallbackWifi) {
    pendingFallbackWifi = false;
    startSetupPortal();
    startStationAttempt(fallbackWifiSsid.c_str(), fallbackWifiPass.c_str(), false, false);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    static String lastLoggedSsid = "";
    if (WiFi.SSID() != lastLoggedSsid) {
      lastLoggedSsid = WiFi.SSID();
      Serial.print("WiFi terhubung ke ");
      Serial.print(WiFi.SSID());
      Serial.print(" | IP: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  unsigned long now = millis();
  if (tryingPrimary) {
    if (now - attemptStartMs >= 12000) {
      startSetupPortal();
      if (fallbackWifiSsid.length() > 0) {
        startStationAttempt(fallbackWifiSsid.c_str(), fallbackWifiPass.c_str(), false, false);
      } else {
        tryingPrimary = false;
        lastPrimaryRetryMs = now;
      }
    }
  } else {
    if (fallbackWifiSsid.length() > 0 && (now - attemptStartMs >= 12000)) {
      startStationAttempt(fallbackWifiSsid.c_str(), fallbackWifiPass.c_str(), false, false);
    }
    if (now - lastPrimaryRetryMs >= 30000) {
      lastPrimaryRetryMs = now;
      startStationAttempt(PRIMARY_WIFI_SSID, PRIMARY_WIFI_PASS, true, false);
    }
  }
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
  delay(200);
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

  cameraReady = initCamera();
  if (!cameraReady) Serial.println("Kamera gagal diinisialisasi");

  prefs.begin("brooder_wifi", true);
  fallbackWifiSsid = prefs.getString("alt_ssid", "");
  fallbackWifiPass = prefs.getString("alt_pass", "");
  prefs.end();

  WiFi.mode(WIFI_STA);
  delay(100);
  setupWebRoutes();
  startStationAttempt(PRIMARY_WIFI_SSID, PRIMARY_WIFI_PASS, true, true);

  espClient.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(4096);
  mqtt.setCallback(mqttCallback);
  updateOled();
}

void loop() {
  if (portalActive && dnsStarted) {
    dnsServer.processNextRequest();
  }
  webServer.handleClient();

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