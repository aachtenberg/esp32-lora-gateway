#include "display_manager.h"
#include "device_config.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

// OLED display instance (128x64 SSD1306)
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C* display = nullptr;

// Display state
static String currentStatus = "Starting...";
static uint32_t packetsReceived = 0;
static int16_t lastRssi = 0;
static String lastDevice = "";

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

    // Snapshot LoRa debug values (avoid holding lock while drawing)
    uint32_t ok, dropped, dup, lastMs;
    uint16_t devShort, seq;
    uint8_t type, payloadLen;
    int16_t rssi, err;
    int8_t snr;
    uint8_t hdr[4];
    portENTER_CRITICAL(&loraDbgMux);
    ok = loraOk;
    dropped = loraDropped;
    dup = loraDup;
    devShort = loraLastDevShort;
    seq = loraLastSeq;
    type = loraLastType;
    payloadLen = loraLastPayloadLen;
    rssi = loraLastRssi;
    snr = loraLastSnr;
    err = loraLastErr;
    lastMs = loraLastPacketMs;
    memcpy(hdr, loraLastHdr, 4);
    portEXIT_CRITICAL(&loraDbgMux);
    
    display->clearBuffer();
    
    // Title
    display->setFont(u8g2_font_ncenB08_tr);
    display->drawStr(0, 10, "LoRa Gateway");
    
    // Packet count + devices
    display->setFont(u8g2_font_6x10_tr);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "OK:%lu Dev:%d", ok ? ok : packets, deviceCount);
    display->drawStr(0, 25, buffer);

    // RX statistics line
    snprintf(buffer, sizeof(buffer), "Drop:%lu Dup:%lu", dropped, dup);
    display->drawStr(0, 38, buffer);

    // Last packet line
    if (lastMs != 0) {
        snprintf(buffer, sizeof(buffer), "ID:%04X S:%u T:%02X L:%u", devShort, (unsigned)seq, type, payloadLen);
        display->drawStr(0, 51, buffer);

        // Header + RSSI/SNR
        char buffer2[32];
        snprintf(buffer2, sizeof(buffer2), "%02X%02X %02X%02X R:%d S:%d", hdr[0], hdr[1], hdr[2], hdr[3], rssi, (int)snr);
        display->setFont(u8g2_font_5x7_tr);
        display->drawStr(0, 63, buffer2);
    } else {
        // No packet yet: show RX state / last error
        if (err != 0) {
            snprintf(buffer, sizeof(buffer), "RX err: %d", err);
        } else {
            snprintf(buffer, sizeof(buffer), "RX: waiting...");
        }
        display->drawStr(0, 51, buffer);
    }
    
    display->sendBuffer();
}

/**
 * Display packet received with sensor data
 */
void displayPacketReceived(uint64_t deviceId, float temp, float humidity, int16_t rssi, int8_t snr) {
    if (display == nullptr) return;
    
    packetsReceived++;
    lastRssi = rssi;
    
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
    
    display->clearBuffer();
    
    display->setFont(u8g2_font_ncenB10_tr);
    display->drawStr(30, 20, "ERROR");
    
    display->setFont(u8g2_font_6x10_tr);
    display->drawStr(5, 40, error);
    
    display->sendBuffer();
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
