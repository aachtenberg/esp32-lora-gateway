# ESP32 LoRa Gateway Architecture

## Overview

The ESP32 LoRa Gateway is an always-on, mains-powered device that bridges LoRa sensors to MQTT and provides a web-based management interface. It operates as a **stateless edge device** with optional PostgreSQL persistence for historical data.

## Current Architecture (As-Built)

### Component Overview

```
┌─────────────────────────────────────────────────────────────┐
│  ESP32 Gateway (Heltec WiFi LoRa 32 V3)                     │
│                                                               │
│  ┌───────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │  LoRa RX Task │  │  MQTT Bridge │  │  Web Server  │     │
│  │  (Core 0)     │  │  (Core 1)    │  │  (Core 1)    │     │
│  └───────┬───────┘  └──────┬───────┘  └──────┬───────┘     │
│          │                  │                  │              │
│          └──────────────────┼──────────────────┘              │
│                             ↓                                 │
│                  ┌──────────────────────┐                    │
│                  │  Device Registry     │                    │
│                  │  (In-Memory Array)   │                    │
│                  │  + Mutex Protection  │                    │
│                  └──────────┬───────────┘                    │
│                             │                                 │
│                             ↓                                 │
│                  ┌──────────────────────┐                    │
│                  │  LittleFS Storage    │                    │
│                  │  /sensor_registry.json│                   │
│                  └──────────────────────┘                    │
└─────────────────────────────────────────────────────────────┘
```

### Current Data Flow

```
Sensor → LoRa RX → Packet Queue → MQTT Bridge → MQTT Broker
                         ↓
                   Update Registry
                         ↓
                   Save JSON File
                         
Web Browser → AsyncWebServer → Read Registry (with mutex lock)
                                     ↓
                              Return JSON snapshot
```

### Current Limitations

1. **Mutex Contention**: Multiple tasks compete for registry lock
   - LoRa RX task (packet updates)
   - Web server (API requests)
   - Command sender (queue management)
   - MQTT bridge (publishing)

2. **Crude Persistence**: Entire JSON file rewritten on every change
   - Flash wear on LittleFS
   - No query capability
   - Manual lock management required

3. **Fixed Array Size**: `MAX_SENSORS = 50` hardcoded limit

4. **No Historical Data**: Only stores last-known state per device

5. **Lost Command Queue on Reboot**: Commands queued in RAM are lost

## Target Architecture (PostgreSQL Migration)

### Design Principles

1. **Database is Optional for Operation**: Gateway must function without database
2. **Device-as-Authority**: Sensors own their configuration
3. **Eventual Consistency**: Cache may be briefly stale
4. **Graceful Degradation**: Continue LoRa operations if DB unavailable
5. **Async Writes**: Never block LoRa operations waiting for database

### New Component Overview

```
┌─────────────────────────────────────────────────────────────┐
│  ESP32 Gateway                                               │
│                                                               │
│  ┌───────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │  LoRa RX Task │  │  MQTT Bridge │  │  Web Server  │     │
│  └───────┬───────┘  └──────┬───────┘  └──────┬───────┘     │
│          │                  │                  │              │
│          └──────────────────┼──────────────────┘              │
│                             ↓                                 │
│                  ┌──────────────────────┐                    │
│                  │  In-Memory Cache     │                    │
│                  │  (std::map)          │                    │
│                  │  No locks needed     │                    │
│                  └──────────┬───────────┘                    │
│                             │                                 │
│                             │ Async writes                    │
│                             ↓                                 │
│                  ┌──────────────────────┐                    │
│                  │  PostgreSQL Client   │                    │
│                  │  - Write queue       │                    │
│                  │  - Auto-reconnect    │                    │
│                  │  - Graceful failure  │                    │
│                  └──────────┬───────────┘                    │
└─────────────────────────────┼───────────────────────────────┘
                              │
                              │ TCP/IP over WiFi
                              ↓
                   ┌──────────────────────┐
                   │  PostgreSQL           │
                   │  (Docker Container)   │
                   │  Port: 5432          │
                   └──────────────────────┘
```

### Operational Modes

```cpp
enum DatabaseStatus {
    DB_CONNECTED,      // Normal operation, writes persisted
    DB_DISCONNECTED,   // Working without persistence
    DB_RECONNECTING    // Attempting to restore connection
};
```

**DB_CONNECTED Mode:**
- All sensor data written to PostgreSQL
- Command history persisted
- Full query capability available
- Historical data accumulates

**DB_DISCONNECTED Mode:**
- LoRa operations continue normally
- Data cached in RAM only
- Web UI serves cached data
- Writes queued for later retry
- Gateway attempts reconnection every 30s

**DB_RECONNECTING Mode:**
- Gateway syncing cached data to database
- New writes continue to queue
- Gradual recovery without blocking operations

### Data Persistence Strategy

**Always in RAM (Hot Cache):**
- Last 24 hours of device states
- Current sensor configurations
- Active command queue
- Deduplication buffers

**Async to PostgreSQL:**
- All sensor readings (packets table)
- Device registry (devices table)
- Command history (commands table)
- Event logs (events table)
- Statistics and metrics

**Never Persisted:**
- Temporary LoRa radio state
- Network buffers
- Task handles and mutexes

## Database Schema

### Core Tables

```sql
-- Device Registry
CREATE TABLE devices (
    device_id BIGINT PRIMARY KEY,
    name VARCHAR(64) NOT NULL,
    location VARCHAR(64),
    last_seen TIMESTAMP NOT NULL,
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

-- Packet History
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

-- Command Queue and History
CREATE TABLE commands (
    id SERIAL PRIMARY KEY,
    device_id BIGINT REFERENCES devices(device_id),
    command_type SMALLINT NOT NULL,
    parameters JSONB,
    status VARCHAR(20) NOT NULL,  -- 'queued', 'sent', 'confirmed', 'failed', 'timeout', 'superseded'
    created_at TIMESTAMP DEFAULT NOW(),
    sent_at TIMESTAMP,
    confirmed_at TIMESTAMP,
    retry_count INTEGER DEFAULT 0,
    error_message TEXT
);

CREATE INDEX idx_commands_device_status ON commands(device_id, status);
CREATE INDEX idx_commands_pending ON commands(status, created_at) 
    WHERE status IN ('queued', 'sent');

-- Event Logs
CREATE TABLE events (
    id SERIAL PRIMARY KEY,
    device_id BIGINT REFERENCES devices(device_id),
    event_type SMALLINT NOT NULL,
    severity SMALLINT NOT NULL,  -- 0=info, 1=warning, 2=error
    message TEXT,
    received_at TIMESTAMP DEFAULT NOW()
);

CREATE INDEX idx_events_device_time ON events(device_id, received_at DESC);
CREATE INDEX idx_events_severity ON events(severity, received_at DESC);

-- Deduplication Buffer (optional - could keep in RAM only)
CREATE TABLE dedup_buffer (
    device_id BIGINT REFERENCES devices(device_id),
    buffer_index SMALLINT NOT NULL,
    sequence_num INTEGER,
    PRIMARY KEY (device_id, buffer_index)
);
```

### Migration Strategy

**Phase 1: Dual Write (Current + New)**
- Keep existing JSON registry functional
- Add PostgreSQL writes in parallel
- Compare results for validation
- No functional changes to gateway behavior

**Phase 2: Database-Primary**
- PostgreSQL becomes source of truth
- JSON registry deprecated but kept as backup
- Load device registry from DB on startup
- Fall back to cached data if DB unavailable

**Phase 3: Remove Legacy**
- Remove JSON file operations
- Remove mutex-based locking
- Clean up device_registry.cpp
- Keep only in-memory cache layer

## Implementation Details

### PostgreSQL Client Configuration

Database credentials are stored in `.env` file (excluded from git). Copy `.env.example` to `.env` and configure:

```bash
# .env
PG_HOST=192.168.x.x
PG_PORT=5432
PG_DATABASE=iot_sensors
PG_USER=your_username
PG_PASSWORD=your_password
```

PlatformIO configuration:

```ini
# platformio.ini
[env:esp32-lora-gateway]
lib_deps =
    # Existing dependencies...
    khoih-prog/PostgreSQL_Generic @ ^1.2.0

build_flags =
    # Existing flags...
    -D PG_HOST=\"${sysenv.PG_HOST}\"
    -D PG_PORT=${sysenv.PG_PORT}
    -D PG_DATABASE=\"${sysenv.PG_DATABASE}\"
    -D PG_USER=\"${sysenv.PG_USER}\"
    -D PG_PASSWORD=\"${sysenv.PG_PASSWORD}\"
```

### Graceful Degradation Logic

> **As built (Oct 2026):** `DatabaseManager` is queue-only. Every `write*()`
> call, from any task (LoRa RX, MQTT, async web server), appends to a
> mutex-guarded `std::deque` and returns. `loop()` on the main task owns the
> single `HTTPClient`, pops the head under the lock before each POST, drains
> within a 4 s budget while feeding the watchdog, and pushes the entry back on
> a retryable failure. 4xx responses drop the record; a 5xx is retried three
> times then dropped; only a transport error marks the API disconnected.
> Command lifecycle updates go to `POST /api/commands/status`. The sketch
> below is the original design and keeps the synchronous "write if connected"
> shape for reference.

```cpp
class DatabaseManager {
private:
    PostgresConnection* conn = nullptr;
    DatabaseStatus status = DB_DISCONNECTED;
    std::queue<PendingWrite> writeQueue;
    uint32_t lastReconnectAttempt = 0;
    
public:
    void init() {
        attemptConnection();
    }
    
    void loop() {
        if (status == DB_DISCONNECTED) {
            if (millis() - lastReconnectAttempt > 30000) {
                attemptConnection();
            }
        } else if (status == DB_CONNECTED) {
            processWriteQueue();
            checkConnectionHealth();
        }
    }
    
    bool write(const String& sql, const JsonDocument& params) {
        if (status == DB_CONNECTED) {
            return executeImmediate(sql, params);
        } else {
            queueWrite(sql, params);
            return false;  // Queued for retry
        }
    }
    
private:
    void attemptConnection() {
        lastReconnectAttempt = millis();
        if (conn->connect(PG_HOST, PG_PORT, PG_USER, PG_PASSWORD, PG_DATABASE)) {
            Serial.println("✅ PostgreSQL connected");
            status = DB_CONNECTED;
            syncCacheToDatabase();
        } else {
            Serial.println("⚠️  PostgreSQL unavailable, continuing without persistence");
            status = DB_DISCONNECTED;
        }
    }
    
    void queueWrite(const String& sql, const JsonDocument& params) {
        if (writeQueue.size() < MAX_QUEUE_SIZE) {
            writeQueue.push({sql, params, millis()});
        } else {
            // Drop oldest to prevent memory overflow
            Serial.println("⚠️  Write queue full, dropping oldest");
            writeQueue.pop();
            writeQueue.push({sql, params, millis()});
        }
    }
};
```

### Device Cache Management

```cpp
class DeviceCache {
private:
    std::map<uint64_t, DeviceInfo> devices;
    uint32_t maxAge = 86400000;  // 24 hours
    
public:
    // No mutex needed - ESP32 FreeRTOS tasks are cooperative
    void update(uint64_t deviceId, const DeviceInfo& info) {
        devices[deviceId] = info;
        devices[deviceId].lastUpdated = millis();
        
        // Async write to database (non-blocking)
        dbManager.writeAsync("UPDATE devices SET ... WHERE device_id = $1", deviceId);
    }
    
    DeviceInfo* get(uint64_t deviceId) {
        auto it = devices.find(deviceId);
        if (it != devices.end()) {
            return &it->second;
        }
        return nullptr;
    }
    
    void pruneOldEntries() {
        uint32_t cutoff = millis() - maxAge;
        for (auto it = devices.begin(); it != devices.end(); ) {
            if (it->second.lastUpdated < cutoff) {
                it = devices.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    JsonDocument toJson() {
        JsonDocument doc;
        JsonArray array = doc.to<JsonArray>();
        for (const auto& [id, info] : devices) {
            JsonObject obj = array.add<JsonObject>();
            obj["id"] = String(id, HEX);
            obj["name"] = info.name;
            // ... other fields
        }
        return doc;
    }
};
```

## Benefits of New Architecture

### Performance
- ✅ No mutex contention (cache is lock-free for reads)
- ✅ Faster web API responses (no disk I/O)
- ✅ Non-blocking writes (database operations async)

### Reliability
- ✅ Gateway continues operating if database down
- ✅ No flash wear from constant JSON rewrites
- ✅ Automatic reconnection with exponential backoff
- ✅ Write queue prevents data loss during disconnection

### Scalability
- ✅ No hardcoded device limit
- ✅ Multiple gateways can write to same database
- ✅ Historical data accumulates indefinitely
- ✅ Complex queries possible (analytics, reports)

### Maintainability
- ✅ Standard PostgreSQL tools for backup/restore
- ✅ No custom persistence logic
- ✅ Easier to add new data tables
- ✅ Simpler code (no manual locking)

### Integration
- ✅ Grafana can query directly
- ✅ External services can access data
- ✅ Standard SQL interface
- ✅ JSONB support for flexible schemas

## Migration Risks and Mitigations

### Risk: Database Performance Issues
**Mitigation:** 
- Use connection pooling
- Index key columns (device_id, received_at)
- Batch writes where possible
- Monitor query performance

### Risk: Network Latency
**Mitigation:**
- All writes are async
- Queue writes during disconnection
- Local cache serves web UI
- Timeout operations appropriately

### Risk: Database Credentials Security
**Mitigation:**
- Store in secure partition (NVS)
- Use environment variables in container
- Rotate credentials regularly
- Restrict database user permissions

### Risk: Data Loss During Migration
**Mitigation:**
- Export existing JSON registry first
- Dual-write during transition period
- Validate data integrity
- Keep JSON backup for 30 days

## Testing Strategy

### Unit Tests
- Device cache operations
- Write queue management
- Connection state machine
- Graceful degradation logic

### Integration Tests
- Gateway + PostgreSQL container
- Database connection failures
- Network interruption scenarios
- High packet rate stress testing

### Acceptance Criteria
- Gateway operates normally with DB connected
- Gateway continues LoRa operations with DB disconnected
- No packet loss during DB reconnection
- Web UI responsive in both modes
- Command history persists across reboots
- Historical queries return accurate data

## Rollout Plan

**Week 1: Infrastructure Setup**
- Configure PostgreSQL container
- Create database schema
- Set up database user and permissions
- Test connectivity from gateway

**Week 2: Code Implementation**
- Add PostgreSQL client library
- Implement DatabaseManager class
- Create device cache wrapper
- Add connection health monitoring

**Week 3: Dual-Write Mode**
- Deploy code with both JSON and PostgreSQL writes
- Monitor for errors
- Compare data consistency
- Validate graceful degradation

**Week 4: Database-Primary Mode**
- Switch to loading from PostgreSQL on boot
- Deprecate JSON registry (keep as backup)
- Monitor performance and stability
- Address any issues

**Week 5: Cleanup**
- Remove JSON registry code
- Remove mutex-based locking
- Performance optimization
- Documentation updates

## Monitoring and Observability

### Key Metrics
- Database connection status (connected/disconnected)
- Write queue depth
- Failed write count
- Reconnection attempts
- Query execution time
- Cache hit rate

### Logging
```cpp
Serial.println("✅ PostgreSQL connected");
Serial.println("⚠️  PostgreSQL unavailable, continuing without persistence");
Serial.printf("📊 Write queue: %d pending\n", queueSize);
Serial.printf("⚠️  Failed writes: %d\n", failedWrites);
Serial.printf("🔄 Reconnection attempt %d\n", attemptCount);
```

### Health Endpoints
```
GET /api/health
{
  "database": "connected",
  "write_queue": 0,
  "cache_entries": 5,
  "uptime_seconds": 86400
}
```

## Future Enhancements

### Phase 2 Features (Post-Migration)
- Time-series data analysis
- Grafana dashboard integration
- Email alerts on events
- Historical trend graphs in web UI
- Command scheduling
- Multi-gateway coordination

### Phase 3 Features
- Machine learning anomaly detection
- Predictive maintenance alerts
- API for external integrations
- Mobile app support
- Over-the-air (OTA) firmware updates via web UI

## References

- [PostgreSQL Documentation](https://www.postgresql.org/docs/)
- [PostgreSQL_Generic Library](https://github.com/khoih-prog/PostgreSQL_Generic)
- [ESP32 FreeRTOS Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos.html)
- [LoRa Protocol Specification](lib/LoRaProtocol/lora_protocol.h)

---

**Document Version:** 1.0  
**Last Updated:** 2026-01-16  
**Author:** System Architecture Team  
**Status:** Approved for Implementation
