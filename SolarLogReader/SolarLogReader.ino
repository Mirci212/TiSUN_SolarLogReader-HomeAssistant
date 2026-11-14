// SolarLogReader.ino
// ESP8285 (Generic ESP8285 Module) + FYSETC SD WiFi Card Reader V2.1
// + TiSun MULTIPLEX -> MQTT -> Home Assistant
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
#include <Ticker.h>
#include "secrets.h"
#include "fields.h"

#define FW_VERSION "2026-11-13-wdt"

// Aktuelle Monats-Datei, z.B. WE2611.DAT. Wird per NTP bestimmt.
String currentLogFile;
String lastActiveFile;
// Merker vom letzten Monatswechsel: welche Datei war aktiv und bis zu welcher
// Position war sie schon publiziert? Ohne das loescht cleanupOldMonthFiles()
// eine alte Monatsdatei, in der noch ungesendete Zeilen stehen.
String rotOldFile = "";
uint32_t rotOldPos = 0;
bool rotOldKnown = false;
// Feste Puffer statt wachsender Strings: ESP8266-String allokiert bei jedem
// += exakt neu. Zeichenweises Sammeln (Leseschleife, tail) hat tausende
// Reallocs pro Minute erzeugt und den Heap so zerlegt, dass nach ~1 Tag kein
// zusammenhaengender Block mehr frei war: lastPos blieb stehen, MQTT lebte
// weiter, kein Neustart - exakt der taegliche Haenger. Darum hier kein Heap.
#define LINE_BUF_SIZE 512
char lineBuf[LINE_BUF_SIZE + 1];   // +1 fuer das Abschluss-Nullzeichen
#define TAIL_BUF_SIZE 512
char tailBuf[TAIL_BUF_SIZE + 1];   // +1 fuer das Abschluss-Nullzeichen
uint32_t lastPosProgressMs = 0;   // wann kam lastPos zuletzt voran
uint32_t lastProgressPos = 0;
uint8_t heapLowCount = 0;
uint32_t lastBeatMs = 0;          // letzter Heartbeat
uint32_t mqttDownSince = 0;       // seit wann MQTT weg ist (0 = verbunden)
uint32_t wifiDownSince = 0;       // seit wann WLAN weg ist (0 = verbunden)
volatile uint32_t lastLoopMs = 0; // letzter loop()-Durchlauf fuer den Hard-Watchdog
Ticker hangTicker;

// Befehle werden NICHT im MQTT-Callback verarbeitet (siehe mqttCallback).
#define CMD_BUF_SIZE 1024
char pendingCmd[CMD_BUF_SIZE];
char pendingTopic[96];
bool pendingCmdReady = false;

void publishDiscovery();
bool publishDiscoverySingle(uint8_t rtIdx);
void publishIp();
void publishTelemetryDiscovery();
void publishTelemetry(uint32_t fileSize, uint32_t lastPos);
String nowIso();
String measIsoFromLine(const String& clean);
uint32_t loadLinesTotal();
void saveLinesTotal(uint32_t v);
void rotateIfPublished(uint32_t fileSize, uint32_t lastPos);
String firstLineOfFile();
time_t recordTimeFromLine(const String& line);
void setupTime();
String monthFilenameFor(time_t t);
String activeLogFile();
bool isLogFile(const String& name);
void cleanupOldMonthFiles(const String& keep);
void updateActiveFile();
bool publishFields(const String& clean);
void refreshFromTail();
String getFieldToken(const String& line, uint8_t wanted);
void mqttCallback(char* topic, byte* payload, unsigned int len);
void handleMqttCommand(const char* topic, const char* payload);
void loadRuntimeFields();
void saveRuntimeFields();
void publishConfigState(const String& info);
void addMissingDefaultFields();
String lastLineOfFile();

// ---------- EEPROM ----------
#define EEPROM_SIZE 2048
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
// WICHTIG: Config belegt 48 .. 48+20*69 = 1428 (69 Byte pro Feld).
// linesTotal muss deshalb in den freien Bereich 5..31, sonst ueberschreibt
// es die Feld-Config (Namen/Units kaputt).
#define EEPROM_LINES_ADDR 8
// Tagesdatei: Es wird nur der heutige Tag behalten. Beim ersten Lesen nach
// einem Tageswechsel werden alle Zeilen frueherer Tage geloescht - auch
// ungelesene, denn der Nutzer will nur 1 Tag. Die Datei wird ueber eine
// Temp-Datei neu geschrieben und in denselben Verzeichnis-Eintrag
// zurueckkopiert (kein remove(), damit der Regler die Datei nicht unter den
// Fuessen verliert). Datum steht in Spalte 0 = MMDD, Spalte 1 = HHMM.
// Der 1,5-GB-Notausgang (MAX_FILE_SIZE_BYTES) bleibt zusaetzlich bestehen.

// Wie oft die letzte Zeile der Datei geprueft wird (unabhaengig von lastPos).
// 20 s statt 10 s: halbiert die SD-Zugriffe. Werte kommen trotzdem promptly an,
// weil der Regler ohnehin nur alle paar Minuten eine neue Zeile schreibt.
#ifndef TAIL_CHECK_MS
#define TAIL_CHECK_MS 20000UL
#endif

// Wie oft die Telemetrie (IP, Dateigroesse, Position, RSSI, Heap, Status)
// gesendet wird. Vorher lief die bei jedem SD-Poll mit (~12 Publishes je Poll).
#ifndef TELEMETRY_MS
#define TELEMETRY_MS 1200000UL   // 20 min
#endif

// Selbstheilung: Wenn trotz bestehender MQTT-Verbindung so lange keine
// SD-Datei mehr geoeffnet wurde, haengt die Schleife (typisch: CS_SENSE
// dauerhaft LOW -> canTakeBus() blockt jede Runde, MQTT bleibt aber
// verbunden und das Geraet antwortet weiter auf Ping). Neustart holt die
// Zeilen nach, weil die Leseposition in der EEPROM steht.
#ifndef SD_STALL_MS
#define SD_STALL_MS 1800000UL    // 30 min
#endif

// Heartbeat + Heap-Selbstheilung (gegen den taeglichen Stillstand, siehe unten)
#ifndef HEARTBEAT_MS
#define HEARTBEAT_MS 300000UL        // 5 min
#endif
#ifndef HEAP_MIN_FREE
#define HEAP_MIN_FREE 8192UL         // freier Heap darunter -> Neustart (laeuft auf allen Cores)
#endif
#ifndef MQTT_STALL_MS
#define MQTT_STALL_MS 900000UL      // 15 min ohne MQTT (trotz WLAN) -> Neustart
#endif

// Hard-Watchdog: laeuft im Ticker-ISR und kommt auch dann noch dran, wenn
// loop() in SD.open()/mqtt.publish() haengt. 10 min ohne Loop-Fortschritt.
#ifndef HANG_RESET_MS
#define HANG_RESET_MS 600000UL      // 10 min
#endif

// WLAN-Watchdog: verbundenes MQTT nutzt nichts, wenn das WLAN weg ist.
#ifndef WIFI_STALL_MS
#define WIFI_STALL_MS 900000UL      // 15 min ohne WLAN -> Neustart
#endif

#define MAX_RT_FIELDS 20
#define KEY_LEN 20
#define NAME_LEN 36
#define UNIT_LEN 8

struct RtField {
  uint8_t index;
  char key[KEY_LEN];
  char name[NAME_LEN];
  char unit[UNIT_LEN];
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
bool tailPublishPending = true;  // nach Reconnect/Config einmal neu senden
uint32_t lastLinesSavedAt = 0;

// SD-Zustand: Die Karte wird EINMAL initialisiert und bleibt gemountet.
// Vorher lief alle 5 s ein SD.begin() - auf dem mit dem Regler geteilten
// Bus schlug das dauernd fehl ("begin() fehlgeschlagen").
bool sdMounted = false;
uint32_t sdNextTry = 0;
uint32_t sdBackoff = 15000UL;
uint32_t lastSdOkMs = 0;   // letzter erfolgreicher SD.open() (Selbstheilung)
uint32_t lastCompactDay = 0; // YYYYMMDD, an dem zuletzt auf "nur heute" gekuerzt wurde
uint32_t nextCompactMs = 0;  // Retry nach fehlgeschlagener Kompaktierung

// ---------- Globale Objekte ----------
WiFiClient espClient;
PubSubClient mqtt(espClient);

volatile unsigned long spiBlockoutUntil = 0;
// Wie im FYSETC-Referenzcode: Waehrend wir den Bus selbst treiben, darf ein
// Pegelwechsel auf CS_SENSE den Blockout nicht verlaengern. Sonst verschiebt
// sich der naechste Leseversuch immer weiter nach hinten.
volatile bool busOwned = false;

void ICACHE_RAM_ATTR hangWatchdogCb() {
  if (lastLoopMs != 0 && (millis() - lastLoopMs) > HANG_RESET_MS) {
    ESP.restart();
  }
}

void ICACHE_RAM_ATTR onCsSenseFall() {
  if (!busOwned) spiBlockoutUntil = millis() + 20000UL;
}

// ---------- WLAN ----------
// Status-Codes beim Verbinden: 0=idle, 1=Netz nicht gefunden
// (Kanal/Band/Reichweite!), 3=verbunden, 4=abgelehnt,
// 5=Verbindung verloren, 6=getrennt (oft falsches Passwort).
static uint8_t wifiFails = 0;

// Diagnose bei WLAN-Ausfall: ist unser Netz ueberhaupt sichtbar?
// Dauert ~2-3 s (blockiert), darum hoechstens alle 10 Minuten.
void wifiScanDiagnose() {
  static uint32_t lastScan = 0;
  if (millis() - lastScan < 600000UL) return;
  lastScan = millis();
  Serial.println(F("[WiFi] Suche Netze ..."));
  int n = WiFi.scanNetworks();
  Serial.print(F("[WiFi] Netze gefunden: "));
  Serial.println(n);
  bool gesehen = false;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    Serial.print(F("  "));
    Serial.print(s);
    Serial.print(F(" RSSI="));
    Serial.print(WiFi.RSSI(i));
    Serial.print(F(" Kanal="));
    Serial.println(WiFi.channel(i));
    if (s == WIFI_SSID) gesehen = true;
    yield();
  }
  WiFi.scanDelete();
  if (!gesehen) {
    Serial.println(F("[WiFi] EIGENES NETZ NICHT SICHTBAR! Pruefen: 2,4 GHz an? Kanal 1-11? Reichweite?"));
  } else {
    Serial.println(F("[WiFi] Netz SICHTBAR, Anmeldung scheitert -> Passwort in secrets.h pruefen!"));
  }
}

bool connectWifi(uint32_t timeout_ms = 20000UL) {
  if (WiFi.status() == WL_CONNECTED) { wifiFails = 0; return true; }
  Serial.print(F("[WiFi] Verbinde mit "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.hostname("fysetc-solarlog");  // Name im Router/DHCP (frueher fuer OTA)
  WiFi.persistent(false);             // nichts in den SDK-Flash schreiben
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  int letzterStatus = -99;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    int st = WiFi.status();
    if (st != letzterStatus) {
      Serial.print(F(" ["));
      Serial.print(st);
      Serial.print(F("]"));
      letzterStatus = st;
    } else {
      Serial.print(F("."));
    }
    yield();
    if (millis() - start > timeout_ms) {
      Serial.print(F(" Timeout, Status="));
      Serial.println(WiFi.status());
      wifiFails++;
      if (wifiFails >= 3) wifiScanDiagnose();
      return false;
    }
  }
  Serial.println();
  Serial.print(F("[WiFi] IP: "));
  Serial.println(WiFi.localIP());
  wifiFails = 0;
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
void applyDefaultLimits(RtField &f) {
  f.lo = 0;
  f.hi = 0;
  for (uint8_t k = 0; k < NUM_FIELDS; k++) {
    if (FIELDS[k].index == f.index) {
      f.lo = FIELDS[k].lo;
      f.hi = FIELDS[k].hi;
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
  addMissingDefaultFields();
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

// Werte eines fields.h-Eintrags in einen Runtime-Slot uebernehmen.
void applyFieldDef(RtField &f, const FieldDef &d) {
  f.index = d.index;
  strncpy(f.key, d.key, KEY_LEN - 1);    f.key[KEY_LEN - 1] = 0;
  strncpy(f.name, d.name, NAME_LEN - 1); f.name[NAME_LEN - 1] = 0;
  strncpy(f.unit, d.unit, UNIT_LEN - 1); f.unit[UNIT_LEN - 1] = 0;
  f.divider = d.divider;
  f.lo = d.lo;
  f.hi = d.hi;
  f.used = true;
  f.disc = false;
  f.pruned = false;
}

// Felder aus fields.h, die in der EEPROM-Config fehlen, hinten anhaengen.
// Bisher galt: ist die Config-Magic gueltig, wird fields.h komplett ignoriert
// -> neue Spalten kamen nach einem Sketch-Update nie an, egal wie oft man
// flasht. Vorhandene Runtime-Namen/Teiler bleiben dabei unangetastet.
void addMissingDefaultFields() {
  bool changed = false;
  for (uint8_t i = 0; i < NUM_FIELDS; i++) {
    int pos = findRtByIndex(FIELDS[i].index);
    if (pos >= 0) {
      // Gleicher Index, aber anderer Key (= Spalte neu zugeordnet): fields.h
      // gewinnt, sonst bliebe die alte Zuordnung fuer immer stehen.
      if (strcmp(rtFields[pos].key, FIELDS[i].key) == 0) continue;
      Serial.print(F("[CFG] Spalte "));
      Serial.print(FIELDS[i].index);
      Serial.print(F(" neu zugeordnet: "));
      Serial.print(rtFields[pos].key);
      Serial.print(F(" -> "));
      Serial.println(FIELDS[i].key);
      applyFieldDef(rtFields[pos], FIELDS[i]);
      changed = true;
      continue;
    }
    int freeSlot = -1;
    for (uint8_t s = 0; s < MAX_RT_FIELDS; s++) if (!rtFields[s].used) { freeSlot = s; break; }
    if (freeSlot < 0) {
      Serial.println(F("[CFG] Kein freier Slot fuer neue fields.h-Felder"));
      break;
    }
    applyFieldDef(rtFields[freeSlot], FIELDS[i]);
    if (freeSlot + 1 > (int)rtCount) rtCount = freeSlot + 1;
    Serial.print(F("[CFG] fields.h nachgetragen: "));
    Serial.print(rtFields[freeSlot].key);
    Serial.print(F(" = Spalte "));
    Serial.println(FIELDS[i].index);
    changed = true;
  }
  if (changed) saveRuntimeFields();
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

// Der Callback darf NICHTS publizieren: PubSubClient nutzt fuer Senden und
// Empfangen denselben Puffer. Ein publish aus dem Callback heraus zerstoert
// ihn (Mosquitto: "malformed packet"/"Invalid QoS") und kann den Heap
// korrumpieren. Darum nur kopieren, verarbeitet wird in loop().
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  if (pendingCmdReady) return;   // vorherigen Befehl nicht ueberschreiben
  if (len >= sizeof(pendingCmd)) len = sizeof(pendingCmd) - 1;
  memcpy(pendingCmd, payload, len);
  pendingCmd[len] = 0;
  strncpy(pendingTopic, topic, sizeof(pendingTopic) - 1);
  pendingTopic[sizeof(pendingTopic) - 1] = 0;
  pendingCmdReady = true;
}

void handleMqttCommand(const char* topic, const char* payload) {
  if (strcmp(topic, MQTT_TOPIC_SETCONFIG) != 0) return;
  // Nach jeder Config-Aenderung die Sensorwerte neu senden (Key/Teiler
  // koennen sich geaendert haben).
  tailPublishPending = true;
  String msg = payload;
  msg.trim();
  if (msg.length() == 0) return;
  Serial.print(F("[MQTT] set_config: "));
  Serial.println(msg);

  String cmd = extractJsonString(msg, "cmd");
  if (cmd == "list") {
    publishConfigState("list");
    return;
  }
  if (cmd == "probe") {
    // Rohspalten der letzten Logzeile: zeigt, ob Spaltenindex und Inhalt
    // wirklich zusammenpassen (und wie viele Spalten die Zeile hat).
    String line = lastLineOfFile();
    if (line.length() == 0) {
      mqtt.publish(MQTT_TOPIC_CONFIGSTATE, "{\"error\":\"keine Zeile gelesen\"}", false);
      return;
    }
    uint8_t cols = 1;
    for (unsigned int i = 0; i < line.length(); i++) if (line[i] == ';') cols++;
    String j = String("{\"cols\":") + String(cols);
    for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) {
      if (!rtFields[i].used) continue;
      String tok = getFieldToken(line, rtFields[i].index);
      tok.trim();
      if (tok.length() > 24) tok = tok.substring(0, 24);
      j += String(",\"") + rtFields[i].key + "\":\"" + tok + "\"";
    }
    j += F(",\"info\":\"probe\"}");
    mqtt.publish(MQTT_TOPIC_CONFIGSTATE, j.c_str(), false);
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
    if (hasDiv) rtFields[pos].divider = ndiv;
    if (hasLo) rtFields[pos].lo = nlo;
    if (hasHi) rtFields[pos].hi = nhi;
  } else {
    // neuer Slot
    int freeSlot = -1;
    for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) if (!rtFields[i].used) { freeSlot = i; break; }
    if (freeSlot < 0) {
      mqtt.publish(MQTT_TOPIC_CONFIGSTATE, "{\"error\":\"kein freier Slot (max 20)\"}", false);
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
  // Entity ist jetzt in HA bekannt -> bei jedem Reconnect erneut anmelden
  rtFields[pos].disc = true;
  rtFields[pos].pruned = false;
  publishConfigState("saved");
}

bool connectMqtt() {
  if (mqtt.connected()) return true;
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  // Puffer wird EINMAL in setup() gesetzt. setBufferSize() allokiert sonst bei
  // jedem Reconnect 2 KB neu und zerteilt den Heap genau so, wie es der
  // taegliche Haenger gebraucht hat.
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
    tailPublishPending = true;
  } else {
    Serial.print(F("[MQTT] Fehler, rc="));
    Serial.println(mqtt.state());
  }
  return ok;
}

// ---------- SD-Bus ----------
bool canTakeBus() {
  if ((long)(millis() - spiBlockoutUntil) < 0) return false;
  static uint32_t csLowSince = 0;
  if (digitalRead(CS_SENSE_PIN) == LOW) {
    // CS_SENSE kann nach einem Regler-Reset dauerhaft LOW haengen. Dann
    // blockiert diese Zeile jede Runde, und auch der 30-min-Neustart hilft
    // nichts, weil die Ursache bleibt. Nach 2 Minuten ohne Flanke nehmen wir
    // den Bus trotzdem - der Regler schreibt nur alle 10 Minuten eine Zeile.
    if (csLowSince == 0) csLowSince = millis();
    if (millis() - csLowSince < 120000UL) {
      spiBlockoutUntil = millis() + 5000UL;
      return false;
    }
    Serial.println(F("[SD] CS_SENSE haengt LOW, nehme Bus trotzdem"));
    csLowSince = 0;
    return true;
  }
  csLowSince = 0;
  return true;
}

void takeBus() {
  busOwned = true;
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
  busOwned = false;
}

// ---------- Feld-Helfer ----------
String getFieldToken(const String& line, uint8_t wanted) {
  // Nur das gesuchte Token als String erzeugen. Die alte Version hat fuer
  // JEDES Token bis zur gesuchten Spalte einen String allokiert (bei Spalte 28
  // also 29 Stueck pro Feld und Zeile) - genau die Heap-Zerlegung, die nach
  // einigen Stunden zum Haenger fuehrte.
  int start = 0;
  for (uint8_t idx = 0; idx < wanted; idx++) {
    int pos = line.indexOf(';', start);
    if (pos < 0) return String("");
    start = pos + 1;
  }
  int pos = line.indexOf(';', start);
  String tok = (pos < 0) ? line.substring(start) : line.substring(start, pos);
  tok.trim();
  return tok;
}

// Icon fuer eine Spalte aus fields.h holen ("" = kein Icon)
const char* iconForIndex(uint8_t idx) {
  for (uint8_t k = 0; k < NUM_FIELDS; k++) {
    if (FIELDS[k].index == idx) return FIELDS[k].icon;
  }
  return "";
}

bool publishDiscoverySingle(uint8_t rtIdx) {
  if (rtIdx >= MAX_RT_FIELDS || !rtFields[rtIdx].used) return true;   // nichts zu senden = kein Fehler
  RtField &f = rtFields[rtIdx];
  String discTopic = String("homeassistant/sensor/solarlog_") + f.key + "/config";
  String stateTopic = String("home/solarlog/") + f.key;
  String payload = String("{\"name\":\"") + f.name + "\",";
  payload += String("\"state_topic\":\"") + stateTopic + "\",";
  payload += String("\"unique_id\":\"solarlog_") + f.key + "\",";
  payload += String("\"device\":{\"identifiers\":[\"fysetc-solarlog\"],\"name\":\"Solarlog\",\"manufacturer\":\"TiSun\"},");
  if (strlen(f.unit) > 0) {
    payload += String("\"unit_of_measurement\":\"") + f.unit + "\",";
  }
  // state_class ist die Voraussetzung fuer Langzeitstatistiken in Home Assistant.
  // Ohne dieses Feld gibt es keine Max-/Min-/Durchschnittskarten im Dashboard
  // (die "Puffer max"-Kachel der Waermepumpe funktioniert genau darueber).
  if (strcmp(f.unit, "°C") == 0) {
    payload += F("\"device_class\":\"temperature\",\"state_class\":\"measurement\",");
  } else if (strcmp(f.unit, "K") == 0) {
    // Bewusst OHNE device_class: HA verwirft die komplette Discovery, wenn
    // "temperature_difference" mit Einheit K kommt -> dT bekaeme dann gar
    // keine state_class und damit keine Statistik. Ohne device_class greift
    // state_class und die Statistik laeuft (so am 08.11. verifiziert).
    payload += F("\"state_class\":\"measurement\",");
  } else if (strcmp(f.unit, "%") == 0 || strcmp(f.unit, "kW") == 0 ||
             strcmp(f.unit, "L/min") == 0) {
    payload += F("\"state_class\":\"measurement\",");
  } else if (strcmp(f.unit, "h") == 0 || strcmp(f.unit, "kWh") == 0) {
    // Zaehler (Betriebsstunden, Energie) laufen nur hoch -> total_increasing.
    // Damit rechnet die Statistik-Karte "change" = Stunden/kWh im Zeitraum,
    // bei measurement gaebe es nur Mittel/Max und keine Differenz.
    if (strcmp(f.unit, "kWh") == 0) {
      payload += F("\"device_class\":\"energy\",");
    }
    payload += F("\"state_class\":\"total_increasing\",");
  }
  const char* ic = iconForIndex(f.index);
  if (ic && ic[0]) {
    payload += String("\"icon\":\"") + ic + "\",";
  }
  payload += String("\"value_template\":\"{{ value }}\"}");
  bool ok = mqtt.publish(discTopic.c_str(), payload.c_str(), true);
  Serial.print(F("[DISC] "));
  Serial.print(f.key);
  Serial.print(F(" -> "));
  Serial.println(discTopic);
  return ok;   // FEHLTE: ohne return gab die Funktion Muell zurueck -> Werte wurden verschluckt
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
// ---------- Erste Zeile der Datei (fuer den Tageswechsel) ----------
String firstLineOfFile() {
  File f = SD.open(currentLogFile.c_str(), FILE_READ);
  if (!f) return String("");
  if (f.size() == 0) { f.close(); return String(""); }
  String line;
  line.reserve(200);
  uint16_t n = 0;
  while (f.available() && n < 200) {
    char c = (char)f.read();
    if (c == '\r' || c == '\n') break;
    line += c;
    n++;
  }
  f.close();
  line.trim();
  if (line.indexOf(';') < 0) return String("");
  return line;
}

// Messzeit einer Logzeile als Unix-Zeit (0 = nicht lesbar).
time_t recordTimeFromLine(const String& line) {
  String s0 = getFieldToken(line, 0);
  String s1 = getFieldToken(line, 1);
  if (s0.length() < 3 || s1.length() < 3) return 0;
  // Nur Ziffern behalten (manche Regler senden "1106." o.ae.)
  String d0 = "", d1 = "";
  for (unsigned int i = 0; i < s0.length(); i++) if (isDigit(s0[i])) d0 += s0[i];
  for (unsigned int i = 0; i < s1.length(); i++) if (isDigit(s1[i])) d1 += s1[i];
  if (d0.length() != 4 || d1.length() != 4) return 0;
  int mm = d0.substring(0, 2).toInt();
  int dd = d0.substring(2, 4).toInt();
  int hh = d1.substring(0, 2).toInt();
  int mi = d1.substring(2, 4).toInt();
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59) return 0;
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
  return (time_t)daysFromCivil(year, mm, dd) * 86400L
         + (time_t)hh * 3600 + (time_t)mi * 60
         - (time_t)deOffsetSec(year, mm, dd, hh);
}

String measIsoFromLine(const String& clean) {
  time_t t = recordTimeFromLine(clean);
  if (t == 0) return String("");
  return isoUtc(t);
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

// Behaelt nur die Zeilen des heutigen Tages. Aeltere Tage werden geloescht,
// auch wenn sie noch nicht per MQTT raus sind - der Nutzer will nur 1 Tag.
// Die Datei wird ueber eine Temp-Datei neu geschrieben und danach in
// denselben Verzeichnis-Eintrag zurueckkopiert (kein remove(), damit der
// Regler die Datei nicht unter den Fuessen verliert).
bool compactToToday(int day, int month) {
  File in = SD.open(currentLogFile.c_str(), FILE_READ);
  if (!in) return false;
  String tmp = currentLogFile.substring(0, currentLogFile.length() - 3) + "TMP";
  SD.remove(tmp.c_str());
  File out = SD.open(tmp.c_str(), FILE_WRITE);
  if (!out) { in.close(); return false; }

  uint16_t n = 0;
  uint32_t kept = 0, dropped = 0;
  while (in.available()) {
    int c = in.read();
    if (c < 0) break;
    if (c == '\n') {
      if (n > 0) {
        lineBuf[n] = 0;
        int mmdd = atoi(lineBuf);          // MMDD, z.B. 1114 = 14.11.
        if ((mmdd / 100) == month && (mmdd % 100) == day) {
          out.write((const uint8_t*)lineBuf, n);
          out.write('\n');
          kept++;
        } else {
          dropped++;
        }
      }
      n = 0;
      yield();
      continue;
    }
    if (c == '\r') continue;
    if (n < LINE_BUF_SIZE) lineBuf[n++] = (char)c;
  }
  if (n > 0) {
    lineBuf[n] = 0;
    int mmdd = atoi(lineBuf);
    if ((mmdd / 100) == month && (mmdd % 100) == day) {
      out.write((const uint8_t*)lineBuf, n);
      out.write('\n');
      kept++;
    } else {
      dropped++;
    }
  }
  in.close();
  out.close();

  // Zurueck in denselben Verzeichnis-Eintrag kopieren (FILE_WRITE kuerzt).
  File src = SD.open(tmp.c_str(), FILE_READ);
  if (!src) { SD.remove(tmp.c_str()); return false; }
  File dst = SD.open(currentLogFile.c_str(), FILE_WRITE);
  if (!dst) { src.close(); SD.remove(tmp.c_str()); return false; }
  uint16_t written = 0;
  while (src.available()) {
    int c = src.read();
    if (c < 0) break;
    dst.write((uint8_t)c);
    if ((++written & 0x7F) == 0) yield();
  }
  src.close();
  dst.close();
  SD.remove(tmp.c_str());

  saveReadPosition(0);
  lastTailLine = "";
  tailPublishPending = true;
  Serial.print(F("[SD] Tageswechsel: "));
  Serial.print(kept);
  Serial.print(F(" Zeilen behalten, "));
  Serial.print(dropped);
  Serial.println(F(" geloescht"));
  String msg = String("{\"cleared\":\"") + currentLogFile + "\",\"reason\":\"day\",\"kept\":" + String(kept) +
               ",\"dropped\":" + String(dropped) + "}";
  mqtt.publish(MQTT_TOPIC_STATUS, msg.c_str(), false);
  return true;
}

// Einmal pro Tag: Die Datei auf den heutigen Tag kuerzen. Das passiert
// absichtlich auch dann, wenn die Leseposition noch nicht am Dateiende ist:
// Die alten Tage sollen weg, nicht erst nach dem kompletten Rueckstand.
void rotateIfPublished(uint32_t fileSize, uint32_t lastPos) {
  (void)fileSize;
  (void)lastPos;
  time_t now = time(nullptr);
  if (now < 1600000000) return;              // kein NTP -> kein Tageswechsel
  struct tm *lt = localtime(&now);
  if (!lt) return;
  struct tm nowTm = *lt;
  uint32_t today = (uint32_t)(nowTm.tm_year + 1900) * 10000UL +
                   (uint32_t)(nowTm.tm_mon + 1) * 100UL +
                   (uint32_t)nowTm.tm_mday;
  if (lastCompactDay == today) return;
  if (nextCompactMs != 0 && (int32_t)(millis() - nextCompactMs) < 0) return;

  String first = firstLineOfFile();
  if (first.length() == 0) { lastCompactDay = today; return; }
  int mmdd = atoi(first.c_str());
  if ((mmdd / 100) == nowTm.tm_mon + 1 && (mmdd % 100) == nowTm.tm_mday) {
    lastCompactDay = today;
    return;
  }
  Serial.println(F("[SD] Tageswechsel: alte Zeilen werden entfernt"));
  if (compactToToday(nowTm.tm_mday, nowTm.tm_mon + 1)) {
    lastCompactDay = today;
    nextCompactMs = 0;
  } else {
    nextCompactMs = millis() + 60000UL;
  }
}

void teleDisc(const char* key, const char* name, const char* stateTopic, const char* unit, const char* devClass) {
  String disc = String("homeassistant/sensor/solarlog_") + key + "/config";
  String p = String("{\"name\":\"") + name + "\",";
  p += String("\"state_topic\":\"") + stateTopic + "\",";
  p += String("\"unique_id\":\"solarlog_") + key + "\",";
  p += String("\"device\":{\"identifiers\":[\"fysetc-solarlog\"],\"name\":\"Solarlog\",\"manufacturer\":\"TiSun\"}");
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

void publishTelemetry(uint32_t fileSize, uint32_t lastPos) {
  if (!mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
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

// Telemetrie drosseln: hoechstens alle TELEMETRY_MS senden, aber beim ersten
// Aufruf nach dem Boot sofort (sonst blieben die retained Werte lange leer).
void publishTelemetryThrottled(uint32_t size, uint32_t pos) {
  static uint32_t lastMs = 0;
  static bool init = false;
  if (init && (millis() - lastMs < TELEMETRY_MS)) return;
  init = true;
  lastMs = millis();
  publishTelemetry(size, pos);
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
  // "Letzte Rohzeile" entfaellt -> Discovery und retained Wert loeschen
  mqtt.publish("homeassistant/sensor/solarlog_last_line/config", "", true);
  mqtt.publish("home/solarlog/last_line", "", true);
  // Schon bekannte Messwert-Sensoren erneut anmelden. Sonst bleiben sie fuer
  // immer weg, wenn jemand die Entities in HA loescht: der ESP hat sie nur
  // einmal pro Boot geschickt (f.disc) und merkt das Loeschen nicht.
  // Felder ohne gueltigen Wert (T3/T5 = 199) bleiben weiter aussen vor.
  for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) {
    if (rtFields[i].used && rtFields[i].disc) publishDiscoverySingle(i);
  }
  // Eigene IP als eigener HA-Sensor (Remote ablesbar)
  {
    String discTopic = "homeassistant/sensor/solarlog_ip/config";
    String payload = String("{\"name\":\"Solarlog IP\",");
    payload += String("\"state_topic\":\"") + MQTT_TOPIC_IP + "\",";
    payload += String("\"unique_id\":\"solarlog_ip\",");
    payload += String("\"icon\":\"mdi:ip-network\",");
    payload += String("\"device\":{\"identifiers\":[\"fysetc-solarlog\"],\"name\":\"Solarlog\",\"manufacturer\":\"TiSun\"}");
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
  // Nur das Schwanzende in einen FESTEN Puffer lesen (kein Heap-Wachstum).
  uint32_t start = (sz > TAIL_BUF_SIZE) ? sz - TAIL_BUF_SIZE : 0;
  if (!f.seek(start)) { f.close(); return String(""); }
  uint16_t n = 0;
  while (f.available() && n < TAIL_BUF_SIZE) {
    int ci = f.read();
    if (ci < 0) break;
    tailBuf[n++] = (char)ci;
  }
  f.close();
  // Letzte Zeile = alles nach dem letzten CR/LF. Der Regler trennt mit CR
  // (teils CRLF), darum beide als Trenner werten.
  int end = n;
  while (end > 0 && (tailBuf[end - 1] == '\r' || tailBuf[end - 1] == '\n')) end--;
  if (end <= 0) return String("");
  int begin = end;
  while (begin > 0 && tailBuf[begin - 1] != '\r' && tailBuf[begin - 1] != '\n') begin--;
  uint16_t len = (uint16_t)(end - begin);
  if (len == 0 || len > 400) return String("");   // Muell, keine echte Zeile
  tailBuf[end] = '\0';   // Platz ist da: Puffer ist TAIL_BUF_SIZE + 1
  String line = String(tailBuf + begin);   // genau EINE Allokation
  line.trim();
  if (line.indexOf(';') < 0) return String("");
  return line;
}

// Beim ersten Zugriff: letzte Zeile einmal auswerten -> sofort Werte in HA
// und ungueltige Felder werden gar nicht erst angelegt.
// Liest die letzte Zeile der Datei und meldet sie nur, wenn sie sich seit dem
// letzten Mal geaendert hat. Das ist unabhaengig von der gespeicherten
// Leseposition und funktioniert auch, wenn die Datei neu angelegt wird.
void refreshFromTail() {
  String line = lastLineOfFile();
  if (line.length() == 0) return;
  bool neu = (line != lastTailLine);
  if (neu) {
    lastTailLine = line;
    Serial.print(F("[SD] neue letzte Zeile: "));
    Serial.println(line);
  }
  // Nur bei neuer Zeile oder wenn nach Reconnect/Config ein Update aussteht
  // senden. Das fruehere "immer alle 20 s" hat pro Durchlauf 14 Felder mit
  // vielen String-Allokationen publiziert und den Heap nach einigen Stunden
  // zerlegt (Haenger nach ca. 4 h).
  if (!neu && !tailPublishPending) return;
  if (publishFields(line)) tailPublishPending = false;
  else tailPublishPending = true;   // naechster Versuch in 20 s
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

// true = alle Werte raus. false = mindestens ein Publish schlug fehl -> der
// Aufrufer stellt die Position NICHT weiter (keine Zeile geht verloren).
bool publishFields(const String& clean) {
  bool ok = true;
  for (uint8_t i = 0; i < MAX_RT_FIELDS; i++) {
    if (!rtFields[i].used) continue;
    RtField &f = rtFields[i];
    String tok = getFieldToken(clean, f.index);
    tok.trim();
    String topic = String("home/solarlog/") + f.key;
    if (tok.length() == 0) {
      // Spalte fehlt in der Zeile (zu wenige Semikolons) -> sichtbar machen,
      // sonst sucht man ewig, warum ein Feld nie auftaucht.
      Serial.print(F("[MQTT] Spalte fehlt, nicht gesendet: "));
      Serial.print(f.key);
      Serial.print(F(" = Spalte "));
      Serial.println(f.index);
      clearFieldTopic(topic);
      continue;
    }
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
    if (!f.disc) {
      // Die Discovery darf das Senden eines Werts NIE verhindern. Vorher stand
      // hier ein "continue": ein fehlgeschlagenes Discovery-Publish hat damit
      // ALLE Messwerte verschluckt - der Fehler vom 11.11. ab 16:30.
      if (publishDiscoverySingle(i)) { f.disc = true; f.pruned = false; }
      // Kein ok=false: Ein Discovery-Fehler darf die Leseposition nicht anhalten.
    }
    // retained: sonst steht nach HA-/Broker-Neustart wieder "Unbekannt"
    if (!mqtt.publish(topic.c_str(), out.c_str(), true)) ok = false;
  }
  return ok;
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

// true = Zeile komplett beim Broker. false = etwas schlug fehl -> Aufrufer
// laesst die Position stehen, die Zeile kommt naechsten Poll erneut dran.
// isLatest = diese Zeile ist die letzte der Datei. Nur dann werden die
// HA-Sensoren gesetzt: Beim Aufholen eines Rueckstands wuerden alte Werte
// sonst die aktuellen ueberschreiben. Die Rohzeile geht immer auf den
// MQTT_TOPIC_SOLAR (inkl. mtime), damit die Historie vollstaendig bleibt.
bool mqttPublishLine(const String& line, uint32_t pos, bool isLatest) {
  if (!mqtt.connected()) return false;
  String clean = line;
  clean.trim();
  if (clean.length() == 0) return true;   // nichts zu senden = kein Fehler
  bool ok = true;
  String mtime = measIsoFromLine(clean);
  if (isLatest) {
    ok = publishFields(clean);
    if (mtime.length() > 0) {
      lastMeasIso = mtime;
      if (!mqtt.publish(MQTT_TOPIC_MEASTIME, mtime.c_str(), true)) ok = false;
    }
  }
  linesTotal++;
  String iso = nowIso();
  if (iso.length() > 0) lastRefreshIso = iso;
  if ((linesTotal % 10) == 0) saveLinesTotal(linesTotal);
  else if (millis() - lastLinesSavedAt > 60000UL) { saveLinesTotal(linesTotal); lastLinesSavedAt = millis(); }
  if (lastRefreshIso.length() > 0 && !mqtt.publish(MQTT_TOPIC_LASTREFRESH, lastRefreshIso.c_str(), true)) ok = false;
  String json = F("{\"raw\":\"");
  json.reserve(320);   // ein Block statt ~8 Reallocs je Zeile
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
  if (!mqtt.publish(MQTT_TOPIC_SOLAR, json.c_str(), false)) ok = false;
  Serial.print(ok ? (isLatest ? F("[MQTT] gesendet: ") : F("[MQTT] Rueckstand (nur Roh-Topic): ")) : F("[MQTT] FEHLER bei: "));
  Serial.println(clean);
  if (!ok) {
    Serial.print(F("[MQTT] state="));
    Serial.print(mqtt.state());
    Serial.print(F(" connected="));
    Serial.println(mqtt.connected() ? 1 : 0);
  }
  return ok;
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
    uint32_t groesse = isDir ? 0 : e.size();
    e.close();
    if (isDir) continue;
    if (!isLogFile(n)) continue;
    // Nur Dateien AELTER als der aktuelle Monat loeschen (WEYYMM ist
    // sortierbar) - bei falscher Uhr kann so nie der neue Monat weg.
    if (n >= keep) continue;
    // Nichts wegwerfen, was noch nicht per MQTT raus ist: geloescht wird nur
    // eine leere Datei oder die alte Monatsdatei, die beim Wechsel komplett
    // gelesen war (size <= gemerkte Leseposition). Sonst bleibt sie liegen.
    bool sicher = (groesse == 0) ||
                  (rotOldKnown && n == rotOldFile && groesse <= rotOldPos);
    if (!sicher) {
      Serial.print(F("[SD] Alter Monat BEHALTEN (noch nicht gesendet): "));
      Serial.print(n);
      Serial.print(F(" size="));
      Serial.println(groesse);
      static String gemeldet = "";
      if (gemeldet != n) {
        gemeldet = n;
        String keepMsg = String("{\"kept\":\"") + n + "\",\"size\":" + String(groesse) + "}";
        mqtt.publish(MQTT_TOPIC_STATUS, keepMsg.c_str(), false);
      }
      yield();
      continue;
    }
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
    // Leseposition der ALTEN Datei merken, bevor sie auf 0 gesetzt wird:
    // der Cleanup entscheidet damit, ob die alte Datei weg darf.
    rotOldFile = currentLogFile;
    rotOldPos = loadReadPosition();
    rotOldKnown = (rotOldFile.length() > 0);
    currentLogFile = neu;
    // Neue Datei = von vorn lesen
    saveReadPosition(0);
  }
}

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

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("=== SolarLogReader " FW_VERSION " startet (Remote-Config) ==="));
  Serial.print(F("[SYS] Reset-Grund: "));
  Serial.println(ESP.getResetReason());

  EEPROM.begin(EEPROM_SIZE);
  loadRuntimeFields();

  pinMode(CS_SENSE_PIN, INPUT_PULLUP);
  attachInterrupt(CS_SENSE_PIN, onCsSenseFall, FALLING);

  WiFi.disconnect(true);   // alte AP-Daten aus dem SDK-Flash werfen (falscher Kanal!)
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
  if (!mqtt.setBufferSize(2048)) {
    Serial.println(F("[MQTT] WARNUNG: 2-KB-Puffer nicht allokierbar"));
  }
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(mqttCallback);
  connectMqtt();

  Serial.print(F("[CFG] Datei: "));
  Serial.println(currentLogFile);
  Serial.print(F("[CFG] Reset ab Bytes: "));
  Serial.println(MAX_FILE_SIZE_BYTES);
  Serial.print(F("[CFG] Start-Position: "));
  Serial.println(loadReadPosition());

  // Hard-Watchdog scharf machen. Erst NACH dem Setup, damit ein langer
  // WLAN-Verbindungsaufbau nicht schon als Haenger gilt.
  lastLoopMs = millis();
  hangTicker.attach(60, hangWatchdogCb);
}

void loop() {
  static uint32_t lastPoll = 0;
  lastLoopMs = millis();
  mqtt.loop();

  // Befehl ausserhalb von mqtt.loop() verarbeiten (Callback-Puffer!).
  if (pendingCmdReady) {
    pendingCmdReady = false;
    handleMqttCommand(pendingTopic, pendingCmd);
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSince == 0) wifiDownSince = millis();
    if (millis() - wifiDownSince > WIFI_STALL_MS) {
      Serial.println(F("[WDT] WLAN dauerhaft weg, Neustart"));
      delay(200);
      ESP.restart();
    }
    connectWifi();
    delay(1000);
    return;
  }
  wifiDownSince = 0;
  // Heartbeat + Heap-Selbstheilung: laeuft UNABHAENGIG vom SD-Teil, damit ein
  // stiller Tod (Heap zerlegt, Position steht, MQTT lebt) sichtbar wird und
  // sich selbst heilt. Alle 5 min ein Lebenszeichen mit Heap-Werten.
  if (mqtt.connected() && (millis() - lastBeatMs >= HEARTBEAT_MS)) {
    lastBeatMs = millis();
    uint32_t heap = ESP.getFreeHeap();   // getMaxFreeBlockSize gibt es erst in neueren Cores
    String beat = String("{\"alive\":1,\"ver\":\"") + FW_VERSION + "\",\"uptime_s\":" + String(millis() / 1000UL) + ",\"heap\":" + String(heap)
        + ",\"rssi\":" + String(WiFi.RSSI())
      + ",\"pos\":" + String(loadReadPosition())
      + "}";
    mqtt.publish(MQTT_TOPIC_STATUS, beat.c_str(), false);
    Serial.print(F("[SYS] Heartbeat, freier Heap: "));
    Serial.println(heap);
    // Heap zerlegt? 3x hintereinander zu kleiner Block -> Neustart (Heap ist
    // danach wieder sauber, die Position steht in der EEPROM).
    if (heap < HEAP_MIN_FREE) {
      if (++heapLowCount >= 3) {
        Serial.println(F("[WDT] Heap zerlegt, Neustart zur Selbstheilung"));
        mqtt.publish(MQTT_TOPIC_STATUS, "{\"restart\":\"heap\"}", false);
        delay(200);
        ESP.restart();
      }
    } else {
      heapLowCount = 0;
    }
  }
  // MQTT dauerhaft weg (trotz WLAN) -> Neustart, sonst bleibt alles stumm.
  if (!mqtt.connected() && WiFi.status() == WL_CONNECTED) {
    if (mqttDownSince == 0) mqttDownSince = millis();
    if (millis() - mqttDownSince > MQTT_STALL_MS) {
      Serial.println(F("[WDT] MQTT dauerhaft weg, Neustart"));
      delay(200);
      ESP.restart();
    }
  } else {
    mqttDownSince = 0;
  }
  if (!mqtt.connected()) {
    connectMqtt();
    delay(2000);
    if (!mqtt.connected()) return;
  }

  // Selbstheilung gegen haengende Schleifen: haengt canTakeBus() (CS_SENSE
  // dauerhaft LOW) oder der SD-Mount, bleibt MQTT "verbunden", es kommen
  // aber keine Werte mehr - genau das ist am 08.11. passiert (28 h Funkstille,
  // ohne dass das Geraet neu gestartet waere).
  if (lastSdOkMs == 0) lastSdOkMs = millis();
  if (mqtt.connected() && (millis() - lastSdOkMs > SD_STALL_MS)) {
    Serial.println(F("[WDT] SD haengt seit 30 min, Neustart zur Selbstheilung"));
    mqtt.publish(MQTT_TOPIC_STATUS, "{\"restart\":\"sd_stall\"}", false);
    delay(200);
    ESP.restart();
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

  lastSdOkMs = millis();
  uint32_t fileSize = f.size();
  uint32_t lastPos = loadReadPosition();
  // Fortschritts-Waechter: Die Datei laesst sich oeffnen, aber die
  // Leseposition kommt nicht voran (der alte SD_STALL greift hier NICHT,
  // weil open() ja klappt). Rueckstand ohne Fortschritt -> Neustart.
  if (fileSize > lastPos) {
    if (lastPos != lastProgressPos) { lastProgressPos = lastPos; lastPosProgressMs = millis(); }
    if (lastPosProgressMs == 0) lastPosProgressMs = millis();
    if (millis() - lastPosProgressMs > SD_STALL_MS) {
      Serial.println(F("[WDT] Leseposition steht, Neustart zur Selbstheilung"));
      f.close();
      releaseBus();
      mqtt.publish(MQTT_TOPIC_STATUS, "{\"restart\":\"sd_noprogress\"}", false);
      delay(200);
      ESP.restart();
    }
  } else {
    lastProgressPos = lastPos;
    lastPosProgressMs = millis();
  }

  Serial.print(F("[SD] size="));
  Serial.print(fileSize);
  Serial.print(F(" lastPos="));
  Serial.println(lastPos);

  publishTelemetryThrottled(fileSize, lastPos);

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
    // Datei wurde geleert/neu angelegt (Rotation, Karte war im PC): die
    // gemerkte Position ist veraltet und wird SOFORT vergessen - der Leser
    // faengt wirklich bei 0 an, nicht bei der alten Zahl.
    Serial.println(F("[SD] lastPos > size, starte bei 0"));
    lastPos = 0;
    saveReadPosition(0);
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
  // Zeichen in einen FESTEN Puffer sammeln: part += c hat pro Zeichen neu
  // allokiert (bis zu 2000 Reallocs je Durchlauf) und den Heap zerlegt.
  uint8_t sent = 0;
  uint16_t lineLen = 0;
  bool lineOk = true;   // false = laenger als jede echte Zeile -> Muell
  // Ein einzelner fehlschlagender Publish darf den Leser nicht fuer immer auf
  // derselben Zeile festhalten. Nach 3 Versuchen (bei bestehender Verbindung)
  // wird die Zeile verbraucht; bei getrennter Verbindung wird nie uebersprungen.
  static uint32_t failEol = 0;
  static uint8_t failCount = 0;
  while (f.available() && sent < MAX_LINES_PER_LOOP) {
    int ci = f.read();
    if (ci < 0) break;
    char c = (char)ci;
    if (c == '\r' || c == '\n') {
      uint32_t eol = f.position();
      // Position nur ueber BESTAETIGTE Zeilen stellen: schlug das Senden
      // fehl, bleibt lastPos stehen und die Zeile kommt naechsten Poll
      // erneut dran - keine Zeile wird uebersprungen, keine geht verloren.
      // Muell/Leerzeilen werden verbraucht, aber nie gesendet.
      bool weiter = true;
      if (lineOk && lineLen > 0) {
        lineBuf[lineLen] = '\0';   // Platz ist da: Puffer ist LINE_BUF_SIZE + 1
        String rec = lineBuf;   // genau EINE Allokation
        rec.trim();
        if (rec.length() > 0) {
          // Nur die letzte Zeile der Datei aktualisiert die HA-Sensoren.
          // Alles davor ist Rueckstand und geht ausschliesslich aufs Roh-Topic.
          bool isLatest = (eol + 1 >= fileSize);
          if (mqttPublishLine(rec, eol, isLatest)) {
            lastPos = eol;
            sent++;
            failCount = 0;
          } else {
            bool aufgeben = false;
            if (mqtt.connected()) {
              if (failEol == eol) {
                if (++failCount >= 3) aufgeben = true;
              } else {
                failEol = eol;
                failCount = 1;
              }
            }
            if (aufgeben) {
              Serial.println(F("[MQTT] Zeile 3x fehlgeschlagen, wird uebersprungen"));
              lastPos = eol;
              sent++;
              failCount = 0;
            } else {
              Serial.println(F("[MQTT] Senden fehlgeschlagen, Rest naechsten Poll"));
              weiter = false;
            }
          }
        } else {
          lastPos = eol;
        }
      } else {
        lastPos = eol;
      }
      saveReadPosition(lastPos);
      lineLen = 0;
      lineOk = true;
      yield();
      if (!weiter) break;
      continue;
    }
    if (lineLen < LINE_BUF_SIZE) {
      lineBuf[lineLen++] = c;
    } else {
      lineOk = false;
    }
    yield();
  }

  if (f.available()) {
    Serial.println(F("[SD] noch mehr neue Zeilen, Rest naechsten Loop"));
  }
  saveLinesTotal(linesTotal);
  publishTelemetryThrottled(fileSize, lastPos);

  f.close();
  rotateIfPublished(fileSize, lastPos);
  releaseBus();
  mqtt.loop();
}