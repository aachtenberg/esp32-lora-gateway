#ifndef LORA_PROTOCOL_H
#define LORA_PROTOCOL_H

#include <stdint.h>

// ====================================================================
// LoRa Peer-to-Peer Communication Protocol
// Shared between esp32-lora-sensor and esp32-lora-gateway
// ====================================================================

// Protocol version
#define LORA_PROTOCOL_VERSION 0x01

// Magic bytes for packet validation
#define LORA_MAGIC_BYTE_1 0xAA
#define LORA_MAGIC_BYTE_2 0x55

// Maximum payload size (LoRa max ~256, reserve 16 for header)
#define LORA_MAX_PAYLOAD_SIZE 240

// ====================================================================
// Message Types
// ====================================================================

enum LoRaMsgType {
    MSG_READINGS     = 0x01,  // Sensor readings (BME280 data)
    MSG_STATUS       = 0x02,  // Device status/health
    MSG_EVENT        = 0x03,  // System events (startup, errors)
    MSG_COMMAND      = 0x10,  // Command from gateway to sensor
    MSG_ACK          = 0x20,  // Acknowledgment
    MSG_BEACON       = 0x30   // Gateway beacon (discovery)
};

// ====================================================================
// Packet Header (16 bytes, fixed size)
// ====================================================================

struct LoRaPacketHeader {
    uint8_t  magic[2];        // 0xAA 0x55 (packet validation)
    uint8_t  version;         // Protocol version (0x01)
    uint8_t  msgType;         // Message type (see enum above)
    uint64_t deviceId;        // Unique device ID (ESP32 chip ID)
    uint16_t sequenceNum;     // Packet sequence for deduplication
    uint8_t  payloadLen;      // Payload length (0-240 bytes)
    uint8_t  checksum;        // XOR checksum of header bytes
} __attribute__((packed));

// ====================================================================
// Payload Structures
// ====================================================================

// Readings payload (37 bytes) - BME280 sensor data
struct ReadingsPayload {
    uint32_t timestamp;       // Unix timestamp (seconds) or uptime
    int16_t  temperature;     // Temperature * 100 (e.g., 2531 = 25.31°C)
    uint16_t humidity;        // Humidity * 100 (e.g., 5520 = 55.20%)
    uint32_t pressure;        // Pressure in Pa
    int16_t  altitude;        // Altitude in meters
    uint16_t batteryVoltage;  // Voltage * 1000 (e.g., 3700 = 3.7V)
    uint8_t  batteryPercent;  // 0-100%
    int32_t  pressureChange;  // Change from baseline (Pa)
    uint8_t  pressureTrend;   // 0=falling, 1=steady, 2=rising
} __attribute__((packed));

// Status payload (88 bytes) - Device health metrics
struct StatusPayload {
    uint32_t uptime;          // Uptime in seconds
    uint16_t wakeCount;       // Deep sleep wake counter
    uint8_t  sensorHealthy;   // Boolean: sensor operational
    int8_t   loraRssi;        // Last LoRa RSSI (dBm)
    int8_t   loraSNR;         // Last LoRa SNR (dB)
    uint16_t freeHeap;        // Free heap in KB
    uint16_t sensorFailures;  // Sensor read failure counter
    uint16_t txFailures;      // LoRa TX failure counter
    uint32_t lastSuccessTx;   // Timestamp of last successful TX
    uint16_t deepSleepSec;    // Current deep sleep interval
    char     deviceName[32];  // Device name (null-terminated)
    char     location[32];    // Location (manual or GPS, null-terminated)
} __attribute__((packed));

// Command types (matching current MQTT commands)
enum CommandType {
    CMD_CALIBRATE       = 0x01,  // Set pressure baseline to current
    CMD_SET_BASELINE    = 0x02,  // Set specific baseline value
    CMD_CLEAR_BASELINE  = 0x03,  // Clear baseline tracking
    CMD_RESTART         = 0x04,  // Restart device
    CMD_STATUS          = 0x05,  // Request immediate status update
    CMD_SET_SLEEP       = 0x06,  // Set deep sleep interval
    CMD_SET_INTERVAL    = 0x07,  // Set sensor read interval
    CMD_OTA_START       = 0x08,  // Start OTA update (future)
    CMD_TIME_SYNC       = 0x09,  // Time synchronization
};

// Event types
enum EventType {
    EVENT_STARTUP       = 0x01,  // Device startup
    EVENT_SHUTDOWN      = 0x02,  // Device shutdown
    EVENT_SENSOR_ERROR  = 0x10,  // Sensor read error
    EVENT_LORA_ERROR    = 0x11,  // LoRa communication error
    EVENT_CONFIG_CHANGE = 0x20,  // Configuration changed
    EVENT_OTA_START     = 0x30,  // OTA update started
    EVENT_OTA_COMPLETE  = 0x31,  // OTA update completed
    EVENT_OTA_FAILED    = 0x32,  // OTA update failed
};

// Event severity levels
enum EventSeverity {
    SEVERITY_INFO       = 0,
    SEVERITY_WARNING    = 1,
    SEVERITY_ERROR      = 2,
    SEVERITY_CRITICAL   = 3
};

// Command payload (variable length, max 240 bytes)
struct CommandPayload {
    uint8_t  cmdType;         // Command type (see enum above)
    uint8_t  paramLen;        // Parameter length
    uint8_t  params[238];     // Parameters (binary or small JSON)
} __attribute__((packed));

// Event payload (variable length)
struct EventPayload {
    uint8_t  eventType;       // Event type code
    uint8_t  severity;        // 0=info, 1=warning, 2=error, 3=critical
    uint8_t  messageLen;      // Message length
    char     message[237];    // Event message string
} __attribute__((packed));

// ACK payload (8 bytes)
struct AckPayload {
    uint16_t ackSequenceNum;  // Sequence number being acknowledged
    uint8_t  success;         // 1=success, 0=failure
    uint8_t  errorCode;       // Error code if success=0
    int8_t   rssi;            // Gateway's received RSSI
    int8_t   snr;             // Gateway's received SNR
    uint16_t reserved;        // Reserved for future use
} __attribute__((packed));

// ====================================================================
// Helper Functions
// ====================================================================

// Calculate XOR checksum of header (excluding checksum field itself)
inline uint8_t calculateHeaderChecksum(const LoRaPacketHeader* header) {
    const uint8_t* bytes = (const uint8_t*)header;
    uint8_t checksum = 0;
    // XOR all bytes except the last one (checksum field)
    for (size_t i = 0; i < sizeof(LoRaPacketHeader) - 1; i++) {
        checksum ^= bytes[i];
    }
    return checksum;
}

// Validate packet header
inline bool validateHeader(const LoRaPacketHeader* header) {
    // Check magic bytes
    if (header->magic[0] != LORA_MAGIC_BYTE_1 ||
        header->magic[1] != LORA_MAGIC_BYTE_2) {
        return false;
    }

    // Check protocol version
    if (header->version != LORA_PROTOCOL_VERSION) {
        return false;
    }

    // Check payload length
    if (header->payloadLen > LORA_MAX_PAYLOAD_SIZE) {
        return false;
    }

    // Verify checksum
    uint8_t expectedChecksum = calculateHeaderChecksum(header);
    if (header->checksum != expectedChecksum) {
        return false;
    }

    return true;
}

// Initialize header with common fields
inline void initHeader(LoRaPacketHeader* header, uint8_t msgType,
                      uint64_t deviceId, uint16_t seqNum, uint8_t payloadLen) {
    header->magic[0] = LORA_MAGIC_BYTE_1;
    header->magic[1] = LORA_MAGIC_BYTE_2;
    header->version = LORA_PROTOCOL_VERSION;
    header->msgType = msgType;
    header->deviceId = deviceId;
    header->sequenceNum = seqNum;
    header->payloadLen = payloadLen;
    header->checksum = calculateHeaderChecksum(header);
}

#endif // LORA_PROTOCOL_H
