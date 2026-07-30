# MQTT-Protokoll WT32-ETH01 <-> nfc_access_control-Addon

Gegenstueck im Addon: `nfc_access_control/app/mqtt_bridge.py` (APDU-Relay
angebunden) und `.../hap_accessory.py` + `.../modules/homekey_service.py`
(HAP-Pairing, siehe ha-nfc-addon-Repo).

## Topics

### `nfc/raw` (ESP32 -> Addon)

Wird bei jeder erkannten Karte einmalig gesendet.

```json
{
  "uid": "04A1B2C3",
  "sak": 32,
  "atqa": "0044",
  "session_id": 17,
  "iso14443_4": true
}
```

- `session_id`: fortlaufende, pro Boot eindeutige Nummer. Identifiziert diesen
  einen Kartenvorgang gegenueber `nfc/apdu_cmd`/`nfc/apdu_resp`/`nfc/result`.
- `iso14443_4`: `SAK & 0x20 != 0`. Nur wenn true sind weitere APDUs moeglich
  (DESFire, HomeKey). Bei false (reine UID-Tags, Mifare Classic) gibt es
  keinen APDU-Kanal -- die Karte bleibt trotzdem kurz selektiert, bis
  `nfc/result` kommt oder ein Timeout (2s) ablaeuft.

### `nfc/apdu_cmd` (Addon -> ESP32)

Nur sinnvoll, wenn die zugehoerige `nfc/raw`-Meldung `iso14443_4: true` hatte.
Die Karte MUSS noch im Feld sein (die Firmware haelt sie bis zum
Sessionende bzw. 3s Inaktivitaet selektiert).

```json
{"session_id": 17, "apdu_hex": "905A0000030112330000"}
```

- `apdu_hex`: das rohe ISO7816-Kommando (inkl. CLA/INS/P1/P2/Lc/Data/Le),
  wie es z.B. `homekey_lib` oder der DESFire-Code bereits als APDU-Bytes
  baut -- hex-kodiert, ohne Leerzeichen.

Die Firmware fuehrt darauf ein PN532 `InDataExchange` mit dem zuletzt per
`InListPassiveTarget` gefundenen Target aus und antwortet auf
`nfc/apdu_resp`.

### `nfc/apdu_resp` (ESP32 -> Addon)

```json
{"session_id": 17, "ok": true, "response_hex": "9000"}
```
oder bei Fehler:
```json
{"session_id": 17, "ok": false, "error": "pn532_data_exchange fehlgeschlagen"}
```

`response_hex` enthaelt die rohen Antwortbytes inkl. SW1/SW2 (wie sie ein
ISO7816-Reader normalerweise liefert), analog zu
`homekey_lib/util/iso7816.py:ISO7816Response.unpack()`.

### `nfc/homekey_group_id` (Addon -> ESP32, retained)

```
6a02cb0206021100...  (16 Hex-Zeichen = 8 Byte, KEIN JSON -- reiner Hex-String als Payload)
```

Die 8-Byte `reader_group_identifier`, die der ECP-Broadcast-Frame (siehe
`pn532_uart.c:pn532_send_homekey_broadcast()`) an wartende iPhones/Watches
sendet. Wird vom Addon retained veroeffentlicht, sobald sich der HomeKey-
Reader-Key aendert (siehe `ha-nfc-addon/nfc_access_control/app/main.py:
_on_homekey_reader_key_changed()`), damit ein iPhone mit bereits
eingerichtetem Home Key den Reader ueberhaupt als Teil seiner "Haushalts"-
Gruppe erkennt. Ungueltige/zu kurze Payloads werden ignoriert (Log-Warnung),
der zuletzt gueltige Wert bleibt bestehen. Vor dem allerersten HAP-Pairing
kommt hier nichts an -- die Firmware sendet dann weiterhin `00...00` als
Identifier (siehe `pn532_uart.c`), der Broadcast wird trotzdem gesendet, nur
erkennt kein Geraet den Reader als "seinen".

### `nfc/relay_pulse_ms` (Addon -> ESP32, retained, optional)

```
1500   (KEIN JSON -- reiner Zahl-String in Millisekunden als Payload, 50-10000)
```

Nur relevant, wenn ueber die WebGUI (siehe README) "Pulsdauer per MQTT
setzen" aktiviert wurde -- dann abonniert die Firmware dieses Topic
zusaetzlich und uebernimmt jeden gueltigen Wert sofort per
`relay_control_set_pulse_ms()` (siehe `relay_control.c`), OHNE ihn in NVS zu
persistieren. Werte ausserhalb 50-10000 oder nicht-numerische Payloads werden
mit einer Log-Warnung ignoriert, der zuletzt gueltige Wert bleibt bestehen.
Ist die Option deaktiviert (Standard), wird dieses Topic gar nicht erst
abonniert und die feste, ueber die WebGUI konfigurierte Pulsdauer gilt.

### `nfc/result` (Addon -> ESP32, bereits vorhanden)

Unveraendertes Format. Wird zusaetzlich als **Sessionende** interpretiert:
Sobald diese Nachricht eintrifft (mit oder ohne `session_id`-Feld -- falls
vorhanden wird geprueft, ob sie zur aktuell offenen APDU-Relay-Session
passt), beendet die Firmware die APDU-Relay-Schleife, gibt das Target frei
(`InRelease`) und pollt sofort weiter.

## Bekannte Einschraenkung

Die PN532-Frame-Engine (`pn532_uart.c`) unterstuetzt keine
Extended-Length-Frames (Antworten/Kommandos > ~255 Byte). Fuer den
FAST- und STANDARD-Auth-Flow von HomeKey sowie normale DESFire-Operationen
reicht das. Der HomeKey-ATTESTATION-Flow (grosse CBOR/mdoc-Envelopes beim
allerersten Pairing eines neuen Geraets ohne bekannten Issuer) kann das
ueberschreiten und ist damit noch nicht abgedeckt.

Real-Hardware-Test bestaetigt: In diesem Fall setzt der PN532 in der
InDataExchange-Antwort Bit 0x40 ("more data folgt"), das
`pn532_data_exchange_once()` bis vor kurzem stillschweigend ignoriert und
maskiert hat (`status = raw_resp[0] & 0x3F`) -- dadurch kam beim Addon ein
unbemerkt abgeschnittenes CBOR-Paket an, das dort erst viel spaeter beim
Parsen mit einem verwirrenden "index out of bounds"-Fehler abgestuerzt ist,
statt dass der eigentliche Grund (Antwort zu lang) sichtbar wurde. Als
Zwischenschritt brach `pn532_data_exchange_once()` bei gesetztem 0x40-Bit
kurzzeitig sauber mit `ESP_ERR_NOT_SUPPORTED` ab, statt still abzuschneiden.

Mittlerweile ist die Fortsetzungslogik implementiert: sobald Bit 0x40
gesetzt ist, fragt `pn532_data_exchange_once()` das naechste Antwortstueck
per weiterem InDataExchange (Parameter nur Zielnummer-Byte, keine neuen
APDU-Daten) ab und haengt es an den Antwortpuffer an, bis das Bit nicht mehr
gesetzt ist. `MQTT_APDU_MAX_LEN` wurde dafuer von 250 auf 2048 Byte erhoeht
(inkl. entsprechend vergroesserter MQTT-Puffer, Task-Stacks und Umstellung
der betroffenen grossen Puffer in `mqtt_client_setup.c` von Stack- auf
Heap-Allokation).

**Wichtig:** Das genaue Fortsetzungs-Wireformat (Anfrage nur mit
Zielnummer-Byte) ist aus einem gaengigen/plausiblen PN532-Verwendungsmuster
abgeleitet, nicht gegen das NXP-Datenblatt verifiziert. Muss auf echter
Hardware (insbesondere beim HomeKey-ATTESTATION-Flow) bestaetigt werden --
die Firmware loggt dabei `"InDataExchange: hole Fortsetzung ab..."` je
abgeholtem Stueck, das sollte im seriellen Log sichtbar sein.

Erste Real-Hardware-Tests zeigten, dass die Karte zwischen Erkennung und dem
ersten APDU (die Zeit fuer den MQTT/Worker-Thread-Rundlauf zum Addon und
zurueck, in der Praxis ~700ms) ihre ISO14443-4-Sitzung verlieren kann (PN532
InDataExchange-Status 0x01 "Timeout, target did not answer") -- vermutlich
weil die vom Kartentyp im ATS ausgehandelte Frame Waiting Time ueberschritten
wird, obwohl die Karte physisch im Feld bleibt. `pn532_data_exchange()`
versucht deshalb bei einem Fehler einmalig eine Re-Aktivierung (erneutes
`InListPassiveTarget`) und wiederholt das APDU danach genau einmal. Das ist
fuer das JEWEILS ERSTE APDU einer Session unbedenklich (kein Auth-Zustand auf
der Karte, der durch die Re-Aktivierung verloren gehen koennte). Tritt
derselbe Fehler spaeter im Auth-Handshake auf, setzt die Re-Aktivierung die
Karte ebenfalls zurueck -- das faellt aber nicht still unter den Tisch,
sondern die Reader-Seite (homekey_lib/DESFireSession) erkennt die daraufhin
nicht mehr passende Kryptoantwort der Karte zuverlaessig als Auth-Fehler statt
sie faelschlich zu akzeptieren.

## MIFARE Classic: Re-Selektion vor jedem nativen Kommando

Auf echter Hardware reproduziert: identisches natives Auth-Kommando (`60`
+ Block + Key + UID) lieferte je nach Vorgeschichte abwechselnd Statusbyte
`14` (Auth fehlgeschlagen) oder `00` (erfolgreich) -- auch wenn per
externem Tool (`mfoc`) verifiziert war, dass der Key fuer den Sektor
korrekt war. Ursache: ein vorher fehlgeschlagener Auth-Versuch (z.B. auf
einem anderen Sektor mit falschem Key, etwa beim sektorweisen
Durchprobieren via NFC-Shell) hinterlaesst die Karte in einem "verwirrten"
Crypto1-Zustand, der auch nachfolgende, eigentlich korrekte Auth-Versuche
mit `14` ablehnt -- bis die Karte sauber neu selektiert (HALT/WakeUp +
Anticollision + SELECT) wird. `mfoc`/libnfc reselektieren aus demselben
Grund vor jedem Dictionary-Versuch.

Behoben in `pn532_data_exchange_ex()`: fuer `native=true` (MIFARE Classic)
wird jetzt vor JEDEM Kommando per `pn532_reactivate_target()` frisch
reselektiert, nicht mehr nur bei echten Kommunikationsfehlern. Fuer den
ISO14443-4-Pfad (DESFire/HomeKey, `native=false`) bleibt das Verhalten
unveraendert -- dort MUSS die Selektion ueber eine ganze APDU-Kette
erhalten bleiben, eine Re-Selektion wuerde den kryptografischen
Sitzungszustand (Auth0/Auth1 bei HomeKey, Session-Keys bei DESFire)
zerstoeren.

## Offene Folgeschritte

- Mifare Classic/Crypto1-Unterstuetzung (addon-seitig, `mifare_classic_module.py`)
  ist noch nicht implementiert.
- Kein Test mit echter Hardware (WT32-ETH01 + PN532 + echter MQTT-Broker +
  echtem iPhone/Watch) -- alles bisher nur gegen simulierte/mock APDU-
  Antworten verifiziert.
