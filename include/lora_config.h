#ifndef LORA_CONFIG_H
#define LORA_CONFIG_H

// ====================================================================
// LoRa Radio Configuration (must match sensor nodes)
// ====================================================================

// Frequency Configuration
#define LORA_FREQUENCY      915.0   // MHz (US: 915, EU: 868)

// Radio Parameters (must match sensor settings exactly)
#define LORA_BANDWIDTH      125.0   // kHz
#define LORA_SPREADING      9       // SF7-SF12
#define LORA_CODING_RATE    7       // 4/7
#define LORA_TX_POWER       14      // dBm (for sending ACKs/commands)
#define LORA_PREAMBLE_LEN   8       // Standard preamble
#define LORA_SYNC_WORD      0x12    // Private network sync word

// Gateway operates in continuous RX mode
#define LORA_RX_MODE_CONTINUOUS true

// CRC and Error Detection
#define LORA_CRC_ENABLED    true

#endif // LORA_CONFIG_H
