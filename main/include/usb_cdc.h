#pragma once

// Canale USB CDC (TinyUSB) verso il PC: sostituisce il vecchio WiFi SoftAP +
// server HTTP come trasporto per la configurazione (vedi spec PWA - il
// gateway diventa BLE-puro, la UI vive in una PWA esterna che parla con
// questo canale via Web Serial). Protocollo testuale riga-per-riga, stesso
// stile del bridge UART verso il relay-controller (vedi BRIDGE_UART_NUM in
// main.c): righe terminate da '\n', comandi "SET;"/"GET;"/"DUMP" compatibili
// col vecchio bridge, comandi "CFG:" nuovi solo per questo canale.
//
// Implementato come modulo a parte (come ble_classic_handler.c) che chiama
// indietro in main.c via una funzione extern, cosi' tutta la logica/stato
// (nodes[], config_busy, ecc.) resta dov'e' senza doverla esportare qui.
void usb_cdc_init(void);

// Manda una riga (senza '\n') sul canale USB CDC - BLOCCANTE.
// Usarla solo da task che possono bloccare (usb_cmd_task): risposte CFG:,
// DBG; ecc. NON chiamarla da callback BLE Mesh o timer (usa push_line).
void usb_cdc_send_line(const char *line);

// Versione NON-BLOCCANTE per push spontanei (ONOFF;/LEVEL;/SENSOR;).
// Accoda il messaggio in una coda FreeRTOS e ritorna subito; un task
// dedicato (usb_push_task) drena la coda verso USB senza bloccare il
// chiamante. Se la coda è piena (USB disconnessa da troppo tempo) la
// riga viene scartata silenziosamente.
void usb_cdc_push_line(const char *line);

bool usb_cdc_is_connected(void);
