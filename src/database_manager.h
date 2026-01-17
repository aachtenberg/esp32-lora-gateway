#ifndef DATABASE_MANAGER_H
#define DATABASE_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <queue>
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

class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();
    
    // Initialize and connect to database API
    void init();
    
    // Main loop - call from main loop
    void loop();
    
    // Write operations (async)
    bool writeDevice(uint64_t deviceId, const String& name, const String& location,
                    int16_t rssi, int16_t snr, uint32_t packetCount, 
                    uint16_t lastSequence, uint16_t sensorInterval, uint16_t deepSleep);
    
    bool writePacket(uint64_t deviceId, const String& gatewayId, uint8_t msgType,
                    uint16_t sequenceNum, int16_t rssi, int16_t snr, 
                    const JsonDocument& payload);
    
    bool writeCommand(uint64_t deviceId, uint8_t commandType, const String& params,
                     const String& status);
    
    bool writeEvent(uint64_t deviceId, uint8_t eventType, uint8_t severity,
                   const String& message);
    
    // Status
    DatabaseStatus getStatus() const { return status; }
    size_t getQueueDepth() const { return writeQueue.size(); }
    uint32_t getFailedWrites() const { return failedWrites; }
    
private:
    HTTPClient http;
    DatabaseStatus status;
    std::queue<PendingWrite> writeQueue;
    uint32_t lastReconnectAttempt;
    uint32_t failedWrites;
    uint32_t reconnectAttempts;
    String apiBaseUrl;
    
    static const size_t MAX_QUEUE_SIZE = 1000;
    static const uint32_t RECONNECT_INTERVAL = 30000;  // 30 seconds
    
    void attemptConnection();
    void processWriteQueue();
    void checkConnectionHealth();
    bool postJson(const String& endpoint, const JsonDocument& doc);
    void queueWrite(const String& endpoint, const JsonDocument& doc);
};

extern DatabaseManager dbManager;

#endif // DATABASE_MANAGER_H
