# ESP32 LoRa Gateway

LoRa-to-MQTT bridge for environmental sensor network.

## Hardware

- **ESP32**: ESP32 LX7 Dual-core
- **LoRa**: SX1262 LoRa module (863-928 MHz)
- **Display**: 0.96" OLED SSD1306 (I2C)
- **Power**: Mains powered (USB or 5V adapter)

## Features

- **Dual-core architecture**: Core 0 for LoRa RX (low latency), Core 1 for MQTT/WiFi
- **LoRa peer-to-peer receiver**: Continuous RX mode for sensor packets
- **MQTT bridge**: Converts binary LoRa packets to JSON MQTT messages
- **Multi-sensor support**: Track up to 10 sensors simultaneously
- **Device registry**: Maps LoRa device IDs to MQTT-friendly names
- **Packet deduplication**: Prevents duplicate messages
- **WiFiManager**: Web portal for WiFi/MQTT configuration
- **OTA updates**: Over-the-air firmware updates
- **OLED status display**: Shows connected sensors, packet counts, WiFi/MQTT status

## MQTT Format (Backward Compatible)

Preserves existing `esp-sensor-hub` topic structure:

### Topics

```
esp-sensor-hub/{device-name}/readings   # Sensor data
esp-sensor-hub/{device-name}/status     # Health metrics
esp-sensor-hub/{device-name}/events     # System events
esp-sensor-hub/{device-name}/command    # Commands (subscribe)
```

### Readings JSON

```json
{
  "device": "Kitchen-BME280",
  "chip_id": "AABBCCDDEEFF0011",
  "firmware_version": "2.0.0-build...",
  "schema_version": 1,
  "timestamp": 1234567890,
  "temperature_c": 25.31,
  "humidity_rh": 55.2,
  "pressure_pa": 101325,
  "pressure_hpa": 1013.25,
  "altitude_m": 120.5,
  "pressure_change_pa": -50,
  "pressure_trend": "falling",
  "baseline_hpa": 1013.75,
  "battery_voltage": 3.7,
  "battery_percent": 85
}
```

## Pin Configuration

See [include/device_config.h](include/device_config.h) for complete pin assignments.

**Critical pins** (verify against your module datasheet):
```
LoRa SX1262 (SPI):
  MISO = 19, MOSI = 27, SCK = 5
  NSS = 18, DIO1 = 26, BUSY = 23, RST = 14

OLED (I2C):
  SDA = 21, SCL = 22
  OLED addr = 0x3C
```

## Quick Start

### 1. Configure Secrets

Copy and edit the secrets file:

```bash
cd ~/repos/esp32-lora-gateway
cp include/secrets.h.example include/secrets.h
nano include/secrets.h
```

Fill in your MQTT broker details:
```cpp
#define MQTT_SERVER "192.168.1.100"
#define MQTT_PORT 1883
#define MQTT_USER "your-username"      // Optional
#define MQTT_PASSWORD "your-password"  // Optional
```

### 2. Build and Upload

```bash
pio run -t upload        # Upload firmware
pio run -t uploadfs      # Upload filesystem (device registry)
```

### 3. Configure WiFi

On first boot, the gateway creates a WiFi access point:

1. Connect to `ESP32-Gateway` WiFi network
2. Browser opens automatically (or go to 192.168.4.1)
3. Click "Configure WiFi"
4. Select your network and enter password
5. Click "Save"

The gateway will connect and remember your WiFi credentials.

### 4. Add Sensors

When a new sensor transmits, the gateway auto-detects it and creates a default name like `Unknown-AABBCCDD`.

To set a friendly name, publish an MQTT message:

```bash
mosquitto_pub -h YOUR_BROKER -t esp-sensor-hub/gateway/register \
  -m '{"device_id": "AABBCCDDEEFF0011", "name": "Kitchen-BME280"}'
```

Or edit `/data/sensor_registry.json` directly and re-upload filesystem.

## Architecture

### Dual-Core Design

**Core 0** (LoRa RX Task):
- Continuous LoRa receive mode
- Interrupt-driven packet reception
- Packet validation and deduplication
- Push valid packets to queue

**Core 1** (MQTT Task):
- Pop packets from queue
- Convert binary → JSON
- Publish to MQTT broker
- Handle incoming MQTT commands
- WiFi management
- OTA updates
- OLED display updates

**Inter-core communication**: FreeRTOS queue (thread-safe)

### Data Flow

```
Sensor (LoRa TX)
    ↓
Gateway Core 0 (LoRa RX)
    ↓ (validate, dedupe)
FreeRTOS Queue
    ↓
Gateway Core 1 (pop packet)
    ↓ (binary → JSON)
MQTT Broker
    ↓
Your monitoring system
```

### Command Flow

```
MQTT command → esp-sensor-hub/{device}/command
    ↓
Gateway Core 1 receives
    ↓ (lookup device ID)
Binary LoRa packet
    ↓
Gateway Core 0 transmits
    ↓
Sensor receives and executes
```

## Supported Commands

Send commands via MQTT to `esp-sensor-hub/{device-name}/command`:

| Command | Payload | Description |
|---------|---------|-------------|
| `calibrate` | (none) | Set pressure baseline to current reading |
| `baseline 1013.25` | Float (hPa) | Set specific pressure baseline |
| `clear_baseline` | (none) | Disable baseline tracking |
| `deepsleep 60` | Integer (sec) | Set deep sleep interval (0=disable) |
| `interval 30` | Integer (sec) | Set sensor read interval |
| `restart` | (none) | Restart sensor device |
| `status` | (none) | Request immediate status update |

## Device Registry

Located at `/data/sensor_registry.json`:

```json
{
  "devices": [
    {
      "device_id": "AABBCCDDEEFF0011",
      "name": "Kitchen-BME280",
      "last_seen": 1234567890,
      "rssi": -85,
      "snr": 8,
      "packet_count": 12345
    }
  ]
}
```

## Troubleshooting

### WiFi not connecting
- Reset WiFi settings: Press RESET button 3 times quickly
- Check WiFiManager portal timeout (default 3 minutes)
- Serial output shows connection status

### MQTT not connecting
- Check broker IP/hostname in `secrets.h`
- Verify broker is reachable: `ping YOUR_BROKER`
- Check username/password if broker requires auth
- Serial output shows MQTT reconnection attempts every 5 seconds

### LoRa not receiving
- Verify SPI pins match your module
- Check antenna connection
- Verify frequency matches sensors (915 MHz US, 868 MHz EU)
- Check LoRa settings match sensors exactly (SF, BW, etc.)
- Serial output shows LoRa initialization status

### Duplicate MQTT messages
- Gateway deduplicates based on sequence numbers
- Check `DEDUP_BUFFER_SIZE` in device_config.h (default 50)
- Old packets (>50 newer packets ago) may be duplicated

### Display not working
- Check I2C wiring (SDA=21, SCL=22)
- Run I2C scanner to verify 0x3C address
- Disable OLED with `-D OLED_ENABLED=0` in platformio.ini if not needed

## Development

### Project Structure

```
esp32-lora-gateway/
├── platformio.ini       # Build configuration
├── src/
│   ├── main.cpp         # Dual-core orchestration
│   ├── lora_receiver.*  # Core 0: LoRa RX
│   ├── mqtt_bridge.*    # Core 1: Binary→JSON→MQTT
│   ├── packet_queue.*   # Thread-safe queue
│   ├── device_registry.*# Sensor tracking
│   ├── wifi_manager.*   # WiFi setup
│   └── display_manager.*# OLED display
├── include/
│   ├── device_config.h  # Hardware pins
│   ├── lora_config.h    # Radio settings
│   ├── secrets.h        # MQTT/WiFi credentials
│   └── version.h        # Firmware version
├── lib/LoRaProtocol/    # Shared protocol library
└── data/                # SPIFFS filesystem
    └── sensor_registry.json
```

### Building

```bash
# Build
pio run

# Upload firmware
pio run -t upload

# Upload filesystem
pio run -t uploadfs

# Monitor serial output
pio device monitor

# Clean build
pio run -t clean
```

### OTA Updates

After initial USB flash, you can update over WiFi:

```bash
# Via PlatformIO
pio run -t upload --upload-port esp32-lora-gateway.local

# Or use Arduino IDE OTA
```

## Performance

- **LoRa RX latency**: <100ms from packet arrival to queue
- **MQTT publish latency**: <1 second from queue to broker
- **Max sensors**: 10 (configurable in device_config.h)
- **Packet throughput**: ~100 packets/minute with SF9
- **WiFi power**: No power save (low MQTT latency)

## Related Projects

- [esp32-lora-sensor](../esp32-lora-sensor/) - Sensor node with BME280

## License

MIT License - see LICENSE file
