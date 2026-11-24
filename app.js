const DEFAULT_WSS_URL = "wss://w13913d1.ala.us-east-1.emqxsl.com:8084/mqtt";
const DEFAULT_PREFIX = "SmartBrooder/jalak";
const ESP_STALE_MS = 10000;
const DEG = "\u00b0C";

const relays = [
  { id: 1, label: "CH1: Bohlam 1", on: false },
  { id: 2, label: "CH2: Bohlam 2", on: false },
  { id: 3, label: "CH3: Kipas 1", on: false },
  { id: 4, label: "CH4: Kipas 2", on: false }
];

let autoMode = true;
let client = null;
let lastTelemetryAt = 0;
let photoChunks = {};
let activePhotoSessionId = null;

let brokerUrl = (localStorage.getItem("emqx_wss_url") || DEFAULT_WSS_URL).trim();
let topicPrefix = (localStorage.getItem("emqx_topic_prefix") || DEFAULT_PREFIX).trim().replace(/\/+$/, "");
let mqttUser = localStorage.getItem("emqx_user") || "SmartBrooder";
let mqttPass = localStorage.getItem("emqx_pass") || "12345678";

const mqttStatusEl = document.getElementById("mqtt-status");
const espStatusEl = document.getElementById("esp-status");
const tempEl = document.getElementById("temp-display");
const humEl = document.getElementById("hum-display");
const lastSeenEl = document.getElementById("last-seen");
const sensorAlertEl = document.getElementById("sensor-alert");
const minTempInput = document.getElementById("min-temp");
const maxTempInput = document.getElementById("max-temp");
const modeBtn = document.getElementById("mode-toggle");
const relayGrid = document.getElementById("relay-grid");
const brokerInput = document.getElementById("broker-url");
const prefixInput = document.getElementById("topic-prefix");
const userInput = document.getElementById("mqtt-user");
const passInput = document.getElementById("mqtt-pass");
const camImg = document.getElementById("cam-photo");
const camPlaceholder = document.getElementById("cam-placeholder");
const photoStatusEl = document.getElementById("photo-status");
const captureBtn = document.getElementById("capture-btn");

brokerInput.value = brokerUrl;
prefixInput.value = topicPrefix;
userInput.value = mqttUser;
passInput.value = mqttPass;

camImg.onerror = () => {
  camImg.style.display = "none";
  camPlaceholder.style.display = "block";
  camPlaceholder.textContent = "Gambar tidak dapat dimuat. Silakan coba ambil foto lagi.";
  photoStatusEl.textContent = "Gagal merender gambar di browser.";
  captureBtn.disabled = false;
};

function setBadge(el, text, type = "") {
  el.textContent = text;
  el.className = "badge" + (type ? ` ${type}` : "");
}

function updateEspHealth() {
  if (!lastTelemetryAt) {
    setBadge(espStatusEl, "ESP32-S3: Menunggu Data", "warn");
    return;
  }
  const ageMs = Date.now() - lastTelemetryAt;
  if (ageMs <= ESP_STALE_MS) {
    setBadge(espStatusEl, `ESP32-S3: Online (${Math.round(ageMs / 1000)}d lalu)`, "ok");
  } else {
    setBadge(espStatusEl, `ESP32-S3: Tidak Ada Data (${Math.round(ageMs / 1000)}d)`, "err");
  }
}

function renderRelays() {
  relayGrid.innerHTML = "";
  relays.forEach((r) => {
    const btn = document.createElement("button");
    btn.className = "relay-btn" + (r.on ? " on" : "");
    btn.innerHTML = `<span class="name">${r.label}</span><span class="state">${r.on ? "AKTIF (ON)" : "MATI (OFF)"}</span>`;
    btn.onclick = () => {
      const next = !r.on;
      publishJson(`${topicPrefix}/cmd/relay`, { ch: r.id, state: next ? 1 : 0 });
    };
    relayGrid.appendChild(btn);
  });
}

function publishJson(topic, obj) {
  if (!client || !client.connected) return;
  client.publish(topic, JSON.stringify(obj), { qos: 1 });
}

function handlePhotoChunk(rawMsg) {
  try {
    const pkt = JSON.parse(rawMsg);
    if (!pkt.id || typeof pkt.idx !== "number" || typeof pkt.total !== "number" || !pkt.data) return;
    if (activePhotoSessionId !== null && pkt.id < activePhotoSessionId) return;

    if (!photoChunks[pkt.id]) {
      photoChunks = {};
      photoChunks[pkt.id] = { total: pkt.total, parts: new Array(pkt.total), createdAt: Date.now() };
    }
    photoChunks[pkt.id].parts[pkt.idx] = pkt.data;

    const received = photoChunks[pkt.id].parts.filter(Boolean).length;
    photoStatusEl.textContent = `Menerima foto: ${received}/${pkt.total} bagian...`;

    if (received === pkt.total) {
      const base64Str = photoChunks[pkt.id].parts.join("");
      delete photoChunks[pkt.id];
      camPlaceholder.style.display = "none";
      camImg.style.display = "block";
      camImg.removeAttribute("src");
      requestAnimationFrame(() => {
        camImg.src = `data:image/jpeg;base64,${base64Str}`;
      });
      photoStatusEl.textContent = `Foto diterima jam ${new Date().toLocaleTimeString("id-ID")}`;
      captureBtn.disabled = false;
    }
  } catch (_) {}
}

function connectMqtt() {
  if (client) {
    client.end(true);
    client = null;
  }

  if (!brokerUrl) {
    setBadge(mqttStatusEl, "Broker: URL Kosong", "warn");
    return;
  }

  setBadge(mqttStatusEl, "Broker: Menghubungkan...");
  client = mqtt.connect(brokerUrl, {
    username: mqttUser || undefined,
    password: mqttPass || undefined,
    reconnectPeriod: 3000,
    connectTimeout: 8000
  });

  client.on("connect", () => {
    setBadge(mqttStatusEl, "Broker: Terhubung", "ok");
    client.subscribe(`${topicPrefix}/telemetry`, { qos: 1 });
    client.subscribe(`${topicPrefix}/photo`, { qos: 0 });
  });

  client.on("offline", () => {
    setBadge(mqttStatusEl, "Broker: Terputus", "err");
  });

  client.on("error", () => {
    setBadge(mqttStatusEl, "Broker: Error / Auth Gagal", "err");
  });

  client.on("message", (topic, message) => {
    if (topic === `${topicPrefix}/photo`) {
      handlePhotoChunk(message.toString());
      return;
    }
    if (topic !== `${topicPrefix}/telemetry`) return;
    try {
      const data = JSON.parse(message.toString());
      lastTelemetryAt = Date.now();
      lastSeenEl.textContent = `Update terakhir: ${new Date(lastTelemetryAt).toLocaleTimeString("id-ID")}`;
      updateEspHealth();

      const tempValid = typeof data.temp === "number" && !Number.isNaN(data.temp);
      const humValid = typeof data.hum === "number" && !Number.isNaN(data.hum);

      tempEl.textContent = tempValid ? `${data.temp.toFixed(1)} ${DEG}` : `--.- ${DEG}`;
      humEl.textContent = `Kelembapan: ${humValid ? `${data.hum.toFixed(1)}%` : "--%"} | Min: ${data.minTemp ?? "--"}${DEG} | Max: ${data.maxTemp ?? "--"}${DEG}`;
      sensorAlertEl.style.display = (!tempValid || !humValid) ? "block" : "none";

      if (typeof data.auto === "boolean") {
        autoMode = data.auto;
        modeBtn.textContent = autoMode ? "Auto: ON" : "Auto: OFF (Manual)";
      }
      if (typeof data.minTemp === "number" && document.activeElement !== minTempInput) {
        minTempInput.value = data.minTemp.toFixed(1);
      }
      if (typeof data.maxTemp === "number" && document.activeElement !== maxTempInput) {
        maxTempInput.value = data.maxTemp.toFixed(1);
      }
      if (Array.isArray(data.relays) && data.relays.length === 4) {
        relays.forEach((r, i) => { r.on = Boolean(data.relays[i]); });
        renderRelays();
      }
    } catch (_) {}
  });
}

captureBtn.onclick = () => {
  if (!client || !client.connected) {
    photoStatusEl.textContent = "Broker belum terhubung.";
    return;
  }
  photoChunks = {};
  activePhotoSessionId = Date.now();
  captureBtn.disabled = true;
  photoStatusEl.textContent = "Meminta ESP32-S3 mengambil foto...";
  publishJson(`${topicPrefix}/cmd/capture`, { capture: 1 });
  setTimeout(() => { captureBtn.disabled = false; }, 15000);
};

document.getElementById("save-limits").onclick = () => {
  const minVal = parseFloat(minTempInput.value);
  const maxVal = parseFloat(maxTempInput.value);
  if (!Number.isNaN(minVal) && !Number.isNaN(maxVal)) {
    if (minVal >= maxVal) {
      alert("Peringatan: Batas Bawah harus lebih kecil dari Batas Atas!");
      return;
    }
    publishJson(`${topicPrefix}/cmd/limits`, { min: minVal, max: maxVal });
  }
};

modeBtn.onclick = () => {
  publishJson(`${topicPrefix}/cmd/mode`, { auto: !autoMode });
};

document.getElementById("all-off").onclick = () => {
  publishJson(`${topicPrefix}/cmd/all_off`, {});
};

document.getElementById("reconnect-mqtt").onclick = () => {
  brokerUrl = brokerInput.value.trim();
  topicPrefix = prefixInput.value.trim().replace(/\/+$/, "");
  mqttUser = userInput.value.trim();
  mqttPass = passInput.value;

  localStorage.setItem("emqx_wss_url", brokerUrl);
  localStorage.setItem("emqx_topic_prefix", topicPrefix);
  localStorage.setItem("emqx_user", mqttUser);
  localStorage.setItem("emqx_pass", mqttPass);

  connectMqtt();
};

document.getElementById("clear-auth").onclick = () => {
  mqttUser = "";
  mqttPass = "";
  userInput.value = "";
  passInput.value = "";
  localStorage.removeItem("emqx_user");
  localStorage.removeItem("emqx_pass");
  connectMqtt();
};

renderRelays();
connectMqtt();
updateEspHealth();
setInterval(updateEspHealth, 2000);