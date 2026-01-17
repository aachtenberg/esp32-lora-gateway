# PostgreSQL Database Integration

## Status

**Current:** Database integration implemented and **enabled by default**
**Mode:** Dual-write mode (JSON registry + database)
**API:** REST API wrapper around PostgreSQL (default: http://192.168.0.167:3000/api)

## Architecture

The gateway uses an HTTP REST API to write to PostgreSQL instead of direct database connections. This is more reliable and simpler for ESP32 hardware.

### How It Works

```
ESP32 Gateway → HTTP POST → REST API → PostgreSQL
     ↓
  (if DB unavailable, queues writes in RAM)
     ↓
  (continues LoRa operations normally)
```

## Setup

### 1. Database Integration Status

Database integration is **enabled by default** in `src/database_manager.cpp`:

```cpp
#define DB_API_ENABLED true
```

To disable database integration, change this to `false`.

### 2. Set API URL

Default is `http://192.168.0.167:3000/api`. To change, add to `platformio.ini`:

```ini
build_flags =
    # ... existing flags ...
    -D DB_API_URL=\"http://your-api-server:port/api\"
```

### 3. Create REST API Service

You need to create a simple Node.js/Python/Go service that:
- Accepts POST requests to `/api/devices`, `/api/packets`, `/api/commands`, `/api/events`
- Writes JSON payloads to PostgreSQL
- Returns HTTP 200 on success

See `docs/API_SPEC.md` for endpoint specifications (to be created).

### 4. Database Schema

Run the schema from `docs/ARCHITECTURE.md` to create required tables:
- `devices` - Device registry
- `packets` - Packet history
- `commands` - Command queue and history
- `events` - Event logs

## Graceful Degradation

The gateway will:
- ✅ Check `/api/health` on startup
- ✅ Continue LoRa operations if database unavailable
- ✅ Queue writes in RAM (max 1000 entries)
- ✅ Retry connection every 30 seconds
- ✅ Process queued writes when reconnected

## Monitoring

Serial output shows database status:

```
[DB] Initializing database manager (REST API mode)
[DB] API URL: http://192.168.0.167:3000/api
[DB] Connection attempt 1...
✅ Database API connected
[DB] Processing 0 queued writes
```

Or if unavailable:

```
⚠️  Database API unavailable (HTTP 0), continuing without persistence
```

## Current Behavior

With `DB_API_ENABLED true` (default):
- Database manager attempts connection to API on startup
- Queues writes in RAM if API unavailable
- Retries every 30 seconds
- LoRa operations continue normally regardless of database status

With `DB_API_ENABLED false`:
- Database manager is disabled
- No network calls made
- No performance impact
- JSON registry still works normally

## Next Steps

1. Create REST API service (Node.js recommended)
2. Test database schema
3. Enable `DB_API_ENABLED`
4. Test with database available and unavailable
5. Monitor performance and queue depth
6. Eventually deprecate JSON registry

## API Endpoints (Draft)

### POST /api/health
Returns 200 if API is healthy

### POST /api/devices
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

### POST /api/packets
```json
{
  "device_id": "12345678",
  "gateway_id": "gateway_main",
  "msg_type": 1,
  "sequence_num": 156,
  "rssi": -85,
  "snr": 8,
  "payload": { /* sensor data */ }
}
```

### POST /api/commands
```json
{
  "device_id": "12345678",
  "command_type": 1,
  "parameters": "90",
  "status": "pending"
}
```

### POST /api/events
```json
{
  "device_id": "12345678",
  "event_type": 1,
  "severity": 1,
  "message": "Low battery warning"
}
```

## Performance Notes

- HTTP requests are async (non-blocking)
- Queue processes 10 writes per loop iteration
- Each write has 5-second timeout
- Failed writes increment `failedWrites` counter
- Health check runs every 60 seconds

## Files Modified

- `platformio.ini` - Added HTTPClient dependency (already included)
- `src/database_manager.h` - Database manager interface
- `src/database_manager.cpp` - REST API implementation
- `src/device_registry.cpp` - Dual-write on device updates
- `src/main.cpp` - Initialize and loop database manager
- `.env` - PostgreSQL credentials (not used directly by ESP32)
- `docs/ARCHITECTURE.md` - Full architecture documentation
