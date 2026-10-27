/**
 * Web Server - LoRa Gateway Admin Dashboard
 * Provides web interface for sensor management and command sending
 */

#include "web_server.h"
#include "device_registry.h"
#include "command_sender.h"
#include "database_manager.h"
#include "mqtt_bridge.h"
#include "lora_protocol.h"
#include "lora_config.h"
#include "version.h"
#include "secrets.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

AsyncWebServer server(80);

// API service URL for proxying requests
#ifndef DB_API_URL
#define DB_API_URL "http://192.168.0.167:3000/api"
#endif

// --------------------------------------------------------------------
// Authentication (HTTP Basic). Enabled when WEB_AUTH_USER and
// WEB_AUTH_PASS are both defined in secrets.h.
// --------------------------------------------------------------------
#if defined(WEB_AUTH_USER) && defined(WEB_AUTH_PASS)
#define WEB_AUTH_ENABLED 1
static bool webAuth(AsyncWebServerRequest* request) {
    if (request->authenticate(WEB_AUTH_USER, WEB_AUTH_PASS)) return true;
    request->requestAuthentication("LoRa Gateway");
    return false;
}
#else
#define WEB_AUTH_ENABLED 0
static bool webAuth(AsyncWebServerRequest*) { return true; }
#endif

// --------------------------------------------------------------------
// Cached history from the API service.
//
// The async web server runs its handlers on the async_tcp task, where a
// blocking HTTPClient request stalls every other connection and can trip
// the watchdog. Instead the main loop (webServerLoop) refreshes these
// caches every few seconds while a dashboard is open, and the handlers
// only copy a string under a mutex.
// --------------------------------------------------------------------
static SemaphoreHandle_t cacheMutex = NULL;
static String eventsCache = "[]";
static String commandsCache = "[]";
static bool eventsCacheOk = false;
static bool commandsCacheOk = false;
static uint32_t lastEventsFetch = 0;
static uint32_t lastCommandsFetch = 0;
static uint32_t lastDashboardPoll = 0;   // last time a browser asked for history

static const uint32_t CACHE_REFRESH_MS = 10000;   // refresh period while a dashboard is open
static const uint32_t DASHBOARD_IDLE_MS = 60000;  // stop refreshing this long after the last poll

static void setCache(String& cache, bool& okFlag, const String& value, bool ok) {
    if (cacheMutex) xSemaphoreTake(cacheMutex, portMAX_DELAY);
    if (ok) cache = value;
    okFlag = ok;
    if (cacheMutex) xSemaphoreGive(cacheMutex);
}

static String readCache(const String& cache, bool& okFlag, bool& okOut) {
    if (cacheMutex) xSemaphoreTake(cacheMutex, portMAX_DELAY);
    String copy = cache;
    okOut = okFlag;
    if (cacheMutex) xSemaphoreGive(cacheMutex);
    return copy;
}

static void refreshCache(const char* path, String& cache, bool& okFlag) {
    HTTPClient http;
    String url = String(DB_API_URL) + path;
    http.begin(url);
    http.setTimeout(2500);
    int httpCode = http.GET();
    if (httpCode == 200) {
        setCache(cache, okFlag, http.getString(), true);
    } else {
        Serial.printf("[WEB] History fetch %s failed: HTTP %d\n", path, httpCode);
        setCache(cache, okFlag, "", false);
    }
    http.end();
}

void webServerLoop() {
    if (WiFi.status() != WL_CONNECTED) return;
    uint32_t now = millis();
    // Only poll the API while someone is actually looking at the dashboard
    if (lastDashboardPoll == 0 || now - lastDashboardPoll > DASHBOARD_IDLE_MS) return;

    // One fetch per loop iteration so the two never block back to back
    if (now - lastEventsFetch >= CACHE_REFRESH_MS) {
        lastEventsFetch = now;
        refreshCache("/events?limit=25", eventsCache, eventsCacheOk);
        return;
    }
    if (now - lastCommandsFetch >= CACHE_REFRESH_MS) {
        lastCommandsFetch = now;
        refreshCache("/commands?limit=25", commandsCache, commandsCacheOk);
    }
}

// Mark that a dashboard is open and make the first refresh happen promptly
static void noteDashboardPoll() {
    bool wasIdle = (lastDashboardPoll == 0 || millis() - lastDashboardPoll > DASHBOARD_IDLE_MS);
    lastDashboardPoll = millis();
    if (wasIdle) {
        lastEventsFetch = 0;
        lastCommandsFetch = 0;
    }
}

// HTML Dashboard (embedded)
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>LoRa Gateway Admin</title>
    <style>
        :root {
            --bg: #0f0f0f; --panel: #1a1a1a; --panel-2: #151515; --line: #2a2a2a;
            --text: #e0e0e0; --muted: #888; --accent: #00d4ff;
            --ok: #10b981; --ok-bg: #065f46; --warn: #f59e0b; --warn-bg: #78350f;
            --bad: #ef4444; --bad-bg: #7f1d1d; --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
        }
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: system-ui, -apple-system, "Segoe UI", Roboto, Ubuntu, sans-serif;
            background: var(--bg); color: var(--text);
        }
        .app { display: flex; min-height: 100vh; }
        .sidebar {
            width: 280px; flex: none; background: var(--panel); border-right: 1px solid var(--line);
            position: sticky; top: 0; height: 100vh; overflow-y: auto;
        }
        .sidebar-header { padding: 20px; border-bottom: 1px solid var(--line); }
        .sidebar-header h1 { color: var(--accent); font-size: 1.3em; font-weight: 500; margin-bottom: 4px; }
        .sidebar-header .subtitle { color: var(--muted); font-size: 0.85em; }
        .gateway-status { padding: 20px; }
        .status-title { color: var(--accent); font-size: 0.8em; font-weight: 600; margin-bottom: 14px; text-transform: uppercase; letter-spacing: 0.5px; }
        .status-item { margin-bottom: 12px; }
        .status-label { color: var(--muted); font-size: 0.78em; margin-bottom: 3px; }
        .status-value { color: #fff; font-size: 0.95em; word-break: break-all; }
        .badge { display: inline-block; padding: 3px 10px; border-radius: 12px; font-size: 0.78em; font-weight: 600; background: var(--ok-bg); color: var(--ok); white-space: nowrap; }
        .badge.warn { background: var(--warn-bg); color: var(--warn); }
        .badge.bad { background: var(--bad-bg); color: var(--bad); }
        .badge.muted { background: #2a2a2a; color: var(--muted); }
        .main { flex: 1; min-width: 0; padding: 24px; }
        .section-header { margin-bottom: 18px; display: flex; justify-content: space-between; align-items: baseline; gap: 12px; flex-wrap: wrap; }
        .section-header h2 { color: #fff; font-size: 1.5em; font-weight: 500; }
        .section-header .description { color: var(--muted); font-size: 0.9em; }
        .section { margin-top: 36px; }
        .sensors-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(340px, 1fr)); gap: 18px; }
        .card { background: var(--panel); border-radius: 12px; padding: 18px; border: 1px solid var(--line); }
        .card.offline { border-color: #3a2a2a; opacity: 0.85; }
        .sensor-header { display: flex; justify-content: space-between; align-items: center; gap: 10px; margin-bottom: 14px; padding-bottom: 10px; border-bottom: 2px solid var(--line); }
        .sensor-name { font-size: 1.15em; font-weight: 500; color: var(--accent); word-break: break-word; }
        .sensor-type { color: var(--muted); font-size: 0.8em; }
        .readings { display: grid; grid-template-columns: repeat(auto-fit, minmax(90px, 1fr)); gap: 8px; margin-bottom: 12px; }
        .reading { background: var(--panel-2); padding: 10px; border-radius: 8px; text-align: center; }
        .reading .v { font-size: 1.35em; font-weight: 600; color: #fff; }
        .reading .l { font-size: 0.72em; color: var(--muted); margin-top: 2px; text-transform: uppercase; letter-spacing: 0.3px; }
        .reading.empty .v { color: var(--muted); font-size: 0.95em; font-weight: 400; }
        .meta { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 12px; }
        .stat { background: var(--panel-2); padding: 8px 10px; border-radius: 8px; }
        .stat.wide { grid-column: span 2; }
        .stat-label { font-size: 0.72em; color: var(--muted); }
        .stat-value { font-size: 0.95em; color: #fff; margin-top: 3px; word-break: break-word; }
        .stat-value.mono { font-family: var(--mono); font-size: 0.85em; }
        .stat.queue-active { background: #1e3a5f; border: 1px solid #2563eb; }
        details.health { margin-bottom: 12px; }
        details.health summary { cursor: pointer; color: var(--muted); font-size: 0.85em; padding: 4px 0; }
        details.health .meta { margin-top: 8px; margin-bottom: 0; }
        .commands { display: grid; grid-template-columns: repeat(2, 1fr); gap: 8px; }
        button {
            font: inherit; font-size: 0.88em; font-weight: 500; color: #fff; border: none; cursor: pointer;
            padding: 9px 12px; border-radius: 8px; background: linear-gradient(135deg, #1e3a8a, #3b82f6); transition: transform 0.15s, box-shadow 0.15s;
        }
        button:hover { transform: translateY(-1px); box-shadow: 0 4px 12px rgba(59,130,246,0.35); }
        button:active { transform: translateY(0); }
        button:disabled { opacity: 0.5; cursor: default; transform: none; box-shadow: none; }
        .btn-danger { background: linear-gradient(135deg, #991b1b, #dc2626); }
        .btn-success { background: linear-gradient(135deg, #065f46, #10b981); }
        .btn-ghost { background: #2a2a2a; color: var(--muted); }
        input[type="number"], input[type="text"] {
            font: inherit; background: var(--panel-2); border: 1px solid #3a3a3a; color: #fff;
            padding: 8px; border-radius: 6px; width: 100%; margin-top: 4px;
        }
        .command-group { grid-column: span 2; background: var(--panel-2); padding: 10px 12px; border-radius: 8px; }
        .command-group label { display: block; font-size: 0.85em; color: var(--accent); }
        .command-group .row { display: flex; gap: 8px; align-items: flex-end; }
        .command-group .row input { flex: 1; }
        .command-group .row button { flex: none; margin-top: 4px; }
        .table-wrap { background: var(--panel); border-radius: 12px; border: 1px solid var(--line); overflow-x: auto; }
        table { width: 100%; border-collapse: collapse; min-width: 560px; }
        th { text-align: left; padding: 10px 12px; color: var(--accent); font-weight: 500; font-size: 0.85em; border-bottom: 1px solid var(--line); }
        td { padding: 10px 12px; border-bottom: 1px solid var(--line); font-size: 0.9em; vertical-align: top; }
        td.time { color: var(--muted); font-size: 0.82em; white-space: nowrap; }
        td.mono { font-family: var(--mono); font-size: 0.82em; }
        .empty-note { color: var(--muted); text-align: center; padding: 28px; }
        .loading { text-align: center; padding: 40px; color: var(--muted); }
        @keyframes spin { to { transform: rotate(360deg); } }
        .spinner { display: inline-block; width: 36px; height: 36px; border: 4px solid var(--line); border-top-color: var(--accent); border-radius: 50%; animation: spin 1s linear infinite; }
        .toast { position: fixed; bottom: 24px; right: 24px; left: auto; max-width: calc(100vw - 48px); background: var(--panel); color: #fff; padding: 14px 18px; border-radius: 8px; border: 1px solid var(--line); box-shadow: 0 4px 20px rgba(0,0,0,0.5); z-index: 10000; animation: slideIn 0.3s ease-out; }
        .toast.success { border-color: var(--ok); background: var(--ok-bg); }
        .toast.error { border-color: var(--bad); background: var(--bad-bg); }
        @keyframes slideIn { from { transform: translateY(20px); opacity: 0; } to { transform: none; opacity: 1; } }
        @media (max-width: 900px) {
            .app { flex-direction: column; }
            .sidebar { width: auto; height: auto; position: static; border-right: none; border-bottom: 1px solid var(--line); }
            .gateway-status { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 10px 16px; }
            .status-title { grid-column: 1 / -1; margin-bottom: 0; }
            .status-item { margin-bottom: 0; }
            .main { padding: 16px; }
            .sensors-grid { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="app">
        <div class="sidebar">
            <div class="sidebar-header">
                <h1>🌐 LoRa Gateway</h1>
                <div class="subtitle">Admin Dashboard</div>
            </div>
            <div class="gateway-status">
                <div class="status-title">Gateway Status</div>
                <div class="status-item"><div class="status-label">WiFi</div><div class="status-value"><span class="badge muted" id="wifi-status">● CHECKING</span></div></div>
                <div class="status-item"><div class="status-label">MQTT Broker</div><div class="status-value"><span class="badge muted" id="mqtt-status">● CHECKING</span></div></div>
                <div class="status-item"><div class="status-label">Database API</div><div class="status-value"><span class="badge muted" id="db-status">● CHECKING</span> <span class="sensor-type" id="db-queue"></span></div></div>
                <div class="status-item"><div class="status-label">IP Address</div><div class="status-value" id="gateway-ip">…</div></div>
                <div class="status-item"><div class="status-label">WiFi Signal</div><div class="status-value" id="wifi-rssi">…</div></div>
                <div class="status-item"><div class="status-label">Uptime</div><div class="status-value" id="uptime">…</div></div>
                <div class="status-item"><div class="status-label">Free Memory</div><div class="status-value" id="free-mem">…</div></div>
                <div class="status-item"><div class="status-label">LoRa Radio</div><div class="status-value" id="lora-radio">…</div></div>
                <div class="status-item"><div class="status-label">Firmware</div><div class="status-value" id="firmware">…</div></div>
                <div class="status-item"><div class="status-label">Sensors</div><div class="status-value" id="sensor-count">…</div></div>
            </div>
        </div>

        <div class="main">
            <div class="section-header">
                <h2>Sensors</h2>
                <div class="description">Last readings, link quality and remote control</div>
            </div>
            <div id="sensors" class="sensors-grid">
                <div class="loading"><div class="spinner"></div><p>Loading sensors…</p></div>
            </div>

            <div class="section">
                <div class="section-header">
                    <h2>Command History</h2>
                    <div class="description">Commands queued by this gateway, from the database</div>
                </div>
                <div id="commands" class="table-wrap"><div class="loading"><div class="spinner"></div></div></div>
            </div>

            <div class="section">
                <div class="section-header">
                    <h2>Recent Events</h2>
                    <div class="description">System and device events, from the database</div>
                </div>
                <div id="events" class="table-wrap"><div class="loading"><div class="spinner"></div></div></div>
            </div>
        </div>
    </div>

    <script>
        // Escape anything that came from a device or the database before it
        // goes into innerHTML. Device names can be set remotely (CMD_SET_NAME).
        function esc(v) {
            return String(v == null ? '' : v)
                .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
                .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
        }
        function attr(v) { return esc(v); }

        const SENSOR_ICONS = { BME280: '🌤️', DHT22: '💧', DS18B20: '🌡️' };
        const CMD_NAMES = { 1: 'calibrate', 2: 'set_baseline', 3: 'clear_baseline', 4: 'restart', 5: 'status',
                            6: 'set_sleep', 7: 'set_interval', 8: 'ota_start', 9: 'time_sync', 10: 'set_name' };
        const TREND = { 0: '↓ falling', 1: '→ steady', 2: '↑ rising' };

        function formatAgo(seconds) {
            if (seconds == null) return '—';
            if (seconds < 60) return seconds + 's ago';
            const min = Math.floor(seconds / 60);
            if (min < 60) return min + 'm ago';
            const hr = Math.floor(min / 60);
            if (hr < 24) return hr + 'h ' + (min % 60) + 'm ago';
            return Math.floor(hr / 24) + 'd ' + (hr % 24) + 'h ago';
        }
        function formatDuration(sec) {
            const d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
            return d > 0 ? `${d}d ${h}h ${m}m` : `${h}h ${m}m ${s}s`;
        }
        function setBadge(id, text, cls) {
            const el = document.getElementById(id);
            el.textContent = '● ' + text;
            el.className = 'badge' + (cls ? ' ' + cls : '');
        }

        function updateGatewayStatus() {
            fetch('/api/gateway').then(r => r.json()).then(d => {
                document.getElementById('gateway-ip').textContent = d.ip || 'Unknown';
                document.getElementById('wifi-rssi').textContent = d.wifi_rssi ? d.wifi_rssi + ' dBm' : 'Unknown';
                document.getElementById('free-mem').textContent = d.free_heap
                    ? Math.round(d.free_heap / 1024) + ' KB (min ' + Math.round(d.min_free_heap / 1024) + ' KB)' : 'Unknown';
                document.getElementById('uptime').textContent = d.uptime ? formatDuration(Math.floor(d.uptime / 1000)) : 'Unknown';
                document.getElementById('lora-radio').textContent = d.lora_frequency + ' MHz · SF' + d.lora_sf + ' · ' + d.lora_bw + ' kHz';
                document.getElementById('firmware').textContent = d.firmware || 'Unknown';
                document.getElementById('sensor-count').textContent = d.sensors_online + ' online / ' + d.sensors_total + ' total';

                setBadge('wifi-status', d.wifi_connected ? 'CONNECTED' : 'DISCONNECTED', d.wifi_connected ? '' : 'bad');
                setBadge('mqtt-status', d.mqtt_connected ? 'CONNECTED' : 'DISCONNECTED', d.mqtt_connected ? '' : 'bad');
                if (d.db_status === 'connected') setBadge('db-status', 'CONNECTED', '');
                else if (d.db_status === 'reconnecting') setBadge('db-status', 'RECONNECTING', 'warn');
                else setBadge('db-status', 'DISCONNECTED', 'bad');
                document.getElementById('db-queue').textContent = d.db_queue > 0 ? d.db_queue + ' queued writes' : '';
            }).catch(e => console.error('Gateway stats error:', e));
        }

        function showToast(message, type = 'success') {
            const toast = document.createElement('div');
            toast.className = 'toast ' + type;
            toast.textContent = message;
            document.body.appendChild(toast);
            setTimeout(() => toast.remove(), 3500);
        }

        // value: number or string; numeric=true parses it as an integer/float
        function sendCommand(deviceId, action, value = null, numeric = true) {
            const payload = { device_id: deviceId, action: action };
            if (value !== null && value !== undefined && value !== '') {
                payload.value = numeric ? Number(value) : value;
            }
            fetch('/api/command', {
                method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) { showToast('✅ Queued: ' + action, 'success'); setTimeout(loadCommands, 1500); }
                else showToast('❌ Failed: ' + (data.error || 'Unknown error'), 'error');
            })
            .catch(e => showToast('❌ Error: ' + e.message, 'error'));
        }
        function confirmCommand(deviceId, action, label) {
            if (confirm(label + '?')) sendCommand(deviceId, action);
        }
        function removeDevice(deviceId, name) {
            if (!confirm('Remove "' + name + '" from the gateway registry? It will be re-added automatically if it transmits again.')) return;
            fetch('/api/device?id=' + encodeURIComponent(deviceId), { method: 'DELETE' })
            .then(r => r.json())
            .then(data => {
                if (data.success) { showToast('✅ Device removed', 'success'); loadSensors(true); }
                else showToast('❌ Failed: ' + (data.error || 'Unknown error'), 'error');
            })
            .catch(e => showToast('❌ Error: ' + e.message, 'error'));
        }
        function inputValue(id) { return document.getElementById(id).value; }

        function renderReadings(d) {
            const r = d.reading;
            if (!r) return `<div class="readings"><div class="reading empty" style="grid-column:1/-1"><div class="v">No readings received yet</div></div></div>`;
            const cells = [];
            cells.push(`<div class="reading"><div class="v">${esc(r.temperature)}°</div><div class="l">Temp °C</div></div>`);
            if (r.humidity !== undefined) cells.push(`<div class="reading"><div class="v">${esc(r.humidity)}%</div><div class="l">Humidity</div></div>`);
            if (r.pressure !== undefined) {
                const chg = Number(r.pressureChange);
                const chgTxt = (chg > 0 ? '+' : '') + chg.toFixed(2);
                cells.push(`<div class="reading"><div class="v">${esc(r.pressure)}</div><div class="l">hPa · ${esc(TREND[r.pressureTrend] || '')} (${esc(chgTxt)})</div></div>`);
            }
            const battCls = r.batteryPercent <= 15 ? 'style="color:var(--bad)"' : (r.batteryPercent <= 30 ? 'style="color:var(--warn)"' : '');
            cells.push(`<div class="reading"><div class="v" ${battCls}>${esc(r.batteryPercent)}%</div><div class="l">Battery · ${esc(r.batteryVoltage)} V</div></div>`);
            if (r.gps) {
                if (r.gps.fix) {
                    const lat = Number(r.gps.latitude).toFixed(5), lon = Number(r.gps.longitude).toFixed(5);
                    cells.push(`<div class="reading" style="grid-column:1/-1"><div class="v" style="font-size:1em"><a style="color:#fff;text-decoration:none" target="_blank" rel="noopener" href="https://www.openstreetmap.org/?mlat=${attr(lat)}&mlon=${attr(lon)}#map=16/${attr(lat)}/${attr(lon)}">📍 ${esc(lat)}, ${esc(lon)}</a></div><div class="l">GPS · ${esc(r.gps.altitude)} m · ${esc(r.gps.satellites)} sats · HDOP ${esc(r.gps.hdop)}</div></div>`);
                } else {
                    cells.push(`<div class="reading"><div class="v" style="font-size:1em">No fix</div><div class="l">GPS · ${esc(r.gps.satellites)} sats</div></div>`);
                }
            }
            return `<div class="readings">${cells.join('')}</div>`;
        }

        function renderHealth(d) {
            const h = d.health;
            if (!h) return '';
            const healthy = h.sensorHealthy ? '<span class="badge">OK</span>' : '<span class="badge bad">FAULT</span>';
            return `<details class="health"><summary>Device health · reported ${esc(formatAgo(h.ageSeconds))}</summary>
                <div class="meta">
                    <div class="stat"><div class="stat-label">Sensor</div><div class="stat-value">${healthy}</div></div>
                    <div class="stat"><div class="stat-label">Uptime</div><div class="stat-value">${esc(formatDuration(h.uptimeSec))}</div></div>
                    <div class="stat"><div class="stat-label">Wake Count</div><div class="stat-value">${esc(h.wakeCount)}</div></div>
                    <div class="stat"><div class="stat-label">Free Heap</div><div class="stat-value">${esc(h.freeHeapKb)} KB</div></div>
                    <div class="stat"><div class="stat-label">Sensor Failures</div><div class="stat-value">${esc(h.sensorFailures)}</div></div>
                    <div class="stat"><div class="stat-label">TX Failures</div><div class="stat-value">${esc(h.txFailures)} · ${esc(h.txPower)} dBm</div></div>
                </div></details>`;
        }

        function renderSensor(d) {
            const type = d.sensorType || 'Unknown';
            const isBME280 = type === 'BME280';
            const icon = SENSOR_ICONS[type] || '❓';
            const online = d.online;
            const statusBadge = online ? '<span class="badge">ONLINE</span>'
                : `<span class="badge bad" title="Expected every ~${attr(d.expectedPeriodSec)}s">OFFLINE</span>`;
            const queue = d.cmdQueueCount > 0
                ? `${esc(d.cmdQueueCount)} pending: ${d.cmdQueue.map(c => esc(c.type) + (c.retries > 0 ? ' (retry ' + esc(c.retries) + ')' : '')).join(', ')}`
                : 'Empty';
            const id = d.id;
            return `
            <div class="card ${online ? '' : 'offline'}">
                <div class="sensor-header">
                    <div><div class="sensor-name">${icon} ${esc(d.name)}</div><div class="sensor-type">${esc(type)}${d.reading && d.reading.gps ? ' + GPS' : ''} · ${esc(d.location)}</div></div>
                    ${statusBadge}
                </div>
                ${renderReadings(d)}
                <div class="meta">
                    <div class="stat wide"><div class="stat-label">Device ID</div><div class="stat-value mono">${esc(id)}</div></div>
                    <div class="stat"><div class="stat-label">Last Seen</div><div class="stat-value">${esc(formatAgo(d.lastSeenSeconds))}</div></div>
                    <div class="stat"><div class="stat-label">Packets</div><div class="stat-value">${esc(d.packetCount)} · seq #${esc(d.lastSequence)}</div></div>
                    <div class="stat"><div class="stat-label">RSSI / SNR</div><div class="stat-value">${esc(d.lastRssi)} dBm / ${esc(d.lastSnr)} dB</div></div>
                    <div class="stat"><div class="stat-label">Sleep / Interval</div><div class="stat-value">${esc(d.deepSleepSec)}s / ${esc(d.sensorInterval)}s</div></div>
                    <div class="stat wide ${d.cmdQueueCount > 0 ? 'queue-active' : ''}"><div class="stat-label">Command Queue</div><div class="stat-value">${queue}</div></div>
                </div>
                ${renderHealth(d)}
                <div class="commands">
                    <button onclick="sendCommand('${attr(id)}', 'status')">📊 Request Status</button>
                    <button class="btn-danger" onclick="confirmCommand('${attr(id)}', 'restart', 'Restart ${attr(d.name)}')">🔄 Restart</button>
                    ${isBME280 ? `<button onclick="sendCommand('${attr(id)}', 'calibrate')">🎯 Calibrate Baseline</button>
                    <button onclick="confirmCommand('${attr(id)}', 'clear_baseline', 'Clear pressure baseline on ${attr(d.name)}')">🗑️ Clear Baseline</button>
                    <div class="command-group"><label>Pressure Baseline (hPa)</label><div class="row">
                        <input type="number" id="baseline_${attr(id)}" value="${d.reading && d.reading.pressure !== undefined ? attr(d.reading.pressure) : '1013.25'}" min="300" max="1100" step="0.01">
                        <button class="btn-success" onclick="sendCommand('${attr(id)}', 'set_baseline', inputValue('baseline_${attr(id)}'))">Set</button></div></div>` : ''}
                    <div class="command-group"><label>Deep Sleep (seconds, 0–3600)</label><div class="row">
                        <input type="number" id="sleep_${attr(id)}" value="${attr(d.deepSleepSec)}" min="0" max="3600">
                        <button class="btn-success" onclick="sendCommand('${attr(id)}', 'set_sleep', inputValue('sleep_${attr(id)}'))">Set</button></div></div>
                    <div class="command-group"><label>Sensor Interval (seconds, 5–3600)</label><div class="row">
                        <input type="number" id="interval_${attr(id)}" value="${attr(d.sensorInterval)}" min="5" max="3600">
                        <button class="btn-success" onclick="sendCommand('${attr(id)}', 'set_interval', inputValue('interval_${attr(id)}'))">Set</button></div></div>
                    <div class="command-group"><label>Device Name (max 31 chars)</label><div class="row">
                        <input type="text" id="name_${attr(id)}" value="${attr(d.name)}" maxlength="31">
                        <button class="btn-success" onclick="sendCommand('${attr(id)}', 'set_name', inputValue('name_${attr(id)}'), false)">Rename</button></div></div>
                    <button class="btn-ghost" style="grid-column: span 2" onclick="removeDevice('${attr(id)}', '${attr(d.name)}')">✖ Remove from registry</button>
                </div>
            </div>`;
        }

        function loadSensors(force = false) {
            // Skip refresh while the user is editing an input field
            const activeEl = document.activeElement;
            if (!force && activeEl && activeEl.tagName === 'INPUT') return;

            fetch('/api/devices').then(r => r.json()).then(devices => {
                const container = document.getElementById('sensors');
                if (devices.length === 0) {
                    container.innerHTML = '<div class="loading"><p>No sensors registered yet</p></div>';
                    return;
                }
                // Preserve any edited input values across the re-render
                const saved = {};
                document.querySelectorAll('.commands input').forEach(i => { if (i.dataset.dirty) saved[i.id] = i.value; });
                devices.sort((a, b) => (a.online === b.online ? a.name.localeCompare(b.name) : (a.online ? -1 : 1)));
                container.innerHTML = devices.map(renderSensor).join('');
                document.querySelectorAll('.commands input').forEach(i => {
                    i.addEventListener('input', () => { i.dataset.dirty = '1'; });
                    if (saved[i.id] !== undefined) { i.value = saved[i.id]; i.dataset.dirty = '1'; }
                });
            }).catch(e => {
                document.getElementById('sensors').innerHTML = '<div class="loading"><p>Error loading sensors: ' + esc(e.message) + '</p></div>';
            });
        }

        function renderTable(headers, rows) {
            return '<table><thead><tr>' + headers.map(h => '<th>' + esc(h) + '</th>').join('') + '</tr></thead><tbody>' + rows.join('') + '</tbody></table>';
        }
        function unavailable(el, what) {
            el.innerHTML = '<p class="empty-note">' + what + ' unavailable (database API not reachable)</p>';
        }

        function loadEvents() {
            const el = document.getElementById('events');
            fetch('/api/events').then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); }).then(data => {
                if (!data || data.length === 0) { el.innerHTML = '<p class="empty-note">No events yet</p>'; return; }
                const sevCls = { 0: '', 1: 'warn', 2: 'bad' }, sevLabel = { 0: 'INFO', 1: 'WARNING', 2: 'ERROR' };
                el.innerHTML = renderTable(['Time', 'Device', 'Severity', 'Message'], data.map(e => `<tr>
                    <td class="time">${esc(new Date(e.received_at).toLocaleString())}</td>
                    <td>${esc(e.device_name)}</td>
                    <td><span class="badge ${sevCls[e.severity] ?? 'muted'}">${esc(sevLabel[e.severity] || 'UNKNOWN')}</span></td>
                    <td>${esc(e.message)}</td></tr>`));
            }).catch(() => unavailable(el, 'Events'));
        }

        function loadCommands() {
            const el = document.getElementById('commands');
            fetch('/api/commands').then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); }).then(data => {
                if (!data || data.length === 0) { el.innerHTML = '<p class="empty-note">No commands recorded yet</p>'; return; }
                const statusCls = { queued: 'warn', sent: '', confirmed: '', failed: 'bad', timeout: 'bad' };
                el.innerHTML = renderTable(['Time', 'Device', 'Command', 'Value', 'Status'], data.map(c => {
                    const p = c.parameters || {};
                    const name = p.action || CMD_NAMES[c.command_type] || ('type ' + c.command_type);
                    return `<tr>
                    <td class="time">${esc(new Date(c.created_at).toLocaleString())}</td>
                    <td>${esc(c.device_name)}</td>
                    <td class="mono">${esc(name)}</td>
                    <td class="mono">${p.value !== undefined ? esc(p.value) : '—'}</td>
                    <td><span class="badge ${statusCls[c.status] ?? 'muted'}">${esc(String(c.status).toUpperCase())}</span></td></tr>`;
                }));
            }).catch(() => unavailable(el, 'Command history'));
        }

        loadSensors();
        updateGatewayStatus();
        loadEvents();
        loadCommands();
        // The gateway fills its history caches shortly after the first poll
        setTimeout(() => { loadEvents(); loadCommands(); }, 3000);

        setInterval(loadSensors, 5000);
        setInterval(updateGatewayStatus, 3000);
        setInterval(loadEvents, 10000);
        setInterval(loadCommands, 10000);
    </script>
</body>
</html>
)rawliteral";

/**
 * Initialize web server
 */
void initWebServer() {
    Serial.println("Initializing web dashboard...");

    if (cacheMutex == NULL) {
        cacheMutex = xSemaphoreCreateMutex();
    }

    // Main dashboard page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!webAuth(request)) return;
        request->send_P(200, "text/html", index_html);
    });

    // API: Get all devices (thread-safe snapshot)
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!webAuth(request)) return;
        String json = getDeviceRegistrySnapshot();
        request->send(200, "application/json", json);
    });

    // API: Get gateway status
    server.on("/api/gateway", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!webAuth(request)) return;
        JsonDocument doc;
        doc["ip"] = WiFi.localIP().toString();
        doc["ssid"] = WiFi.SSID();
        doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
        doc["wifi_rssi"] = WiFi.RSSI();
        doc["mqtt_connected"] = isMqttConnected();
        doc["free_heap"] = ESP.getFreeHeap();
        doc["min_free_heap"] = ESP.getMinFreeHeap();
        doc["uptime"] = millis();
        doc["firmware"] = getFirmwareVersion();
        doc["lora_frequency"] = LORA_FREQUENCY;
        doc["lora_sf"] = LORA_SPREADING;
        doc["lora_bw"] = LORA_BANDWIDTH;
        doc["auth_enabled"] = (bool)WEB_AUTH_ENABLED;

        // Database status
        DatabaseStatus dbStatus = dbManager.getStatus();
        doc["db_status"] = (dbStatus == DB_CONNECTED) ? "connected" :
                           (dbStatus == DB_RECONNECTING) ? "reconnecting" : "disconnected";
        doc["db_queue"] = dbManager.getQueueDepth();
        doc["db_failed_writes"] = dbManager.getFailedWrites();

        // Sensor counts (online = reported within its expected window)
        int total = 0, online = 0;
        {
            JsonDocument devices;
            deserializeJson(devices, getDeviceRegistrySnapshot());
            for (JsonObject d : devices.as<JsonArray>()) {
                total++;
                if (d["online"] | false) online++;
            }
        }
        doc["sensors_total"] = total;
        doc["sensors_online"] = online;

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // API: Recent events (served from the cache refreshed by webServerLoop)
    server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!webAuth(request)) return;
        noteDashboardPoll();
        bool ok;
        String payload = readCache(eventsCache, eventsCacheOk, ok);
        request->send(ok ? 200 : 503, "application/json", ok ? payload : "[]");
    });

    // API: Command history (served from the cache refreshed by webServerLoop)
    server.on("/api/commands", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!webAuth(request)) return;
        noteDashboardPoll();
        bool ok;
        String payload = readCache(commandsCache, commandsCacheOk, ok);
        request->send(ok ? 200 : 503, "application/json", ok ? payload : "[]");
    });

    // API: Send command to sensor
    server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            if (!webAuth(request)) return;

            // Parse JSON body
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, data, len);

            if (error) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
                return;
            }

            const char* deviceIdStr = doc["device_id"];
            const char* action = doc["action"];

            if (!deviceIdStr || !action) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing device_id or action\"}");
                return;
            }

            // Parse device ID
            uint64_t deviceId = strtoull(deviceIdStr, nullptr, 16);

            // Map action to command. cmdType and cmdLog are recorded in the
            // command history (PostgreSQL via API); parameters must be JSON.
            bool success = false;
            uint8_t cmdType = 0;
            JsonDocument cmdLog;
            cmdLog["action"] = action;
            if (strcmp(action, "set_interval") == 0) {
                uint16_t value = doc["value"].as<uint16_t>();
                if (value == 0) value = 90;  // Default if missing

                // Validate interval range: must be between 5 and 3600 seconds
                if (value < 5 || value > 3600) {
                    request->send(400, "application/json", "{\"success\":false,\"error\":\"Interval must be between 5 and 3600 seconds\"}");
                    return;
                }

                Serial.printf("[WEB] set_interval: value=%u\n", value);

                // Format as ASCII decimal string (matches MQTT handler and command_sender)
                char valueStr[8];
                snprintf(valueStr, sizeof(valueStr), "%u", value);
                cmdType = CMD_SET_INTERVAL;
                cmdLog["value"] = value;
                success = queueCommand(deviceId, cmdType, (uint8_t*)valueStr, strlen(valueStr));
            }
            else if (strcmp(action, "set_sleep") == 0) {
                uint16_t value = doc["value"].as<uint16_t>();
                if (value == 0) value = 90;  // Default if missing

                // Validate sleep value: maximum 3600 seconds
                if (value > 3600) {
                    request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid sleep value (max 3600 seconds)\"}");
                    return;
                }

                Serial.printf("[WEB] set_sleep: value=%u\n", value);

                // Format as ASCII decimal string (matches MQTT handler and command_sender)
                char valueStr[8];
                snprintf(valueStr, sizeof(valueStr), "%u", value);
                cmdType = CMD_SET_SLEEP;
                cmdLog["value"] = value;
                success = queueCommand(deviceId, cmdType, (uint8_t*)valueStr, strlen(valueStr));
            }
            else if (strcmp(action, "set_baseline") == 0) {
                float value = doc["value"].as<float>();

                // Plausible sea-level-referenced pressure range in hPa
                if (value < 300.0f || value > 1100.0f) {
                    request->send(400, "application/json", "{\"success\":false,\"error\":\"Baseline must be between 300 and 1100 hPa\"}");
                    return;
                }

                Serial.printf("[WEB] set_baseline: value=%.2f\n", value);

                // Format as ASCII decimal string (matches MQTT handler)
                char valueStr[16];
                snprintf(valueStr, sizeof(valueStr), "%.2f", value);
                cmdType = CMD_SET_BASELINE;
                cmdLog["value"] = value;
                success = queueCommand(deviceId, cmdType, (uint8_t*)valueStr, strlen(valueStr));
            }
            else if (strcmp(action, "calibrate") == 0) {
                cmdType = CMD_CALIBRATE;
                success = queueCommand(deviceId, cmdType, nullptr, 0);
            }
            else if (strcmp(action, "clear_baseline") == 0) {
                cmdType = CMD_CLEAR_BASELINE;
                success = queueCommand(deviceId, cmdType, nullptr, 0);
            }
            else if (strcmp(action, "restart") == 0) {
                cmdType = CMD_RESTART;
                success = queueCommand(deviceId, cmdType, nullptr, 0);
            }
            else if (strcmp(action, "status") == 0) {
                cmdType = CMD_STATUS;
                success = queueCommand(deviceId, cmdType, nullptr, 0);
            }
            else if (strcmp(action, "set_name") == 0) {
                const char* name = doc["value"].as<const char*>();
                if (!name || strlen(name) == 0) {
                    request->send(400, "application/json", "{\"success\":false,\"error\":\"Name cannot be empty\"}");
                    return;
                }
                size_t nameLen = strlen(name);
                if (nameLen > 31) nameLen = 31;  // Truncate to max length
                Serial.printf("[WEB] set_name: value=%s\n", name);
                cmdType = CMD_SET_NAME;
                cmdLog["value"] = String(name).substring(0, nameLen);
                success = queueCommand(deviceId, cmdType, (uint8_t*)name, nameLen);
            }
            else {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown action\"}");
                return;
            }

            String cmdParams;
            serializeJson(cmdLog, cmdParams);
            if (success) {
                request->send(200, "application/json", "{\"success\":true}");
                Serial.printf("[WEB] Command queued: %s for device 0x%016llX\n", action, deviceId);
                // Persist command to PostgreSQL via API
                dbManager.writeCommand(deviceId, cmdType, cmdParams, "queued");
            } else {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to queue command\"}");
                dbManager.writeCommand(deviceId, cmdType, cmdParams, "failed");
            }
        }
    );

    // API: Remove a device from the registry
    server.on("/api/device", HTTP_DELETE, [](AsyncWebServerRequest *request){
        if (!webAuth(request)) return;
        if (!request->hasParam("id")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing id parameter\"}");
            return;
        }

        String deviceIdStr = request->getParam("id")->value();
        uint64_t deviceId = strtoull(deviceIdStr.c_str(), nullptr, 16);

        if (removeDevice(deviceId)) {
            request->send(200, "application/json", "{\"success\":true}");
            Serial.printf("[WEB] Removed device 0x%016llX\n", deviceId);
        } else {
            request->send(404, "application/json", "{\"success\":false,\"error\":\"Device not found\"}");
        }
    });

    // Start server
    server.begin();
    Serial.printf("✅ Web dashboard started on port 80 (auth %s)\n", WEB_AUTH_ENABLED ? "enabled" : "disabled");
}
