#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include <Arduino.h>

// Device information
struct DeviceInfo {
    uint64_t deviceId;        // LoRa device ID (chip ID)
    String deviceName;        // MQTT-friendly name
    uint32_t lastSeen;        // Timestamp of last packet
    int16_t lastRssi;         // Last RSSI value
    int8_t lastSnr;           // Last SNR value
    uint32_t packetCount;     // Total packets received
    uint16_t lastSequence;    // Last sequence number
    uint16_t sequenceBuffer[50];  // Deduplication buffer
    uint8_t bufferIndex;      // Circular buffer index
};

// Initialize device registry
void initDeviceRegistry();

// Get device name from LoRa ID
String getDeviceName(uint64_t deviceId);

// Update device info on packet reception
void updateDeviceInfo(uint64_t deviceId, uint16_t seqNum, int16_t rssi, int8_t snr);

// Check if packet is duplicate
bool isDuplicate(uint64_t deviceId, uint16_t seqNum);

// Add new device to registry
void addDevice(uint64_t deviceId, const String& name);

// Get device info by ID
DeviceInfo* getDeviceInfo(uint64_t deviceId);

// Get total device count
int getDeviceCount();

// Save registry to SPIFFS
bool saveRegistry();

// Load registry from SPIFFS
bool loadRegistry();

#endif // DEVICE_REGISTRY_H
