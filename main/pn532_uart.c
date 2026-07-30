/*
 * PN532-Kommunikation über UART (HSU Mode)
 *
 * Pin-Belegung (WT32-ETH01):
 *   - TX: IO14 (ESP32) -> RX (PN532)
 *   - RX: IO15 (ESP32) -> TX (PN532) [mit internem Pull-Up]
 *
 * Neben den Basisbefehlen (SAMConfiguration, InListPassiveTarget) enthaelt
 * dieses Modul jetzt auch:
 *   - Den ECP-Broadcast-Frame (InCommunicateThru + CRC-A), der ein iPhone/
 *     eine Watch mit Apple Home Key ueberhaupt erst dazu bringt, auf
 *     InListPassiveTarget zu antworten. Byte-fuer-Byte nachgebildet aus
 *     kormax/apple-home-key-reader (util/bfclf.py:sense_broadcast() und
 *     util/ecp.py:ECP.home()), dort via nfcpy/PN532-Chipset-Kommandos.
 *   - InDataExchange, um nach der Erkennung weitere ISO7816-APDUs mit einer
 *     bereits selektierten Karte auszutauschen (fuer DESFire/HomeKey; wird
 *     vom MQTT-APDU-Relay in main.c benutzt).
 */

#include "pn532_uart.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "pn532_uart";

#define PN532_UART_PORT      UART_NUM_1
#define PN532_UART_TX_PIN    14
#define PN532_UART_RX_PIN    15
#define PN532_UART_BAUDRATE  115200
#define PN532_UART_BUF_SIZE  512

// PN532-Kommandocodes (Host -> PN532, TFI 0xD4)
#define PN532_CMD_WRITE_REGISTER          0x08
#define PN532_CMD_RF_CONFIGURATION        0x32
#define PN532_CMD_IN_DATA_EXCHANGE        0x40
#define PN532_CMD_IN_COMMUNICATE_THRU     0x42
#define PN532_CMD_IN_LIST_PASSIVE_TARGET  0x4A
#define PN532_CMD_IN_RELEASE              0x52
#define PN532_CMD_SAM_CONFIGURATION       0x14

// CIU-Registeradressen (identisch zu nfcpy's REG-Tabelle, siehe
// nfc/clf/pn53x.py -- der CIU-Kern des PN532 ist zum MFRC522 kompatibel)
#define PN532_REG_CIU_BIT_FRAMING  0x633D

static const uint8_t PN532_ACK_FRAME[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// HomeKey-ECP: Terminal-Type "Access" / Subtype "HomeKey", TCI-Praefix.
// Siehe homekey_lib/util/ecp.py:TYPE_ACCESS, SUBTYPE_HOMEKEY, TCI_HOMEKEY.
#define ECP_HEADER           0x6A
#define ECP_VERSION_2        0x02
#define ECP_TERMINAL_TYPE_ACCESS    0x02
#define ECP_TERMINAL_SUBTYPE_HOMEKEY 0x06
static const uint8_t ECP_TCI_HOMEKEY[] = {0x02, 0x11, 0x00};

static uint8_t s_homekey_group_identifier[8] = {0};
static uint8_t s_last_target_number = 0;
static bool s_target_selected = false;

// Sicherheitsmarge fuer ein paar ISO14443-4-WTX-Verlaengerungen obendrauf auf
// das reine FWT -- der PN532 bedient WTX-Anfragen der Karte chip-intern
// transparent, aber nur innerhalb des Timeouts, das wir ihm hier mitgeben.
#define PN532_RESPONSE_TIMEOUT_WTX_MARGIN_FACTOR 4
#define PN532_RESPONSE_TIMEOUT_MIN_MS   200
#define PN532_RESPONSE_TIMEOUT_MAX_MS   5000
// Fallback, falls eine Karte kein TB(1) (und damit kein FWI) in ihrer ATS
// mitliefert -- ISO14443-4 erlaubt das (Default FWI=4, ~4.8ms), das ist uns
// aber zu knapp, deshalb hier grosszuegiger.
#define PN532_RESPONSE_TIMEOUT_DEFAULT_MS 1000

static uint32_t s_response_timeout_ms = PN532_RESPONSE_TIMEOUT_DEFAULT_MS;

/* Berechnet und merkt sich das InDataExchange-Timeout aus dem FWI-Nibble in
 * TB(1) der ATS (Nachbau von nfcpy tt4.py: fwt = 4096/13.56MHz * 2^FWI) --
 * siehe pn532_get_response_timeout_ms() im Header fuer das Warum. ats zeigt
 * auf T0 (erstes Byte NACH dem TL-Laengenbyte), ats_len ist die Anzahl
 * verbleibender ATS-Bytes ab T0. */
// Fuer Diagnose-Hexdumps -- cap bei 48 Byte, reicht fuer eine komplette
// InListPassiveTarget-Antwort (NbTg..ATS) in der Praxis locker aus.
#define PN532_HEXDUMP_CAP 48

static void pn532_log_hex(const char *tag_suffix, const char *prefix, const uint8_t *data, size_t len)
{
    char hex[3 * PN532_HEXDUMP_CAP + 1] = {0};
    size_t n = len < PN532_HEXDUMP_CAP ? len : PN532_HEXDUMP_CAP;
    for (size_t i = 0; i < n; i++) {
        snprintf(&hex[i * 3], 4, "%02X ", data[i]);
    }
    ESP_LOGI(TAG, "%s%s (%d Byte): %s", prefix, tag_suffix, (int)len, hex);
}

static void pn532_log_ats_hex(const char *prefix, const uint8_t *ats, size_t ats_len)
{
    pn532_log_hex(" ab T0", prefix, ats, ats_len);
}

static void pn532_update_response_timeout_from_ats(const uint8_t *ats, size_t ats_len)
{
    s_response_timeout_ms = PN532_RESPONSE_TIMEOUT_DEFAULT_MS;
    pn532_log_ats_hex("ATS", ats, ats_len);

    if (ats_len < 1) {
        ESP_LOGI(TAG, "ATS leer -> Default-Timeout %u ms", (unsigned)s_response_timeout_ms);
        return;
    }

    uint8_t t0 = ats[0];
    size_t idx = 1;
    if (t0 & 0x10) idx++;  // TA(1) vorhanden -> ueberspringen
    if (!(t0 & 0x20) || idx >= ats_len) {
        ESP_LOGI(TAG, "ATS ohne TB(1)/FWI (T0=0x%02X) -> Default-Timeout %u ms", t0,
                 (unsigned)s_response_timeout_ms);
        return;
    }

    uint8_t fwi = (ats[idx] >> 4) & 0x0F;
    double fwt_ms = (4096.0 / 13560000.0) * (double)(1u << fwi) * 1000.0;
    double timeout_ms = fwt_ms * PN532_RESPONSE_TIMEOUT_WTX_MARGIN_FACTOR;

    if (timeout_ms < PN532_RESPONSE_TIMEOUT_MIN_MS) timeout_ms = PN532_RESPONSE_TIMEOUT_MIN_MS;
    if (timeout_ms > PN532_RESPONSE_TIMEOUT_MAX_MS) timeout_ms = PN532_RESPONSE_TIMEOUT_MAX_MS;

    s_response_timeout_ms = (uint32_t)timeout_ms;
    ESP_LOGI(TAG, "ATS: FWI=%u -> InDataExchange-Timeout = %u ms", (unsigned)fwi,
             (unsigned)s_response_timeout_ms);
}

uint32_t pn532_get_response_timeout_ms(void)
{
    return s_response_timeout_ms;
}

void pn532_set_homekey_group_identifier(const uint8_t identifier[8])
{
    memcpy(s_homekey_group_identifier, identifier, sizeof(s_homekey_group_identifier));
}

esp_err_t pn532_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = PN532_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(PN532_UART_PORT, PN532_UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PN532_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(PN532_UART_PORT, PN532_UART_TX_PIN, PN532_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    gpio_set_pull_mode(PN532_UART_RX_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "PN532 UART (TX=%d, RX=%d) initialisiert", PN532_UART_TX_PIN, PN532_UART_RX_PIN);
    return ESP_OK;
}

uart_port_t pn532_uart_get_port(void)
{
    return PN532_UART_PORT;
}

/* ISO/IEC 14443-3 CRC_A (Praeset 0x6363), wird u.a. fuer den rohen
 * ECP-Broadcast-Frame gebraucht, den InCommunicateThru NICHT automatisch
 * mit CRC versieht (anders als z.B. InListPassiveTarget-Kommandos, die vom
 * PN532 intern per ISO14443-Framing behandelt werden). */
static void crc_a(const uint8_t *data, size_t len, uint8_t crc_out[2])
{
    uint16_t w_crc = 0x6363;
    for (size_t i = 0; i < len; i++) {
        uint8_t bt = data[i];
        bt ^= (uint8_t)(w_crc & 0x00FF);
        bt ^= (uint8_t)(bt << 4);
        w_crc = (uint16_t)((w_crc >> 8) ^ ((uint16_t)bt << 8) ^ ((uint16_t)bt << 3) ^ (bt >> 4));
    }
    crc_out[0] = (uint8_t)(w_crc & 0xFF);
    crc_out[1] = (uint8_t)((w_crc >> 8) & 0xFF);
}

static void pn532_send_frame(const uint8_t *tfi_and_data, size_t len)
{
    uart_flush_input(PN532_UART_PORT);

    static const uint8_t wakeup[] = {0x55, 0x55, 0x00, 0x00, 0x00, 0x00};
    uart_write_bytes(PN532_UART_PORT, (const char *)wakeup, sizeof(wakeup));

    uint8_t header[] = {0x00, 0x00, 0xFF};
    uart_write_bytes(PN532_UART_PORT, (const char *)header, sizeof(header));

    uint8_t lcs = (uint8_t)(0x100 - len);
    uint8_t len_field[] = {(uint8_t)len, lcs};
    uart_write_bytes(PN532_UART_PORT, (const char *)len_field, sizeof(len_field));

    uart_write_bytes(PN532_UART_PORT, (const char *)tfi_and_data, len);

    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += tfi_and_data[i];
    uint8_t dcs = (uint8_t)(0x100 - sum);
    uint8_t tail[] = {dcs, 0x00};
    uart_write_bytes(PN532_UART_PORT, (const char *)tail, sizeof(tail));
}

static bool pn532_wait_for_ack(int timeout_ms)
{
    uint8_t buf[sizeof(PN532_ACK_FRAME)];
    int len = uart_read_bytes(PN532_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(timeout_ms));
    if (len != sizeof(PN532_ACK_FRAME)) return false;
    return memcmp(buf, PN532_ACK_FRAME, sizeof(PN532_ACK_FRAME)) == 0;
}

/* Liest genau EIN normales PN532-Antwort-Frame:
 *   00 00 FF LEN LCS D5 <RESPCODE> <DATA...> DCS 00
 * *out_data erhaelt <DATA...> (ohne TFI 0xD5 und RESPCODE).
 * Unterstuetzt KEINE Extended-Length-Frames (LEN==0xFF) -- fuer unsere
 * Kommandos (Register/RFConfig/Broadcast/List/DataExchange mit kurzen APDUs)
 * bleibt die Antwort immer unter 255 Byte. */
static esp_err_t pn532_read_response_frame(uint8_t *out_data, size_t out_cap,
                                            size_t *out_len, uint8_t *out_response_code,
                                            uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int zero_count = 0;
    uint8_t b;

    // Praeambel/Startcode ueberlesen, bis "00 00 FF" gefunden wurde
    while (1) {
        if (esp_timer_get_time() > deadline_us) return ESP_ERR_TIMEOUT;
        int n = uart_read_bytes(PN532_UART_PORT, &b, 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        if (b == 0xFF && zero_count >= 2) break;
        zero_count = (b == 0x00) ? zero_count + 1 : 0;
    }

    uint8_t len_lcs[2];
    if (uart_read_bytes(PN532_UART_PORT, len_lcs, 2, pdMS_TO_TICKS(200)) != 2) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t len = len_lcs[0];

    if (len == 0x00) {
        // Das war ein ACK-Frame (LEN=00 LCS=FF), keine echte Antwort -- der
        // Aufrufer wartet i.d.R. schon separat per pn532_wait_for_ack() darauf,
        // aber falls wir hier landen: sauber weiterlesen statt haengenzubleiben.
        uint8_t discard;
        uart_read_bytes(PN532_UART_PORT, &discard, 1, pdMS_TO_TICKS(50));
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (len == 0xFF) {
        return ESP_ERR_NOT_SUPPORTED;  // Extended-Length-Frame, nicht unterstuetzt
    }

    uint8_t frame_data[300];
    if ((size_t)len + 1 > sizeof(frame_data)) return ESP_ERR_INVALID_SIZE;

    int remaining_ms = (int)((deadline_us - esp_timer_get_time()) / 1000);
    if (remaining_ms < 50) remaining_ms = 50;
    if (uart_read_bytes(PN532_UART_PORT, frame_data, len + 1, pdMS_TO_TICKS(remaining_ms)) != len + 1) {
        return ESP_ERR_TIMEOUT;
    }

    if (len < 2 || frame_data[0] != 0xD5) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_response_code = frame_data[1];
    size_t data_len = (size_t)len - 2;
    if (data_len > out_cap) data_len = out_cap;
    memcpy(out_data, &frame_data[2], data_len);
    *out_len = data_len;

    // Postamble (0x00) noch abholen, best effort
    uint8_t postamble;
    uart_read_bytes(PN532_UART_PORT, &postamble, 1, pdMS_TO_TICKS(20));

    return ESP_OK;
}

/* Sendet ein D4-Kommando (cmd_code + params), wartet auf ACK und liest die
 * zugehoerige Antwort. out_data erhaelt die Nutzdaten der Antwort (ohne
 * Response-Code, der implizit auf cmd_code+1 geprueft wird). */
static esp_err_t pn532_command(uint8_t cmd_code, const uint8_t *params, size_t params_len,
                                uint8_t *out_data, size_t out_cap, size_t *out_len,
                                uint32_t timeout_ms)
{
    uint8_t frame[300];
    if (params_len > sizeof(frame) - 2) return ESP_ERR_INVALID_SIZE;
    frame[0] = 0xD4;
    frame[1] = cmd_code;
    if (params_len) memcpy(&frame[2], params, params_len);
    pn532_send_frame(frame, 2 + params_len);

    if (!pn532_wait_for_ack(200)) {
        ESP_LOGW(TAG, "Kein ACK fuer Kommando 0x%02X", cmd_code);
        return ESP_ERR_TIMEOUT;
    }

    size_t local_len = 0;
    uint8_t response_code = 0;
    esp_err_t err = pn532_read_response_frame(out_data, out_cap, &local_len, &response_code, timeout_ms);
    if (err != ESP_OK) return err;

    if (response_code != (uint8_t)(cmd_code + 1)) {
        ESP_LOGW(TAG, "Unerwarteter Response-Code 0x%02X (erwartet 0x%02X) fuer Kommando 0x%02X",
                 response_code, (uint8_t)(cmd_code + 1), cmd_code);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_len) *out_len = local_len;
    return ESP_OK;
}

static esp_err_t pn532_write_register(uint16_t addr, uint8_t value)
{
    uint8_t params[] = {(uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), value};
    uint8_t resp[8];
    size_t resp_len = 0;
    return pn532_command(PN532_CMD_WRITE_REGISTER, params, sizeof(params), resp, sizeof(resp), &resp_len, 200);
}

static esp_err_t pn532_rf_configuration(uint8_t cfg_item, const uint8_t *cfg_data, size_t cfg_len)
{
    uint8_t params[16];
    if (cfg_len + 1 > sizeof(params)) return ESP_ERR_INVALID_SIZE;
    params[0] = cfg_item;
    memcpy(&params[1], cfg_data, cfg_len);
    uint8_t resp[8];
    size_t resp_len = 0;
    return pn532_command(PN532_CMD_RF_CONFIGURATION, params, cfg_len + 1, resp, sizeof(resp), &resp_len, 200);
}

esp_err_t pn532_sam_configuration(void)
{
    uint8_t params[] = {0x01, 0x00};
    uint8_t resp[8];
    size_t resp_len = 0;
    esp_err_t err = pn532_command(PN532_CMD_SAM_CONFIGURATION, params, sizeof(params), resp, sizeof(resp), &resp_len, 200);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "SAMConfiguration erfolgreich");

    // Basiskonfiguration wie in nfcpy's PN532-Init (nfc/clf/pn532.py:Device.__init__):
    // WICHTIG ist vor allem Item 0x05 (MaxRetries) mit MxRtyPassiveActivation=1 --
    // ohne das wuerde InListPassiveTarget bei leerem Feld je nach Werkseinstellung
    // sehr lange/unbegrenzt blockieren, statt schnell mit NbTg=0 zu antworten. Das
    // ist Voraussetzung dafuer, dass sich Polling und ECP-Broadcast (siehe
    // pn532_poll_once()) im schnellen Wechsel abwechseln koennen.
    static const uint8_t timings[] = {0x00, 0x0B, 0x0A};
    static const uint8_t max_retries[] = {0x01, 0x00, 0x01};
    pn532_rf_configuration(0x02, timings, sizeof(timings));
    pn532_rf_configuration(0x04, (const uint8_t[]){0x00}, 1);
    pn532_rf_configuration(0x05, max_retries, sizeof(max_retries));

    return ESP_OK;
}

/* Sendet den ECP-"Home"-Broadcast-Frame per InCommunicateThru, damit ein
 * HomeKey-faehiges Geraet im Feld aufwacht. Vor dem eigentlichen Broadcast
 * werden -- wie im Original -- Detection-Retries deaktiviert und ein kurzes
 * Antwort-Timeout gesetzt, damit die Broadcast-Sequenz nicht durch die
 * normalen Polling-Retries des PN532 gestoert wird. Ein Timeout beim
 * InCommunicateThru selbst ist normal (nicht jedes Geraet antwortet auf den
 * Broadcast) und wird hier nicht als Fehler gewertet. */
static void pn532_send_homekey_broadcast(void)
{
    static const uint8_t detection_retries[] = {0xFF, 0x01, 0x00};
    static const uint8_t timings[] = {0x0A, 0x0B, 0x08};
    pn532_rf_configuration(0x05, detection_retries, sizeof(detection_retries));
    pn532_rf_configuration(0x02, timings, sizeof(timings));
    pn532_write_register(PN532_REG_CIU_BIT_FRAMING, 0x00);

    uint8_t payload[5 + sizeof(ECP_TCI_HOMEKEY) + sizeof(s_homekey_group_identifier)];
    uint8_t terminal_info = (uint8_t)((1 << 7) | (1 << 6) | (sizeof(ECP_TCI_HOMEKEY) + sizeof(s_homekey_group_identifier)));
    payload[0] = ECP_HEADER;
    payload[1] = ECP_VERSION_2;
    payload[2] = terminal_info;
    payload[3] = ECP_TERMINAL_TYPE_ACCESS;
    payload[4] = ECP_TERMINAL_SUBTYPE_HOMEKEY;
    memcpy(&payload[5], ECP_TCI_HOMEKEY, sizeof(ECP_TCI_HOMEKEY));
    memcpy(&payload[5 + sizeof(ECP_TCI_HOMEKEY)], s_homekey_group_identifier, sizeof(s_homekey_group_identifier));

    uint8_t crc[2];
    crc_a(payload, sizeof(payload), crc);

    uint8_t broadcast_with_crc[sizeof(payload) + 2];
    memcpy(broadcast_with_crc, payload, sizeof(payload));
    broadcast_with_crc[sizeof(payload)] = crc[0];
    broadcast_with_crc[sizeof(payload) + 1] = crc[1];

    uint8_t resp[64];
    size_t resp_len = 0;
    // Kurzes Timeout: es ist normal, dass hier haeufig nichts antwortet.
    pn532_command(PN532_CMD_IN_COMMUNICATE_THRU, broadcast_with_crc, sizeof(broadcast_with_crc),
                  resp, sizeof(resp), &resp_len, 250);
}

esp_err_t pn532_poll_once(pn532_card_t *out_card, uint32_t timeout_ms)
{
    s_target_selected = false;

    // BrTy 0x00 = 106 kbps Type A (ISO14443A / Mifare / DESFire / HomeKey)
    uint8_t params[] = {0x01, 0x00};
    uint8_t resp[300];
    size_t resp_len = 0;

    esp_err_t err = pn532_command(PN532_CMD_IN_LIST_PASSIVE_TARGET, params, sizeof(params),
                                   resp, sizeof(resp), &resp_len, timeout_ms);

    if (err != ESP_OK || resp_len < 1 || resp[0] == 0) {
        // Nichts gefunden: HomeKey-Broadcast senden, damit ein wartendes
        // Geraet beim naechsten Zyklus antwortet.
        pn532_send_homekey_broadcast();
        return ESP_ERR_NOT_FOUND;
    }

    // resp: NbTg(1) Tg(1) SensRes(2) SelRes(1) NFCIDLength(1) NFCID(...) [ATSLength ATS...]
    // NFCIDLength steht bei Index 5 (NICHT 6!) -- verifiziert anhand echter
    // Rohdaten (siehe Kommentar unten und PROTOCOL.md): der vorherige Code
    // las Index 6 (das erste echte NFCID-Byte) faelschlich als Laenge und
    // haengte beim Kopieren ATS-Bytes mit in die "UID". Beispiel iPhone-
    // HomeKey-Session: RAW = 01 01 00 04 20 04 08 6B F6 01 05 78 80 70 02 --
    // NFCIDLength=resp[5]=04, NFCID=resp[6..9]=08 6B F6 01 (korrekt 4 Byte),
    // ATSLength=resp[10]=05, ATS=resp[11..14]=78 80 70 02 (T0=78 mit TB/TC,
    // FWI=8) -- Summe 5+1+4+1+4=15 Byte, exakt resp_len. Mit der alten,
    // falschen Annahme (NFCIDLength bei Index 6=08) wurden stattdessen 8 Byte
    // ab Index 7 kopiert (6B F6 01 05 78 80 70 02) und faelschlich als UID
    // "6BF6010578807002" gemeldet -- exakt die zuvor beobachtete falsche UID.
    if (resp_len < 6) return ESP_ERR_NOT_FOUND;

    // Diagnose: rohe InListPassiveTarget-Antwort komplett loggen, BEVOR
    // irgendeine Annahme ueber Feld-Offsets angewendet wird.
    pn532_log_hex("", "InListPassiveTarget RAW", resp, resp_len);

    uint8_t tg = resp[1];
    uint8_t uid_len = resp[5];
    if (uid_len > sizeof(out_card->uid) || (size_t)(6 + uid_len) > resp_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out_card, 0, sizeof(*out_card));
    out_card->atqa[0] = resp[2];
    out_card->atqa[1] = resp[3];
    out_card->sak = resp[4];
    out_card->uid_len = uid_len;
    memcpy(out_card->uid, &resp[6], uid_len);
    out_card->target_number = tg;
    out_card->iso14443_4 = (out_card->sak & 0x20) != 0;

    s_last_target_number = tg;
    s_target_selected = true;

    // ATS (falls vorhanden, [ATSLength ATS...] direkt nach der NFCID) auswerten,
    // um das InDataExchange-Timeout auf diese Karte/dieses Geraet abzustimmen
    // (siehe pn532_update_response_timeout_from_ats()). TL-Laengenbyte zaehlt
    // sich selbst mit, ats_len = TL - 1.
    s_response_timeout_ms = PN532_RESPONSE_TIMEOUT_DEFAULT_MS;
    size_t ats_tl_index = (size_t)6 + uid_len;
    if (out_card->iso14443_4 && ats_tl_index < resp_len) {
        uint8_t ats_tl = resp[ats_tl_index];
        if (ats_tl >= 1 && ats_tl_index + ats_tl <= resp_len) {
            pn532_update_response_timeout_from_ats(&resp[ats_tl_index + 1], ats_tl - 1);
        } else {
            ESP_LOGI(TAG, "ATSLength=%u unplausibel (resp_len=%d) -> Default-Timeout %u ms",
                     (unsigned)ats_tl, (int)resp_len, (unsigned)s_response_timeout_ms);
        }
    } else if (out_card->iso14443_4) {
        ESP_LOGI(TAG, "Keine ATS in der InListPassiveTarget-Antwort -> Default-Timeout %u ms",
                 (unsigned)s_response_timeout_ms);
    }

    return ESP_OK;
}

/* Holt ein einzelnes InDataExchange-Antwort-Frame ab und prueft Statusbyte
 * und Puffergrenzen. is_continuation=true bedeutet: params enthaelt NUR das
 * Zielnummer-Byte (keine neuen APDU-Daten) -- das ist die Anfrage an den
 * PN532, das naechste Stueck einer bereits laufenden "more data"-Antwort
 * nachzuliefern (siehe pn532_data_exchange_once()). *out_more zeigt an, ob
 * Bit 0x40 gesetzt war (weitere Fortsetzung noetig). */
static esp_err_t pn532_data_exchange_fetch(const uint8_t *params, size_t params_len,
                                            uint8_t *chunk, size_t chunk_cap,
                                            size_t *chunk_len, bool *out_more,
                                            uint32_t timeout_ms)
{
    uint8_t raw_resp[300];
    size_t raw_len = 0;
    esp_err_t err = pn532_command(PN532_CMD_IN_DATA_EXCHANGE, params, params_len,
                                   raw_resp, sizeof(raw_resp), &raw_len, timeout_ms);
    if (err != ESP_OK) return err;
    if (raw_len < 1) return ESP_ERR_INVALID_RESPONSE;

    uint8_t status = raw_resp[0] & 0x3F;
    if (status != 0x00) {
        ESP_LOGW(TAG, "InDataExchange Statusfehler 0x%02X", status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t data_len = raw_len - 1;
    if (data_len > chunk_cap) {
        ESP_LOGW(TAG, "InDataExchange: Fortsetzungsstueck (%d Byte) passt nicht in den "
                       "Puffer (%d Byte) -- breche ab statt still abzuschneiden",
                 (int)data_len, (int)chunk_cap);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(chunk, &raw_resp[1], data_len);
    *chunk_len = data_len;
    *out_more = (raw_resp[0] & 0x40) != 0;
    return ESP_OK;
}

/* Fuehrt ein InDataExchange aus und holt bei Bedarf ueber weitere
 * InDataExchange-Aufrufe (nur Zielnummer-Byte, keine neuen Daten) die
 * Fortsetzung ab, solange Bit 0x40 ("more data folgt") gesetzt bleibt --
 * so werden z.B. grosse HomeKey-ATTESTATION-CBOR-Envelopes, die der PN532
 * nicht in einem einzelnen Kurzframe (~253 Byte Nutzdaten) liefern kann,
 * ueber mehrere Aufrufe im resp-Puffer zusammengesetzt.
 *
 * WICHTIG: dieses Fortsetzungsschema (Anfrage nur mit Zielnummer-Byte, ohne
 * neue APDU-Daten) ist aus einem gaengigen/plausiblen PN532-Verwendungsmuster
 * abgeleitet, nicht gegen das NXP-Datenblatt verifiziert -- muss auf echter
 * Hardware bestaetigt werden (siehe PROTOCOL.md). */
static esp_err_t pn532_data_exchange_once(const uint8_t *apdu, size_t apdu_len,
                                           uint8_t *resp, size_t resp_cap, size_t *resp_len,
                                           uint32_t timeout_ms)
{
    uint8_t params[300];
    if (apdu_len + 1 > sizeof(params)) return ESP_ERR_INVALID_SIZE;
    params[0] = s_last_target_number;
    memcpy(&params[1], apdu, apdu_len);

    size_t total_len = 0;
    bool more = false;
    esp_err_t err = pn532_data_exchange_fetch(params, apdu_len + 1,
                                               resp, resp_cap, &total_len, &more, timeout_ms);
    if (err != ESP_OK) return err;

    int continuation_count = 0;
    while (more) {
        continuation_count++;
        ESP_LOGI(TAG, "InDataExchange: hole Fortsetzung ab (bisher %d Byte, Abholung #%d)...",
                 (int)total_len, continuation_count);

        if (continuation_count > 32) {
            // Schutz gegen eine kaputte/unerwartete Endlosschleife -- eine
            // reale Antwort (auch die groessten HomeKey-Envelopes) braucht
            // dafuer keine 32 Fortsetzungsstuecke.
            ESP_LOGW(TAG, "InDataExchange: zu viele Fortsetzungsstuecke (>%d) -- breche ab",
                     continuation_count);
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint8_t continuation_params[] = {s_last_target_number};
        uint8_t chunk[300];
        size_t chunk_len = 0;
        err = pn532_data_exchange_fetch(continuation_params, sizeof(continuation_params),
                                         chunk, sizeof(chunk), &chunk_len, &more, timeout_ms);
        if (err != ESP_OK) return err;

        if (total_len + chunk_len > resp_cap) {
            ESP_LOGW(TAG, "InDataExchange: zusammengesetzte Antwort (%d Byte) passt nicht in "
                           "den Puffer (%d Byte) -- breche ab statt still abzuschneiden",
                     (int)(total_len + chunk_len), (int)resp_cap);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(&resp[total_len], chunk, chunk_len);
        total_len += chunk_len;
    }

    if (continuation_count > 0) {
        ESP_LOGI(TAG, "InDataExchange: Fortsetzung komplett abgeholt (%d Byte gesamt, %d Abholungen)",
                 (int)total_len, continuation_count);
    }

    *resp_len = total_len;
    return ESP_OK;
}

/* Sucht die Karte per InListPassiveTarget erneut und aktualisiert das Target
 * fuer InDataExchange. Genutzt, um nach einem RF-Kommunikationsfehler (z.B.
 * PN532-Status 0x01 "Timeout, target did not answer") eine noch physisch im
 * Feld befindliche Karte neu zu aktivieren -- deren ISO14443-4-Sitzung kann
 * abgelaufen sein (Frame Waiting Time ueberschritten, z.B. durch die
 * MQTT-Rundlaufzeit bis zum naechsten APDU), ohne dass die Karte das Feld
 * verlassen hat.
 *
 * WICHTIG: sendet vorher den ECP/HomeKey-Broadcast (siehe
 * pn532_send_homekey_broadcast()) -- ein blankes InListPassiveTarget (reines
 * REQA/WUPA) reicht bei einem iPhone/einer Watch mit HomeKey NICHT aus, um es
 * zur erneuten Antwort zu bewegen: Apples HCE-Implementierung reagiert nur
 * auf den proprietaeren ECP-Praefix, nicht auf ein generisches Polling (siehe
 * pn532_poll_once(), das genau deswegen zwischen Polling-Versuchen den
 * Broadcast einstreut). Ohne dieses Broadcast-Priming schlug die
 * Re-Aktivierung bei echten Handshakes mit einem gepairten iPhone-HomeKey
 * ausnahmslos fehl ("Karte hat das Feld vermutlich verlassen"), obwohl das
 * Telefon nachweislich noch aufgelegt war (siehe PROTOCOL.md). Fuer reine
 * Passivkarten (DESFire etc.) ist der zusaetzliche Broadcast unschaedlich --
 * sie ignorieren ihn einfach. */
static esp_err_t pn532_reactivate_target(void)
{
    pn532_send_homekey_broadcast();

    uint8_t params[] = {0x01, 0x00};
    uint8_t resp[300];
    size_t resp_len = 0;

    esp_err_t err = pn532_command(PN532_CMD_IN_LIST_PASSIVE_TARGET, params, sizeof(params),
                                   resp, sizeof(resp), &resp_len, 300);
    if (err != ESP_OK || resp_len < 7 || resp[0] == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    s_last_target_number = resp[1];
    s_target_selected = true;

    // ATS neu auswerten (wie in pn532_poll_once()) -- dieselbe Karte kann bei
    // der Re-Aktivierung ein anderes Timing melden, und ohne das wuerde ein
    // Default-Timeout stehenbleiben, statt das tatsaechlich per FWI
    // gemeldete zu verwenden. NFCIDLength bei Index 5, nicht 6 -- siehe
    // ausfuehrliche Begruendung/Beispielrechnung in pn532_poll_once().
    uint8_t uid_len = resp[5];
    size_t ats_tl_index = (size_t)6 + uid_len;
    if (ats_tl_index < resp_len) {
        uint8_t ats_tl = resp[ats_tl_index];
        if (ats_tl >= 1 && ats_tl_index + ats_tl <= resp_len) {
            pn532_update_response_timeout_from_ats(&resp[ats_tl_index + 1], ats_tl - 1);
        }
    }

    return ESP_OK;
}

/* Wie pn532_data_exchange_fetch(), aber fuer native MIFARE-Classic-
 * Kommandos (Auth 0x60/0x61, Read 0x30, Write 0xA0 -- siehe
 * mifare_classic_module.py) statt ISO7816-APDUs: das PN532-Statusbyte IST
 * hier die eigentliche, von der Karte gelieferte Antwort (z.B. 0x14
 * "MIFARE-Authentifizierungsfehler" bei falschem Key) -- eine normale,
 * gueltige Antwort und KEIN Kommunikationsfehler, anders als bei
 * ISO7816-APDUs (dort lebt das logische Ergebnis in SW1/SW2 INNERHALB der
 * Nutzdaten, und ein Statusbyte != 0 bedeutet tatsaechlich einen RF-/PN532-
 * Fehler). Das Statusbyte wird deshalb unveraendert als letztes Byte an die
 * Nutzdaten angehaengt durchgereicht (Konvention, siehe
 * mifare_classic_module.py: "letztes Antwort-Byte = Status") statt als
 * Fehler wie z.B. ESP_ERR_INVALID_RESPONSE gewertet zu werden. Nur ein
 * echter Kommunikationsfehler (pn532_command() selbst schlaegt fehl, kein
 * gueltiges Antwort-Frame) bleibt ein Fehler. Keine "more data"-Fortsetzung
 * noetig -- native MIFARE-Kommandos passen immer in ein einzelnes
 * Kurzframe (max. 16 Byte Nutzdaten bei ReadBlock). */
static esp_err_t pn532_data_exchange_once_native(const uint8_t *cmd, size_t cmd_len,
                                                  uint8_t *resp, size_t resp_cap, size_t *resp_len,
                                                  uint32_t timeout_ms)
{
    uint8_t params[300];
    if (cmd_len + 1 > sizeof(params)) return ESP_ERR_INVALID_SIZE;
    params[0] = s_last_target_number;
    memcpy(&params[1], cmd, cmd_len);

    uint8_t raw_resp[300];
    size_t raw_len = 0;
    esp_err_t err = pn532_command(PN532_CMD_IN_DATA_EXCHANGE, params, cmd_len + 1,
                                   raw_resp, sizeof(raw_resp), &raw_len, timeout_ms);
    if (err != ESP_OK) return err;
    if (raw_len < 1) return ESP_ERR_INVALID_RESPONSE;

    uint8_t status = raw_resp[0] & 0x3F;
    size_t data_len = raw_len - 1;
    if (data_len + 1 > resp_cap) {
        ESP_LOGW(TAG, "InDataExchange (nativ): Antwort (%d+1 Byte) passt nicht in den Puffer "
                       "(%d Byte) -- breche ab statt still abzuschneiden",
                 (int)data_len, (int)resp_cap);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(resp, &raw_resp[1], data_len);
    resp[data_len] = status;
    *resp_len = data_len + 1;

    if (status != 0x00) {
        ESP_LOGI(TAG, "InDataExchange (nativ): Statusbyte 0x%02X (kein Fehler -- wird "
                       "unveraendert an das Addon durchgereicht)", status);
    }
    return ESP_OK;
}

#define PN532_DATA_EXCHANGE_MAX_REACTIVATIONS 2

esp_err_t pn532_data_exchange(const uint8_t *apdu, size_t apdu_len,
                               uint8_t *resp, size_t resp_cap, size_t *resp_len,
                               uint32_t timeout_ms)
{
    return pn532_data_exchange_ex(apdu, apdu_len, resp, resp_cap, resp_len, timeout_ms, false);
}

esp_err_t pn532_data_exchange_ex(const uint8_t *apdu, size_t apdu_len,
                                  uint8_t *resp, size_t resp_cap, size_t *resp_len,
                                  uint32_t timeout_ms, bool native)
{
    if (!s_target_selected) return ESP_ERR_INVALID_STATE;

    // MIFARE Classic Auth-Kommandos (0x60/0x61) brauchen eine FRISCHE
    // Selektion (HALT/WakeUp+Anticollision+SELECT), sonst kann die Karte nach
    // einem vorherigen fehlgeschlagenen Auth-Versuch (falscher Key, z.B. beim
    // sektorweisen Durchprobieren via NFC-Shell) in einem "verwirrten"
    // Crypto1-Zustand haengenbleiben und einen eigentlich KORREKTEN Key beim
    // naechsten Versuch trotzdem mit Statusbyte 0x14 ablehnen -- auf echter
    // Hardware reproduziert: derselbe Key/Sektor lieferte abwechselnd 0x14
    // und 0x00, je nachdem ob vorher schon ein falscher Key auf einem anderen
    // Sektor probiert wurde. mfoc/libnfc reselektieren aus demselben Grund
    // vor jedem Dictionary-Versuch.
    //
    // WICHTIG: NUR vor Auth (0x60/0x61) reselektieren, NICHT vor Read (0x30)/
    // Write (0xA0)/anderen nativen Kommandos -- ein Read/Write NACH einer
    // erfolgreichen Auth braucht genau das GEGENTEIL, naemlich dass die
    // Selektion (und damit der authentifizierte Zustand) seit der Auth
    // erhalten bleibt. Eine Re-Selektion hier wuerde jeden Auth+Read/Write-
    // Ablauf (z.B. beim Zuruecksetzen eines Sektor-Keys per Write) sofort
    // wieder kaputt machen.
    bool is_native_auth = native && apdu_len >= 1 && (apdu[0] == 0x60 || apdu[0] == 0x61);
    if (is_native_auth) {
        if (pn532_reactivate_target() != ESP_OK) {
            ESP_LOGW(TAG, "Re-Selektion vor nativem MIFARE-Auth fehlgeschlagen -- "
                          "Karte hat das Feld vermutlich verlassen");
            return ESP_ERR_NOT_FOUND;
        }
    }

    esp_err_t err = native
        ? pn532_data_exchange_once_native(apdu, apdu_len, resp, resp_cap, resp_len, timeout_ms)
        : pn532_data_exchange_once(apdu, apdu_len, resp, resp_cap, resp_len, timeout_ms);

    // Erholungsversuche: die Karte kann noch im Feld sein, auch wenn ihre
    // bisherige ISO14443-4-Sitzung nicht mehr antwortet (z.B. FWT
    // ueberschritten). WICHTIG: nur unbedenklich, solange noch kein
    // kryptografischer Auth-Zustand auf der Karte existiert, den eine
    // Re-Aktivierung (RATS) zerstoeren wuerde -- fuer das jeweils ERSTE APDU
    // einer Session (SELECT/AUTH0) ist das der Fall, spaeter im Handshake
    // koennte ein stiller Session-Reset sonst zu verwirrenden Folgefehlern
    // fuehren statt zu einem klaren Abbruch (siehe PROTOCOL.md). Fuer den
    // nativen Pfad greift das nur noch bei ECHTEN Kommunikationsfehlern,
    // nicht mehr bei einem normalen MIFARE-Statusbyte wie 0x14.
    for (int attempt = 1; err != ESP_OK && attempt <= PN532_DATA_EXCHANGE_MAX_REACTIVATIONS; attempt++) {
        ESP_LOGW(TAG, "InDataExchange fehlgeschlagen (%s), Re-Aktivierungsversuch %d/%d...",
                 esp_err_to_name(err), attempt, PN532_DATA_EXCHANGE_MAX_REACTIVATIONS);
        if (pn532_reactivate_target() != ESP_OK) {
            ESP_LOGW(TAG, "Re-Aktivierung fehlgeschlagen -- Karte hat das Feld vermutlich verlassen");
            break;
        }
        err = native
            ? pn532_data_exchange_once_native(apdu, apdu_len, resp, resp_cap, resp_len, timeout_ms)
            : pn532_data_exchange_once(apdu, apdu_len, resp, resp_cap, resp_len, timeout_ms);
    }
    return err;
}

void pn532_release_field(void)
{
    if (!s_target_selected) return;

    uint8_t params[] = {s_last_target_number};
    uint8_t resp[8];
    size_t resp_len = 0;
    pn532_command(PN532_CMD_IN_RELEASE, params, sizeof(params), resp, sizeof(resp), &resp_len, 200);
    s_target_selected = false;
}
