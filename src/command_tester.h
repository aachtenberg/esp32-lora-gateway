#ifndef COMMAND_TESTER_H
#define COMMAND_TESTER_H

#include <Arduino.h>
#include <stdint.h>

/**
 * Serial command handler for testing command sender
 * Processes commands like:
 *   "send_interval <hex_device_id> <seconds>"
 *   "send_sleep <hex_device_id> <seconds>"
 *   "send_restart <hex_device_id>"
 *   "send_status <hex_device_id>"
 */
void handleSerialCommands();

#endif // COMMAND_TESTER_H
