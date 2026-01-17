#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include <Arduino.h>

// Device information
struct DeviceInfo {
    uint64_t deviceId;        // LoRa device ID (chip ID)
    String deviceName;        // Descriptive device name
    String location;          // Physical location of device
    String sensorType;        // Sensor type: "BME280", "DS18B20", or "Unknown"
    uint32_t lastSeen;        // Timestamp of last packet
    int16_t lastRssi;         // Last RSSI value
    int8_t lastSnr;           // Last SNR value
    uint32_t packetCount;     // Total packets received
    uint16_t lastSequence;    // Last sequence number
    uint16_t sequenceBuffer[50];  // Deduplication buffer
    uint8_t bufferIndex;      // Circular buffer index
    uint16_t sensorInterval;  // Sensor reading interval (seconds)
    uint16_t deepSleepSec;    // Deep sleep duration (seconds)
};

// Thread-safe access functions
// NOTE: For web server, use getDeviceRegistrySnapshot() to avoid holding mutex too long

// Initialize device registry
void initDeviceRegistry();

// Get device name from LoRa ID
String getDeviceName(uint64_t deviceId);

// Update device info on packet reception
void updateDeviceInfo(uint64_t deviceId, uint16_t seqNum, int16_t rssi, int8_t snr);

// Check if packet is duplicate
bool isDuplicate(uint64_t deviceId, uint16_t seqNum);

// Clear deduplication buffer for a device (call on device restart)
void clearDuplicationBuffer(uint64_t deviceId);

// Add new device to registry
void addDevice(uint64_t deviceId, const String& name, const String& location = "Unknown");

// Get device location from LoRa ID
String getDeviceLocation(uint64_t deviceId);

// Update device name in registry
void updateDeviceName(uint64_t deviceId, const String& name);

// Update device location in registry
void updateDeviceLocation(uint64_t deviceId, const String& location);

// Update device config (sensor interval and deep sleep)
void updateDeviceConfig(uint64_t deviceId, uint16_t sensorInterval, uint16_t deepSleep);

// Update device sensor type (BME280, DS18B20, etc.)
void updateDeviceSensorType(uint64_t deviceId, const char* sensorType);

// Get device sensor type
String getDeviceSensorType(uint64_t deviceId);

// Get device info by ID
DeviceInfo* getDeviceInfo(uint64_t deviceId);

// Get total device count
int getDeviceCount();

// Save registry to SPIFFS
bool saveRegistry();

// Load registry from SPIFFS
bool loadRegistry();

// Get thread-safe snapshot of all devices (for web server)
String getDeviceRegistrySnapshot();

#endif // DEVICE_REGISTRY_H
