/**
 * Web Server - LoRa Gateway Admin Dashboard
 * Provides web interface for sensor management and command sending
 */

#include "web_server.h"
#include "device_registry.h"
#include "command_sender.h"
#include "lora_protocol.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

AsyncWebServer server(80);

// HTML Dashboard (embedded)
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>LoRa Gateway Admin</title>
    <link href="https://fonts.googleapis.com/css2?family=Ubuntu:wght@300;400;500;700&display=swap" rel="stylesheet">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: 'Ubuntu', sans-serif;
            background: #0f0f0f;
            color: #e0e0e0;
            overflow: hidden;
        }
        .app-container {
            display: flex;
            height: 100vh;
        }
        .sidebar {
            width: 280px;
            background: #1a1a1a;
            border-right: 1px solid #2a2a2a;
            display: flex;
            flex-direction: column;
            overflow-y: auto;
        }
        .sidebar-header {
            padding: 20px;
            border-bottom: 1px solid #2a2a2a;
        }
        .sidebar-header h1 {
            color: #00d4ff;
            font-size: 1.4em;
            font-weight: 500;
            margin-bottom: 5px;
        }
        .sidebar-header .subtitle {
            color: #888;
            font-size: 0.85em;
            font-weight: 300;
        }
        .gateway-status {
            padding: 20px;
            border-bottom: 1px solid #2a2a2a;
        }
        .status-title {
            color: #00d4ff;
            font-size: 0.9em;
            font-weight: 500;
            margin-bottom: 15px;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        .status-item {
            margin-bottom: 12px;
        }
        .status-label {
            color: #888;
            font-size: 0.8em;
            font-weight: 300;
            margin-bottom: 4px;
        }
        .status-value {
            color: #fff;
            font-size: 0.95em;
            font-weight: 400;
        }
        .status-badge {
            display: inline-block;
            padding: 4px 10px;
            border-radius: 12px;
            font-size: 0.8em;
            font-weight: 500;
            background: #065f46;
            color: #10b981;
        }
        .main-content {
            flex: 1;
            overflow-y: auto;
            padding: 30px;
        }
        .content-header {
            margin-bottom: 25px;
        }
        .content-header h2 {
            color: #fff;
            font-size: 1.8em;
            font-weight: 500;
            margin-bottom: 5px;
        }
        .content-header .description {
            color: #888;
            font-size: 0.95em;
            font-weight: 300;
        }
        .sensors-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(420px, 1fr));
            gap: 20px;
        }
        .sensor-card {
            background: #1a1a1a;
            border-radius: 12px;
            padding: 20px;
            border: 1px solid #2a2a2a;
            transition: all 0.3s;
        }
        .sensor-card:hover {
            border-color: #00d4ff;
            box-shadow: 0 0 20px rgba(0,212,255,0.3);
        }
        .sensor-header {
            display: flex;
            justify-content: space-between;
            margin-bottom: 15px;
            padding-bottom: 12px;
            border-bottom: 2px solid #2a2a2a;
        }
        .sensor-name { 
            font-size: 1.2em; 
            font-weight: 500; 
            color: #00d4ff; 
        }
        .sensor-stats {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 10px;
            margin-bottom: 15px;
        }
        .stat { 
            background: #151515;
            padding: 10px;
            border-radius: 8px;
        }
        .stat-label { 
            font-size: 0.8em; 
            color: #888;
            font-weight: 300;
        }
        .stat-value { 
            font-size: 1.05em; 
            font-weight: 500; 
            color: #fff; 
            margin-top: 5px; 
        }
        .commands {
            display: grid;
            grid-template-columns: repeat(2, 1fr);
            gap: 8px;
        }
        button {
            font-family: 'Ubuntu', sans-serif;
            background: linear-gradient(135deg, #1e3a8a, #3b82f6);
            color: white;
            border: none;
            padding: 10px 15px;
            border-radius: 8px;
            cursor: pointer;
            font-size: 0.9em;
            font-weight: 500;
            transition: all 0.2s;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 15px rgba(59,130,246,0.4);
        }
        button:active { transform: translateY(0); }
        .btn-danger { background: linear-gradient(135deg, #991b1b, #dc2626); }
        .btn-success { background: linear-gradient(135deg, #065f46, #10b981); }
        input[type="number"] {
            font-family: 'Ubuntu', sans-serif;
            background: #151515;
            border: 1px solid #3a3a3a;
            color: #fff;
            padding: 8px;
            border-radius: 6px;
            width: 100%;
            margin-top: 5px;
            font-weight: 400;
        }
        .command-group {
            grid-column: span 2;
            background: #151515;
            padding: 12px;
            border-radius: 8px;
        }
        .command-group label {
            display: block;
            margin-bottom: 8px;
            font-size: 0.9em;
            font-weight: 400;
            color: #00d4ff;
        }
        .status { 
            padding: 4px 12px;
            border-radius: 12px;
            font-size: 0.8em;
            font-weight: 500;
            background: #065f46;
            color: #10b981;
        }
        .loading {
            text-align: center;
            padding: 40px;
            color: #888;
        }
        @keyframes spin {
            to { transform: rotate(360deg); }
        }
        .spinner {
            display: inline-block;
            width: 40px;
            height: 40px;
            border: 4px solid #2a2a2a;
            border-top-color: #00d4ff;
            border-radius: 50%;
            animation: spin 1s linear infinite;
        }
    </style>
</head>
<body>
    <div class="app-container">
        <!-- Sidebar -->
        <div class="sidebar">
            <div class="sidebar-header">
                <h1>🌐 LoRa Gateway</h1>
                <div class="subtitle">Admin Dashboard</div>
            </div>
            
            <div class="gateway-status">
                <div class="status-title">Gateway Status</div>
                
                <div class="status-item">
                    <div class="status-label">System</div>
                    <div class="status-value">
                        <span class="status-badge">● ONLINE</span>
                    </div>
                </div>
                
                <div class="status-item">
                    <div class="status-label">IP Address</div>
                    <div class="status-value" id="gateway-ip">Loading...</div>
                </div>
                
                <div class="status-item">
                    <div class="status-label">WiFi Signal</div>
                    <div class="status-value" id="wifi-rssi">Loading...</div>
                </div>
                
                <div class="status-item">
                    <div class="status-label">Uptime</div>
                    <div class="status-value" id="uptime">Loading...</div>
                </div>
                
                <div class="status-item">
                    <div class="status-label">Free Memory</div>
                    <div class="status-value" id="free-mem">Loading...</div>
                </div>
                
                <div class="status-item">
                    <div class="status-label">LoRa Frequency</div>
                    <div class="status-value">915 MHz</div>
                </div>
                
                <div class="status-item">
                    <div class="status-label">Active Sensors</div>
                    <div class="status-value" id="sensor-count">0</div>
                </div>
            </div>
        </div>
        
        <!-- Main Content -->
        <div class="main-content">
            <div class="content-header">
                <h2>Connected Sensors</h2>
                <div class="description">Monitor and control your LoRa sensor network</div>
            </div>
            
            <div id="sensors" class="sensors-grid">
                <div class="loading">
                    <div class="spinner"></div>
                    <p>Loading sensors...</p>
                </div>
            </div>
        </div>
    </div>
    
    <script>
        let gatewayStartTime = Date.now();
        
        function formatTime(seconds) {
            if (seconds < 60) return seconds + 's ago';
            const min = Math.floor(seconds / 60);
            if (min < 60) return min + 'm ago';
            const hr = Math.floor(min / 60);
            if (hr < 24) return hr + 'h ago';
            const days = Math.floor(hr / 24);
            return days + 'd ago';
        }
        
        function formatUptime(ms) {
            const sec = Math.floor(ms / 1000);
            const hrs = Math.floor(sec / 3600);
            const mins = Math.floor((sec % 3600) / 60);
            const secs = sec % 60;
            return `${hrs}h ${mins}m ${secs}s`;
        }
        
        function updateGatewayStatus() {
            // Update uptime
            document.getElementById('uptime').textContent = formatUptime(Date.now() - gatewayStartTime);
            
            // Fetch gateway stats
            fetch('/api/gateway')
            .then(r => r.json())
            .then(data => {
                document.getElementById('gateway-ip').textContent = data.ip || 'Unknown';
                document.getElementById('wifi-rssi').textContent = data.wifi_rssi ? data.wifi_rssi + ' dBm' : 'Unknown';
                document.getElementById('free-mem').textContent = data.free_heap ? Math.round(data.free_heap / 1024) + ' KB' : 'Unknown';
            })
            .catch(e => console.error('Gateway stats error:', e));
        }
        
        function sendCommand(deviceId, action, value = null) {
            const payload = { device_id: deviceId, action: action };
            if (value !== null) payload.value = parseInt(value);
            
            fetch('/api/command', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    alert('✅ Command sent: ' + action);
                } else {
                    alert('❌ Failed: ' + (data.error || 'Unknown error'));
                }
            })
            .catch(e => alert('❌ Error: ' + e.message));
        }
        
        function loadSensors() {
            fetch('/api/devices')
            .then(r => r.json())
            .then(devices => {
                const container = document.getElementById('sensors');
                document.getElementById('sensor-count').textContent = devices.length;
                
                if (devices.length === 0) {
                    container.innerHTML = '<div class="loading"><p>No sensors registered yet</p></div>';
                    return;
                }
                
                // Preserve input values before refresh
                const savedValues = {};
                document.querySelectorAll('input[type="number"]').forEach(input => {
                    savedValues[input.id] = input.value;
                });
                
                container.innerHTML = devices.map(d => `
                    <div class="sensor-card">
                        <div class="sensor-header">
                            <div class="sensor-name">${d.name}</div>
                            <div class="status">ONLINE</div>
                        </div>
                        <div class="sensor-stats">
                            <div class="stat">
                                <div class="stat-label">Device ID</div>
                                <div class="stat-value">${d.id.substring(0,12)}...</div>
                            </div>
                            <div class="stat">
                                <div class="stat-label">Location</div>
                                <div class="stat-value">${d.location}</div>
                            </div>
                            <div class="stat">
                                <div class="stat-label">Last Seen</div>
                                <div class="stat-value">${formatTime(d.lastSeenSeconds)}</div>
                            </div>
                            <div class="stat">
                                <div class="stat-label">Packets</div>
                                <div class="stat-value">${d.packetCount}</div>
                            </div>
                            <div class="stat">
                                <div class="stat-label">RSSI / SNR</div>
                                <div class="stat-value">${d.lastRssi} dBm / ${d.lastSnr} dB</div>
                            </div>
                            <div class="stat">
                                <div class="stat-label">Sequence</div>
                                <div class="stat-value">#${d.lastSequence}</div>
                            </div>
                            <div class="stat" style="grid-column: span 2; ${d.cmdQueueCount > 0 ? 'background: #1e3a5f; border: 1px solid #2563eb;' : ''}">
                                <div class="stat-label">Command Queue</div>
                                <div class="stat-value">
                                    ${d.cmdQueueCount > 0 
                                        ? d.cmdQueueCount + ' pending: ' + d.cmdQueue.map(c => c.type + (c.retries > 0 ? ' (retry ' + c.retries + ')' : '')).join(', ')
                                        : 'Empty'}
                                </div>
                            </div>
                        </div>
                        <div class="commands">
                            <button onclick="sendCommand('${d.id}', 'status')">📊 Status</button>
                            <button class="btn-danger" onclick="sendCommand('${d.id}', 'restart')">🔄 Restart</button>
                            <button onclick="sendCommand('${d.id}', 'calibrate')">🎯 Calibrate</button>
                            <button onclick="sendCommand('${d.id}', 'clear_baseline')">🗑️ Clear Baseline</button>
                            <div class="command-group">
                                <label>Sleep Interval (seconds)</label>
                                <input type="number" id="sleep_${d.id}" value="90" min="10" max="3600">
                                <button class="btn-success" style="margin-top:8px" onclick="sendCommand('${d.id}', 'set_sleep', document.getElementById('sleep_${d.id}').value)">Set Sleep</button>
                            </div>
                            <div class="command-group">
                                <label>Sensor Interval (seconds)</label>
                                <input type="number" id="interval_${d.id}" value="60" min="10" max="3600">
                                <button class="btn-success" style="margin-top:8px" onclick="sendCommand('${d.id}', 'set_interval', document.getElementById('interval_${d.id}').value)">Set Interval</button>
                            </div>
                        </div>
                    </div>
                `).join('');
                
                // Restore saved input values after refresh
                Object.keys(savedValues).forEach(id => {
                    const input = document.getElementById(id);
                    if (input) {
                        input.value = savedValues[id];
                    }
                });
            })
            .catch(e => {
                document.getElementById('sensors').innerHTML = 
                    '<div class="loading"><p>Error loading sensors: ' + e.message + '</p></div>';
            });
        }
        
        // Load sensors and gateway status on page load
        loadSensors();
        updateGatewayStatus();
        
        // Auto-refresh sensors every 5 seconds
        setInterval(loadSensors, 5000);
        
        // Update gateway status every 2 seconds
        setInterval(updateGatewayStatus, 2000);
    </script>
</body>
</html>
)rawliteral";

/**
 * Initialize web server
 */
void initWebServer() {
    Serial.println("Initializing web dashboard...");
    
    // Main dashboard page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });
    
    // API: Get all devices (thread-safe snapshot)
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = getDeviceRegistrySnapshot();
        request->send(200, "application/json", json);
    });
    
    // API: Get gateway status
    server.on("/api/gateway", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["ip"] = WiFi.localIP().toString();
        doc["wifi_rssi"] = WiFi.RSSI();
        doc["free_heap"] = ESP.getFreeHeap();
        doc["uptime"] = millis();
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });
    
    // API: Send command to sensor
    server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
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
            
            // Map action to command
            bool success = false;
            if (strcmp(action, "set_interval") == 0) {
                uint16_t value = doc["value"] | 90;
                success = queueCommand(deviceId, CMD_SET_INTERVAL, (uint8_t*)&value, sizeof(value));
            }
            else if (strcmp(action, "set_sleep") == 0) {
                uint16_t value = doc["value"] | 90;
                success = queueCommand(deviceId, CMD_SET_SLEEP, (uint8_t*)&value, sizeof(value));
            }
            else if (strcmp(action, "calibrate") == 0) {
                success = queueCommand(deviceId, CMD_CALIBRATE, nullptr, 0);
            }
            else if (strcmp(action, "clear_baseline") == 0) {
                success = queueCommand(deviceId, CMD_CLEAR_BASELINE, nullptr, 0);
            }
            else if (strcmp(action, "restart") == 0) {
                success = queueCommand(deviceId, CMD_RESTART, nullptr, 0);
            }
            else if (strcmp(action, "status") == 0) {
                success = queueCommand(deviceId, CMD_STATUS, nullptr, 0);
            }
            else {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown action\"}");
                return;
            }
            
            if (success) {
                request->send(200, "application/json", "{\"success\":true}");
                Serial.printf("[WEB] Command queued: %s for device 0x%016llX\n", action, deviceId);
            } else {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to queue command\"}");
            }
        }
    );
    
    // Start server
    server.begin();
    Serial.println("✅ Web dashboard started on port 80");
}
