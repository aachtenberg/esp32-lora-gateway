#include "device_registry.h"
#include "device_config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// Device registry storage
static DeviceInfo devices[MAX_SENSORS];
static int deviceCount = 0;

// Registry file path
#define REGISTRY_FILE "/sensor_registry.json"

/**
 * Initialize device registry
 * Loads existing registry from filesystem
 */
void initDeviceRegistry() {
    Serial.println("\n=== Device Registry Initialization ===");
    
    // Initialize LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println("⚠️  LittleFS mount failed, starting with empty registry");
        return;
    }
    
    // Try to load existing registry
    if (loadRegistry()) {
        Serial.printf("✅ Loaded %d devices from registry\n", deviceCount);
    } else {
        Serial.println("⚠️  No existing registry found, starting fresh");
    }
    
    Serial.println("======================================\n");
}

/**
 * Get device name from LoRa ID
 * Returns device name if found, otherwise generates default name
 */
String getDeviceName(uint64_t deviceId) {
    // Search for device in registry
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            return devices[i].deviceName;
        }
    }
    
    // Device not found, generate default name from ID
    String defaultName = "sensor_" + String((uint32_t)(deviceId & 0xFFFFFFFF), HEX);
    
    // Auto-register new device with default location
    addDevice(deviceId, defaultName, "Unknown");
    
    return defaultName;
}

/**
 * Get device location from LoRa ID
 * Returns device location if found, otherwise "Unknown"
 */
String getDeviceLocation(uint64_t deviceId) {
    // Search for device in registry
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            return devices[i].location;
        }
    }
    
    return "Unknown";
}

/**
 * Update device name in registry
 * Called when sensor reports its local name
 */
void updateDeviceName(uint64_t deviceId, const String& name) {
    // Find device
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            // Only update if name is different and not empty
            if (name.length() > 0 && !devices[i].deviceName.equals(name)) {
                Serial.printf("📝 Updating device name: '%s' -> '%s'\n", 
                             devices[i].deviceName.c_str(), name.c_str());
                devices[i].deviceName = name;
                saveRegistry();  // Persist changes
            }
            return;
        }
    }
    
    // Device not found - this shouldn't happen since updateDeviceInfo is called first
    Serial.printf("⚠️  Device 0x%016llX not in registry for name update\n", deviceId);
}

/**
 * Update device location in registry
 * Called when sensor reports its location (manual or GPS)
 */
void updateDeviceLocation(uint64_t deviceId, const String& location) {
    // Find device
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            // Only update if location is different and not empty
            if (location.length() > 0 && !devices[i].location.equals(location)) {
                Serial.printf("📍 Updating device location: '%s' -> '%s'\n", 
                             devices[i].location.c_str(), location.c_str());
                devices[i].location = location;
                saveRegistry();  // Persist changes
            }
            return;
        }
    }
    
    // Device not found
    Serial.printf("⚠️  Device 0x%016llX not in registry for location update\n", deviceId);
}

/**
 * Update device info on packet reception
 */
void updateDeviceInfo(uint64_t deviceId, uint16_t seqNum, int16_t rssi, int8_t snr) {
    // Find device
    DeviceInfo* device = nullptr;
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            device = &devices[i];
            break;
        }
    }
    
    // Device not found, auto-register
    if (device == nullptr) {
        String name = "sensor_" + String((uint32_t)(deviceId & 0xFFFFFFFF), HEX);
        addDevice(deviceId, name, "Unknown");
        device = &devices[deviceCount - 1];
    }
    
    // Update info
    device->lastSeen = millis();
    device->lastRssi = rssi;
    device->lastSnr = snr;
    device->packetCount++;
    device->lastSequence = seqNum;
    
    // Add to deduplication buffer
    device->sequenceBuffer[device->bufferIndex] = seqNum;
    device->bufferIndex = (device->bufferIndex + 1) % DEDUP_BUFFER_SIZE;
}

/**
 * Check if packet is duplicate
 * Uses circular buffer to track recent sequence numbers
 */
bool isDuplicate(uint64_t deviceId, uint16_t seqNum) {
    // Find device
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            // Check if sequence number is in buffer
            for (int j = 0; j < DEDUP_BUFFER_SIZE; j++) {
                if (devices[i].sequenceBuffer[j] == seqNum) {
                    return true;  // Duplicate found
                }
            }
            return false;  // Not a duplicate
        }
    }
    
    // Device not in registry, can't be duplicate
    return false;
}

/**
 * Clear deduplication buffer for a device
 * Call this when a device restarts to reset sequence tracking
 */
void clearDuplicationBuffer(uint64_t deviceId) {
    // Find device
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            // Clear the buffer by setting all entries to 0xFFFF (invalid)
            for (int j = 0; j < DEDUP_BUFFER_SIZE; j++) {
                devices[i].sequenceBuffer[j] = 0xFFFF;
            }
            devices[i].bufferIndex = 0;
            Serial.printf("🔄 Cleared deduplication buffer for device 0x%016llX\n", deviceId);
            return;
        }
    }
}

/**
 * Add new device to registry
 */
void addDevice(uint64_t deviceId, const String& name, const String& location) {
    if (deviceCount >= MAX_SENSORS) {
        Serial.println("⚠️  Registry full, cannot add device!");
        return;
    }
    
    // Initialize new device
    devices[deviceCount].deviceId = deviceId;
    devices[deviceCount].deviceName = name;
    devices[deviceCount].location = location;
    devices[deviceCount].lastSeen = millis();
    devices[deviceCount].lastRssi = 0;
    devices[deviceCount].lastSnr = 0;
    devices[deviceCount].packetCount = 0;
    devices[deviceCount].lastSequence = 0;
    devices[deviceCount].bufferIndex = 0;
    
    // Clear deduplication buffer
    memset(devices[deviceCount].sequenceBuffer, 0, sizeof(devices[deviceCount].sequenceBuffer));
    
    deviceCount++;
    
    Serial.printf("[Registry] Added device: %s (0x%016llX)\n", name.c_str(), deviceId);
    
    // Save updated registry
    saveRegistry();
}

/**
 * Get device info by ID
 */
DeviceInfo* getDeviceInfo(uint64_t deviceId) {
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            return &devices[i];
        }
    }
    return nullptr;
}

/**
 * Get total device count
 */
int getDeviceCount() {
    return deviceCount;
}

/**
 * Save registry to LittleFS as JSON
 */
bool saveRegistry() {
    Serial.println("[Registry] Saving to filesystem...");
    
    // Create JSON document
    JsonDocument doc;
    JsonArray devicesArray = doc["devices"].to<JsonArray>();
    
    for (int i = 0; i < deviceCount; i++) {
        JsonObject deviceObj = devicesArray.add<JsonObject>();
        
        // Convert device ID to string (JSON doesn't handle uint64_t well)
        char idStr[20];
        snprintf(idStr, sizeof(idStr), "%016llX", devices[i].deviceId);
        
        deviceObj["id"] = idStr;
        deviceObj["name"] = devices[i].deviceName;
        deviceObj["location"] = devices[i].location;
        deviceObj["lastSeen"] = devices[i].lastSeen;
        deviceObj["packetCount"] = devices[i].packetCount;
    }
    
    // Open file for writing
    File file = LittleFS.open(REGISTRY_FILE, "w");
    if (!file) {
        Serial.println("❌ Failed to open registry file for writing");
        return false;
    }
    
    // Serialize JSON to file
    if (serializeJson(doc, file) == 0) {
        Serial.println("❌ Failed to write registry JSON");
        file.close();
        return false;
    }
    
    file.close();
    Serial.println("✅ Registry saved");
    return true;
}

/**
 * Load registry from LittleFS JSON file
 */
bool loadRegistry() {
    // Open file for reading
    File file = LittleFS.open(REGISTRY_FILE, "r");
    if (!file) {
        Serial.println("⚠️  Registry file not found");
        return false;
    }
    
    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("❌ Failed to parse registry JSON: %s\n", error.c_str());
        return false;
    }
    
    // Load devices
    JsonArray devicesArray = doc["devices"];
    deviceCount = 0;
    
    for (JsonObject deviceObj : devicesArray) {
        if (deviceCount >= MAX_SENSORS) {
            Serial.println("⚠️  Registry full, skipping remaining devices");
            break;
        }
        
        // Parse device ID from hex string
        const char* idStr = deviceObj["id"];
        if (idStr == nullptr) {
            Serial.println("⚠️  Device ID is null, skipping");
            continue;
        }
        devices[deviceCount].deviceId = strtoull(idStr, nullptr, 16);
        
        devices[deviceCount].deviceName = deviceObj["name"] | "Unknown";
        devices[deviceCount].location = deviceObj["location"] | "Unknown";
        devices[deviceCount].lastSeen = deviceObj["lastSeen"] | 0;
        devices[deviceCount].packetCount = deviceObj["packetCount"] | 0;
        devices[deviceCount].lastRssi = 0;
        devices[deviceCount].lastSnr = 0;
        devices[deviceCount].lastSequence = 0;
        devices[deviceCount].bufferIndex = 0;
        
        // Clear deduplication buffer
        memset(devices[deviceCount].sequenceBuffer, 0, sizeof(devices[deviceCount].sequenceBuffer));
        
        deviceCount++;
    }
    
    return true;
}
