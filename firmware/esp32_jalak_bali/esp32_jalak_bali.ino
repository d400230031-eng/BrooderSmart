#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ArduinoJson.h>
#define CAMERA_MODEL_ESP32S3_EYE
#include "camera_pins.h"
#include "esp_http_server.h"
#include "board_config.h"

static const char* WIFI_SSID = "Aryo Sidiq";
static const char* WIFI_PASS = "cukitooo";

static const char* SUPABASE_URL      = "https://shrtuvtzmhhjvpadgmcr.supabase.co";
static const char* SUPABASE_ANON_KEY = "sb_publishable_xgTkr-YhiRGNwX_6IY_Qew_QV6tzICa";
static const char* DEVICE_ID         = "jalak-bali-esp32-01";

static const int PIN_DHT = 14;
#define DHT_TYPE DHT22

static const int PIN_RELAY_LAMP1 = 47;
static const int PIN_RELAY_LAMP2 = 48;
static const int PIN_RELAY_FAN1  = 41;
static const int PIN_RELAY_FAN2  = 42;

static const int PIN_OLED_SDA = 1;
static const int PIN_OLED_SCL = 2;
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define OLED_ADDR 0x3C

static const int PIN_ENC_CLK = 38;
static const int PIN_ENC_DT  = 39;
static const int PIN_ENC_SW  = 40;

static const unsigned long TELEMETRY_INTERVAL_MS = 15000UL;
static const unsigned long COMMAND_POLL_MS     = 5000UL;
static const unsigned long SENSOR_READ_MS      = 2000UL;
static const unsigned long OLED_REFRESH_MS     = 250UL;

static const float DEFAULT_HEATER_MIN = 30.0f;
static const float DEFAULT_COOLER_MAX = 33.0f;
static const float EMERGENCY_MAX_TEMP = 38.0f;

DHT dht(PIN_DHT, DHT_TYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences prefs;
static String g_streamUrl = "";

static httpd_handle_t camera_httpd = NULL;

static esp_err_t stream_handler(httpd_req_t* req) {
  camera_fb_t* fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=123456789000000000000987654321");
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
      break;
    }

    size_t hlen = snprintf(
      part_buf, sizeof(part_buf),
      "--123456789000000000000987654321\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      (unsigned)fb->len
    );

    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, "\r\n", 2);
    }

    esp_camera_fb_return(fb);
    fb = NULL;

    if (res != ESP_OK) break;
    delay(1);
  }

  return res;
}

void startCameraServer() {
  if (camera_httpd != NULL) return;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.ctrl_port = 32769;
  config.stack_size = 8192;

  esp_err_t err = httpd_start(&camera_httpd, &config);
  if (err != ESP_OK || camera_httpd == NULL) {
    Serial.printf("httpd_start failed: 0x%x\n", err);
    camera_httpd = NULL;
    return;
  }
  Serial.println("Camera HTTP server started on port 81");

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  err = httpd_register_uri_handler(camera_httpd, &stream_uri);
  if (err != ESP_OK) {
    Serial.printf("httpd_register_uri_handler failed: 0x%x\n", err);
  }
}

enum ControlMode { MODE_AUTO, MODE_MANUAL };

struct SystemState {
  float temperatureC = NAN;
  float humidityPct  = NAN;
  bool sensorOk      = false;
  String tempAlert   = "SENSOR_ERROR";
  ControlMode mode   = MODE_AUTO;
  float heaterMinC   = DEFAULT_HEATER_MIN;
  float coolerMaxC   = DEFAULT_COOLER_MAX;
  bool lamp1On       = false;
  bool lamp2On       = false;
  bool fan1On        = false;
  bool fan2On        = false;
  bool wifiConnected     = false;
  bool supabaseReachable = false;
  String lastSyncStatus  = "INIT";
  unsigned long wifiRetryDelayMs = 1000UL;
  unsigned long lastWifiAttemptMs = 0UL;
  unsigned long lastSensorReadMs = 0UL;
  unsigned long lastTelemetryMs = 0UL;
  unsigned long lastCommandPollMs = 0UL;
  unsigned long lastOledRefreshMs = 0UL;
};

SystemState state;

bool initCameraStream() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM; c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000; c.pixel_format = PIXFORMAT_JPEG; c.frame_size = FRAMESIZE_VGA; c.jpeg_quality = 14;
  c.fb_count = psramFound() ? 2 : 1;
  c.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("esp_camera_init failed: 0x%x\n", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->id.PID == OV3660_PID) { s->set_vflip(s, 1); s->set_brightness(s, 1); s->set_saturation(s, -2); }
  return true;
}

void applyRelaysHardware(bool l1, bool l2, bool f1, bool f2) {
  if ((l1 || l2) && (f1 || f2)) { l1 = false; l2 = false; f1 = false; f2 = false; }
  state.lamp1On = l1; state.lamp2On = l2; state.fan1On = f1; state.fan2On = f2;
  digitalWrite(PIN_RELAY_LAMP1, l1 ? HIGH : LOW);
  digitalWrite(PIN_RELAY_LAMP2, l2 ? HIGH : LOW);
  digitalWrite(PIN_RELAY_FAN1,  f1 ? HIGH : LOW);
  digitalWrite(PIN_RELAY_FAN2,  f2 ? HIGH : LOW);
}

void allRelaysOff() { applyRelaysHardware(false, false, false, false); }

void saveSettingsToNVS() {
  prefs.begin("jalakbali", false);
  prefs.putUChar("mode", static_cast<uint8_t>(state.mode));
  prefs.putFloat("heatMin", state.heaterMinC);
  prefs.putFloat("coolMax", state.coolerMaxC);
  prefs.end();
}

void loadSettingsFromNVS() {
  prefs.begin("jalakbali", true);
  uint8_t savedMode = prefs.getUChar("mode", static_cast<uint8_t>(MODE_AUTO));
  float savedHeat = prefs.getFloat("heatMin", DEFAULT_HEATER_MIN);
  float savedCool = prefs.getFloat("coolMax", DEFAULT_COOLER_MAX);
  prefs.end();
  state.mode = (savedMode == static_cast<uint8_t>(MODE_MANUAL)) ? MODE_MANUAL : MODE_AUTO;
  if (savedHeat >= 15.0f && savedHeat <= 45.0f && savedCool > savedHeat && savedCool < EMERGENCY_MAX_TEMP) {
    state.heaterMinC = savedHeat; state.coolerMaxC = savedCool;
  }
}

void readDHTAndEvaluateControl() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(t) || isnan(h) || h < 0.0f || h > 100.0f || t < -20.0f || t > 80.0f) {
    state.sensorOk = false; state.tempAlert = "SENSOR_ERROR"; allRelaysOff(); return;
  }
  state.sensorOk = true; state.temperatureC = t; state.humidityPct = h;
  if (state.temperatureC >= EMERGENCY_MAX_TEMP) {
    state.tempAlert = "EMERGENCY_HIGH"; applyRelaysHardware(false, false, true, true); return;
  }
  if (state.mode == MODE_AUTO) {
    if (state.temperatureC < state.heaterMinC) { state.tempAlert = "LOW_TEMP"; applyRelaysHardware(true, true, false, false); }
    else if (state.temperatureC > state.coolerMaxC) { state.tempAlert = "HIGH_TEMP"; applyRelaysHardware(false, false, true, true); }
    else { state.tempAlert = "NORMAL"; }
  } else {
    state.tempAlert = (state.temperatureC > state.coolerMaxC) ? "HIGH_TEMP" : "NORMAL";
    applyRelaysHardware(state.lamp1On, state.lamp2On, state.fan1On, state.fan2On);
  }
}

void handleWiFiConnection(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    bool justConnected = !state.wifiConnected;
    state.wifiConnected = true;
    state.wifiRetryDelayMs = 1000UL;
    g_streamUrl = "http://" + WiFi.localIP().toString() + ":81/stream";
    if (justConnected) {
      Serial.print("IP ESP32: ");
      Serial.println(WiFi.localIP());
    }
    if (camera_httpd == NULL) {
      startCameraServer();
    }
    return;
  }
  state.wifiConnected = false; state.supabaseReachable = false;
  if (now - state.lastWifiAttemptMs >= state.wifiRetryDelayMs) {
    state.lastWifiAttemptMs = now;
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    state.wifiRetryDelayMs = min(state.wifiRetryDelayMs * 2UL, 30000UL);
  }
}

bool supabaseRequest(const String& pathAndQuery, const char* method, const String& payload, String* responseBody = nullptr) {
  if (!state.wifiConnected) return false;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(SUPABASE_URL) + pathAndQuery)) return false;
  http.setTimeout(6000);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  int httpCode = -1;
  if (strcmp(method, "GET") == 0) {
    httpCode = http.GET();
  } else if (strcmp(method, "POST") == 0) {
    if (pathAndQuery.indexOf("on_conflict=") >= 0) {
      http.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");
    } else {
      http.addHeader("Prefer", "return=minimal");
    }
    httpCode = http.POST(payload);
  } else if (strcmp(method, "PATCH") == 0) {
    http.addHeader("Prefer", "return=minimal");
    httpCode = http.PATCH(payload);
  }
  bool ok = (httpCode >= 200 && httpCode < 300);
  if (!ok) {
    Serial.printf("[SB] %s %s -> HTTP %d\n", method, pathAndQuery.c_str(), httpCode);
  }
  if (ok && responseBody != nullptr) *responseBody = http.getString();
  http.end();
  return ok;
}

void sendTelemetryToSupabase() {
  if (!state.wifiConnected) { state.lastSyncStatus = "OFFLINE_WIFI"; return; }
  bool readingSent = true;
  if (state.sensorOk) {
    JsonDocument docReading;
    docReading["device_id"] = DEVICE_ID;
    docReading["temperature_c"] = serialized(String(state.temperatureC, 2));
    docReading["humidity_pct"] = serialized(String(state.humidityPct, 2));
    docReading["sensor_ok"] = true;
    docReading["temp_alert"] = state.tempAlert;
    String bodyReading; serializeJson(docReading, bodyReading);
    readingSent = supabaseRequest("/rest/v1/sensor_readings", "POST", bodyReading);
  }
  JsonDocument docStatus;
  docStatus["device_id"] = DEVICE_ID;
  docStatus["is_online"] = true;
  docStatus["control_mode"] = (state.mode == MODE_AUTO) ? "AUTO" : "MANUAL";
  docStatus["heater_min_c"] = serialized(String(state.heaterMinC, 1));
  docStatus["cooler_max_c"] = serialized(String(state.coolerMaxC, 1));
  docStatus["emergency_max_c"] = serialized(String(EMERGENCY_MAX_TEMP, 1));
  docStatus["lamp1_on"] = state.lamp1On; docStatus["lamp2_on"] = state.lamp2On;
  docStatus["fan1_on"] = state.fan1On; docStatus["fan2_on"] = state.fan2On;
  docStatus["sensor_ok"] = state.sensorOk; docStatus["temp_alert"] = state.tempAlert;
  docStatus["wifi_rssi"] = WiFi.RSSI();
  if (g_streamUrl.length() > 0) docStatus["camera_stream_url"] = g_streamUrl;
  if (state.sensorOk) {
    docStatus["last_temperature_c"] = serialized(String(state.temperatureC, 2));
    docStatus["last_humidity_pct"] = serialized(String(state.humidityPct, 2));
  }
  String bodyStatus; serializeJson(docStatus, bodyStatus);
  bool statusSent = supabaseRequest("/rest/v1/device_status?on_conflict=device_id", "POST", bodyStatus);
  state.supabaseReachable = (readingSent && statusSent);
  state.lastSyncStatus = state.supabaseReachable ? "SYNC_OK" : "SYNC_ERR";
}

void markCommandStatus(const String& cmdId, const char* newStatus, const char* note) {
  JsonDocument doc; doc["status"] = newStatus; doc["status_note"] = note;
  String body; serializeJson(doc, body);
  supabaseRequest("/rest/v1/device_commands?id=eq." + cmdId, "PATCH", body);
}

void pollPendingCommands() {
  if (!state.wifiConnected) return;
  String resp;
  String query = String("/rest/v1/device_commands?device_id=eq.") + DEVICE_ID + "&status=eq.pending&order=created_at.asc&limit=5";
  if (!supabaseRequest(query, "GET", "", &resp)) return;
  JsonDocument doc;
  if (deserializeJson(doc, resp) || !doc.is<JsonArray>()) return;
  for (JsonObject cmd : doc.as<JsonArray>()) {
    String cmdId = cmd["id"] | ""; String cmdType = cmd["command_type"] | ""; JsonObject payload = cmd["payload"].as<JsonObject>();
    if (cmdId.length() == 0) continue;
    if (cmdType == "SET_MODE") {
      state.mode = (String(payload["control_mode"] | "AUTO") == "MANUAL") ? MODE_MANUAL : MODE_AUTO;
      saveSettingsToNVS(); markCommandStatus(cmdId, "APPLIED", "Mode diperbarui");
    } else if (cmdType == "SET_SETPOINTS") {
      float hMin = payload["heater_min_c"] | state.heaterMinC; float cMax = payload["cooler_max_c"] | state.coolerMaxC;
      if (hMin >= 15.0f && hMin <= 45.0f && cMax > hMin && cMax < EMERGENCY_MAX_TEMP) {
        state.heaterMinC = hMin; state.coolerMaxC = cMax; saveSettingsToNVS(); markCommandStatus(cmdId, "APPLIED", "Setpoint disimpan ke NVS");
      } else { markCommandStatus(cmdId, "REJECTED", "Setpoint di luar batas aman"); }
    } else if (cmdType == "SET_RELAYS") {
      if (state.sensorOk && state.temperatureC >= EMERGENCY_MAX_TEMP) { markCommandStatus(cmdId, "REJECTED", "Ditolak: proteksi suhu darurat aktif"); continue; }
      bool l1 = payload["lamp1_on"] | state.lamp1On; bool l2 = payload["lamp2_on"] | state.lamp2On;
      bool f1 = payload["fan1_on"] | state.fan1On; bool f2 = payload["fan2_on"] | state.fan2On;
      if ((l1 || l2) && (f1 || f2)) { markCommandStatus(cmdId, "REJECTED", "Ditolak: lampu & kipas tidak boleh bersamaan"); }
      else { state.mode = MODE_MANUAL; saveSettingsToNVS(); applyRelaysHardware(l1, l2, f1, f2); markCommandStatus(cmdId, "APPLIED", "Relay manual diterapkan"); }
    } else if (cmdType == "EMERGENCY_OFF") {
      allRelaysOff(); markCommandStatus(cmdId, "APPLIED", "Semua relay dimatikan");
    } else { markCommandStatus(cmdId, "REJECTED", "Tipe perintah tidak dikenal"); }
  }
}

void updateOledDisplay() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 0);
  if (state.sensorOk) display.printf("T:%.1fC H:%.0f%% %s", state.temperatureC, state.humidityPct, (state.mode == MODE_AUTO) ? "AUTO" : "MAN");
  else display.printf("DHT ERR! %s", (state.mode == MODE_AUTO) ? "AUTO" : "MAN");
  display.setCursor(0, 11);
  display.printf("L:%d%d F:%d%d | %s", state.lamp1On ? 1 : 0, state.lamp2On ? 1 : 0, state.fan1On ? 1 : 0, state.fan2On ? 1 : 0, state.tempAlert.c_str());
  display.setCursor(0, 22);
  display.printf("WiFi:%s | %s", state.wifiConnected ? "OK" : "OFF", state.lastSyncStatus.c_str());
  display.display();
}

void setup() {
  Serial.begin(115200); delay(200);
  pinMode(PIN_RELAY_LAMP1, OUTPUT); pinMode(PIN_RELAY_LAMP2, OUTPUT);
  pinMode(PIN_RELAY_FAN1,  OUTPUT); pinMode(PIN_RELAY_FAN2,  OUTPUT);
  allRelaysOff();
  pinMode(PIN_ENC_CLK, INPUT_PULLUP); pinMode(PIN_ENC_DT, INPUT_PULLUP); pinMode(PIN_ENC_SW, INPUT_PULLUP);
  loadSettingsFromNVS();
  dht.begin();
  initCameraStream();
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) { display.clearDisplay(); display.display(); }
  readDHTAndEvaluateControl();
  updateOledDisplay();
}

void loop() {
  unsigned long now = millis();
  handleWiFiConnection(now);
  if (now - state.lastSensorReadMs >= SENSOR_READ_MS) { state.lastSensorReadMs = now; readDHTAndEvaluateControl(); }
  if (now - state.lastTelemetryMs >= TELEMETRY_INTERVAL_MS) { state.lastTelemetryMs = now; sendTelemetryToSupabase(); }
  if (now - state.lastCommandPollMs >= COMMAND_POLL_MS) { state.lastCommandPollMs = now; pollPendingCommands(); }
  if (now - state.lastOledRefreshMs >= OLED_REFRESH_MS) { state.lastOledRefreshMs = now; updateOledDisplay(); }
}
