# ESP32 LoRa Gateway - API Service

Lightweight HTTP → PostgreSQL bridge for ESP32 gateway.
## Production Deployment

✅ **Deployed to:** Raspberry Pi Docker stack (`~/docker/esp32-lora-api/`)

**Configuration:**
- Network: `monitoring` (shared with postgres, grafana, telegraf)
- Database: `sre-postgres` (internal hostname)
- Port: 3000
- Password: `${SRE_POSTGRES_PASSWORD}` (from environment)
- Build context: `./esp32-lora-api` (relative to main compose file)
## Features

- ✅ HTTP REST endpoints
- ✅ Direct PostgreSQL writes (no Telegraf limitations)
- ✅ UPSERT for devices, INSERT for packets/commands/events
- ✅ Health check endpoint
- ✅ ~5MB Docker image
- ✅ ~20MB RAM usage

## Development

### Prerequisites

- Go 1.21+ (optional for local testing)
- Docker (for deployment)

### Local Testing (Optional)

```bash
cd api-service

# Install dependencies
go mod download

# Run locally
export DB_HOST=192.168.0.167
export DB_PORT=5432
export DB_USER=sre_agent
export DB_PASSWORD=your_password
export DB_NAME=iot_sensors

go run main.go
```

## Deployment to Raspberry Pi

### Recommended Directory Structure

```
/home/pi/docker/
├── postgres/
├── grafana/
├── telegraf/
├── mqtt/
├── esp32-lora-api/     ← New service
│   ├── main.go
│   ├── go.mod
│   ├── go.sum
│   ├── Dockerfile
│   └── docker-compose.yml
└── docker-compose.yml   ← Main stack file (optional)
```

### Option 1: Build on Raspberry Pi (Recommended)

```bash
# SSH to Raspberry Pi
ssh pi@your-rpi

# Create service directory
mkdir -p ~/docker/esp32-lora-api

# Exit SSH
exit

# Copy files from development Main Stack

If you have a central `docker-compose.yml` that manages all services:

```yaml
# In /home/pi/docker/docker-compose.yml
services:
  # ... your existing services (postgres, grafana, etc) ...
  
  esp32-lora-api:
    build: ./esp32-lora-api
    container_name: esp32-lora-api
    restart: unless-stopped
    ports:
      - "3000:3000"
    environment:
      - PORT=3000
      - DB_HOST=postgres  # If postgres is in same stack
      - DB_PORT=5432
      - DB_USER=${PG_USER}
      - DB_PASSWORD=${PG_PASSWORD}
      - DB_NAME=${PG_DATABASE}
    depends_on:
      - postgres
    networks:
      - iot_network

networks:
  iot_network:
    driver: bridge
```

Then deploy entire stack:
```bash
cd ~/docker
docker-compose up -d esp32-lora-apiour registry
docker tag esp32-lora-api:latest your-registry/esp32-lora-api:latest
docker push your-registry/esp32-lora-api:latest

# On Raspberry Pi, pull and run
docker pull your-registry/esp32-lora-api:latest
docker-compose up -d
```

### Option 3: Add to Existing docker-compose Stack

Add this to your main `docker-compose.yml`:

```yaml
services:
  esp32-lora-api:
    image: esp32-lora-api:latest
    build: ./esp32-lora-gateway/api-service
    restart: unless-stopped
    ports:
      - "3000:3000"
    environment:
      - DB_HOST=${PG_HOST}
      - DB_PORT=${PG_PORT}
      - DB_USER=${PG_USER}
      - DB_PASSWORD=${PG_PASSWORD}
      - DB_NAME=${PG_DATABASE}
    networks:
      - iot_network
```

## Configuration

Environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `PORT` | 3000 | HTTP server port |
| `DB_HOST` | 192.168.0.167 | PostgreSQL host |
| `DB_PORT` | 5432 | PostgreSQL port |
| `DB_USER` | sre_agent | Database user |
| `DB_PASSWORD` | (required) | Database password |
| `DB_NAME` | iot_sensors | Database name |

## API Endpoints

- `GET /api/health` - Health check
- `POST /api/devices` - Device updates (UPSERT)
- `POST /api/packets` - Packet history (INSERT)
- `POST /api/commands` - Commands (INSERT)
- `POST /api/events` - Events (INSERT)

See [TELEGRAF_SPEC.md](../docs/TELEGRAF_SPEC.md) for payload formats.

## Testing

```bash
# Health check
curl http://localhost:3000/api/health

# Test device write
curl -X POST http://localhost:3000/api/devices \
  -H "Content-Type: application/json" \
  -d '{"device_id":"12345678","name":"test","location":"test","last_rssi":-85,"last_snr":8,"packet_count":1,"last_sequence":1,"sensor_interval":90,"deep_sleep_sec":60}'

# Check PostgreSQL
psql -h 192.168.0.167 -U sre_agent -d iot_sensors -c "SELECT * FROM devices;"
```

## Logs

```bash
# View logs
docker logs -f esp32-lora-api

# Example output:
# 2026/01/16 10:30:00 Connected to PostgreSQL at 192.168.0.167:5432
# 2026/01/16 10:30:00 Starting server on :3000
# 2026/01/16 10:31:15 Device updated: 12345678 (sensor_01)
# 2026/01/16 10:32:30 Packet logged: device=12345678 seq=156
```

## Resource Usage

- **Image Size:** ~8MB
- **RAM Usage:** ~20MB
- **CPU Usage:** <1% (idle), ~5% (active)
- **Startup Time:** <1 second

## Troubleshooting

**Connection refused:**
- Check PostgreSQL is accessible from container
- Verify network configuration
- Check firewall rules

**Database errors:**
- Ensure tables are created (run schema from TELEGRAF_SPEC.md)
- Verify database credentials
- Check PostgreSQL logs

**503 on /api/health:**
- Database connection failed
- Check DB_HOST and credentials
