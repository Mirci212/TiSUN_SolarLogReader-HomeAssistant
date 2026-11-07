// fields.h - TiSUN MULTIPLEX: Spalten aus WE2611.DAT, abgeglichen mit dem Display
//
// Logzeile 18:30 : 1106;1830;10;46;199;30;199;-40;-40;-40;-40;-40;-40;0.0;0.0;0;0;...;183297;023977;25;1.5;...;EU;6123;...
// Display        : T1= 5C  T2=46C  T3=199C (Ausser Betrieb)  T4=29C  T5=199C (Ausser Betrieb)
//                  P1=0%  Leistung 0.0kW  Energie 183297kWh  Betrieb 23977h  dT 0C
//
// Daraus folgt sicher:
//   0 = Datum MMDD (1106), 1 = Uhrzeit HHMM (1830)
//   2..6 = T1..T5 ROH in Grad Celsius -> KEIN Teiler (divider 1)!
//          Gegenprobe: T2 = 46 im Log = 46 C am Display, T3/T5 = 199 = "Ausser Betrieb",
//          T4 = 30 im Log / 29 C am Display (1 Grad Drift ueber den Tag).
//   25 = Energie in kWh (183297), 26 = Betriebsstunden (23977) - beide 1:1
//   7..12 = -40 = Fuehler nicht belegt (wird nicht veroeffentlicht)
//   13 = dT, 14 = Leistung kW (beide 0.0 am Display), 15 = P1 in %
//   27 = Temperatur GDS1 (21 = 21 C), 28 = Durchfluss GDS1 (1.5 L/min)
//       Gegenprobe am Geraet: Spalte 27 ist die Temperatur, Spalte 28 der
//       Volumenstrom. divider 1 (statt 0) haelt die Nachkommastelle, sonst
//       wuerde 1.5 zu "2" gerundet.
//       Namen bewusst nur "Solar T1..T5", Rollen (Boiler/Puffer) stehen am
//       Geraet auf dem Aufkleber, nicht im Sensornamen.
//   39 = EU (Sprache), 40 = 6123 (Variante), Rest Status/Timer
//
// lo/hi = Plausibilitaet: alles ausserhalb wird NICHT gesendet (Topic wird
// geleert), damit 199 ("Ausser Betrieb") oder -40 ("unbelegt") nicht als
// Messwert im Diagramm landen. 0/0 = keine Grenze.
//
// Umbenennen ohne Reflash (Topic home/solarlog/set_config):
//   {"index":4,"key":"t_kollektor","name":"Kollektor","unit":"°C","divider":1,"min":-35,"max":150}
// Min/Max gelten zur Laufzeit (nicht im EEPROM); nach Reboot kommen die
// Grenzen unten wieder. {"cmd":"list"} zeigt alle Felder inkl. min/max.

#ifndef FIELDS_H
#define FIELDS_H

struct FieldDef {
  uint8_t index;
  const char* key;
  const char* name;
  const char* unit;
  float divider; // 0 = ganzzahlig roh, 1/10/... = durch diesen Wert teilen
  float lo;      // Plausibilitaet min
  float hi;      // Plausibilitaet max
  const char* icon; // mdi-Name fuer Home Assistant ("" = kein Icon)
};

static const FieldDef FIELDS[] = {
  { 0,   "datum",     "Solar Datum",       "",      0,   0,     0,        "mdi:calendar" },
  { 1,   "uhrzeit",   "Solar Uhrzeit",     "",      0,   0,     0,        "mdi:clock-outline" },
  { 2,   "t1",        "Solar T1",          "°C",    1,   -35,   150,      "mdi:storage-tank" },
  { 3,   "t2",        "Solar T2",          "°C",    1,   -35,   150,      "mdi:storage-tank" },
  { 4,   "t3",        "Solar T3",          "°C",    1,   -35,   150,      "mdi:storage-tank" },
  { 5,   "t4",        "Solar T4",          "°C",    1,   -35,   150,      "mdi:storage-tank" },
  { 6,   "t5",        "Solar T5",          "°C",    1,   -35,   150,      "mdi:storage-tank" },
  { 13,  "dt",        "Solar dT",          "K",     1,   -35,   150,      "mdi:thermometer" },
  { 14,  "leistung",  "Solar Leistung",    "kW",    1,   0,     200,      "mdi:solar-power" },
  { 15,  "p1",        "Solar P1",          "%",     0,   0,     100,      "mdi:pump" },
  { 16,  "p2",        "Solar P2",          "%",     0,   0,     100,      "mdi:pump" },
  { 27,  "gds1temp",  "Solar GDS1 Temperatur", "°C",    0,   0,     100,      "mdi:thermometer" },
  { 28,  "gds1fluss", "Solar GDS1 Durchfluss", "L/min", 1,   0,     100,      "mdi:pump" },
  { 25,  "energie",   "Solar Energie",     "kWh",   0,   0,     2000000,  "mdi:lightning-bolt" },
  { 26,  "betrieb",   "Solar Betrieb",     "h",     0,   0,     200000,   "mdi:counter" },
};

static const uint8_t NUM_FIELDS = sizeof(FIELDS) / sizeof(FIELDS[0]);

#endif