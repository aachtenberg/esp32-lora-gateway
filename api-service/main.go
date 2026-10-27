package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"

	_ "github.com/lib/pq"
)

type Config struct {
	Port       string
	DBHost     string
	DBPort     string
	DBUser     string
	DBPassword string
	DBName     string
}

type DevicePayload struct {
	DeviceID       string `json:"device_id"`
	Name           string `json:"name"`
	Location       string `json:"location"`
	SensorType     string `json:"sensor_type"`
	LastRSSI       int16  `json:"last_rssi"`
	LastSNR        int16  `json:"last_snr"`
	PacketCount    int32  `json:"packet_count"`
	LastSequence   int32  `json:"last_sequence"`
	SensorInterval int16  `json:"sensor_interval"`
	DeepSleepSec   int16  `json:"deep_sleep_sec"`
}

type PacketPayload struct {
	DeviceID    string          `json:"device_id"`
	GatewayID   string          `json:"gateway_id"`
	MsgType     int16           `json:"msg_type"`
	SequenceNum int32           `json:"sequence_num"`
	RSSI        int16           `json:"rssi"`
	SNR         int16           `json:"snr"`
	Payload     json.RawMessage `json:"payload"`
}

type CommandPayload struct {
	DeviceID    string `json:"device_id"`
	CommandType int16  `json:"command_type"`
	Parameters  string `json:"parameters"`
	Status      string `json:"status"`
}

// CommandStatusPayload updates the most recent open command (status queued or
// sent) for a device and command type. The gateway queues at most one command
// per device+type, so this identifies the row without the gateway having to
// hold database ids.
type CommandStatusPayload struct {
	DeviceID     string  `json:"device_id"`
	CommandType  int16   `json:"command_type"`
	Status       string  `json:"status"`
	From         string  `json:"from,omitempty"` // row status to match; defaults per transition
	RetryCount   *int    `json:"retry_count,omitempty"`
	ErrorMessage *string `json:"error_message,omitempty"`
}

type EventPayload struct {
	DeviceID  string `json:"device_id"`
	EventType int16  `json:"event_type"`
	Severity  int16  `json:"severity"`
	Message   string `json:"message"`
}

var db *sql.DB

func main() {
	config := Config{
		Port:       getEnv("PORT", "3000"),
		DBHost:     getEnv("DB_HOST", "192.168.0.167"),
		DBPort:     getEnv("DB_PORT", "5432"),
		DBUser:     getEnv("DB_USER", "sre_agent"),
		DBPassword: getEnv("DB_PASSWORD", ""),
		DBName:     getEnv("DB_NAME", "iot_sensors"),
	}

	// Connect to PostgreSQL
	connStr := fmt.Sprintf("host=%s port=%s user=%s password=%s dbname=%s sslmode=disable",
		config.DBHost, config.DBPort, config.DBUser, config.DBPassword, config.DBName)

	var err error
	db, err = sql.Open("postgres", connStr)
	if err != nil {
		log.Fatal("Failed to open database:", err)
	}
	defer db.Close()

	// Test connection
	if err := db.Ping(); err != nil {
		log.Fatal("Failed to connect to database:", err)
	}

	log.Printf("Connected to PostgreSQL at %s:%s", config.DBHost, config.DBPort)

	// CORS middleware
	corsHandler := func(next http.HandlerFunc) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Access-Control-Allow-Origin", "*")
			w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
			w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
			if r.Method == "OPTIONS" {
				w.WriteHeader(http.StatusOK)
				return
			}
			next(w, r)
		}
	}

	// Setup HTTP routes
	// NOTE: Packets endpoint removed - sensor data goes to MQTT → timeseries DB
	// This API is for device registry and management only
	http.HandleFunc("/api/health", corsHandler(healthHandler))
	http.HandleFunc("/api/devices", corsHandler(devicesHandler))
	http.HandleFunc("/api/commands", corsHandler(commandsHandler))
	http.HandleFunc("/api/commands/status", corsHandler(commandStatusHandler))
	http.HandleFunc("/api/events", corsHandler(eventsHandler))

	// Start server
	addr := ":" + config.Port
	log.Printf("Starting server on %s", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatal("Server failed:", err)
	}
}

func healthHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Check database connection
	if err := db.Ping(); err != nil {
		http.Error(w, "Database unavailable", http.StatusServiceUnavailable)
		return
	}

	w.WriteHeader(http.StatusOK)
	w.Write([]byte("OK"))
}

func devicesHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var payload DevicePayload
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		log.Printf("Invalid JSON: %v", err)
		return
	}

	// UPSERT device
	query := `
		INSERT INTO devices (
			device_id, name, location, sensor_type, last_rssi, last_snr,
			packet_count, last_sequence, sensor_interval, deep_sleep_sec,
			last_seen, updated_at
		) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, NOW(), NOW())
		ON CONFLICT (device_id) DO UPDATE SET
			name = EXCLUDED.name,
			location = EXCLUDED.location,
			sensor_type = EXCLUDED.sensor_type,
			last_rssi = EXCLUDED.last_rssi,
			last_snr = EXCLUDED.last_snr,
			packet_count = EXCLUDED.packet_count,
			last_sequence = EXCLUDED.last_sequence,
			sensor_interval = EXCLUDED.sensor_interval,
			deep_sleep_sec = EXCLUDED.deep_sleep_sec,
			last_seen = NOW(),
			updated_at = NOW()
	`

	_, err := db.Exec(query,
		payload.DeviceID, payload.Name, payload.Location, payload.SensorType,
		payload.LastRSSI, payload.LastSNR, payload.PacketCount,
		payload.LastSequence, payload.SensorInterval, payload.DeepSleepSec)

	if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		log.Printf("Failed to upsert device: %v", err)
		return
	}

	log.Printf("Device updated: %s (%s)", payload.DeviceID, payload.Name)
	w.WriteHeader(http.StatusOK)
}

// packetsHandler removed - sensor data goes to MQTT → timeseries DB
// This API handles device registry and management only

func commandsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		// GET: Fetch recent command history (newest first)
		limit := r.URL.Query().Get("limit")
		if limit == "" {
			limit = "50"
		}

		query := `
			SELECT c.id, c.device_id, d.name, c.command_type, COALESCE(c.parameters, '{}'::jsonb),
			       c.status, c.created_at, c.sent_at, c.confirmed_at, COALESCE(c.retry_count, 0), c.error_message
			FROM commands c
			LEFT JOIN devices d ON c.device_id = d.device_id
			ORDER BY c.created_at DESC
			LIMIT $1
		`

		rows, err := db.Query(query, limit)
		if err != nil {
			http.Error(w, "Database error", http.StatusInternalServerError)
			log.Printf("Failed to fetch commands: %v", err)
			return
		}
		defer rows.Close()

		type CommandResponse struct {
			ID           int             `json:"id"`
			DeviceID     string          `json:"device_id"`
			DeviceName   string          `json:"device_name"`
			CommandType  int             `json:"command_type"`
			Parameters   json.RawMessage `json:"parameters"`
			Status       string          `json:"status"`
			CreatedAt    string          `json:"created_at"`
			SentAt       *string         `json:"sent_at"`
			ConfirmedAt  *string         `json:"confirmed_at"`
			RetryCount   int             `json:"retry_count"`
			ErrorMessage *string         `json:"error_message"`
		}

		commands := []CommandResponse{}
		for rows.Next() {
			var c CommandResponse
			var deviceName *string
			var params []byte
			if err := rows.Scan(&c.ID, &c.DeviceID, &deviceName, &c.CommandType, &params, &c.Status, &c.CreatedAt,
				&c.SentAt, &c.ConfirmedAt, &c.RetryCount, &c.ErrorMessage); err != nil {
				log.Printf("Error scanning command: %v", err)
				continue
			}
			if deviceName != nil {
				c.DeviceName = *deviceName
			} else {
				c.DeviceName = "Unknown"
			}
			if len(params) == 0 {
				params = []byte("{}")
			}
			c.Parameters = json.RawMessage(params)
			commands = append(commands, c)
		}

		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(commands)
		return
	}

	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var payload CommandPayload
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		log.Printf("Invalid JSON: %v", err)
		return
	}

	query := `
		INSERT INTO commands (
			device_id, command_type, parameters, status, created_at
		) VALUES ($1, $2, $3, $4, NOW())
		RETURNING id
	`

	var id int
	err := db.QueryRow(query,
		payload.DeviceID, payload.CommandType,
		payload.Parameters, payload.Status).Scan(&id)

	if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		log.Printf("Failed to insert command: %v", err)
		return
	}

	log.Printf("Command logged: device=%s type=%d id=%d", payload.DeviceID, payload.CommandType, id)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]int{"id": id})
}

// commandStatusHandler moves one command row for a device+type through its
// lifecycle. Each transition targets the newest row in a specific prior
// status, so two open rows of the same type (one sent, one queued behind it)
// are never confused:
//
//	sent                 <- queued
//	confirmed            <- sent
//	timeout, superseded,
//	failed               <- queued (or "from":"sent" when a newer command of
//	                        the same type was transmitted over an unconfirmed one)
//
// sent_at / confirmed_at are stamped on the matching transition. A request
// that matches no row is not an error (the row may have been inserted while
// the gateway was offline).
func commandStatusHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var payload CommandStatusPayload
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		log.Printf("Invalid JSON: %v", err)
		return
	}

	from := payload.From
	switch payload.Status {
	case "sent", "timeout", "superseded", "failed":
		if from == "" {
			from = "queued"
		}
	case "confirmed":
		if from == "" {
			from = "sent"
		}
	default:
		http.Error(w, "Invalid status", http.StatusBadRequest)
		return
	}
	if from != "queued" && from != "sent" {
		http.Error(w, "Invalid from status", http.StatusBadRequest)
		return
	}

	// $3 is both assigned to a varchar column and compared as text; without the
	// casts Postgres reports "inconsistent types deduced for parameter $3".
	query := `
		UPDATE commands SET
			status = $3::text,
			retry_count = COALESCE($4::int, retry_count),
			error_message = COALESCE($5::text, error_message),
			sent_at = CASE WHEN $3::text = 'sent' AND sent_at IS NULL THEN NOW() ELSE sent_at END,
			confirmed_at = CASE WHEN $3::text = 'confirmed' THEN NOW() ELSE confirmed_at END
		WHERE id = (
			SELECT id FROM commands
			WHERE device_id = $1 AND command_type = $2 AND status = $6::text
			ORDER BY created_at DESC
			LIMIT 1
		)
	`

	res, err := db.Exec(query,
		payload.DeviceID, payload.CommandType, payload.Status,
		payload.RetryCount, payload.ErrorMessage, from)
	if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		log.Printf("Failed to update command status: %v", err)
		return
	}
	n, _ := res.RowsAffected()

	log.Printf("Command status: device=%s type=%d %s -> %s (%d row)", payload.DeviceID, payload.CommandType, from, payload.Status, n)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]int64{"updated": n})
}

func eventsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		// GET: Fetch recent events
		limit := r.URL.Query().Get("limit")
		if limit == "" {
			limit = "50" // Default to last 50 events
		}

		query := `
			SELECT e.id, e.device_id, d.name, e.event_type, e.severity, e.message, e.received_at
			FROM events e
			LEFT JOIN devices d ON e.device_id = d.device_id
			ORDER BY e.received_at DESC
			LIMIT $1
		`

		rows, err := db.Query(query, limit)
		if err != nil {
			http.Error(w, "Database error", http.StatusInternalServerError)
			log.Printf("Failed to fetch events: %v", err)
			return
		}
		defer rows.Close()

		type EventResponse struct {
			ID         int    `json:"id"`
			DeviceID   string `json:"device_id"`
			DeviceName string `json:"device_name"`
			EventType  int    `json:"event_type"`
			Severity   int    `json:"severity"`
			Message    string `json:"message"`
			ReceivedAt string `json:"received_at"`
		}

		events := []EventResponse{}
		for rows.Next() {
			var e EventResponse
			var deviceName *string
			if err := rows.Scan(&e.ID, &e.DeviceID, &deviceName, &e.EventType, &e.Severity, &e.Message, &e.ReceivedAt); err != nil {
				log.Printf("Error scanning event: %v", err)
				continue
			}
			if deviceName != nil {
				e.DeviceName = *deviceName
			} else {
				e.DeviceName = "Unknown"
			}
			events = append(events, e)
		}

		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(events)
		return
	}

	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var payload EventPayload
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		log.Printf("Invalid JSON: %v", err)
		return
	}

	query := `
		INSERT INTO events (
			device_id, event_type, severity, message, received_at
		) VALUES ($1, $2, $3, $4, NOW())
	`

	_, err := db.Exec(query,
		payload.DeviceID, payload.EventType,
		payload.Severity, payload.Message)

	if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		log.Printf("Failed to insert event: %v", err)
		return
	}

	log.Printf("Event logged: device=%s severity=%d", payload.DeviceID, payload.Severity)
	w.WriteHeader(http.StatusOK)
}

func getEnv(key, defaultValue string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return defaultValue
}
