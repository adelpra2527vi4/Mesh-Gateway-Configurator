#pragma once

#include <stdint.h>
#include <stdbool.h>

#define BLE_CLASSIC_MAX_SENSORS 8

// Configura i sensori da cercare (mac in formato "aa:bb:cc:dd:ee:ff" minuscolo,
// rules nello stesso formato testuale di targetRules in C_BLE.ino:
// "SVC,<uuid16hex>,<offset>,<len>,<op>,<label>;...")
void ble_classic_configure_sensor(uint8_t idx, const char *mac, const char *rules);
void ble_classic_init(void);

// Ultimo dato letto per il sensore idx, formato "label=val;...;rssi=N" (vuoto se non ancora visto)
// out deve avere almeno 160 byte.
void ble_classic_get_last(uint8_t idx, char *out, int out_len);

// mac+rules attualmente configurati per idx (vuoto se non configurato). buf >= 24 byte per mac.
void ble_classic_get_config(uint8_t idx, char *mac_out, int mac_len, char *rules_out, int rules_len);

// Etichetta libera per lo slot (es. "Cucina"), indipendente da mac/rules.
void ble_classic_set_name(uint8_t idx, const char *name);
void ble_classic_get_name(uint8_t idx, char *out, int out_len);

// Svuota tutti gli slot configurati (equivalente al "Factory Reset" sensori dell'app BLE).
void ble_classic_reset_all(void);

// Svuota un solo slot (mac/regole/nome/ultimo dato), lasciando intatti gli altri.
void ble_classic_reset_slot(uint8_t idx);

// ============================================================
// Sniffer BLE (per /setup): cattura TUTTI gli advertising nei dintorni, non
// solo i sensori configurati - serve a trovare mac+payload di un sensore
// nuovo prima di scrivere la regola di decodifica (come lo sniffer dell'app
// Web Bluetooth originale). Mentre e' attivo, mesh e scanner "normale" dei
// sensori configurati sono IN PAUSA (un solo radio, va condiviso a turni):
// vedi mesh_pause_for_ble_scan()/mesh_resume_after_ble_scan() in main.c.
// ============================================================
void ble_classic_sniffer_start(void);
void ble_classic_sniffer_stop(void);
bool ble_classic_sniffer_is_active(void);

// JSON array dei dispositivi visti dallo sniffer: [{"mac":"..","name":"..",
// "rssi":N,"svc_uuid":"2A6E","svc_hex":"...","mfr_hex":"..."}, ...]
// Scrive in buf (troncando se serve), ritorna la lunghezza scritta.
int ble_classic_sniffer_get_json(char *buf, int buf_len);

// Accessor testuali equivalenti (per il dispatcher USB CDC "CFG:" in main.c,
// che non ha un parser JSON): numero di dispositivi visti e dati del
// dispositivo i-esimo, stessi campi di ble_classic_sniffer_get_json ma senza
// dover ricostruire/parsare JSON. mac_out >= 13 byte (hex, senza ':'),
// name_out >= 32, svc_hex_out/mfr_hex_out >= 42 (vedi SNIFFER_HEX_CAP).
int  ble_classic_sniffer_count(void);
bool ble_classic_sniffer_get_dev(int i, char *mac_out, char *name_out, int *rssi,
                                  uint16_t *svc_uuid, char *svc_hex_out, char *mfr_hex_out);
