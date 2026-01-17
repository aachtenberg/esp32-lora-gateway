#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include "device_config.h"
#include "lora_config.h"
#include "version.h"
#include "secrets.h"
#include "lora_receiver.h"
#include "mqtt_bridge.h"
#include "wifi_manager.h"
#include "device_registry.h"
#include "display_manager.h"
#include "command_sender.h"
#include "command_tester.h"
#include "web_server.h"
#include "database_manager.h"

// Watchdog timeout (seconds)
#define WDT_TIMEOUT 30

// FreeRTOS task handles
TaskHandle_t loraRxTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;

void setup() {
    Serial.begin(115200);
    delay(100);

    Serial.println("\n\n====================================");
    Serial.println("ESP32 LoRa Gateway - Startup");
    Serial.println("====================================");
    Serial.printf("Firmware: %s\n", getFirmwareVersion().c_str());
    Serial.printf("Build: %s %s\n", BUILD_DATE, BUILD_TIME);
    
    // Configure Task Watchdog Timer
    Serial.printf("Configuring watchdog timer (%d seconds)... ", WDT_TIMEOUT);
    esp_task_wdt_init(WDT_TIMEOUT, true);  // 30 second timeout, panic on timeout
    esp_task_wdt_add(NULL);  // Add current task (setup/loop)
    Serial.println("✅");

#ifdef OLED_ENABLED
    // Initialize OLED display
    Serial.println("Initializing OLED display...");
    initDisplay();
    displayStartup(getFirmwareVersion().c_str());
#endif

    // Initialize WiFi
    Serial.println("\nConnecting to WiFi...");
    if (!initWiFi()) {
        Serial.println("ERROR: WiFi initialization failed");
        displayError("WiFi Failed!");
        delay(5000);
        ESP.restart();
    }

    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Initialize OTA updates
    Serial.println("Initializing OTA updates...");
    ArduinoOTA.setHostname("esp32-lora-gateway");
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        Serial.println("OTA update starting...");
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA update complete!");
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA Error[%u]: ", error);
    });

    ArduinoOTA.begin();

    // Initialize device registry
    Serial.println("Initializing device registry...");
    initDeviceRegistry();

    // Initialize database manager
    Serial.println("Initializing database manager...");
    dbManager.init();

    // Initialize command sender with retry mechanism
    Serial.println("Initializing command sender...");
    initCommandSender();

    // Initialize MQTT bridge
    Serial.println("Initializing MQTT bridge...");
    if (!initMqttBridge()) {
        Serial.println("WARNING: MQTT initialization failed");
        // Continue anyway - will retry in loop
    }

    // Initialize web dashboard
    Serial.println("Initializing web dashboard...");
    initWebServer();

    // Initialize LoRa receiver
    Serial.println("Initializing LoRa receiver...");
    if (!initLoRaReceiver()) {
        Serial.println("ERROR: LoRa initialization failed!");
        displayError("LoRa Failed!");
        delay(5000);
        ESP.restart();
    }

    // Create FreeRTOS tasks on separate cores
    Serial.println("\nStarting dual-core tasks...");

    // Core 0: LoRa RX (high priority, minimal latency)
    xTaskCreatePinnedToCore(
        loraRxTask,           // Task function
        "LoRaRX",             // Task name
        8192,                 // Stack size
        NULL,                 // Parameters
        2,                    // Priority (higher than MQTT)
        &loraRxTaskHandle,    // Task handle
        0                     // Core 0
    );

    // Core 1: MQTT + WiFi + OTA + Display
    xTaskCreatePinnedToCore(
        mqttTask,             // Task function
        "MQTT",               // Task name
        8192,                 // Stack size
        NULL,                 // Parameters
        1,                    // Priority
        &mqttTaskHandle,      // Task handle
        1                     // Core 1
    );

    Serial.println("Gateway startup complete!");
    Serial.println("====================================\n");

    // Note: Don't call displayStatus() here - let the loop handle display updates
    // to avoid watchdog timeout during long I2C operations in setup()
}

void loop() {
    // Main loop runs on Core 1
    // Feed watchdog
    esp_task_wdt_reset();
    
    // Handle serial commands for testing
    handleSerialCommands();

    // Handle OTA updates
    ArduinoOTA.handle();

    // Handle database operations
    dbManager.loop();

    // Check WiFi connection
    static uint32_t lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck > 30000) {  // Check every 30 seconds
        lastWiFiCheck = millis();
        if (!isWiFiConnected()) {
            Serial.println("WiFi disconnected, reconnecting...");
            reconnectWiFi();
        }
    }

#ifdef OLED_ENABLED
    // Update display periodically
    static uint32_t lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > 1000) {  // Update every second
        lastDisplayUpdate = millis();
        // packets argument is ignored in favor of LoRa debug counters when available
        displayStatus(0, getDeviceCount());
    }
#endif

    // Yield to other tasks
    vTaskDelay(pdMS_TO_TICKS(10));
}
