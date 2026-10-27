#ifndef COMMAND_SENDER_H
#define COMMAND_SENDER_H

#include <Arduino.h>
#include <stdint.h>
#include "lora_protocol.h"

// Maximum queued commands
#define MAX_QUEUED_COMMANDS 10

// Command expiration time (30 minutes - long enough for deep-sleeping sensors)
#define COMMAND_EXPIRATION_MS (30 * 60 * 1000)

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
 * Confirmation by observed effect. Sensors do not acknowledge commands, so a
 * command counts as confirmed when a later packet shows it took effect:
 *  - CMD_SET_SLEEP / CMD_SET_INTERVAL / CMD_SET_NAME: status report with the sent value
 *  - CMD_RESTART: EVENT_STARTUP received after the send (an unrelated reboot in
 *    the 15-minute window also confirms it; the dashboard says so)
 * CMD_STATUS stays "sent": sensors report status on their own, so a later
 * report proves nothing about the request. Calibration/baseline commands have
 * no observable proof either and stay "sent".
 *
 * @param packetReceivedMs: millis() when the packet arrived (must be after the send)
 */
void confirmCommandsFromStatus(uint64_t sensorId, const StatusPayload* status, uint32_t packetReceivedMs);
void confirmCommandsFromEvent(uint64_t sensorId, uint8_t eventType, uint32_t packetReceivedMs);

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

/**
 * Send CMD_SET_NAME command to sensor
 * Set the device's friendly name (max 31 characters)
 *
 * @param sensorId: 64-bit device ID of target sensor
 * @param name: New device name (will be truncated to 31 chars)
 * @return true if transmission successful
 */
bool sendSetNameCommand(uint64_t sensorId, const char* name);

/**
 * Get number of queued commands for a specific sensor
 * 
 * @param sensorId: 64-bit device ID of sensor
 * @return number of commands in queue for this sensor
 */
int getQueuedCommandCount(uint64_t sensorId);

/**
 * Get JSON array of queued commands for a specific sensor
 * 
 * @param sensorId: 64-bit device ID of sensor
 * @return JSON string with array of command types
 */
String getQueuedCommandsJson(uint64_t sensorId);

#endif // COMMAND_SENDER_H
