# Remote: Feldnamen per MQTT ändern

Die vollständige Anleitung steht in **[README.md](README.md)** — hier nur die
Kurzfassung. **Kein OTA:** Firmware wird ausschließlich per USB geflasht.

## Feldnamen / Einheiten / Grenzen per MQTT

Topic `home/solarlog/set_config`, Antwort `home/solarlog/config_state`.

```json
{"index":4,"key":"kollektor","name":"Kollektor","unit":"°C","divider":1,"min":-35,"max":150}
{"cmd":"list"}
{"cmd":"reset"}
```

- `index` = Spalte in der Logzeile ab 0 (`1106;1830;10;…` → 0 = 1106)
- `key` = MQTT-Endung (`home/solarlog/<key>`), klein + Unterstrich
- `divider` = `0` ganzzahlig roh, `1` eine Nachkommastelle, `10` durch 10 teilen
- `min`/`max` = Plausibilität; außerhalb wird **nicht** gesendet

Die Werte liegen im EEPROM und überleben einen Neustart. Die HA-Entities werden
per Discovery neu angelegt, kein Neustart nötig.
