#include "display_manager.h"
#include "device_config.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>

// OLED display instance (128x64 SSD1306)
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C* display = nullptr;

// Display state
static String currentStatus = "Starting...";
static uint32_t packetsReceived = 0;
static int16_t lastRssi = 0;
static String lastDevice = "";

// Last sensor readings for display
static float lastTemp = 0.0;
static float lastHumidity = 0.0;
static float lastPressure = 0.0;
static int8_t lastPressureTrend = 1;  // 0=falling, 1=steady, 2=rising
static float lastPressureChange = 0.0;

// Mutex for display rendering (to prevent corruption from concurrent access)
static SemaphoreHandle_t displayMutex = nullptr;

// LoRa debug state (updated from LoRa RX task; rendered from main loop)
static portMUX_TYPE loraDbgMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t loraOk = 0;
static uint32_t loraDropped = 0;
static uint32_t loraDup = 0;
static uint16_t loraLastDevShort = 0;
static uint16_t loraLastSeq = 0;
static uint8_t loraLastType = 0;
static uint8_t loraLastPayloadLen = 0;
static int16_t loraLastRssi = 0;
static int8_t loraLastSnr = 0;
static int16_t loraLastErr = 0;
static uint32_t loraLastPacketMs = 0;
static uint8_t loraLastHdr[4] = {0, 0, 0, 0};

static uint8_t detectOledAddress7Bit() {
    // Common I2C addresses for 128x64 OLEDs
    const uint8_t candidates[] = {0x3C, 0x3D};
    for (uint8_t addr : candidates) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            return addr;
        }
    }
    return 0;
}

void displayUpdateLoRaStats(uint32_t ok, uint32_t dropped, uint32_t duplicates) {
    portENTER_CRITICAL(&loraDbgMux);
    loraOk = ok;
    loraDropped = dropped;
    loraDup = duplicates;
    portEXIT_CRITICAL(&loraDbgMux);
}

void displayUpdateLoRaLastPacket(uint16_t deviceShort, uint16_t seq, uint8_t msgType, uint8_t payloadLen,
                                 int16_t rssi, int8_t snr, const uint8_t headerBytes[4]) {
    portENTER_CRITICAL(&loraDbgMux);
    loraLastDevShort = deviceShort;
    loraLastSeq = seq;
    loraLastType = msgType;
    loraLastPayloadLen = payloadLen;
    loraLastRssi = rssi;
    loraLastSnr = snr;
    loraLastPacketMs = millis();
    if (headerBytes) {
        memcpy(loraLastHdr, headerBytes, 4);
    }
    loraLastErr = 0;
    portEXIT_CRITICAL(&loraDbgMux);
}

void displayUpdateLoRaLastError(int16_t err) {
    portENTER_CRITICAL(&loraDbgMux);
    loraLastErr = err;
    portEXIT_CRITICAL(&loraDbgMux);
}

/**
 * Initialize OLED display
 */
bool initDisplay() {
    Serial.print("Checking for I2C display... ");

    // Ensure peripherals are powered (Heltec V3: OLED is on VEXT)
    // OFF first to ensuring clean reset
    pinMode(VEXT_CTRL, OUTPUT);
    digitalWrite(VEXT_CTRL, HIGH); 
    delay(10);
    // ON
    digitalWrite(VEXT_CTRL, LOW);
    delay(100);
    
    // Manually reset OLED
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(100);
    digitalWrite(OLED_RST, HIGH);
    delay(100);

    // Initialize I2C (OLED bus on Heltec V3)
    // S3 hardware I2C often needs explicit pin re-setup if used previously, 
    // but here it is first use.
    if (!Wire.begin(OLED_SDA, OLED_SCL)) {
        Serial.println("Wire.begin failed!");
        return false;
    }
    Wire.setClock(100000); // 100kHz
    
    Serial.printf("Scanning I2C SCL=%d SDA=%d... \n", OLED_SCL, OLED_SDA);

    // Detect OLED I2C address (0x3C or 0x3D)
    uint8_t oledAddr7 = 0;
    for (int retry = 0; retry < 3; retry++) {
        oledAddr7 = detectOledAddress7Bit();
        if (oledAddr7 != 0) break;
        Serial.print(".");
        delay(100);
    }
    
    if (oledAddr7 == 0) {
        // Detailed scan for debugging
        Serial.println("\nI2C Scan details:");
        int nDevices = 0;
        for(uint8_t address = 1; address < 127; address++) {
            Wire.beginTransmission(address);
            uint8_t error = Wire.endTransmission();
            if (error == 0) {
                Serial.printf("  Device found at 0x%02X\n", address);
                nDevices++;
                if (address == 0x3C || address == 0x3D) oledAddr7 = address;
            } else if (error == 4) {
                // Unknown error
                Serial.print("  Error at 0x"); Serial.println(address, HEX);
            }
        }
        
        if (oledAddr7 == 0) {
            Serial.println("Not found (display optional)");
            return false;
        }
    }
    Serial.printf("Found at 0x%02X!\n", oledAddr7);
    
    // Create display instance
    // Use HW I2C; rely on Wire.begin() for pin mapping
    display = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, OLED_RST);

    // U8g2 expects 8-bit I2C address (7-bit << 1)
    display->setI2CAddress(oledAddr7 << 1);
    display->setBusClock(100000);
    
    // Initialize display (returns true when not using dynamic alloc)
    display->begin();
    display->setContrast(255);
    
    // Create display mutex
    if (displayMutex == nullptr) {
        displayMutex = xSemaphoreCreateMutex();
        if (displayMutex == nullptr) {
            Serial.println("Failed to create display mutex!");
            return false;
        }
    }
    
    display->clearBuffer();
    display->setFont(u8g2_font_ncenB08_tr);

    // Immediate self-test screen
    display->drawStr(0, 12, "OLED OK");
    display->drawStr(0, 28, "LoRa Gateway");
    display->sendBuffer();
    
    Serial.println("✅ Display initialized");
    return true;
}

/**
 * Display startup screen
 */
void displayStartup(const char* version) {
    if (display == nullptr) return;
    
    display->clearBuffer();
    
    // Title
    display->setFont(u8g2_font_ncenB10_tr);
    display->drawStr(10, 15, "LoRa Gateway");
    
    // Version
    display->setFont(u8g2_font_ncenB08_tr);
    display->drawStr(20, 30, version);
    
    // Status
    display->drawStr(15, 50, "Initializing...");
    
    display->sendBuffer();
}

/**
 * Display WiFi connection status
 */
void displayWiFi(const char* ssid, const char* ip) {
    if (display == nullptr) return;
    
    display->clearBuffer();
    
    display->setFont(u8g2_font_ncenB08_tr);
    display->drawStr(0, 10, "WiFi Connected");
    display->drawStr(0, 25, ssid);
    
    display->setFont(u8g2_font_6x10_tr);
    display->drawStr(0, 40, ip);
    
    display->sendBuffer();
}

/**
 * Display main status screen
 */
void displayStatus(uint32_t packets, int deviceCount) {
    if (display == nullptr) return;

    // Snapshot LoRa debug values
    uint32_t ok, dropped, dup;
    uint16_t devShort;
    int16_t rssi;
    int8_t snr;
    portENTER_CRITICAL(&loraDbgMux);
    ok = loraOk;
    dropped = loraDropped;
    dup = loraDup;
    devShort = loraLastDevShort;
    rssi = loraLastRssi;
    snr = loraLastSnr;
    portEXIT_CRITICAL(&loraDbgMux);

    // Lock display for rendering
    if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        display->clearBuffer();
        char buffer[32];

        // Title
        display->setFont(u8g2_font_ncenB08_tr);
        display->drawStr(0, 10, "LoRa Gateway");

        // Temperature and humidity (if we have data)
        if (lastTemp != 0.0 || lastHumidity != 0.0) {
            display->setFont(u8g2_font_6x10_tr);
            snprintf(buffer, sizeof(buffer), "%.1fC  H:%.0f%%", lastTemp, lastHumidity);
            display->drawStr(0, 24, buffer);

            // Pressure with trend
            const char* trendSymbol = "-";  // Steady
            if (lastPressureTrend == 2) trendSymbol = "^";      // Rising
            else if (lastPressureTrend == 0) trendSymbol = "v"; // Falling

            if (lastPressure > 0) {
                snprintf(buffer, sizeof(buffer), "P:%.0f%s", lastPressure, trendSymbol);
                display->drawStr(0, 37, buffer);

                // Pressure change (if baseline set)
                if (lastPressureChange != 0.0) {
                    display->setFont(u8g2_font_5x7_tf);
                    snprintf(buffer, sizeof(buffer), "%+.1f", lastPressureChange);
                    display->drawStr(70, 37, buffer);
                    display->setFont(u8g2_font_6x10_tr);
                }
            }
        } else {
            // No sensor data yet
            display->setFont(u8g2_font_6x10_tr);
            display->drawStr(0, 24, "Waiting for");
            display->drawStr(0, 37, "sensor data...");
        }

        // Device count and packet stats
        display->setFont(u8g2_font_5x7_tf);
        snprintf(buffer, sizeof(buffer), "Dev:%d RX:%lu D:%lu", deviceCount, ok ? ok : packets, dropped);
        display->drawStr(0, 50, buffer);

        // Last device ID and RSSI
        if (devShort != 0) {
            snprintf(buffer, sizeof(buffer), "ID:%04X RSSI:%ddBm", devShort, rssi);
            display->drawStr(0, 62, buffer);
        } else {
            display->drawStr(0, 62, "No packets yet");
        }

        display->sendBuffer();
        xSemaphoreGive(displayMutex);
    }
}

/**
 * Display packet received with sensor data
 */
void displayPacketReceived(uint64_t deviceId, float temp, float humidity, int16_t rssi, int8_t snr) {
    if (display == nullptr) return;

    packetsReceived++;
    lastRssi = rssi;

    // Store readings for main display
    lastTemp = temp;
    lastHumidity = humidity;

    if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        display->clearBuffer();
        display->setFont(u8g2_font_ncenB08_tr);
        display->drawStr(0, 10, "Packet RX");

        // Device ID (last 4 hex digits)
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "ID: %04X", (uint16_t)(deviceId & 0xFFFF));
        display->drawStr(0, 22, buffer);

        // Temperature and Humidity
        display->setFont(u8g2_font_6x10_tr);
        snprintf(buffer, sizeof(buffer), "T:%.1fC H:%.0f%%", temp, humidity);
        display->drawStr(0, 34, buffer);

        // RSSI and SNR
        snprintf(buffer, sizeof(buffer), "RSSI:%d SNR:%d", rssi, snr);
        display->drawStr(0, 46, buffer);

        // Packet count
        snprintf(buffer, sizeof(buffer), "Total: %lu", packetsReceived);
        display->drawStr(0, 58, buffer);

        display->sendBuffer();
        xSemaphoreGive(displayMutex);
    }
}

/**
 * Update stored sensor readings for display
 */
void displayUpdateSensorData(float temp, float humidity, float pressure, int8_t pressureTrend, float pressureChange) {
    lastTemp = temp;
    lastHumidity = humidity;
    lastPressure = pressure;
    lastPressureTrend = pressureTrend;
    lastPressureChange = pressureChange;
}

/**
 * Display packet received
 */
void displayPacket(const char* deviceName, int16_t rssi, int8_t snr) {
    lastDevice = String(deviceName);
    lastRssi = rssi;
    packetsReceived++;
}

/**
 * Display error message
 */
void displayError(const char* error) {
    if (display == nullptr) return;
    
    if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        display->clearBuffer();
        
        display->setFont(u8g2_font_ncenB10_tr);
        display->drawStr(30, 20, "ERROR");
        
        display->setFont(u8g2_font_6x10_tr);
        display->drawStr(5, 40, error);
        
        display->sendBuffer();
        xSemaphoreGive(displayMutex);
    }
}

/**
 * Update display (call periodically from main loop)
 */
void updateDisplay(uint32_t packets, int deviceCount) {
    static uint32_t lastUpdate = 0;
    
    // Update every 1 second
    if (millis() - lastUpdate > 1000) {
        lastUpdate = millis();
        displayStatus(packets, deviceCount);
    }
}
