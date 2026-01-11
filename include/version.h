#ifndef VERSION_H
#define VERSION_H

#include <Arduino.h>

// Firmware version (set via build flags)
#ifndef FIRMWARE_VERSION_MAJOR
#define FIRMWARE_VERSION_MAJOR 2
#endif

#ifndef FIRMWARE_VERSION_MINOR
#define FIRMWARE_VERSION_MINOR 0
#endif

#ifndef FIRMWARE_VERSION_PATCH
#define FIRMWARE_VERSION_PATCH 0
#endif

// Build timestamp (auto-generated at compile time)
#define BUILD_DATE __DATE__
#define BUILD_TIME __TIME__

// Helper function to get version string
inline String getFirmwareVersion() {
    char version[32];
    snprintf(version, sizeof(version), "%d.%d.%d-build%s",
             FIRMWARE_VERSION_MAJOR,
             FIRMWARE_VERSION_MINOR,
             FIRMWARE_VERSION_PATCH,
             __DATE__);
    return String(version);
}

#endif // VERSION_H
