# Telegraf Integration Specification for ESP32 LoRa Gateway

## Overview

ESP32 gateway will POST JSON payloads to Telegraf HTTP listener endpoints. Telegraf writes to existing PostgreSQL database.

## ⚠️ Important Limitation

**Telegraf's PostgreSQL output plugin creates its own table structure** based on the measurement name (`http_listener_v2`) rather than routing to custom tables. All data goes into a single Telegraf-managed table with tags and fields columns.

**Current Behavior:**
- Data is successfully written to PostgreSQL ✅
- Goes to `http_listener_v2` table (Telegraf's default) instead of custom tables
- JSON payload is stored as JSONB fields

**Options:**
1. **Use Telegraf's table** - Query the single table, data is accessible
2. **Add PostgreSQL triggers** - Auto-copy data from Telegraf table to custom tables
3. **Use Execd processor** - Custom script to route data before database write
4. **Write custom API service** - Bypass Telegraf, direct PostgreSQL writes with custom logic

## Requirements

### Telegraf Configuration

**Listen Address:** Port 3000 (or configure via `DB_API_URL` in ESP32)  
**Input Plugin:** `http_listener_v2`  
**Output Plugin:** `postgresql`

### Database Connection

```
Host: 192.168.0.167 (or sre-postgres via Docker internal network)
Port: 5432
Database: iot_sensors
User: sre_agent
Password: ${SRE_POSTGRES_PASSWORD} (via environment variable)
```

## HTTP Endpoints

### 1. Health Check
```
GET /api/health
Response: 200 OK (no body required)
```

### 2. Device Updates
```
POST /api/devices
Content-Type: application/json
```

**Payload:**
```json
{
  "device_id": "12345678",
  "name": "sensor_01",
  "location": "Living Room",
  "last_rssi": -85,
  "last_snr": 8,
  "packet_count": 42,
  "last_sequence": 156,
  "sensor_interval": 90,
  "deep_sleep_sec": 60
}
```

**Table:** `devices`  
**Operation:** UPSERT on `device_id`

### 3. Packet History
```
POST /api/packets
Content-Type: application/json
```

**Payload:**
```json
{
  "device_id": "12345678",
  "gateway_id": "gateway_main",
  "msg_type": 1,
  "sequence_num": 156,
  "rssi": -85,
  "snr": 8,
  "payload": {
    "temperature": 22.5,
    "humidity": 65,
    "pressure": 1013.25
  }
}
```

**Table:** `packets`  
**Operation:** INSERT

### 4. Commands
```
POST /api/commands
Content-Type: application/json
```

**Payload:**
```json
{
  "device_id": "12345678",
  "command_type": 1,
  "parameters": "90",
  "status": "pending"
}
```

**Table:** `commands`  
**Operation:** INSERT

### 5. Events
```
POST /api/events
Content-Type: application/json
```

**Payload:**
```json
{
  "device_id": "12345678",
  "event_type": 1,
  "severity": 1,
  "message": "Low battery warning"
}
```

**Table:** `events`  
**Operation:** INSERT

## Database Schema

```sql
-- Device Registry (UPSERT)
CREATE TABLE devices (
    device_id BIGINT PRIMARY KEY,
    name VARCHAR(64) NOT NULL,
    location VARCHAR(64),
    last_seen TIMESTAMP DEFAULT NOW(),
    last_rssi SMALLINT,
    last_snr SMALLINT,
    packet_count INTEGER DEFAULT 0,
    last_sequence INTEGER,
    sensor_interval SMALLINT,
    deep_sleep_sec SMALLINT,
    created_at TIMESTAMP DEFAULT NOW(),
    updated_at TIMESTAMP DEFAULT NOW()
);

CREATE INDEX idx_devices_last_seen ON devices(last_seen DESC);

-- Packet History (INSERT)
CREATE TABLE packets (
    id SERIAL PRIMARY KEY,
    device_id BIGINT REFERENCES devices(device_id),
    received_at TIMESTAMP DEFAULT NOW(),
    gateway_id VARCHAR(32),
    msg_type SMALLINT NOT NULL,
    sequence_num INTEGER NOT NULL,
    rssi SMALLINT,
    snr SMALLINT,
    payload JSONB,
    time_on_air SMALLINT,
    tx_power SMALLINT
);

CREATE INDEX idx_packets_device_time ON packets(device_id, received_at DESC);
CREATE INDEX idx_packets_received ON packets(received_at DESC);

-- Command Queue (INSERT)
CREATE TABLE commands (
    id SERIAL PRIMARY KEY,
    device_id BIGINT REFERENCES devices(device_id),
    command_type SMALLINT NOT NULL,
    parameters JSONB,
    status VARCHAR(20) NOT NULL,
    created_at TIMESTAMP DEFAULT NOW(),
    sent_at TIMESTAMP,
    confirmed_at TIMESTAMP,
    retry_count INTEGER DEFAULT 0,
    error_message TEXT
);

CREATE INDEX idx_commands_device_status ON commands(device_id, status);

-- Event Logs (INSERT)
CREATE TABLE events (
    id SERIAL PRIMARY KEY,
    device_id BIGINT REFERENCES devices(device_id),
    event_type SMALLINT NOT NULL,
    severity SMALLINT NOT NULL,
    message TEXT,
    received_at TIMESTAMP DEFAULT NOW()
);

CREATE INDEX idx_events_device_time ON events(device_id, received_at DESC);
```

## Current Telegraf Configuration

```toml
# HTTP Listener Input
[[inputs.http_listener_v2]]
  service_address = ":3000"
  paths = ["/api/devices", "/api/packets", "/api/commands", "/api/events", "/api/health"]
  methods = ["POST", "GET"]
  data_format = "json"
  
  # Tag with endpoint path for routing
  [inputs.http_listener_v2.tags]
    source = "esp32_gateway"

# PostgreSQL Output
[[outputs.postgresql]]
  address = "host=192.168.0.167 port=5432 user=sre_agent password=3KNXAdSdHxtqzA4mhOjz5nJRoPuxM9yjEIn/05jeKRk= dbname=iot_sensors sslmode=disable"
  
  # Connection pool
  max_connections = 5
  
  # Retry settings
  retry_policy = "exponential"
  retry_max_attempts = 3
```

**Actual Table Structure Created by Telegraf:**

```sql
-- Telegraf creates this table automatically
CREATE TABLE http_listener_v2 (
    time TIMESTAMP NOT NULL,
    tags JSONB,
    fields JSONB
);

-- Data example:
-- time: 2026-01-16 10:30:45
-- tags: {"source": "esp32_gateway", "path": "/api/devices"}
-- fields: {"device_id": "12345678", "name": "sensor_01", "last_rssi": -85, ...}
```

## Workaround Options

### Option 1: Query Telegraf's Table Directly

Accept Telegraf's structure and query it:

```sql
-- Get all device records
SELECT 
    time,
    fields->>'device_id' as device_id,
    fields->>'name' as name,
    fields->>'location' as location,
    fields->>'last_rssi' as last_rssi
FROM http_listener_v2
WHERE tags->>'path' = '/api/devices'
ORDER BY time DESC;

-- Get packet history
SELECT 
    time,
    fields->>'device_id' as device_id,
    fields->>'rssi' as rssi,
    fields->>'snr' as snr,
    fields->'payload' as payload
FROM http_listener_v2
WHERE tags->>'path' = '/api/packets'
ORDER BY time DESC;
```

### Option 2: PostgreSQL Triggers (Recommended for Production)

Add triggers to auto-populate custom tables from Telegraf's table:

```sql
-- Function to route device updates
CREATE OR REPLACE FUNCTION route_device_data()
RETURNS TRIGGER AS $$
BEGIN
    IF NEW.tags->>'path' = '/api/devices' THEN
        INSERT INTO devices (
            device_id, name, location, last_rssi, last_snr, 
            packet_count, last_sequence, sensor_interval, deep_sleep_sec,
            last_seen, updated_at
        ) VALUES (
            (NEW.fields->>'device_id')::BIGINT,
            NEW.fields->>'name',
            NEW.fields->>'location',
            (NEW.fields->>'last_rssi')::SMALLINT,
            (NEW.fields->>'last_snr')::SMALLINT,
            (NEW.fields->>'packet_count')::INTEGER,
            (NEW.fields->>'last_sequence')::INTEGER,
            (NEW.fields->>'sensor_interval')::SMALLINT,
            (NEW.fields->>'deep_sleep_sec')::SMALLINT,
            NEW.time,
            NEW.time
        )
        ON CONFLICT (device_id) DO UPDATE SET
            name = EXCLUDED.name,
            location = EXCLUDED.location,
            last_rssi = EXCLUDED.last_rssi,
            last_snr = EXCLUDED.last_snr,
            packet_count = EXCLUDED.packet_count,
            last_sequence = EXCLUDED.last_sequence,
            sensor_interval = EXCLUDED.sensor_interval,
            deep_sleep_sec = EXCLUDED.deep_sleep_sec,
            last_seen = EXCLUDED.last_seen,
            updated_at = EXCLUDED.updated_at;
    END IF;
    
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Trigger on Telegraf table
CREATE TRIGGER route_device_trigger
AFTER INSERT ON http_listener_v2
FOR EACH ROW
EXECUTE FUNCTION route_device_data();

-- Similar triggers for packets, commands, events...
```

### Option 3: Telegraf Execd Processor (More Complex)

Use Telegraf's execd processor plugin to run a custom script that routes data:

```toml
[[processors.execd]]
  command = ["/usr/local/bin/route-data.py"]
  data_format = "json"
```

Custom script routes based on path and writes to correct tables directly.

## Expected Behavior

1. **Devices Table:**
   - UPSERT (INSERT or UPDATE) based on `device_id`
   - Update `last_seen` timestamp automatically
   - Update `updated_at` timestamp automatically

2. **Packets Table:**
   - INSERT only (append history)
   - Auto-generate `id` (SERIAL)
   - Set `received_at` timestamp automatically
   - Store full sensor data in `payload` JSONB column

3. **Commands Table:**
   - INSERT only
   - Set `created_at` timestamp automatically

4. **Events Table:**
   - INSERT only
   - Set `received_at` timestamp automatically

## Traffic Volume

- **Typical:** 1-5 devices × 1 packet/minute = ~5 requests/minute
- **Peak:** 50 devices × 1 packet/minute = ~50 requests/minute
- **Size:** ~500 bytes per request

## Error Handling

ESP32 will:
- Timeout after 5 seconds
- Queue failed writes (max 1000 in RAM)
- Retry every 30 seconds
- Continue LoRa operations regardless of DB status

Telegraf should:
- Return HTTP 200 for successful writes
- Return HTTP 5xx for database errors
- Buffer writes if PostgreSQL unavailable

## Testing

**Health check:**
```bash
curl http://192.168.0.167:3000/api/health
# Should return 200 OK
```

**Test device write:**
```bash
curl -X POST http://192.168.0.167:3000/api/devices \
  -H "Content-Type: application/json" \
  -d '{"device_id":"12345678","name":"test","location":"test","last_rssi":-85,"last_snr":8,"packet_count":1,"last_sequence":1,"sensor_interval":90,"deep_sleep_sec":60}'
```

**Verify in PostgreSQL (Telegraf's table):**
```sql
-- Check data arrived
SELECT * FROM http_listener_v2 
WHERE tags->>'path' = '/api/devices' 
ORDER BY time DESC LIMIT 5;

-- If triggers are installed, check custom table
SELECT * FROM devices WHERE device_id = 12345678;
```

## Current Status

✅ **Working:** Data successfully writing to PostgreSQL via Telegraf  
⚠️ **Limitation:** All data goes to `http_listener_v2` table, not custom tables  
🔄 **Next Step:** Choose routing strategy (triggers recommended)

## Recommendation

**For immediate use:** Query `http_listener_v2` table directly using JSONB operators  
**For production:** Implement PostgreSQL triggers to auto-populate custom tables  

Triggers allow:
- Telegraf continues working as-is (no config changes)
- ESP32 code requires no changes
- Data appears in both Telegraf table AND custom tables
- Custom tables can be queried with normal SQL
- Grafana/tools can use either table structure

## Notes

- ESP32 POSTs JSON with `Content-Type: application/json`
- Telegraf timestamps each record on receipt
- JSONB fields store entire JSON payload
- Path is tagged (e.g., `/api/devices`, `/api/packets`)
- Can filter by path: `WHERE tags->>'path' = '/api/devices'`
- Device IDs are unsigned 64-bit integers represented as strings in JSON
- ESP32 database integration currently disabled (`DB_API_ENABLED false`)
- Enable after choosing table routing strategy
