// SolarLogReader.ino
// ESP8285 (Generic ESP8285 Module) + FYSETC SD WiFi Card Reader V2.1
// + TiSUN MULTIPLEX -> MQTT -> Home Assistant
//
// Remote-Funktion (ohne Rausziehen):
//  Namen per MQTT aendern: Topic home/solarlog/set_config
//  (Kein OTA: Firmware wird per USB geflasht.)
//
// B-Beispiele (per MQTT Explorer / HA Entwicklerwerkzeuge -> MQTT senden):
//  Einzelnes Feld umbenennen / anlegen (min/max = Plausibilitaet):
//    {"index":4,"key":"vorlauf","name":"Vorlauf","unit":"°C","divider":1,"min":-35,"max":150}
//  Werte ausserhalb min/max werden NICHT gesendet (Topic wird geleert),
//  damit z.B. 199 = "Ausser Betrieb" nicht als 199 °C im Diagramm landet.
//  Nur anzeigen was aktiv ist:
//    {"cmd":"list"}
//  Alles auf fields.h-Defaults zurueck:
//    {"cmd":"reset"}
// Antwort kommt auf home/solarlog/config_state.

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <SD.h>
#include <EEPROM.h>
#include <time.h>
#include "secrets.h"
#include "fields.h"

// Aktuelle Monats-Datei, z.B. WE2611.DAT. Wird per NTP bestimmt.
String currentLogFile;
String lastActiveFile;

void publishDiscovery();
void publishDiscoverySingle(uint8_t rtIdx);
void publishIp();
void publishTelemetryDiscovery();
void publishTelemetry(uint32_t fileSize, uint32_t lastPos);
String nowIso();
String measIsoFromLine(const String& clean);
uint32_t loadLinesTotal();
void saveLinesTotal(uint32_t v);
void setupTime();
String monthFilenameFor(time_t t);
String activeLogFile();
bool isLogFile(const String& name);
void cleanupOldMonthFiles(const String& keep);
void updateActiveFile();
void publishFields(const String& clean);
void refreshFromTail();
String getFieldToken(const String& line, uint8_t wanted);
void mqttCallback(char* topic, byte* payload, unsigned int len);
void loadRuntimeFields();
void saveRuntimeFields();
void publishConfigState(const String& info);

// ---------- EEPROM ----------
#define EEPROM_SIZE 1024
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_POS 1
#define EEPROM_MAGIC 0xA5
#define EEPROM_CFG_MAGIC_ADDR 32
// 0xC4: neue Feldtabelle aus fields.h erzwingen (Spalten aus WE2611.DAT,
//       Temperaturen ohne Teiler, Plausibilitaetsgrenzen, Namen nur T1..T5,
//       Spalte 28 entfernt). Alte EEPROM-Namen/Teiler werden ersetzt.
#define EEPROM_CFG_MAGIC 0xC4
#define EEPROM_CFG_COUNT_ADDR 33
#define EEPROM_CFG_BASE 48
// WICHTIG: Config belegt 48 .. 1014 (14 Felder x 69 Byte).
// linesTotal muss deshalb in den freien Bereich 5..31, sonst ueberschreibt
// es die Feld-Config (Namen/Units kaputt).
#define EEPROM_LINES_ADDR 8

// Herstellername in allen MQTT-Discovery-Payloads (Geraeteseite in HA)
#define MANUFACTURER_NAME "TiSUN"

// Wie oft die letzte Zeile der Datei geprueft wird (unabhaengig von lastPos).
#ifndef TAIL_CHECK_MS
#define TAIL_CHECK_MS 10000UL
#endif

#define MAX_RT_FIELDS 14
#define KEY_LEN 20
#define NAME_LEN 36
#define UNIT_LEN 8
#define ICON_LEN 32

struct RtField {
  uint8_t index;
  char key[KEY_LEN];
  char name[NAME_LEN];
  char unit[UNIT_LEN];
  char icon[ICON_LEN];  // mdi-Name fuer HA (RAM, Default aus fields.h)
  float divider;
  float lo;      // Plausibilitaet (nur RAM, Defaults aus fields.h)
  float hi;
  bool used;
  bool disc;     // Discovery in HA vorhanden (erst nach 1. gueltigem Wert)
  bool pruned;   // ungueltig seit Start -> Discovery wurde geloescht
};

RtField rtFields[MAX_RT_FIELDS];
uint8_t rtCount = 0;

// Telemetrie
uint32_t linesTotal = 0;
String lastRefreshIso = "";
String lastMeasIso = "";
String lastTailLine = "";   // zuletzt gemeldete letzte Datei-Zeile
uint32_t lastLinesSavedAt = 0;

// SD-Zustand: Die Karte wird EINMAL initialisiert und bleibt gemountet.
// Vorher lief alle 5 s ein SD.begin() - auf dem mit dem Regler geteilten
// Bus schlug das dauernd fehl ("begin() fehlgeschlagen").
bool sdMounted = false;
uint32_t sdNextTry = 0;
uint32_t sdBackoff = 15000UL;

// ---------- Globale Objekte ----------
WiFiClient espClient;
PubSubClient mqtt(espClient);

volatile unsigned long spiBlockoutUntil = 0;

void ICACHE_RAM_ATTR onCsSenseFall() {
  spiBlockoutUntil = millis() + 20000UL;
}

// ---------- WLAN ----------
bool connectWifi(uint32_t timeout_ms = 20000UL) {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print(F("[WiFi] Verbinde mit "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.hostname("fysetc-solarlog");  // Name im Router/DHCP (frueher fuer OTA)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(F("."));
    yield();
    if (millis() - start > timeout_ms) {
      Serial.println(F(" Timeout"));
      return false;
    }
  }
  Serial.println();
  Serial.print(F("[WiFi] IP: "));
  Serial.println(WiFi.localIP());
  return true;
}

// ---------- EEPROM Position ----------
uint32_t loadReadPosition() {
  uint8_t magic = EEPROM.read(EEPROM_ADDR_MAGIC);
  if (magic != EEPROM_MAGIC) {
    Serial.println(F("[EEPROM] Erster Start, Position=0"));
    return 0;
  }
  uint32_t pos = 0;
  for (uint8_t i = 0; i < 4; i++) {
    pos |= ((uint32_t)EEPROM.read(EEPROM_ADDR_POS + i)) << (i * 8);
  }
  return pos;
}

void saveReadPosition(uint32_t pos) {
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  for (uint8_t i = 0; i < 4; i++) {
    EEPROM.write(EEPROM_ADDR_POS + i, (pos >> (i * 8)) & 0xFF);
  }
  EEPROM.commit();
}

// ---------- Runtime-Felder (remote aenderbar) ----------
// Grenzen aus fields.h zum Spaltenindex suchen (0/0 = keine Grenze)
// Setzt Grenzen UND Icon aus fields.h (gilt auch nach dem Laden aus EEPROM,
// weil das Icon bewusst nicht im EEPROM liegt).
void applyDefaultLimits(RtField &f) {
  f.lo = 0;
  f.hi = 0;
  f.icon[0] = 0;
  for (uint8_t k = 0; k < NUM_FIELDS; k++) {
    if (FIELDS[k].index == f.index) {
      f.lo = FIELDS[k].lo;
      f.hi = FIELDS[k].hi;
      strncpy(f.icon, FIELDS[k].icon, ICON_LEN - 1);
      f.icon[ICON_LEN - 1] = 0;
      return;
    }
  }
}

void copyDefaultsToRt() {
  rtCount = 0;
  for (uint8_t i = 0; i < NUM_FIELDS && i < MAX_RT_FIELDS; i++) {
    rtFields[i].index = FIELDS[i].index;
    strncpy(rtFields[i].key, FIELDS[i].key, KEY_LEN - 1);
    rtFields[i].key[KEY_LEN - 1] = 0;
    strncpy(rtFields[i].name, FIELDS[i].name, NAME_LEN - 1);
    rtFields[i].name[NAME_LEN - 1] = 0;
    strncpy(rtFields[i].unit, FIELDS[i].unit, UNIT_LEN - 1);
    rtFields[i].unit[UNIT_LEN - 1] = 0;
    strncpy(rtFields[i].icon, FIELDS[i].icon, ICON_LEN - 1);
    rtFields[i].icon[ICON_LEN - 1] = 0;
    rtFields[i].divider = FIELDS[i].divider;
    rtFields[i].lo = FIELDS[i].lo;
    rtFields[i].hi = FIELDS[i].hi;
    rtFields[i].used = true;
    rtFields[i].disc = false;
    rtFields[i].pruned = false;
    rtCount++;
  }
  for (uint8_t i = rtCount; i < MAX_RT_FIELDS; i++) {
    rtFields[i].used = false;
  }
}

void loadRuntimeFields() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t m = EEPROM.read(EEPROM_CFG_MAGIC_ADDR);
  if (m != EEPROM_CFG_MAGIC) {
    Serial.println(F("[CFG] Keine Remote-Config, nehme fields.h"));
    copyDefaultsToRt();
    saveRuntimeFields();
    return;
  }
  uint8_t cnt = EEPROM.read(EEPROM_CFG_COUNT_ADDR);
  if (cnt == 0 || cnt > MAX_RT_FIELDS) {
    Serial.println(F("[CFG] Ungueltige Config, nehme Defaults"));
    copyDefaultsToRt();
    saveRuntimeFields();
    return;
  }
  rtCount = 0;
  int addr = EEPROM_CFG_BASE;
  for (uint8_t i = 0; i < cnt; i++) {
    RtField f;
    f.index = EEPROM.read(addr++);
    for (uint8_t k = 0; k < KEY_LEN; k++) f.key[k] = (char)EEPROM.read(addr++);
    for (uint8_t k = 0; k < NAME_LEN; k++) f.name[k] = (char)EEPROM.read(addr++);
    for (uint8_t k = 0; k < UNIT_LEN; k++) f.unit[k] = (char)EEPROM.read(addr++);
    union { float f; uint8_t b[4]; } u;
    for (uint8_t k = 0; k < 4; k++) u.b[k] = EEPROM.read(addr++);
    f.divider = u.f;
    f.used = (strlen(f.key) > 0);
    applyDefaultLimits(f);
    f.disc = false;
    f.pruned = false;
    rtFields[i] = f;
    if (f.used) rtCount = i + 1;
  }
  Serial.print(F("[CFG] Remote-Config geladen, Felder: "));
  Serial.println(rtCount);
}

void saveRuntimeFields() {
  EEPROM.write(EEPROM_CFG_MAGIC_ADDR, EEPROM_CFG_MAGIC);
  uint8_t cnt = 0;
  for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) if (rtFields[i].used) cnt = i + 1;
  if (cnt == 0) cnt = 1;
  EEPROM.write(EEPROM_CFG_COUNT_ADDR, cnt);
  int addr = EEPROM_CFG_BASE;
  for (uint8_t i = 0; i < cnt; i++) {
    EEPROM.write(addr++, rtFields[i].used ? rtFields[i].index : 0);
    for (uint8_t k = 0; k < KEY_LEN; k++) EEPROM.write(addr++, (uint8_t)rtFields[i].key[k]);
    for (uint8_t k = 0; k < NAME_LEN; k++) EEPROM.write(addr++, (uint8_t)rtFields[i].name[k]);
    for (uint8_t k = 0; k < UNIT_LEN; k++) EEPROM.write(addr++, (uint8_t)rtFields[i].unit[k]);
    union { float f; uint8_t b[4]; } u;
    u.f = rtFields[i].divider;
    for (uint8_t k = 0; k < 4; k++) EEPROM.write(addr++, u.b[k]);
  }
  // Magic fuer Leseposition erhalten
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) {
    EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
    for (uint8_t i = 0; i < 4; i++) EEPROM.write(EEPROM_ADDR_POS + i, 0);
  }
  EEPROM.commit();
}

int findRtByIndex(uint8_t index) {
  for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) {
    if (rtFields[i].used && rtFields[i].index == index) return i;
  }
  return -1;
}

// ---------- Mini-JSON-Parser (ohne extra Library) ----------
String extractJsonString(const String& json, const char* key) {
  String pat = String("\"") + key + String("\"");
  int p = json.indexOf(pat);
  if (p < 0) return String("");
  int c = json.indexOf(':', p);
  if (c < 0) return String("");
  int q1 = json.indexOf('"', c);
  if (q1 < 0) return String("");
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return String("");
  return json.substring(q1 + 1, q2);
}

bool extractJsonInt(const String& json, const char* key, long &out) {
  String pat = String("\"") + key + String("\"");
  int p = json.indexOf(pat);
  if (p < 0) return false;
  int c = json.indexOf(':', p);
  if (c < 0) return false;
  int i = c + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
  int j = i;
  while (j < (int)json.length() && (isDigit(json[j]) || json[j] == '-')) j++;
  if (j == i) return false;
  out = json.substring(i, j).toInt();
  return true;
}

bool extractJsonFloat(const String& json, const char* key, float &out) {
  String pat = String("\"") + key + String("\"");
  int p = json.indexOf(pat);
  if (p < 0) return false;
  int c = json.indexOf(':', p);
  if (c < 0) return false;
  int i = c + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
  int j = i;
  while (j < (int)json.length() && (isDigit(json[j]) || json[j] == '-' || json[j] == '.' || json[j] == 'e' || json[j] == 'E' || json[j] == '+')) j++;
  if (j == i) return false;
  String num = json.substring(i, j);
  num.replace(',', '.');
  out = num.toFloat();
  // 0 ist mehrdeutig, trotzdem als gefunden melden (Sender schickt divider:0 bewusst)
  return true;
}

// ---------- MQTT ----------
void publishConfigState(const String& info) {
  String json = F("{\"fields\":[");
  bool first = true;
  for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) {
    if (!rtFields[i].used) continue;
    if (!first) json += ",";
    first = false;
    json += String("{\"index\":") + String(rtFields[i].index);
    json += String(",\"key\":\"") + rtFields[i].key + "\"";
    json += String(",\"name\":\"") + rtFields[i].name + "\"";
    json += String(",\"unit\":\"") + rtFields[i].unit + "\"";
    json += String(",\"icon\":\"") + rtFields[i].icon + "\"";
    json += String(",\"divider\":") + String(rtFields[i].divider, 2);
    json += String(",\"min\":") + String(rtFields[i].lo, 1);
    json += String(",\"max\":") + String(rtFields[i].hi, 1) + "}";
  }
  json += F("],\"info\":\"");
  json += info;
  json += F("\"}");
  mqtt.publish(MQTT_TOPIC_CONFIGSTATE, json.c_str(), false);
  Serial.print(F("[CFG] state: "));
  Serial.println(json);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String t = String(topic);
  if (t != MQTT_TOPIC_SETCONFIG) return;
  String msg;
  msg.reserve(len + 1);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  Serial.print(F("[MQTT] set_config: "));
  Serial.println(msg);

  String cmd = extractJsonString(msg, "cmd");
  if (cmd == "list") {
    publishConfigState("list");
    return;
  }
  if (cmd == "reset") {
    // Alte Discovery-Eintraege loeschen
    for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) {
      if (rtFields[i].used && strlen(rtFields[i].key) > 0) {
        String disc = String("homeassistant/sensor/solarlog_") + rtFields[i].key + "/config";
        mqtt.publish(disc.c_str(), "", true);
      }
    }
    copyDefaultsToRt();
    saveRuntimeFields();
    publishDiscovery();
    publishConfigState("reset ok");
    return;
  }

  long idxL;
  if (!extractJsonInt(msg, "index", idxL) || idxL < 0 || idxL > 60) {
    mqtt.publish(MQTT_TOPIC_CONFIGSTATE, "{\"error\":\"index fehlt (0..60)\"}", false);
    return;
  }
  uint8_t fidx = (uint8_t)idxL;
  String nkey = extractJsonString(msg, "key");
  String nname = extractJsonString(msg, "name");
  String nunit = extractJsonString(msg, "unit");
  String nicon = extractJsonString(msg, "icon");
  float ndiv;
  bool hasDiv = extractJsonFloat(msg, "divider", ndiv);
  float nlo, nhi;
  bool hasLo = extractJsonFloat(msg, "min", nlo);
  bool hasHi = extractJsonFloat(msg, "max", nhi);

  int pos = findRtByIndex(fidx);
  char oldKey[KEY_LEN] = {0};
  if (pos >= 0) {
    strncpy(oldKey, rtFields[pos].key, KEY_LEN - 1);
    if (nkey.length() > 0) { strncpy(rtFields[pos].key, nkey.c_str(), KEY_LEN - 1); rtFields[pos].key[KEY_LEN-1]=0; }
    if (nname.length() > 0) { strncpy(rtFields[pos].name, nname.c_str(), NAME_LEN - 1); rtFields[pos].name[NAME_LEN-1]=0; }
    if (msg.indexOf("\"unit\"") >= 0) { strncpy(rtFields[pos].unit, nunit.c_str(), UNIT_LEN - 1); rtFields[pos].unit[UNIT_LEN-1]=0; }
    if (msg.indexOf("\"icon\"") >= 0) { strncpy(rtFields[pos].icon, nicon.c_str(), ICON_LEN - 1); rtFields[pos].icon[ICON_LEN-1]=0; }
    if (hasDiv) rtFields[pos].divider = ndiv;
    if (hasLo) rtFields[pos].lo = nlo;
    if (hasHi) rtFields[pos].hi = nhi;
  } else {
    // neuer Slot
    int freeSlot = -1;
    for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) if (!rtFields[i].used) { freeSlot = i; break; }
    if (freeSlot < 0) {
      mqtt.publish(MQTT_TOPIC_CONFIGSTATE, "{\"error\":\"kein freier Slot (max 14)\"}", false);
      return;
    }
    if (nkey.length() == 0) {
      mqtt.publish(MQTT_TOPIC_CONFIGSTATE, "{\"error\":\"key fehlt fuer neues Feld\"}", false);
      return;
    }
    rtFields[freeSlot].index = fidx;
    strncpy(rtFields[freeSlot].key, nkey.c_str(), KEY_LEN - 1); rtFields[freeSlot].key[KEY_LEN-1]=0;
    if (nname.length() > 0) strncpy(rtFields[freeSlot].name, nname.c_str(), NAME_LEN - 1);
    else snprintf(rtFields[freeSlot].name, NAME_LEN, "Solar Feld %d", fidx);
    strncpy(rtFields[freeSlot].unit, nunit.c_str(), UNIT_LEN - 1); rtFields[freeSlot].unit[UNIT_LEN-1]=0;
    if (nicon.length() > 0) { strncpy(rtFields[freeSlot].icon, nicon.c_str(), ICON_LEN - 1); rtFields[freeSlot].icon[ICON_LEN-1]=0; }
    rtFields[freeSlot].divider = hasDiv ? ndiv : 0;
    applyDefaultLimits(rtFields[freeSlot]);
    if (hasLo) rtFields[freeSlot].lo = nlo;
    if (hasHi) rtFields[freeSlot].hi = nhi;
    rtFields[freeSlot].used = true;
    pos = freeSlot;
  }

  saveRuntimeFields();

  // Falls key umbenannt: alte Discovery loeschen
  if (strlen(oldKey) > 0 && strcmp(oldKey, rtFields[pos].key) != 0) {
    String oldDisc = String("homeassistant/sensor/solarlog_") + oldKey + "/config";
    mqtt.publish(oldDisc.c_str(), "", true);
  }
  publishDiscoverySingle((uint8_t)pos);
  publishConfigState("saved");
}

bool connectMqtt() {
  if (mqtt.connected()) return true;
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(mqttCallback);
  Serial.print(F("[MQTT] Verbinde mit "));
  Serial.print(MQTT_BROKER);
  Serial.print(F(":"));
  Serial.println(MQTT_PORT);
  bool ok;
  if (strlen(MQTT_USER) == 0) {
    ok = mqtt.connect(MQTT_CLIENT_ID);
  } else {
    ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
  }
  if (ok) {
    Serial.println(F("[MQTT] verbunden"));
    mqtt.subscribe(MQTT_TOPIC_SETCONFIG);
    publishIp();
    {
      String ip = WiFi.localIP().toString();
      String online = String("{\"online\":true,\"ip\":\"") + ip + "\",";
      online += String("\"rssi\":") + String(WiFi.RSSI()) + "}";
      mqtt.publish(MQTT_TOPIC_STATUS, online.c_str(), false);
    }
    publishDiscovery();
  } else {
    Serial.print(F("[MQTT] Fehler, rc="));
    Serial.println(mqtt.state());
  }
  return ok;
}

// ---------- SD-Bus ----------
bool canTakeBus() {
  if ((long)(millis() - spiBlockoutUntil) < 0) return false;
  if (digitalRead(CS_SENSE_PIN) == LOW) {
    spiBlockoutUntil = millis() + 5000UL;
    return false;
  }
  return true;
}

void takeBus() {
  pinMode(MISO_PIN, SPECIAL);
  pinMode(MOSI_PIN, SPECIAL);
  pinMode(SCK, SPECIAL);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  delay(5);
}

void releaseBus() {
  pinMode(MISO_PIN, INPUT);
  pinMode(MOSI_PIN, INPUT);
  pinMode(SCK, INPUT);
  pinMode(SD_CS_PIN, INPUT);
}

// ---------- Feld-Helfer ----------
String getFieldToken(const String& line, uint8_t wanted) {
  int start = 0;
  uint8_t idx = 0;
  while (true) {
    int pos = line.indexOf(';', start);
    String tok;
    if (pos < 0) {
      tok = line.substring(start);
      tok.trim();
      if (idx == wanted) return tok;
      return String("");
    } else {
      tok = line.substring(start, pos);
      tok.trim();
      if (idx == wanted) return tok;
      start = pos + 1;
      idx++;
      if (idx > 60) return String("");
    }
    yield();
  }
}

void publishDiscoverySingle(uint8_t rtIdx) {
  if (rtIdx >= MAX_RT_FIELDS || !rtFields[rtIdx].used) return;
  RtField &f = rtFields[rtIdx];
  String discTopic = String("homeassistant/sensor/solarlog_") + f.key + "/config";
  String stateTopic = String("home/solarlog/") + f.key;
  String payload = String("{\"name\":\"") + f.name + "\",";
  payload += String("\"state_topic\":\"") + stateTopic + "\",";
  payload += String("\"unique_id\":\"solarlog_") + f.key + "\",";
  payload += String("\"device\":{\"identifiers\":[\"fysetc-solarlog\"],\"name\":\"Solarlog\",\"manufacturer\":\"") + MANUFACTURER_NAME + String("},");
  if (strlen(f.unit) > 0) {
    payload += String("\"unit_of_measurement\":\"") + f.unit + "\",";
  }
  if (strlen(f.icon) > 0) {
    payload += String("\"icon\":\"") + f.icon + "\",";
  }
  payload += String("\"value_template\":\"{{ value }}\"}");
  mqtt.publish(discTopic.c_str(), payload.c_str(), true);
  Serial.print(F("[DISC] "));
  Serial.print(f.key);
  Serial.print(F(" -> "));
  Serial.println(discTopic);
}

void publishIp() {
  if (WiFi.status() != WL_CONNECTED) return;
  String ip = WiFi.localIP().toString();
  mqtt.publish(MQTT_TOPIC_IP, ip.c_str(), true);
  Serial.print(F("[IP] "));
  Serial.println(ip);
}

// Zeitstempel gehen als UTC mit "Z" raus (z.B. 2026-11-06T20:44:32Z).
// HA rechnet selbst auf Ortszeit um -> keine doppelte Zeitzone mehr.
// (Vorher: localtime + TZ-Suffix -> der ESP lag 2 h in der Zukunft.)
long daysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097L + (long)doe - 719468L;
}

int daysInMonthOf(int y, int m) {
  static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) return 29;
  if (m < 1 || m > 12) return 30;
  return dm[m - 1];
}

// 0 = Sonntag ... 6 = Samstag
int weekdayOf(int y, int m, int d) {
  long days = daysFromCivil(y, m, d) + 4;  // 1970-01-01 war ein Donnerstag
  int wd = (int)(days % 7);
  if (wd < 0) wd += 7;
  return wd;
}

int lastSundayOf(int y, int m) {
  int dim = daysInMonthOf(y, m);
  int wd = weekdayOf(y, m, dim);
  return dim - wd;
}

// Deutsche Sommerzeit: letzter So im Maerz 02:00 bis letzter So im Oktober 03:00
int deOffsetSec(int y, int m, int d, int h) {
  int s = lastSundayOf(y, 3);
  int e = lastSundayOf(y, 10);
  bool dst;
  if (m > 3 && m < 10) dst = true;
  else if (m == 3) dst = (d > s) || (d == s && h >= 2);
  else if (m == 10) dst = (d < e) || (d == e && h < 3);
  else dst = false;
  return dst ? 7200 : 3600;
}

String isoUtc(time_t t) {
  struct tm *g = gmtime(&t);
  if (!g || g->tm_year < 120) return String("");
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
           g->tm_hour, g->tm_min, g->tm_sec);
  return String(buf);
}

String nowIso() {
  time_t now = time(nullptr);
  if (now < 1600000000) return String("");  // noch kein NTP
  return isoUtc(now);
}

// Startzeit des ESP (now - Laufzeit) als vollwertiger Zeitstempel
String bootIso() {
  time_t now = time(nullptr);
  if (now < 1600000000) return String("");
  return isoUtc(now - (time_t)(millis() / 1000));
}

// Baut aus "1106;1830;..." den echten Messzeitpunkt: 1106=MMDD, 1830=HHMM.
// Gibt ISO zurueck, z.B. 2026-11-06T18:30:00. Jahr aus NTP, mit Jahreswechsel-Korrektur.
String measIsoFromLine(const String& clean) {
  String s0 = getFieldToken(clean, 0);
  String s1 = getFieldToken(clean, 1);
  if (s0.length() < 3 || s1.length() < 3) return String("");
  // Nur Ziffern behalten (manche Regler senden "1106." o.ae.)
  String d0 = "", d1 = "";
  for (unsigned int i = 0; i < s0.length(); i++) if (isDigit(s0[i])) d0 += s0[i];
  for (unsigned int i = 0; i < s1.length(); i++) if (isDigit(s1[i])) d1 += s1[i];
  if (d0.length() != 4 || d1.length() != 4) return String("");
  int mm = d0.substring(0, 2).toInt();
  int dd = d0.substring(2, 4).toInt();
  int hh = d1.substring(0, 2).toInt();
  int mi = d1.substring(2, 4).toInt();
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59) return String("");
  time_t now = time(nullptr);
  struct tm *tn = localtime(&now);
  int year = (tn && tn->tm_year >= 120) ? tn->tm_year + 1900 : 2026;
  // Jahreswechsel: Log aus Dez, ESP schon im Jan -> Vorjahr nehmen (und umgekehrt)
  if (tn && tn->tm_year >= 120) {
    int curM = tn->tm_mon + 1;
    if (curM == 1 && mm == 12) year--;
    if (curM == 12 && mm == 1) year++;
  }
  // Logzeit ist Ortszeit -> in UTC umrechnen (Sommerzeit des Messtags beachten)
  time_t utc = (time_t)daysFromCivil(year, mm, dd) * 86400L
             + (time_t)hh * 3600 + (time_t)mi * 60
             - (time_t)deOffsetSec(year, mm, dd, hh);
  return isoUtc(utc);
}

uint32_t loadLinesTotal() {
  uint32_t v = 0;
  for (uint8_t i = 0; i < 4; i++) v |= ((uint32_t)EEPROM.read(EEPROM_LINES_ADDR + i)) << (i * 8);
  // 0xFFFFFFFF = nie geschrieben. Alles ueber 5 Mio. ist EEPROM-Muell
  // (z.B. aus der alten Adresse 400) und wird verworfen.
  if (v == 0xFFFFFFFF || v > 5000000UL) return 0;
  return v;
}

void saveLinesTotal(uint32_t v) {
  for (uint8_t i = 0; i < 4; i++) EEPROM.write(EEPROM_LINES_ADDR + i, (v >> (i * 8)) & 0xFF);
  EEPROM.commit();
}

void teleDisc(const char* key, const char* name, const char* stateTopic, const char* unit, const char* devClass) {
  String disc = String("homeassistant/sensor/solarlog_") + key + "/config";
  String p = String("{\"name\":\"") + name + "\",";
  p += String("\"state_topic\":\"") + stateTopic + "\",";
  p += String("\"unique_id\":\"solarlog_") + key + "\",";
  p += String("\"device\":{\"identifiers\":[\"fysetc-solarlog\"],\"name\":\"Solarlog\",\"manufacturer\":\"") + MANUFACTURER_NAME + String("}");
  if (unit && strlen(unit) > 0) p += String(",\"unit_of_measurement\":\"") + unit + "\"";
  if (devClass && strlen(devClass) > 0) p += String(",\"device_class\":\"") + devClass + "\"";
  p += String(",\"value_template\":\"{{ value }}\"}");
  mqtt.publish(disc.c_str(), p.c_str(), true);
}

void publishTelemetryDiscovery() {
  // Zwei getrennte Zeiten: ESP-Abruf (last_refresh) und Zeit der Logzeile
  teleDisc("meas_time", "Solarlog Letzte Logzeile", MQTT_TOPIC_MEASTIME, "", "timestamp");
  teleDisc("last_refresh", "Solarlog Letzter Refresh", MQTT_TOPIC_LASTREFRESH, "", "timestamp");
  teleDisc("file", "Solarlog Datei", MQTT_TOPIC_FILE, "", "");
  teleDisc("size", "Solarlog Dateigroesse", MQTT_TOPIC_SIZE, "B", "data_size");
  teleDisc("pos", "Solarlog Leseposition", MQTT_TOPIC_POS, "B", "");
  teleDisc("rssi", "Solarlog WLAN", MQTT_TOPIC_RSSI, "dBm", "signal_strength");
  teleDisc("uptime", "Solarlog Start", MQTT_TOPIC_UPTIME, "", "timestamp");
  teleDisc("heap", "Solarlog Heap", MQTT_TOPIC_HEAP, "B", "data_size");

}

// Telemetrie (RSSI, Heap, Dateigroesse, Leseposition) NICHT bei jedem Poll
// senden: nur bei Aenderung oder alle 10 Minuten. Das spart WLAN-Traffic, CPU
// und damit Waerme - der ESP funkt nur noch, wenn es sein muss.
#define TELEMETRY_INTERVAL_MS 600000UL

void publishTelemetry(uint32_t fileSize, uint32_t lastPos) {
  if (!mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  static uint32_t lastSent = 0;
  static uint32_t lastSize = 0xFFFFFFFFUL;
  static uint32_t lastPosSent = 0xFFFFFFFFUL;
  bool changed = (fileSize != lastSize) || (lastPos != lastPosSent);
  bool due = (lastSent == 0) || ((uint32_t)(millis() - lastSent) >= TELEMETRY_INTERVAL_MS);
  if (!changed && !due) return;
  lastSent = millis();
  lastSize = fileSize;
  lastPosSent = lastPos;
  String ip = WiFi.localIP().toString();
  if (lastRefreshIso.length() == 0) lastRefreshIso = nowIso();
  mqtt.publish(MQTT_TOPIC_IP, ip.c_str(), true);
  if (currentLogFile.length() > 0) mqtt.publish(MQTT_TOPIC_FILE, currentLogFile.c_str(), true);
  mqtt.publish(MQTT_TOPIC_SIZE, String(fileSize).c_str(), true);
  mqtt.publish(MQTT_TOPIC_POS, String(lastPos).c_str(), true);
  mqtt.publish(MQTT_TOPIC_RSSI, String(WiFi.RSSI()).c_str(), true);
  String boot = bootIso();
  if (boot.length() > 0) mqtt.publish(MQTT_TOPIC_UPTIME, boot.c_str(), true);
  mqtt.publish(MQTT_TOPIC_HEAP, String(ESP.getFreeHeap()).c_str(), true);
  // retained: sonst steht nach HA-Neustart "Unbekannt"
  if (lastRefreshIso.length() > 0) mqtt.publish(MQTT_TOPIC_LASTREFRESH, lastRefreshIso.c_str(), true);
  if (lastMeasIso.length() > 0) mqtt.publish(MQTT_TOPIC_MEASTIME, lastMeasIso.c_str(), true);
  String st = String("{\"size\":") + String(fileSize) +
              String(",\"pos\":") + String(lastPos) +
              String(",\"file\":\"") + currentLogFile +
              String(",\"ip\":\"") + ip + String("\",\"rssi\":") + String(WiFi.RSSI()) +
              String(",\"uptime\":") + String(millis() / 1000) +
              String(",\"heap\":") + String(ESP.getFreeHeap()) +
              String(",\"last_refresh\":\"") + lastRefreshIso + String("\",\"meas_time\":\"") + lastMeasIso + String("\"}");
  mqtt.publish(MQTT_TOPIC_STATUS, st.c_str(), false);
}

void publishDiscovery() {
  // Bewusst KEINE Feld-Discovery beim Start: die entsteht erst mit dem ersten
  // gueltigen Wert (publishFields). Sonst legt HA Entities fuer Felder an, die
  // dauerhaft 199 = "Ausser Betrieb" liefern (T3/T5).
  publishTelemetryDiscovery();
  // Nicht mehr benutzte Entities entfernen (sonst bleiben sie mit alten
  // Werten stehen): Zeilen-Zaehler und der falsch gemappte Durchfluss.
  mqtt.publish("homeassistant/sensor/solarlog_lines_total/config", "", true);
  mqtt.publish("homeassistant/sensor/solarlog_durchfluss/config", "", true);
  // Die Diagnose-Entity "Letzte Rohzeile" gibt es nicht mehr -> Reste aus
  // Home Assistant und den retained Wert entfernen.
  mqtt.publish("homeassistant/sensor/solarlog_last_line/config", "", true);
  mqtt.publish("home/solarlog/last_line", "", true);
  // Eigene IP als eigener HA-Sensor (Remote ablesbar)
  {
    String discTopic = "homeassistant/sensor/solarlog_ip/config";
    String payload = String("{\"name\":\"Solarlog IP\",");
    payload += String("\"state_topic\":\"") + MQTT_TOPIC_IP + "\",";
    payload += String("\"unique_id\":\"solarlog_ip\",");
    payload += String("\"icon\":\"mdi:ip-network\",");
    payload += String("\"device\":{\"identifiers\":[\"fysetc-solarlog\"],\"name\":\"Solarlog\",\"manufacturer\":\"") + MANUFACTURER_NAME + String("}");
    payload += String(",\"value_template\":\"{{ value }}\"}");
    mqtt.publish(discTopic.c_str(), payload.c_str(), true);
  }
}

String fmtUhrzeit(const String& tok);
String fmtDatum(const String& tok);

// ---------- Plausibilitaet ----------
// Zahl aus einem Token lesen; false bei Text wie "EU", "-" oder leer
bool tokenToNumber(const String& tok, float &out) {
  String s = tok;
  s.trim();
  s.replace(',', '.');
  if (s.length() == 0 || s.length() > 12) return false;
  bool digit = false;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isDigit(c)) { digit = true; continue; }
    if (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') continue;
    return false;
  }
  if (!digit) return false;
  out = s.toFloat();
  return true;
}

// 0/0 = keine Grenze. Sonst gilt lo..hi als plausibel.
bool plausibleValue(const RtField &f, float v) {
  if (f.lo == 0 && f.hi == 0) return true;
  if (v < f.lo) return false;
  if (f.hi != 0 && v > f.hi) return false;
  return true;
}

// Lieber gar keinen Wert als "unbekannt"/199: bestehenden (retained) Wert
// loeschen. HA graut die Entity dann aus, statt einen falschen Messwert
// ins Diagramm zu schreiben.
void clearFieldTopic(const String& topic) {
  mqtt.publish(topic.c_str(), "", true);
}

// Ungueltiger Wert (199 = Ausser Betrieb, -40 = Fuehler fehlt, Text, leer):
//  - Feld war schon in HA -> nur den Wert leeren, Entity wird grau
//  - Feld war nie gueltig -> Discovery loeschen, damit gar keine Entity entsteht
void dropField(uint8_t i, const String& tok) {
  RtField &f = rtFields[i];
  Serial.print(F("[MQTT] unplausibel, nicht gesendet: "));
  Serial.print(f.key);
  Serial.print('=');
  Serial.println(tok);
  if (f.disc) { clearFieldTopic(String("home/solarlog/") + f.key); return; }
  if (f.pruned) return;
  String disc = String("homeassistant/sensor/solarlog_") + f.key + "/config";
  mqtt.publish(disc.c_str(), "", true);
  f.pruned = true;
}

// Letzte vollstaendige Zeile der Datei (ohne Position zu veraendern)
String lastLineOfFile() {
  File f = SD.open(currentLogFile.c_str(), FILE_READ);
  if (!f) return String("");
  uint32_t sz = f.size();
  if (sz == 0) { f.close(); return String(""); }
  uint32_t start = (sz > 2048) ? sz - 2048 : 0;
  if (!f.seek(start)) { f.close(); return String(""); }
  String tail;
  tail.reserve(2048);
  while (f.available()) tail += (char)f.read();
  f.close();
  // Der Regler trennt Zeilen mit CR (teils CRLF). Ohne Normalisierung fand
  // lastIndexOf('\n') nichts und "letzte Zeile" war der ganze 2-KB-Block.
  tail.replace("\r\n", "\n");
  tail.replace('\r', '\n');
  while (tail.length() > 0 && tail[tail.length() - 1] == '\n') tail.remove(tail.length() - 1);
  int nl = tail.lastIndexOf('\n');
  if (nl >= 0) tail = tail.substring(nl + 1);
  tail.trim();
  if (tail.indexOf(';') < 0) return String("");
  return tail;
}

// Beim ersten Zugriff: letzte Zeile einmal auswerten -> sofort Werte in HA
// und ungueltige Felder werden gar nicht erst angelegt.
// Liest die letzte Zeile der Datei und meldet sie nur, wenn sie sich seit dem
// letzten Mal geaendert hat. Das ist unabhaengig von der gespeicherten
// Leseposition und funktioniert auch, wenn die Datei neu angelegt wird.
void refreshFromTail() {
  String line = lastLineOfFile();
  if (line.length() == 0) return;
  if (line == lastTailLine) return;
  lastTailLine = line;
  Serial.print(F("[SD] neue letzte Zeile: "));
  Serial.println(line);
  publishFields(line);
  String mt = measIsoFromLine(line);
  if (mt.length() > 0) {
    lastMeasIso = mt;
    mqtt.publish(MQTT_TOPIC_MEASTIME, mt.c_str(), true);
  }
  String iso = nowIso();
  if (iso.length() > 0) {
    lastRefreshIso = iso;
    mqtt.publish(MQTT_TOPIC_LASTREFRESH, iso.c_str(), true);
  }
}

void publishFields(const String& clean) {
  for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) {
    if (!rtFields[i].used) continue;
    RtField &f = rtFields[i];
    String tok = getFieldToken(clean, f.index);
    tok.trim();
    String topic = String("home/solarlog/") + f.key;
    if (tok.length() == 0) { clearFieldTopic(topic); continue; }
    String out = tok;
    // Spalte 0 = MMDD (Datum), Spalte 1 = HHMM (Uhrzeit) -> lesbar formatieren
    bool isTimeKey = (strcmp(f.key, "uhrzeit") == 0 || strcmp(f.key, "time") == 0);
    bool isDateKey = (strcmp(f.key, "datum") == 0 || strcmp(f.key, "date") == 0);
    if (isTimeKey || (f.index == 1 && f.divider == 0)) {
      out = fmtUhrzeit(tok);
    } else if (isDateKey || (f.index == 0 && f.divider == 0)) {
      out = fmtDatum(tok);
    } else {
      float v;
      if (!tokenToNumber(tok, v) || !plausibleValue(f, v)) {
        dropField(i, tok);
        continue;
      }
      if (f.divider == 0) {
        out = String((long)(v < 0 ? v - 0.5f : v + 0.5f));
      } else {
        out = String(v / f.divider, 1);
      }
    }
    if (out.length() == 0) { dropField(i, tok); continue; }
    // Entity erst anlegen, wenn wirklich ein gueltiger Wert vorliegt
    if (!f.disc) { publishDiscoverySingle(i); f.disc = true; f.pruned = false; }
    // retained: sonst steht nach HA-/Broker-Neustart wieder "Unbekannt"
    mqtt.publish(topic.c_str(), out.c_str(), true);
  }
}

// "2110" -> "21:10"   |   "1106" -> "06.11.2026"
String fmtUhrzeit(const String& tok) {
  String d = "";
  for (unsigned int i = 0; i < tok.length(); i++) if (isDigit(tok[i])) d += tok[i];
  if (d.length() < 3) return tok;
  while (d.length() < 4) d = "0" + d;
  return d.substring(0, 2) + ":" + d.substring(2, 4);
}

String fmtDatum(const String& tok) {
  String d = "";
  for (unsigned int i = 0; i < tok.length(); i++) if (isDigit(tok[i])) d += tok[i];
  if (d.length() != 4) return tok;
  int mm = d.substring(0, 2).toInt();
  int dd = d.substring(2, 4).toInt();
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31) return tok;
  // Jahr aus NTP, mit Jahreswechsel-Korrektur (Log Dez, ESP schon Jan)
  time_t now = time(nullptr);
  struct tm *tn = localtime(&now);
  int year = (tn && tn->tm_year >= 120) ? tn->tm_year + 1900 : 2026;
  if (tn && tn->tm_year >= 120) {
    int curM = tn->tm_mon + 1;
    if (curM == 1 && mm == 12) year--;
    if (curM == 12 && mm == 1) year++;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d.%02d.%04d", dd, mm, year);
  return String(buf);
}

void mqttPublishLine(const String& line, uint32_t pos) {
  String clean = line;
  clean.trim();
  if (clean.length() == 0) return;
  publishFields(clean);
  String mtime = measIsoFromLine(clean);
  if (mtime.length() > 0) {
    lastMeasIso = mtime;
    mqtt.publish(MQTT_TOPIC_MEASTIME, mtime.c_str(), true);
  }
  linesTotal++;
  String iso = nowIso();
  if (iso.length() > 0) lastRefreshIso = iso;
  if ((linesTotal % 10) == 0) saveLinesTotal(linesTotal);
  else if (millis() - lastLinesSavedAt > 60000UL) { saveLinesTotal(linesTotal); lastLinesSavedAt = millis(); }
  if (lastRefreshIso.length() > 0) mqtt.publish(MQTT_TOPIC_LASTREFRESH, lastRefreshIso.c_str(), true);
  String json = F("{\"raw\":\"");
  json += clean;
  json += F("\",\"pos\":");
  json += String(pos);
  json += F(",\"file\":\"");
  json += currentLogFile;
  if (mtime.length() > 0) { json += F(",\"mtime\":\""); json += mtime; json += F("\""); }
  json += F("\"}");
  if (json.length() > 900) {
    Serial.println(F("[MQTT] Zeile zu lang, gekuerzt gesendet"));
    json = json.substring(0, 900) + F("\"}");
  }
  bool ok = mqtt.publish(MQTT_TOPIC_SOLAR, json.c_str(), false);
  Serial.print(ok ? F("[MQTT] gesendet: ") : F("[MQTT] FEHLER bei: "));
  Serial.println(clean);
}

// ---------- Datei leeren ----------
// ---------- Monats-Dateiname (WEJJMM.DAT) ----------
// time() kommt als UTC vom NTP. Vorher stand hier zusaetzlich
// setenv("TZ", "CET-1CEST,...") - dadurch lag der ESP 2 Stunden in der
// Zukunft. Die Ortszeit rechnet jetzt HA beim Anzeigen aus.
void setupTime() {
  configTime(0, 0, NTP_SERVER);
}

String monthFilenameFor(time_t t) {
  struct tm *tm = localtime(&t);
  if (!tm || tm->tm_year < 120) return String(""); // vor 2020 = noch kein NTP
  char buf[16];
  snprintf(buf, sizeof(buf), "%s%02d%02d%s", SD_PREFIX, (tm->tm_year + 1900) % 100, tm->tm_mon + 1, SD_SUFFIX);
  return String(buf);
}

String activeLogFile() {
  time_t now = time(nullptr);
  String m = monthFilenameFor(now);
  if (m.length() > 0) return m;
  if (currentLogFile.length() > 0) return currentLogFile;
  return String(SD_FILENAME_FALLBACK);
}

bool isLogFile(const String& name) {
  if (!name.startsWith(SD_PREFIX)) return false;
  if (!name.endsWith(SD_SUFFIX)) return false;
  if (name.length() != 10) return false; // WE2611.DAT
  return true;
}

// Loescht alles was NICHT der aktuelle Monat ist, z.B. im Dezember das WE2611.DAT.
// Die aktuelle Datei wird nie angefasst.
void cleanupOldMonthFiles(const String& keep) {
  File root = SD.open("/");
  if (!root) return;
  if (!root.isDirectory()) { root.close(); return; }
  while (true) {
    File e = root.openNextFile();
    if (!e) break;
    String n = String(e.name());
    // SD-Lib liefert teils mit / vorne
    if (n.startsWith("/")) n = n.substring(1);
    bool isDir = e.isDirectory();
    e.close();
    if (isDir) continue;
    if (!isLogFile(n)) continue;
    // Nur Dateien AELTER als der aktuelle Monat loeschen (WEYYMM ist
    // sortierbar) - bei falscher Uhr kann so nie der neue Monat weg.
    if (n >= keep) continue;
    Serial.print(F("[SD] Alter Monat, loesche: "));
    Serial.println(n);
    if (SD.remove(n.c_str())) {
      String msg = String("{\"deleted\":\"") + n + "\",\"keep\":\"" + keep + "\"}";
      mqtt.publish(MQTT_TOPIC_STATUS, msg.c_str(), false);
    } else {
      Serial.print(F("[SD] Loeschen fehlgeschlagen: "));
      Serial.println(n);
    }
    yield();
  }
  root.close();
}

void updateActiveFile() {
  String neu = activeLogFile();
  if (neu != currentLogFile) {
    Serial.print(F("[SD] Monatswechsel: "));
    Serial.print(currentLogFile);
    Serial.print(F(" -> "));
    Serial.println(neu);
    currentLogFile = neu;
    // Neue Datei = von vorn lesen
    saveReadPosition(0);
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("=== SolarLogReader startet (Remote-Config) ==="));

  EEPROM.begin(EEPROM_SIZE);
  loadRuntimeFields();

  pinMode(CS_SENSE_PIN, INPUT_PULLUP);
  attachInterrupt(CS_SENSE_PIN, onCsSenseFall, FALLING);

  connectWifi();
  setupTime();
  // Kurz auf NTP warten (max 5s), sonst Fallback-Datei
  for (uint8_t i = 0; i < 10; i++) {
    time_t now = time(nullptr);
    struct tm *tm = localtime(&now);
    if (tm && tm->tm_year >= 120) break;
    delay(500);
  }
  currentLogFile = activeLogFile();
  lastActiveFile = currentLogFile;
  Serial.print(F("[SD] Aktive Monats-Datei: "));
  Serial.println(currentLogFile);
  linesTotal = loadLinesTotal();
  lastRefreshIso = nowIso();
  Serial.print(F("[CFG] linesTotal: "));
  Serial.println(linesTotal);
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setBufferSize(2048);
  mqtt.setCallback(mqttCallback);
  connectMqtt();

  Serial.print(F("[CFG] Datei: "));
  Serial.println(currentLogFile);
  Serial.print(F("[CFG] Reset ab Bytes: "));
  Serial.println(MAX_FILE_SIZE_BYTES);
  Serial.print(F("[CFG] Start-Position: "));
  Serial.println(loadReadPosition());
}

// Notausgang: Wird eine einzelne Logdatei doch einmal 1,5 GB gross (z. B. wenn
// die Monatsloeschung nie greift), wird sie geleert. Bei ~1 MB pro Monat
// passiert das praktisch nie.
bool resetFile() {
  Serial.println(F("[SD] Reset: Datei wird geleert..."));
  updateActiveFile();
  if (SD.exists(currentLogFile.c_str())) {
    if (!SD.remove(currentLogFile.c_str())) {
      Serial.println(F("[SD] FEHLER: remove() schlug fehl"));
      return false;
    }
  }
  File f = SD.open(currentLogFile.c_str(), FILE_WRITE);
  if (!f) {
    Serial.println(F("[SD] FEHLER: Datei konnte nicht neu angelegt werden"));
    return false;
  }
  f.close();
  saveReadPosition(0);
  mqtt.publish(MQTT_TOPIC_STATUS, "{\"reset\":true,\"reason\":\"1.5GB erreicht, Datei geleert\"}");
  Serial.println(F("[SD] Datei geleert, Position=0"));
  return true;
}

void loop() {
  static uint32_t lastPoll = 0;
  mqtt.loop();

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
    delay(1000);
    return;
  }
  if (!mqtt.connected()) {
    connectMqtt();
    delay(2000);
    if (!mqtt.connected()) return;
  }

  if (millis() - lastPoll < READ_INTERVAL_MS) {
    delay(50);
    return;
  }
  lastPoll = millis();

  if (!canTakeBus()) {
    Serial.println(F("[SD] Regler aktiv, uebersprungen"));
    return;
  }

  updateActiveFile();
  // Monatswechsel erkannt -> alte Monats-Datei loeschen
  static String lastCleanupFile = "";
  static uint32_t lastCleanupMs = 0;
  bool monthChanged = (lastCleanupFile != currentLogFile);

  takeBus();

  // Nur beim Start bzw. nach einem Fehler initialisieren. Auf dem geteilten
  // Bus ist ein staendiges Re-Init der sicherste Weg ins Dauerfehlschlagen.
  if (!sdMounted) {
    if ((long)(millis() - sdNextTry) < 0) { releaseBus(); return; }
    bool ok = SD.begin(SD_CS_PIN, SPI_HALF_SPEED);   // halbe Taktrate = robuster
    if (!ok) { delay(50); ok = SD.begin(SD_CS_PIN, SPI_HALF_SPEED); }
    if (!ok) {
      Serial.print(F("[SD] Init fehlgeschlagen, naechster Versuch in "));
      Serial.print(sdBackoff / 1000);
      Serial.println(F(" s"));
      sdBackoff = (sdBackoff < 60000UL) ? sdBackoff * 2 : 60000UL;
      sdNextTry = millis() + sdBackoff;
      releaseBus();
      return;
    }
    sdMounted = true;
    sdBackoff = 15000UL;
    Serial.println(F("[SD] Karte bereit"));
  }

  // Alte Monate loeschen: bei Monatswechsel sofort, sonst max 1x pro Stunde
  if (monthChanged || (millis() - lastCleanupMs > 3600000UL)) {
    cleanupOldMonthFiles(currentLogFile);
    lastCleanupFile = currentLogFile;
    lastCleanupMs = millis();
  }

  if (!SD.exists(currentLogFile.c_str())) {
    Serial.print(F("[SD] Datei fehlt, lege an: "));
    Serial.println(currentLogFile);
    File nf = SD.open(currentLogFile.c_str(), FILE_WRITE);
    if (nf) nf.close();
    saveReadPosition(0);
    releaseBus();
    return;
  }

  File f = SD.open(currentLogFile.c_str(), FILE_READ);
  if (!f) {
    // Karte/Dateisystem hat sich verschluckt -> beim naechsten Mal neu mounten
    Serial.println(F("[SD] open() fehlgeschlagen, mounte neu"));
    sdMounted = false;
    sdNextTry = millis() + 5000UL;
    releaseBus();
    return;
  }

  uint32_t fileSize = f.size();
  uint32_t lastPos = loadReadPosition();

  Serial.print(F("[SD] size="));
  Serial.print(fileSize);
  Serial.print(F(" lastPos="));
  Serial.println(lastPos);

  publishTelemetry(fileSize, lastPos);

  // Letzte Zeile der Datei pruefen: beim Start sofort, danach zyklisch.
  // Vorher lief das nur einmal im Setup -> neue Werte kamen ausschliesslich
  // nach einem Neustart an, weil das Nachlesen ab lastPos nichts lieferte.
  static bool tailInit = false;
  static uint32_t lastTailMs = 0;
  if (!tailInit || (millis() - lastTailMs >= TAIL_CHECK_MS)) {
    tailInit = true;
    lastTailMs = millis();
    refreshFromTail();
  }

  if (fileSize >= MAX_FILE_SIZE_BYTES) {
    f.close();
    resetFile();
    releaseBus();
    return;
  }

  if (lastPos > fileSize) {
    Serial.println(F("[SD] lastPos > size, starte bei 0"));
    lastPos = 0;
  }

  if (fileSize == lastPos) {
    f.close();
    releaseBus();
    Serial.println(F("[SD] nichts Neues"));
    return;
  }

  if (!f.seek(lastPos)) {
    Serial.println(F("[SD] seek fehlgeschlagen, lese ab 0"));
    lastPos = 0;
    f.seek(0);
  }

  // Zeilen enden bei diesem Regler mit CR (teils CRLF). readStringUntil('\n')
  // las deshalb die GANZE Datei als eine Zeile und lieferte nur die Spalten
  // des ersten Records. Jetzt zeichenweise lesen und an CR UND LF trennen;
  // lastPos bleibt auf der letzten vollstaendigen Zeile stehen.
  uint8_t sent = 0;
  String part;
  while (f.available() && sent < MAX_LINES_PER_LOOP) {
    int ci = f.read();
    if (ci < 0) break;
    char c = (char)ci;
    if (c == '\r' || c == '\n') {
      String rec = part;
      part = "";
      rec.trim();
      if (rec.length() > 0) {
        lastPos = f.position();
        mqttPublishLine(rec, lastPos);
        saveReadPosition(lastPos);
        sent++;
      }
      yield();
      continue;
    }
    part += c;
    if (part.length() > 1500) part = "";  // Muellzeile, nicht weiter sammeln
    yield();
  }

  if (f.available()) {
    Serial.println(F("[SD] noch mehr neue Zeilen, Rest naechsten Loop"));
  }
  saveLinesTotal(linesTotal);
  publishTelemetry(fileSize, lastPos);

  f.close();
  releaseBus();
  mqtt.loop();
}
