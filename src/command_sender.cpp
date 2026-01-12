/**
 * Command Sender - LoRa Gateway
 * Sends remote configuration commands to sensors
 */

#include "command_sender.h"
#include "lora_config.h"
#include "lora_protocol.h"
#include <RadioLib.h>

// Forward declarations - radio initialized in lora_receiver.cpp
extern SX1262* getRadio();
extern uint64_t getGatewayId();
extern bool isRadioInitialized();

/**
 * Helper: Create LoRa packet header
 */
static void initCommandHeader(LoRaPacketHeader* header, uint64_t deviceId, 
                               uint16_t seqNum, uint8_t payloadLen) {
    header->magic[0] = LORA_MAGIC_BYTE_1;
    header->magic[1] = LORA_MAGIC_BYTE_2;
    header->version = LORA_PROTOCOL_VERSION;
    header->msgType = MSG_COMMAND;
    header->deviceId = deviceId;
    header->sequenceNum = seqNum;
    header->payloadLen = payloadLen;
    
    // Calculate XOR checksum of header bytes
    uint8_t* headerBytes = (uint8_t*)header;
    header->checksum = 0;
    for (int i = 0; i < sizeof(LoRaPacketHeader) - 1; i++) {
        header->checksum ^= headerBytes[i];
    }
}

/**
 * Send a command to a remote LoRa sensor
 */
bool sendCommand(uint64_t sensorId, uint8_t cmdType, const uint8_t* params, uint8_t paramLen) {
    if (!isRadioInitialized()) {
        Serial.println("❌ [COMMAND] LoRa radio not initialized!");
        return false;
    }
    
    if (paramLen > 238) {
        Serial.printf("❌ [COMMAND] Parameters too large: %d bytes (max 238)\n", paramLen);
        return false;
    }
    
    SX1262* radio = getRadio();
    uint64_t gatewayId = getGatewayId();
    
    if (!radio) {
        Serial.println("❌ [COMMAND] Radio pointer is null!");
        return false;
    }
    
    // Build command payload
    CommandPayload cmd;
    cmd.cmdType = cmdType;
    cmd.paramLen = paramLen;
    if (paramLen > 0 && params) {
        memcpy(cmd.params, params, paramLen);
    }
    
    // Build complete packet
    uint8_t packet[sizeof(LoRaPacketHeader) + sizeof(CommandPayload)];
    LoRaPacketHeader* header = (LoRaPacketHeader*)packet;
    
    static uint16_t commandSeqNum = 0;
    initCommandHeader(header, sensorId, commandSeqNum++, sizeof(CommandPayload));
    
    memcpy(packet + sizeof(LoRaPacketHeader), &cmd, sizeof(CommandPayload));
    
    // Display command info
    Serial.printf("\n[COMMAND TX] Sending to sensor: 0x%016llX\n", sensorId);
    Serial.printf("  Type: 0x%02X, Params: %d bytes, Seq: %d\n", 
                  cmdType, paramLen, commandSeqNum - 1);
    
    // Ensure radio is in standby before transmit
    radio->standby();
    delay(10);
    
    // Transmit command packet
    Serial.print("  Transmitting... ");
    int state = radio->transmit((uint8_t*)packet, sizeof(packet));
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("✅ Success!");
        Serial.printf("  RSSI: %d dBm, SNR: %d dB\n", radio->getRSSI(), radio->getSNR());
        return true;
    } else {
        Serial.printf("❌ Failed! (code: %d)\n", state);
        return false;
    }
}

/**
 * Send CMD_SET_SLEEP command to sensor
 */
bool sendSetSleepCommand(uint64_t sensorId, uint32_t sleepSeconds) {
    if (sleepSeconds > 3600) {
        Serial.printf("❌ Invalid sleep interval: %lu (max 3600 seconds)\n", sleepSeconds);
        return false;
    }
    
    Serial.printf("\n📡 Sending SET_SLEEP command: %lu seconds\n", sleepSeconds);
    
    // Pack seconds as ASCII decimal string for readability in logs
    char paramStr[16];
    snprintf(paramStr, sizeof(paramStr), "%lu", sleepSeconds);
    uint8_t paramLen = strlen(paramStr);
    
    return sendCommand(sensorId, CMD_SET_SLEEP, (uint8_t*)paramStr, paramLen);
}

/**
 * Send CMD_SET_INTERVAL command to sensor
 */
bool sendSetIntervalCommand(uint64_t sensorId, uint32_t intervalSeconds) {
    if (intervalSeconds < 5 || intervalSeconds > 3600) {
        Serial.printf("❌ Invalid interval: %lu (valid range: 5-3600 seconds)\n", intervalSeconds);
        return false;
    }
    
    Serial.printf("\n📡 Sending SET_INTERVAL command: %lu seconds\n", intervalSeconds);
    
    // Pack seconds as ASCII decimal string
    char paramStr[16];
    snprintf(paramStr, sizeof(paramStr), "%lu", intervalSeconds);
    uint8_t paramLen = strlen(paramStr);
    
    return sendCommand(sensorId, CMD_SET_INTERVAL, (uint8_t*)paramStr, paramLen);
}

/**
 * Send CMD_RESTART command to sensor
 */
bool sendRestartCommand(uint64_t sensorId) {
    Serial.println("\n📡 Sending RESTART command");
    return sendCommand(sensorId, CMD_RESTART, nullptr, 0);
}

/**
 * Send CMD_STATUS command to sensor
 */
bool sendStatusCommand(uint64_t sensorId) {
    Serial.println("\n📡 Sending STATUS command");
    return sendCommand(sensorId, CMD_STATUS, nullptr, 0);
}
