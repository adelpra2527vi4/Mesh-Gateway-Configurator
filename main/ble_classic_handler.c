// Scanner BLE classico (beacon temp/hum), in TIME-SHARING col BLE Mesh sullo
// stesso radio (Bluedroid). A differenza del relay-controller originale
// (C_BLE.ino, bleTask) che scansiona quasi sempre - li' non c'era nessun
// mesh a contendersi il radio - qui il provisioner BLE Mesh deve restare
// attivo quasi sempre (disabilitarlo rompe la ricezione di QUALSIASI
// messaggio mesh, non solo il discovery di nodi nuovi: provato via test).
//
// Quindi: ogni SCAN_PERIOD_MS si apre una finestra breve (SCAN_WINDOW_MS) in
// cui si chiede a main.c di metter in pausa il provisioner (e i GET di
// poll_cb) tramite mesh_pause_for_ble_scan(), si scansiona, e alla fine si
// richiama mesh_resume_after_ble_scan() per restituire il radio al mesh.
//
// Stessa logica di parsing di C_BLE.ino (parseBeaconPayload) ma porting in
// C puro.
#include "ble_classic_handler.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "BLE_CLASSIC"

// Storico: 4s/10s (40%) -> 3s/10s (30%) -> 2s/10s (20%) -> 1.5s/20s (7.5%):
// finestra ridotta e periodo raddoppiato per dare ancora piu' priorita' al
// BLE Mesh sull'unica antenna condivisa. I beacon temp/hum vengono comunque
// catturati (advertise ogni 1-5s, finestra da 1.5s al 98.75% duty cycle),
// ma il mesh non si ferma piu' per 2s ogni 10s.
#define SCAN_WINDOW_MS   1500   // durata di ogni finestra di scan (mesh in pausa per questo tempo)
#define SCAN_PERIOD_MS   20000  // intervallo tra una finestra e la successiva

// Definite in main.c: mettono in pausa/riprendono il provisioner mesh
// (e i GET periodici di poll_cb) per la durata della nostra finestra di scan.
extern void mesh_pause_for_ble_scan(void);
extern void mesh_resume_after_ble_scan(void);
// true mentre e' in corso un provisioning/bind (vedi config_busy/pair_active
// in main.c): in quella finestra il radio BT serve solo al provisioning,
// niente finestre di scan classico automatiche (richiesto: durante il
// provisioning da WiFi non deve fare altro, ne' mesh ne' beacon BLE).
extern bool gateway_is_provisioning(void);

typedef struct {
    bool configured;
    uint8_t mac[6];
    char mac_str[24];
    char rules[160];
    char last[160];
    char name[24]; // etichetta libera (es. "Cucina"), non in NVS blob: persistita come stringa separata da main.c
} ble_sensor_t;

static ble_sensor_t s_sensors[BLE_CLASSIC_MAX_SENSORS];
static SemaphoreHandle_t s_mutex;

static bool parse_mac(const char *mac, uint8_t out[6]) {
    if (!mac || strlen(mac) < 17) return false;
    unsigned v[6];
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

void ble_classic_configure_sensor(uint8_t idx, const char *mac, const char *rules) {
    if (idx >= BLE_CLASSIC_MAX_SENSORS) return;
    if (!parse_mac(mac, s_sensors[idx].mac)) return;
    strncpy(s_sensors[idx].mac_str, mac, sizeof(s_sensors[idx].mac_str) - 1);
    strncpy(s_sensors[idx].rules, rules, sizeof(s_sensors[idx].rules) - 1);
    s_sensors[idx].configured = true;
}

void ble_classic_set_name(uint8_t idx, const char *name) {
    if (idx >= BLE_CLASSIC_MAX_SENSORS) return;
    strncpy(s_sensors[idx].name, name, sizeof(s_sensors[idx].name) - 1);
    s_sensors[idx].name[sizeof(s_sensors[idx].name) - 1] = '\0';
}

void ble_classic_get_name(uint8_t idx, char *out, int out_len) {
    out[0] = '\0';
    if (idx >= BLE_CLASSIC_MAX_SENSORS) return;
    strncpy(out, s_sensors[idx].name, out_len - 1);
    out[out_len - 1] = '\0';
}

void ble_classic_reset_all(void) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memset(s_sensors, 0, sizeof(s_sensors));
        xSemaphoreGive(s_mutex);
    }
}

void ble_classic_reset_slot(uint8_t idx) {
    if (idx >= BLE_CLASSIC_MAX_SENSORS) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memset(&s_sensors[idx], 0, sizeof(s_sensors[idx]));
        xSemaphoreGive(s_mutex);
    }
}

void ble_classic_get_config(uint8_t idx, char *mac_out, int mac_len, char *rules_out, int rules_len) {
    mac_out[0] = '\0';
    rules_out[0] = '\0';
    if (idx >= BLE_CLASSIC_MAX_SENSORS || !s_sensors[idx].configured) return;
    strncpy(mac_out, s_sensors[idx].mac_str, mac_len - 1);
    mac_out[mac_len - 1] = '\0';
    strncpy(rules_out, s_sensors[idx].rules, rules_len - 1);
    rules_out[rules_len - 1] = '\0';
}

void ble_classic_get_last(uint8_t idx, char *out, int out_len) {
    out[0] = '\0';
    if (idx >= BLE_CLASSIC_MAX_SENSORS) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(out, s_sensors[idx].last, out_len - 1);
        out[out_len - 1] = '\0';
        xSemaphoreGive(s_mutex);
    }
}

// esp_ble_resolve_adv_data() (Bluedroid) restituisce SOLO il primo blocco di
// un dato tipo AD nel pacchetto: se il beacon manda temperatura e umidita'
// come due blocchi "Service Data 16-bit UUID" distinti nello stesso
// advertisement, quella funzione vede sempre e solo il primo, mai il
// secondo - una delle due regole non troverebbe mai il suo UUID. Qui invece
// scandiamo a mano tutti i blocchi AD del pacchetto cercando quello con
// l'UUID richiesto da QUESTA regola.
#define AD_TYPE_SERVICE_DATA_16 0x16

static bool find_service_data_by_uuid(const uint8_t *adv, int adv_len, uint16_t want_uuid,
                                       const uint8_t **out_payload, int *out_len) {
    int i = 0;
    while (i < adv_len) {
        uint8_t field_len = adv[i];
        if (field_len == 0) break;
        if (i + 1 + field_len > adv_len) break;
        uint8_t ad_type = adv[i + 1];
        if (ad_type == AD_TYPE_SERVICE_DATA_16 && field_len >= 3) {
            uint16_t uuid = adv[i + 2] | ((uint16_t)adv[i + 3] << 8);
            if (uuid == want_uuid) {
                *out_payload = &adv[i + 4];
                *out_len = field_len - 3; // - type byte - 2 byte uuid
                return true;
            }
        }
        i += 1 + field_len;
    }
    return false;
}

// Applica una singola regola "SOURCE,target,offset,len,op,label" al pacchetto
// advertising completo e appende "label=val;" a out.
static void apply_rule(const char *rule, const uint8_t *adv_data, int adv_len,
                        const uint8_t *mfr_data, int mfr_len,
                        char *out, int out_cap) {
    char source[8] = {0}, target[8] = {0}, op[16] = {0}, label[32] = {0};
    int offset = 0, len = 0;
    // formato: SOURCE,target,offset,len,op,label
    char buf[160];
    strncpy(buf, rule, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // strtok_r (non strtok!): handle_adv sta a sua volta iterando le regole
    // con un altro strtok sulla stessa stringa ";" - strtok ha un solo stato
    // globale, quindi una chiamata annidata romperebbe quell'iterazione
    // esterna (e infatti e' quello che succedeva: solo la prima regola di
    // ogni slot veniva mai applicata).
    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    if (!tok) return;
    strncpy(source, tok, sizeof(source) - 1);
    tok = strtok_r(NULL, ",", &save);
    if (!tok) return;
    strncpy(target, tok, sizeof(target) - 1);
    tok = strtok_r(NULL, ",", &save);
    if (!tok) return;
    offset = atoi(tok);
    tok = strtok_r(NULL, ",", &save);
    if (!tok) return;
    len = atoi(tok);
    tok = strtok_r(NULL, ",", &save);
    if (!tok) return;
    strncpy(op, tok, sizeof(op) - 1);
    tok = strtok_r(NULL, ",", &save);
    if (!tok) return;
    strncpy(label, tok, sizeof(label) - 1);

    // ID=0 (non documentato/sconosciuto) = elabora il pacchetto intero invece
    // di filtrare per UUID/company ID - stesso comportamento del manuale Hub
    // ("In caso di ID non documentato, l'inserimento del valore 0 consente
    // l'elaborazione dell'intero pacchetto da parte dell'Hub").
    bool target_is_zero = (strtol(target, NULL, 16) == 0);

    const uint8_t *payload = NULL;
    int payload_len = 0;
    if (strcmp(source, "MFR") == 0 && mfr_data && mfr_len > 0) {
        if (target_is_zero) {
            payload = mfr_data; payload_len = mfr_len;
        } else {
            // I primi 2 byte dei Manufacturer Data sono il Company ID
            // (little-endian, assegnato dal Bluetooth SIG): se l'ID richiesto
            // non corrisponde questo pacchetto non e' del produttore atteso.
            uint16_t want_company = (uint16_t)strtol(target, NULL, 16);
            if (mfr_len >= 2) {
                uint16_t company = mfr_data[0] | ((uint16_t)mfr_data[1] << 8);
                if (company == want_company) {
                    payload = mfr_data + 2; payload_len = mfr_len - 2;
                }
            }
        }
    } else if (strcmp(source, "SVC") == 0) {
        if (target_is_zero) {
            payload = adv_data; payload_len = adv_len;
        } else {
            uint16_t want = (uint16_t)strtol(target, NULL, 16);
            find_service_data_by_uuid(adv_data, adv_len, want, &payload, &payload_len);
        }
    }
    if (!payload || payload_len < offset + len || len <= 0 || len > 4) return;

    char *opp = op;
    bool big_endian = false, is_signed = false;
    if (*opp == 'B') { big_endian = true; opp++; }
    if (*opp == 'S') { is_signed = true; opp++; }

    uint32_t raw = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = payload[offset + i];
        if (big_endian) raw = (raw << 8) | b;
        else raw = raw | ((uint32_t)b << (i * 8));
    }

    float val;
    if (is_signed) {
        int32_t sv = (int32_t)raw;
        if (len == 1 && (raw & 0x80)) sv |= 0xFFFFFF00;
        else if (len == 2 && (raw & 0x8000)) sv |= 0xFFFF0000;
        else if (len == 3 && (raw & 0x800000)) sv |= 0xFF000000;
        val = (float)sv;
    } else {
        val = (float)raw;
    }

    if (opp[0] == '&') {
        val = (float)((long)val & strtol(opp + 1, NULL, 10));
    } else if (opp[0] == '>' && opp[1] == '>') {
        val = (float)((long)val >> atoi(opp + 2));
    } else if (opp[0] == '%') {
        int mod = atoi(opp + 1);
        if (mod != 0) val = (float)((long)val % mod);
    } else {
        float div = atof(opp);
        if (div != 0) val = val / div;
    }

    int n = strlen(out);
    snprintf(out + n, out_cap - n, "%s=%.2f;", label, val);
}

// Unisce "new_part" (campi appena letti, es. "temp=24.16;rssi=-93") nello
// stato precedente "old" (es. "hum=55.00;rssi=-91"), tenendo le chiavi di
// "old" che "new_part" non contiene. Serve perche' alcuni beacon alternano i
// campi su pacchetti advertising diversi (un pacchetto ha solo temp, il
// successivo solo hum): senza merge l'ultimo pacchetto arrivato cancellava
// sempre l'altro valore.
static void merge_fields(const char *old, const char *new_part, char *out, int out_cap) {
    char new_keys[8][32]; int new_key_count = 0;
    out[0] = '\0';

    char buf[160];
    strncpy(buf, new_part, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, ";");
    while (tok) {
        int n = strlen(out);
        snprintf(out + n, out_cap - n, "%s;", tok);
        char *eq = strchr(tok, '=');
        if (eq && new_key_count < 8) {
            int klen = (int)(eq - tok); if (klen > 31) klen = 31;
            memcpy(new_keys[new_key_count], tok, klen);
            new_keys[new_key_count][klen] = '\0';
            new_key_count++;
        }
        tok = strtok(NULL, ";");
    }

    strncpy(buf, old, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    tok = strtok(buf, ";");
    while (tok) {
        char *eq = strchr(tok, '=');
        bool already = false;
        if (eq) {
            int klen = (int)(eq - tok); if (klen > 31) klen = 31;
            for (int i = 0; i < new_key_count; i++) {
                if ((int)strlen(new_keys[i]) == klen && strncmp(new_keys[i], tok, klen) == 0) { already = true; break; }
            }
        }
        if (!already) {
            int n = strlen(out);
            snprintf(out + n, out_cap - n, "%s;", tok);
        }
        tok = strtok(NULL, ";");
    }

    int len = strlen(out);
    if (len > 0 && out[len - 1] == ';') out[len - 1] = '\0';
}

static void handle_adv(const uint8_t *bda, const uint8_t *adv_data, uint8_t adv_len, int rssi) {
    for (int i = 0; i < BLE_CLASSIC_MAX_SENSORS; i++) {
        if (!s_sensors[i].configured) continue;
        if (memcmp(bda, s_sensors[i].mac, 6) != 0) continue;

        uint8_t mfr_len = 0;
        uint8_t *mfr_data = esp_ble_resolve_adv_data((uint8_t *)adv_data, ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &mfr_len);

        char result[160] = {0};
        char rules_copy[160];
        strncpy(rules_copy, s_sensors[i].rules, sizeof(rules_copy) - 1);
        rules_copy[sizeof(rules_copy) - 1] = '\0';

        char *save_rule = NULL;
        char *rule = strtok_r(rules_copy, ";", &save_rule);
        while (rule) {
            apply_rule(rule, adv_data, adv_len, mfr_data, mfr_len, result, sizeof(result));
            rule = strtok_r(NULL, ";", &save_rule);
        }

        if (result[0] != '\0') {
            int n = strlen(result);
            snprintf(result + n, sizeof(result) - n, "rssi=%d", rssi);
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                char merged[160];
                merge_fields(s_sensors[i].last, result, merged, sizeof(merged));
                strncpy(s_sensors[i].last, merged, sizeof(s_sensors[i].last) - 1);
                xSemaphoreGive(s_mutex);
            }
            ESP_LOGI(TAG, "[%d] %s", i, s_sensors[i].last);
        }
        return;
    }
}

static esp_timer_handle_t s_scan_timer;
static esp_timer_handle_t s_safety_timer; // failsafe: forza il resume se la finestra si incanta
static esp_timer_handle_t s_window_timer; // chiude esplicitamente la finestra periodica dopo SCAN_WINDOW_MS
static bool               s_scanning     = false;
static bool               s_mesh_paused  = false; // true solo mentre SIAMO NOI ad aver messo in pausa il mesh

// ============================================================
// Sniffer BLE per /setup: scan continuo (non a finestre) di TUTTI gli
// advertising nei dintorni, per trovare mac+payload di un sensore nuovo.
// Mentre e' attivo, mesh e scan periodico dei sensori configurati restano
// IN PAUSA fino a ble_classic_sniffer_stop() - un solo radio, va condiviso
// a turni, e qui l'utente lo sta usando attivamente per configurare.
// ============================================================
#define SNIFFER_MAX_DEVICES 20
#define SNIFFER_HEX_CAP      42 // ~20 byte in hex, capienti per la maggior parte dei payload reali

typedef struct {
    uint8_t mac[6];
    char    name[32];
    int8_t  rssi;
    uint16_t svc_uuid;
    char    svc_hex[SNIFFER_HEX_CAP];
    char    mfr_hex[SNIFFER_HEX_CAP];
    int64_t last_seen_us;
} sniffer_dev_t;

static sniffer_dev_t s_sniffer_devs[SNIFFER_MAX_DEVICES];
static int           s_sniffer_count  = 0;
static bool          s_sniffer_active = false;

static void bytes_to_hex(const uint8_t *data, int len, char *out, int out_cap) {
    int n = 0;
    for (int i = 0; i < len && n + 2 < out_cap - 1; i++) n += snprintf(out + n, out_cap - n, "%02X", data[i]);
    out[n] = '\0';
}

// Come find_service_data_by_uuid ma generico per qualsiasi tipo di campo AD
// (qui serve per il nome, 0x09/0x08, e per i Manufacturer Data, 0xFF).
static bool find_ad_type(const uint8_t *adv, int adv_len, uint8_t type,
                          const uint8_t **out_payload, int *out_len) {
    int i = 0;
    while (i < adv_len) {
        uint8_t field_len = adv[i];
        if (field_len == 0) break;
        if (i + 1 + field_len > adv_len) break;
        if (adv[i + 1] == type) {
            *out_payload = &adv[i + 2];
            *out_len = field_len - 1;
            return true;
        }
        i += 1 + field_len;
    }
    return false;
}

static void sniffer_handle_adv(const uint8_t *bda, const uint8_t *adv, int adv_len, int rssi) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return; // occupato: salta questo pacchetto, arriva il prossimo

    int idx = -1;
    for (int i = 0; i < s_sniffer_count; i++) {
        if (memcmp(s_sniffer_devs[i].mac, bda, 6) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (s_sniffer_count < SNIFFER_MAX_DEVICES) {
            idx = s_sniffer_count++;
        } else {
            // Lista piena: ricicla il dispositivo non visto da piu' tempo.
            int64_t oldest_t = s_sniffer_devs[0].last_seen_us;
            idx = 0;
            for (int i = 1; i < SNIFFER_MAX_DEVICES; i++) {
                if (s_sniffer_devs[i].last_seen_us < oldest_t) { oldest_t = s_sniffer_devs[i].last_seen_us; idx = i; }
            }
        }
        memset(&s_sniffer_devs[idx], 0, sizeof(s_sniffer_devs[idx]));
        memcpy(s_sniffer_devs[idx].mac, bda, 6);
    }

    sniffer_dev_t *d = &s_sniffer_devs[idx];
    d->rssi = (int8_t)rssi;
    d->last_seen_us = esp_timer_get_time();

    const uint8_t *p; int plen;
    if (find_ad_type(adv, adv_len, 0x09, &p, &plen) || find_ad_type(adv, adv_len, 0x08, &p, &plen)) {
        int cl = plen < (int)sizeof(d->name) - 1 ? plen : (int)sizeof(d->name) - 1;
        memcpy(d->name, p, cl);
        d->name[cl] = '\0';
        // Sanitizza per JSON: niente '"' '\' o caratteri di controllo nel nome.
        for (int i = 0; i < cl; i++) {
            if (d->name[i] == '"' || d->name[i] == '\\' || (unsigned char)d->name[i] < 32) d->name[i] = '_';
        }
    }
    if (find_ad_type(adv, adv_len, AD_TYPE_SERVICE_DATA_16, &p, &plen) && plen >= 2) {
        d->svc_uuid = p[0] | (p[1] << 8);
        bytes_to_hex(p + 2, plen - 2, d->svc_hex, sizeof(d->svc_hex));
    }
    if (find_ad_type(adv, adv_len, ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &p, &plen)) {
        bytes_to_hex(p, plen, d->mfr_hex, sizeof(d->mfr_hex));
    }

    xSemaphoreGive(s_mutex);
}

void ble_classic_sniffer_start(void) {
    if (s_sniffer_active) return;
    s_sniffer_active = true;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_sniffer_count = 0;
        xSemaphoreGive(s_mutex);
    }
    esp_timer_stop(s_scan_timer); // niente piu' finestre brevi periodiche finche' lo sniffer e' attivo
    if (s_scanning) {
        // Una finestra "normale" e' in corso: la fermiamo, e quando arriva
        // ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT (vedi gap_cb) partira' da sola
        // la scansione continua dello sniffer, perche' s_sniffer_active e'
        // gia' true a quel punto.
        esp_ble_gap_stop_scanning();
        return;
    }
    if (!s_mesh_paused) {
        s_mesh_paused = true;
        mesh_pause_for_ble_scan();
    }
    static esp_ble_scan_params_t sniff_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE, // attivo: chiede anche lo scan response (nome dispositivo)
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        // Come l'originale (setInterval(100)/setWindow(99) in ms, libreria
        // Arduino): quasi il 100% del tempo di ogni ciclo passato in ascolto
        // effettivo, non il 60% di prima (0x50/0x30) - un beacon che cade nel
        // 40% "cieco" per sfortuna di fase puo' restare invisibile per minuti
        // anche scansionando di continuo. Unita': 0.625ms/step.
        // 0xA0=160 step=100ms, 0x9E=158 step=98.75ms.
        .scan_interval      = 0xA0,
        .scan_window        = 0x9E,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    esp_ble_gap_set_scan_params(&sniff_params);
}

void ble_classic_sniffer_stop(void) {
    if (!s_sniffer_active) return;
    s_sniffer_active = false;
    esp_ble_gap_stop_scanning(); // ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT fara' il mesh_resume_after_ble_scan()
    esp_timer_start_periodic(s_scan_timer, (int64_t)SCAN_PERIOD_MS * 1000); // riprende le finestre brevi normali
}

bool ble_classic_sniffer_is_active(void) { return s_sniffer_active; }

int ble_classic_sniffer_get_json(char *buf, int buf_len) {
    int n = snprintf(buf, buf_len, "[");
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < s_sniffer_count && n < buf_len - 2; i++) {
            sniffer_dev_t *d = &s_sniffer_devs[i];
            n += snprintf(buf + n, buf_len - n,
                "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"name\":\"%s\",\"rssi\":%d,"
                "\"svc_uuid\":\"%04X\",\"svc_hex\":\"%s\",\"mfr_hex\":\"%s\"}",
                i ? "," : "", d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
                d->name, d->rssi, d->svc_uuid, d->svc_hex, d->mfr_hex);
        }
        xSemaphoreGive(s_mutex);
    }
    n += snprintf(buf + n, buf_len - n, "]");
    return n;
}

int ble_classic_sniffer_count(void)
{
    return s_sniffer_count;
}

bool ble_classic_sniffer_get_dev(int i, char *mac_out, char *name_out, int *rssi,
                                  uint16_t *svc_uuid, char *svc_hex_out, char *mfr_hex_out)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool ok = (i >= 0 && i < s_sniffer_count);
    if (ok) {
        sniffer_dev_t *d = &s_sniffer_devs[i];
        snprintf(mac_out, 13, "%02X%02X%02X%02X%02X%02X",
                 d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
        strncpy(name_out, d->name, 31); name_out[31] = '\0';
        *rssi = d->rssi;
        *svc_uuid = d->svc_uuid;
        strncpy(svc_hex_out, d->svc_hex, SNIFFER_HEX_CAP - 1); svc_hex_out[SNIFFER_HEX_CAP - 1] = '\0';
        strncpy(mfr_hex_out, d->mfr_hex, SNIFFER_HEX_CAP - 1); mfr_hex_out[SNIFFER_HEX_CAP - 1] = '\0';
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

// Se entro questo tempo dalla richiesta di pausa lo scan non e' partito (o non
// si e' mai fermato), il mesh resta disabilitato per sempre - mai successo per
// arresto/avvio dello scan stesso, ma solo per radio occupato in quel preciso
// istante. Il watchdog forza il resume comunque, cosi' il mesh non resta morto.
// (Si applica solo alle finestre brevi periodiche, non allo sniffer: mentre
// l'utente lo sta usando attivamente non vogliamo un timeout che lo spegne.)
#define SAFETY_TIMEOUT_MS (SCAN_WINDOW_MS + 2000)

static void safety_timer_cb(void *arg) {
    if (s_mesh_paused && !s_sniffer_active) {
        ESP_LOGW(TAG, "Finestra di scan incagliata, forzo la ripresa del mesh.");
        mesh_resume_after_ble_scan();
        s_mesh_paused = false;
        s_scanning    = false;
    }
}

// Chiude esplicitamente la finestra periodica normale dopo SCAN_WINDOW_MS:
// uno stop esplicito genera in modo affidabile ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,
// a differenza dello stop "automatico" per scadenza durata (vedi PARAM_SET_COMPLETE_EVT).
static void window_timer_cb(void *arg) {
    if (s_scanning && !s_sniffer_active) {
        esp_ble_gap_stop_scanning();
    }
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        // Sempre scan continuo (durata 0): la durata-in-secondi passata qui al
        // controller NON genera in modo affidabile ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT
        // quando lo stop e' "automatico" per scadenza - su questo stack capitava
        // SEMPRE (ogni finestra finiva nel watchdog di sicurezza a 3.5s invece
        // che chiudersi a 1.5s). Lo stop ora e' sempre esplicito (vedi
        // window_timer_cb/ble_classic_sniffer_stop), che invece genera l'evento
        // in modo affidabile.
        esp_ble_gap_start_scanning(0);
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = param;
        if (scan_result->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            if (s_sniffer_active) {
                sniffer_handle_adv(scan_result->scan_rst.bda, scan_result->scan_rst.ble_adv,
                                   scan_result->scan_rst.adv_data_len, scan_result->scan_rst.rssi);
            } else {
                handle_adv(scan_result->scan_rst.bda, scan_result->scan_rst.ble_adv,
                           scan_result->scan_rst.adv_data_len, scan_result->scan_rst.rssi);
            }
        }
        break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        s_scanning = true;
        if (!s_sniffer_active) {
            // Finestra periodica normale: la chiudiamo NOI esplicitamente dopo
            // SCAN_WINDOW_MS (vedi window_timer_cb) invece di affidarci alla
            // durata passata a start_scanning - vedi commento su PARAM_SET_COMPLETE_EVT.
            esp_timer_start_once(s_window_timer, (int64_t)SCAN_WINDOW_MS * 1000);
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scanning = false;
        esp_timer_stop(s_safety_timer);
        esp_timer_stop(s_window_timer);
        if (s_sniffer_active) {
            // Era una finestra "normale" interrotta per far partire lo
            // sniffer (vedi ble_classic_sniffer_start): il mesh resta in
            // pausa, si passa subito alla scansione continua.
            static esp_ble_scan_params_t sniff_params = {
                .scan_type          = BLE_SCAN_TYPE_ACTIVE,
                .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
                .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
                .scan_interval      = 0xA0, // 100ms, vedi commento sopra in ble_classic_sniffer_start
                .scan_window        = 0x9E, // 98.75ms (quasi continuo)
                .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
            };
            esp_ble_gap_set_scan_params(&sniff_params);
        } else if (s_mesh_paused) {
            // Fine della finestra (il controller si auto-ferma dopo la durata
            // passata a start_scanning, oppure stop esplicito da sniffer_stop):
            // restituisce il radio al mesh.
            mesh_resume_after_ble_scan();
            s_mesh_paused = false;
        }
        break;
    default:
        break;
    }
}

static void scan_timer_cb(void *arg) {
    if (s_sniffer_active || s_scanning || s_mesh_paused) return; // sniffer attivo o finestra precedente non finita, salta
    if (gateway_is_provisioning()) return; // provisioning in corso: il radio serve solo a lui
    static esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        // Come l'originale (setInterval(100)/setWindow(99) in ms, libreria
        // Arduino): quasi il 100% del tempo di ogni ciclo passato in ascolto
        // effettivo, non il 60% di prima (0x50/0x30) - un beacon che cade nel
        // 40% "cieco" per sfortuna di fase puo' restare invisibile per minuti
        // anche scansionando di continuo. Unita': 0.625ms/step.
        // 0xA0=160 step=100ms, 0x9E=158 step=98.75ms.
        .scan_interval      = 0xA0,
        .scan_window        = 0x9E,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    s_mesh_paused = true;
    mesh_pause_for_ble_scan();
    esp_ble_gap_set_scan_params(&scan_params); // l'avvio scan avviene nella callback PARAM_SET_COMPLETE
    esp_timer_start_once(s_safety_timer, (int64_t)SAFETY_TIMEOUT_MS * 1000);
}

void ble_classic_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    esp_ble_gap_register_callback(gap_cb);

    esp_timer_create_args_t args = {
        .callback = scan_timer_cb,
        .name = "ble_classic_scan",
    };
    esp_timer_create(&args, &s_scan_timer);
    // Era stato disattivato in via diagnostica per escludere che fosse lui a
    // impedire al WiFi SoftAP di trasmettere beacon. La causa reale era
    // invece l'overflow in build_mesh_addr_list (main.c), gia' risolto: senza
    // questa riga lo scan periodico classico non partiva MAI in modalita'
    // normale (solo lo sniffer manuale funzionava) - rimesso.
    esp_timer_start_periodic(s_scan_timer, (int64_t)SCAN_PERIOD_MS * 1000);

    esp_timer_create_args_t safety_args = {
        .callback = safety_timer_cb,
        .name = "ble_classic_safety",
    };
    esp_timer_create(&safety_args, &s_safety_timer);

    esp_timer_create_args_t window_args = {
        .callback = window_timer_cb,
        .name = "ble_classic_window",
    };
    esp_timer_create(&window_args, &s_window_timer);
}
