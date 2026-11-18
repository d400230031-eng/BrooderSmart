#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "esp_camera.h"
#include "esp_http_server.h"

const char* WIFI_SSID = "Aryo Sidiq";
const char* WIFI_PASS = "cukitooo";

const char* MQTT_HOST      = "w13913d1.ala.us-east-1.emqxsl.com";
const int   MQTT_PORT      = 8883;
const char* MQTT_CLIENT_ID = "esp32s3-jalak-bali-01";
const char* MQTT_USER      = "SmartBrooder";
const char* MQTT_PASS      = "12345678";

const char* TOPIC_TELEMETRY = "brooder/jalak/telemetry";
const char* TOPIC_CMD_RELAY = "brooder/jalak/cmd/relay";
const char* TOPIC_CMD_MODE  = "brooder/jalak/cmd/mode";
const char* TOPIC_CMD_TARG  = "brooder/jalak/cmd/target";
const char* TOPIC_CMD_OFF   = "brooder/jalak/cmd/all_off";

#define DHTPIN 14
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// CH1 & CH2 = Bohlam (heat), CH3 & CH4 = Kipas (cool)
const int RELAY_PINS[4] = {41, 42, 47, 48};
const char* RELAY_TYPES[4] = {"heat", "heat", "fan", "fan"};
const bool RELAY_ACTIVE_LOW = true;
bool relayState[4] = {false, false, false, false};

// Pinout kamera umum ESP32-S3 CAM (verifikasi dengan datasheet board kamu)
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
float targetTemp = 33.0;
const float HYSTERESIS = 0.5;

float cachedTemp = NAN;
float cachedHum = NAN;
unsigned long lastDhtReadMs = 0;
unsigned long lastPubMs = 0;
unsigned long lastWifiCheckMs = 0;
bool cameraReady = false;

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
httpd_handle_t jalak_stream_httpd = NULL;

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
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 18;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size   = FRAMESIZE_QQVGA;
    config.jpeg_quality = 20;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  return (esp_camera_init(&config) == ESP_OK);
}

static esp_err_t stream_handler(httpd_req_t *req) {
  if (!cameraReady) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];
  static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
  static const char* _STREAM_BOUNDARY = "\r\n--123456789000000000000987654321\r\n";
  static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      delay(15);
      continue;
    }
    size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;
    delay(15);
  }
  return res;
}

void startStreamServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.ctrl_port   = 32769;
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  if (httpd_start(&jalak_stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(jalak_stream_httpd, &stream_uri);
    Serial.println("Stream kamera aktif di port 81 (/stream)");
  } else {
    Serial.println("Gagal menjalankan stream server di port 81");
  }
}

void publishTelemetry() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  if (isnan(cachedTemp)) doc["temp"] = nullptr;
  else doc["temp"] = serialized(String(cachedTemp, 1));

  if (isnan(cachedHum)) doc["hum"] = nullptr;
  else doc["hum"] = serialized(String(cachedHum, 1));

  doc["auto"] = autoMode;
  doc["target"] = targetTemp;
  JsonArray rArr = doc["relays"].to<JsonArray>();
  for (int i = 0; i < 4; i++) {
    rArr.add(relayState[i] ? 1 : 0);
  }

  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(TOPIC_TELEMETRY, payload, true);
}

void applyAutoControl() {
  if (!autoMode) return;

  if (isnan(cachedTemp)) {
    setAllRelaysOff();
    return;
  }

  float low = targetTemp - HYSTERESIS;
  float high = targetTemp + HYSTERESIS;

  for (int i = 0; i < 4; i++) {
    bool wantOn = false;
    if (cachedTemp < low && strcmp(RELAY_TYPES[i], "heat") == 0) {
      wantOn = true;
    } else if (cachedTemp > high && strcmp(RELAY_TYPES[i], "fan") == 0) {
      wantOn = true;
    }
    writeRelayPin(i, wantOn);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) return;

  String t = String(topic);
  if (t == TOPIC_CMD_RELAY) {
    int ch = doc["ch"] | 0;
    int st = doc["state"] | 0;
    if (ch >= 1 && ch <= 4) {
      autoMode = false;
      writeRelayPin(ch - 1, st != 0);
      publishTelemetry();
    }
  } else if (t == TOPIC_CMD_MODE) {
    autoMode = doc["auto"] | true;
    if (autoMode) applyAutoControl();
    publishTelemetry();
  } else if (t == TOPIC_CMD_TARG) {
    if (!doc["target"].isNull()) {
      targetTemp = doc["target"].as<float>();
      if (autoMode) applyAutoControl();
      publishTelemetry();
    }
  } else if (t == TOPIC_CMD_OFF) {
    autoMode = false;
    setAllRelaysOff();
    publishTelemetry();
  }
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
    mqtt.subscribe(TOPIC_CMD_TARG, 1);
    mqtt.subscribe(TOPIC_CMD_OFF, 1);
    publishTelemetry();
  } else {
    Serial.print("Gagal konek EMQX, rc=");
    Serial.println(mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  for (int i = 0; i < 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    writeRelayPin(i, false);
  }

  cameraReady = initCamera();
  if (!cameraReady) {
    Serial.println("Kamera gagal diinisialisasi");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP ESP32-S3: ");
    Serial.println(WiFi.localIP());
  }

  espClient.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setCallback(mqttCallback);

  startStreamServer();
  ensureMqtt();
}

void loop() {
  ensureWiFi();
  ensureMqtt();
  mqtt.loop();

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

  if (now - lastPubMs >= 2500) {
    lastPubMs = now;
    publishTelemetry();
  }
}
