#include "lora_receiver.h"
#include "lora_config.h"
#include "device_config.h"
#include "packet_queue.h"
#include "device_registry.h"
#include "display_manager.h"
#include <RadioLib.h>
#include <SPI.h>

// SX1262 module instance
static SX1262* radio = nullptr;

// Packet queue for communication between LoRa RX and MQTT tasks
static QueueHandle_t rxPacketQueue = nullptr;

// Gateway device ID
static uint64_t gatewayId = 0;

// Statistics
static uint32_t packetsReceived = 0;
static uint32_t packetsDropped = 0;
static uint32_t duplicatesFiltered = 0;

// Power control pin (Heltec boards)
#ifndef VEXT_CTRL
#define VEXT_CTRL 36  // Vext control pin for Heltec boards
#endif

/**
 * Initialize LoRa receiver
 */
bool initLoRaReceiver() {
    Serial.println("\n=== LoRa Receiver Initialization ===");
    
    // Get gateway unique ID
    gatewayId = ESP.getEfuseMac();
    Serial.printf("Gateway ID: 0x%016llX\n", gatewayId);
    
    // Enable Vext power (Heltec boards require this)
    Serial.print("Enabling Vext power... ");
    pinMode(VEXT_CTRL, OUTPUT);
    digitalWrite(VEXT_CTRL, LOW);  // LOW = power ON
    delay(100);
    Serial.println("✅");
    
    // Initialize SPI
    Serial.print("Initializing SPI... ");
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
    delay(50);
    Serial.println("✅");
    
    // Create SX1262 instance
    Serial.print("Creating SX1262 instance... ");
    radio = new SX1262(new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY));
    Serial.println("✅");
    
    // Initialize SX1262
    Serial.print("Initializing SX1262... ");
    int state = radio->begin(LORA_FREQUENCY,
                             LORA_BANDWIDTH,
                             LORA_SPREADING,
                             LORA_CODING_RATE,
                             LORA_SYNC_WORD,
                             LORA_TX_POWER,
                             LORA_PREAMBLE_LEN);
    
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("❌ Failed! (code: %d)\n", state);
        delete radio;
        radio = nullptr;
        return false;
    }
    Serial.println("✅");
    
    // Configure CRC
    state = radio->setCRC(LORA_CRC_ENABLED);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("⚠️  CRC config failed (code: %d)\n", state);
    }
    
    // Force explicit header mode (includes length/coding info in packet)
    state = radio->explicitHeader();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("⚠️  Explicit header config failed (code: %d)\n", state);
    }
    
    // Print radio configuration
    Serial.println("\nRadio Configuration:");
    Serial.printf("  Frequency: %.1f MHz\n", LORA_FREQUENCY);
    Serial.printf("  Bandwidth: %.1f kHz\n", LORA_BANDWIDTH);
    Serial.printf("  Spreading Factor: %d\n", LORA_SPREADING);
    Serial.printf("  Coding Rate: 4/%d\n", LORA_CODING_RATE);
    Serial.printf("  TX Power: %d dBm\n", LORA_TX_POWER);
    Serial.printf("  Sync Word: 0x%02X\n", LORA_SYNC_WORD);
    Serial.println("===================================\n");
    
    // Configure IRQ on DIO1
    // The SX126x requires we set up DIO1 to notify us of RxDone.
    // In RadioLib setDio1Action() usually attaches an interrupt to a pin.
    // Since we are moving to a polling model in the task, we may not want the ISR.
    // However, if we don't set it, RadioLib might not configure the radio to fire the signal on the pin.
    // But startReceive() configures the radio to listen.
    
    // Create packet queue
    rxPacketQueue = xQueueCreate(20, sizeof(ReceivedPacket));
    if (rxPacketQueue == NULL) {
        Serial.println("❌ Failed to create packet queue!");
        return false;
    }

    // Start continuous receive mode
    Serial.print("Starting continuous RX... ");
    state = radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("❌ (code: %d)\n", state);
        return false;
    }
    Serial.println("✅");
    
    Serial.println("✅ LoRa receiver ready!\n");
    Serial.println("Gateway will poll for packets using IRQ flag checks in loraRxTask()\n");
    return true;
}

/**
 * LoRa RX Task (runs on Core 0)
 * High-priority task for receiving and processing LoRa packets
 */
void loraRxTask(void* parameter) {
    Serial.println("[LoRa RX Task] Started on Core 0");
    
    uint8_t rxBuffer[sizeof(LoRaPacketHeader) + LORA_MAX_PAYLOAD_SIZE];
    
    while (true) {
        // Poll for packet availability
        // In "startReceive" mode (interrupt-based logic but without ISR callback),
        // we can poll the IRQ pin manually or ask the radio object.
        
        // Method 1: Check hardware pin if available (fastest, no SPI)
        bool irqTriggered = (digitalRead(LORA_DIO1) == HIGH);
        
        // Method 2: Ask RadioLib (uses SPI, slower)
        // int16_t state = radio->readData(rxBuffer, sizeof(rxBuffer));
        // But readData() is blocking or designed for packet mode?
        // Actually, startReceive() puts radio in RX.
        // We must detect when it's done.
        
        if (irqTriggered) {
             // Interrupt detected! A packet might be ready.
             // readData() reads the packet and clears the IRQ flags.
             memset(rxBuffer, 0, sizeof(rxBuffer));
             int state = radio->readData(rxBuffer, sizeof(rxBuffer));
             
             if (state == RADIOLIB_ERR_NONE) {
                // Packet received successfully
                packetsReceived++;
                
                // Get actual packet length
                size_t packetLen = radio->getPacketLength();
                
                // Get RSSI and SNR
                int16_t rssi = radio->getRSSI();
                int8_t snr = radio->getSNR();
            uint32_t timestamp = millis();
            
            Serial.printf("\n[LoRa RX] Packet received (RSSI: %d dBm, SNR: %d dB, Len: %zu bytes)\n", 
                         rssi, snr, packetLen);
            
            // Validate minimum packet size (header must be present)
            if (packetLen < sizeof(LoRaPacketHeader)) {
                Serial.printf("⚠️  Packet too short (%zu bytes)\n", packetLen);
                packetsDropped++;
                continue;
            }
            
            // Parse header
            LoRaPacketHeader* header = (LoRaPacketHeader*)rxBuffer;
            
            // Debug: Print raw header bytes
            Serial.print("  Raw header: ");
            for (size_t i = 0; i < sizeof(LoRaPacketHeader); i++) {
                Serial.printf("%02X ", rxBuffer[i]);
            }
            Serial.println();
            
            // Debug: Print entire packet
            Serial.printf("  Full packet (%zu bytes): ", packetLen);
            for (size_t i = 0; i < packetLen && i < 80; i++) {
                Serial.printf("%02X ", rxBuffer[i]);
                if ((i + 1) % 20 == 0) Serial.println();
            }
            Serial.println();
            
            // Validate header
            if (!validateHeader(header)) {
                Serial.printf("⚠️  Invalid packet header (Magic: %02X%02X, Ver: %02X, Chk: %02X exp: %02X)\n",
                             header->magic[0], header->magic[1], header->version,
                             header->checksum, calculateHeaderChecksum(header));
                packetsDropped++;
                displayUpdateLoRaStats(packetsReceived, packetsDropped, duplicatesFiltered);
                continue;
            }
            
            // Check for duplicate
            if (isDuplicate(header->deviceId, header->sequenceNum)) {
                Serial.printf("⚠️  Duplicate packet (Seq: %d)\n", header->sequenceNum);
                duplicatesFiltered++;
                displayUpdateLoRaStats(packetsReceived, packetsDropped, duplicatesFiltered);
                continue;
            }
            
            Serial.printf("  Device: 0x%016llX\n", header->deviceId);
            Serial.printf("  Type: 0x%02X, Seq: %d, Payload: %d bytes\n",
                         header->msgType, header->sequenceNum, header->payloadLen);
            
            // Update device registry
            updateDeviceInfo(header->deviceId, header->sequenceNum, rssi, snr);

            // Update OLED debug state (safe: does not touch I2C)
            uint8_t hdr4[4] = { rxBuffer[0], rxBuffer[1], rxBuffer[2], rxBuffer[3] };
            displayUpdateLoRaLastPacket((uint16_t)(header->deviceId & 0xFFFF),
                                        header->sequenceNum,
                                        header->msgType,
                                        header->payloadLen,
                                        rssi,
                                        snr,
                                        hdr4);
            displayUpdateLoRaStats(packetsReceived, packetsDropped, duplicatesFiltered);
            
            // Build received packet structure
            ReceivedPacket packet;
            memcpy(&packet.header, header, sizeof(LoRaPacketHeader));
            
            // Copy payload
            if (header->payloadLen > 0 && header->payloadLen <= LORA_MAX_PAYLOAD_SIZE) {
                memcpy(packet.payload, rxBuffer + sizeof(LoRaPacketHeader), header->payloadLen);
            }
            
            packet.rssi = rssi;
            packet.snr = snr;
            packet.timestamp = timestamp;
            
            // Send to MQTT task via queue
            if (xQueueSend(rxPacketQueue, &packet, pdMS_TO_TICKS(100)) != pdTRUE) {
                Serial.println("⚠️  Queue full, packet dropped!");
                packetsDropped++;
            } else {
                Serial.println("✅ Packet queued for MQTT");
            }
            
            // Send ACK if this is a readings/status/event message
            if (header->msgType == MSG_READINGS ||
                header->msgType == MSG_STATUS ||
                header->msgType == MSG_EVENT) {
                sendAck(header->deviceId, header->sequenceNum, true, rssi, snr);
            }

            // Resume RX after reading a packet matches
            // RadioLib readData() puts radio in standby. We must restart RX.
            radio->startReceive();
             } 
             else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
                 Serial.println("Rx CRC error");
                 radio->startReceive();
             }
             else {
                 Serial.printf("Rx Error or false alarm: %d\n", state);
                 radio->startReceive();
             }
        } else {
            // Yield to avoid starving other tasks / triggering watchdog
            // For high responsiveness, we can reduce this, but 10ms is usually fine.
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        
        // Print statistics every 60 seconds
        static uint32_t lastStats = 0;
        if (millis() - lastStats > 60000) {
            lastStats = millis();
            Serial.printf("\n[Stats] RX: %d, Dropped: %d, Duplicates: %d\n",
                         packetsReceived, packetsDropped, duplicatesFiltered);
        }
    }
}

/**
 * Send ACK to sensor
 */
bool sendAck(uint64_t deviceId, uint16_t seqNum, bool success, int8_t rssi, int8_t snr) {
    if (radio == nullptr) {
        return false;
    }
    
    // Build ACK payload
    AckPayload ack;
    ack.ackSequenceNum = seqNum;
    ack.success = success ? 1 : 0;
    ack.errorCode = 0;
    ack.rssi = rssi;
    ack.snr = snr;
    ack.reserved = 0;
    
    // Build packet header
    LoRaPacketHeader header;
    initHeader(&header, MSG_ACK, gatewayId, 0, sizeof(AckPayload));
    
    // Combine header + payload
    uint8_t txBuffer[sizeof(LoRaPacketHeader) + sizeof(AckPayload)];
    memcpy(txBuffer, &header, sizeof(LoRaPacketHeader));
    memcpy(txBuffer + sizeof(LoRaPacketHeader), &ack, sizeof(AckPayload));
    
    // Transmit ACK (briefly stop RX mode)
    Serial.printf("[LoRa TX] Sending ACK for seq %d... ", seqNum);
    
    int state = radio->transmit(txBuffer, sizeof(txBuffer));
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("✅");
        // Restart receiving
        radio->startReceive();
        return true;
    } else {
        Serial.printf("❌ (code: %d)\n", state);
        // Restart receiving even on failure
        radio->startReceive();
        return false;
    }
}

/**
 * Send command to sensor
 */
bool sendCommand(uint64_t deviceId, const CommandPayload* cmd) {
    if (radio == nullptr) {
        return false;
    }
    
    // Build packet header
    LoRaPacketHeader header;
    uint8_t payloadLen = sizeof(uint8_t) + sizeof(uint8_t) + cmd->paramLen;
    initHeader(&header, MSG_COMMAND, gatewayId, 0, payloadLen);
    
    // Combine header + payload
    uint8_t txBuffer[sizeof(LoRaPacketHeader) + payloadLen];
    memcpy(txBuffer, &header, sizeof(LoRaPacketHeader));
    memcpy(txBuffer + sizeof(LoRaPacketHeader), cmd, payloadLen);
    
    Serial.printf("[LoRa TX] Sending command 0x%02X to device 0x%016llX... ", cmd->cmdType, deviceId);
    
    int state = radio->transmit(txBuffer, sizeof(LoRaPacketHeader) + payloadLen);
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("✅");
        // Restart receiving
        radio->startReceive();
        return true;
    } else {
        Serial.printf("❌ (code: %d)\n", state);
        // Restart receiving even on failure
        radio->startReceive();
        return false;
    }
}

/**
 * Get packet queue handle (used by MQTT task)
 */
QueueHandle_t getPacketQueue() {
    return rxPacketQueue;
}
