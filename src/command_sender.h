#ifndef COMMAND_SENDER_H
#define COMMAND_SENDER_H

#include <Arduino.h>
#include <stdint.h>

// Maximum queued commands
#define MAX_QUEUED_COMMANDS 10

// Command expiration time (5 minutes)
#define COMMAND_EXPIRATION_MS (5 * 60 * 1000)

/**
 * Initialize command sender with retry mechanism
 */
void initCommandSender();

/**
 * Send a command to a remote LoRa sensor (immediate transmission)
 * 
 * @param sensorId: 64-bit device ID of target sensor
 * @param cmdType: Command type (from lora_protocol.h)
 * @param params: Parameter data (binary)
 * @param paramLen: Length of parameter data (max 238 bytes)
 * @return true if transmission successful, false otherwise
 */
bool sendCommand(uint64_t sensorId, uint8_t cmdType, const uint8_t* params, uint8_t paramLen);

/**
 * Queue a command for persistent retry until received
 * Command will be retried automatically on sensor activity
 * 
 * @param sensorId: 64-bit device ID of target sensor
 * @param cmdType: Command type (from lora_protocol.h)
 * @param params: Parameter data (binary)
 * @param paramLen: Length of parameter data (max 238 bytes)
 * @return true if queued successfully, false if queue full
 */
bool queueCommand(uint64_t sensorId, uint8_t cmdType, const uint8_t* params, uint8_t paramLen);

/**
 * Retry queued commands for a specific sensor
 * Call this when a packet is received from the sensor
 * 
 * @param sensorId: 64-bit device ID of sensor that just transmitted
 */
void retryCommandsForSensor(uint64_t sensorId);

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

/**
 * Send CMD_CALIBRATE command to sensor
 * Set current pressure reading as baseline
 *
 * @param sensorId: 64-bit device ID of target sensor
 * @return true if transmission successful
 */
bool sendCalibrateCommand(uint64_t sensorId);

/**
 * Send CMD_SET_BASELINE command to sensor
 * Set specific pressure baseline value
 *
 * @param sensorId: 64-bit device ID of target sensor
 * @param baselineHpa: Baseline pressure in hPa (900-1100)
 * @return true if transmission successful
 */
bool sendSetBaselineCommand(uint64_t sensorId, float baselineHpa);

/**
 * Send CMD_CLEAR_BASELINE command to sensor
 * Disable pressure baseline tracking
 *
 * @param sensorId: 64-bit device ID of target sensor
 * @return true if transmission successful
 */
bool sendClearBaselineCommand(uint64_t sensorId);

#endif // COMMAND_SENDER_H
