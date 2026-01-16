#include "device_registry.h"
#include "device_config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// Device registry storage
static DeviceInfo devices[MAX_SENSORS];
static int deviceCount = 0;

// Mutex for thread-safe access to device registry
static SemaphoreHandle_t registryMutex = NULL;

// Registry file path
#define REGISTRY_FILE "/sensor_registry.json"

// Mutex helper macros
#define LOCK_REGISTRY() if (registryMutex) xSemaphoreTake(registryMutex, portMAX_DELAY)
#define UNLOCK_REGISTRY() if (registryMutex) xSemaphoreGive(registryMutex)

/**
 * Initialize device registry
 * Loads existing registry from filesystem
 */
void initDeviceRegistry() {
    Serial.println("\n=== Device Registry Initialization ===");
    
    // Create mutex for thread-safe access
    registryMutex = xSemaphoreCreateMutex();
    if (registryMutex == NULL) {
        Serial.println("❌ Failed to create registry mutex!");
    }
    
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
    LOCK_REGISTRY();
    
    // Search for device in registry
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            String name = devices[i].deviceName;
            UNLOCK_REGISTRY();
            return name;
        }
    }
    
    UNLOCK_REGISTRY();
    
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
    LOCK_REGISTRY();
    
    // Search for device in registry
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            String location = devices[i].location;
            UNLOCK_REGISTRY();
            return location;
        }
    }
    
    UNLOCK_REGISTRY();
    return "Unknown";
}

/**
 * Update device name in registry
 * Called when sensor reports its local name
 */
void updateDeviceName(uint64_t deviceId, const String& name) {
    LOCK_REGISTRY();
    
    // Find device
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            // Only update if name is different and not empty
            if (name.length() > 0 && !devices[i].deviceName.equals(name)) {
                Serial.printf("📝 Updating device name: '%s' -> '%s'\n", 
                             devices[i].deviceName.c_str(), name.c_str());
                devices[i].deviceName = name;
                UNLOCK_REGISTRY();
                saveRegistry();  // Persist changes
                return;
            }
            UNLOCK_REGISTRY();
            return;
        }
    }
    
    UNLOCK_REGISTRY();
    
    // Device not found - this shouldn't happen since updateDeviceInfo is called first
    Serial.printf("⚠️  Device 0x%016llX not in registry for name update\n", deviceId);
}

/**
 * Update device location in registry
 * Called when sensor reports its location (manual or GPS)
 */
void updateDeviceLocation(uint64_t deviceId, const String& location) {
    LOCK_REGISTRY();
    
    // Find device
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            // Only update if location is different and not empty
            if (location.length() > 0 && !devices[i].location.equals(location)) {
                Serial.printf("📍 Updating device location: '%s' -> '%s'\n", 
                             devices[i].location.c_str(), location.c_str());
                devices[i].location = location;
                UNLOCK_REGISTRY();
                saveRegistry();  // Persist changes
                return;
            }
            UNLOCK_REGISTRY();
            return;
        }
    }
    
    UNLOCK_REGISTRY();
    
    // Device not found
    Serial.printf("⚠️  Device 0x%016llX not in registry for location update\n", deviceId);
}

/**
 * Update device info on packet reception
 */
void updateDeviceInfo(uint64_t deviceId, uint16_t seqNum, int16_t rssi, int8_t snr) {
    LOCK_REGISTRY();
    
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
        UNLOCK_REGISTRY();
        addDevice(deviceId, name, "Unknown");
        LOCK_REGISTRY();
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
    
    UNLOCK_REGISTRY();
}

/**
 * Check if packet is duplicate
 * Uses circular buffer to track recent sequence numbers
 */
bool isDuplicate(uint64_t deviceId, uint16_t seqNum) {
    LOCK_REGISTRY();
    
    // Find device
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            // Check if sequence number is in buffer
            for (int j = 0; j < DEDUP_BUFFER_SIZE; j++) {
                if (devices[i].sequenceBuffer[j] == seqNum) {
                    UNLOCK_REGISTRY();
                    return true;  // Duplicate found
                }
            }
            UNLOCK_REGISTRY();
            return false;  // Not a duplicate
        }
    }
    
    UNLOCK_REGISTRY();
    
    // Device not in registry, can't be duplicate
    return false;
}

/**
 * Clear deduplication buffer for a device
 * Call this when a device restarts to reset sequence tracking
 */
void clearDuplicationBuffer(uint64_t deviceId) {
    LOCK_REGISTRY();
    
    // Find device
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceId == deviceId) {
            // Clear the buffer by setting all entries to 0xFFFF (invalid)
            for (int j = 0; j < DEDUP_BUFFER_SIZE; j++) {
                devices[i].sequenceBuffer[j] = 0xFFFF;
            }
            devices[i].bufferIndex = 0;
            Serial.printf("🔄 Cleared deduplication buffer for device 0x%016llX\n", deviceId);
            UNLOCK_REGISTRY();
            return;
        }
    }
    
    UNLOCK_REGISTRY();
}

/**
 * Add new device to registry
 */
void addDevice(uint64_t deviceId, const String& name, const String& location) {
    LOCK_REGISTRY();
    
    if (deviceCount >= MAX_SENSORS) {
        Serial.println("⚠️  Registry full, cannot add device!");
        UNLOCK_REGISTRY();
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
    
    UNLOCK_REGISTRY();
    
    // Save updated registry
    saveRegistry();
}

/**
 * Get device info by ID
 */
DeviceInfo* getDeviceInfo(uint64_t deviceId) {
    // NOTE: This returns a pointer to internal data
    // Caller must NOT hold this pointer across task switches
    // Use with LOCK_REGISTRY if needed externally
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
    LOCK_REGISTRY();
    int count = deviceCount;
    UNLOCK_REGISTRY();
    return count;
}

/**
 * Save registry to LittleFS as JSON
 */
bool saveRegistry() {
    Serial.println("[Registry] Saving to filesystem...");
    
    LOCK_REGISTRY();
    
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
    
    UNLOCK_REGISTRY();
    
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
    
    LOCK_REGISTRY();
    
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
    
    UNLOCK_REGISTRY();
    
    return true;
}

/**
 * Get a snapshot of all devices for web server
 * Returns JSON array - thread-safe for async web handlers
 */
String getDeviceRegistrySnapshot() {
    LOCK_REGISTRY();
    
    JsonDocument doc;
    JsonArray devicesArray = doc.to<JsonArray>();
    
    for (int i = 0; i < deviceCount; i++) {
        JsonObject deviceObj = devicesArray.add<JsonObject>();
        
        // Convert device ID to string
        char idStr[20];
        snprintf(idStr, sizeof(idStr), "%016llX", devices[i].deviceId);
        
        deviceObj["id"] = idStr;
        deviceObj["name"] = devices[i].deviceName;
        deviceObj["location"] = devices[i].location;
        deviceObj["lastSeen"] = devices[i].lastSeen;
        deviceObj["lastRssi"] = devices[i].lastRssi;
        deviceObj["lastSnr"] = devices[i].lastSnr;
        deviceObj["packetCount"] = devices[i].packetCount;
        deviceObj["lastSequence"] = devices[i].lastSequence;
    }
    
    UNLOCK_REGISTRY();
    
    String result;
    serializeJson(doc, result);
    return result;
}
