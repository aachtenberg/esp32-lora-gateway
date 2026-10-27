#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include <Arduino.h>
#include "lora_protocol.h"
#include "lora_receiver.h"

// Initialize MQTT bridge
bool initMqttBridge();

// MQTT task (runs on Core 1)
void mqttTask(void* parameter);

// Publish readings to MQTT (binary → JSON conversion)
void publishReadings(const ReceivedPacket* packet);

// Publish status to MQTT
void publishStatus(const ReceivedPacket* packet);

// Publish event to MQTT
void publishEvent(const ReceivedPacket* packet);

// MQTT callback for incoming commands
void mqttCallback(char* topic, byte* payload, unsigned int length);

// Reconnect to MQTT broker
bool reconnectMqtt();

// True while the client is connected to the broker (for the dashboard)
bool isMqttConnected();

#endif // MQTT_BRIDGE_H
