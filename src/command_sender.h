#ifndef COMMAND_SENDER_H
#define COMMAND_SENDER_H

#include <Arduino.h>
#include <stdint.h>

/**
 * Send a command to a remote LoRa sensor
 * 
 * @param sensorId: 64-bit device ID of target sensor
 * @param cmdType: Command type (from lora_protocol.h)
 * @param params: Parameter data (binary)
 * @param paramLen: Length of parameter data (max 238 bytes)
 * @return true if transmission successful, false otherwise
 */
bool sendCommand(uint64_t sensorId, uint8_t cmdType, const uint8_t* params, uint8_t paramLen);

/**
 * Send CMD_SET_SLEEP command to sensor
 * Configure deep sleep interval in seconds
 * 
 * @param sensorId: 64-bit device ID of target sensor
 * @param sleepSeconds: Sleep interval (0-3600 seconds)
 * @return true if transmission successful
 */
bool sendSetSleepCommand(uint64_t sensorId, uint32_t sleepSeconds);

/**
 * Send CMD_SET_INTERVAL command to sensor
 * Configure sensor read interval in seconds
 * 
 * @param sensorId: 64-bit device ID of target sensor
 * @param intervalSeconds: Read interval (5-3600 seconds)
 * @return true if transmission successful
 */
bool sendSetIntervalCommand(uint64_t sensorId, uint32_t intervalSeconds);

/**
 * Send CMD_RESTART command to sensor
 * Request device restart
 * 
 * @param sensorId: 64-bit device ID of target sensor
 * @return true if transmission successful
 */
bool sendRestartCommand(uint64_t sensorId);

/**
 * Send CMD_STATUS command to sensor
 * Request immediate status update
 * 
 * @param sensorId: 64-bit device ID of target sensor
 * @return true if transmission successful
 */
bool sendStatusCommand(uint64_t sensorId);

#endif // COMMAND_SENDER_H
