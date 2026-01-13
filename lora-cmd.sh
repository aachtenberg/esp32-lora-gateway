#!/bin/bash
# LoRa Sensor Remote Command Script
# Sends commands to LoRa sensors via MQTT gateway

# Get script directory for relative paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load .env file if it exists
if [ -f "$SCRIPT_DIR/.env" ]; then
    # Export variables from .env file (ignore comments and empty lines)
    export $(grep -v '^#' "$SCRIPT_DIR/.env" | grep -v '^$' | xargs)
fi

# Configuration - Uses values from .env or defaults
MQTT_BROKER="${MQTT_BROKER:-localhost}"
MQTT_PORT="${MQTT_PORT:-1883}"
MQTT_USER="${MQTT_USER:-}"
MQTT_PASS="${MQTT_PASS:-}"
MQTT_TOPIC="lora/command"

# Default device ID (full 16-character hex)
DEFAULT_DEVICE_ID="${DEFAULT_DEVICE_ID:-AABBCCDDEEFF0011}"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Print usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS] <command> [value]

Send remote commands to LoRa sensors via MQTT gateway.

COMMANDS:
  interval <seconds>    Set sensor reading interval (5-3600s)
  sleep <seconds>       Set deep sleep duration (0-3600s)
  status                Request status update
  restart               Restart the sensor
  calibrate             Set current pressure as baseline
  baseline <hPa>        Set specific pressure baseline (900-1100 hPa)
  clear_baseline        Disable pressure baseline tracking

OPTIONS:
  -d, --device <id>     Target device ID (default: $DEFAULT_DEVICE_ID)
  -b, --broker <host>   MQTT broker address (default: $MQTT_BROKER)
  -p, --port <port>     MQTT broker port (default: $MQTT_PORT)
  -u, --user <user>     MQTT username
  -P, --pass <pass>     MQTT password
  -h, --help            Show this help message

EXAMPLES:
  $0 interval 90                    # Set interval to 90 seconds
  $0 sleep 900                      # Set sleep to 15 minutes
  $0 status                         # Request status update
  $0 restart                        # Restart sensor
  $0 calibrate                      # Set current pressure as baseline
  $0 baseline 1013.25               # Set baseline to 1013.25 hPa
  $0 clear_baseline                 # Clear baseline tracking
  $0 -d aabbccdd interval 60        # Command different sensor
  $0 -b 192.168.1.100 interval 60  # Use different broker

CONFIGURATION:
  The script looks for a .env file in the same directory.
  Copy .env.example to .env and configure your settings.

  You can also set environment variables:
    MQTT_BROKER         MQTT broker hostname/IP
    MQTT_PORT           MQTT broker port
    MQTT_USER           MQTT username
    MQTT_PASS           MQTT password
    DEFAULT_DEVICE_ID   Default sensor device ID

  Command-line options override .env and environment variables.

EOF
    exit 0
}

# Parse command line arguments
DEVICE_ID="$DEFAULT_DEVICE_ID"

while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--device)
            DEVICE_ID="$2"
            shift 2
            ;;
        -b|--broker)
            MQTT_BROKER="$2"
            shift 2
            ;;
        -p|--port)
            MQTT_PORT="$2"
            shift 2
            ;;
        -u|--user)
            MQTT_USER="$2"
            shift 2
            ;;
        -P|--pass)
            MQTT_PASS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        interval)
            ACTION="set_interval"
            VALUE="$2"
            shift 2
            break
            ;;
        sleep)
            ACTION="set_sleep"
            VALUE="$2"
            shift 2
            break
            ;;
        status)
            ACTION="status"
            shift
            break
            ;;
        restart)
            ACTION="restart"
            shift
            break
            ;;
        calibrate)
            ACTION="calibrate"
            shift
            break
            ;;
        baseline)
            ACTION="set_baseline"
            VALUE="$2"
            shift 2
            break
            ;;
        clear_baseline)
            ACTION="clear_baseline"
            shift
            break
            ;;
        *)
            echo -e "${RED}Error: Unknown option or command: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Check if action was provided
if [ -z "$ACTION" ]; then
    echo -e "${RED}Error: No command specified${NC}"
    usage
fi

# Validate value for commands that require it
if [ "$ACTION" = "set_interval" ] || [ "$ACTION" = "set_sleep" ]; then
    if [ -z "$VALUE" ]; then
        echo -e "${RED}Error: '$ACTION' requires a value${NC}"
        exit 1
    fi

    # Validate numeric value
    if ! [[ "$VALUE" =~ ^[0-9]+$ ]]; then
        echo -e "${RED}Error: Value must be a number${NC}"
        exit 1
    fi

    # Validate range
    if [ "$ACTION" = "set_interval" ]; then
        if [ "$VALUE" -lt 5 ] || [ "$VALUE" -gt 3600 ]; then
            echo -e "${RED}Error: Interval must be between 5 and 3600 seconds${NC}"
            exit 1
        fi
    elif [ "$ACTION" = "set_sleep" ]; then
        if [ "$VALUE" -lt 0 ] || [ "$VALUE" -gt 3600 ]; then
            echo -e "${RED}Error: Sleep duration must be between 0 and 3600 seconds${NC}"
            exit 1
        fi
    fi
elif [ "$ACTION" = "set_baseline" ]; then
    if [ -z "$VALUE" ]; then
        echo -e "${RED}Error: 'baseline' requires a value in hPa${NC}"
        exit 1
    fi

    # Validate numeric value (allow decimals)
    if ! [[ "$VALUE" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
        echo -e "${RED}Error: Baseline must be a number${NC}"
        exit 1
    fi

    # Validate range (900-1100 hPa)
    if (( $(echo "$VALUE < 900" | bc -l) )) || (( $(echo "$VALUE > 1100" | bc -l) )); then
        echo -e "${RED}Error: Baseline must be between 900 and 1100 hPa${NC}"
        exit 1
    fi
fi

# Build JSON payload
if [ -n "$VALUE" ]; then
    PAYLOAD="{\"device_id\":\"$DEVICE_ID\",\"action\":\"$ACTION\",\"value\":$VALUE}"
else
    PAYLOAD="{\"device_id\":\"$DEVICE_ID\",\"action\":\"$ACTION\"}"
fi

# Build mosquitto_pub command
MQTT_CMD="mosquitto_pub -h $MQTT_BROKER -p $MQTT_PORT -t $MQTT_TOPIC -m '$PAYLOAD'"

# Add authentication if provided
if [ -n "$MQTT_USER" ]; then
    MQTT_CMD="$MQTT_CMD -u $MQTT_USER"
fi
if [ -n "$MQTT_PASS" ]; then
    MQTT_CMD="$MQTT_CMD -P $MQTT_PASS"
fi

# Print command info
echo -e "${YELLOW}═══════════════════════════════════════${NC}"
echo -e "${YELLOW}LoRa Remote Command${NC}"
echo -e "${YELLOW}═══════════════════════════════════════${NC}"
echo -e "Broker:  $MQTT_BROKER:$MQTT_PORT"
echo -e "Device:  0x$DEVICE_ID"
echo -e "Action:  $ACTION"
if [ -n "$VALUE" ]; then
    echo -e "Value:   $VALUE"
fi
echo -e "${YELLOW}───────────────────────────────────────${NC}"

# Execute command
echo -e "Sending command..."
if eval "$MQTT_CMD"; then
    echo -e "${GREEN}✓ Command sent successfully${NC}"
    echo ""
    echo -e "${YELLOW}Note:${NC} Sensor will process commands during its 10-second"
    echo -e "      listening window after sending data."
    echo ""
    echo -e "Monitor sensor logs to confirm command was received:"
    echo -e "  ${GREEN}pio device monitor${NC}"
else
    echo -e "${RED}✗ Failed to send command${NC}"
    exit 1
fi
