#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

// ====================================================================
// ESP32 LoRa Gateway - Hardware Configuration
// ====================================================================

// SX1262 LoRa Module Pins (SPI Interface)
// Heltec WiFi LoRa 32 V3 (ESP32-S3) with built-in SX1262
#define LORA_MISO      11   // Built-in SX1262 MISO
#define LORA_MOSI      10   // Built-in SX1262 MOSI
#define LORA_SCK       9    // Built-in SX1262 SCK
#define LORA_NSS       8    // Built-in SX1262 NSS (Chip Select)
#define LORA_DIO1      14   // Built-in SX1262 DIO1 (IRQ)
#define LORA_BUSY      13   // Built-in SX1262 BUSY
#define LORA_RST       12   // Built-in SX1262 RST

// Heltec-specific: Vext power control for LoRa and OLED
// Vext must be LOW to enable power to peripherals
#ifndef VEXT_CTRL
#define VEXT_CTRL      36   // Vext control pin (LOW = ON, HIGH = OFF)
#endif

// OLED Display (I2C - Built-in on board)
#define OLED_SDA       17   // Built-in OLED SDA
#define OLED_SCL       18   // Built-in OLED SCL
#define OLED_RST       21   // Built-in OLED Reset
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
