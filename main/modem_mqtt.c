// Driver AT per il modem cellulare Air780E (set di comandi "Luat"), porting in
// C puro della logica di B_ModemMQTT.ino (ESP32-terminal-controller-main):
// stessa sequenza ATE0 -> CGREG? -> MCONFIG -> MQTTMSGSET -> MIPSTART ->
// MCONNECT -> MSUB, stessa gestione errore "767" (sessione MQTT caduta, TCP
// ancora valido: si riconnette solo MQTT, non tutto il modem).
//
// UART dedicata: GPIO15=TX, GPIO16=RX, 115200 8N1 (vedi client_definitivo_relay.ino,
// EXT_SERIAL_TX/EXT_SERIAL_RX - quella verso il modem, diversa dalla UART del
// vecchio bridge ONOFF/SET/GET che usava i GPIO 4/5).
#include "modem_mqtt.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "MODEM_MQTT"

#define MODEM_UART_NUM    UART_NUM_2
#define MODEM_TX_PIN      15
#define MODEM_RX_PIN      16
#define MODEM_BAUD        115200
#define MODEM_RX_BUF_SIZE 2048
#define MODEM_LINE_MAX          256

#define PUB_QUEUE_LEN     32
#define PUB_THROTTLE_MS   60   // pausa tra un AT+MPUB e il successivo (vedi MPUB_THROTTLE_MS originale)
#define MQTT_KEEPALIVE_S  300  // 5 minuti, come l'originale

// 200 bastava per i messaggi pipe-delimited storici ma troncava in silenzio
// il payload base64(JSON) di "modem/lampade" (vedi base64_encode in main.c:
// con MAX_NODES=10 lampade e nomi al limite, caso peggiore teorico ~1584
// byte) - portato a 1600 per coprirlo con margine.
typedef struct {
    char topic[32];
    char payload[1600];
} pub_item_t;

static char     s_client_id[32];
static char     s_broker_host[64];
static uint16_t s_broker_port;
static char     s_sub_topic[32];

static bool                     s_connected = false;
static modem_mqtt_rx_cb_t       s_rx_cb     = NULL;
static modem_mqtt_recovery_cb_t s_recovery_cb = NULL;
static modem_mqtt_log_cb_t      s_log_cb    = NULL;
static int                 s_consecutive_767 = 0;
#define MAX_CONSECUTIVE_767 5

// Timestamp (us) di quando la sessione e' caduta, 0 = nessun buco in corso.
// Impostato solo alla transizione true->false (non a ogni retry falito di
// modem_handshake), cosi' la durata riportata e' quella reale dell'interruzione.
static int64_t s_disconnect_started_us = 0;

static void mark_disconnected(void)
{
    if (s_connected) {
        s_disconnect_started_us = esp_timer_get_time();
    }
    s_connected = false;
}

static pub_item_t        s_pub_queue[PUB_QUEUE_LEN];
static int                s_pub_count = 0;
static SemaphoreHandle_t  s_pub_mutex;

static void uart_send_at(const char *cmd)
{
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
    ESP_LOGI(TAG, "[TX] %s", cmd);
}

static void flush_rx(void)
{
    uint8_t tmp[64];
    while (uart_read_bytes(MODEM_UART_NUM, tmp, sizeof(tmp), 0) > 0) {}
}

// Legge righe (terminate da '\n') per al massimo timeout_ms, cercando una
// corrispondenza con una delle stringhe in candidates. Gestisce anche le
// risposte "OK" mozzate (isOkResponse nell'originale: a volte il modem
// restituisce solo "K" o "O" per limiti di buffer).
static bool wait_for_any(const char **candidates, int count, int timeout_ms, int *which)
{
    int64_t start = esp_timer_get_time() / 1000;
    char buf[MODEM_LINE_MAX]; int len = 0;
    while ((esp_timer_get_time() / 1000) - start < timeout_ms) {
        uint8_t c;
        int n = uart_read_bytes(MODEM_UART_NUM, &c, 1, pdMS_TO_TICKS(20));
        if (n <= 0) continue;
        if (c == '\n') {
            buf[len] = '\0';
            char *p = buf;
            while (*p == '\r' || *p == ' ') p++;
            if (*p) ESP_LOGI(TAG, "[RX] '%s'", p);
            for (int i = 0; i < count; i++) {
                if (strstr(p, candidates[i])) { if (which) *which = i; return true; }
                if (strcmp(candidates[i], "OK") == 0 && strlen(p) == 2 &&
                    (p[1] == 'K' || p[0] == 'O')) { if (which) *which = i; return true; }
            }
            len = 0;
        } else if (len < (int)sizeof(buf) - 1) {
            buf[len++] = (char)c;
        }
    }
    return false;
}

static bool wait_for(const char *expected, int timeout_ms)
{
    const char *c[1] = { expected };
    return wait_for_any(c, 1, timeout_ms, NULL);
}

static bool wait_network_registered(int timeout_ms)
{
    int64_t start = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000) - start < timeout_ms) {
        uart_send_at("AT+CGREG?");
        char buf[128]; int len = 0;
        int64_t t0 = esp_timer_get_time() / 1000;
        while ((esp_timer_get_time() / 1000) - t0 < 2000 && len < (int)sizeof(buf) - 1) {
            uint8_t c;
            if (uart_read_bytes(MODEM_UART_NUM, &c, 1, pdMS_TO_TICKS(20)) > 0) buf[len++] = (char)c;
        }
        buf[len] = '\0';
        if (strstr(buf, ",1") || strstr(buf, ",5")) return true;
        ESP_LOGI(TAG, "Rete non ancora registrata, riprovo...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return false;
}

// Sequenza completa di aggancio MQTT sul modem. Ritorna true se alla fine la
// sottoscrizione e' attiva. NOTA: AT+MCONFIG qui usa client_id come user/pass
// (placeholder "homeauto"/"homeauto") perche' il broker di destinazione non
// richiede autenticazione - se il tuo firmware Air780E rifiuta il comando con
// campi vuoti o vuole credenziali specifiche, e' questo il punto da adattare.
// Tentativi di handshake falliti consecutivamente (azzerato al primo successo):
// oltre questa soglia, ATE0+CGREG? non bastano piu' a far riprendere il modem
// (es. stack radio incastrato dopo una perdita di campo prolungata) - serve
// un riavvio piu' profondo del modulo stesso, non solo dei comandi MQTT.
#define MODEM_RESET_AFTER_FAILS 2
static int s_handshake_fails = 0;

static bool modem_handshake(void)
{
    if (s_handshake_fails >= MODEM_RESET_AFTER_FAILS) {
        ESP_LOGW(TAG, "%d handshake falliti di fila, riavvio il modulo modem (AT+CFUN=1,1)...", s_handshake_fails);
        flush_rx();
        uart_send_at("AT+CFUN=1,1");
        wait_for("OK", 5000);
        vTaskDelay(pdMS_TO_TICKS(8000)); // tempo per il reboot interno del modulo prima di riparlargli
        s_handshake_fails = 0;
    }

    flush_rx();
    uart_send_at("ATE0");
    vTaskDelay(pdMS_TO_TICKS(300));
    flush_rx();
    uart_send_at("ATE0");
    vTaskDelay(pdMS_TO_TICKS(300));
    flush_rx();

    ESP_LOGI(TAG, "Attendo registrazione rete...");
    if (!wait_network_registered(10000)) {
        ESP_LOGE(TAG, "Rete non disponibile.");
        s_handshake_fails++;
        return false;
    }
    ESP_LOGI(TAG, "Rete OK, configuro MQTT...");

    char cmd[160];
    snprintf(cmd, sizeof(cmd), "AT+MCONFIG=%s,homeauto,homeauto", s_client_id);
    uart_send_at(cmd);
    if (!wait_for("OK", 5000)) { s_handshake_fails++; return false; }

    uart_send_at("AT+MQTTMSGSET=0");
    if (!wait_for("OK", 3000)) { s_handshake_fails++; return false; }

    snprintf(cmd, sizeof(cmd), "AT+MIPSTART=\"%s\",\"%u\"", s_broker_host, s_broker_port);
    uart_send_at(cmd);
    const char *mip_candidates[3] = { "CONNECT OK", "ALREADY CONNECT", "ERROR" };
    int which = -1;
    if (!wait_for_any(mip_candidates, 3, 15000, &which)) {
        ESP_LOGE(TAG, "Timeout MIPSTART.");
        s_handshake_fails++;
        return false;
    }
    if (which == 2) {
        ESP_LOGE(TAG, "MIPSTART fallito.");
        s_handshake_fails++;
        return false;
    }
    bool tcp_already = (which == 1);

    if (tcp_already) {
        ESP_LOGI(TAG, "TCP gia' aperto, salto MCONNECT.");
    } else {
        snprintf(cmd, sizeof(cmd), "AT+MCONNECT=1,%d", MQTT_KEEPALIVE_S);
        uart_send_at(cmd);
        if (!wait_for("CONNACK OK", 10000)) { s_handshake_fails++; return false; }
    }

    snprintf(cmd, sizeof(cmd), "AT+MSUB=\"%s\",0", s_sub_topic);
    uart_send_at(cmd);
    const char *sub_candidates[2] = { "SUBACK", "OK" };
    wait_for_any(sub_candidates, 2, 5000, NULL); // non bloccante sul risultato, come l'originale

    vTaskDelay(pdMS_TO_TICKS(300));
    flush_rx();
    s_handshake_fails = 0;
    return true;
}

static void handle_rx_line(char *line)
{
    char *p = line;
    while (*p == '\r' || *p == ' ') p++;
    if (!*p) return;

    // Log ogni riga ricevuta dal modem (eccetto le "OK" solitarie che arrivano
    // dopo ogni AT+MPUB e sarebbero troppo frequenti).
    if (strcmp(p, "OK") != 0 && s_log_cb) {
        char log_buf[MODEM_LINE_MAX + 16];
        snprintf(log_buf, sizeof(log_buf), "MDM;%s", p);
        s_log_cb(log_buf);
    }

    if (strncmp(p, "+MSUB:", 6) == 0) {
        ESP_LOGI(TAG, "[MQTT IN] %s", p);
        char *marker = strstr(p, " byte,");
        if (marker) {
            char *payload = marker + 6;
            size_t pl = strlen(payload);
            if (pl >= 2 && payload[0] == '"' && payload[pl - 1] == '"') {
                payload[pl - 1] = '\0';
                payload++;
            }
            if (*payload) ESP_LOGI(TAG, "[PAYLOAD] %s", payload);
            if (s_rx_cb) s_rx_cb(payload);
        }
    } else if (strstr(p, "CONNACK OK")) {
        ESP_LOGI(TAG, "Riconnesso MQTT, rifaccio la subscribe.");
        s_consecutive_767 = 0;
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "AT+MSUB=\"%s\",0", s_sub_topic);
        uart_send_at(cmd);
    } else if (strstr(p, "767")) {
        s_consecutive_767++;
        if (s_consecutive_767 >= MAX_CONSECUTIVE_767) {
            // MCONNECT continua a fallire con 767: il TCP sottostante e' probabilmente
            // morto anche lui, riprovare solo MCONNECT in loop non risolve niente e
            // intasa la UART. Forza una riaggancio completo (con backoff 5s del loop).
            ESP_LOGE(TAG, "767 ripetuto %d volte, riaggancio completo del modem.", s_consecutive_767);
            s_consecutive_767 = 0;
            mark_disconnected();
        } else {
            ESP_LOGW(TAG, "Sessione MQTT caduta (767), riconnetto MQTT...");
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+MCONNECT=1,%d", MQTT_KEEPALIVE_S);
            uart_send_at(cmd);
            // Il CONNACK OK di conferma arriva al prossimo giro del loop RX.
        }
    } else if (strstr(p, "ERROR")) {
        // NON chiamare mark_disconnected() qui: "ERROR" e' la risposta generica
        // a un AT command fallito (AT+MPUB con payload problematico, modem
        // temporaneamente busy, ecc.) - NON indica necessariamente la perdita
        // della sessione MQTT. Ogni mark_disconnected() scatenava un handshake
        // da 30-60s durante il quale nessun +MSUB: veniva letto, rendendo l'ESP
        // sordo a tutti i comandi da Manager.py. La perdita della sessione MQTT
        // e' indicata specificamente dal codice 767 (gestito sopra).
        ESP_LOGW(TAG, "Risposta ERROR dal modem (non fatale, sessione MQTT invariata): %s", p);
    }
}

// Legge tutti i byte disponibili sulla UART del modem e li accumula in
// rx_line/rx_len, processando ogni riga completa con handle_rx_line().
// Estratta dal loop principale per poter essere chiamata anche durante il
// throttle di publish (vedi sotto): cosi' un +MSUB arrivato mentre l'ESP
// sta mandando AT+MPUB non aspetta fino al prossimo giro del loop (fino a
// PUB_THROTTLE_MS ms di ritardo, che con una coda piena diventava 1+ secondi).
static void drain_uart_rx(char *rx_line, int *rx_len)
{
    uint8_t c;
    while (uart_read_bytes(MODEM_UART_NUM, &c, 1, 0) > 0) {
        if (c == '\n') {
            // Il modem termina le righe con "\r\n": senza togliere il \r,
            // un confronto esatto come strcmp(cmd,"ON") in RELAYCMD non
            // trova mai match (mentre atoi() su MESHCMD pct/onoff lo
            // ignora silenziosamente, per questo solo RELAYCMD sembrava rotto).
            if (*rx_len > 0 && rx_line[*rx_len - 1] == '\r') (*rx_len)--;
            rx_line[*rx_len] = '\0';
            handle_rx_line(rx_line);
            *rx_len = 0;
        } else if (*rx_len < MODEM_LINE_MAX - 1) {
            rx_line[(*rx_len)++] = (char)c;
        }
    }
}

static void modem_mqtt_task(void *arg)
{
    char rx_line[MODEM_LINE_MAX]; int rx_len = 0;

    for (;;) {
        if (!s_connected) {
            rx_len = 0;
            if (modem_handshake()) {
                s_connected = true;
                ESP_LOGI(TAG, "MQTT connesso.");
                if (s_disconnect_started_us > 0) {
                    uint32_t downtime_sec = (uint32_t)((esp_timer_get_time() - s_disconnect_started_us) / 1000000);
                    s_disconnect_started_us = 0;
                    ESP_LOGW(TAG, "Ripreso dopo %u s di buco.", (unsigned)downtime_sec);
                    if (s_recovery_cb) s_recovery_cb(downtime_sec);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }

        drain_uart_rx(rx_line, &rx_len);

        // Drena la coda di publish, un messaggio per giro (throttle incluso).
        // item/esc/cmd static (non sullo stack): un solo modem_mqtt_task,
        // sequenziale, nessun rischio di concorrenza - ed erano abbastanza
        // grandi da far traboccare lo stack del task anche a 8192 byte.
        static pub_item_t item;
        bool has_item = false;
        if (xSemaphoreTake(s_pub_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_pub_count > 0) {
                item = s_pub_queue[0];
                for (int i = 1; i < s_pub_count; i++) s_pub_queue[i - 1] = s_pub_queue[i];
                s_pub_count--;
                has_item = true;
            }
            xSemaphoreGive(s_pub_mutex);
        }
        if (has_item && s_connected) {
            // AT+MPUB racchiude il payload tra virgolette. Il backslash in
            // stile C (\") NON e' supportato (provato: il modem risponde
            // "ERROR") - la doc ufficiale Air780E (docs.openluat.com/air780e/
            // at/app/command/mqtt/) usa un escape diverso, col CODICE HEX del
            // carattere: '"' (0x22) -> "\22", '\' (0x5C) -> "\5C". Necessario
            // per poter mandare JSON grezzo (es. "modem/lampade" in main.c)
            // leggibile a vista invece che in base64.
            static char esc[1800];
            int ei = 0;
            for (const char *p = item.payload; *p && ei < (int)sizeof(esc) - 5; p++) {
                if (*p == '"')       { memcpy(esc + ei, "\\22", 3); ei += 3; }
                else if (*p == '\\') { memcpy(esc + ei, "\\5C", 3); ei += 3; }
                else                  esc[ei++] = *p;
            }
            esc[ei] = '\0';
            static char cmd[sizeof(esc) + 64];
            snprintf(cmd, sizeof(cmd), "AT+MPUB=\"%s\",0,0,\"%s\"", item.topic, esc);
            uart_send_at(cmd);
            // Throttle con lettura UART continua: prima era vTaskDelay(60ms) secco,
            // che bloccava il processing dei +MSUB in arrivo per tutta la durata.
            // Con una coda piena (16 item) il ritardo totale era 16x80ms=1.28s.
            // Ora leggiamo la UART ogni 5ms durante il throttle: un comando
            // MESHCMD/RELAYCMD arrivato in questo frangente viene processato
            // subito invece di aspettare la fine del burst di publish.
            int64_t t_end = esp_timer_get_time() + (int64_t)PUB_THROTTLE_MS * 1000;
            while (esp_timer_get_time() < t_end) {
                vTaskDelay(pdMS_TO_TICKS(5));
                drain_uart_rx(rx_line, &rx_len);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void modem_mqtt_init(const char *client_id, const char *broker_host,
                      uint16_t broker_port, const char *sub_topic)
{
    strncpy(s_client_id, client_id, sizeof(s_client_id) - 1);
    strncpy(s_broker_host, broker_host, sizeof(s_broker_host) - 1);
    s_broker_port = broker_port;
    strncpy(s_sub_topic, sub_topic, sizeof(s_sub_topic) - 1);

    s_pub_mutex = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate  = MODEM_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(MODEM_UART_NUM, MODEM_RX_BUF_SIZE, 256, 0, NULL, 0);
    uart_param_config(MODEM_UART_NUM, &cfg);
    uart_set_pin(MODEM_UART_NUM, MODEM_TX_PIN, MODEM_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "UART modem attiva: TX=%d RX=%d @ %d baud (broker %s:%u).",
             MODEM_TX_PIN, MODEM_RX_PIN, MODEM_BAUD, s_broker_host, s_broker_port);

    // Pinnato sul core 1, lontano dai task interni WiFi/BT (core 0), per non
    // contendere CPU con lo stack radio della mesh.
    // Stack alzato 4096 -> 8192 -> 16384: i buffer per l'escape \22/\5C del
    // payload JSON di "modem/lampade" (item.payload[1600] + esc[1800] +
    // cmd[1864], tutti su stack nello stesso giro di modem_mqtt_task) avevano
    // ancora fatto traboccare gli 8192 (stack overflow + crash, vedi log) -
    // margine ampio stavolta per non doverci tornare ad ogni cambio di buffer.
    xTaskCreatePinnedToCore(modem_mqtt_task, "modem_mqtt", 16384, NULL, 5, NULL, 1);
}

bool modem_mqtt_is_connected(void)
{
    return s_connected;
}

void modem_mqtt_publish(const char *topic, const char *payload)
{
    if (xSemaphoreTake(s_pub_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_pub_count < PUB_QUEUE_LEN) {
            pub_item_t *it = &s_pub_queue[s_pub_count++];
            strncpy(it->topic, topic, sizeof(it->topic) - 1);
            it->topic[sizeof(it->topic) - 1] = '\0';
            strncpy(it->payload, payload, sizeof(it->payload) - 1);
            it->payload[sizeof(it->payload) - 1] = '\0';
        } // coda piena: scarta, non e' critico (ne arrivera' un altro aggiornamento)
        xSemaphoreGive(s_pub_mutex);
    }
}

void modem_mqtt_set_rx_callback(modem_mqtt_rx_cb_t cb)
{
    s_rx_cb = cb;
}

void modem_mqtt_set_recovery_callback(modem_mqtt_recovery_cb_t cb)
{
    s_recovery_cb = cb;
}

void modem_mqtt_set_log_callback(modem_mqtt_log_cb_t cb)
{
    s_log_cb = cb;
}
