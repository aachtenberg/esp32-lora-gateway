#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <Arduino.h>
#include "lora_receiver.h"

// Initialize packet queue (FreeRTOS queue for Core 0 → Core 1)
void initPacketQueue();

// Push packet to queue (called by Core 0)
bool pushPacket(const ReceivedPacket* packet);

// Pop packet from queue (called by Core 1)
bool popPacket(ReceivedPacket* packet, uint32_t timeout_ms);

// Get current queue size
size_t getQueueSize();

#endif // PACKET_QUEUE_H
