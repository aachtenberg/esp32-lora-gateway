#include "mqtt_bridge.h"
#include "device_registry.h"
#include "lora_receiver.h"
#include "device_config.h"
#include "secrets.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#ifdef OLED_ENABLED
#include "display_manager.h"
#endif

// MQTT client
static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

// MQTT topics
#define MQTT_TOPIC_PREFIX "homeassistant/sensor/"
#define MQTT_COMMAND_TOPIC "lora/command"
#define MQTT_STATUS_TOPIC "lora/gateway/status"

// Reconnection tracking
static uint32_t lastMqttReconnectAttempt = 0;

// External function to get packet queue from lora_receiver
extern QueueHandle_t getPacketQueue();

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
    
    // Wait for packet queue to be initialized
    QueueHandle_t packetQueue = NULL;
    while (packetQueue == NULL) {
        packetQueue = getPacketQueue();
        if (packetQueue == NULL) {
            Serial.println("[MQTT Task] Waiting for packet queue...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    
    ReceivedPacket packet;
    
    while (true) {
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
    
    // Get device name
    String deviceName = getDeviceName(packet->header.deviceId);
    
    // Build JSON
    JsonDocument doc;
    doc["device_id"] = String((uint32_t)(packet->header.deviceId & 0xFFFFFFFF), HEX);
    doc["device_name"] = deviceName;
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
    String topic = String(MQTT_TOPIC_PREFIX) + deviceName + "/state";
    
    if (mqttClient.publish(topic.c_str(), jsonString.c_str(), false)) {
        Serial.printf("✅ Published to %s\n", topic.c_str());
        Serial.println(jsonString);
        
        // Update display with received packet info
#ifdef OLED_ENABLED
        displayPacketReceived(packet->header.deviceId, 
                            readings->temperature / 100.0,
                            readings->humidity / 100.0,
                            packet->rssi, 
                            packet->snr);
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
    
    // Get device name
    String deviceName = getDeviceName(packet->header.deviceId);
    
    // Build JSON
    JsonDocument doc;
    doc["device_id"] = String((uint32_t)(packet->header.deviceId & 0xFFFFFFFF), HEX);
    doc["device_name"] = deviceName;
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
    String topic = String(MQTT_TOPIC_PREFIX) + deviceName + "/status";
    
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
    
    // Get device name
    String deviceName = getDeviceName(packet->header.deviceId);
    
    // Extract message (null-terminate)
    char message[238];
    size_t messageLen = min((size_t)event->messageLen, sizeof(message) - 1);
    memcpy(message, event->message, messageLen);
    message[messageLen] = '\0';
    
    // Build JSON
    JsonDocument doc;
    doc["device_id"] = String((uint32_t)(packet->header.deviceId & 0xFFFFFFFF), HEX);
    doc["device_name"] = deviceName;
    doc["event_type"] = event->eventType;
    doc["severity"] = event->severity;  // 0=info, 1=warning, 2=error, 3=critical
    doc["message"] = message;
    doc["timestamp"] = packet->timestamp;
    
    // Serialize
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Publish
    String topic = String(MQTT_TOPIC_PREFIX) + deviceName + "/event";
    
    if (mqttClient.publish(topic.c_str(), jsonString.c_str(), false)) {
        Serial.printf("✅ Published event: %s\n", message);
    } else {
        Serial.printf("❌ Failed to publish event\n");
    }
}

/**
 * MQTT callback for incoming commands
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
    uint8_t cmdType = doc["command"];
    
    if (targetDeviceStr == nullptr) {
        Serial.println("❌ Missing device_id in command");
        return;
    }
    
    // Parse device ID
    uint64_t targetDevice = strtoull(targetDeviceStr, nullptr, 16);
    
    Serial.printf("Command: 0x%02X for device: 0x%016llX\n", cmdType, targetDevice);
    
    // Build command payload
    CommandPayload cmd;
    cmd.cmdType = cmdType;
    cmd.paramLen = 0;
    
    // Add parameters based on command type
    if (cmdType == CMD_SET_BASELINE && doc.containsKey("baseline")) {
        uint32_t baseline = doc["baseline"];
        memcpy(cmd.params, &baseline, sizeof(baseline));
        cmd.paramLen = sizeof(baseline);
    } else if (cmdType == CMD_SET_SLEEP && doc.containsKey("sleep_seconds")) {
        uint16_t sleepSec = doc["sleep_seconds"];
        memcpy(cmd.params, &sleepSec, sizeof(sleepSec));
        cmd.paramLen = sizeof(sleepSec);
    }
    
    // Send command via LoRa
    extern bool sendCommand(uint64_t deviceId, const CommandPayload* cmd);
    if (sendCommand(targetDevice, &cmd)) {
        Serial.println("✅ Command sent via LoRa");
    } else {
        Serial.println("❌ Failed to send command");
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
