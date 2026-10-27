#ifndef DATABASE_MANAGER_H
#define DATABASE_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <deque>
#include <vector>

enum DatabaseStatus {
    DB_CONNECTED,
    DB_DISCONNECTED,
    DB_RECONNECTING
};

struct PendingWrite {
    String endpoint;
    JsonDocument doc;
    uint32_t timestamp;
};

/**
 * Persists registry, command and event records through the Go API service.
 *
 * Threading: write*() may be called from any task (LoRa RX, MQTT, async_tcp web
 * server). They only append to a mutex-guarded queue and return immediately.
 * All HTTP traffic happens in loop() on the main task, which also owns the
 * single HTTPClient. Nothing else may block on the network.
 */
class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();

    // Initialize and connect to database API
    void init();

    // Main loop - call from main loop
    void loop();

    // Write operations (queued, non-blocking; return true if queued)
    bool writeDevice(uint64_t deviceId, const String& name, const String& location,
                    const String& sensorType, int16_t rssi, int16_t snr, uint32_t packetCount,
                    uint16_t lastSequence, uint16_t sensorInterval, uint16_t deepSleep);

    bool writePacket(uint64_t deviceId, const String& gatewayId, uint8_t msgType,
                    uint16_t sequenceNum, int16_t rssi, int16_t snr,
                    const JsonDocument& payload);

    // Insert a command row (status "queued" or "failed"); params must be JSON text
    bool writeCommand(uint64_t deviceId, uint8_t commandType, const String& params,
                     const String& status);

    // Move a command row for deviceId+commandType to a new status: "sent",
    // "confirmed", "timeout", "superseded" or "failed". The API picks the newest
    // row in the expected prior status (sent<-queued, confirmed<-sent, others
    // <-queued); fromStatus overrides that, e.g. "sent" to supersede an
    // unconfirmed sent command. retryCount < 0 leaves the stored count unchanged;
    // an empty errorMessage is not sent.
    bool writeCommandStatus(uint64_t deviceId, uint8_t commandType, const char* status,
                            int retryCount = -1, const String& errorMessage = String(),
                            const char* fromStatus = nullptr);

    bool writeEvent(uint64_t deviceId, uint8_t eventType, uint8_t severity,
                   const String& message);

    // Status
    DatabaseStatus getStatus() const { return status; }
    size_t getQueueDepth();
    uint32_t getFailedWrites() const { return failedWrites; }

private:
    HTTPClient http;                 // used only from loop()
    volatile DatabaseStatus status;
    std::deque<PendingWrite> writeQueue;  // front = oldest; only loop() pops
    SemaphoreHandle_t queueMutex;
    uint32_t lastReconnectAttempt;
    uint32_t failedWrites;
    uint32_t reconnectAttempts;
    String apiBaseUrl;

    static const size_t MAX_QUEUE_SIZE = 1000;
    static const uint32_t RECONNECT_INTERVAL = 30000;  // 30 seconds
    static const uint32_t HTTP_TIMEOUT_MS = 3000;      // per request
    static const uint32_t DRAIN_BUDGET_MS = 4000;      // max wall time per loop() pass

    void attemptConnection();
    void processWriteQueue();
    void checkConnectionHealth();
    // Outcome of one HTTP post
    enum PostResult { POST_OK, POST_REJECTED, POST_SERVER_ERROR, POST_UNREACHABLE };
    PostResult postJson(const String& endpoint, const JsonDocument& doc);
    bool queueWrite(const String& endpoint, const JsonDocument& doc);
    uint8_t headFailures;            // consecutive server errors for the queue head
    static const uint8_t MAX_HEAD_FAILURES = 3;
};

extern DatabaseManager dbManager;

#endif // DATABASE_MANAGER_H
