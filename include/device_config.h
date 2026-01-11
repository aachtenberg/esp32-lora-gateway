#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

// ====================================================================
// ESP32 LoRa Gateway - Hardware Configuration
// ====================================================================

// SX1262 LoRa Module Pins (SPI Interface)
// IMPORTANT: Verify these against your specific ESP32 LoRa module datasheet
#define LORA_MISO      19   // SPI MISO
#define LORA_MOSI      27   // SPI MOSI
#define LORA_SCK       5    // SPI Clock
#define LORA_NSS       18   // Chip Select (SS)
#define LORA_DIO1      26   // IRQ Pin (DIO1)
#define LORA_BUSY      23   // Busy Pin
#define LORA_RST       14   // Reset Pin

// OLED Display (I2C)
#define OLED_SDA       21   // I2C SDA
#define OLED_SCL       22   // I2C SCL
#define OLED_ADDR      0x3C // Standard I2C address for SSD1306

// Status LED (optional)
#define STATUS_LED     2    // Built-in LED on most ESP32 boards

// Gateway Configuration
#define MAX_SENSORS    10   // Maximum number of tracked sensors
#define DEDUP_BUFFER_SIZE 50  // Deduplication buffer per sensor

// MQTT Configuration
#define MQTT_RECONNECT_INTERVAL_MS 5000  // Retry interval
#define MQTT_KEEPALIVE_SEC 15

// WiFiManager Configuration
#define CONFIG_PORTAL_TIMEOUT_SEC 180  // 3 minutes

#endif // DEVICE_CONFIG_H
