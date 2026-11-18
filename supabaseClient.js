export const SUPABASE_CONFIG = {
  url: "https://shrtuvtzmhhjvpadgmcr.supabase.co",
  anonKey: "sb_publishable_xgTkr-YhiRGNwX_6IY_Qew_QV6tzICa",
  deviceId: "jalak-bali-esp32-01",
  offlineThresholdMs: 45000,
};

function buildRestHeaders(preferHeader = null) {
  const headers = {
    apikey: SUPABASE_CONFIG.anonKey,
    Authorization: `Bearer ${SUPABASE_CONFIG.anonKey}`,
    "Content-Type": "application/json",
  };
  if (preferHeader) headers["Prefer"] = preferHeader;
  return headers;
}

function restUrl(pathWithQuery) {
  return `${SUPABASE_CONFIG.url}/rest/v1${pathWithQuery}`;
}

export async function fetchSensorReadings(limit = 50, sinceIso = null) {
  let query = `/sensor_readings?device_id=eq.${encodeURIComponent(SUPABASE_CONFIG.deviceId)}&order=recorded_at.desc&limit=${Number(limit)}`;
  if (sinceIso) query += `&recorded_at=gte.${encodeURIComponent(sinceIso)}`;
  const res = await fetch(restUrl(query), { method: "GET", headers: buildRestHeaders() });
  if (!res.ok) throw new Error(`Gagal memuat sensor_readings (HTTP ${res.status}): ${await res.text()}`);
  return res.json();
}

export async function fetchDeviceStatus() {
  const query = `/device_status?device_id=eq.${encodeURIComponent(SUPABASE_CONFIG.deviceId)}&select=*`;
  const res = await fetch(restUrl(query), { method: "GET", headers: buildRestHeaders() });
  if (!res.ok) throw new Error(`Gagal memuat device_status (HTTP ${res.status}): ${await res.text()}`);
  const rows = await res.json();
  return rows[0] || null;
}

export function evaluateDeviceOnline(statusRow) {
  if (!statusRow || !statusRow.is_online || !statusRow.updated_at) return false;
  const lastUpdateMs = Date.parse(statusRow.updated_at);
  if (Number.isNaN(lastUpdateMs)) return false;
  return (Date.now() - lastUpdateMs) <= SUPABASE_CONFIG.offlineThresholdMs;
}

export async function sendDeviceCommand(commandType, payload = {}) {
  const body = {
    device_id: SUPABASE_CONFIG.deviceId,
    command_type: commandType,
    payload,
    status: "PENDING",
  };
  const res = await fetch(restUrl("/device_commands"), {
    method: "POST",
    headers: buildRestHeaders("return=representation"),
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`Gagal mengirim perintah ${commandType} (HTTP ${res.status}): ${await res.text()}`);
  const created = await res.json();
  return created[0] || null;
}

export const commands = {
  setMode: (controlMode) => sendDeviceCommand("SET_MODE", { control_mode: controlMode }),
  setSetpoints: (heaterMinC, coolerMaxC) => sendDeviceCommand("SET_SETPOINTS", { heater_min_c: Number(heaterMinC), cooler_max_c: Number(coolerMaxC) }),
  setRelays: ({ lamp1_on, lamp2_on, fan1_on, fan2_on }) => sendDeviceCommand("SET_RELAYS", {
    lamp1_on: Boolean(lamp1_on),
    lamp2_on: Boolean(lamp2_on),
    fan1_on: Boolean(fan1_on),
    fan2_on: Boolean(fan2_on),
  }),
  emergencyOff: () => sendDeviceCommand("EMERGENCY_OFF", {}),
};

export function subscribeRealtimeUpdates(supabaseClient, handlers = {}) {
  const channel = supabaseClient
    .channel(`jalak-bali-realtime-${SUPABASE_CONFIG.deviceId}`)
    .on("postgres_changes", { event: "INSERT", schema: "public", table: "sensor_readings", filter: `device_id=eq.${SUPABASE_CONFIG.deviceId}` },
      (payload) => { if (handlers.onNewReading && payload.new) handlers.onNewReading(payload.new); })
    .on("postgres_changes", { event: "*", schema: "public", table: "device_status", filter: `device_id=eq.${SUPABASE_CONFIG.deviceId}` },
      (payload) => { if (handlers.onStatusChange && payload.new) handlers.onStatusChange(payload.new); })
    .on("postgres_changes", { event: "*", schema: "public", table: "device_commands", filter: `device_id=eq.${SUPABASE_CONFIG.deviceId}` },
      (payload) => { if (handlers.onCommandChange && payload.new) handlers.onCommandChange(payload.new); })
    .subscribe((status) => { if (handlers.onConnectionState) handlers.onConnectionState(status); });

  return () => supabaseClient.removeChannel(channel);
}
