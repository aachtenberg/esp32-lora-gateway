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
#include "database_manager.h"

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

// The queue is written by the web server (async_tcp task) and the MQTT task,
// and read by the registry snapshot. All access goes through queueMutex.
// Radio transmission never happens while the lock is held, and never on the
// async_tcp task: queueCommand() only enqueues, and the MQTT task transmits in
// retryCommandsForSensor() once the sensor opens its RX window after a packet.
static QueuedCommand commandQueue[MAX_QUEUED_COMMANDS];
static uint8_t queueSize = 0;
static SemaphoreHandle_t queueMutex = NULL;

#define LOCK_QUEUE()   do { if (queueMutex) xSemaphoreTake(queueMutex, portMAX_DELAY); } while (0)
#define UNLOCK_QUEUE() do { if (queueMutex) xSemaphoreGive(queueMutex); } while (0)

// Commands transmitted and awaiting confirmation by observed effect
// (see confirmCommandsFromStatus/Event). Guarded by queueMutex.
struct SentCommand {
    uint64_t sensorId;
    uint8_t cmdType;
    uint8_t params[40];
    uint8_t paramLen;
    uint32_t sentAt;
};
#define CONFIRM_WINDOW_MS (15 * 60 * 1000)
static SentCommand sentCommands[MAX_QUEUED_COMMANDS];
static uint8_t sentCount = 0;

// Caller holds queueMutex
static void removeSentAt(int index) {
    for (int j = index; j < sentCount - 1; j++) {
        sentCommands[j] = sentCommands[j + 1];
    }
    sentCount--;
}

// Caller holds queueMutex
static void expireSentCommands(uint32_t now) {
    for (int i = sentCount - 1; i >= 0; i--) {
        if (now - sentCommands[i].sentAt > CONFIRM_WINDOW_MS) removeSentAt(i);
    }
}

// Caller holds queueMutex. Replaces an older entry for the same sensor+type;
// returns true if one was displaced (its DB row can no longer be confirmed and
// the caller marks it superseded).
static bool rememberSent(const QueuedCommand& cmd) {
    uint32_t now = millis();
    expireSentCommands(now);
    bool displaced = false;
    for (int i = 0; i < sentCount; i++) {
        if (sentCommands[i].sensorId == cmd.sensorId && sentCommands[i].cmdType == cmd.cmdType) {
            removeSentAt(i);
            displaced = true;
            break;
        }
    }
    if (sentCount >= MAX_QUEUED_COMMANDS) removeSentAt(0);
    SentCommand& s = sentCommands[sentCount++];
    s.sensorId = cmd.sensorId;
    s.cmdType = cmd.cmdType;
    s.paramLen = cmd.paramLen < sizeof(s.params) ? cmd.paramLen : sizeof(s.params);
    memcpy(s.params, cmd.params, s.paramLen);
    s.sentAt = now;
    return displaced;
}

// Caller holds queueMutex. Null-terminated copy of a sent command's params.
static void sentParamsAsString(const SentCommand& s, char* out, size_t len) {
    size_t n = s.paramLen < len - 1 ? s.paramLen : len - 1;
    memcpy(out, s.params, n);
    out[n] = '\0';
}

/**
 * Initialize command sender
 */
void initCommandSender() {
    if (queueMutex == NULL) {
        queueMutex = xSemaphoreCreateMutex();
    }
    LOCK_QUEUE();
    queueSize = 0;
    UNLOCK_QUEUE();
    Serial.println("[CMD] Command sender initialized with retry mechanism");
}

/**
 * Remove a queue entry by index (caller holds queueMutex)
 */
static void removeQueuedAt(int index) {
    for (int j = index; j < queueSize - 1; j++) {
        commandQueue[j] = commandQueue[j + 1];
    }
    queueSize--;
}

/**
 * Add command to persistent queue.
 * Does not transmit: callers may be on the async_tcp task, and a sleeping
 * sensor only listens right after its own transmission anyway. The MQTT task
 * sends it from retryCommandsForSensor() on the sensor's next packet.
 */
bool queueCommand(uint64_t sensorId, uint8_t cmdType, const uint8_t* params, uint8_t paramLen) {
    if (paramLen > sizeof(commandQueue[0].params)) {
        Serial.println("❌ [CMD] Command parameters too long");
        return false;
    }

    LOCK_QUEUE();

    // Check if same command already queued for this sensor
    for (int i = 0; i < queueSize; i++) {
        if (commandQueue[i].sensorId == sensorId && commandQueue[i].cmdType == cmdType) {
            Serial.println("⚠️  [CMD] Command already queued, replacing it");
            // The earlier row in the command history is now superseded by the
            // row the caller is about to insert for this request.
            dbManager.writeCommandStatus(sensorId, cmdType, "superseded", commandQueue[i].retryCount);
            commandQueue[i].queuedAt = millis();
            commandQueue[i].retryCount = 0;
            commandQueue[i].paramLen = paramLen;
            if (paramLen > 0 && params) {
                memcpy(commandQueue[i].params, params, paramLen);
            }
            UNLOCK_QUEUE();
            return true;
        }
    }

    if (queueSize >= MAX_QUEUED_COMMANDS) {
        UNLOCK_QUEUE();
        Serial.println("❌ [CMD] Command queue full!");
        return false;
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
    uint8_t depth = queueSize;

    UNLOCK_QUEUE();

    Serial.printf("✅ [CMD] Queued command 0x%02X for sensor 0x%016llX (%d in queue)\n",
                  cmdType, sensorId, depth);
    return true;
}

/**
 * Remove expired commands from queue (caller holds queueMutex)
 */
static void cleanExpiredCommands() {
    uint32_t now = millis();
    
    for (int i = queueSize - 1; i >= 0; i--) {
        if (now - commandQueue[i].queuedAt > COMMAND_EXPIRATION_MS) {
            Serial.printf("⏰ [CMD] Command 0x%02X expired for sensor 0x%016llX\n", 
                          commandQueue[i].cmdType, commandQueue[i].sensorId);
            dbManager.writeCommandStatus(commandQueue[i].sensorId, commandQueue[i].cmdType, "timeout",
                                         commandQueue[i].retryCount, "no RX window within 30 minutes");
            removeQueuedAt(i);
        }
    }
}

/**
 * Retry queued commands for a specific sensor
 * Call this (from the MQTT task) when the sensor transmits and opens its RX window.
 * The queue lock is released while the radio is busy.
 */
void retryCommandsForSensor(uint64_t sensorId) {
    // Snapshot this sensor's commands under the lock
    QueuedCommand pending[MAX_QUEUED_COMMANDS];
    int pendingCount = 0;

    LOCK_QUEUE();
    cleanExpiredCommands();
    for (int i = 0; i < queueSize; i++) {
        if (commandQueue[i].sensorId == sensorId) {
            commandQueue[i].retryCount++;
            pending[pendingCount++] = commandQueue[i];
        }
    }
    UNLOCK_QUEUE();

    if (pendingCount == 0) return;

    for (int p = 0; p < pendingCount; p++) {
        Serial.printf("🔄 [CMD] Retrying command 0x%02X for sensor 0x%016llX (attempt %d)\n",
                      pending[p].cmdType, sensorId, pending[p].retryCount);

        bool success = sendCommand(sensorId, pending[p].cmdType,
                                   pending[p].params, pending[p].paramLen);

        if (success) {
            // The lock was released during TX, so the queue entry may have been
            // replaced by a newer command of the same type (queuedAt differs).
            // Only the entry we actually sent is removed; a replacement stays
            // queued for the next RX window and its row keeps its own history.
            bool stillOurs = false;
            bool displacedSent = false;
            LOCK_QUEUE();
            for (int i = 0; i < queueSize; i++) {
                if (commandQueue[i].sensorId == sensorId && commandQueue[i].cmdType == pending[p].cmdType) {
                    if (commandQueue[i].queuedAt == pending[p].queuedAt) {
                        removeQueuedAt(i);
                        stillOurs = true;
                    }
                    break;
                }
            }
            if (stillOurs) displacedSent = rememberSent(pending[p]);
            UNLOCK_QUEUE();

            if (stillOurs) {
                Serial.printf("✅ [CMD] Command sent, removing from queue\n");
                if (displacedSent) {
                    // An earlier unconfirmed send of the same type can no longer be confirmed
                    dbManager.writeCommandStatus(sensorId, pending[p].cmdType, "superseded", -1, String(), "sent");
                }
                dbManager.writeCommandStatus(sensorId, pending[p].cmdType, "sent", pending[p].retryCount);
            } else {
                Serial.printf("ℹ️  [CMD] Command 0x%02X was replaced while transmitting; replacement stays queued\n",
                              pending[p].cmdType);
            }
        }

        // Small delay between retries
        delay(50);
    }

    LOCK_QUEUE();
    uint8_t remaining = queueSize;
    UNLOCK_QUEUE();
    if (remaining > 0) {
        Serial.printf("📋 [CMD] %d commands remaining in queue\n", remaining);
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
 * Send CMD_SET_NAME command to sensor
 */
bool sendSetNameCommand(uint64_t sensorId, const char* name) {
    if (!name || strlen(name) == 0) {
        Serial.println("❌ Invalid name: empty string");
        return false;
    }
    
    size_t nameLen = strlen(name);
    if (nameLen > 31) {
        Serial.printf("⚠️  Name truncated from %d to 31 chars\n", nameLen);
        nameLen = 31;
    }
    
    Serial.printf("\n📡 Sending SET_NAME command: %s\n", name);
    return sendCommand(sensorId, CMD_SET_NAME, (uint8_t*)name, nameLen);
}

/**
 * Confirmation by observed effect (see header). Packets that arrived before
 * the send cannot confirm it, which matters because the MQTT task transmits
 * 3 s after the packet that opened the RX window and then processes that
 * same packet. Only commands whose value is echoed back are confirmed here.
 */
void confirmCommandsFromStatus(uint64_t sensorId, const StatusPayload* status, uint32_t packetReceivedMs) {
    uint8_t confirmed[MAX_QUEUED_COMMANDS];
    int confirmedCount = 0;

    LOCK_QUEUE();
    expireSentCommands(millis());
    for (int i = sentCount - 1; i >= 0; i--) {
        const SentCommand& s = sentCommands[i];
        if (s.sensorId != sensorId) continue;
        if ((int32_t)(packetReceivedMs - s.sentAt) <= 0) continue;  // packet predates the send

        char value[41];
        bool ok = false;
        switch (s.cmdType) {
            // CMD_STATUS is deliberately absent: sensors report status unprompted
            case CMD_SET_SLEEP:
                sentParamsAsString(s, value, sizeof(value));
                ok = (uint32_t)atoi(value) == status->deepSleepSec;
                break;
            case CMD_SET_INTERVAL:
                sentParamsAsString(s, value, sizeof(value));
                ok = (uint32_t)atoi(value) == status->sensorIntervalSec;
                break;
            case CMD_SET_NAME:
                sentParamsAsString(s, value, sizeof(value));
                ok = strncmp(value, status->deviceName, sizeof(status->deviceName)) == 0;
                break;
            default:
                break;  // no observable proof for this command type
        }
        if (ok) {
            confirmed[confirmedCount++] = s.cmdType;
            removeSentAt(i);
        }
    }
    UNLOCK_QUEUE();

    for (int c = 0; c < confirmedCount; c++) {
        Serial.printf("✅ [CMD] Command 0x%02X confirmed by status report from 0x%016llX\n", confirmed[c], sensorId);
        dbManager.writeCommandStatus(sensorId, confirmed[c], "confirmed");
    }
}

void confirmCommandsFromEvent(uint64_t sensorId, uint8_t eventType, uint32_t packetReceivedMs) {
    if (eventType != EVENT_STARTUP) return;
    bool confirmed = false;

    LOCK_QUEUE();
    expireSentCommands(millis());
    for (int i = sentCount - 1; i >= 0; i--) {
        const SentCommand& s = sentCommands[i];
        if (s.sensorId != sensorId || s.cmdType != CMD_RESTART) continue;
        if ((int32_t)(packetReceivedMs - s.sentAt) <= 0) continue;
        removeSentAt(i);
        confirmed = true;
    }
    UNLOCK_QUEUE();

    if (confirmed) {
        Serial.printf("✅ [CMD] Restart confirmed by startup event from 0x%016llX\n", sensorId);
        dbManager.writeCommandStatus(sensorId, CMD_RESTART, "confirmed");
    }
}

/**
 * Get number of queued commands for a specific sensor
 */
int getQueuedCommandCount(uint64_t sensorId) {
    int count = 0;
    LOCK_QUEUE();
    for (int i = 0; i < queueSize; i++) {
        if (commandQueue[i].sensorId == sensorId) {
            count++;
        }
    }
    UNLOCK_QUEUE();
    return count;
}

/**
 * Get JSON array of queued commands for a specific sensor
 */
String getQueuedCommandsJson(uint64_t sensorId) {
    String result = "[";
    bool first = true;
    
    LOCK_QUEUE();
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
                case CMD_SET_NAME: cmdName = "set_name"; break;
            }
            
            result += "{\"type\":\"";
            result += cmdName;
            result += "\",\"retries\":";
            result += String(commandQueue[i].retryCount);
            result += "}";
        }
    }
    UNLOCK_QUEUE();
    
    result += "]";
    return result;
}
