#include "wifi_manager.h"
#include "secrets.h"
#include "device_config.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <LittleFS.h>

// WiFi connection timeout
#define WIFI_CONNECT_TIMEOUT_MS 10000

// Device name configuration file
#define DEVICE_NAME_FILE "/device_name.txt"
#define DEFAULT_DEVICE_NAME "lora-gateway"

// WiFiManager instance
static WiFiManager wifiManager;

// Device name storage
static String deviceName = DEFAULT_DEVICE_NAME;

/**
 * Load device name from LittleFS
 */
static void loadDeviceName() {
    if (!LittleFS.begin(true)) {
        Serial.println("⚠️  LittleFS mount failed, using default device name");
        return;
    }
    
    if (LittleFS.exists(DEVICE_NAME_FILE)) {
        File file = LittleFS.open(DEVICE_NAME_FILE, "r");
        if (file) {
            deviceName = file.readStringUntil('\n');
            deviceName.trim();
            file.close();
            Serial.printf("Device name loaded: %s\n", deviceName.c_str());
        }
    } else {
        Serial.printf("Using default device name: %s\n", deviceName.c_str());
    }
}

/**
 * Save device name to LittleFS
 */
static void saveDeviceName(const String& name) {
    if (!LittleFS.begin(true)) {
        Serial.println("⚠️  LittleFS mount failed, cannot save device name");
        return;
    }
    
    File file = LittleFS.open(DEVICE_NAME_FILE, "w");
    if (file) {
        file.println(name);
        file.close();
        Serial.printf("Device name saved: %s\n", name.c_str());
    }
}

/**
 * Initialize WiFi using WiFiManager
 * Falls back to AP mode for configuration if no credentials stored
 */
bool initWiFi() {
    Serial.println("\n=== WiFi Initialization ===");
    
    // Load device name from storage
    loadDeviceName();
    
    // Set WiFi mode
    WiFi.mode(WIFI_STA);
    
    // Disable WiFi power save for low latency
    #ifdef WIFI_PS_MODE
    WiFi.setSleep(false);
    #endif
    
    // Custom parameter for device name
    char deviceNameBuffer[40];
    strncpy(deviceNameBuffer, deviceName.c_str(), sizeof(deviceNameBuffer) - 1);
    WiFiManagerParameter customDeviceName("device_name", "Device Name", deviceNameBuffer, 40);
    
    // Configure WiFiManager
    wifiManager.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SEC);
    wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
    wifiManager.addParameter(&customDeviceName);
    
    // Save config callback
    wifiManager.setSaveConfigCallback([]() {
        Serial.println("Configuration saved via portal");
    });
    
    // Set custom AP name for configuration portal
    String apName = "LoRa-Gateway-" + String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
    
    Serial.printf("WiFi AP Name: %s\n", apName.c_str());
    
    // Try to connect with stored credentials or start config portal
    Serial.println("Connecting to WiFi...");
    
    // If credentials are hardcoded in secrets.h (not empty), try connecting directly first
    if (strlen(WIFI_SSID) > 0) {
        Serial.println("Trying hardcoded WiFi credentials...");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        
        uint32_t startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS) {
            delay(100);
            Serial.print(".");
        }
        Serial.println();
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("✅ Connected with hardcoded credentials!");
            Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
            return true;
        }
        Serial.println("Hardcoded credentials failed, trying WiFiManager...");
    }
    
    // Fall back to WiFiManager
    Serial.println("Trying WiFiManager autoConnect...");
    if (wifiManager.autoConnect(apName.c_str())) {
        Serial.println("✅ WiFi connected!");
        Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
        
        // Save device name if it was changed
        String newDeviceName = customDeviceName.getValue();
        if (newDeviceName.length() > 0 && newDeviceName != deviceName) {
            deviceName = newDeviceName;
            saveDeviceName(deviceName);
        }
        
        return true;
    }
    
    Serial.println("❌ WiFi connection failed!");
    return false;
}

/**
 * Get configured device name
 */
String getDeviceName() {
    return deviceName;
}

/**
 * Check WiFi connection status
 */
bool isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

/**
 * Reconnect to WiFi if disconnected
 */
bool reconnectWiFi() {
    if (isWiFiConnected()) {
        return true;
    }
    
    Serial.println("\n[WiFi] Reconnecting...");
    
    // Try to reconnect using stored credentials
    WiFi.reconnect();
    
    uint32_t startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS) {
        delay(100);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("✅ WiFi reconnected!");
        Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    
    Serial.println("❌ WiFi reconnection failed");
    return false;
}
