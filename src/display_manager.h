#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>

// Initialize OLED display
void initDisplay();

// Display startup screen
void displayStartup(const char* version);

// Display gateway status (WiFi, MQTT, LoRa, sensor count)
void displayGatewayStatus(bool wifiConnected, bool mqttConnected, int sensorCount, uint32_t packetCount);

// Display error message
void displayError(const char* error);

// Update display (call periodically from Core 1)
void updateDisplay();

#endif // DISPLAY_MANAGER_H
