// Canale USB CDC (TinyUSB) - vedi usb_cdc.h. Una sola porta CDC: qui dentro
// arrivano sia i vecchi comandi SET;/GET;/DUMP del bridge relay-controller
// sia i nuovi comandi CFG: della PWA di configurazione (deciso: niente
// ESP_LOG su questa USB, una porta sola - vedi conversazione).
#include "usb_cdc.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#define TAG "USB_CDC"
#define USB_LINE_MAX 600 // CFG:STATE in ingresso non serve (e' tutto in uscita), ma CFG:PAIR/ADDDEV col QR possono essere lunghi

// Definita in main.c: dispatcher dei comandi (CFG: e SET;/GET;/DUMP) - stessa
// convenzione di mesh_pause_for_ble_scan() in ble_classic_handler.c (extern
// diretto invece di un header condiviso, per non esportare tutto lo stato di
// main.c qui dentro).
extern void usb_cfg_handle_line(char *line);

static SemaphoreHandle_t s_tx_mutex;
static bool              s_connected = false;
static char              s_rx_line[USB_LINE_MAX];
static size_t            s_rx_len = 0;

// La callback RX di TinyUSB NON deve mai bloccarsi (vedi nota su
// tinyusb_cdcacm_write_flush in tusb_cdc_acm.h: un flush con timeout
// chiamato da dentro una callback TinyUSB puo' bloccarsi per l'intero
// timeout, perche' il completamento del flush dipende da una successiva
// callback RX che non puo' arrivare finche' siamo bloccati qui dentro).
// cfg_send_state()/cfg_status() emettono pero' decine di righe ciascuno e
// usb_cdc_send_line() deve poter bloccare in sicurezza per non perdere
// righe quando il buffer TX si riempie (vedi usb_cdc_write_all). Quindi:
// la callback RX si limita ad accodare la riga completa su s_line_queue,
// e un task dedicato (usb_cmd_task) la elabora fuori dal contesto TinyUSB.
// Coda comandi in ingresso (RX, da host a ESP)
#define USB_CMD_QUEUE_LEN 4
static QueueHandle_t s_line_queue;

typedef struct {
    char data[USB_LINE_MAX];
} usb_cmd_msg_t;

// Coda push spontanei in uscita (TX, da ESP a host via usb_cdc_push_line).
// Task dedicato: le callback BLE Mesh/timer postano qui e tornano subito,
// senza mai bloccare sul canale USB.
#define USB_PUSH_QUEUE_LEN 24
#define USB_PUSH_LINE_MAX  96

typedef struct {
    char data[USB_PUSH_LINE_MAX];
} usb_push_msg_t;

static QueueHandle_t s_push_queue;

static void usb_push_task(void *arg)
{
    (void)arg;
    usb_push_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_push_queue, &msg, portMAX_DELAY) == pdTRUE)
            usb_cdc_send_line(msg.data);
    }
}

void usb_cdc_push_line(const char *line)
{
    usb_push_msg_t msg;
    strncpy(msg.data, line, USB_PUSH_LINE_MAX - 1);
    msg.data[USB_PUSH_LINE_MAX - 1] = '\0';
    xQueueSend(s_push_queue, &msg, 0); // non-bloccante: scarta se coda piena
}

static void usb_cmd_task(void *arg)
{
    (void)arg;
    usb_cmd_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_line_queue, &msg, portMAX_DELAY) == pdTRUE) {
            usb_cfg_handle_line(msg.data);
        }
    }
}

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[64];
    size_t rx_size = 0;
    if (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx_size) != ESP_OK) return;

    for (size_t i = 0; i < rx_size; i++) {
        char c = (char)buf[i];
        if (c == '\n') {
            if (s_rx_len > 0 && s_rx_line[s_rx_len - 1] == '\r') s_rx_len--;
            s_rx_line[s_rx_len] = '\0';
            if (s_rx_len > 0) {
                usb_cmd_msg_t msg;
                memcpy(msg.data, s_rx_line, s_rx_len + 1);
                // Non bloccante: se la coda e' piena (comando precedente
                // ancora in elaborazione) scartiamo, mai bloccare qui.
                xQueueSend(s_line_queue, &msg, 0);
            }
            s_rx_len = 0;
        } else if (s_rx_len < sizeof(s_rx_line) - 1) {
            s_rx_line[s_rx_len++] = c;
        }
        // riga troppo lunga: i byte in eccesso vengono scartati finche' non
        // arriva il prossimo '\n' (stesso comportamento del bridge UART).
    }
}

static void cdc_line_state_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_connected = event->line_state_changed_data.dtr;
    ESP_LOGI(TAG, "USB CDC %s", s_connected ? "connesso (DTR alto)" : "disconnesso");
}

void usb_cdc_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    s_line_queue = xQueueCreate(USB_CMD_QUEUE_LEN, sizeof(usb_cmd_msg_t));
    s_push_queue = xQueueCreate(USB_PUSH_QUEUE_LEN, sizeof(usb_push_msg_t));
    xTaskCreate(usb_cmd_task,  "usb_cmd",  4096, NULL, 5, NULL);
    xTaskCreate(usb_push_task, "usb_push", 2048, NULL, 4, NULL);

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL, // default: VID/PID Espressif, via menuconfig
        .string_descriptor = NULL,
        .external_phy = false,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev          = TINYUSB_USBDEV_0,
        .cdc_port         = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 256,
        .callback_rx      = &cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &cdc_line_state_callback,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
    ESP_LOGI(TAG, "USB CDC attivo (protocollo CFG: + SET;/GET;/DUMP).");
}

// Scrive byte su byte (TinyUSB write_queue puo' accodarne meno di quanti
// richiesti se il ring buffer TX e' pieno) bloccando con piccoli flush+retry
// finche' non sono tutti accodati. cfg_send_state() puo' emettere decine di
// righe consecutive: con flush(0) (non bloccante, comportamento precedente)
// le righe in eccesso venivano silenziosamente scartate quando il buffer TX
// si riempiva (es. mentre arrivano anche push ONOFF/LEVEL in parallelo),
// disallineando il protocollo a blocchi CFG:STATE_START/...END lato PWA
// (mai ricevuto STATE_END -> solo timeout, contatori sempre vuoti).
static void usb_cdc_write_all(const uint8_t *data, size_t len)
{
    // Esce dopo 3 stalli consecutivi (write_queue ritorna 0 = buffer pieno e
    // nessuno lo drena). Con host connesso il buffer si svuota in <1ms dopo
    // ogni flush: non si raggiunge mai stalls=3 e tutti i dati vengono inviati.
    // Con host assente: 3 × (flush 20ms + delay 5ms) = ~75ms poi abbandona,
    // invece dei precedenti 5s (200 × 25ms). Non dipende dal DTR/s_connected.
    size_t sent = 0;
    int stalls = 0;
    while (sent < len && stalls < 3) {
        size_t n = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data + sent, len - sent);
        sent += n;
        if (sent >= len) break;
        stalls = (n == 0) ? stalls + 1 : 0;
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void usb_cdc_send_line(const char *line)
{
    // Rimosso il guard s_connected: alcuni host (Chrome Web Serial) non alzano
    // il DTR. Inviamo comunque, TinyUSB bufferizza finche' non c'e' un lettore.
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    usb_cdc_write_all((const uint8_t *)line, strlen(line));
    usb_cdc_write_all((const uint8_t *)"\n", 1);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
    xSemaphoreGive(s_tx_mutex);
}

bool usb_cdc_is_connected(void)
{
    return s_connected;
}
