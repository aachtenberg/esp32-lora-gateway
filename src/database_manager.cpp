#include "database_manager.h"
#include <esp_task_wdt.h>

DatabaseManager dbManager;

// REST API base URL - will be set from environment or use direct PostgreSQL REST wrapper
// ✅ Enabled by default - API service running on 192.168.0.167:3000
#define DB_API_ENABLED true
#ifndef DB_API_URL
#define DB_API_URL "http://192.168.0.167:3000/api"
#endif

#define LOCK_WRITES()   do { if (queueMutex) xSemaphoreTake(queueMutex, portMAX_DELAY); } while (0)
#define UNLOCK_WRITES() do { if (queueMutex) xSemaphoreGive(queueMutex); } while (0)

static void formatDeviceIdDecimal(uint64_t deviceId, char* out, size_t len) {
    // Decimal string: avoids truncation on 32-bit and matches the BIGINT column
    snprintf(out, len, "%llu", (unsigned long long)deviceId);
}

DatabaseManager::DatabaseManager()
    : status(DB_DISCONNECTED)
    , queueMutex(NULL)
    , lastReconnectAttempt(0)
    , failedWrites(0)
    , reconnectAttempts(0)
    , apiBaseUrl(DB_API_URL) {
}

DatabaseManager::~DatabaseManager() {
    http.end();
}

void DatabaseManager::init() {
    if (queueMutex == NULL) {
        queueMutex = xSemaphoreCreateMutex();
    }
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

size_t DatabaseManager::getQueueDepth() {
    LOCK_WRITES();
    size_t depth = writeQueue.size();
    UNLOCK_WRITES();
    return depth;
}

void DatabaseManager::attemptConnection() {
    lastReconnectAttempt = millis();
    reconnectAttempts++;

    Serial.printf("[DB] Connection attempt %d...\n", reconnectAttempts);

    // Simple health check to API
    http.begin(apiBaseUrl + "/health");
    http.setTimeout(HTTP_TIMEOUT_MS);
    int httpCode = http.GET();
    http.end();

    if (httpCode == 200) {
        Serial.println("✅ Database API connected");
        status = DB_CONNECTED;
        reconnectAttempts = 0;
        Serial.printf("[DB] Processing %d queued writes\n", getQueueDepth());
    } else {
        Serial.printf("⚠️  Database API unavailable (HTTP %d), continuing without persistence\n", httpCode);
        status = DB_DISCONNECTED;
    }
}

/**
 * Drain queued writes in FIFO order. Only this task pops, so the front entry
 * can be copied out, posted with the lock released, and popped afterwards.
 * Bounded by DRAIN_BUDGET_MS per pass and feeds the watchdog between posts so
 * a slow API cannot starve the main loop.
 */
void DatabaseManager::processWriteQueue() {
    uint32_t started = millis();
    int processed = 0;

    while (status == DB_CONNECTED && millis() - started < DRAIN_BUDGET_MS) {
        PendingWrite write;
        LOCK_WRITES();
        bool have = !writeQueue.empty();
        if (have) write = writeQueue.front();
        UNLOCK_WRITES();
        if (!have) break;

        esp_task_wdt_reset();
        if (postJson(write.endpoint, write.doc)) {
            LOCK_WRITES();
            if (!writeQueue.empty()) writeQueue.pop();
            UNLOCK_WRITES();
            processed++;
        } else {
            // Connection likely failed; leave the entry for the next attempt
            Serial.println("⚠️  Database write failed, marking disconnected");
            status = DB_DISCONNECTED;
            break;
        }
    }
    esp_task_wdt_reset();

    if (processed > 0) {
        size_t remaining = getQueueDepth();
        if (remaining > 0) {
            Serial.printf("[DB] Processed %d queued writes, %d remaining\n", processed, remaining);
        }
    }
}

void DatabaseManager::checkConnectionHealth() {
    // Simple health check every minute, only when there is nothing to drain
    // (a successful post is a better health signal than an extra request)
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 60000) {
        lastCheck = millis();
        if (getQueueDepth() > 0) return;

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
    String url = apiBaseUrl + endpoint;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);

    String jsonStr;
    serializeJson(doc, jsonStr);

    int httpCode = http.POST(jsonStr);
    http.end();

    if (httpCode >= 200 && httpCode < 300) {
        return true;
    }

    // 4xx means the API rejected this record; retrying it would never succeed
    // and would block everything behind it. Drop it and keep the connection.
    if (httpCode >= 400 && httpCode < 500) {
        Serial.printf("⚠️  POST %s rejected: HTTP %d (dropping record)\n", endpoint.c_str(), httpCode);
        failedWrites++;
        return true;
    }

    Serial.printf("⚠️  POST %s failed: HTTP %d\n", endpoint.c_str(), httpCode);
    failedWrites++;
    return false;
}

bool DatabaseManager::queueWrite(const String& endpoint, const JsonDocument& doc) {
    PendingWrite write;
    write.endpoint = endpoint;
    write.doc = doc;
    write.timestamp = millis();

    LOCK_WRITES();
    if (writeQueue.size() >= MAX_QUEUE_SIZE) {
        // Drop oldest to prevent memory overflow
        writeQueue.pop();
        failedWrites++;
        Serial.println("⚠️  Write queue full, dropping oldest");
    }
    writeQueue.push(write);
    UNLOCK_WRITES();
    return true;
}

bool DatabaseManager::writeDevice(uint64_t deviceId, const String& name, const String& location,
                                  const String& sensorType, int16_t rssi, int16_t snr, uint32_t packetCount,
                                  uint16_t lastSequence, uint16_t sensorInterval, uint16_t deepSleep) {
#if !DB_API_ENABLED
    return false;  // Silently skip if disabled
#endif

    JsonDocument doc;
    char deviceIdStr[32];
    formatDeviceIdDecimal(deviceId, deviceIdStr, sizeof(deviceIdStr));
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

    return queueWrite("/devices", doc);
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
    char deviceIdStr[32];
    formatDeviceIdDecimal(deviceId, deviceIdStr, sizeof(deviceIdStr));
    doc["device_id"] = deviceIdStr;
    doc["command_type"] = commandType;
    doc["parameters"] = params;
    doc["status"] = statusStr;

    return queueWrite("/commands", doc);
}

bool DatabaseManager::writeCommandStatus(uint64_t deviceId, uint8_t commandType, const char* statusStr,
                                         int retryCount, const String& errorMessage) {
#if !DB_API_ENABLED
    return false;
#endif

    JsonDocument doc;
    char deviceIdStr[32];
    formatDeviceIdDecimal(deviceId, deviceIdStr, sizeof(deviceIdStr));
    doc["device_id"] = deviceIdStr;
    doc["command_type"] = commandType;
    doc["status"] = statusStr;
    if (retryCount >= 0) doc["retry_count"] = retryCount;
    if (errorMessage.length() > 0) doc["error_message"] = errorMessage;

    return queueWrite("/commands/status", doc);
}

bool DatabaseManager::writeEvent(uint64_t deviceId, uint8_t eventType, uint8_t severity,
                                 const String& message) {
#if !DB_API_ENABLED
    return false;
#endif

    JsonDocument doc;
    char deviceIdStr[32];
    formatDeviceIdDecimal(deviceId, deviceIdStr, sizeof(deviceIdStr));
    doc["device_id"] = deviceIdStr;
    doc["event_type"] = eventType;
    doc["severity"] = severity;
    doc["message"] = message;

    return queueWrite("/events", doc);
}
