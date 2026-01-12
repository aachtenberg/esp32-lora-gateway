#ifndef LORA_RECEIVER_H
#define LORA_RECEIVER_H

#include <Arduino.h>
#include "lora_protocol.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <RadioLib.h>

// Received packet structure (header + payload + metadata)
struct ReceivedPacket {
    LoRaPacketHeader header;
    uint8_t payload[LORA_MAX_PAYLOAD_SIZE];
    int16_t rssi;
    int8_t snr;
    uint32_t timestamp;  // Millis when received
};

// Initialize LoRa receiver
bool initLoRaReceiver();

// LoRa RX task (runs on Core 0)
void loraRxTask(void* parameter);

// Send ACK to sensor
bool sendAck(uint64_t deviceId, uint16_t seqNum, bool success, int8_t rssi, int8_t snr);

// Get radio instance for transmission
SX1262* getRadio();

// Get gateway ID
uint64_t getGatewayId();

// Check if radio is initialized
bool isRadioInitialized();

// Get packet queue handle (for MQTT task)
QueueHandle_t getPacketQueue();

#endif // LORA_RECEIVER_H
