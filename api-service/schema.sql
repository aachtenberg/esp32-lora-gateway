-- ESP32 LoRa Gateway Database Schema
-- Database: iot_sensors
-- Run this after creating the database:
-- sudo docker exec -i sre-postgres psql -U sre_agent -d iot_sensors < schema.sql

-- Drop existing tables (clean migration)
DROP TABLE IF EXISTS events CASCADE;
DROP TABLE IF EXISTS commands CASCADE;
DROP TABLE IF EXISTS packets CASCADE;
DROP TABLE IF EXISTS devices CASCADE;

-- Device Registry (UPSERT on device_id)
CREATE TABLE IF NOT EXISTS devices (
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

CREATE INDEX IF NOT EXISTS idx_devices_last_seen ON devices(last_seen DESC);

-- NOTE: Packet/sensor data is NOT stored here - it goes to MQTT → timeseries DB
-- This database is for device registry and management only

-- Command Queue and History (INSERT only)
CREATE TABLE IF NOT EXISTS commands (
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

CREATE INDEX IF NOT EXISTS idx_commands_device_status ON commands(device_id, status);
CREATE INDEX IF NOT EXISTS idx_commands_pending ON commands(status, created_at) WHERE status = 'pending';

-- Event Logs (INSERT only)
CREATE TABLE IF NOT EXISTS events (
    id SERIAL PRIMARY KEY,
    device_id BIGINT REFERENCES devices(device_id),
    event_type SMALLINT NOT NULL,
    severity SMALLINT NOT NULL,
    message TEXT,
    received_at TIMESTAMP DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_events_device_time ON events(device_id, received_at DESC);
CREATE INDEX IF NOT EXISTS idx_events_severity ON events(severity, received_at DESC);

-- Verify tables created
SELECT 
    schemaname, 
    tablename, 
    tableowner 
FROM pg_tables 
WHERE schemaname = 'public' 
ORDER BY tablename;
