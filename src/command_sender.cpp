/**
 * Command Sender - LoRa Gateway
 * Sends remote configuration commands to sensors with retry mechanism
 */

#include "command_sender.h"
#include "lora_config.h"
#include "lora_protocol.h"
#include "device_config.h"
#include <RadioLib.h>

// Forward declarations - radio initialized in lora_receiver.cpp
extern SX1262* getRadio();
extern uint64_t getGatewayId();
extern bool isRadioInitialized();
extern SemaphoreHandle_t getRadioMutex();

// ====================================================================
// Command Queue for Persistent Retry
// ====================================================================

struct QueuedCommand {
    uint64_t sensorId;
    uint8_t cmdType;
    uint8_t params[238];
    uint8_t paramLen;
    uint32_t queuedAt;
    uint8_t retryCount;
};

static QueuedCommand commandQueue[MAX_QUEUED_COMMANDS];
static uint8_t queueSize = 0;

/**
 * Initialize command sender
 */
void initCommandSender() {
    queueSize = 0;
    Serial.println("[CMD] Command sender initialized with retry mechanism");
}

/**
 * Add command to persistent queue
 */
bool queueCommand(uint64_t sensorId, uint8_t cmdType, const uint8_t* params, uint8_t paramLen) {
    if (queueSize >= MAX_QUEUED_COMMANDS) {
        Serial.println("❌ [CMD] Command queue full!");
        return false;
    }
    
    // Check if same command already queued for this sensor
    for (int i = 0; i < queueSize; i++) {
        if (commandQueue[i].sensorId == sensorId && commandQueue[i].cmdType == cmdType) {
            Serial.println("⚠️  [CMD] Command already queued, updating timestamp");
            commandQueue[i].queuedAt = millis();
            commandQueue[i].retryCount = 0;
            if (paramLen > 0 && params) {
                memcpy(commandQueue[i].params, params, paramLen);
                commandQueue[i].paramLen = paramLen;
            }
            return true;
        }
    }
    
    // Add new command to queue
    QueuedCommand* cmd = &commandQueue[queueSize];
    cmd->sensorId = sensorId;
    cmd->cmdType = cmdType;
    cmd->paramLen = paramLen;
    if (paramLen > 0 && params) {
        memcpy(cmd->params, params, paramLen);
    }
    cmd->queuedAt = millis();
    cmd->retryCount = 0;
    queueSize++;
    
    Serial.printf("✅ [CMD] Queued command 0x%02X for sensor 0x%016llX (%d in queue)\n", 
                  cmdType, sensorId, queueSize);
    
    // Try sending immediately
    sendCommand(sensorId, cmdType, params, paramLen);
    
    return true;
}

/**
 * Remove expired commands from queue
 */
static void cleanExpiredCommands() {
    uint32_t now = millis();
    
    for (int i = queueSize - 1; i >= 0; i--) {
        if (now - commandQueue[i].queuedAt > COMMAND_EXPIRATION_MS) {
            Serial.printf("⏰ [CMD] Command 0x%02X expired for sensor 0x%016llX\n", 
                          commandQueue[i].cmdType, commandQueue[i].sensorId);
            
            // Remove from queue by shifting remaining items
            for (int j = i; j < queueSize - 1; j++) {
                commandQueue[j] = commandQueue[j + 1];
            }
            queueSize--;
        }
    }
}

/**
 * Retry queued commands for a specific sensor
 * Call this when sensor transmits (opens its RX window)
 */
void retryCommandsForSensor(uint64_t sensorId) {
    cleanExpiredCommands();
    
    bool foundCommands = false;
    for (int i = 0; i < queueSize; i++) {
        if (commandQueue[i].sensorId == sensorId) {
            foundCommands = true;
            commandQueue[i].retryCount++;
            
            Serial.printf("🔄 [CMD] Retrying command 0x%02X for sensor 0x%016llX (attempt %d)\n", 
                          commandQueue[i].cmdType, sensorId, commandQueue[i].retryCount);
            
            bool success = sendCommand(sensorId, commandQueue[i].cmdType, 
                                      commandQueue[i].params, commandQueue[i].paramLen);
            
            if (success) {
                // Remove from queue on successful transmission
                Serial.printf("✅ [CMD] Command sent, removing from queue\n");
                for (int j = i; j < queueSize - 1; j++) {
                    commandQueue[j] = commandQueue[j + 1];
                }
                queueSize--;
                i--;  // Adjust index after removal
            }
            
            // Small delay between retries
            delay(50);
        }
    }
    
    if (foundCommands && queueSize > 0) {
        Serial.printf("📋 [CMD] %d commands remaining in queue\n", queueSize);
    }
}

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
    SemaphoreHandle_t radioMutex = getRadioMutex();
    
    if (!radio) {
        Serial.println("❌ [COMMAND] Radio pointer is null!");
        return false;
    }
    
    if (!radioMutex) {
        Serial.println("❌ [COMMAND] Radio mutex not available!");
        return false;
    }
    
    // Build command payload
    CommandPayload cmd;
    cmd.cmdType = cmdType;
    cmd.paramLen = paramLen;
    if (paramLen > 0 && params) {
        memcpy(cmd.params, params, paramLen);
    }

    // Calculate actual payload size (cmdType + paramLen + actual params, not full buffer)
    uint8_t actualPayloadLen = 2 + paramLen;  // 2 = cmdType + paramLen fields

    // Build complete packet (header + actual payload, not full CommandPayload struct)
    uint8_t packet[sizeof(LoRaPacketHeader) + actualPayloadLen];
    LoRaPacketHeader* header = (LoRaPacketHeader*)packet;

    static uint16_t commandSeqNum = 0;
    initCommandHeader(header, sensorId, commandSeqNum++, actualPayloadLen);

    // Copy only the actual command data (not the full CommandPayload buffer)
    packet[sizeof(LoRaPacketHeader)] = cmd.cmdType;
    packet[sizeof(LoRaPacketHeader) + 1] = cmd.paramLen;
    if (paramLen > 0) {
        memcpy(packet + sizeof(LoRaPacketHeader) + 2, cmd.params, paramLen);
    }
    
    // Display command info
    Serial.printf("\n[COMMAND TX] Sending to sensor: 0x%016llX\n", sensorId);
    Serial.printf("  Type: 0x%02X, Params: %d bytes, Seq: %d\n", 
                  cmdType, paramLen, commandSeqNum - 1);
    
    // Acquire radio mutex (wait up to 5 seconds to allow RX task to finish)
    Serial.print("  Acquiring radio mutex... ");
    if (xSemaphoreTake(radioMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.println("❌ Failed (timeout)!");
        return false;
    }
    Serial.println("✅");

    // Stop RX mode - must explicitly call standby when in continuous RX
    Serial.print("  Stopping RX mode... ");
    int state = radio->standby();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("❌ Failed! (code: %d)\n", state);
        xSemaphoreGive(radioMutex);
        return false;
    }
    Serial.println("✅");

    // Wait for radio to be ready (BUSY pin should be LOW)
    uint32_t timeout = millis() + 1000;
    while (digitalRead(LORA_BUSY) == HIGH && millis() < timeout) {
        delay(1);
    }

    if (digitalRead(LORA_BUSY) == HIGH) {
        Serial.println("❌ Radio still BUSY after timeout!");
        radio->startReceive();
        xSemaphoreGive(radioMutex);
        return false;
    }

    // Transmit command packet
    Serial.print("  Transmitting... ");
    state = radio->transmit((uint8_t*)packet, sizeof(packet));
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("✅ Success!");
        // Small delay before restarting RX
        delay(10);
        // Restart RX mode
        radio->startReceive();
        // Release mutex
        xSemaphoreGive(radioMutex);
        return true;
    } else {
        Serial.printf("❌ Failed! (code: %d)\n", state);
        // Small delay before restarting RX
        delay(10);
        // Restart RX mode even on failure
        radio->startReceive();
        // Release mutex
        xSemaphoreGive(radioMutex);
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

/**
 * Send CMD_CALIBRATE command to sensor (set current pressure as baseline)
 */
bool sendCalibrateCommand(uint64_t sensorId) {
    Serial.println("\n📡 Sending CALIBRATE command (set current pressure as baseline)");
    return sendCommand(sensorId, CMD_CALIBRATE, nullptr, 0);
}

/**
 * Send CMD_SET_BASELINE command to sensor
 */
bool sendSetBaselineCommand(uint64_t sensorId, float baselineHpa) {
    if (baselineHpa < 900.0 || baselineHpa > 1100.0) {
        Serial.printf("❌ Invalid baseline: %.2f (valid range: 900-1100 hPa)\n", baselineHpa);
        return false;
    }

    Serial.printf("\n📡 Sending SET_BASELINE command: %.2f hPa\n", baselineHpa);

    // Pack baseline as ASCII decimal string
    char paramStr[16];
    snprintf(paramStr, sizeof(paramStr), "%.2f", baselineHpa);
    uint8_t paramLen = strlen(paramStr);

    return sendCommand(sensorId, CMD_SET_BASELINE, (uint8_t*)paramStr, paramLen);
}

/**
 * Send CMD_CLEAR_BASELINE command to sensor
 */
bool sendClearBaselineCommand(uint64_t sensorId) {
    Serial.println("\n📡 Sending CLEAR_BASELINE command");
    return sendCommand(sensorId, CMD_CLEAR_BASELINE, nullptr, 0);
}

/**
 * Get number of queued commands for a specific sensor
 */
int getQueuedCommandCount(uint64_t sensorId) {
    int count = 0;
    for (int i = 0; i < queueSize; i++) {
        if (commandQueue[i].sensorId == sensorId) {
            count++;
        }
    }
    return count;
}

/**
 * Get JSON array of queued commands for a specific sensor
 */
String getQueuedCommandsJson(uint64_t sensorId) {
    String result = "[";
    bool first = true;
    
    for (int i = 0; i < queueSize; i++) {
        if (commandQueue[i].sensorId == sensorId) {
            if (!first) result += ",";
            first = false;
            
            // Map command type to readable name
            const char* cmdName = "unknown";
            switch (commandQueue[i].cmdType) {
                case CMD_SET_SLEEP: cmdName = "set_sleep"; break;
                case CMD_SET_INTERVAL: cmdName = "set_interval"; break;
                case CMD_RESTART: cmdName = "restart"; break;
                case CMD_STATUS: cmdName = "status"; break;
                case CMD_CALIBRATE: cmdName = "calibrate"; break;
                case CMD_SET_BASELINE: cmdName = "set_baseline"; break;
                case CMD_CLEAR_BASELINE: cmdName = "clear_baseline"; break;
            }
            
            result += "{\"type\":\"";
            result += cmdName;
            result += "\",\"retries\":";
            result += String(commandQueue[i].retryCount);
            result += "}";
        }
    }
    
    result += "]";
    return result;
}
