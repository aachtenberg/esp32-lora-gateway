#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>

// Lightweight debug status pushed from LoRa RX task (no I2C in these setters)
void displayUpdateLoRaStats(uint32_t ok, uint32_t dropped, uint32_t duplicates);
void displayUpdateLoRaLastPacket(uint16_t deviceShort, uint16_t seq, uint8_t msgType, uint8_t payloadLen,
								 int16_t rssi, int8_t snr, const uint8_t headerBytes[4]);
void displayUpdateLoRaLastError(int16_t err);

// Initialize OLED display
bool initDisplay();

// Display startup screen
void displayStartup(const char* version);

// Display main status screen
void displayStatus(uint32_t packets, int deviceCount);

// Display packet received (updates display with latest packet info)
void displayPacketReceived(uint64_t deviceId, float temp, float humidity, int16_t rssi, int8_t snr);

// Display error message
void displayError(const char* error);

#endif // DISPLAY_MANAGER_H
