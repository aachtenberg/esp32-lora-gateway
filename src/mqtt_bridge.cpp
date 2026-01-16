#include "mqtt_bridge.h"
#include "device_registry.h"
#include "lora_receiver.h"
#include "device_config.h"
#include "secrets.h"
#include "command_sender.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#ifdef OLED_ENABLED
#include "display_manager.h"
#endif

// MQTT client
static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

// MQTT topics
#define MQTT_TOPIC_PREFIX "esp-sensor-hub/"
#define MQTT_COMMAND_TOPIC "lora/command"
#define MQTT_STATUS_TOPIC "lora/gateway/status"

// Reconnection tracking
static uint32_t lastMqttReconnectAttempt = 0;

// External function to get packet queue from lora_receiver
extern QueueHandle_t getPacketQueue();

/**
 * Format 64-bit device ID as hex string
 */
static String formatDeviceId(uint64_t deviceId) {
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llX", deviceId);
    return String(buffer);
}

/**
 * Initialize MQTT bridge
 */
bool initMqttBridge() {
    Serial.println("\n=== MQTT Bridge Initialization ===");
    
    // Configure MQTT server
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE_SEC);
    
    Serial.printf("MQTT Broker: %s:%d\n", MQTT_BROKER, MQTT_PORT);
    
    // Try initial connection
    if (reconnectMqtt()) {
        Serial.println("✅ MQTT connected");
        return true;
    }
    
    Serial.println("⚠️  MQTT connection failed, will retry in loop");
    return false;
}

/**
 * MQTT task (runs on Core 1)
 * Processes received LoRa packets and publishes to MQTT
 */
void mqttTask(void* parameter) {
    Serial.println("[MQTT Task] Started on Core 1");
    
    // Subscribe this task to the watchdog
    esp_task_wdt_add(NULL);
    
    // Wait for packet queue to be initialized
    QueueHandle_t packetQueue = NULL;
    while (packetQueue == NULL) {
        packetQueue = getPacketQueue();
        if (packetQueue == NULL) {
            Serial.println("[MQTT Task] Waiting for packet queue...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();  // Feed watchdog while waiting
        }
    }
    
    ReceivedPacket packet;
    
    while (true) {
        // Feed watchdog at start of loop
        esp_task_wdt_reset();
        
        // Maintain MQTT connection
        if (!mqttClient.connected()) {
            uint32_t now = millis();
            if (now - lastMqttReconnectAttempt > MQTT_RECONNECT_INTERVAL_MS) {
                lastMqttReconnectAttempt = now;
                if (reconnectMqtt()) {
                    lastMqttReconnectAttempt = 0;
                }
            }
        } else {
            // Process MQTT loop (handles callbacks, keepalive, etc.)
            mqttClient.loop();
        }
        
        // Check for packets from LoRa RX task
        if (xQueueReceive(packetQueue, &packet, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.printf("\n[MQTT] Processing packet from device 0x%016llX\n", packet.header.deviceId);
            
            // Retry any queued commands for this sensor (it's now in RX window)
            retryCommandsForSensor(packet.header.deviceId);
            
            // Route packet based on message type
            switch (packet.header.msgType) {
                case MSG_READINGS:
                    publishReadings(&packet);
                    break;
                    
                case MSG_STATUS:
                    publishStatus(&packet);
                    break;
                    
                case MSG_EVENT:
                    publishEvent(&packet);
                    break;
                    
                default:
                    Serial.printf("⚠️  Unknown message type: 0x%02X\n", packet.header.msgType);
                    break;
            }
        }
        
        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * Publish sensor readings to MQTT
 * Converts binary ReadingsPayload to JSON
 */
void publishReadings(const ReceivedPacket* packet) {
    if (packet->header.payloadLen != sizeof(ReadingsPayload)) {
        Serial.println("⚠️  Invalid readings payload size");
        return;
    }
    
    // Parse payload
    ReadingsPayload* readings = (ReadingsPayload*)packet->payload;
    
    // Get device name and ID
    String deviceName = getDeviceName(packet->header.deviceId);
    String deviceLocation = getDeviceLocation(packet->header.deviceId);
    String deviceId = formatDeviceId(packet->header.deviceId);
    
    // Build JSON
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["device_name"] = deviceName;
    doc["location"] = deviceLocation;
    doc["timestamp"] = readings->timestamp;
    doc["sequence"] = packet->header.sequenceNum;
    
    // Sensor data
    doc["temperature"] = readings->temperature / 100.0;
    doc["humidity"] = readings->humidity / 100.0;
    doc["pressure"] = readings->pressure / 100.0;
    doc["altitude"] = readings->altitude;
    
    // Battery data
    doc["battery_voltage"] = readings->batteryVoltage / 1000.0;
    doc["battery_percent"] = readings->batteryPercent;
    
    // Pressure trend
    doc["pressure_change"] = readings->pressureChange;
    doc["pressure_trend"] = readings->pressureTrend;  // 0=falling, 1=steady, 2=rising
    
    // LoRa metadata
    doc["rssi"] = packet->rssi;
    doc["snr"] = packet->snr;
    doc["gateway_time"] = packet->timestamp;
    
    // Serialize to string
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Publish to MQTT
    String topic = String(MQTT_TOPIC_PREFIX) + deviceId + "/readings";
    
    if (mqttClient.publish(topic.c_str(), jsonString.c_str(), false)) {
        Serial.printf("✅ Published to %s\n", topic.c_str());
        Serial.println(jsonString);
        
        // Update display with sensor readings (shown in main status screen)
#ifdef OLED_ENABLED
        displayUpdateSensorData(readings->temperature / 100.0,
                               readings->humidity / 100.0,
                               readings->pressure / 100.0,
                               readings->pressureTrend,
                               readings->pressureChange / 100.0);
#endif
    } else {
        Serial.printf("❌ Failed to publish to %s\n", topic.c_str());
    }
}

/**
 * Publish device status to MQTT
 */
void publishStatus(const ReceivedPacket* packet) {
    if (packet->header.payloadLen != sizeof(StatusPayload)) {
        Serial.println("⚠️  Invalid status payload size");
        return;
    }
    
    // Parse payload
    StatusPayload* status = (StatusPayload*)packet->payload;
    
    // Extract device name from payload and update registry if present
    if (status->deviceName[0] != '\0') {
        String sensorName = String(status->deviceName);
        sensorName.trim();
        if (sensorName.length() > 0) {
            updateDeviceName(packet->header.deviceId, sensorName);
        }
    }
    
    // Extract location from payload and update registry if present (future: GPS)
    if (status->location[0] != '\0') {
        String sensorLocation = String(status->location);
        sensorLocation.trim();
        if (sensorLocation.length() > 0) {
            updateDeviceLocation(packet->header.deviceId, sensorLocation);
        }
    }
    
    // Get device name and ID (may have just been updated)
    String deviceName = getDeviceName(packet->header.deviceId);
    String deviceLocation = getDeviceLocation(packet->header.deviceId);
    String deviceId = formatDeviceId(packet->header.deviceId);
    
    // Build JSON
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["device_name"] = deviceName;
    doc["location"] = deviceLocation;
    doc["uptime"] = status->uptime;
    doc["wake_count"] = status->wakeCount;
    doc["sensor_healthy"] = (bool)status->sensorHealthy;
    doc["lora_rssi"] = status->loraRssi;
    doc["lora_snr"] = status->loraSNR;
    doc["free_heap_kb"] = status->freeHeap;
    doc["sensor_failures"] = status->sensorFailures;
    doc["tx_failures"] = status->txFailures;
    doc["last_success_tx"] = status->lastSuccessTx;
    doc["deep_sleep_sec"] = status->deepSleepSec;
    
    // LoRa metadata
    doc["rssi"] = packet->rssi;
    doc["snr"] = packet->snr;
    
    // Serialize
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Publish
    String topic = String(MQTT_TOPIC_PREFIX) + deviceId + "/status";
    
    if (mqttClient.publish(topic.c_str(), jsonString.c_str(), false)) {
        Serial.printf("✅ Published status to %s\n", topic.c_str());
    } else {
        Serial.printf("❌ Failed to publish status\n");
    }
}

/**
 * Publish system event to MQTT
 */
void publishEvent(const ReceivedPacket* packet) {
    if (packet->header.payloadLen < sizeof(uint8_t) * 3) {
        Serial.println("⚠️  Invalid event payload size");
        return;
    }
    
    // Parse payload
    EventPayload* event = (EventPayload*)packet->payload;
    
    // Clear deduplication buffer on device restart
    if (event->eventType == 0x01) {  // EVENT_STARTUP
        clearDuplicationBuffer(packet->header.deviceId);
    }
    
    // Get device name and ID
    String deviceName = getDeviceName(packet->header.deviceId);
    String deviceLocation = getDeviceLocation(packet->header.deviceId);
    String deviceId = formatDeviceId(packet->header.deviceId);
    
    // Extract message (null-terminate)
    char message[238];
    size_t messageLen = min((size_t)event->messageLen, sizeof(message) - 1);
    memcpy(message, event->message, messageLen);
    message[messageLen] = '\0';
    
    // Build JSON
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["device_name"] = deviceName;
    doc["location"] = deviceLocation;
    doc["event_type"] = event->eventType;
    doc["severity"] = event->severity;  // 0=info, 1=warning, 2=error, 3=critical
    doc["message"] = message;
    doc["timestamp"] = packet->timestamp;
    
    // Serialize
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Publish
    String topic = String(MQTT_TOPIC_PREFIX) + deviceId + "/events";
    
    if (mqttClient.publish(topic.c_str(), jsonString.c_str(), false)) {
        Serial.printf("✅ Published event: %s\n", message);
    } else {
        Serial.printf("❌ Failed to publish event\n");
    }
}

/**
 * MQTT callback for incoming commands
 * Expected JSON format:
 * {
 *   "device_id": "f09e9e76aec4",
 *   "action": "set_interval",  // or "set_sleep", "restart", "status"
 *   "value": 90                // optional, for set_interval/set_sleep
 * }
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("\n[MQTT] Message received on topic: %s\n", topic);
    
    // Parse JSON payload
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) {
        Serial.printf("❌ JSON parse error: %s\n", error.c_str());
        return;
    }
    
    // Extract command details
    const char* targetDeviceStr = doc["device_id"];
    const char* action = doc["action"];
    
    if (targetDeviceStr == nullptr || action == nullptr) {
        Serial.println("❌ Missing device_id or action in command");
        return;
    }
    
    // Parse device ID (hex string to uint64_t)
    uint64_t targetDevice = strtoull(targetDeviceStr, nullptr, 16);
    
    Serial.printf("[MQTT CMD] Action: %s for device: 0x%016llX\n", action, targetDevice);
    
    bool success = false;
    
    // Route to appropriate command sender
    if (strcmp(action, "set_interval") == 0) {
        uint32_t seconds = doc["value"] | 30;  // Default 30 seconds
        Serial.printf("  Setting sensor interval to %lu seconds\n", seconds);
        
        // Pack seconds as ASCII decimal string
        char paramStr[16];
        snprintf(paramStr, sizeof(paramStr), "%lu", seconds);
        success = queueCommand(targetDevice, 0x07, (uint8_t*)paramStr, strlen(paramStr));  // CMD_SET_INTERVAL
        
    } else if (strcmp(action, "set_sleep") == 0) {
        uint32_t seconds = doc["value"] | 900;  // Default 15 minutes
        Serial.printf("  Setting deep sleep to %lu seconds\n", seconds);
        
        // Pack seconds as ASCII decimal string
        char paramStr[16];
        snprintf(paramStr, sizeof(paramStr), "%lu", seconds);
        success = queueCommand(targetDevice, 0x06, (uint8_t*)paramStr, strlen(paramStr));  // CMD_SET_SLEEP
        
    } else if (strcmp(action, "restart") == 0) {
        Serial.println("  Sending restart command");
        success = queueCommand(targetDevice, 0x04, nullptr, 0);  // CMD_RESTART
        
    } else if (strcmp(action, "status") == 0) {
        Serial.println("  Requesting status update");
        success = queueCommand(targetDevice, 0x05, nullptr, 0);  // CMD_STATUS

    } else if (strcmp(action, "calibrate") == 0) {
        Serial.println("  Calibrating pressure baseline (current reading)");
        success = queueCommand(targetDevice, 0x01, nullptr, 0);  // CMD_CALIBRATE

    } else if (strcmp(action, "set_baseline") == 0) {
        float baselineHpa = doc["value"] | 1013.25;  // Default sea level pressure
        Serial.printf("  Setting pressure baseline to %.2f hPa\n", baselineHpa);
        
        // Pack baseline as ASCII decimal string
        char paramStr[16];
        snprintf(paramStr, sizeof(paramStr), "%.2f", baselineHpa);
        success = queueCommand(targetDevice, 0x02, (uint8_t*)paramStr, strlen(paramStr));  // CMD_SET_BASELINE

    } else if (strcmp(action, "clear_baseline") == 0) {
        Serial.println("  Clearing pressure baseline");
        success = queueCommand(targetDevice, 0x03, nullptr, 0);  // CMD_CLEAR_BASELINE

    } else {
        Serial.printf("❌ Unknown action: %s\n", action);
        return;
    }
    
    // Publish result
    if (success) {
        Serial.println("✅ Command queued for retry on sensor activity");
        // Optionally publish ACK back to MQTT
        char ackTopic[64];
        snprintf(ackTopic, sizeof(ackTopic), "lora/command/ack");
        char ackPayload[128];
        snprintf(ackPayload, sizeof(ackPayload), 
                 "{\"device_id\":\"%s\",\"action\":\"%s\",\"status\":\"queued\"}", 
                 targetDeviceStr, action);
        mqttClient.publish(ackTopic, ackPayload);
    } else {
        Serial.println("❌ Command queueing failed");
    }
}

/**
 * Reconnect to MQTT broker
 */
bool reconnectMqtt() {
    Serial.print("[MQTT] Connecting to broker... ");
    
    // Generate unique client ID
    String clientId = "LoRa-Gateway-" + String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
    
    // Attempt connection
    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
        Serial.println("✅ Connected!");
        
        // Subscribe to command topic
        mqttClient.subscribe(MQTT_COMMAND_TOPIC);
        Serial.printf("Subscribed to: %s\n", MQTT_COMMAND_TOPIC);
        
        // Publish gateway online status
        JsonDocument doc;
        doc["status"] = "online";
        doc["gateway_id"] = String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
        doc["ip"] = WiFi.localIP().toString();
        
        String jsonString;
        serializeJson(doc, jsonString);
        mqttClient.publish(MQTT_STATUS_TOPIC, jsonString.c_str(), true);  // Retained
        
        return true;
    }
    
    Serial.printf("❌ Failed (rc=%d)\n", mqttClient.state());
    return false;
}
