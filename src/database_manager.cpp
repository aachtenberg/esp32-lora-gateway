#include "database_manager.h"

DatabaseManager dbManager;

// REST API base URL - will be set from environment or use direct PostgreSQL REST wrapper
// ✅ Enabled by default - API service running on 192.168.0.167:3000
#define DB_API_ENABLED true
#ifndef DB_API_URL
#define DB_API_URL "http://192.168.0.167:3000/api"
#endif

DatabaseManager::DatabaseManager() 
    : status(DB_DISCONNECTED)
    , lastReconnectAttempt(0)
    , failedWrites(0)
    , reconnectAttempts(0)
    , apiBaseUrl(DB_API_URL) {
}

DatabaseManager::~DatabaseManager() {
    http.end();
}

void DatabaseManager::init() {
#if DB_API_ENABLED
    Serial.println("[DB] Initializing database manager (REST API mode)");
    Serial.printf("[DB] API URL: %s\n", apiBaseUrl.c_str());
    attemptConnection();
#else
    Serial.println("[DB] Database manager disabled (no API configured)");
    status = DB_DISCONNECTED;
#endif
}

void DatabaseManager::loop() {
#if DB_API_ENABLED
    if (status == DB_DISCONNECTED) {
        if (millis() - lastReconnectAttempt > RECONNECT_INTERVAL) {
            attemptConnection();
        }
    } else if (status == DB_CONNECTED) {
        processWriteQueue();
        checkConnectionHealth();
    }
#endif
}

void DatabaseManager::attemptConnection() {
    lastReconnectAttempt = millis();
    reconnectAttempts++;
    
    Serial.printf("[DB] Connection attempt %d...\n", reconnectAttempts);
    
    // Simple health check to API
    http.begin(apiBaseUrl + "/health");
    http.setTimeout(5000);
    int httpCode = http.GET();
    http.end();
    
    if (httpCode == 200) {
        Serial.println("✅ Database API connected");
        status = DB_CONNECTED;
        reconnectAttempts = 0;
        
        // Process any queued writes
        Serial.printf("[DB] Processing %d queued writes\n", writeQueue.size());
    } else {
        Serial.printf("⚠️  Database API unavailable (HTTP %d), continuing without persistence\n", httpCode);
        status = DB_DISCONNECTED;
    }
}

void DatabaseManager::processWriteQueue() {
    if (writeQueue.empty() || status != DB_CONNECTED) {
        return;
    }
    
    // Process up to 10 writes per loop iteration to avoid blocking
    int processed = 0;
    while (!writeQueue.empty() && processed < 10 && status == DB_CONNECTED) {
        PendingWrite& write = writeQueue.front();
        
        if (postJson(write.endpoint, write.doc)) {
            writeQueue.pop();
            processed++;
        } else {
            // Connection likely failed
            Serial.println("⚠️  Database write failed, marking disconnected");
            status = DB_DISCONNECTED;
            break;
        }
    }
    
    if (processed > 0) {
        Serial.printf("[DB] Processed %d queued writes, %d remaining\n", 
                     processed, writeQueue.size());
    }
}

void DatabaseManager::checkConnectionHealth() {
    // Simple health check every minute
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 60000) {
        lastCheck = millis();
        
        http.begin(apiBaseUrl + "/health");
        http.setTimeout(2000);
        int httpCode = http.GET();
        http.end();
        
        if (httpCode != 200) {
            Serial.println("⚠️  Database connection lost");
            status = DB_DISCONNECTED;
        }
    }
}

bool DatabaseManager::postJson(const String& endpoint, const JsonDocument& doc) {
    if (status != DB_CONNECTED) {
        return false;
    }
    
    String url = apiBaseUrl + endpoint;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    int httpCode = http.POST(jsonStr);
    http.end();
    
    if (httpCode >= 200 && httpCode < 300) {
        return true;
    } else {
        Serial.printf("⚠️  POST %s failed: HTTP %d\n", endpoint.c_str(), httpCode);
        failedWrites++;
        return false;
    }
}

void DatabaseManager::queueWrite(const String& endpoint, const JsonDocument& doc) {
    if (writeQueue.size() >= MAX_QUEUE_SIZE) {
        // Drop oldest to prevent memory overflow
        Serial.println("⚠️  Write queue full, dropping oldest");
        writeQueue.pop();
        failedWrites++;
    }
    
    PendingWrite write;
    write.endpoint = endpoint;
    write.doc = doc;
    write.timestamp = millis();
    writeQueue.push(write);
}

bool DatabaseManager::writeDevice(uint64_t deviceId, const String& name, const String& location,
                                  const String& sensorType, int16_t rssi, int16_t snr, uint32_t packetCount,
                                  uint16_t lastSequence, uint16_t sensorInterval, uint16_t deepSleep) {
#if !DB_API_ENABLED
    return false;  // Silently skip if disabled
#endif

    JsonDocument doc;
    // Convert uint64_t to string to avoid truncation on 32-bit systems
    char deviceIdStr[32];
    snprintf(deviceIdStr, sizeof(deviceIdStr), "%llu", (unsigned long long)deviceId);
    doc["device_id"] = deviceIdStr;
    doc["name"] = name;
    doc["location"] = location;
    doc["sensor_type"] = sensorType;
    doc["last_rssi"] = rssi;
    doc["last_snr"] = snr;
    doc["packet_count"] = packetCount;
    doc["last_sequence"] = lastSequence;
    doc["sensor_interval"] = sensorInterval;
    doc["deep_sleep_sec"] = deepSleep;
    
    if (status == DB_CONNECTED) {
        return postJson("/devices", doc);
    } else {
        queueWrite("/devices", doc);
        return false;
    }
}

bool DatabaseManager::writePacket(uint64_t deviceId, const String& gatewayId, uint8_t msgType,
                                  uint16_t sequenceNum, int16_t rssi, int16_t snr,
                                  const JsonDocument& payload) {
    // Packets go to MQTT → timeseries DB, not PostgreSQL
    // This database is for device registry only
    return false;
}

bool DatabaseManager::writeCommand(uint64_t deviceId, uint8_t commandType, const String& params,
                                   const String& statusStr) {
#if !DB_API_ENABLED
    return false;
#endif
    
    JsonDocument doc;
    // Convert uint64_t to string to avoid truncation on 32-bit systems
    char deviceIdStr[32];
    snprintf(deviceIdStr, sizeof(deviceIdStr), "%llu", (unsigned long long)deviceId);
    doc["device_id"] = deviceIdStr;
    doc["command_type"] = commandType;
    doc["parameters"] = params;
    doc["status"] = statusStr;
    
    if (status == DB_CONNECTED) {
        return postJson("/commands", doc);
    } else {
        queueWrite("/commands", doc);
        return false;
    }
}

bool DatabaseManager::writeEvent(uint64_t deviceId, uint8_t eventType, uint8_t severity,
                                 const String& message) {
#if !DB_API_ENABLED
    return false;
#endif
    
    JsonDocument doc;
    // Convert uint64_t to string to avoid truncation on 32-bit systems
    char deviceIdStr[32];
    snprintf(deviceIdStr, sizeof(deviceIdStr), "%llu", (unsigned long long)deviceId);
    doc["device_id"] = deviceIdStr;
    doc["event_type"] = eventType;
    doc["severity"] = severity;
    doc["message"] = message;
    
    if (status == DB_CONNECTED) {
        return postJson("/events", doc);
    } else {
        queueWrite("/events", doc);
        return false;
    }
}
