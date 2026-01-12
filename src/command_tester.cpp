/**
 * Command Tester - Serial Command Handler for Gateway
 * Allows testing command sender via serial console
 */

#include "command_tester.h"
#include "command_sender.h"
#include <Arduino.h>

/**
 * Parse hex string to uint64_t
 */
static uint64_t parseHex(const String& hexStr) {
    return strtoull(hexStr.c_str(), nullptr, 16);
}

/**
 * Handle serial commands for testing
 * Format: send_interval <device_id> <seconds>
 *         send_sleep <device_id> <seconds>
 *         send_restart <device_id>
 *         send_status <device_id>
 */
void handleSerialCommands() {
    if (!Serial.available()) {
        return;
    }
    
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    if (command.length() == 0) {
        return;
    }
    
    Serial.println("[CMD] Received: " + command);
    
    // Split command into parts
    int spaceIdx1 = command.indexOf(' ');
    if (spaceIdx1 == -1) {
        Serial.println("❌ Invalid format");
        return;
    }
    
    String action = command.substring(0, spaceIdx1);
    String rest = command.substring(spaceIdx1 + 1);
    
    int spaceIdx2 = rest.indexOf(' ');
    String deviceIdStr = (spaceIdx2 != -1) ? rest.substring(0, spaceIdx2) : rest;
    String paramStr = (spaceIdx2 != -1) ? rest.substring(spaceIdx2 + 1) : "";
    
    uint64_t deviceId = parseHex(deviceIdStr);
    
    Serial.printf("[CMD] Action: %s, Device: %016llX\n", action.c_str(), deviceId);
    
    bool success = false;
    
    if (action.equalsIgnoreCase("send_interval")) {
        uint32_t seconds = paramStr.toInt();
        Serial.printf("[CMD] Setting interval to %lu seconds\n", seconds);
        success = sendSetIntervalCommand(deviceId, seconds);
        
    } else if (action.equalsIgnoreCase("send_sleep")) {
        uint32_t seconds = paramStr.toInt();
        Serial.printf("[CMD] Setting sleep to %lu seconds\n", seconds);
        success = sendSetSleepCommand(deviceId, seconds);
        
    } else if (action.equalsIgnoreCase("send_restart")) {
        Serial.println("[CMD] Sending restart command");
        success = sendRestartCommand(deviceId);
        
    } else if (action.equalsIgnoreCase("send_status")) {
        Serial.println("[CMD] Sending status command");
        success = sendStatusCommand(deviceId);
        
    } else if (action.equalsIgnoreCase("help")) {
        Serial.println("\n=== Command Tester Help ===");
        Serial.println("send_interval <device_id> <seconds>  - Set sensor read interval");
        Serial.println("send_sleep <device_id> <seconds>     - Set deep sleep interval");
        Serial.println("send_restart <device_id>             - Restart device");
        Serial.println("send_status <device_id>              - Request status update");
        Serial.println("Example: send_interval f09e9e76aec4 90");
        Serial.println("============================\n");
        return;
        
    } else {
        Serial.println("❌ Unknown action: " + action);
        Serial.println("Type 'help' for command list");
        return;
    }
    
    if (success) {
        Serial.println("✅ Command sent successfully!");
    } else {
        Serial.println("❌ Command send failed!");
    }
}
