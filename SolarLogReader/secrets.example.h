// secrets.example.h
//
// Kopieren nach secrets.h und ausfuellen:
//     cp secrets.example.h secrets.h
//
// secrets.h steht in .gitignore und gehoert NICHT ins Repository.

#ifndef SECRETS_H
#define SECRETS_H

#include <stdint.h>

// ---------------- WiFi ----------------
const char WIFI_SSID[]     = "DEIN_WLAN";
const char WIFI_PASSWORD[] = "DEIN_WLAN_PASSWORT";

// ---------------- MQTT (Home Assistant) ----------------
const char MQTT_BROKER[]    = "192.168.0.2";       // IP des Brokers
const uint16_t MQTT_PORT    = 1883;
const char MQTT_USER[]      = "mqtt_user";         // "" = anonym
const char MQTT_PASS[]      = "mqtt_passwort";
const char MQTT_CLIENT_ID[] = "fysetc-solarlog-reader";

// Topic mit der Rohzeile als JSON
const char MQTT_TOPIC_SOLAR[]  = "home/solarlog/values";
// Status-Topic (Dateigroesse, Position, Reset-Meldungen)
const char MQTT_TOPIC_STATUS[] = "home/solarlog/status";

// Remote-Konfiguration (Feldnamen ohne Reflash aendern)
const char MQTT_TOPIC_SETCONFIG[]   = "home/solarlog/set_config";
const char MQTT_TOPIC_CONFIGSTATE[] = "home/solarlog/config_state";
// Eigene IP des ESP (fuer Debug / Netzwerk)
const char MQTT_TOPIC_IP[] = "home/solarlog/ip";
// Telemetrie (auch mit Discovery in Home Assistant)
const char MQTT_TOPIC_LASTREFRESH[] = "home/solarlog/last_refresh";
const char MQTT_TOPIC_FILE[]  = "home/solarlog/file";
const char MQTT_TOPIC_SIZE[]  = "home/solarlog/size";
const char MQTT_TOPIC_POS[]   = "home/solarlog/pos";
const char MQTT_TOPIC_RSSI[]  = "home/solarlog/rssi";
// Startzeit des ESP als Timestamp (Topic-Name aus Kompatibilitaet behalten)
const char MQTT_TOPIC_UPTIME[] = "home/solarlog/uptime";
const char MQTT_TOPIC_HEAP[]  = "home/solarlog/heap";
// Messzeitpunkt aus der SD-Zeile (1106;1830 = 06.11. 18:30), nicht ESP-Lesezeit
const char MQTT_TOPIC_MEASTIME[] = "home/solarlog/meas_time";

// ---------------- SD-Karte / FYSETC SD WiFi Card Reader V2.1 ----------------
// FYSETC: SD_CS = 4, CS_SENSE = 5.
// SPI-Pins (12/13/14) NICHT hier definieren: die kommen schon aus dem
// ESP8266-Core (SD-Lib -> Sd2PinMap.h).
const uint8_t SD_CS_PIN    = 4;
const uint8_t CS_SENSE_PIN = 5;

// ---------------- Verhalten ----------------
const uint32_t READ_INTERVAL_MS  = 10000;
const uint8_t MAX_LINES_PER_LOOP = 20;

// Ab dieser Dateigroesse wird die Datei geleert (1,5 GB in Byte):
const uint32_t MAX_FILE_SIZE_BYTES = 1500UL * 1024UL * 1024UL;

// Monats-Log: WE + JJMM + .DAT, z.B. WE2611.DAT = Nov 2026.
// Der ESP baut den Namen aus der Uhrzeit (NTP) selbst.
const char SD_FILENAME_FALLBACK[] = "WE2611.DAT";
const char SD_PREFIX[] = "WE";
const char SD_SUFFIX[] = ".DAT";
// NTP fuer Monats-Erkennung und Zeitstempel
const char NTP_SERVER[] = "pool.ntp.org";

#endif
