#!/bin/bash
# Automated deployment script for ESP32 LoRa API Service
# Usage: ./deploy.sh

set -e  # Exit on error

PI_USER="aachten"
PI_HOST="raspberrypi"
PI_DIR="~/docker/esp32-lora-api"

echo "🚀 Deploying ESP32 LoRa API Service to ${PI_USER}@${PI_HOST}"

# Copy files to Pi
echo "📦 Copying files to Pi..."
ssh ${PI_USER}@${PI_HOST} "mkdir -p ${PI_DIR}"
scp -r main.go go.mod go.sum Dockerfile schema.sql .gitignore README.md \
    ${PI_USER}@${PI_HOST}:${PI_DIR}/

echo ""
echo "✅ Files copied successfully!"
echo ""
echo "📝 Next steps on the Pi:"
echo "  1. Run database migration:"
echo "     sudo docker exec -i sre-postgres psql -U sre_agent -d iot_sensors < ${PI_DIR}/schema.sql"
echo ""
echo "  2. Rebuild and restart service:"
echo "     cd ~/docker && sudo docker compose stop esp32-lora-api && sudo docker compose build esp32-lora-api && sudo docker compose up -d esp32-lora-api"
echo ""
echo "  3. Check health:"
echo "     curl http://localhost:3000/api/health"
