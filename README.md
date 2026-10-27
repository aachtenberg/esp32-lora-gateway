# ESP32 LoRa Gateway

LoRa-to-MQTT bridge for environmental sensor network.

## Hardware

- **Board**: Heltec WiFi LoRa 32 V3 (ESP32-S3) with built-in SX1262
- **LoRa**: SX1262 LoRa module (863-928 MHz) - built-in
- **Display**: 0.96" OLED SSD1306 (I2C) - built-in
- **Power**: Mains powered (USB or 5V adapter)

## Features

- **Dual-core architecture**: Core 0 for LoRa RX (low latency), Core 1 for MQTT/WiFi
- **LoRa peer-to-peer receiver**: Continuous RX mode for sensor packets
- **MQTT bridge**: Converts binary LoRa packets to JSON MQTT messages
- **Command retry mechanism**: Persistent queue automatically retries commands until sensor receives them
- **Command history**: Every command is recorded in PostgreSQL and tracked through `queued → sent → confirmed` (or `timeout` / `superseded`)
- **Web admin dashboard**: Live readings, link quality, device health and remote control for every sensor, plus event and command history (HTTP Basic auth)
- **Device auto-discovery**: Sensors automatically register with name and location
- **Multi-sensor support**: Track up to 10 sensors simultaneously
- **Device registry**: Auto-updates device names and locations from sensor status messages
- **Packet deduplication**: Per-device circular buffer (50 entries) prevents duplicate messages
- **Thread-safe radio access**: FreeRTOS mutex prevents RX/TX conflicts
- **WiFiManager**: Web portal for WiFi/MQTT configuration
- **OTA updates**: Over-the-air firmware updates
- **OLED status display**: Shows connected sensors, packet counts, WiFi/MQTT status

## MQTT Format (Backward Compatible)

Preserves existing `esp-sensor-hub` topic structure:

### Topics

```
esp-sensor-hub/AABBCCDDEEFF0011/readings   # Sensor data (full 64-bit device ID)
esp-sensor-hub/AABBCCDDEEFF0011/status     # Health metrics and device info
esp-sensor-hub/AABBCCDDEEFF0011/events     # System events (startup, errors)
lora/command                                # Commands (publish here)
lora/command/ack                            # Command acknowledgments
lora/gateway/status                         # Gateway health
```

### Readings JSON

```json
{
  "device_id": "AABBCCDDEEFF0011",
  "device_name": "BME280-LoRa-001",
  "location": "Office",
  "timestamp": 1234567890,
  "sequence": 123,
  "temperature": 25.31,
  "humidity": 55.2,
  "pressure": 1013.25,
  "altitude": 120,
  "battery_voltage": 3.7,
  "battery_percent": 85,
  "pressure_change": -50,
  "pressure_trend": 0,
  "rssi": -85,
  "snr": 8.5,
  "gateway_time": 1234567890
}
```

### Status JSON

```json
{
  "device_id": "AABBCCDDEEFF0011",
  "device_name": "BME280-LoRa-001",
  "location": "Office",
  "uptime": 3600,
  "wake_count": 24,
  "sensor_healthy": true,
  "lora_rssi": -85,
  "lora_snr": 8,
  "free_heap_kb": 128,
  "sensor_failures": 0,
  "tx_failures": 0,
  "last_success_tx": 1234567890,
  "deep_sleep_sec": 90,
  "rssi": -85,
  "snr": 8.5
}
```

### Events JSON

```json
{
  "device_id": "AABBCCDDEEFF0011",
  "device_name": "BME280-LoRa-001",
  "location": "Office",
  "event_type": 1,
  "severity": 0,
  "message": "Device startup - wake count: 1",
  "timestamp": 1234567890
}
```

**Event Types:**
- `1` = Startup, `2` = Shutdown
- `16` = Sensor error, `17` = LoRa error
- `32` = Config change, `33` = Restart

**Severity:**
- `0` = Info, `1` = Warning, `2` = Error, `3` = Critical

## Pin Configuration

See [include/device_config.h](include/device_config.h) for complete pin assignments.

**Heltec WiFi LoRa 32 V3 (ESP32-S3):**
```
LoRa SX1262 (SPI - built-in):
  MISO = 11, MOSI = 10, SCK = 9
  NSS = 8, DIO1 = 14, BUSY = 13, RST = 12

OLED Display (I2C - built-in):
  SDA = 17, SCL = 18, RST = 21
  OLED addr = 0x3C

Vext Control: GPIO 36 (LOW = peripherals ON)
```

## Quick Start

### 1. Configure Secrets

Copy and edit the secrets file:

```bash
cd ~/repos/esp32-lora-gateway
cp include/secrets.h.example include/secrets.h
nano include/secrets.h
```

Fill in your MQTT broker details and the dashboard login:
```cpp
#define MQTT_SERVER "192.168.1.100"
#define MQTT_PORT 1883
#define MQTT_USER "your-username"      // Optional
#define MQTT_PASSWORD "your-password"  // Optional

// Web dashboard login (leave both undefined for an open dashboard)
#define WEB_AUTH_USER "admin"
#define WEB_AUTH_PASS "change-me"
```

### 2. Build and Upload

```bash
pio run -e esp32-lora-gateway-usb -t upload   # First flash over USB (/dev/ttyUSB0)
pio run -t upload                             # Later updates over the air (esp32-lora-gateway.local)
pio run -t uploadfs                           # Upload filesystem (device registry)
```

The build reads `PG_*` from the environment for the `-D PG_*` flags; set placeholders if you do not use them.

### 3. Configure WiFi

On first boot, the gateway creates a WiFi access point:

1. Connect to `ESP32-Gateway` WiFi network
2. Browser opens automatically (or go to 192.168.4.1)
3. Click "Configure WiFi"
4. Select your network and enter password
5. Click "Save"

The gateway will connect and remember your WiFi credentials.

### 4. Add Sensors

Sensors automatically register when they send their first status message. The gateway extracts the device name and location from the status payload and updates the registry.

**Sensor sends:**
- Device name from `/data/device_name.txt` (e.g., "BME280-LoRa-001")
- Location from config or GPS (future feature)

**Gateway automatically:**
- Creates registry entry with full 64-bit device ID
- Updates MQTT topics: `esp-sensor-hub/AABBCCDDEEFF0011/*`
- Includes friendly name in all JSON messages

**Manual override** (optional):

Edit `/data/sensor_registry.json` and re-upload filesystem:

```json
{
  "devices": [
    {
      "id": "AABBCCDDEEFF0011",
      "name": "Kitchen-BME280",
      "location": "Kitchen"
    }
  ]
}
```

## Architecture

### Dual-Core Design

**Core 0** (LoRa RX Task):
- Continuous LoRa receive mode
- Interrupt-driven packet reception
- Packet validation and deduplication (per-device, 50-entry circular buffer)
- Clear deduplication buffer on sensor restart (EVENT_STARTUP)
- Push valid packets to queue
- Thread-safe radio access with FreeRTOS mutex

**Core 1** (MQTT Task):
- Pop packets from queue
- Trigger command retry for active sensor (RX window open)
- Convert binary → JSON
- Auto-update device registry from status messages
- Publish to MQTT broker with full 64-bit device IDs
- Handle incoming MQTT commands with persistent retry queue
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

### Command Flow (with Retry Mechanism)

```
MQTT command (lora/command) or dashboard POST /api/command
    ↓
Gateway queues command (30 min expiration)        → history row: queued
    ↓
Sensor transmits data packet
    ↓
Gateway waits ~3 s for the sensor's RX window, then transmits
    ↓
Sensor receives during RX window and executes      → history row: sent
    ↓
Sensor's next status report echoes the new value   → history row: confirmed
```

**Key features:**
- Commands persist in the RAM queue until sent or expired (30 minutes → `timeout`)
- Automatic retry on every sensor transmission; the radio is only ever driven from the MQTT task
- Queuing the same command type again for a sensor replaces the pending one (`superseded`)
- Sensor only listens briefly after each transmission
- Thread-safe radio and queue access with FreeRTOS mutexes

**Command history lifecycle** (PostgreSQL `commands` table, shown on the dashboard):

| Status | Meaning |
|---|---|
| `queued` | Accepted by the gateway, waiting for the sensor's RX window |
| `failed` | Rejected by the gateway (queue full) |
| `sent` | Transmitted over LoRa (`sent_at`, `retry_count`) |
| `confirmed` | A later status report echoed the new sleep / interval / name, or a startup event followed a restart |
| `timeout` | Not sent within 30 minutes |
| `superseded` | Replaced by a newer command of the same type |

Sensors do not acknowledge commands, so `status` requests and calibration commands stop at `sent`.

## Supported Commands

Send commands via MQTT to topic: `lora/command`

Commands use JSON format with the following structure:

```json
{
  "device_id": "AABBCCDDEEFF0011",   // Full 64-bit device ID (16 hex chars)
  "action": "set_interval",           // Command type
  "value": 90                         // Optional value (for set_interval, set_sleep, set_baseline)
}
```

**Important:** Use the full 16-character device ID, not the shortened version.

### Available Actions

| Action | Value | Description | Example |
|--------|-------|-------------|---------|
| `set_interval` | Integer (seconds) | Set sensor reading interval (5-3600s) | `{"device_id":"AABBCCDDEEFF0011","action":"set_interval","value":90}` |
| `set_sleep` | Integer (seconds) | Set deep sleep duration (0-3600s) | `{"device_id":"AABBCCDDEEFF0011","action":"set_sleep","value":900}` |
| `calibrate` | None | Set current pressure as baseline | `{"device_id":"AABBCCDDEEFF0011","action":"calibrate"}` |
| `set_baseline` | Float (hPa) | Set specific pressure baseline (900-1100 hPa) | `{"device_id":"AABBCCDDEEFF0011","action":"set_baseline","value":1013.25}` |
| `clear_baseline` | None | Disable pressure baseline tracking | `{"device_id":"AABBCCDDEEFF0011","action":"clear_baseline"}` |
| `status` | None | Request immediate status update | `{"device_id":"AABBCCDDEEFF0011","action":"status"}` |
| `restart` | None | Restart sensor device | `{"device_id":"AABBCCDDEEFF0011","action":"restart"}` |

### Using the Command Script (Recommended)

The `lora-cmd.sh` script provides a convenient interface for sending commands.

**Quick Setup:**
```bash
# 1. Copy the example configuration
cp .env.example .env

# 2. Edit with your MQTT broker settings
nano .env

# 3. Run commands
./lora-cmd.sh interval 90
```

**Basic Usage:**
```bash
./lora-cmd.sh interval 90         # Set interval to 90 seconds
./lora-cmd.sh sleep 900           # Set sleep to 15 minutes
./lora-cmd.sh calibrate           # Set current pressure as baseline
./lora-cmd.sh baseline 1013.25    # Set baseline to 1013.25 hPa
./lora-cmd.sh clear_baseline      # Clear baseline tracking
./lora-cmd.sh status              # Request status
./lora-cmd.sh restart             # Restart sensor
```

**Advanced Usage:**
```bash
# Target different sensors
./lora-cmd.sh -d AABBCCDDEEFF0011 interval 60

# Override broker from .env
./lora-cmd.sh -b 192.168.1.100 interval 120

# With authentication
./lora-cmd.sh -u user -P password interval 90

# Show help
./lora-cmd.sh --help
```

**Configuration Priority:**
1. Command-line options (highest priority)
2. Environment variables
3. `.env` file
4. Built-in defaults (lowest priority)

### Manual Command Examples (mosquitto_pub)

```bash
# Set reading interval to 90 seconds
mosquitto_pub -h YOUR_BROKER -t "lora/command" \
  -m '{"device_id":"AABBCCDDEEFF0011","action":"set_interval","value":90}'

# Set deep sleep to 15 minutes
mosquitto_pub -h YOUR_BROKER -t "lora/command" \
  -m '{"device_id":"AABBCCDDEEFF0011","action":"set_sleep","value":900}'

# Calibrate pressure baseline (current reading)
mosquitto_pub -h YOUR_BROKER -t "lora/command" \
  -m '{"device_id":"AABBCCDDEEFF0011","action":"calibrate"}'

# Set specific pressure baseline
mosquitto_pub -h YOUR_BROKER -t "lora/command" \
  -m '{"device_id":"AABBCCDDEEFF0011","action":"set_baseline","value":1013.25}'

# Clear pressure baseline
mosquitto_pub -h YOUR_BROKER -t "lora/command" \
  -m '{"device_id":"AABBCCDDEEFF0011","action":"clear_baseline"}'

# Request status update
mosquitto_pub -h YOUR_BROKER -t "lora/command" \
  -m '{"device_id":"AABBCCDDEEFF0011","action":"status"}'

# Restart sensor
mosquitto_pub -h YOUR_BROKER -t "lora/command" \
  -m '{"device_id":"AABBCCDDEEFF0011","action":"restart"}'
```

### Command Acknowledgments

When the gateway receives and processes a command, it publishes an acknowledgment to `lora/command/ack`:

```json
{
  "device_id": "AABBCCDDEEFF0011",
  "action": "set_interval",
  "status": "queued"
}
```

**Status values:**
- `queued`: Command queued for retry (will be sent when sensor opens RX window)
- Command will be retried automatically on every sensor transmission
- Commands expire after 30 minutes if not sent

The MQTT ack only says the gateway accepted the command. Delivery is tracked in the command history (see Command Flow above and the dashboard).

## Web Admin Dashboard

The gateway serves an admin dashboard on port 80 (`http://esp32-lora-gateway.local/`). It requires the HTTP Basic credentials from `secrets.h` when `WEB_AUTH_USER` / `WEB_AUTH_PASS` are defined.

**Sidebar:** WiFi, MQTT broker and database API status, IP, signal, uptime, free memory, LoRa radio parameters, firmware version, online/total sensors.

**Sensor cards:** last readings (temperature, humidity, pressure with trend, battery, GPS with map link), device health from the last status report, link quality, packet counts, the live command queue, and controls: request status, restart, calibrate / set / clear pressure baseline (BME280), deep sleep, sensor interval, rename, remove from registry. A sensor is ONLINE when heard within twice its reporting period; one not heard since the gateway booted says so.

**Command History** and **Recent Events** come from the database API. The gateway refreshes them from its main loop while a dashboard is open, so no HTTP request is made from the web server's own task.

**API** (same auth):

| Route | Purpose |
|---|---|
| `GET /api/gateway` | Gateway status JSON |
| `GET /api/devices` | Registry snapshot with readings, health, `online`, queue |
| `GET /api/events`, `GET /api/commands` | Cached history from the database API |
| `POST /api/command` | `{"device_id":"<hex>","action":"set_interval","value":90}`; actions: `status`, `restart`, `calibrate`, `set_baseline`, `clear_baseline`, `set_sleep`, `set_interval`, `set_name` |
| `DELETE /api/device?id=<hex>` | Remove a device from the registry |

Everything shown from a device or the database is HTML-escaped; device names are remotely settable, so treat them as untrusted.

## Device Registry

Located at `/data/sensor_registry.json`:

```json
{
  "devices": [
    {
      "id": "AABBCCDDEEFF0011",
      "name": "BME280-LoRa-001",
      "location": "Office"
    }
  ]
}
```

**Auto-update:** Gateway automatically updates `name` and `location` when sensor sends status messages.

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
- Check I2C wiring (SDA=17, SCL=18, RST=21 on Heltec V3)
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
# Via PlatformIO (upload_port is already esp32-lora-gateway.local in platformio.ini)
pio run -t upload

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

## Shared Components

Both the sensor and gateway share:
- **Protocol library**: `lib/LoRaProtocol/lora_protocol.h` - Binary packet format definitions
- **LoRa configuration**: Must match exactly (frequency, spreading factor, bandwidth, sync word)
- **Hardware**: Heltec WiFi LoRa 32 V3 (ESP32-S3 with built-in SX1262)

## License

MIT License - see LICENSE file
