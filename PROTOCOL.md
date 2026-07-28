# MQTT-Protokoll WT32-ETH01 <-> nfc_access_control-Addon

Gegenstueck im Addon: `nfc_access_control/app/mqtt_bridge.py` (dort ist der
APDU-Teil noch NICHT angebunden -- `pn532_transceive` wirft aktuell noch
`NotImplementedError`, siehe `_process_card()`. Das ist der naechste
Arbeitsschritt.)

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

## Offene Folgeschritte

1. Addon-seitig `mqtt_bridge.py`/`router.py` so erweitern, dass
   `pn532_transceive` tatsaechlich `nfc/apdu_cmd` published und synchron auf
   die passende `nfc/apdu_resp` wartet (Request/Response-Korrelation ueber
   `session_id`).
2. HAP-Accessory-Server im Addon (siehe `ha-nfc-addon`-Repo,
   `homekey_lib/NOTICE.md`), damit `HOMEKEY_GROUP_IDENTIFIER` in `main.c`
   nicht mehr hart auf `00...00` steht, sondern vom Addon nach erfolgreichem
   Pairing gesetzt werden kann (z.B. ueber ein retained Config-Topic).
