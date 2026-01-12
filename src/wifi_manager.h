#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

// Initialize WiFi (uses WiFiManager for configuration)
bool initWiFi();

// Check WiFi connection status
bool isWiFiConnected();

// Reconnect to WiFi if disconnected
bool reconnectWiFi();

// Get configured device name
String getDeviceName();

#endif // WIFI_MANAGER_H
