#!/bin/bash
# Deploy the ESP32 LoRa API Service to the k3s cluster on the Pi.
#
# The deployment manifest lives in homelab-infra (k3s/base/iot/esp32-lora-api.yml)
# and is applied by ArgoCD. It uses a locally built image with
# imagePullPolicy: Never, so a code change is shipped by:
#   1. copying the source to the Pi's compose build directory
#   2. building the image there with docker compose (name: docker-esp32-lora-api:latest)
#   3. importing it into k3s containerd
#   4. restarting the deployment
#
# Usage: ./deploy.sh            (run from api-service/)
set -euo pipefail

PI_USER="${PI_USER:-aachten}"
PI_HOST="${PI_HOST:-raspberrypi}"
PI_DIR="~/docker/esp32-lora-api"
COMPOSE_DIR="~/docker"
IMAGE="docker.io/library/docker-esp32-lora-api:latest"
NAMESPACE="iot"
DEPLOY="esp32-lora-api"
API_URL="http://192.168.0.167:3000/api"

cd "$(dirname "$0")"
PI="${PI_USER}@${PI_HOST}"

echo "🚀 Deploying ESP32 LoRa API Service to k3s via ${PI}"

echo "📦 Copying source to ${PI}:${PI_DIR}"
ssh "${PI}" "mkdir -p ${PI_DIR}"
scp -q main.go go.mod go.sum Dockerfile schema.sql README.md "${PI}:${PI_DIR}/"

echo "🔨 Building image with docker compose"
ssh "${PI}" "cd ${COMPOSE_DIR} && docker compose build esp32-lora-api"

echo "💾 Backing up the currently deployed image to /tmp/esp32-lora-api-prev.tar"
ssh "${PI}" "sudo k3s ctr images export /tmp/esp32-lora-api-prev.tar ${IMAGE} >/dev/null 2>&1 || echo '   (no existing image to back up)'"

echo "📥 Importing image into k3s containerd"
ssh "${PI}" "docker save ${IMAGE} | sudo k3s ctr images import -"

echo "🔄 Restarting deployment ${NAMESPACE}/${DEPLOY}"
ssh "${PI}" "kubectl -n ${NAMESPACE} rollout restart deployment/${DEPLOY} && kubectl -n ${NAMESPACE} rollout status deployment/${DEPLOY} --timeout=180s"

echo "🩺 Verifying"
sleep 2
echo "   health:       HTTP $(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "${API_URL}/health")"
echo "   GET commands: HTTP $(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "${API_URL}/commands?limit=1")"
echo "   GET events:   HTTP $(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "${API_URL}/events?limit=1")"

cat <<EOF

✅ Done.

Rollback (if needed):
  ssh ${PI} "sudo k3s ctr images import /tmp/esp32-lora-api-prev.tar && kubectl -n ${NAMESPACE} rollout restart deployment/${DEPLOY}"

Schema changes are not applied by this script. If schema.sql changed:
  ssh ${PI} "kubectl -n data exec -i sre-postgres-0 -- psql -U sre_agent -d iot_sensors" < schema.sql
EOF
