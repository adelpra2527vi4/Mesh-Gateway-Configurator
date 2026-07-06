#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "relay_handler.h"
#include "ble_classic_handler.h"
#include "modem_mqtt.h"
#include "usb_cdc.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_sensor_model_api.h"

#include "ble_mesh_example_init.h"

#define TAG                "GATEWAY_MESH"
#define CID_ESP            0x02E5
#define MAX_ONOFF_ELEMENTS 8
#define MAX_LEVEL_ELEMENTS 8
#define MAX_NODES          15  // 12 dispositivi target + margine
#define MAX_BIND_QUEUE     20
#define MAX_SENSOR_ELEMENTS 4
#define GROUP_ALL          0xC000u

// Sensor model property IDs (Mesh Device Properties)
#define PROP_PRESENCE_DETECTED      0x004Du   // bool, 1 byte
#define PROP_PRESENT_AMBIENT_LIGHT  0x004Eu   // uint24, unita' 0.01 lux
// Sensor Server delle lampade (potenza/energia): PID identificati via dump
// raw sul campo (bytes= nei log "PID 0x... (ignorato)"), non confermati su
// datasheet ma coerenti coi valori osservati (0x0052 segue il dimming in
// tempo reale, 0x0072 cresce lentamente/monotono nel tempo).
#define PROP_PRESENT_INPUT_POWER    0x0052u   // uint24 LE, unita' 0.1 W
#define PROP_TOTAL_ENERGY           0x0072u   // uint32 LE, unita' Wh

// Silvair EnOcean / Companion switch proxy (vendor model sulle lampade)
#define CID_SILVAIR         0x0136
#define SILVAIR_ENOCEAN_MID 0x0001
#define OP_SILVAIR_ENOCEAN  ESP_BLE_MESH_MODEL_OP_3(0xF4, CID_SILVAIR)
#define ENOCEAN_SUBOP_SET   0x01
#define ENOCEAN_SUBOP_GET   0x00
#define ENOCEAN_SUBOP_DELETE 0x02

// Pulsante utente e LED RGB integrati sulla Waveshare ESP32-S3-Zero.
// Servono a separare a runtime la fase "WiFi/config" (provisioner web UI,
// tenuta premuta) dalla fase "dati" (BLE mesh + UART verso il relay-
// controller, di default): tenendo WiFi spento il piu' possibile si
// libera banda radio alla mesh, accelerando lettura/invio dei comandi.
// Pin ufficiali Waveshare ESP32-S3-Relay-6CH (wiki): BOOT button = GPIO0,
// LED RGB WS2812 integrato = GPIO38. GPIO21 su questa board e' il buzzer,
// NON il LED (corretto qui: era erroneamente impostato su GPIO21).
#define USER_BTN_GPIO       GPIO_NUM_0
#define RGB_LED_GPIO        GPIO_NUM_38

// ============================================================
// Forward declarations
// ============================================================
static void gateway_read_onoff_state(uint16_t target_addr);
static void gateway_read_level_state(uint16_t target_addr);
static void gateway_read_sensor(uint16_t target_addr);
static void save_nodes_nvs(void);
static void send_onoff_elem(uint8_t node_idx, uint8_t elem_idx, uint8_t onoff);
static void send_level_elem(uint8_t node_idx, uint8_t level_idx, int16_t level_val);
static void send_level_elem_ex(uint8_t node_idx, uint8_t level_idx, int16_t level_val,
                                uint8_t trans_time, uint8_t delay);
static void uart_push_onoff(uint16_t addr, uint8_t val);
static void uart_push_level(uint16_t addr, int16_t val);
static void uart_push_sensor(uint16_t addr, int8_t presence, int32_t light_cl);
static void send_onoff_group(uint16_t group_addr, uint8_t onoff);
static void send_level_group(uint16_t group_addr, int16_t level_val);
static void send_level_group_ex(uint16_t group_addr, int16_t level_val,
                                 uint8_t trans_time, uint8_t delay);
static void rgb_led_set(uint8_t r, uint8_t g, uint8_t b);
static void url_decode_inplace(char *s);
static void publish_caps(void);
static void publish_sensorcfg(void);
static void publish_relaystate(void);
static int  publish_meshconfig(void);
static void publish_mesh_sensor_names(void);
static void force_full_mqtt_resync(void);

// ============================================================
// Bind queue — sequenza di operazioni di configurazione
// ============================================================
typedef enum { BIND_APP_KEY, BIND_PUB_SET, BIND_SUB_ADD, BIND_SUB_DEL, BIND_RELAY_SET } bind_op_t;

typedef struct {
    uint16_t  elem_addr;
    uint16_t  model_id;
    bind_op_t op;
    uint16_t  group_addr;
} bind_entry_t;

static bind_entry_t bind_queue[MAX_BIND_QUEUE];
static uint8_t      bind_queue_len = 0;

// ============================================================
// Nodi gestiti (lampade e switch)
// ============================================================
typedef struct {
    uint16_t unicast;
    uint8_t  elem_num;
    bool     configured;
    bool     failed;
    bool     is_switch;

    // lampade: OnOff Server
    uint8_t  onoff_offsets[MAX_ONOFF_ELEMENTS];
    uint8_t  onoff_states[MAX_ONOFF_ELEMENTS];
    uint8_t  onoff_count;

    // lampade: Level Server
    uint8_t  level_offsets[MAX_LEVEL_ELEMENTS];
    int16_t  level_states[MAX_LEVEL_ELEMENTS];
    uint8_t  level_count;

    // switch: OnOff Client
    uint8_t  cli_offsets[MAX_ONOFF_ELEMENTS];
    uint8_t  cli_count;
    bool     companion_paired;
    uint16_t companion_lamp_elem;

    // sensore: Sensor Server (model 0x1100)
    // NB: presenza e luce sono per-ELEMENTO, non per-nodo: un nodo puo'
    // avere un elemento dedicato alla presenza e un altro alla luce
    // ambientale, ognuno con il proprio Sensor Server indipendente.
    bool     is_sensor;             // hint da auto-detect (non decisivo)
    uint8_t  sensor_offsets[MAX_SENSOR_ELEMENTS];
    uint8_t  sensor_count;
    int8_t   sensor_presence[MAX_SENSOR_ELEMENTS];  // -1 = sconosciuto, 0/1
    int32_t  sensor_light_cl[MAX_SENSOR_ELEMENTS];  // -1 = sconosciuto, altrimenti centilux (lux*100)
    // Sensor Server delle LAMPADE (stesso model 0x1100, elemento dedicato):
    // potenza istantanea in ingresso e energia totale accumulata. PID
    // identificati via dump raw sul campo (vedi PROP_PRESENT_INPUT_POWER/
    // PROP_TOTAL_ENERGY): -1/-1 = sconosciuto.
    int32_t  sensor_power_dw[MAX_SENSOR_ELEMENTS];  // -1 = sconosciuto, altrimenti deciwatt (W*10)
    int32_t  sensor_energy_wh[MAX_SENSOR_ELEMENTS]; // -1 = sconosciuto, altrimenti Wh
    // Calibrazione luce ambiente: offset in centilux (lux*100) aggiunto al
    // valore grezzo del Sensor Server prima di salvarlo/pubblicarlo - serve a
    // far coincidere la lettura con un luxmetro di riferimento. Un solo nodo
    // ha sempre un solo elemento che riporta la luce (l'altro la presenza),
    // quindi un campo per-nodo basta (non serve un array per-elemento).
    // Default 0 = nessuna calibrazione, anche sui nodi salvati prima di
    // questo campo (NVS blob piu' vecchio, coda non scritta resta a 0).
    // int32 (non int16!): la differenza tra lettura vera e valore grezzo del
    // sensore puo' superare facilmente i 327 lux che un int16 in centilux
    // riesce a contenere - un primo tentativo con int16 troncava silenziosamente
    // l'offset (clamp) producendo una calibrazione completamente sbagliata.
    int32_t  sensor_light_cal;

    uint8_t  kind;                  // scelto dall'utente: NODE_LAMP / NODE_SENSOR

    // UUID del dispositivo (fisso di fabbrica, non cambia mai) salvato al
    // provisioning - serve a riconoscere quando un device gia' noto "cade"
    // dalla mesh e ricompare in discovered[] con lo stesso UUID: la PWA puo'
    // avvisare prima di riprovisionarlo che e' lo stesso nodo (es. "AltoDX")
    // invece di sembrare che sparisca/si sostituisca con uno nuovo a sorpresa
    // (vedi conversazione - utente confuso da indirizzi unicast riassegnati
    // allo stesso device che si ripresentava come "non provisionato").
    uint8_t  uuid[16];
} lamp_node_t;

#define NODE_LAMP   0
#define NODE_SENSOR 1

static lamp_node_t nodes[MAX_NODES];
static uint8_t     node_count = 0;

// Nome assegnato dall'utente, indicizzato per UNICAST ADDRESS (non per indice
// in nodes[]!). Era indicizzato per indice di array: /forget fa una memmove
// di nodes[] che sposta gli indici di tutti i nodi successivi ma NON
// spostava in parallelo node_names[], quindi dopo aver rimosso un nodo (o
// dopo che node_count viene ricostruito da zero in altro ordine, es. lo
// scarto-tutto su NVS corrotta) i nomi finivano associati al device sbagliato.
// Tenuto comunque separato dalla struct lamp_node_t (che resta un blob NVS
// grezzo) per non rompere la compatibilita' di quel blob.
static uint16_t name_addrs[MAX_NODES];
static char     node_names[MAX_NODES][24];
static uint8_t  name_count = 0;

static const char *get_node_name(uint16_t addr)
{
    for (uint8_t i = 0; i < name_count; i++)
        if (name_addrs[i] == addr) return node_names[i];
    return "";
}

static void set_node_name(uint16_t addr, const char *name)
{
    int idx = -1;
    for (uint8_t i = 0; i < name_count; i++)
        if (name_addrs[i] == addr) { idx = i; break; }
    if (idx < 0) {
        if (name_count >= MAX_NODES) return; // tabella piena, non dovrebbe succedere
        idx = name_count++;
        name_addrs[idx] = addr;
    }
    strncpy(node_names[idx], name, sizeof(node_names[idx]) - 1);
    node_names[idx][sizeof(node_names[idx]) - 1] = '\0';
}

// Solo RAM (non in NVS, niente problemi di compatibilita' del blob salvato):
// true se il nodo ha completato con successo il bind queue (incluso il
// SUB_ADD a GROUP_ALL) in QUESTA sessione del gateway - serve solo come
// indicatore in UI per sapere chi ha gia' fatto il rebind dopo l'update
// che ha aggiunto la sub del Level Server. Si azzera ad ogni riavvio: la
// sub vera e propria vive sul nodo mesh stesso ed e' persistente li',
// questo flag dice solo "l'ho verificato/rifatto in questo boot".
static bool group_bound[MAX_NODES];

// Ultimo livello/onoff comandato dal GATEWAY verso ogni elemento lampada.
// RAM only (non in NVS, lamp_node_t e' un blob grezzo che non va toccato).
// Serve a distinguere un cambio di stato dovuto a un nostro MESHCMD da uno
// dovuto a un pulsante companion: se il poll riporta un livello diverso da
// quello che abbiamo noi comandato l'ultima volta, qualcun altro ha cambiato
// la lampada => MESHOVERRIDE pubblicato su MQTT => Manager.py inibisce
// l'automazione per quella lampada per il tempo configurato.
// valid=false finche' non abbiamo MAI comandato quell'elemento in questa sessione.
static bool    s_cmd_level_valid[MAX_NODES][MAX_LEVEL_ELEMENTS];
static int16_t s_cmd_level_val  [MAX_NODES][MAX_LEVEL_ELEMENTS];

static int     configuring_idx = -1;
static bool    config_busy     = false;
static uint8_t bind_idx        = 0;
static uint8_t cfg_retry       = 0;
#define MAX_CFG_RETRY 5

// Punto 3: config_busy diventa true solo a PROV_LINK_CLOSE (dopo che il
// nodo e' GIA' provisionato, prima del bind AppKey) - lascia scoperta la
// finestra fra CFG:PROVISION e la chiusura del link di provisioning vero e
// proprio, durante la quale altre attivita' BLE (scan classico, nuovi
// unprovisioned in discovered[]) potrebbero comunque intromettersi sullo
// stesso radio condiviso. g_node_provisioning_active copre ESATTAMENTE
// quella finestra: settata in cfg_provision() prima di add_unprov_dev(),
// azzerata quando il provisioning fallisce/completa la configurazione (o
// se il nodo risulta "failed", vedi piu' sotto), e va in OR con
// config_busy/pair_active dentro gateway_is_provisioning().
static bool g_node_provisioning_active = false;
static int64_t g_node_provisioning_started_us = 0; // per il watchdog in poll_cb, vedi sotto
static uint8_t g_provisioning_uuid[16] = {0}; // uuid del device che si sta provisionando ORA, per il watchdog
bool gateway_is_provisioning(void); // def. piu' sotto - usata da poll_cb() prima della sua definizione

// ============================================================
// Dispositivi rilevati ma non ancora provisionati
// ============================================================
#define MAX_DISCOVERED       20
#define DISCOVERED_EXPIRE_US (30LL * 1000000LL)

typedef struct {
    uint8_t  uuid[16];
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint8_t  bearer;
    uint16_t oob_info;
    bool     needs_qr_oob;
    int8_t   rssi;
    int64_t  last_seen_us;
} discovered_dev_t;

static discovered_dev_t discovered[MAX_DISCOVERED];
static uint8_t          discovered_count = 0;

// ============================================================
// Static OOB per switch con QR code
// ============================================================
static uint8_t oob_buf[16]  = {0};
static bool    oob_ready    = false;

static int find_node_by_addr(uint16_t addr)
{
    for (uint8_t i = 0; i < node_count; i++) {
        if (nodes[i].unicast != ESP_BLE_MESH_ADDR_UNASSIGNED &&
            addr >= nodes[i].unicast &&
            addr <  (uint16_t)(nodes[i].unicast + nodes[i].elem_num)) {
            return i;
        }
    }
    return -1;
}

static int find_next_unconfigured(void)
{
    for (uint8_t i = 0; i < node_count; i++) {
        if (!nodes[i].configured && !nodes[i].failed) return i;
    }
    return -1;
}

static const uint8_t fixed_app_key[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};

static esp_ble_mesh_client_t config_client;
static esp_ble_mesh_client_t onoff_client;
static esp_ble_mesh_client_t level_client;
static esp_ble_mesh_client_t sensor_client;

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit    = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay           = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit= ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon          = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy      = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state    = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state    = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl     = 7,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(NULL, &onoff_client),
    ESP_BLE_MESH_MODEL_GEN_LEVEL_CLI(NULL, &level_client),
    ESP_BLE_MESH_MODEL_SENSOR_CLI(NULL, &sensor_client),
};

// Vendor model nostro (CID Espressif) solo per INVIARE l'opcode Silvair 0xF4
// alle lampade. L'op table serve a ricevere eventuali STATUS di ritorno.
static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(OP_SILVAIR_ENOCEAN, 0),
    ESP_BLE_MESH_MODEL_OP_END,
};
static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, 0x0000, vnd_op, NULL, NULL),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid           = CID_ESP,
    .element_count = ARRAY_SIZE(elements),
    .elements      = elements,
};

static uint8_t dev_uuid[16] = {0};

static esp_ble_mesh_prov_t prov = {
    .prov_uuid           = dev_uuid,
    .prov_unicast_addr   = 0x0001,
    .prov_start_address  = 0x0005,
    .prov_static_oob_val = oob_buf,
    .prov_static_oob_len = 0,
};

// ============================================================
// Helper: parametri comuni messaggi client
// ============================================================
static void set_msg_common(esp_ble_mesh_client_common_param_t *common,
                           uint16_t dst_addr,
                           esp_ble_mesh_model_t *model,
                           uint32_t opcode)
{
    common->opcode       = opcode;
    common->model        = model;
    common->ctx.net_idx  = 0;
    common->ctx.app_idx  = (model == config_client.model) ? ESP_BLE_MESH_KEY_DEV : 0;
    common->ctx.addr     = dst_addr;
    common->ctx.send_ttl = 7;
    common->msg_timeout  = 8000;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
    common->msg_role = ROLE_PROVISIONER;
#endif
}

// ============================================================
// Parser Composition Data Page 0
// ============================================================
static void parse_composition_data(struct net_buf_simple *buf, lamp_node_t *node)
{
    node->onoff_count  = 0;
    node->level_count  = 0;
    node->cli_count    = 0;
    node->is_switch    = false;
    node->is_sensor    = false;
    node->sensor_count = 0;

    if (!buf || buf->len < 10) {
        ESP_LOGW(TAG, "Composition Data assente/troppo corta.");
        return;
    }

    uint8_t  *p          = buf->data;
    uint16_t  len        = buf->len;
    uint16_t  idx        = 10;
    uint8_t   elem_index = 0;

    while (idx + 4 <= len) {
        uint8_t  num_s        = p[idx + 2];
        uint8_t  num_v        = p[idx + 3];
        uint16_t models_start = idx + 4;

        for (uint8_t i = 0; i < num_s; i++) {
            uint16_t off = models_start + i * 2;
            if (off + 1 >= len) break;
            uint16_t model_id = p[off] | (p[off + 1] << 8);

            if (model_id == ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV &&
                node->onoff_count < MAX_ONOFF_ELEMENTS) {
                ESP_LOGI(TAG, "OnOff Server: elem #%d", elem_index);
                node->onoff_offsets[node->onoff_count] = elem_index;
                node->onoff_states[node->onoff_count]  = 0;
                node->onoff_count++;
            }
            if (model_id == ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_SRV &&
                node->level_count < MAX_LEVEL_ELEMENTS) {
                ESP_LOGI(TAG, "Level Server: elem #%d", elem_index);
                node->level_offsets[node->level_count] = elem_index;
                node->level_states[node->level_count]  = 0;
                node->level_count++;
            }
            if (model_id == ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI &&
                node->cli_count < MAX_ONOFF_ELEMENTS) {
                ESP_LOGI(TAG, "OnOff Client: elem #%d", elem_index);
                node->cli_offsets[node->cli_count] = elem_index;
                node->cli_count++;
            }
            if (model_id == ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_CLI) {
                ESP_LOGI(TAG, "Level Client: elem #%d", elem_index);
            }
            if (model_id == ESP_BLE_MESH_MODEL_ID_SENSOR_SRV &&
                node->sensor_count < MAX_SENSOR_ELEMENTS) {
                ESP_LOGI(TAG, "Sensor Server: elem #%d", elem_index);
                node->sensor_offsets[node->sensor_count++] = elem_index;
            }
        }

        uint16_t vnd_start = models_start + (uint16_t)num_s * 2;
        for (uint8_t i = 0; i < num_v; i++) {
            uint16_t off = vnd_start + i * 4;
            if (off + 3 >= len) break;
            uint16_t cid = p[off]     | (p[off + 1] << 8);
            uint16_t mid = p[off + 2] | (p[off + 3] << 8);
            ESP_LOGI(TAG, "Vendor Model: elem #%d  cid=0x%04x mid=0x%04x", elem_index, cid, mid);
        }

        idx = vnd_start + (uint16_t)num_v * 4;
        elem_index++;
    }

    // La classificazione automatica e' inaffidabile (lampade e PIR 0-10V hanno gli
    // stessi modelli OnOff/Level/Sensor). Diamo solo un default ragionevole: SENSORE
    // se ha Sensor Server e nessun OnOff (caso "puro sensore"), altrimenti LAMPADA.
    // L'utente puo' poi cambiare il tipo dalla UI (menu a tendina -> /setkind).
    for (uint8_t i = 0; i < MAX_SENSOR_ELEMENTS; i++) {
        node->sensor_presence[i]  = -1;
        node->sensor_light_cl[i]  = -1;
        node->sensor_power_dw[i]  = -1;
        node->sensor_energy_wh[i] = -1;
    }
    node->sensor_light_cal = 0; // solo per nodi nuovi (mai configurati): un nodo
                                // gia' noto non passa di nuovo da qui, la sua
                                // calibrazione salvata in NVS resta intatta.
    node->is_sensor = (node->sensor_count > 0);
    node->is_switch = (node->cli_count > 0 && node->onoff_count == 0 && node->sensor_count == 0);
    node->kind = (node->sensor_count > 0 && node->onoff_count == 0) ? NODE_SENSOR : NODE_LAMP;

    ESP_LOGI(TAG, "Capabilities: OnOff=%d Level=%d Sensor=%d Cli=%d -> default %s",
             node->onoff_count, node->level_count, node->sensor_count, node->cli_count,
             node->kind == NODE_SENSOR ? "SENSORE" : "LAMPADA");

    if (node->onoff_count == 0 && node->level_count == 0 && node->sensor_count == 0 &&
        node->cli_count == 0) {
        ESP_LOGW(TAG, "Nessun modello noto: fallback OnOff elem 0.");
        node->onoff_offsets[0] = 0;
        node->onoff_states[0]  = 0;
        node->onoff_count      = 1;
    }
}

// ============================================================
// Costruisce la coda bind
// ============================================================
static void build_bind_queue(lamp_node_t *node, uint16_t base)
{
    bind_queue_len = 0;

    // Abilita il Relay feature su QUESTO nodo, primo passo della coda (cosi'
    // passa per lo stesso meccanismo sequenziale "un comando alla volta,
    // aspetta risposta, poi il prossimo" di tutto il resto del bind - vedi
    // nota in ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT). Di default molti
    // dispositivi mesh hanno il Relay DISABILITATO: senza, un nodo
    // provisionato fuori dalla portata diretta del gateway ma raggiungibile
    // solo passando per un nodo gia' configurato non riceve mai nulla.
    // model_id/group_addr non usati per questo op (vedi send_next_bind).
    bind_queue[bind_queue_len++] = (bind_entry_t){ base, 0, BIND_RELAY_SET, 0 };

    if (node->is_switch) {
        // Puro switch mesh (raro): bind + pubblicazione OnOff Client su GROUP_ALL
        for (uint8_t i = 0; i < node->cli_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            uint16_t ea = base + node->cli_offsets[i];
            bind_queue[bind_queue_len++] = (bind_entry_t){
                ea, ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, BIND_APP_KEY, 0
            };
        }
        for (uint8_t i = 0; i < node->cli_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            uint16_t ea = base + node->cli_offsets[i];
            bind_queue[bind_queue_len++] = (bind_entry_t){
                ea, ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, BIND_PUB_SET, GROUP_ALL
            };
        }
    } else {
        // Qualsiasi device con server: lega AppKey a TUTTE le capability presenti
        // (OnOff + Level + Sensor), cosi' il "kind" scelto dall'utente funziona
        // sempre senza dover ri-bindare.
        for (uint8_t i = 0; i < node->onoff_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            bind_queue[bind_queue_len++] = (bind_entry_t){
                base + node->onoff_offsets[i], ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV, BIND_APP_KEY, 0
            };
        }
        for (uint8_t i = 0; i < node->level_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            bind_queue[bind_queue_len++] = (bind_entry_t){
                base + node->level_offsets[i], ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_SRV, BIND_APP_KEY, 0
            };
        }
        for (uint8_t i = 0; i < node->sensor_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            bind_queue[bind_queue_len++] = (bind_entry_t){
                base + node->sensor_offsets[i], ESP_BLE_MESH_MODEL_ID_SENSOR_SRV, BIND_APP_KEY, 0
            };
        }
        for (uint8_t i = 0; i < node->onoff_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            bind_queue[bind_queue_len++] = (bind_entry_t){
                base + node->onoff_offsets[i], ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV, BIND_SUB_ADD, GROUP_ALL
            };
        }
        // Anche il Level Server in GROUP_ALL: serve a send_onoff_group()/
        // send_level_group() (vedi bridge_handle_line) per accendere/dimmare
        // tutte le lampade della stanza con UNA sola trasmissione radio,
        // invece di N comandi unicast sequenziali (causa dell'accensione
        // "a scaglioni" osservata tornando in automatico).
        for (uint8_t i = 0; i < node->level_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            bind_queue[bind_queue_len++] = (bind_entry_t){
                base + node->level_offsets[i], ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_SRV, BIND_SUB_ADD, GROUP_ALL
            };
        }
    }

    ESP_LOGI(TAG, "Coda bind: %d voci", bind_queue_len);
}

static void send_next_bind(uint16_t dst_node)
{
    if (bind_idx >= bind_queue_len) return;
    bind_entry_t *e = &bind_queue[bind_idx];

    esp_ble_mesh_client_common_param_t  common    = {0};
    esp_ble_mesh_cfg_client_set_state_t set_state = {0};

    switch (e->op) {
    case BIND_RELAY_SET:
        ESP_LOGI(TAG, "RELAY_SET @ 0x%04x (%d/%d)", e->elem_addr, bind_idx + 1, bind_queue_len);
        set_msg_common(&common, dst_node, config_client.model,
                       ESP_BLE_MESH_MODEL_OP_RELAY_SET);
        set_state.relay_set.relay            = 1;    // abilitato
        // count=1 (1 ritrasmissione dopo il TX iniziale = 2 totali), intervallo
        // 10ms*(2+1)=30ms. Il valore precedente (0x05 = count=5, 10ms) generava
        // 6 copie per ogni messaggio relayed, moltiplicato per 3+ nodi relay
        // attivi = congestione radio sistematica che bloccava la config del 4°+
        // nodo (Composition Data Get mai risposto, timeout ripetuti).
        set_state.relay_set.relay_retransmit = 0x11; // count=1 (bit0-2=1), intervallo 30ms (bit3-7=2)
        break;

    case BIND_APP_KEY:
        ESP_LOGI(TAG, "APP_BIND 0x%04x @ 0x%04x (%d/%d)",
                 e->model_id, e->elem_addr, bind_idx + 1, bind_queue_len);
        set_msg_common(&common, dst_node, config_client.model,
                       ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
        set_state.model_app_bind.element_addr  = e->elem_addr;
        set_state.model_app_bind.model_id      = e->model_id;
        set_state.model_app_bind.company_id    = ESP_BLE_MESH_CID_NVAL;
        set_state.model_app_bind.model_app_idx = 0;
        break;

    case BIND_PUB_SET:
        ESP_LOGI(TAG, "PUB_SET 0x%04x @ 0x%04x -> 0x%04x (%d/%d)",
                 e->model_id, e->elem_addr, e->group_addr,
                 bind_idx + 1, bind_queue_len);
        set_msg_common(&common, dst_node, config_client.model,
                       ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET);
        set_state.model_pub_set.element_addr       = e->elem_addr;
        set_state.model_pub_set.publish_addr       = e->group_addr;
        set_state.model_pub_set.publish_app_idx    = 0;
        set_state.model_pub_set.cred_flag          = false;
        set_state.model_pub_set.publish_ttl        = 7;
        set_state.model_pub_set.publish_period     = 0;
        set_state.model_pub_set.publish_retransmit = 0;
        set_state.model_pub_set.model_id           = e->model_id;
        set_state.model_pub_set.company_id         = ESP_BLE_MESH_CID_NVAL;
        break;

    case BIND_SUB_ADD:
        ESP_LOGI(TAG, "SUB_ADD 0x%04x @ 0x%04x -> 0x%04x (%d/%d)",
                 e->model_id, e->elem_addr, e->group_addr,
                 bind_idx + 1, bind_queue_len);
        set_msg_common(&common, dst_node, config_client.model,
                       ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD);
        set_state.model_sub_add.element_addr = e->elem_addr;
        set_state.model_sub_add.sub_addr     = e->group_addr;
        set_state.model_sub_add.model_id     = e->model_id;
        set_state.model_sub_add.company_id   = ESP_BLE_MESH_CID_NVAL;
        break;

    case BIND_SUB_DEL:
        ESP_LOGI(TAG, "SUB_DEL 0x%04x @ 0x%04x -> 0x%04x (%d/%d)",
                 e->model_id, e->elem_addr, e->group_addr,
                 bind_idx + 1, bind_queue_len);
        set_msg_common(&common, dst_node, config_client.model,
                       ESP_BLE_MESH_MODEL_OP_MODEL_SUB_DELETE);
        set_state.model_sub_delete.element_addr = e->elem_addr;
        set_state.model_sub_delete.sub_addr     = e->group_addr;
        set_state.model_sub_delete.model_id     = e->model_id;
        set_state.model_sub_delete.company_id   = ESP_BLE_MESH_CID_NVAL;
        break;
    }

    esp_ble_mesh_config_client_set_state(&common, &set_state);
}

// ============================================================
// COMPANION SWITCH (Silvair EnOcean proxy)
// Flusso: bind AppKey al vendor model 0x0136/0x0001 della lampada,
// poi invio SET con [subop][chiave 16][MAC 6].
// ============================================================
static bool     pair_active = false;
static uint16_t pair_dst    = 0;          // unicast elem0 della lampada
static uint8_t  pair_key[16] = {0};
static uint8_t  pair_mac[6]  = {0};
static uint8_t  pair_retry  = 0;
static uint8_t  pair_stage  = 0;          // 1=bind vendor, 2=bind+pub OnOff CLI
static uint16_t pair_target_elem = 0;
static bool     pair_is_unpair = false;   // true = flusso /unpair (non rimarcare paired)

// Il proxy, alla pressione, pubblica tramite i suoi OnOff Client interni: vanno
// bindati alla AppKey e gli va impostato il publish address (Nordic conferma che
// la destinazione e' la publication del client, non una dest nel SET).
static void build_pair_pub_queue(uint16_t pub_addr) { 
    bind_queue_len = 0;
    uint16_t e0 = pair_dst + 0;
    uint16_t e1 = pair_dst + 1;
    // placca A (elem0) -> ON/OFF (OnOff Client) ; placca B (elem1) -> DIM (Level Client)
    bind_queue[bind_queue_len++] = (bind_entry_t){e0, ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, BIND_APP_KEY, 0};
    bind_queue[bind_queue_len++] = (bind_entry_t){e1, ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_CLI, BIND_APP_KEY, 0};
    bind_queue[bind_queue_len++] = (bind_entry_t){e0, ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, BIND_PUB_SET, pub_addr};
    bind_queue[bind_queue_len++] = (bind_entry_t){e1, ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_CLI, BIND_PUB_SET, pub_addr};
    ESP_LOGI(TAG, "Companion: placca A=OnOff(e0) B=Level(e1) -> 0x%04x, %d voci",
             pub_addr, bind_queue_len);
}

static void pair_send_set(void)
{
    uint8_t payload[1 + 16 + 6];
    payload[0] = ENOCEAN_SUBOP_SET;
    memcpy(&payload[1],  pair_key, 16);
    memcpy(&payload[17], pair_mac, 6);

    ESP_LOGI(TAG, "Companion SET -> 0x%04x payload(%d):", pair_dst, (int)sizeof(payload));
    ESP_LOG_BUFFER_HEX(TAG, payload, sizeof(payload));

    esp_ble_mesh_msg_ctx_t ctx = {0};
    ctx.net_idx  = 0;
    ctx.app_idx  = 0;
    ctx.addr     = pair_dst;
    ctx.send_ttl = 7;

    esp_err_t err = esp_ble_mesh_server_model_send_msg(
        &vnd_models[0], &ctx, OP_SILVAIR_ENOCEAN, sizeof(payload), payload);
    ESP_LOGI(TAG, "Companion SET inviato (err 0x%x).", err);
}

// Invia ENOOCEAN_SUBOP_DELETE alla lampada per scollegare il companion
static void unpair_send_delete(uint16_t lamp_elem0) {
    uint8_t payload[1] = { ENOCEAN_SUBOP_DELETE };
    esp_ble_mesh_msg_ctx_t ctx = {0};
    ctx.net_idx  = 0;
    ctx.app_idx  = 0;
    ctx.addr     = lamp_elem0;
    ctx.send_ttl = 7;
    esp_err_t err = esp_ble_mesh_server_model_send_msg(
        &vnd_models[0], &ctx, OP_SILVAIR_ENOCEAN,
        sizeof(payload), payload);
    ESP_LOGI(TAG, "Companion DELETE -> 0x%04x (err 0x%x)", lamp_elem0, err);
}

// ============================================================
// Helper generici per il protocollo USB CDC "CFG:" (vedi usb_cdc.c). Stesso
// stile di bridge_get_kv_hex/bridge_get_kv_int (sopra, per SET;/GET;): cerca
// "key=" con strstr e legge da li'. cfg_kv_rest e' diversa - serve per "qr="
// che puo' contenere ';' al suo interno (vedi nota protocollo): prende TUTTO
// il resto della riga, non si ferma al prossimo ';'.
// ============================================================
static bool cfg_kv_int(const char *line, const char *key, int *out)
{
    char pat[16]; snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    *out = (int)strtol(p + strlen(pat), NULL, 10);
    return true;
}

static bool cfg_kv_rest(const char *line, const char *key, char *out, size_t outsz)
{
    char pat[16]; snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    strncpy(out, p + strlen(pat), outsz - 1);
    out[outsz - 1] = '\0';
    return true;
}

// %XX -> byte, NESSUN '+'->spazio (a differenza di url_decode_inplace usato
// da /setname): nel QR '+' e' il separatore di campo, va lasciato intatto.
static void cfg_urldecode(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char h[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(h, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = 0;
}

static void cfg_ok(const char *cmd)   { char m[32]; snprintf(m, sizeof(m), "CFG:OK;%s", cmd);   usb_cdc_send_line(m); }
static void cfg_busy(const char *cmd) { char m[32]; snprintf(m, sizeof(m), "CFG:BUSY;%s", cmd); usb_cdc_send_line(m); }
static void cfg_err(const char *cmd, const char *msg)
{
    char m[256]; snprintf(m, sizeof(m), "CFG:ERR;%s;%s", cmd, msg); usb_cdc_send_line(m);
}

// ============================================================
// LED RGB (WS2812 su RMT) — indicatore di stato. Recuperato da una versione
// precedente del file (era stato cancellato per errore insieme a
// user_button_task durante la rimozione del WiFi): verde fisso = mesh attivo,
// la variazione ambra/WiFi e' sparita insieme al pulsante/WiFi (vedi spec).
// ============================================================
static rmt_channel_handle_t led_chan          = NULL;
static rmt_encoder_handle_t led_bytes_encoder  = NULL;

static void rgb_led_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = RGB_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz     = 10 * 1000 * 1000,  // 10 MHz -> 1 tick = 100 ns
        .trans_queue_depth = 4,
    };
    esp_err_t err_ch = rmt_new_tx_channel(&tx_chan_config, &led_chan);
    if (err_ch != ESP_OK) {
        ESP_LOGW(TAG, "RMT LED: new_tx_channel fallita (0x%x), indicatore disabilitato.", err_ch);
        return;
    }

    // Timing WS2812 standard: T0H=0.3us/T0L=0.9us, T1H=0.9us/T1L=0.3us.
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = { .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0 },
        .bit1 = { .duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0 },
        .flags.msb_first = 1,
    };
    esp_err_t err_enc = rmt_new_bytes_encoder(&bytes_encoder_config, &led_bytes_encoder);
    esp_err_t err_en  = rmt_enable(led_chan);
    if (err_enc != ESP_OK || err_en != ESP_OK) {
        ESP_LOGW(TAG, "RMT LED: encoder (0x%x) / enable (0x%x) falliti, indicatore disabilitato.",
                 err_enc, err_en);
        led_chan = NULL;
        return;
    }
    ESP_LOGI(TAG, "RMT LED: inizializzato su GPIO%d.", RGB_LED_GPIO);
}

static void rgb_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_chan == NULL || led_bytes_encoder == NULL) {
        ESP_LOGW(TAG, "RMT LED: set(%d,%d,%d) ignorato, canale non inizializzato.", r, g, b);
        return;
    }
    // Ordine canali WS2812 standard GRB.
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    esp_err_t e = rmt_transmit(led_chan, led_bytes_encoder, grb, sizeof(grb), &tx_config);
    if (e != ESP_OK) ESP_LOGW(TAG, "RMT LED: transmit(%d,%d,%d) errore 0x%x.", r, g, b, e);
}

// ============================================================
// Pulsante BOOT (USER_BTN_GPIO, GPIO0, attivo basso) — toggle modalita' USB.
// Ricalca il vecchio comportamento WiFi: di default il device e' in
// modalita' "normale" (pubblica solo su MQTT, mqtt_periodic_publish attivo);
// tenendo premuto BOOT per piu' di USB_MODE_LONGPRESS_MS il device passa in
// modalita' "USB config" (sospende mqtt_periodic_publish, come faceva la
// vecchia SoftAP, e rende operativi i comandi CFG: di scrittura/provisioning
// dalla PWA via USB). Un altro long-press in modalita' USB torna a quella
// normale. Letto/debounciato dentro poll_cb (gia' un timer periodico a 2s -
// qui sotto il debounce e' fatto leggendo il pin a ogni tick del timer
// invece di crearne uno nuovo, vedi nota istruzioni).
// ============================================================
#define USB_MODE_LONGPRESS_MS 2000

static bool g_usb_mode_active = false; // false = normale/MQTT (default al boot), true = USB config

static void user_btn_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << USER_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE, // BOOT e' attivo basso, premuto = 0
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

// Chiamata da poll_cb ogni 2s: non e' un vero debounce a campionamento
// rapido (il tick e' troppo rado per quello), ma basta per un long-press di
// secondi - misura il tempo trascorso da quando il pin e' visto basso senza
// interruzioni e scatta il toggle una sola volta al superamento della soglia.
static void user_btn_poll(void)
{
    static bool    was_pressed   = false;
    static int64_t pressed_at_us = 0;
    static bool    fired         = false; // evita toggle ripetuti finche' il tasto resta premuto

    bool pressed = (gpio_get_level(USER_BTN_GPIO) == 0);
    int64_t now_us = esp_timer_get_time();

    if (pressed && !was_pressed) {
        pressed_at_us = now_us;
        fired         = false;
    } else if (pressed && !fired &&
               (now_us - pressed_at_us) >= (int64_t)USB_MODE_LONGPRESS_MS * 1000) {
        g_usb_mode_active = !g_usb_mode_active;
        fired = true;
        ESP_LOGI(TAG, "BOOT long-press: modalita' USB %s.", g_usb_mode_active ? "ATTIVATA" : "disattivata");
        // Feedback colori coerente con rgb_led_set(0,25,0)="verde fisso = mesh
        // attivo" gia' usato altrove nel file: blu fisso per la modalita' USB.
        rgb_led_set(g_usb_mode_active ? 0 : 0, g_usb_mode_active ? 0 : 25, g_usb_mode_active ? 25 : 0);
        // Uscendo dalla modalita' USB, forza la ripubblicazione completa dello
        // stato: Manager.py non riceve nulla (MESH|, CAPS, RELAYSTATE) durante
        // la modalita' blu. Se si e' anche riavviato mentre l'ESP era in USB,
        // il suo CONFIGREQ|ALL e' stato ignorato (mqtt_rx_handler esce subito
        // con g_usb_mode_active=true), quindi rimanda tutto come farebbe il
        // CONFIGREQ handler: caps, relaystate, meshconfig + nomi sensori +
        // force_full_mqtt_resync per i nodi mesh.
        if (!g_usb_mode_active) {
            force_full_mqtt_resync();
            publish_caps();
            publish_relaystate();
            publish_meshconfig();
            publish_mesh_sensor_names();
            publish_sensorcfg();
        }
    }
    was_pressed = pressed;
}

// CFG:UNPAIR;node=N  — scollega il companion dalla lampada N
static void cfg_unpair(const char *line) {
    int ni = -1;
    if (!cfg_kv_int(line, "node", &ni)) { cfg_err("UNPAIR", "parametro node mancante"); return; }
    if (ni < 0 || ni >= node_count || nodes[ni].is_switch || !nodes[ni].configured) {
        cfg_err("UNPAIR", "indice lampada non valido"); return;
    }
    if (config_busy || pair_active) { cfg_busy("UNPAIR"); return; }
    // Invia DELETE al vendor model della lampada
    unpair_send_delete(nodes[ni].unicast);
    // Azzera PUB_SET dei due CLI del companion (pub_addr = 0x0000 = unassigned)
    // Usiamo la build_pair_pub_queue con addr 0 e pair_dst = nodes[ni].unicast
    pair_dst          = nodes[ni].unicast;
    pair_target_elem  = nodes[ni].companion_lamp_elem;
    build_pair_pub_queue(ESP_BLE_MESH_ADDR_UNASSIGNED);
    bind_idx    = 0;
    pair_stage  = 2;   // salta il bind vendor, vai diretto ai PUB_SET
    pair_retry  = 0;
    pair_is_unpair = true;
    pair_active = true;
    config_busy = true;
    send_next_bind(pair_dst);
    // Deregistra lo stato paired
    nodes[ni].companion_paired    = false;
    nodes[ni].companion_lamp_elem = 0;
    ESP_LOGI(TAG, "Unpair nodo #%d avviato.", ni);
    cfg_ok("UNPAIR");
}

static void pair_start_bind(void)
{
    pair_stage = 1;   // bind AppKey al vendor model (serve per inviare il SET)
    ESP_LOGI(TAG, "Companion: bind AppKey al vendor 0x%04x/0x%04x @ 0x%04x...",
             CID_SILVAIR, SILVAIR_ENOCEAN_MID, pair_dst);
    esp_ble_mesh_client_common_param_t  common    = {0};
    esp_ble_mesh_cfg_client_set_state_t set_state = {0};
    set_msg_common(&common, pair_dst, config_client.model,
                   ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
    set_state.model_app_bind.element_addr  = pair_dst;
    set_state.model_app_bind.model_id      = SILVAIR_ENOCEAN_MID;
    set_state.model_app_bind.company_id    = CID_SILVAIR;
    set_state.model_app_bind.model_app_idx = 0;
    esp_ble_mesh_config_client_set_state(&common, &set_state);
}

// ============================================================
// Polling stato: rilegge ON/OFF + Level di una lampada per tick
// (round-robin) così la UI riflette anche i cambi fatti dal pulsante.
// ============================================================
static esp_timer_handle_t poll_timer;
static uint8_t            poll_idx        = 0;
// PROVATO E SCARTATO: POLL_BATCH_SIZE = MAX_NODES (tutti i nodi ogni 2s).
// Il client mesh accetta una sola richiesta in volo per destinazione: con
// tutti i GET sparati nello stesso tick, le risposte non arrivano in tempo
// prima del giro successivo, risultato "Busy sending message" a raffica,
// LEVEL GET che torna -32768 per errore, comandi di gruppo in NoMatchAppKey.
// Compromesso: batch piu' ampio del round-robin originale (era 2) ma non
// totale, per restare dentro la capacita' reale del radio.
// Tornato a 3: il problema precedente (batch=3 → 6 richieste in volo, "Busy
// sending message") nasceva dal fatto che ogni lampada generava Level GET +
// Sensor GET nello stesso tick. Ora Level e Sensor sono su tick ALTERNATI
// (poll_phase_sensor) e i NODE_SENSOR (PIR/lux) sono usciti dal batch e
// vengono interrogati separatamente ogni tick. Risultato: batch=3 produce
// solo 3 richieste per tick dalle lampade + 2 dai sensori mesh = 5 totali,
// ben dentro la capacita' del radio (era il doppio prima del refactoring).
// Con 6 lampade e batch=3: ciclo completo in 2 tick × 2s = 4s (era 6s).
#define POLL_BATCH_SIZE 3

static bool g_mesh_poll_paused; // def. piu' sotto, vicino a mesh_pause_for_ble_scan
static void mqtt_periodic_publish(void); // def. piu' sotto, vicino al ponte MQTT
static void user_btn_poll(void); // def. piu' sotto, vicino a user_btn_init

static void poll_cb(void *arg)
{
    user_btn_poll(); // debounce/long-press BOOT, vedi user_btn_poll() sopra
    mqtt_periodic_publish();

    // Watchdog: se l'handshake BLE di provisioning (PROV_COMPLETE_EVT) non
    // arriva mai - osservato in pratica, vedi conversazione - g_node_
    // provisioning_active restava true per sempre, bloccando la UI su
    // "Provisioning..." e mettendo in pausa tutto il resto a vuoto. 25s e'
    // abbondante (un provisioning riuscito completa in pochi secondi).
    if (g_node_provisioning_active && !config_busy &&
        (esp_timer_get_time() - g_node_provisioning_started_us) > 25000000) {
        ESP_LOGW(TAG, "Watchdog: provisioning bloccato da troppo tempo, sblocco.");
        usb_cdc_send_line("DBG;WATCHDOG provisioning timeout, sblocco");
        // CRITICO: senza questo, lo stack ESP-BLE-MESH restava con un record
        // "in corso" per questo UUID (ADD_DEV_RM_AFTER_PROV_FLAG lo rimuove
        // SOLO dopo un provisioning riuscito, non su un handshake abortito/
        // fallito a meta'). Al tentativo successivo (anche con un device
        // diverso) l'allocatore di indirizzi unicast interno andava in
        // confusione e riassegnava un indirizzo GIA' occupato da un nodo
        // attivo, che il nostro PROV_COMPLETE_EVT poi sovrascriveva - vedi
        // conversazione (BassoSX cancellato da un retry su un device diverso).
        esp_ble_mesh_provisioner_delete_node_with_uuid(g_provisioning_uuid);
        g_node_provisioning_active = false;
    }

    // Watchdog config_busy: se la sequenza di bind/config rimane bloccata
    // (es. callback per nodo sconosciuto -> ni<0 -> early return senza
    // resettare config_busy, o MAX_CFG_RETRY esauriti su un nodo sparito),
    // config_busy resta true per sempre e mqtt_periodic_publish() non pubblica
    // piu' nulla. 45s e' abbondante anche per la sequenza piu' lunga
    // (relay_set + N bind + N pub_set + N sub_add con retry).
    {
        static int64_t config_busy_since_us = 0;
        if (config_busy && !pair_active) {
            if (config_busy_since_us == 0) config_busy_since_us = esp_timer_get_time();
            if (esp_timer_get_time() - config_busy_since_us > 45LL * 1000 * 1000) {
                ESP_LOGW(TAG, "Watchdog config_busy stuck >45s, sblocco forzato.");
                usb_cdc_send_line("DBG;WATCHDOG config_busy stuck, sblocco forzato");
                config_busy     = false;
                configuring_idx = -1;
                config_busy_since_us = 0;
            }
        } else {
            config_busy_since_us = 0; // resetta quando config_busy torna false
        }
    }

    if (g_mesh_poll_paused) return; // finestra di scan BLE classico in corso, vedi mesh_pause_for_ble_scan
    // gateway_is_provisioning() copre anche g_node_provisioning_active (la
    // finestra tra CFG:PROVISION e l'inizio vero della config, prima che
    // config_busy diventi true) - senza, il polling dei nodi gia' connessi
    // continuava a martellare il radio mentre si stava provisionando un
    // nodo nuovo, rallentandone/facendone fallire il provisioning.
    if (gateway_is_provisioning() || node_count == 0) return;

    // Round-robin a blocchi di POLL_BATCH_SIZE nodi per tick (2s): un blocco
    // piu' grande di prima (era 2) accelera il refresh completo, ma non
    // tutti i nodi insieme, altrimenti il radio satura (vedi commento sopra).
    // Provato un tick a 1s per dimezzare i tempi di refresh: il vecchio
    // sospetto (rendeva invisibile il WiFi SoftAP) era in realta' un bug di
    // overflow altrove, gia' risolto - ma a 1s i comandi manuali (onoff/level)
    // restavano in coda dietro il polling molto piu' spesso (richiesta una
    // sola request in volo per destinazione), richiedendo decine di tentativi.
    // Tornato a 2s per questo: la reattivita' dei comandi vale piu' del refresh.
    // Per una lampada con sia Level che Sensor Server, mandare Level GET e
    // Sensor GET nello stesso tick (uno dietro l'altro, senza aspettare la
    // risposta del primo) faceva fallire SEMPRE il secondo - il radio lo
    // trovava ancora occupato dal primo. Risultato: i Sensor GET delle
    // lampade non arrivavano mai a buon fine (100% timeout), anche dopo aver
    // ridotto POLL_BATCH_SIZE. Soluzione: alternare i due tipi di richiesta
    // su tick separati (mai insieme per lo stesso nodo nello stesso giro).

    // Sensori PIR/lux (NODE_SENSOR): interrogati OGNI tick fuori dal batch
    // round-robin, così la latenza MQTT per la rilevazione presenza scende da
    // ~8s (vecchio ciclo round-robin 8 nodi / batch 2) a ~2s (ogni poll_cb).
    // Non contano per POLL_BATCH_SIZE: sono destinazioni diverse dalle lampade
    // e i loro GET non competono con il Level/Sensor delle lampade.
    for (uint8_t i = 0; i < node_count; i++) {
        lamp_node_t *sn = &nodes[i];
        if (!sn->configured || sn->kind != NODE_SENSOR) continue;
        for (uint8_t s = 0; s < sn->sensor_count; s++)
            gateway_read_sensor(sn->unicast + sn->sensor_offsets[s]);
    }

    // Round-robin SOLO per lampade (NODE_SENSOR esclusi): con 6 lampade e
    // batch 2, il ciclo completo e' ora ~6s invece degli 8s precedenti (i
    // 2 slot sensori erano rubati dal round-robin).
    static bool poll_phase_sensor = false;
    uint8_t done = 0;
    for (uint8_t k = 0; k < node_count && done < POLL_BATCH_SIZE; k++) {
        uint8_t i = (poll_idx + k) % node_count;
        lamp_node_t *n = &nodes[i];
        if (n->is_switch || !n->configured || n->kind == NODE_SENSOR) continue;
        if (!poll_phase_sensor) {
            if (n->level_count > 0) {
                // Solo Level GET: per spec Generic OnOff e Generic Level sullo
                // stesso elemento sono "bound" (level>0 implica onoff=1 e
                // viceversa), quindi la GET separata di OnOff era ridondante -
                // generava due messaggi MESH quasi identici per ogni lampada
                // dimmer ad ogni ciclo di poll.
                gateway_read_level_state(n->unicast + n->level_offsets[0]);
            }
        } else {
            // Sensor Server della lampada (elem 1, model 0x1100): potenza/
            // energia. Sul tick "alternato", non sullo stesso del Level GET.
            for (uint8_t s = 0; s < n->sensor_count; s++)
                gateway_read_sensor(n->unicast + n->sensor_offsets[s]);
        }
        done++;
        poll_idx = (i + 1) % node_count;
    }
    poll_phase_sensor = !poll_phase_sensor;
}

// ============================================================
// Timer delay configurazione post-provisioning
// ============================================================
static esp_timer_handle_t config_delay_timer;

static void config_delay_cb(void *arg)
{
    // DEBUG TEMPORANEO: vedi conversazione - il 4 nodo si blocca sempre allo
    // stesso modo (mai nessuna risposta al primo Composition Data Get).
    // Logghiamo ANCHE se questa callback proprio non viene chiamata, o se
    // ritorna subito per uno dei guard, o se la send stessa fallisce: finora
    // non sapevamo nemmeno se il comando veniva davvero spedito sul radio.
    {
        char dbg[120];
        snprintf(dbg, sizeof(dbg), "DBG;config_delay_cb configuring_idx=%d", configuring_idx);
        usb_cdc_send_line(dbg);
    }
    if (configuring_idx < 0) { usb_cdc_send_line("DBG;config_delay_cb: configuring_idx<0, esco"); return; }
    lamp_node_t *node = &nodes[configuring_idx];
    if (node->unicast == ESP_BLE_MESH_ADDR_UNASSIGNED || node->configured) {
        char dbg[120];
        snprintf(dbg, sizeof(dbg), "DBG;config_delay_cb: esco guard unicast=0x%04x configured=%d",
                 node->unicast, node->configured ? 1 : 0);
        usb_cdc_send_line(dbg);
        return;
    }

    cfg_retry = 0;
    ESP_LOGI(TAG, "STEP 1: Composition Data Get a 0x%04x...", node->unicast);
    esp_ble_mesh_client_common_param_t  common    = {0};
    esp_ble_mesh_cfg_client_get_state_t get_state = {0};
    set_msg_common(&common, node->unicast, config_client.model,
                   ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
    get_state.comp_data_get.page = 0;
    esp_err_t err = esp_ble_mesh_config_client_get_state(&common, &get_state);
    char dbg[120];
    snprintf(dbg, sizeof(dbg), "DBG;CompDataGet -> 0x%04x err=0x%x (%s)",
             node->unicast, err, esp_err_to_name(err));
    usb_cdc_send_line(dbg);
}

static void advance_config_queue(void)
{
    int next = find_next_unconfigured();
    if (next >= 0) {
        configuring_idx = next;
        cfg_retry       = 0;
        ESP_LOGI(TAG, "Coda config: passo al nodo #%d...", next);
        esp_timer_start_once(config_delay_timer, 500000);
    } else {
        configuring_idx = -1;
        config_busy     = false;
        // Punto 3: fine sequenza provisioning+config del nodo (o di tutta la
        // coda di rebind) - da qui altre attivita' BLE (scan classico,
        // discovery di nuovi unprovisioned) possono riprendere.
        g_node_provisioning_active = false;
        ESP_LOGI(TAG, "Coda config vuota.");
        save_nodes_nvs();
    }
}

// ============================================================
// CALLBACK PROVISIONING
// ============================================================
static void prov_callback(esp_ble_mesh_prov_cb_event_t event,
                          esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Provisioner abilitato (err %d).",
                 param->provisioner_prov_enable_comp.err_code);
        break;

    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        ESP_LOGI(TAG, "ADD_LOCAL_APP_KEY (err 0x%x, idx 0x%x)",
                 param->provisioner_add_app_key_comp.err_code,
                 param->provisioner_add_app_key_comp.app_idx);
        if (param->provisioner_add_app_key_comp.err_code == ESP_OK) {
            uint16_t idx = param->provisioner_add_app_key_comp.app_idx;
            esp_err_t e;
            e = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
                0x0001, idx, ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, ESP_BLE_MESH_CID_NVAL);
            if (e != ESP_OK) ESP_LOGE(TAG, "Bind OnOff CLI local: %d", e);
            e = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
                0x0001, idx, ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_CLI, ESP_BLE_MESH_CID_NVAL);
            if (e != ESP_OK) ESP_LOGE(TAG, "Bind Level CLI local: %d", e);
            e = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
                0x0001, idx, 0x0000, CID_ESP);
            if (e != ESP_OK) ESP_LOGE(TAG, "Bind vendor local: %d", e);
            e = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
                0x0001, idx, ESP_BLE_MESH_MODEL_ID_SENSOR_CLI, ESP_BLE_MESH_CID_NVAL);
            if (e != ESP_OK) ESP_LOGE(TAG, "Bind Sensor CLI local: %d", e);
        }
        break;

    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "BIND_LOCAL_MODEL (err 0x%x)",
                 param->provisioner_bind_app_key_to_model_comp.err_code);
        break;

    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        // Punto 3 della richiesta utente: mentre un provisioning e' in corso
        // (config_busy/pair_active/g_node_provisioning_active - stessa
        // condizione di gateway_is_provisioning(), definita piu' sotto, qui
        // usata per nome perche' non ancora dichiarata a questo punto del
        // file) non si aggiungono/aggiornano voci in discovered[] - il radio
        // e gli scambi BLE in volo servono solo al nodo che si sta
        // configurando, niente distrazioni su altri beacon unprovisioned nel
        // frattempo. g_node_provisioning_active copre anche la finestra fra
        // CFG:PROVISION e PROV_LINK_CLOSE, prima che config_busy diventi true.
        if (config_busy || pair_active || g_node_provisioning_active) break;

        uint16_t oob_info = param->provisioner_recv_unprov_adv_pkt.oob_info;
        const uint8_t *uuid = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        bool needs_qr_oob = (oob_info & 0x0004);
        ESP_LOGI(TAG, "BEACON: UUID=%02x%02x%02x%02x... oob=0x%04x",
                 uuid[0], uuid[1], uuid[2], uuid[3], oob_info);

        // Rimuovi voci scadute
        int64_t now_us = esp_timer_get_time();
        uint8_t dj = 0;
        for (uint8_t di = 0; di < discovered_count; di++) {
            if (now_us - discovered[di].last_seen_us < DISCOVERED_EXPIRE_US)
                discovered[dj++] = discovered[di];
        }
        discovered_count = dj;

        // Aggiorna o aggiungi alla lista
        int found_di = -1;
        for (uint8_t di = 0; di < discovered_count; di++) {
            if (memcmp(discovered[di].uuid, uuid, 16) == 0) { found_di = (int)di; break; }
        }
        if (found_di < 0 && discovered_count < MAX_DISCOVERED) {
            found_di = (int)discovered_count++;
            memcpy(discovered[found_di].uuid, uuid, 16);
            memcpy(discovered[found_di].addr,
                   param->provisioner_recv_unprov_adv_pkt.addr, 6);
            discovered[found_di].addr_type    = param->provisioner_recv_unprov_adv_pkt.addr_type;
            discovered[found_di].bearer       = param->provisioner_recv_unprov_adv_pkt.bearer;
            discovered[found_di].oob_info     = oob_info;
            discovered[found_di].needs_qr_oob = needs_qr_oob;
        }
        if (found_di >= 0) {
            discovered[found_di].rssi         = param->provisioner_recv_unprov_adv_pkt.rssi;
            discovered[found_di].last_seen_us = now_us;
        }
        break;
    }

    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        uint16_t uaddr = param->provisioner_prov_complete.unicast_addr;
        uint8_t  elem  = param->provisioner_prov_complete.element_num;
        // ATTENZIONE: qui serve sapere se QUESTO indirizzo BASE corrisponde
        // gia' a un nodo noto, non "a quale nodo appartiene questo indirizzo
        // di un elemento" - find_node_by_addr() fa match per INTERVALLO
        // (serve altrove, per risalire al nodo da un elemento secondario nei
        // callback di config) e qui causava un bug serio: se il nuovo
        // indirizzo assegnato dal provisioner cadeva dentro l'intervallo di
        // un nodo multi-elemento gia' esistente (es. base 0x0005 con 4
        // elementi = intervallo 0x0005-0x0008), un dispositivo NUOVO e
        // diverso veniva scambiato per quello vecchio e ne sovrascriveva i
        // dati (lampade gia' configurate "cancellate" da un provisioning
        // successivo). Qui serve un confronto ESATTO sull'indirizzo base.
        int ni = -1;
        for (uint8_t i = 0; i < node_count; i++) {
            if (nodes[i].unicast == uaddr) { ni = i; break; }
        }
        // DEBUG TEMPORANEO (vedi conversazione: sovrascrittura nodi durante
        // provisioning) - anche su USB CDC cosi' e' visibile dalla PWA senza
        // bisogno del monitor seriale di sistema in parallelo. Rimuovere dopo
        // la diagnosi.
        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg),
                     "DBG;PROV_COMPLETE uaddr=0x%04x elem=%d match_idx=%d node_count=%d",
                     uaddr, elem, ni, node_count);
            usb_cdc_send_line(dbg);
        }
        if (ni < 0) {
            if (node_count >= MAX_NODES) {
                ESP_LOGW(TAG, "Limite %d nodi raggiunto.", MAX_NODES);
                break;
            }
            ni = node_count++;
        } else {
            char dbg[160];
            snprintf(dbg, sizeof(dbg),
                     "DBG;ATTENZIONE riuso slot #%d (vecchio unicast=0x%04x) per nuovo nodo 0x%04x",
                     ni, nodes[ni].unicast, uaddr);
            usb_cdc_send_line(dbg);
        }
        nodes[ni].unicast     = uaddr;
        nodes[ni].elem_num    = elem;
        nodes[ni].configured  = false;
        nodes[ni].failed      = false;
        nodes[ni].onoff_count = 0;
        nodes[ni].level_count = 0;
        nodes[ni].cli_count   = 0;
        nodes[ni].is_switch   = false;
        memcpy(nodes[ni].uuid, g_provisioning_uuid, 16); // vedi nota sul campo uuid in lamp_node_t
        ESP_LOGI(TAG, "Nodo #%d provisionato: 0x%04x (%d elem). In coda...",
                 ni, uaddr, elem);

        // NOTA: avviare la config da qui (subito dopo PROV_COMPLETE_EVT, con
        // solo 1s di margine) era un tentativo per aggirare un PROV_LINK_CLOSE_EVT
        // che SEMBRAVA non scattare mai in alcuni test - poi rivelatosi falso:
        // l'evento arriva regolarmente (vedi DBG;PROV_LINK_CLOSE nei log). Il
        // problema reale era altrove: partire troppo presto, prima che il nodo
        // abbia finito di assestarsi dopo la chiusura del bearer di provisioning
        // (passaggio da bearer di provisioning a bearer di rete), causava
        // fallimenti consistenti proprio sul primo Composition Data Get -
        // pattern osservato: sempre il nodo N+1-esimo, indipendente dal device
        // fisico. Riportato al comportamento della versione funzionante
        // precedente (main_grafica.c, WiFi): si avvia SOLO da
        // PROV_LINK_CLOSE_EVT, con 3s di margine. Qui ci si limita ad
        // aggiungere/aggiornare il nodo.
        break;
    }

    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT: {
        // DEBUG TEMPORANEO: vedi conversazione - un nodo provisionato restava
        // "Non ancora configurato" per sempre, senza che la sequenza di
        // config (Composition Data Get -> AppKey -> bind) partisse mai.
        // Sospetto: PROV_LINK_CLOSE_EVT non scatta per quel link, oppure
        // scatta con config_busy gia' true (e quindi salta tutto, vedi
        // "if (!config_busy)" sotto) - questa riga mostra entrambi i casi.
        {
            char dbg[100];
            snprintf(dbg, sizeof(dbg), "DBG;PROV_LINK_CLOSE config_busy=%d g_node_prov_active=%d node_count=%d",
                     config_busy ? 1 : 0, g_node_provisioning_active ? 1 : 0, node_count);
            usb_cdc_send_line(dbg);
        }
        if (!config_busy) {
            int next = find_next_unconfigured();
            { char dbg[48]; snprintf(dbg, sizeof(dbg), "DBG;find_next_unconfigured -> %d", next); usb_cdc_send_line(dbg); }
            if (next >= 0) {
                configuring_idx = next;
                config_busy     = true;
                // NON spegniamo piu' il WiFi qui: l'utente perdeva la connessione
                // alla pagina web ogni volta che un nodo veniva configurato. Il
                // guard "config_busy" sulle altre route (/pair, /provision, /rebind,
                // ecc.) gia' impedisce di lanciare altre azioni mentre il bind e'
                // in corso, che e' l'unica cosa che serve davvero proteggere.
                ESP_LOGI(TAG, "Link chiuso. Configuro nodo #%d (WiFi resta come da pulsante)...", next);
                esp_err_t terr = esp_timer_start_once(config_delay_timer, 3000000);
                if (terr != ESP_OK) {
                    char dbg[80];
                    snprintf(dbg, sizeof(dbg), "DBG;ATTENZIONE esp_timer_start_once FALLITO err=0x%x (%s)",
                             terr, esp_err_to_name(terr));
                    usb_cdc_send_line(dbg);
                }
            } else if (g_node_provisioning_active) {
                // Punto 3: link chiuso senza che PROV_COMPLETE_EVT abbia mai
                // aggiunto un nodo da configurare (handshake di provisioning
                // fallito/annullato, es. OOB sbagliato o timeout) - niente da
                // mettere in coda, quindi advance_config_queue() non scattera'
                // mai a sbloccare il guard. Sblocchiamo qui, altrimenti
                // gateway_is_provisioning() resterebbe vero per sempre.
                // Stesso motivo del watchdog in poll_cb: senza ripulire anche
                // lato stack (delete_node_with_uuid), un retry successivo
                // rischia un riassegnamento di indirizzo unicast in conflitto
                // con un nodo gia' attivo (vedi conversazione).
                esp_ble_mesh_provisioner_delete_node_with_uuid(g_provisioning_uuid);
                g_node_provisioning_active = false;
                ESP_LOGW(TAG, "Link chiuso senza provisioning completato, sblocco scan BLE.");
            }
        }
        break;
    }

    default:
        break;
    }
}

// ============================================================
// CALLBACK CONFIG CLIENT
// ============================================================
static bool is_bind_op(uint32_t op)
{
    return op == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND   ||
           op == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET    ||
           op == ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD    ||
           op == ESP_BLE_MESH_MODEL_OP_MODEL_SUB_DELETE ||
           op == ESP_BLE_MESH_MODEL_OP_RELAY_SET;
}

static void config_client_callback(esp_ble_mesh_cfg_client_cb_event_t event,
                                   esp_ble_mesh_cfg_client_cb_param_t *param)
{
    uint16_t dst_node = param->params->ctx.addr;
    uint32_t opcode   = param->params->opcode;

    // DEBUG TEMPORANEO: vedi conversazione - serve sapere se TIMEOUT_EVT
    // scatta MAI per il nodo bloccato (finora non loggato, possibile punto
    // cieco: se ni<0 qui sotto si esce con un log che NON va su USB CDC).
    {
        char dbg[100];
        snprintf(dbg, sizeof(dbg), "DBG;config_client_callback event=%d dst=0x%04x opcode=0x%08lx",
                 (int)event, dst_node, (unsigned long)opcode);
        usb_cdc_send_line(dbg);
    }

    int ni = find_node_by_addr(dst_node);
    if (ni < 0) {
        ESP_LOGW(TAG, "Config callback per nodo sconosciuto 0x%04x.", dst_node);
        usb_cdc_send_line("DBG;ATTENZIONE ni<0, nodo sconosciuto, esco");
        // Se config_busy e' true ma non sappiamo piu' a quale nodo si riferisce
        // (es. watchdog di provisioning ha cancellato il nodo, o il nodo e'
        // stato rimosso tra l'invio e la risposta), config_busy resterebbe true
        // per sempre fermando mqtt_periodic_publish indefinitamente. Avanzare
        // la coda risolve: o trova il prossimo nodo da configurare, o svuota
        // la coda e resetta config_busy.
        if (config_busy && !pair_active) advance_config_queue();
        return;
    }
    lamp_node_t *node = &nodes[ni];

    switch (event) {
    case ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT:
        if (opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) {
            parse_composition_data(param->status_cb.comp_data_status.composition_data, node);
            cfg_retry = 0;
            ESP_LOGI(TAG, "STEP 2: AppKey Add a 0x%04x...", dst_node);
            esp_ble_mesh_client_common_param_t  common    = {0};
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            set_msg_common(&common, dst_node, config_client.model,
                           ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set_state.app_key_add.net_idx = 0;
            set_state.app_key_add.app_idx = 0;
            memcpy(set_state.app_key_add.app_key, fixed_app_key, 16);
            esp_ble_mesh_config_client_set_state(&common, &set_state);
            // Il Relay Set (abilita il Relay feature su questo nodo, vedi
            // BIND_RELAY_SET) NON va piu' mandato qui fire-and-forget: il
            // config client BLE Mesh gestisce una transazione alla volta, due
            // messaggi consecutivi senza aspettare risposta si scontravano
            // silenziosamente (osservato: lo stesso nodo si bloccava sempre
            // allo stesso modo). Ora e' il primo elemento della bind queue
            // sequenziale, vedi build_bind_queue()/send_next_bind().
        }
        break;

    case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
        if (pair_active && dst_node == pair_dst) {
            if (pair_stage == 1 && opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
                ESP_LOGI(TAG, "Companion: vendor bindato, invio SET + configuro pubblicazione...");
                pair_retry = 0;
                pair_send_set();
                build_pair_pub_queue(pair_target_elem);  // usa la variabile globale salvata in pair_handler
                bind_idx   = 0;
                pair_stage = 2;
                if (bind_queue_len) {
                    send_next_bind(pair_dst);
                } else {
                    pair_active = false;
                    config_busy = false;
                }
                break;
            }
            if (pair_stage == 2 && is_bind_op(opcode)) {
                pair_retry = 0;
                bind_idx++;
                if (bind_idx < bind_queue_len) {
                    send_next_bind(pair_dst);
                } else {
                    int lamp_ni = find_node_by_addr(pair_target_elem);
                    if (lamp_ni < 0) lamp_ni = find_node_by_addr(pair_dst); // fallback elem0
                    if (pair_is_unpair) {
                        ESP_LOGI(TAG, "--- COMPANION SCOLLEGATO ---");
                        if (lamp_ni >= 0) {
                            nodes[lamp_ni].companion_paired    = false;
                            nodes[lamp_ni].companion_lamp_elem = 0;
                        }
                    } else {
                        ESP_LOGI(TAG, "--- COMPANION ABBINATO (OnOff CLI -> 0x%04x) ---", GROUP_ALL);
                        if (lamp_ni >= 0) {
                            nodes[lamp_ni].companion_paired    = true;
                            nodes[lamp_ni].companion_lamp_elem = pair_target_elem;
                        }
                    }
                    pair_active  = false;
                    config_busy  = false;
                }
                break;
            }
        }
        // Il dispositivo puo' rispondere con uno status di errore (es. rifiuta
        // il bind su un elemento specifico) pur mandando comunque una risposta
        // valida - prima di questo controllo, qualunque risposta ricevuta
        // veniva trattata come successo, e il nodo finiva segnato "configurato"
        // anche con un modello NON bindato all'AppKey: i comandi venivano poi
        // mandati ma il dispositivo li scartava in silenzio (nessun errore
        // visibile lato gateway). Diagnosticato su un nodo NCL DALI 32conv ELT
        // col Generic OnOff Server sull'elemento #0 (visibile/controllabile da
        // nRF Mesh ma non dalla nostra PWA dopo il nostro provisioning).
        uint8_t bind_status = 0;
        if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            bind_status = param->status_cb.appkey_status.status;
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            bind_status = param->status_cb.model_app_status.status;
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
            bind_status = param->status_cb.model_pub_status.status;
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD ||
                   opcode == ESP_BLE_MESH_MODEL_OP_MODEL_SUB_DELETE) {
            bind_status = param->status_cb.model_sub_status.status;
        }
        if (bind_status != 0 && (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD || is_bind_op(opcode))) {
            ESP_LOGE(TAG, "Config FALLITA per nodo #%d (0x%04x): opcode=0x%08lx status=0x%02x "
                     "(il dispositivo ha rifiutato il bind, il modello NON e' utilizzabile)",
                     ni, dst_node, (unsigned long)opcode, bind_status);
            node->failed = true;
            // Non interrompiamo la coda: proseguiamo comunque sugli altri
            // elementi/modelli, cosi' un singolo bind rifiutato non blocca il
            // resto della configurazione del nodo.
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "DBG;BIND_FALLITO nodo=#%d addr=0x%04x opcode=0x%08lx status=0x%02x",
                     ni, dst_node, (unsigned long)opcode, bind_status);
            usb_cdc_send_line(dbg);
        }

        // DEBUG TEMPORANEO: traccia ogni step di config/bind per capire quale
        // nodo (ni/indirizzo) sta venendo aggiornato in ogni momento - vedi
        // conversazione sovrascrittura nodi. Rimuovere dopo la diagnosi.
        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "DBG;CFGCB ni=%d addr=0x%04x opcode=0x%08lx event=%d bind_idx=%d/%d",
                     ni, dst_node, (unsigned long)opcode, (int)event, bind_idx, bind_queue_len);
            usb_cdc_send_line(dbg);
        }

        if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            cfg_retry = 0;
            build_bind_queue(node, dst_node);
            bind_idx = 0;
            send_next_bind(dst_node);

        } else if (is_bind_op(opcode)) {
            bind_idx++;
            cfg_retry = 0;
            if (bind_idx < bind_queue_len) {
                send_next_bind(dst_node);
            } else {
                node->configured = true;
                if (!node->is_switch) group_bound[ni] = true;
                ESP_LOGI(TAG, "--- DISPOSITIVO #%d CONFIGURATO (OnOff:%d Level:%d Sensor:%d, default %s) ---",
                         ni, node->onoff_count, node->level_count, node->sensor_count,
                         node->kind == NODE_SENSOR ? "SENSORE" : "LAMPADA");
                if (node->kind == NODE_SENSOR) {
                    for (uint8_t k = 0; k < node->sensor_count; k++)
                        gateway_read_sensor(node->unicast + node->sensor_offsets[k]);
                } else {
                    for (uint8_t k = 0; k < node->onoff_count; k++)
                        gateway_read_onoff_state(node->unicast + node->onoff_offsets[k]);
                    for (uint8_t k = 0; k < node->level_count; k++)
                        gateway_read_level_state(node->unicast + node->level_offsets[k]);
                }
                advance_config_queue();
            }
        }
        break;

    case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
        if (pair_active && dst_node == pair_dst) {
            if (++pair_retry > 3) {
                ESP_LOGE(TAG, "Companion: timeout, abbandono pairing.");
                pair_active = false;
                config_busy = false;
                break;
            }
            ESP_LOGW(TAG, "Companion: timeout stage %d (%d/3), ritento...", pair_stage, pair_retry);
            if (pair_stage == 1) pair_start_bind();
            else                 send_next_bind(pair_dst);
            break;
        }
        if (++cfg_retry > MAX_CFG_RETRY) {
            ESP_LOGE(TAG, "Nodo #%d: troppi timeout, abbandono.", ni);
            node->failed = true;
            advance_config_queue();
            break;
        }
        ESP_LOGW(TAG, "Timeout 0x%" PRIx32 " (%d/%d), retry...",
                 opcode, cfg_retry, MAX_CFG_RETRY);

        if (opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) {
            esp_ble_mesh_client_common_param_t  common    = {0};
            esp_ble_mesh_cfg_client_get_state_t get_state = {0};
            set_msg_common(&common, dst_node, config_client.model,
                           ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
            get_state.comp_data_get.page = 0;
            esp_ble_mesh_config_client_get_state(&common, &get_state);
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            esp_ble_mesh_client_common_param_t  common    = {0};
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            set_msg_common(&common, dst_node, config_client.model,
                           ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set_state.app_key_add.net_idx = 0;
            set_state.app_key_add.app_idx = 0;
            memcpy(set_state.app_key_add.app_key, fixed_app_key, 16);
            esp_ble_mesh_config_client_set_state(&common, &set_state);
        } else if (is_bind_op(opcode)) {
            send_next_bind(dst_node);
        } else {
            ESP_LOGE(TAG, "Timeout non gestito, abbandono.");
            node->failed = true;
            advance_config_queue();
        }
        break;

    default:
        break;
    }
}

// ============================================================
// CALLBACK GENERIC CLIENT (OnOff + Level)
// ============================================================
static void generic_client_callback(esp_ble_mesh_generic_client_cb_event_t event,
                                    esp_ble_mesh_generic_client_cb_param_t *param)
{
    uint16_t node_addr = param->params->ctx.addr;
    uint32_t recv_op   = param->params->ctx.recv_op;

    if (event != ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT &&
        event != ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT) return;

    if (recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
        uint8_t new_state = param->status_cb.onoff_status.op_en
                            ? param->status_cb.onoff_status.target_onoff
                            : param->status_cb.onoff_status.present_onoff;
        int ni = find_node_by_addr(node_addr);
        if (ni >= 0) {
            lamp_node_t *node = &nodes[ni];
            for (uint8_t i = 0; i < node->onoff_count; i++) {
                if (node_addr == (uint16_t)(node->unicast + node->onoff_offsets[i])) {
                    node->onoff_states[i] = new_state;
                    uart_push_onoff(node_addr, new_state);
                    break;
                }
            }
        }
        ESP_LOGI(TAG, "[ONOFF %s] 0x%04X -> %s",
                 event == ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT ? "GET" : "SET",
                 node_addr, new_state ? "ON" : "OFF");

    } else if (recv_op == ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_STATUS) {
        int16_t new_level = param->status_cb.level_status.op_en
                            ? param->status_cb.level_status.target_level
                            : param->status_cb.level_status.present_level;
        int ni = find_node_by_addr(node_addr);
        if (ni >= 0) {
            lamp_node_t *node = &nodes[ni];
            for (uint8_t i = 0; i < node->level_count; i++) {
                if (node_addr == (uint16_t)(node->unicast + node->level_offsets[i])) {
                    node->level_states[i] = new_level;
                    uart_push_level(node_addr, new_level);
                    // DALI: arc power resta non-zero (es. 66%) anche quando la lampada
                    // e' logicamente spenta via OnOff OFF. Derivare onoff da level>0
                    // sovrascriveva lo stato ottimistico onoff=0 impostato dal comando
                    // dell'utente con onoff=1 (lampada "accesa" perche' arc power>0).
                    // Fix: deriviamo onoff dal level SOLO in direzione on (level>0 →
                    // onoff=1), mai in direzione off (lasciamo l'ultimo stato esplicito).
                    if (new_level > 0) {
                        for (uint8_t j = 0; j < node->onoff_count; j++) {
                            if (node->onoff_offsets[j] == node->level_offsets[i]) {
                                if (node->onoff_states[j] != 1) {
                                    node->onoff_states[j] = 1;
                                    uart_push_onoff(node_addr, 1);
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }
        int pct = (new_level <= 0) ? 0 : (int)((int32_t)new_level * 100 / 32767);
        ESP_LOGI(TAG, "[LEVEL %s] 0x%04X -> %d (%d%%)",
                 event == ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT ? "GET" : "SET",
                 node_addr, new_level, pct);
    }
}

// ============================================================
// CALLBACK VENDOR MODEL (STATUS di ritorno dal companion proxy)
// ============================================================
static void custom_model_callback(esp_ble_mesh_model_cb_event_t event,
                                  esp_ble_mesh_model_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT:
        ESP_LOGI(TAG, "[VND RECV] op=0x%06" PRIx32 " src=0x%04x len=%d",
                 param->model_operation.opcode,
                 param->model_operation.ctx->addr,
                 param->model_operation.length);
        if (param->model_operation.length)
            ESP_LOG_BUFFER_HEX(TAG, param->model_operation.msg,
                               param->model_operation.length);
        break;
    case ESP_BLE_MESH_MODEL_SEND_COMP_EVT:
        ESP_LOGI(TAG, "[VND SEND] op=0x%06" PRIx32 " err=%d",
                 param->model_send_comp.opcode, param->model_send_comp.err_code);
        break;
    default:
        break;
    }
}

// ============================================================
// ONOFF SET / LEVEL SET
// ============================================================
static void send_onoff_elem(uint8_t node_idx, uint8_t elem_idx, uint8_t onoff)
{
    if (node_idx >= node_count) return;
    lamp_node_t *node = &nodes[node_idx];
    if (elem_idx >= node->onoff_count) return;

    uint16_t elem_addr = node->unicast + node->onoff_offsets[elem_idx];

    // Accendi con livello 0%: la lampada risponde "sono accesa" ma il DALI
    // rimane a 0 di arco, fisicamente buia. Se c'e' un elemento Level sullo
    // stesso offset con level=0, ripristina al 50% prima del comando OnOff.
    // NON mandare Level SET -32768 per lo spegnimento: il modulo BLE della
    // lampada e' alimentato dalla linea DALI e arc power=0 ne causa lo
    // spegnimento, impedendo ai comandi successivi di raggiungere il nodo.
    if (onoff == 1) {
        uint8_t off = node->onoff_offsets[elem_idx];
        for (uint8_t l = 0; l < node->level_count; l++) {
            if (node->level_offsets[l] == off && node->level_states[l] <= 0) {
                send_level_elem(node_idx, l, 16383); // ~50%
                break;
            }
        }
    }

    ESP_LOGI(TAG, "OnOff SET %d -> nodo #%d elem #%d (0x%04x)",
             onoff, node_idx, elem_idx, elem_addr);

    // SET_UNACK: niente attesa di Status dal nodo (fino a 8s di msg_timeout
    // per ogni comando con la versione acked) - lo stato lo pubblichiamo
    // subito noi stessi, in modo ottimistico, com'e' coerente con quanto
    // gia' fa Manager.py per i rele' classici (LAST_RELAY_STATUS forzato).
    esp_ble_mesh_client_common_param_t      common    = {0};
    esp_ble_mesh_generic_client_set_state_t set_state = {0};
    set_msg_common(&common, elem_addr, onoff_client.model,
                   ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK);
    set_state.onoff_set.op_en = false;
    set_state.onoff_set.onoff = onoff;
    set_state.onoff_set.tid   = esp_random() % 255;
    esp_ble_mesh_generic_client_set_state(&common, &set_state);
    node->onoff_states[elem_idx] = onoff;
    uart_push_onoff(elem_addr, onoff);

    // Aggiorna s_cmd_level_val sull'elemento level abbinato (stesso offset)
    // cosi' la detection MESHOVERRIDE nel poll puo' distinguere un cambio
    // dovuto a noi da uno dovuto a un companion che cambia solo l'OnOff
    // senza toccare il Level (comportamento tipico DALI: arc power invariato).
    {
        uint8_t off = node->onoff_offsets[elem_idx];
        for (uint8_t l = 0; l < node->level_count; l++) {
            if (node->level_offsets[l] != off) continue;
            s_cmd_level_valid[node_idx][l] = true;
            if (onoff == 0)
                s_cmd_level_val[node_idx][l] = 0;
            else if (s_cmd_level_val[node_idx][l] <= 0)
                s_cmd_level_val[node_idx][l] = 1; // sentinella: "intendevamo ON"
            break;
        }
    }
}

// trans_time/delay come da spec Generic Level Set: trans_time codifica step*risoluzione
// (vedi ESP_BLE_MESH_GET_TRANSITION_TIME), delay in unita' di 5ms. 0,0 = nessuna
// transizione/ritardo (comportamento storico, op_en resta false in quel caso).
static void send_level_elem_ex(uint8_t node_idx, uint8_t level_idx, int16_t level_val,
                                uint8_t trans_time, uint8_t delay)
{
    if (node_idx >= node_count) return;
    lamp_node_t *node = &nodes[node_idx];
    if (level_idx >= node->level_count) return;

    uint16_t elem_addr = node->unicast + node->level_offsets[level_idx];
    int pct = (level_val <= 0) ? 0 : (int)((int32_t)level_val * 100 / 32767);
    ESP_LOGI(TAG, "Level SET %d (%d%%) trans=%d delay=%d -> nodo #%d lvl #%d (0x%04x)",
             level_val, pct, trans_time, delay, node_idx, level_idx, elem_addr);

    // SET_UNACK: stesso discorso di send_onoff_elem() - niente attesa di
    // Status, stato pubblicato subito in modo ottimistico.
    esp_ble_mesh_client_common_param_t      common    = {0};
    esp_ble_mesh_generic_client_set_state_t set_state = {0};
    set_msg_common(&common, elem_addr, level_client.model,
                   ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_SET_UNACK);
    set_state.level_set.op_en     = (trans_time != 0 || delay != 0);
    set_state.level_set.level     = level_val;
    set_state.level_set.tid       = esp_random() % 255;
    set_state.level_set.trans_time = trans_time;
    set_state.level_set.delay      = delay;
    esp_ble_mesh_generic_client_set_state(&common, &set_state);
    node->level_states[level_idx] = level_val;
    uart_push_level(elem_addr, level_val);
    s_cmd_level_valid[node_idx][level_idx] = true;
    s_cmd_level_val  [node_idx][level_idx] = level_val;
}

static void send_level_elem(uint8_t node_idx, uint8_t level_idx, int16_t level_val)
{
    send_level_elem_ex(node_idx, level_idx, level_val, 0, 0);
}

// ============================================================
// ONOFF/LEVEL SET DI GRUPPO — una sola trasmissione radio raggiunge tutte
// le lampade sottoscritte a GROUP_ALL (vedi build_bind_queue: ogni lampada
// ci si iscrive in automatico durante la configurazione), invece di N
// trasmissioni unicast sequenziali una dopo l'altra.
// ============================================================
static void send_onoff_group(uint16_t group_addr, uint8_t onoff)
{
    ESP_LOGI(TAG, "OnOff SET %d -> gruppo 0x%04x (broadcast)", onoff, group_addr);

    esp_ble_mesh_client_common_param_t      common    = {0};
    esp_ble_mesh_generic_client_set_state_t set_state = {0};
    set_msg_common(&common, group_addr, onoff_client.model,
                   ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK);
    set_state.onoff_set.op_en = false;
    set_state.onoff_set.onoff = onoff;
    set_state.onoff_set.tid   = esp_random() % 255;
    esp_ble_mesh_generic_client_set_state(&common, &set_state);

    // Bookkeeping locale + UART per ogni lampada (solo per tenere sincronizzati
    // stato/UI lato Pi: la trasmissione radio sopra e' stata UNA sola).
    for (uint8_t i = 0; i < node_count; i++) {
        lamp_node_t *n = &nodes[i];
        if (n->is_switch || !n->configured) continue;
        for (uint8_t e = 0; e < n->onoff_count; e++) {
            n->onoff_states[e] = onoff;
            uart_push_onoff((uint16_t)(n->unicast + n->onoff_offsets[e]), onoff);
        }
        // Aggiorna s_cmd_level_val per tutti gli elementi level: serve alla
        // detection MESHOVERRIDE per distinguere un onoff inviato da noi
        // (nessun mismatch) da uno inviato dal companion (mismatch -> MESHOVERRIDE).
        for (uint8_t e = 0; e < n->level_count; e++) {
            s_cmd_level_valid[i][e] = true;
            if (onoff == 0)
                s_cmd_level_val[i][e] = 0;
            else if (s_cmd_level_val[i][e] <= 0)
                s_cmd_level_val[i][e] = 1; // sentinella: "intendevamo ON"
        }
    }
}

static void send_level_group_ex(uint16_t group_addr, int16_t level_val,
                                 uint8_t trans_time, uint8_t delay)
{
    int pct = (level_val <= 0) ? 0 : (int)((int32_t)level_val * 100 / 32767);
    ESP_LOGI(TAG, "Level SET %d (%d%%) trans=%d delay=%d -> gruppo 0x%04x (broadcast)",
             level_val, pct, trans_time, delay, group_addr);

    esp_ble_mesh_client_common_param_t      common    = {0};
    esp_ble_mesh_generic_client_set_state_t set_state = {0};
    set_msg_common(&common, group_addr, level_client.model,
                   ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_SET_UNACK);
    set_state.level_set.op_en      = (trans_time != 0 || delay != 0);
    set_state.level_set.level      = level_val;
    set_state.level_set.tid        = esp_random() % 255;
    set_state.level_set.trans_time = trans_time;
    set_state.level_set.delay      = delay;
    esp_ble_mesh_generic_client_set_state(&common, &set_state);

    for (uint8_t i = 0; i < node_count; i++) {
        lamp_node_t *n = &nodes[i];
        if (n->is_switch || !n->configured) continue;
        for (uint8_t e = 0; e < n->level_count; e++) {
            n->level_states[e] = level_val;
            uart_push_level((uint16_t)(n->unicast + n->level_offsets[e]), level_val);
            s_cmd_level_valid[i][e] = true;
            s_cmd_level_val  [i][e] = level_val;
        }
    }
}

static void send_level_group(uint16_t group_addr, int16_t level_val)
{
    send_level_group_ex(group_addr, level_val, 0, 0);
}

// ============================================================
// GET HELPERS
// ============================================================
static void gateway_read_onoff_state(uint16_t target_addr)
{
    esp_ble_mesh_client_common_param_t      common    = {0};
    esp_ble_mesh_generic_client_get_state_t get_state = {0};
    common.opcode       = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET;
    common.model        = onoff_client.model;
    common.ctx.net_idx  = 0;
    common.ctx.app_idx  = 0;
    common.ctx.addr     = target_addr;
    common.ctx.send_ttl = 7;
    common.msg_timeout  = 2000;
    esp_err_t err = esp_ble_mesh_generic_client_get_state(&common, &get_state);
    if (err != ESP_OK) ESP_LOGE(TAG, "GET OnOff 0x%04X err %d", target_addr, err);
}

static void gateway_read_level_state(uint16_t target_addr)
{
    esp_ble_mesh_client_common_param_t      common    = {0};
    esp_ble_mesh_generic_client_get_state_t get_state = {0};
    common.opcode       = ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_GET;
    common.model        = level_client.model;
    common.ctx.net_idx  = 0;
    common.ctx.app_idx  = 0;
    common.ctx.addr     = target_addr;
    common.ctx.send_ttl = 7;
    common.msg_timeout  = 2000;
    esp_err_t err = esp_ble_mesh_generic_client_get_state(&common, &get_state);
    if (err != ESP_OK) ESP_LOGE(TAG, "GET Level 0x%04X err %d", target_addr, err);
    else ESP_LOGI(TAG, "GET Level inviato a 0x%04X", target_addr);
}

// Sensor Get (tutte le property dell'elemento): op_en=false
static void gateway_read_sensor(uint16_t target_addr)
{
    esp_ble_mesh_client_common_param_t     common    = {0};
    esp_ble_mesh_sensor_client_get_state_t get_state = {0};
    common.opcode       = ESP_BLE_MESH_MODEL_OP_SENSOR_GET;
    common.model        = sensor_client.model;
    common.ctx.net_idx  = 0;
    common.ctx.app_idx  = 0;
    common.ctx.addr     = target_addr;
    common.ctx.send_ttl = 7;
    common.msg_timeout  = 2000;
    get_state.sensor_get.op_en = false;   // nessuna property -> ritorna tutto
    esp_err_t err = esp_ble_mesh_sensor_client_get_state(&common, &get_state);
    if (err != ESP_OK) ESP_LOGE(TAG, "GET Sensor 0x%04X err %d", target_addr, err);
}

// Combina i valori noti tra tutti gli elementi sensore del nodo: ogni
// elemento puo' riportare presenza, luce, o entrambe (dipende dal device),
// quindi a valle (UART/MQTT/UI) esponiamo SEMPRE un solo dato per dispositivo
// fisico, non uno per elemento.
static void sensor_combined(lamp_node_t *node, int8_t *pres_out, int32_t *light_out)
{
    int8_t  p = -1;
    int32_t l = -1;
    for (uint8_t s = 0; s < node->sensor_count; s++) {
        if (p == -1 && node->sensor_presence[s] != -1) p = node->sensor_presence[s];
        if (l == -1 && node->sensor_light_cl[s] != -1) l = node->sensor_light_cl[s];
    }
    *pres_out  = p;
    *light_out = l;
}

// Parsing dati "marshalled" del Sensor Status -> aggiorna presenza/luce dell'elemento
static void parse_sensor_status(uint16_t src, struct net_buf_simple *buf)
{
    int ni = find_node_by_addr(src);
    if (ni < 0 || buf == NULL) return;
    lamp_node_t *node = &nodes[ni];

    int ei = -1;
    for (uint8_t k = 0; k < node->sensor_count; k++) {
        if ((uint16_t)(node->unicast + node->sensor_offsets[k]) == src) { ei = k; break; }
    }
    if (ei < 0) return;

    uint8_t *d = buf->data;
    uint16_t len = buf->len;
    uint16_t i = 0;
    while (i < len) {
        uint8_t  b0 = d[i];
        uint16_t pid;
        uint8_t  vlen;
        uint16_t hdr;
        if (b0 & 0x01) {                 // Format B: 3 byte header
            if (i + 3 > len) break;
            vlen = ((b0 >> 1) & 0x7F) + 1;
            pid  = d[i + 1] | (d[i + 2] << 8);
            hdr  = 3;
        } else {                         // Format A: 2 byte header
            if (i + 2 > len) break;
            vlen = ((b0 >> 1) & 0x0F) + 1;
            pid  = ((b0 >> 5) & 0x07) | (d[i + 1] << 3);
            hdr  = 2;
        }
        uint16_t voff = i + hdr;
        if (voff + vlen > len) break;

        if (pid == PROP_PRESENCE_DETECTED && vlen >= 1) {
            node->sensor_presence[ei] = d[voff] ? 1 : 0;
            ESP_LOGI(TAG, "[SENSOR 0x%04x] Presenza = %s", src,
                     node->sensor_presence[ei] ? "TRUE" : "FALSE");
        } else if (pid == PROP_PRESENT_AMBIENT_LIGHT && vlen >= 3) {
            uint32_t raw = d[voff] | (d[voff + 1] << 8) | (d[voff + 2] << 16);
            int32_t calibrated = (int32_t)raw + node->sensor_light_cal;
            if (calibrated < 0) calibrated = 0;
            node->sensor_light_cl[ei] = calibrated;   // centilux, gia' calibrato
            ESP_LOGI(TAG, "[SENSOR 0x%04x] Luce = %ld.%02ld lux (grezzo %lu.%02lu, cal %ld)", src,
                     (long)(calibrated / 100), (long)(calibrated % 100),
                     (unsigned long)(raw / 100), (unsigned long)(raw % 100), (long)node->sensor_light_cal);
        } else if (pid == PROP_PRESENT_INPUT_POWER && vlen >= 3) {
            uint32_t raw = d[voff] | (d[voff + 1] << 8) | (d[voff + 2] << 16);
            node->sensor_power_dw[ei] = (int32_t)raw;   // deciwatt
            ESP_LOGI(TAG, "[SENSOR 0x%04x] Potenza = %lu.%01lu W", src,
                     (unsigned long)(raw / 10), (unsigned long)(raw % 10));
        } else if (pid == PROP_TOTAL_ENERGY && vlen >= 4) {
            uint32_t raw = d[voff] | (d[voff + 1] << 8) | (d[voff + 2] << 16) | ((uint32_t)d[voff + 3] << 24);
            node->sensor_energy_wh[ei] = (int32_t)raw;  // Wh
            ESP_LOGI(TAG, "[SENSOR 0x%04x] Energia = %lu.%03lu kWh", src,
                     (unsigned long)(raw / 1000), (unsigned long)(raw % 1000));
        } else {
            // Dump raw temporaneo per identificare i PID di potenza/energia del
            // Sensor Server della lampada (model 0x1100, elem 1): non sappiamo
            // ancora i Property ID esatti usati da questo firmware, quindi
            // mostriamo i byte cosi' come arrivano per riconoscerli a vista.
            char hex[2 * 8 + 1] = {0};
            uint8_t hlen = vlen > 8 ? 8 : vlen;
            for (uint8_t hi = 0; hi < hlen; hi++) {
                snprintf(hex + hi * 2, 3, "%02x", d[voff + hi]);
            }
            ESP_LOGI(TAG, "[SENSOR 0x%04x] PID 0x%04x len %d bytes=%s (ignorato)",
                     src, pid, vlen, hex);
        }
        i = voff + vlen;
    }
    int8_t  cp;
    int32_t cl;
    sensor_combined(node, &cp, &cl);
    uart_push_sensor(node->unicast, cp, cl);
}

static void sensor_client_callback(esp_ble_mesh_sensor_client_cb_event_t event,
                                   esp_ble_mesh_sensor_client_cb_param_t *param)
{
    if (param == NULL || param->params == NULL) return;
    uint16_t src = param->params->ctx.addr;
    uint32_t recv_op = param->params->ctx.recv_op;

    if (event == ESP_BLE_MESH_SENSOR_CLIENT_GET_STATE_EVT ||
        event == ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT) {
        if (recv_op == ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS) {
            parse_sensor_status(src, param->status_cb.sensor_status.marshalled_sensor_data);
        }
    } else if (event == ESP_BLE_MESH_SENSOR_CLIENT_TIMEOUT_EVT) {
        ESP_LOGW(TAG, "[SENSOR 0x%04x] timeout get", src);
    }
}

// ============================================================
// UART BRIDGE — collegamento seriale verso l'ESP32 relay-controller
//
// UART1, GPIO4=TX, GPIO5=RX, 115200 8N1.
// Protocollo testuale a riga (terminata da '\n'), campi chiave=valore
// separati da ';'. Indirizzo elemento in hex su 4 cifre (es. 0006).
//
// TX (gateway -> relay-controller), inviato a ogni cambio di stato
// e in risposta a DUMP:
//   ONOFF;addr=0006;val=1
//   LEVEL;addr=0006;val=12000;pct=37
//   SENSOR;addr=0006;presence=1;lux=235.50      (lux=-1 se sconosciuto)
//   DUMPEND                                      (fine di un DUMP)
//
// RX (relay-controller -> gateway):
//   SET;addr=0006;onoff=1
//   SET;addr=0006;level=12000
//   SET;addr=0006;pct=50                         (comodo: 0-100 -> level)
//   GET;addr=0006                                (richiede letture fresche)
//   DUMP                                          (richiede stato corrente di tutto)
// ============================================================
#define BRIDGE_UART_NUM    UART_NUM_1
#define BRIDGE_TXD_PIN      4
#define BRIDGE_RXD_PIN      5
#define BRIDGE_BAUD         115200
#define BRIDGE_RX_BUF_SIZE  1024
#define BRIDGE_LINE_MAX     128

static void uart_send_line(const char *line)
{
    uart_write_bytes(BRIDGE_UART_NUM, line, strlen(line));
    uart_write_bytes(BRIDGE_UART_NUM, "\n", 1);
    // Push non-bloccante verso USB: le callback BLE Mesh che chiamano questa
    // funzione non devono mai bloccare sul canale USB. usb_push_task drena
    // la coda in modo indipendente senza coinvolgere il BLE Mesh task.
    usb_cdc_push_line(line);
}

static void uart_push_onoff(uint16_t addr, uint8_t val)
{
    char line[64];
    snprintf(line, sizeof(line), "ONOFF;addr=%04X;val=%d", addr, val);
    uart_send_line(line);
}

static void uart_push_level(uint16_t addr, int16_t val)
{
    int pct = (val <= 0) ? 0 : (int)((int32_t)val * 100 / 32767);
    char line[64];
    snprintf(line, sizeof(line), "LEVEL;addr=%04X;val=%d;pct=%d", addr, val, pct);
    uart_send_line(line);
}

static void uart_push_sensor(uint16_t addr, int8_t presence, int32_t light_cl)
{
    char line[80];
    if (light_cl >= 0) {
        snprintf(line, sizeof(line), "SENSOR;addr=%04X;presence=%d;lux=%ld.%02ld",
                 addr, presence, (long)(light_cl / 100), (long)(light_cl % 100));
    } else {
        snprintf(line, sizeof(line), "SENSOR;addr=%04X;presence=%d;lux=-1", addr, presence);
    }
    uart_send_line(line);
}

static bool bridge_get_kv_hex(const char *line, const char *key, uint16_t *out)
{
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    *out = (uint16_t)strtoul(p + strlen(pat), NULL, 16);
    return true;
}

static bool bridge_get_kv_int(const char *line, const char *key, int *out)
{
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    *out = (int)strtol(p + strlen(pat), NULL, 10);
    return true;
}

static bool find_onoff_by_addr(uint16_t addr, uint8_t *node_idx, uint8_t *elem_idx)
{
    for (uint8_t n = 0; n < node_count; n++) {
        lamp_node_t *node = &nodes[n];
        for (uint8_t i = 0; i < node->onoff_count; i++) {
            if ((uint16_t)(node->unicast + node->onoff_offsets[i]) == addr) {
                *node_idx = n; *elem_idx = i; return true;
            }
        }
    }
    return false;
}

static bool find_level_by_addr(uint16_t addr, uint8_t *node_idx, uint8_t *elem_idx)
{
    for (uint8_t n = 0; n < node_count; n++) {
        lamp_node_t *node = &nodes[n];
        for (uint8_t i = 0; i < node->level_count; i++) {
            if ((uint16_t)(node->unicast + node->level_offsets[i]) == addr) {
                *node_idx = n; *elem_idx = i; return true;
            }
        }
    }
    return false;
}

static void bridge_dump_all(void)
{
    for (uint8_t n = 0; n < node_count; n++) {
        lamp_node_t *node = &nodes[n];
        if (!node->configured || node->is_switch) continue;
        for (uint8_t i = 0; i < node->onoff_count; i++)
            uart_push_onoff((uint16_t)(node->unicast + node->onoff_offsets[i]), node->onoff_states[i]);
        for (uint8_t i = 0; i < node->level_count; i++)
            uart_push_level((uint16_t)(node->unicast + node->level_offsets[i]), node->level_states[i]);
        if (node->kind == NODE_SENSOR) {
            int8_t  cp;
            int32_t cl;
            sensor_combined(node, &cp, &cl);
            uart_push_sensor(node->unicast, cp, cl);
        }
    }
    uart_send_line("DUMPEND");
}

static void bridge_handle_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
    if (len == 0) return;

    uint16_t addr;
    int      ival;
    uint8_t  ni, ei;

    if (strncmp(line, "DUMP", 4) == 0) {
        bridge_dump_all();
    } else if (strncmp(line, "GET;", 4) == 0) {
        if (bridge_get_kv_hex(line, "addr", &addr)) {
            if (find_onoff_by_addr(addr, &ni, &ei)) gateway_read_onoff_state(addr);
            if (find_level_by_addr(addr, &ni, &ei)) gateway_read_level_state(addr);
        }
    } else if (strncmp(line, "SET;", 4) == 0) {
        if (!bridge_get_kv_hex(line, "addr", &addr)) return;
        // Indirizzi di gruppo BLE Mesh: 0xC000-0xFEFF. Usato da Manager.py per
        // accendere/dimmare tutte le lampade di un'area con un solo comando
        // (vedi send_onoff_group/send_level_group): niente lookup per nodo,
        // si invia direttamente al gruppo.
        bool is_group = (addr >= 0xC000 && addr <= 0xFEFF);
        if (bridge_get_kv_int(line, "onoff", &ival)) {
            if (is_group) {
                send_onoff_group(addr, ival ? 1 : 0);
            } else {
                // Manager.py indirizza solo il Level element (il solo esposto in
                // modem/display), ma ogni lampada DALI ha N canali OnOff che devono
                // essere tutti accesi/spenti insieme. Cerchiamo il nodo tramite
                // qualsiasi indirizzo (OnOff o Level) e inviamo a TUTTI gli elementi.
                bool found = find_onoff_by_addr(addr, &ni, &ei);
                if (!found) found = find_level_by_addr(addr, &ni, &ei);
                if (found) {
                    lamp_node_t *ln = &nodes[ni];
                    if (ln->onoff_count > 0) {
                        for (uint8_t k = 0; k < ln->onoff_count; k++)
                            send_onoff_elem(ni, k, ival ? 1 : 0);
                    } else {
                        uint8_t lei;
                        if (find_level_by_addr(addr, &ni, &lei))
                            send_level_elem(ni, lei, ival ? 32767 : -32768);
                    }
                }
            }
        }
        if (bridge_get_kv_int(line, "level", &ival)) {
            if (is_group) send_level_group(addr, (int16_t)ival);
            else if (find_level_by_addr(addr, &ni, &ei)) send_level_elem(ni, ei, (int16_t)ival);
        }
        if (bridge_get_kv_int(line, "pct", &ival)) {
            ival = ival < 0 ? 0 : (ival > 100 ? 100 : ival);
            int16_t lvl = (int16_t)((int32_t)ival * 32767 / 100);
            // trans/delay opzionali (default 0,0): vedi /level lato web per la
            // stessa codifica (trans in ms -> step Generic Level Set, delay in ms -> step da 5ms).
            int trans_ms = 0, delay_ms = 0;
            bridge_get_kv_int(line, "trans", &trans_ms);
            bridge_get_kv_int(line, "delay", &delay_ms);
            uint8_t trans_time = 0;
            if (trans_ms > 0) {
                if (trans_ms <= 6200) trans_time = (uint8_t)((trans_ms + 99) / 100);
                else {
                    int steps = (trans_ms + 999) / 1000;
                    if (steps > 62) steps = 62;
                    trans_time = (uint8_t)(steps | (1 << 6));
                }
            }
            uint8_t delay_steps = (delay_ms <= 0) ? 0
                                : (uint8_t)((delay_ms > 1275) ? 255 : (delay_ms / 5));
            if (is_group) send_level_group_ex(addr, lvl, trans_time, delay_steps);
            else if (find_level_by_addr(addr, &ni, &ei)) send_level_elem_ex(ni, ei, lvl, trans_time, delay_steps);
        }
    }
}

static void uart_bridge_task(void *arg)
{
    uart_config_t cfg = {
        .baud_rate  = BRIDGE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // TX buffer 256B (era 0 = scrittura bloccante sulla FIFO): con un buffer
    // i push UART (uart_push_onoff/level/sensor, chiamati anche dai callback
    // BLE Mesh) tornano subito senza tenere impegnato il task chiamante
    // finche' i byte non sono fisicamente usciti dalla UART.
    uart_driver_install(BRIDGE_UART_NUM, BRIDGE_RX_BUF_SIZE, 256, 0, NULL, 0);
    uart_param_config(BRIDGE_UART_NUM, &cfg);
    uart_set_pin(BRIDGE_UART_NUM, BRIDGE_TXD_PIN, BRIDGE_RXD_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    char   line[BRIDGE_LINE_MAX];
    size_t idx = 0;
    uint8_t byte;

    ESP_LOGI(TAG, "UART bridge attivo: TX=%d RX=%d @ %d baud", BRIDGE_TXD_PIN, BRIDGE_RXD_PIN, BRIDGE_BAUD);

    while (1) {
        int n = uart_read_bytes(BRIDGE_UART_NUM, &byte, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) continue;
        if (byte == '\n') {
            line[idx] = '\0';
            bridge_handle_line(line);
            idx = 0;
        } else if (idx < BRIDGE_LINE_MAX - 1) {
            line[idx++] = (char)byte;
        }
    }
}

// ============================================================
// CFG:STATE — stato completo (nodi + discovered) in righe di testo, vedi
// protocollo USB CDC. Sostituisce il vecchio /state JSON: stessi dati, ma
// "name" e "power"/"energy"/"lightcal" sono usciti dal nuovo contratto (la
// PWA non li mostra piu' - vedi spec) quindi non vengono emessi.
// ============================================================
static void cfg_send_state(const char *line)
{
    (void)line;
    char m[200];

    // DEBUG TEMPORANEO: vedi nota in usb_cfg_handle_line.
    ESP_LOGI(TAG, "cfg_send_state: node_count=%d discovered_count=%d", node_count, discovered_count);

    usb_cdc_send_line("CFG:STATE_START");
    // CFG:BUSY copriva solo config_busy (il bind, dopo che il provisioning
    // BLE e' gia' completato): la finestra fra "click Provisiona" e la fine
    // dell'handshake BLE (PROV_COMPLETE) - spesso la piu' lenta, specie con
    // tanti nodi gia' connessi che occupano il radio - non veniva segnalata,
    // e il dispositivo intanto spariva dalla UI (tolto da discovered[],
    // non ancora in nodes[]): sembrava che non stesse succedendo nulla.
    // gateway_is_provisioning() copre l'intera finestra (vedi g_node_provisioning_active).
    snprintf(m, sizeof(m), "CFG:BUSY;%s", gateway_is_provisioning() ? "true" : "false");
    usb_cdc_send_line(m);
    snprintf(m, sizeof(m), "CFG:OOB;%s", oob_ready ? "true" : "false");
    usb_cdc_send_line(m);
    // Nuovo campo (vedi punto 1 della richiesta): indica se il device e' in
    // modalita' USB (scan/provisioning/comandi di scrittura abilitati) o in
    // modalita' normale (solo MQTT, BOOT premuto a lungo per cambiare). La
    // PWA lo usa per mostrare un avviso e disabilitare i controlli di scrittura.
    snprintf(m, sizeof(m), "CFG:USBMODE;active=%s", g_usb_mode_active ? "true" : "false");
    usb_cdc_send_line(m);

    for (uint8_t ni = 0; ni < node_count; ni++) {
        lamp_node_t *node = &nodes[ni];
        snprintf(m, sizeof(m),
            "CFG:NODE;i=%d;base=0x%04x;cfg=%d;fail=%d;sw=%d;kind=%d;grp=%d;paired=%d;lamp_elem=%d;name=%s",
            ni, node->unicast, node->configured ? 1 : 0, node->failed ? 1 : 0,
            node->is_switch ? 1 : 0, node->kind, group_bound[ni] ? 1 : 0,
            node->companion_paired ? 1 : 0, node->companion_lamp_elem, get_node_name(node->unicast));
        usb_cdc_send_line(m);

        if (node->is_switch) continue; // niente ELEM/LVL/SENSOR_DATA per gli switch (vedi spec UI: solo "Rimuovi")

        if (node->kind == NODE_SENSOR) {
            int8_t  pres;
            int32_t light;
            sensor_combined(node, &pres, &light);
            snprintf(m, sizeof(m), "CFG:SENSOR_DATA;node=%d;pres=%d;light=%ld;hassens=%d",
                     ni, pres, (long)light, node->sensor_count > 0 ? 1 : 0);
            usb_cdc_send_line(m);
        } else {
            for (uint8_t j = 0; j < node->onoff_count; j++) {
                uint16_t addr = node->unicast + node->onoff_offsets[j];
                snprintf(m, sizeof(m), "CFG:ELEM;node=%d;e=%d;addr=0x%04x;on=%d",
                         ni, node->onoff_offsets[j], addr, node->onoff_states[j] ? 1 : 0);
                usb_cdc_send_line(m);
            }
            for (uint8_t j = 0; j < node->level_count; j++) {
                uint16_t addr = node->unicast + node->level_offsets[j];
                int pct = (node->level_states[j] <= 0)
                          ? 0
                          : (int)((int32_t)node->level_states[j] * 100 / 32767);
                snprintf(m, sizeof(m), "CFG:LVL;node=%d;li=%d;e=%d;addr=0x%04x;pct=%d",
                         ni, j, node->level_offsets[j], addr, pct);
                usb_cdc_send_line(m);
            }
        }
    }

    int64_t now_us = esp_timer_get_time();
    for (uint8_t di = 0; di < discovered_count; di++) {
        if (now_us - discovered[di].last_seen_us >= DISCOVERED_EXPIRE_US) continue;
        char uuid_hex[33] = {0};
        for (int j = 0; j < 16; j++)
            snprintf(&uuid_hex[j * 2], 3, "%02x", discovered[di].uuid[j]);
        char mac_hex[13] = {0};
        for (int j = 0; j < 6; j++)
            snprintf(&mac_hex[j * 2], 3, "%02x", discovered[di].addr[j]);
        // Il dispositivo scoperto puo' essere un nodo GIA' noto che e'
        // "caduto" dalla mesh (es. si e' resettato) e si ripresenta come non
        // provisionato con lo stesso UUID (fisso di fabbrica): la PWA puo'
        // avvisare prima di riprovisionarlo, invece di sembrare che un nodo
        // sparisca/si sostituisca a sorpresa - vedi conversazione.
        int known_ni = -1;
        for (uint8_t ki = 0; ki < node_count; ki++) {
            if (memcmp(nodes[ki].uuid, discovered[di].uuid, 16) == 0) { known_ni = ki; break; }
        }
        char knownbuf[40] = {0};
        if (known_ni >= 0) {
            snprintf(knownbuf, sizeof(knownbuf), ";known=1;knownname=%s",
                     get_node_name(nodes[known_ni].unicast));
        }
        snprintf(m, sizeof(m), "CFG:DISCOVERED;uuid=%s;addr=%s;rssi=%d;oob=%d%s",
                 uuid_hex, mac_hex, (int)discovered[di].rssi, discovered[di].needs_qr_oob ? 1 : 0, knownbuf);
        usb_cdc_send_line(m);
    }

    usb_cdc_send_line("CFG:STATE_END");
}

// ============================================================
// CFG:CMD;node=N;elem=E;val=0|1|2
// ============================================================
static void cfg_cmd_elem(const char *line)
{
    // Mentre si sta provisionando/configurando un nodo nuovo, niente comandi
    // manuali verso i nodi gia' connessi: condividono lo stesso radio BLE e
    // un GET/SET in volo puo' far fallire/rallentare il provisioning in corso.
    if (gateway_is_provisioning()) { cfg_err("CMD", "provisioning in corso, attendi"); return; }

    int node_idx = -1, elem_idx = -1, val = 0;
    cfg_kv_int(line, "node", &node_idx);
    cfg_kv_int(line, "elem", &elem_idx);
    cfg_kv_int(line, "val",  &val);

    if (node_idx < 0 || node_idx >= node_count) { cfg_err("CMD", "nodo non valido"); return; }
    lamp_node_t *node = &nodes[node_idx];
    if (!node->configured || node->is_switch ||
        elem_idx < 0 || elem_idx >= node->onoff_count) {
        cfg_err("CMD", "elemento non valido"); return;
    }

    if (val == 2) {
        gateway_read_onoff_state(node->unicast + node->onoff_offsets[elem_idx]);
        vTaskDelay(pdMS_TO_TICKS(150));
    } else {
        send_onoff_elem((uint8_t)node_idx, (uint8_t)elem_idx, (uint8_t)val);
    }
    cfg_ok("CMD");
}

// ============================================================
// CFG:LEVEL;node=N;elem=I;val=0-100[;trans=ms;delay=ms]
// ============================================================
static void cfg_level(const char *line)
{
    if (gateway_is_provisioning()) { cfg_err("LEVEL", "provisioning in corso, attendi"); return; }

    int node_idx = -1, level_idx = -1, val = 0, trans_ms = 0, delay_ms = 0;
    cfg_kv_int(line, "node",  &node_idx);
    cfg_kv_int(line, "elem",  &level_idx);
    cfg_kv_int(line, "val",   &val);
    cfg_kv_int(line, "trans", &trans_ms);
    cfg_kv_int(line, "delay", &delay_ms);

    if (node_idx < 0 || node_idx >= node_count) { cfg_err("LEVEL", "nodo non valido"); return; }
    lamp_node_t *node = &nodes[node_idx];
    if (!node->configured || node->is_switch ||
        level_idx < 0 || level_idx >= node->level_count) {
        cfg_err("LEVEL", "elemento level non valido"); return;
    }

    // Codifica spec Generic Level Set: trans_time = num_steps | (resolution<<6).
    // Risoluzione 0 = 100ms/step, max 62 step -> 6200ms; oltre, usiamo
    // risoluzione 1 = 1s/step (max 62s), piu' che sufficiente per una lampada.
    uint8_t trans_time = 0;
    if (trans_ms > 0) {
        if (trans_ms <= 6200) {
            trans_time = (uint8_t)((trans_ms + 99) / 100);       // step da 100ms, risoluzione 0
        } else {
            int steps = (trans_ms + 999) / 1000;
            if (steps > 62) steps = 62;
            trans_time = (uint8_t)(steps | (1 << 6));            // step da 1s, risoluzione 1
        }
    }
    uint8_t delay_steps = (delay_ms <= 0) ? 0
                        : (uint8_t)((delay_ms > 1275) ? 255 : (delay_ms / 5));

    send_level_elem_ex((uint8_t)node_idx, (uint8_t)level_idx,
                        (int16_t)((val * 32767L) / 100), trans_time, delay_steps);
    cfg_ok("LEVEL");
}

// ============================================================
// CFG:ADDDEV;qr=<contenuto QR code>
// Estrae il campo Z (Static OOB 16 byte) dal formato Sunricher
// ============================================================
static void cfg_adddev(const char *line)
{
    char qr[200] = {0};
    if (!cfg_kv_rest(line, "qr", qr, sizeof(qr))) { cfg_err("ADDDEV", "parametro 'qr' mancante"); return; }
    cfg_urldecode(qr);
    ESP_LOGI(TAG, "QR decodificato: %s", qr);

    // Cerca il campo con prefisso Z seguito da esattamente 32 hex
    uint8_t oob_tmp[16] = {0};
    bool    found       = false;
    char   *p           = qr;

    while (*p && !found) {
        if (*p == 'Z') {
            char *hex = p + 1;
            size_t rem = strlen(hex);
            if (rem >= 32) {
                bool valid = true;
                for (int i = 0; i < 32 && valid; i++) {
                    char c = hex[i];
                    valid = ((c >= '0' && c <= '9') ||
                             (c >= 'A' && c <= 'F') ||
                             (c >= 'a' && c <= 'f'));
                }
                if (valid) {
                    for (int i = 0; i < 16; i++) {
                        char h[3] = {hex[i*2], hex[i*2+1], 0};
                        oob_tmp[i] = (uint8_t)strtol(h, NULL, 16);
                    }
                    found = true;
                    break;
                }
            }
        }
        // avanza al prossimo campo
        while (*p && *p != '+') p++;
        if (*p == '+') p++;
    }

    if (!found) {
        cfg_err("ADDDEV", "campo OOB non trovato, il QR deve contenere un campo Z con 32 caratteri hex");
        return;
    }

    memcpy(oob_buf, oob_tmp, 16);
    oob_ready              = true;
    prov.prov_static_oob_len = 16;

    ESP_LOGI(TAG, "OOB registrato: %02X%02X%02X...%02X",
             oob_buf[0], oob_buf[1], oob_buf[2], oob_buf[15]);
    cfg_ok("ADDDEV");
}

// ============================================================
// /pair?node=N&qr=<QR>&rev=1
// Abbina un companion switch a una lampada: estrae chiave (campo Z) e
// MAC (campo 30S) dal QR, poi configura il vendor model EnOcean della
// lampada via SET. rev=1 (default) inverte chiave/MAC in little-endian.
// ============================================================
static bool parse_hex_n(const char *s, uint8_t *out, int nbytes)
{
    for (int i = 0; i < nbytes * 2; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    for (int i = 0; i < nbytes; i++) {
        char h[3] = { s[i*2], s[i*2+1], 0 };
        out[i] = (uint8_t)strtol(h, NULL, 16);
    }
    return true;
}

// CFG:PAIR;node=N;qr=<QR>[;rev=0|1][;kr=0|1][;mr=0|1]
static void cfg_pair(const char *line)
{
    char qr[200] = {0};
    int  ni = -1;

    if (!cfg_kv_rest(line, "qr", qr, sizeof(qr)) || !cfg_kv_int(line, "node", &ni)) {
        cfg_err("PAIR", "parametri 'node' e 'qr' richiesti"); return;
    }
    // kr = inverti chiave, mr = inverti MAC. Nordic conferma che la chiave NON va
    // invertita (passata as-is a bt_enocean_commission) -> kr default 0.
    // Il MAC va in little-endian -> mr default 1. 'rev' imposta entrambi.
    bool kr = false, mr = true;
    int tmp;
    if (cfg_kv_int(line, "rev", &tmp)) kr = mr = (tmp != 0);
    if (cfg_kv_int(line, "kr",  &tmp)) kr = (tmp != 0);
    if (cfg_kv_int(line, "mr",  &tmp)) mr = (tmp != 0);

    if (ni < 0 || ni >= node_count || nodes[ni].is_switch || !nodes[ni].configured) {
        cfg_err("PAIR", "indice lampada non valido o non ancora configurata"); return;
    }
    if (config_busy || pair_active) { cfg_busy("PAIR"); return; }

    // cfg_kv_rest gia' preso il resto della riga: il '+' del QR resta intatto,
    // ma se "qr=" finiva a meta' della NOSTRA riga di comando (non dovrebbe,
    // qr e' sempre l'ultimo parametro) non ci sono altri campi dopo da tagliare.
    cfg_urldecode(qr);
    ESP_LOGI(TAG, "PAIR node #%d QR: %s", ni, qr);

    uint8_t key[16] = {0}, mac[6] = {0};
    bool hkey = false, hmac = false;
    char *p = qr;
    while (*p && (!hkey || !hmac)) {
        if (!hkey && p[0] == 'Z' && parse_hex_n(p + 1, key, 16)) {
            hkey = true;
        } else if (!hmac && p[0] == '3' && p[1] == '0' && p[2] == 'S' &&
                   parse_hex_n(p + 3, mac, 6)) {
            hmac = true;
        }
        while (*p && *p != '+') p++;
        if (*p == '+') p++;
    }

    if (!hkey || !hmac) {
        cfg_err("PAIR", "QR senza campo Z (chiave) o 30S (MAC) validi"); return;
    }

    if (kr)
        for (int i = 0; i < 8; i++) { uint8_t t = key[i]; key[i] = key[15-i]; key[15-i] = t; }
    if (mr)
        for (int i = 0; i < 3; i++) { uint8_t t = mac[i]; mac[i] = mac[5-i];  mac[5-i]  = t; }
    ESP_LOGI(TAG, "PAIR kr=%d mr=%d", kr, mr);

    memcpy(pair_key, key, 16);
    memcpy(pair_mac, mac, 6);
    pair_dst          = nodes[ni].unicast;
    // La lampada che riceve il beacon EnOcean pubblica il comando a GROUP_ALL:
    // cosi' qualunque lampada raggiunga il beacon funge da relay mesh per
    // le altre (es. la 3a lampada piu' lontana dal companion). Se il pub
    // fosse unicast su sé stessa (unicast+1) solo la lampada che riceve
    // direttamente il beacon si accenderebbe; le altre dipendono dalla
    // portata radio del companion, non dalla copertura della mesh.
    pair_target_elem  = GROUP_ALL;
    pair_retry        = 0;
    pair_is_unpair    = false;
    pair_active       = true;
    config_busy       = true;
    pair_start_bind();
    cfg_ok("PAIR");
}

// ============================================================
// CFG:RESET — cancella NVS e riavvia (nessuna risposta, vedi spec)
// ============================================================
static void cfg_reset(const char *line)
{
    (void)line;
    vTaskDelay(pdMS_TO_TICKS(200));
    nvs_flash_erase();
    esp_restart();
}

// ============================================================
// CFG:CLEAROOB — dimentica il valore OOB registrato
// ============================================================
static void cfg_clearoob(const char *line)
{
    (void)line;
    memset(oob_buf, 0, sizeof(oob_buf));
    oob_ready              = false;
    prov.prov_static_oob_len = 0;
    ESP_LOGI(TAG, "OOB cancellato.");
    cfg_ok("CLEAROOB");
}

// ============================================================
// CFG:SCANSTART — riavvia la scansione provisioner (es. se sembra bloccata)
// ============================================================
static void cfg_scanstart(const char *line)
{
    (void)line;
    esp_err_t err = esp_ble_mesh_provisioner_prov_enable(
        ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    ESP_LOGI(TAG, "Scansione riavviata (err 0x%x).", err);
    cfg_ok("SCANSTART");
}

// ============================================================
// Finestre di scan condivise col BLE classico (vedi ble_classic_handler.c).
//
// Il provisioner mesh deve restare attivo quasi sempre (disabilitarlo rompe
// la ricezione di QUALSIASI messaggio mesh, non solo il discovery - provato).
// Per dare comunque una possibilita' al radio condiviso allo scanner BLE
// classico, ble_classic_handler.c chiama queste due funzioni a bracket di
// ogni sua breve finestra di scan: durante quella finestra il provisioner
// viene disabilitato (e il poll_cb smette di inviare nuovi GET, vedi
// g_mesh_poll_paused) cosi' non genera richieste destinate al timeout.
// ============================================================
static bool g_mesh_poll_paused = false;

void mesh_pause_for_ble_scan(void)
{
    g_mesh_poll_paused = true;
    esp_ble_mesh_provisioner_prov_disable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
}

void mesh_resume_after_ble_scan(void)
{
    esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    g_mesh_poll_paused = false;
}

// Usata da ble_classic_handler.c per non aprire finestre di scan automatiche
// mentre e' in corso un provisioning/bind (config_busy) o un pairing switch
// (pair_active): in quella finestra il radio BT serve solo al provisioning.
bool gateway_is_provisioning(void)
{
    return config_busy || pair_active || g_node_provisioning_active;
}

// CFG:REBIND;node=N — ri-applica la coda di bind (AppKey + SUB_ADD a GROUP_ALL)
// a un nodo GIA' configurato, senza doverlo scollegare/riprovisionare.
// Serve dopo un aggiornamento firmware che cambia cosa viene legato (es.
// l'aggiunta della SUB_ADD del Level Server a GROUP_ALL per i comandi di
// gruppo): i nodi configurati PRIMA dell'update non hanno quella sub finche'
// non rifai il bind.
static void cfg_rebind(const char *line)
{
    int ni = -1;
    if (!cfg_kv_int(line, "node", &ni)) { cfg_err("REBIND", "node richiesto"); return; }
    if (ni < 0 || ni >= node_count || nodes[ni].is_switch || !nodes[ni].configured) {
        cfg_err("REBIND", "indice nodo non valido"); return;
    }
    if (config_busy || pair_active) { cfg_busy("REBIND"); return; }
    configuring_idx = ni;
    config_busy     = true;
    cfg_retry        = 0;
    build_bind_queue(&nodes[ni], nodes[ni].unicast);
    bind_idx = 0;
    send_next_bind(nodes[ni].unicast);
    ESP_LOGI(TAG, "Rebind avviato per nodo #%d (0x%04x).", ni, nodes[ni].unicast);
    cfg_ok("REBIND");
}

// CFG:SETKIND;node=N;kind=0|1  — l'utente classifica il device (0=lampada,1=sensore)
static void cfg_setkind(const char *line)
{
    int ni = -1, kind = -1;
    if (!cfg_kv_int(line, "node", &ni) || !cfg_kv_int(line, "kind", &kind)) {
        cfg_err("SETKIND", "node e kind richiesti"); return;
    }
    if (ni < 0 || ni >= node_count) { cfg_err("SETKIND", "nodo non valido"); return; }
    bool was_sensor = (nodes[ni].kind == NODE_SENSOR);
    nodes[ni].kind = kind ? NODE_SENSOR : NODE_LAMP;
    ESP_LOGI(TAG, "Nodo #%d kind -> %s", ni,
             nodes[ni].kind == NODE_SENSOR ? "SENSORE" : "LAMPADA");
    save_nodes_nvs();

    // Quando si passa da lampada a sensore: rimuove la subscription a GROUP_ALL
    // su OnOff e Level server, cosi' il sensore non risponde piu' ai comandi
    // companion/gruppo (il relay integrato del sensore non deve accendersi con le lampade).
    // Viceversa (sensore -> lampada): ri-aggiunge la subscription.
    bool now_sensor = (nodes[ni].kind == NODE_SENSOR);
    if (now_sensor != was_sensor && nodes[ni].configured && !config_busy && !pair_active) {
        bind_op_t op = now_sensor ? BIND_SUB_DEL : BIND_SUB_ADD;
        bind_queue_len = 0;
        bind_idx       = 0;
        cfg_retry      = 0;
        lamp_node_t *n = &nodes[ni];
        for (uint8_t i = 0; i < n->onoff_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            bind_queue[bind_queue_len++] = (bind_entry_t){
                (uint16_t)(n->unicast + n->onoff_offsets[i]),
                ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV, op, GROUP_ALL
            };
        }
        for (uint8_t i = 0; i < n->level_count && bind_queue_len < MAX_BIND_QUEUE; i++) {
            bind_queue[bind_queue_len++] = (bind_entry_t){
                (uint16_t)(n->unicast + n->level_offsets[i]),
                ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_SRV, op, GROUP_ALL
            };
        }
        if (bind_queue_len > 0) {
            configuring_idx = ni;
            config_busy     = true;
            ESP_LOGI(TAG, "SETKIND: avvio %s GROUP_ALL per %d elementi nodo #%d",
                     now_sensor ? "SUB_DEL" : "SUB_ADD", bind_queue_len, ni);
            send_next_bind(n->unicast);
        }
    }

    if (nodes[ni].kind == NODE_SENSOR) {
        for (uint8_t k = 0; k < nodes[ni].sensor_count; k++)
            gateway_read_sensor(nodes[ni].unicast + nodes[ni].sensor_offsets[k]);
    } else {
        for (uint8_t k = 0; k < nodes[ni].onoff_count; k++)
            gateway_read_onoff_state(nodes[ni].unicast + nodes[ni].onoff_offsets[k]);
    }
    cfg_ok("SETKIND");
}

// CFG:SETNAME;node=N;name=... — assegna un nome leggibile al nodo (es. "Cucina")
static void cfg_setname(const char *line)
{
    int ni = -1;
    char nmbuf[64] = {0};
    if (!cfg_kv_int(line, "node", &ni)) { cfg_err("SETNAME", "node richiesto"); return; }
    if (ni < 0 || ni >= node_count) { cfg_err("SETNAME", "nodo non valido"); return; }
    cfg_kv_rest(line, "name", nmbuf, sizeof(nmbuf)); // assente/vuoto -> azzera il nome
    // Niente '|' né ';' né '"': romperebbero il formato dei messaggi MQTT e questo stesso protocollo.
    for (char *c = nmbuf; *c; c++) if (*c == '|' || *c == ';' || *c == '"') *c = ' ';
    set_node_name(nodes[ni].unicast, nmbuf);
    save_nodes_nvs();
    // Per le lampade il nome arriva a Manager.py solo con CFG:MESHSAVE; i nodi
    // Sensore non passano da li' (publish_meshconfig() li esclude apposta),
    // quindi senza questa chiamata il nome di un sensore mesh non si
    // propagava mai via MQTT finche' non arrivava un CONFIGREQ.
    if (nodes[ni].kind == NODE_SENSOR && modem_mqtt_is_connected()) publish_mesh_sensor_names();
    cfg_ok("SETNAME");
}

// ============================================================
// CFG:FORGET;node=N — rimuove un nodo dal provisioner (RAM + NVS)
// ============================================================
static void cfg_forget(const char *line)
{
    int ni = -1;
    if (!cfg_kv_int(line, "node", &ni)) { cfg_err("FORGET", "node richiesto"); return; }
    if (ni < 0 || ni >= node_count) { cfg_err("FORGET", "nodo non valido"); return; }
    uint16_t uaddr = nodes[ni].unicast;
    esp_ble_mesh_provisioner_delete_node_with_addr(uaddr);
    if (ni < node_count - 1)
        memmove(&nodes[ni], &nodes[ni + 1],
                (node_count - 1 - ni) * sizeof(lamp_node_t));
    node_count--;
    // Tolgo anche il nome associato a questo indirizzo, altrimenti se in
    // futuro lo stesso unicast viene riassegnato a un device diverso si
    // ritroverebbe il nome del vecchio.
    for (uint8_t i = 0; i < name_count; i++) {
        if (name_addrs[i] == uaddr) {
            if (i < name_count - 1) {
                memmove(&name_addrs[i], &name_addrs[i + 1], (name_count - 1 - i) * sizeof(name_addrs[0]));
                memmove(&node_names[i], &node_names[i + 1], (name_count - 1 - i) * sizeof(node_names[0]));
            }
            name_count--;
            break;
        }
    }
    if (configuring_idx == ni) { configuring_idx = -1; config_busy = false; }
    else if (configuring_idx > ni) configuring_idx--;
    ESP_LOGI(TAG, "Nodo #%d (0x%04x) rimosso.", ni, uaddr);
    save_nodes_nvs();
    cfg_ok("FORGET");
}

// CFG:PROVISION;uuid=<32hex> — l'utente sceglie quale device provisionare
// ============================================================
static void cfg_provision(const char *line)
{
    char ubuf[36] = {0};
    if (!cfg_kv_rest(line, "uuid", ubuf, sizeof(ubuf)) || strlen(ubuf) < 32) {
        cfg_err("PROVISION", "uuid non valido"); return;
    }

    uint8_t uuid[16];
    for (int i = 0; i < 16; i++) {
        char h[3] = {ubuf[i * 2], ubuf[i * 2 + 1], 0};
        uuid[i] = (uint8_t)strtol(h, NULL, 16);
    }

    int di = -1;
    for (int i = 0; i < (int)discovered_count; i++) {
        if (memcmp(discovered[i].uuid, uuid, 16) == 0) { di = i; break; }
    }
    if (di < 0) { cfg_err("PROVISION", "dispositivo non piu' visibile, riattendi il beacon"); return; }
    if (config_busy || pair_active) { cfg_busy("PROVISION"); return; }
    if (node_count >= MAX_NODES) { cfg_err("PROVISION", "limite nodi raggiunto"); return; }

    discovered_dev_t dev = discovered[di];

    if (dev.needs_qr_oob && !oob_ready) {
        cfg_err("PROVISION", "questo switch richiede OOB, registra prima il QR code"); return;
    }
    prov.prov_static_oob_len = (dev.needs_qr_oob && oob_ready) ? 16 : 0;

    // Rimuovi dalla lista discovered
    if (di < (int)discovered_count - 1)
        memmove(&discovered[di], &discovered[di + 1],
                ((int)discovered_count - 1 - di) * sizeof(discovered_dev_t));
    discovered_count--;

    // Punto 3: da qui fino a fine configurazione (bind queue completata in
    // config_client_callback, o fallimento) nessun'altra attivita' BLE deve
    // intromettersi - vedi gateway_is_provisioning() e il guard su
    // RECV_UNPROV_ADV_PKT_EVT sopra. Settato PRIMA di add_unprov_dev() cosi'
    // copre anche l'intero handshake di provisioning, non solo il bind
    // (config_busy scatta solo dopo, a PROV_LINK_CLOSE).
    g_node_provisioning_active    = true;
    g_node_provisioning_started_us = esp_timer_get_time();
    memcpy(g_provisioning_uuid, dev.uuid, 16); // per il watchdog in poll_cb, vedi sopra

    esp_ble_mesh_unprov_dev_add_t add_dev = {0};
    memcpy(add_dev.addr, dev.addr, BD_ADDR_LEN);
    add_dev.addr_type = dev.addr_type;
    memcpy(add_dev.uuid, dev.uuid, 16);
    add_dev.oob_info  = dev.oob_info;
    add_dev.bearer    = dev.bearer;
    esp_err_t err = esp_ble_mesh_provisioner_add_unprov_dev(&add_dev,
        ADD_DEV_RM_AFTER_PROV_FLAG |
        ADD_DEV_START_PROV_NOW_FLAG |
        ADD_DEV_FLUSHABLE_DEV_FLAG);
    ESP_LOGI(TAG, "Provisioning richiesto UUID %02x%02x%02x%02x... (err 0x%x)",
             dev.uuid[0], dev.uuid[1], dev.uuid[2], dev.uuid[3], err);

    if (err == ESP_OK) {
        cfg_ok("PROVISION");
    } else {
        g_node_provisioning_active = false; // avvio fallito, niente da aspettare
        cfg_err("PROVISION", "avvio provisioning fallito");
    }
}

// ============================================================
// Dispatcher comandi USB CDC (vedi usb_cdc.c) - chiamata per ogni riga
// ricevuta dalla porta USB. "CFG:<CMD>[;k=v;...]" va ai comandi di
// configurazione qui sotto; qualunque altra riga (SET;/GET;/DUMP) va al
// bridge condiviso col relay-controller (bridge_handle_line, sopra) - stessa
// logica, due trasporti (vedi nota protocollo: "il gateway li accetta su
// entrambe le porte"). Risposte/push sono sempre broadcast su entrambe
// (uart_send_line manda anche su USB, vedi sopra) per semplicita': la spec
// chiederebbe di rispondere solo sulla porta di origine, ma qui non c'e'
// nessun dato sensibile e l'overhead di qualche riga in piu' sull'altro
// canale e' trascurabile.
// ============================================================
// Definite piu' sotto, vicino a "Persistenza setup unificato" (hub_name):
// in avanti perche' usb_cfg_handle_line/il suo dispatch vengono prima nel file.
static void cfg_sethubname(const char *line);
static void cfg_relaycfg(const char *line);
static void cfg_sensorcfg(const char *line);
static void cfg_resetsensors(const char *line);
static void cfg_resetslot(const char *line);
static void cfg_meshsave(const char *line);
static void cfg_status(const char *line);
static void cfg_relayset(const char *line);
static void cfg_snifferstart(const char *line);
static void cfg_snifferstop(const char *line);
static void cfg_snifferdata(const char *line);

void usb_cfg_handle_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
    if (len == 0) return;

    // DEBUG TEMPORANEO: per isolare perche' CFG:STATE/CFG:STATUS non
    // rispondono mai dalla PWA - rimuovere dopo la diagnosi. Va sul log di
    // sistema (UART0/JTAG, idf.py monitor), non sul canale dati USB CDC.
    ESP_LOGI(TAG, "usb_cfg_handle_line: riga ricevuta='%s' (len=%d)", line, (int)len);

    if (strncmp(line, "CFG:", 4) != 0) {
        bridge_handle_line(line);
        return;
    }
    const char *body = line + 4;
    const char *semi = strchr(body, ';');
    size_t clen = semi ? (size_t)(semi - body) : strlen(body);
    char cmd[16] = {0};
    if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;
    memcpy(cmd, body, clen);

    // Punto 2 della richiesta utente: CFG:STATE/STATUS/SNIFFERDATA restano
    // SEMPRE leggibili (servono anche solo per mostrare alla PWA "device in
    // modalita' MQTT, premi BOOT per attivare la modalita' USB"), ma tutti
    // gli altri comandi CFG: (scrittura/provisioning/pairing/relay/ecc.)
    // richiedono la modalita' USB attiva (g_usb_mode_active, vedi
    // user_btn_poll) - stessa logica di prima quando il gate sui comandi di
    // scrittura era la modalita' SoftAP/WiFi.
    bool always_allowed = (strcmp(cmd, "STATE") == 0 || strcmp(cmd, "STATUS") == 0 ||
                            strcmp(cmd, "SNIFFERDATA") == 0);
    if (!always_allowed && !g_usb_mode_active) {
        cfg_err(cmd, "modalita' USB non attiva: tieni premuto BOOT per qualche secondo");
        return;
    }

    if      (strcmp(cmd, "STATE")     == 0) cfg_send_state(line);
    else if (strcmp(cmd, "SCANSTART") == 0) cfg_scanstart(line);
    else if (strcmp(cmd, "PROVISION") == 0) cfg_provision(line);
    else if (strcmp(cmd, "FORGET")    == 0) cfg_forget(line);
    else if (strcmp(cmd, "REBIND")    == 0) cfg_rebind(line);
    else if (strcmp(cmd, "SETKIND")   == 0) cfg_setkind(line);
    else if (strcmp(cmd, "CMD")       == 0) cfg_cmd_elem(line);
    else if (strcmp(cmd, "LEVEL")     == 0) cfg_level(line);
    else if (strcmp(cmd, "ADDDEV")    == 0) cfg_adddev(line);
    else if (strcmp(cmd, "CLEAROOB")  == 0) cfg_clearoob(line);
    else if (strcmp(cmd, "PAIR")      == 0) cfg_pair(line);
    else if (strcmp(cmd, "UNPAIR")    == 0) cfg_unpair(line);
    else if (strcmp(cmd, "RESET")     == 0) cfg_reset(line);
    else if (strcmp(cmd, "SETHUBNAME")   == 0) cfg_sethubname(line);
    else if (strcmp(cmd, "RELAYCFG")     == 0) cfg_relaycfg(line);
    else if (strcmp(cmd, "SENSORCFG")    == 0) cfg_sensorcfg(line);
    else if (strcmp(cmd, "RESETSENSORS") == 0) cfg_resetsensors(line);
    else if (strcmp(cmd, "RESETSLOT")    == 0) cfg_resetslot(line);
    else if (strcmp(cmd, "MESHSAVE")     == 0) cfg_meshsave(line);
    else if (strcmp(cmd, "SETNAME")      == 0) cfg_setname(line);
    else if (strcmp(cmd, "STATUS")       == 0) cfg_status(line);
    else if (strcmp(cmd, "RELAYSET")     == 0) cfg_relayset(line);
    else if (strcmp(cmd, "SNIFFERSTART") == 0) cfg_snifferstart(line);
    else if (strcmp(cmd, "SNIFFERSTOP")  == 0) cfg_snifferstop(line);
    else if (strcmp(cmd, "SNIFFERDATA")  == 0) cfg_snifferdata(line);
    else cfg_err(cmd, "comando sconosciuto");
}

// ============================================================
// Persistenza nodi in NVS (namespace "gw_nodes"). Recuperata da una versione
// precedente del file (era stata cancellata per errore insieme al WiFi/HTTP
// durante la riscrittura USB CDC) - aggiornata al formato attuale: i nomi
// sono in una tabella separata indicizzata per unicast (name_addrs/name_count,
// non piu' un blob "names" allineato per indice di nodes[], vedi get_node_name/
// set_node_name) e c'e' la validazione difensiva sui conteggi che evita di
// rileggere un blob salvato da una versione precedente della struct lamp_node_t
// (overflow/crash altrove, vedi build_mesh_addr_list).
// ============================================================
#define NVS_NS_NODES "gw_nodes"

static void save_nodes_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NODES, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "count", node_count);
    if (node_count > 0) {
        nvs_set_blob(h, "nodes", nodes, node_count * sizeof(lamp_node_t));
    }
    nvs_set_u8(h, "ncount", name_count);
    if (name_count > 0) {
        nvs_set_blob(h, "naddrs", name_addrs, name_count * sizeof(name_addrs[0]));
        nvs_set_blob(h, "names", node_names, name_count * sizeof(node_names[0]));
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS: salvati %d nodi.", node_count);
}

static void load_nodes_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NODES, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t cnt = 0;
    if (nvs_get_u8(h, "count", &cnt) == ESP_OK && cnt > 0 && cnt <= MAX_NODES) {
        size_t sz = (size_t)cnt * sizeof(lamp_node_t);
        if (nvs_get_blob(h, "nodes", nodes, &sz) == ESP_OK) {
            node_count = cnt;
        }
    }
    uint8_t ncnt = 0;
    if (nvs_get_u8(h, "ncount", &ncnt) == ESP_OK && ncnt > 0 && ncnt <= MAX_NODES) {
        size_t asz = (size_t)ncnt * sizeof(name_addrs[0]);
        size_t nsz = (size_t)ncnt * sizeof(node_names[0]);
        if (nvs_get_blob(h, "naddrs", name_addrs, &asz) == ESP_OK &&
            nvs_get_blob(h, "names", node_names, &nsz) == ESP_OK) {
            name_count = ncnt;
        }
    }
    nvs_close(h);

    // Validazione difensiva: se la struct lamp_node_t e' cambiata da quando i
    // nodi sono stati salvati, i byte vengono reinterpretati con l'offset
    // sbagliato e onoff_count/level_count/sensor_count possono leggersi come
    // spazzatura enorme - poi usati come limite di loop altrove causano
    // scritture fuori dai buffer e crash/corruzione random. Meglio scartare
    // TUTTO il blob (richiede riprovisionare) che proseguire con dati non
    // verificabili.
    for (uint8_t i = 0; i < node_count; i++) {
        lamp_node_t *n = &nodes[i];
        if (n->onoff_count > MAX_ONOFF_ELEMENTS || n->level_count > MAX_LEVEL_ELEMENTS ||
            n->sensor_count > MAX_SENSOR_ELEMENTS || n->cli_count > MAX_ONOFF_ELEMENTS) {
            ESP_LOGE(TAG, "NVS: nodo #%d ha conteggi non validi (onoff=%d level=%d sensor=%d cli=%d) - "
                          "probabile formato vecchio della struct, scarto TUTTI i nodi salvati.",
                     i, n->onoff_count, n->level_count, n->sensor_count, n->cli_count);
            node_count = 0;
            memset(nodes, 0, sizeof(nodes));
            break;
        }
    }
    ESP_LOGI(TAG, "NVS: caricati %d nodi.", node_count);
}

// ============================================================
// Persistenza setup unificato (nome hub, relè abilitati, sensori BLE classici)
// Sostituisce il wizard WiFi one-shot + l'app Web Bluetooth del relay-controller
// originale: qui e' una pagina del webserver mesh, gia' attivo permanentemente,
// niente radio aggiuntiva.
// ============================================================
#define NVS_NS_UNIFIED "unified_cfg"
static char hub_name[32] = "Hub-1";

// URL-decode in-place: %XX -> char, '+' -> spazio (i browser percent-encodano
// ':' ',' ';' nei campi del form, serve per ricostruire mac/regole originali).
static void url_decode_inplace(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char h[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(h, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = 0;
}

static void save_unified_config_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_UNIFIED, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "hub_name", hub_name);
    uint8_t relay_mask = 0;
    for (int i = 0; i < RELAY_COUNT; i++) if (relay_is_enabled(i)) relay_mask |= (1 << i);
    nvs_set_u8(h, "relay_mask", relay_mask);
    for (int i = 0; i < BLE_CLASSIC_MAX_SENSORS; i++) {
        char mac[24] = "", rules[160] = "", name[24] = "";
        ble_classic_get_config(i, mac, sizeof(mac), rules, sizeof(rules));
        ble_classic_get_name(i, name, sizeof(name));
        char key_mac[16], key_rules[16], key_name[16];
        snprintf(key_mac, sizeof(key_mac), "s%d_mac", i);
        snprintf(key_rules, sizeof(key_rules), "s%d_rules", i);
        snprintf(key_name, sizeof(key_name), "s%d_name", i);
        nvs_set_str(h, key_mac, mac);
        nvs_set_str(h, key_rules, rules);
        nvs_set_str(h, key_name, name);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS: setup unificato salvato (%s).", hub_name);
}

static void load_unified_config_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_UNIFIED, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(hub_name);
    nvs_get_str(h, "hub_name", hub_name, &sz);
    uint8_t relay_mask = 0;
    nvs_get_u8(h, "relay_mask", &relay_mask);
    for (int i = 0; i < RELAY_COUNT; i++) relay_set_enabled(i, (relay_mask >> i) & 1);
    for (int i = 0; i < BLE_CLASSIC_MAX_SENSORS; i++) {
        char mac[24] = "", rules[160] = "", name[24] = "";
        char key_mac[16], key_rules[16], key_name[16];
        size_t mac_sz = sizeof(mac), rules_sz = sizeof(rules), name_sz = sizeof(name);
        snprintf(key_mac, sizeof(key_mac), "s%d_mac", i);
        snprintf(key_rules, sizeof(key_rules), "s%d_rules", i);
        snprintf(key_name, sizeof(key_name), "s%d_name", i);
        if (nvs_get_str(h, key_mac, mac, &mac_sz) == ESP_OK &&
            nvs_get_str(h, key_rules, rules, &rules_sz) == ESP_OK &&
            mac[0] != '\0') {
            ble_classic_configure_sensor(i, mac, rules);
        }
        if (nvs_get_str(h, key_name, name, &name_sz) == ESP_OK && name[0] != '\0') {
            ble_classic_set_name(i, name);
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "NVS: setup unificato caricato (%s).", hub_name);
}

// Comandi CFG: aggiunti oltre la spec PWA originale: quella spec copre solo
// la mesh BLE (provisioning/lampade/sensori/pairing), ma la vecchia pagina
// "/setup" gestiva anche nome hub, relè abilitati e MAC/regole dei sensori
// BLE classici - senza un equivalente, quella configurazione sarebbe rimasta
// bloccata per sempre a quello che c'e' oggi in NVS. Decisione: aggiungerli
// (vedi conversazione), non fa parte del contratto fornito dalla PWA.

// CFG:SETHUBNAME;name=<str>
static void cfg_sethubname(const char *line)
{
    char nbuf[40] = {0};
    if (!cfg_kv_rest(line, "name", nbuf, sizeof(nbuf))) { cfg_err("SETHUBNAME", "name richiesto"); return; }
    cfg_urldecode(nbuf);
    strncpy(hub_name, nbuf, sizeof(hub_name) - 1);
    hub_name[sizeof(hub_name) - 1] = '\0';
    save_unified_config_nvs();
    cfg_ok("SETHUBNAME");
}

// CFG:RELAYCFG;n=<idx>;enabled=0|1 — quali relè sono fisicamente presenti
static void cfg_relaycfg(const char *line)
{
    int n = -1, enabled = -1;
    if (!cfg_kv_int(line, "n", &n) || !cfg_kv_int(line, "enabled", &enabled)) {
        cfg_err("RELAYCFG", "n e enabled richiesti"); return;
    }
    if (n < 0 || n >= RELAY_COUNT) { cfg_err("RELAYCFG", "indice relè non valido"); return; }
    relay_set_enabled(n, enabled != 0);
    save_unified_config_nvs();
    cfg_ok("RELAYCFG");
}

// CFG:SENSORCFG;slot=<N>;mac=<hex>;rules=<...>;name=<str> — configura uno
// slot sensore BLE classico (mac vuoto = non tocca la config, solo il nome).
static void cfg_sensorcfg(const char *line)
{
    int slot = -1;
    if (!cfg_kv_int(line, "slot", &slot) || slot < 0 || slot >= BLE_CLASSIC_MAX_SENSORS) {
        cfg_err("SENSORCFG", "slot non valido"); return;
    }
    char mac[72] = {0}, rules[480] = {0}, name[72] = {0};
    cfg_kv_rest(line, "mac", mac, sizeof(mac));
    // "rules" e "name" possono contenere ';' al loro interno (vedi nota
    // protocollo su "qr="): per restare ordinati nel parsing, vanno passati
    // PER ULTIMI nella riga, in quest'ordine (mac;rules;name) - cfg_kv_rest
    // su "rules" si fermerebbe al primo ';' di "name=" se name venisse prima.
    char *rules_start = strstr(line, "rules=");
    if (rules_start) {
        char *name_marker = strstr(rules_start, ";name=");
        size_t rlen = name_marker ? (size_t)(name_marker - (rules_start + 6)) : strlen(rules_start + 6);
        if (rlen >= sizeof(rules)) rlen = sizeof(rules) - 1;
        memcpy(rules, rules_start + 6, rlen);
        rules[rlen] = '\0';
    }
    cfg_kv_rest(line, "name", name, sizeof(name));
    cfg_urldecode(mac);
    cfg_urldecode(rules);
    cfg_urldecode(name);
    for (char *c = name; *c; c++) if (*c == '|' || *c == ';' || *c == '"') *c = ' '; // non rompere JSON/protocollo MQTT
    if (mac[0] != '\0') ble_classic_configure_sensor(slot, mac, rules);
    ble_classic_set_name(slot, name); // indipendente dal mac: il nome si puo' salvare anche da solo
    save_unified_config_nvs();
    if (modem_mqtt_is_connected()) publish_sensorcfg();
    cfg_ok("SENSORCFG");
}

// CFG:RESETSENSORS — svuota tutti gli slot sensore BLE classico
static void cfg_resetsensors(const char *line)
{
    (void)line;
    ble_classic_reset_all();
    save_unified_config_nvs();
    cfg_ok("RESETSENSORS");
}

// CFG:RESETSLOT;slot=N — svuota un solo slot sensore BLE classico
static void cfg_resetslot(const char *line)
{
    int slot = -1;
    if (!cfg_kv_int(line, "slot", &slot) || slot < 0 || slot >= BLE_CLASSIC_MAX_SENSORS) {
        cfg_err("RESETSLOT", "slot non valido"); return;
    }
    ble_classic_reset_slot((uint8_t)slot);
    save_unified_config_nvs();
    if (modem_mqtt_is_connected()) publish_sensorcfg();
    cfg_ok("RESETSLOT");
}


// ============================================================
// Ponte MQTT (modem cellulare Air780E via modem_mqtt.c).
//
// Stesso broker/topic/formato messaggi del Manager.py esistente (vedi
// B_ModemMQTT.ino + MeshBridge.cpp in ESP32-terminal-controller-main):
//   topic pubblicazione "modem/display", sottoscrizione "modem/invio".
//   SENSOR|hub|slot|k=v;...;rssi=N      sensore BLE classico (vedi ble_classic_handler.c)
//   MESH|hub|addrHex|onoff=X[;pct=Y]    elemento lampada mesh (onoff/level)
//   MESH|hub|addrHex|presence=X;lux=Y   elemento sensore mesh
//   MESHLIST|hub|addr1,addr2,...        manifest periodico (pulizia "lampade fantasma")
//   MESHBOOT|hub|addr1,addr2,...        manifest UNA VOLTA al boot/prima connessione,
//                                       fonte di verita' immediata (vedi mqtt_publish_meshboot)
//   MESHCONFIG|hub|count|nome1:addr1,...  su richiesta utente (/meshsave), composizione
//                                       con nomi (vedi meshsave_handler)
//   RELAYSTATE|hub|bitstring            stato relè fisici
//   CAPS|hub|pinStr                     relè abilitati (annuncio post-boot)
// Topic separato "modem/lampade" (vedi mqtt_publish_lamps_snapshot, ogni 5s),
// payload JSON leggibile (le '"' vengono escapate in AT+MPUB con la sintassi
// Air780E \22, vedi modem_mqtt.c - niente base64):
//   {"hub":"...","count":N,"lamps":[{"name":"...","onoff":1,"pct":46,
//     "w":4.0,"wh":0.003},...]}
//   Tutte le lampade mesh connesse in un solo messaggio, niente indirizzo
//   (nome vuoto se non assegnato via /setname, w/wh null se il nodo non ha
//   Sensor Server di potenza).
// In ingresso: RELAYCMD|hub|n|ON_o_OFF, MESHCMD|hub|addrHex|onoff|v,
// MESHCMD|hub|addrHex|pct|v, MESHCMD|hub|addrHex|get - stesso formato gestito
// da handleIncomingMQTT/handleMeshMQTTCommand nell'originale: qui rispediamo
// i comandi MESHCMD a bridge_handle_line() costruendo la riga SET;/GET;
// equivalente, cosi' la logica di invio (gruppi, lookup per addr) è una sola.
// CONFIGREQ|ALL - Manager.py la manda al proprio avvio: ogni gateway in
// ascolto rispedisce subito CAPS+RELAYSTATE+MESHCONFIG (vedi mqtt_rx_handler).
// ============================================================
#define MQTT_BROKER_HOST "91.241.86.224"
#define MQTT_BROKER_PORT 1883
#define MQTT_PUB_TOPIC   "modem/display"
#define MQTT_SUB_TOPIC   "modem/invio"
#define MQTT_PUB_TOPIC_LAMPS "modem/lampade"

static void mqtt_publish_msg(const char *msg)
{
    modem_mqtt_publish(MQTT_PUB_TOPIC, msg);
    ESP_LOGI(TAG, "TX MQTT: %s", msg);
}

// Sistema preventivo/diagnostico per i buchi di connettivita' del modem
// cellulare (vedi modem_mqtt_set_recovery_callback): segnala via MQTT quanto
// e' durato un buco appena la sessione torna su, cosi' Manager.py se ne
// accorge nei suoi log anche se nessuno aveva il monitor seriale collegato
// nel momento esatto del problema (caso reale: buco di 9 minuti scoperto
// solo a posteriori dal grafico, senza nessuna traccia diagnostica).
static void modem_recovery_handler(uint32_t downtime_sec)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "MODEMRECOVERY|%s|%u", hub_name, (unsigned)downtime_sec);
    mqtt_publish_msg(msg);
}

static void mqtt_rx_handler(const char *payload)
{
    if (g_usb_mode_active) return; // modalita' USB: ignora tutto l'MQTT in ingresso
    ESP_LOGI(TAG, "[CMD] ricevuto: '%s'", payload);

    const char *p1 = strchr(payload, '|');
    if (!p1) return;
    char type[16] = {0};
    size_t tlen = (size_t)(p1 - payload);
    if (tlen >= sizeof(type)) return;
    memcpy(type, payload, tlen);

    // CONFIGREQ|ALL — sempre eseguito, anche durante provisioning: Manager.py
    // ha bisogno della configurazione per funzionare, bloccarla non ha senso.
    if (strcmp(type, "CONFIGREQ") == 0) {
        ESP_LOGI(TAG, "CONFIGREQ ricevuto, rispedisco la configurazione completa.");
        publish_caps();
        publish_sensorcfg();
        publish_relaystate();
        publish_meshconfig();
        publish_mesh_sensor_names();
        force_full_mqtt_resync();
        return;
    }

    const char *p2 = strchr(p1 + 1, '|');
    if (!p2) return;
    char hub[32] = {0};
    size_t hlen = (size_t)(p2 - (p1 + 1));
    if (hlen >= sizeof(hub)) return;
    memcpy(hub, p1 + 1, hlen);
    if (strcmp(hub, hub_name) != 0) return;

    const char *tail = p2 + 1;

    // RELAYCMD — sempre eseguito: i relè sono GPIO puri, indipendenti dalla
    // mesh BLE. Bloccarli durante config_busy/pair_active era sbagliato e
    // rendeva i relè irraggiungibili ogni volta che il provisioning si inceppava.
    if (strcmp(type, "RELAYCMD") == 0) {
        const char *s1 = strchr(tail, '|');
        if (!s1) return;
        int idx = atoi(tail) - 1;
        const char *cmd = s1 + 1;
        if (idx < 0 || idx >= RELAY_COUNT) return;
        if (!relay_is_enabled(idx)) {
            ESP_LOGW(TAG, "Comando relè ignorato: relè %d disabilitato dal setup.", idx + 1);
            return;
        }
        bool state = (strcmp(cmd, "ON") == 0 || strcmp(cmd, "1") == 0);
        relay_set(idx, state);
        ESP_LOGI(TAG, "RELAYCMD applicato: idx=%d state=%d", idx, state);
        return;
    }

    // MESHCMD usa il Generic OnOff/Level client, il provisioning usa il Config
    // client: sono modelli BLE Mesh completamente separati, non si interferiscono.
    // Bloccare MESHCMD su config_busy causava il sintomo "relay OK, lampade no":
    // config_busy stuck (loop di retry su nodo non configurato) bloccava tutte
    // le lampade indefinitamente mentre relay (prima del guard) funzionavano.
    if (strcmp(type, "MESHCMD") == 0) {
        // tail = "addrHex|onoff|v" oppure "addrHex|pct|v" oppure "addrHex|get".
        // Per "pct" v puo' anche essere "valore,trans_ms,delay_ms" (Manager.py,
        // impostazioni Generic Level: transizione/ritardo) - i due campi extra
        // sono opzionali, default 0,0 se assenti (comportamento storico).
        char addr_s[8] = {0}, op[8] = {0}, val_s[32] = {0};
        const char *s1 = strchr(tail, '|');
        if (!s1) return;
        size_t alen = (size_t)(s1 - tail);
        if (alen >= sizeof(addr_s)) return;
        memcpy(addr_s, tail, alen);
        const char *s2 = strchr(s1 + 1, '|');
        if (s2) {
            size_t oplen = (size_t)(s2 - (s1 + 1));
            if (oplen >= sizeof(op)) return;
            memcpy(op, s1 + 1, oplen);
            strncpy(val_s, s2 + 1, sizeof(val_s) - 1);
        } else {
            strncpy(op, s1 + 1, sizeof(op) - 1);
        }

        char line[96];
        if (strcmp(op, "onoff") == 0) {
            int v = (strcmp(val_s, "1") == 0 || strcmp(val_s, "ON") == 0) ? 1 : 0;
            snprintf(line, sizeof(line), "SET;addr=%s;onoff=%d", addr_s, v);
            bridge_handle_line(line);
        } else if (strcmp(op, "pct") == 0) {
            int pct = 0, trans_ms = 0, delay_ms = 0;
            sscanf(val_s, "%d,%d,%d", &pct, &trans_ms, &delay_ms);
            snprintf(line, sizeof(line), "SET;addr=%s;pct=%d;trans=%d;delay=%d",
                     addr_s, pct, trans_ms, delay_ms);
            bridge_handle_line(line);
        } else if (strcmp(op, "get") == 0) {
            snprintf(line, sizeof(line), "GET;addr=%s", addr_s);
            bridge_handle_line(line);
        }
        return;
    }

}

// Stato pubblicato l'ultima volta, per inviare solo le variazioni (stesso
// principio di RELAYSTATE/MeshElem nell'originale: niente publish ridondanti).
typedef struct {
    uint8_t onoff_states[MAX_ONOFF_ELEMENTS];
    int16_t level_states[MAX_LEVEL_ELEMENTS];
    int8_t  sensor_presence;
    int32_t sensor_light_cl;
    int32_t sensor_power_dw;
    int32_t sensor_energy_wh;
    bool    has_snapshot;
} mqtt_node_snapshot_t;

static mqtt_node_snapshot_t mqtt_snapshots[MAX_NODES];
static char                 mqtt_last_relay_bits[RELAY_COUNT + 1] = "";
static char                 mqtt_last_sensor[BLE_CLASSIC_MAX_SENSORS][160] = {{0}};
static bool                 mqtt_caps_sent = false;

// Forza il prossimo giro di mqtt_publish_node_changes()/mqtt_publish_relay_and_sensors()
// a mandare TUTTO lo stato attuale, non solo le variazioni: serve quando
// Manager.py si riavvia a memoria vuota (CONFIGREQ) - senza, se nessuna
// lampada/sensore e' cambiata stato da quando il gateway e' acceso, non
// verrebbe rimandato nulla e Manager.py resterebbe senza stato delle lampade.
static void force_full_mqtt_resync(void)
{
    for (uint8_t i = 0; i < node_count; i++) mqtt_snapshots[i].has_snapshot = false;
    memset(mqtt_last_sensor, 0, sizeof(mqtt_last_sensor));
}

static void mqtt_publish_node_changes(void)
{
    for (uint8_t n = 0; n < node_count; n++) {
        lamp_node_t *node = &nodes[n];
        if (!node->configured || node->is_switch) continue;
        mqtt_node_snapshot_t *snap = &mqtt_snapshots[n];

        // I nodi classificati "Sensore" (/setkind) non vanno pubblicati come
        // lampada anche se la composizione espone pure modelli OnOff/Level
        // (capita su PIR/sensori 0-10V - vedi parse_composition_data): senza
        // questo filtro Manager.py li mostrava come lampade accendibili.
        if (node->kind != NODE_SENSOR) {
            // Ciclo level PRIMA del ciclo onoff (importante): per un elemento
            // con onoff+level insieme deve controllare anche se l'ONOFF e'
            // cambiato (non solo il valore numerico del level - una lampada
            // spenta puo' mantenere l'ultimo level invariato, dipende dal
            // device), leggendo lo snapshot onoff COSI' COM'ERA prima che il
            // ciclo onoff qui sotto lo risincronizzi. Altrimenti uno spegni/
            // accendi a level invariato non veniva mai pubblicato.
            for (uint8_t i = 0; i < node->level_count; i++) {
                bool level_changed = !(snap->has_snapshot && snap->level_states[i] == node->level_states[i]);
                uint8_t matched_onoff = 255; // 255 = nessun elem onoff su questo offset
                bool onoff_changed = false;
                for (uint8_t k = 0; k < node->onoff_count; k++) {
                    if (node->onoff_offsets[k] != node->level_offsets[i]) continue;
                    onoff_changed = !(snap->has_snapshot && snap->onoff_states[k] == node->onoff_states[k]);
                    matched_onoff = node->onoff_states[k];
                    break;
                }
                // Override detection: il companion (o altra sorgente esterna) ha
                // modificato la lampada senza che noi lo abbiamo comandato.
                // Caso 1 (level_changed): il Level arc power e' diverso dall'ultimo
                //   valore che abbiamo comandato noi.
                // Caso 2 (ov_onoff, DALI): level invariato ma OnOff e' cambiato —
                //   i moduli DALI mantengono arc power non-zero anche da spenti
                //   (Generic OnOff OFF non tocca il Level), quindi level_changed=false
                //   anche se il companion ha spento/acceso la lampada. Rileviamo il
                //   mismatch confrontando lo stato onoff attuale con quello implicato
                //   dall'ultimo level che abbiamo comandato noi.
                bool ov_level = level_changed && s_cmd_level_valid[n][i] &&
                                s_cmd_level_val[n][i] != node->level_states[i];
                bool ov_onoff = !ov_level && onoff_changed && matched_onoff != 255 &&
                                s_cmd_level_valid[n][i] &&
                                ((s_cmd_level_val[n][i] > 0 ? 1 : 0) != matched_onoff);
                if (ov_level || ov_onoff) {
                    uint8_t pub_onoff_ov = (matched_onoff != 255) ? matched_onoff
                                         : (node->level_states[i] > 0 ? 1 : 0);
                    int pct_ov = (pub_onoff_ov == 0) ? 0
                               : (node->level_states[i] <= 0) ? 0
                               : (int)((int32_t)node->level_states[i] * 100 / 32767);
                    char omsg[96];
                    snprintf(omsg, sizeof(omsg), "MESHOVERRIDE|%s|%04X|onoff=%d;pct=%d",
                             hub_name,
                             (uint16_t)(node->unicast + node->level_offsets[i]),
                             pub_onoff_ov, pct_ov);
                    mqtt_publish_msg(omsg);
                    // Accetta il nuovo stato (previene re-emissione al prossimo tick):
                    // se e' un ov_onoff con lampada spenta, segna level=0 per coerenza
                    // col nuovo onoff; altrimenti aggiorna al level attuale.
                    s_cmd_level_val[n][i] = (ov_onoff && matched_onoff == 0)
                                           ? 0 : node->level_states[i];
                }
                snap->level_states[i] = node->level_states[i];
                if (!level_changed && !onoff_changed) continue;
                // Usa onoff_states per l'onoff pubblicato: i moduli DALI mantengono
                // arc power non-zero (es. 66%) anche da spenti via OnOff OFF, quindi
                // derivare onoff da level>0 causerebbe Manager.py a credere la lampada
                // accesa anche dopo il comando di spegnimento. Il pct viene forzato a
                // 0 quando la lampada e' logicamente spenta (onoff=0) per coerenza.
                uint8_t pub_onoff = (node->level_states[i] > 0) ? 1 : 0;
                for (uint8_t k = 0; k < node->onoff_count; k++) {
                    if (node->onoff_offsets[k] == node->level_offsets[i]) {
                        pub_onoff = node->onoff_states[k]; break;
                    }
                }
                int pct = (pub_onoff == 0) ? 0
                        : (node->level_states[i] <= 0) ? 0
                        : (int)((int32_t)node->level_states[i] * 100 / 32767);
                char msg[96];
                snprintf(msg, sizeof(msg), "MESH|%s|%04X|onoff=%d;pct=%d", hub_name,
                         (uint16_t)(node->unicast + node->level_offsets[i]),
                         pub_onoff, pct);
                mqtt_publish_msg(msg);
            }
            for (uint8_t i = 0; i < node->onoff_count; i++) {
                bool onoff_changed = !(snap->has_snapshot && snap->onoff_states[i] == node->onoff_states[i]);
                // Se questo stesso elemento ha anche il Level (dimmer), il
                // ciclo sopra l'ha gia' pubblicato (con "pct" in piu') se
                // serviva - niente messaggio duplicato "solo onoff" per lo
                // stesso indirizzo (faceva sparire lo slider lato Manager.py
                // quando arrivava dopo quello con pct).
                bool has_level_too = false;
                for (uint8_t k = 0; k < node->level_count; k++) {
                    if (node->level_offsets[k] == node->onoff_offsets[i]) { has_level_too = true; break; }
                }
                snap->onoff_states[i] = node->onoff_states[i];
                if (has_level_too || !onoff_changed) continue;
                char msg[96];
                snprintf(msg, sizeof(msg), "MESH|%s|%04X|onoff=%d", hub_name,
                         (uint16_t)(node->unicast + node->onoff_offsets[i]), node->onoff_states[i]);
                mqtt_publish_msg(msg);
            }
            // Sensor Server della lampada (potenza/energia): pubblicato sullo
            // STESSO indirizzo onoff/level (non sensor_offsets[0]) cosi'
            // Manager.py lo trova nella stessa voce MESH_STATUS[hub][addr]
            // della lampada invece che in una voce separata da correlare.
            if (node->sensor_count > 0) {
                int32_t pw = node->sensor_power_dw[0];
                int32_t en = node->sensor_energy_wh[0];
                uint16_t lamp_addr = node->level_count > 0
                                   ? (uint16_t)(node->unicast + node->level_offsets[0])
                                   : (uint16_t)(node->unicast + node->onoff_offsets[0]);
                if (pw >= 0 && en >= 0 &&
                    (!snap->has_snapshot || snap->sensor_power_dw != pw || snap->sensor_energy_wh != en)) {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "MESH|%s|%04X|power=%ld.%01ld;energy=%ld.%03ld", hub_name,
                             lamp_addr,
                             (long)(pw / 10), (long)(pw % 10), (long)(en / 1000), (long)(en % 1000));
                    mqtt_publish_msg(msg);
                }
                snap->sensor_power_dw  = pw;
                snap->sensor_energy_wh = en;
            }
        }
        if (node->kind == NODE_SENSOR) {
            int8_t cp; int32_t cl;
            sensor_combined(node, &cp, &cl);
            if (!snap->has_snapshot || snap->sensor_presence != cp || snap->sensor_light_cl != cl) {
                char msg[96];
                snprintf(msg, sizeof(msg), "MESH|%s|%04X|presence=%d;lux=%ld.%02ld", hub_name,
                         node->unicast, cp, (long)(cl / 100), (long)(cl % 100));
                mqtt_publish_msg(msg);
                snap->sensor_presence = cp;
                snap->sensor_light_cl = cl;
            }
        }
        snap->has_snapshot = true;
    }
}

// Pubblica TUTTE le lampade connesse in UN SOLO messaggio JSON leggibile:
// {"hub":"...","count":N,"lamps":[{"name":"...","onoff":1,"pct":46,"w":4.0,
// "wh":0.003},...]}. Le '"' nel payload vengono escapate da modem_mqtt_publish
// (sintassi Air780E \22, vedi modem_mqtt.c) - niente base64 necessario. Niente
// indirizzo (richiesto: troppi dati) - solo il nome assegnato via /setname
// (vuoto se non assegnato). w/wh = null se il nodo non ha Sensor Server di
// potenza.
static void mqtt_publish_lamps_snapshot(void)
{
    static char list[1024];
    int n = 0; list[0] = '\0'; int count = 0;
    for (uint8_t ni = 0; ni < node_count && n < (int)sizeof(list); ni++) {
        lamp_node_t *node = &nodes[ni];
        if (!node->configured || node->is_switch || node->kind == NODE_SENSOR) continue;
        if (node->level_count == 0 && node->onoff_count == 0) continue;

        int onoff; int pct;
        if (node->level_count > 0) {
            onoff = node->level_states[0] > 0 ? 1 : 0;
            pct   = (node->level_states[0] <= 0) ? 0 : (int)((int32_t)node->level_states[0] * 100 / 32767);
        } else {
            onoff = node->onoff_states[0] ? 1 : 0;
            pct   = onoff ? 100 : 0;
        }
        int32_t pw = node->sensor_count > 0 ? node->sensor_power_dw[0]  : -1;
        int32_t en = node->sensor_count > 0 ? node->sensor_energy_wh[0] : -1;
        const char *nm = get_node_name(node->unicast);

        n += snprintf(list + n, sizeof(list) - n, "%s{\"name\":\"%s\",\"onoff\":%d,\"pct\":%d,",
                      count ? "," : "", nm, onoff, pct);
        if (n >= (int)sizeof(list)) break;
        if (pw >= 0 && en >= 0)
            n += snprintf(list + n, sizeof(list) - n, "\"w\":%ld.%01ld,\"wh\":%ld.%03ld}",
                          (long)(pw / 10), (long)(pw % 10), (long)(en / 1000), (long)(en % 1000));
        else
            n += snprintf(list + n, sizeof(list) - n, "\"w\":null,\"wh\":null}");
        if (n >= (int)sizeof(list)) break;
        count++;
    }
    if (n >= (int)sizeof(list)) list[sizeof(list) - 1] = '\0';

    // static, non sullo stack: questa funzione gira nel task esp_timer (dentro
    // poll_cb -> mqtt_periodic_publish), che ha uno stack piccolo - un buffer
    // grande in piu' su quello stack lo faceva traboccare (vedi log crash).
    static char msg[1024 + 160];
    static char last_msg[1024 + 160];
    snprintf(msg, sizeof(msg), "{\"hub\":\"%s\",\"count\":%d,\"lamps\":[%s]}", hub_name, count, list);
    if (strcmp(msg, last_msg) == 0) return; // nessuna variazione, non inviare
    memcpy(last_msg, msg, sizeof(last_msg));
    modem_mqtt_publish(MQTT_PUB_TOPIC_LAMPS, msg);
    ESP_LOGI(TAG, "TX MQTT [%s]: %s", MQTT_PUB_TOPIC_LAMPS, msg);
}

static void build_mesh_addr_list(char *list, int cap)
{
    // snprintf() ritorna quanto AVREBBE scritto anche se troncato: senza
    // questo clamp, una volta che n supera cap, "cap - n" passato come size_t
    // (non negativo, essendo size_t senza segno) diventa un numero enorme e
    // le snprintf successive scrivono ben oltre "list" sullo stack - e' quello
    // che e' successo con nodi NVS corrotti (onoff_count/level_count letti
    // come spazzatura dopo un cambio di layout della struct).
    int n = 0; list[0] = '\0';
    for (uint8_t ni = 0; ni < node_count && n < cap; ni++) {
        lamp_node_t *node = &nodes[ni];
        if (!node->configured || node->is_switch) continue;
        if (node->kind == NODE_SENSOR) {
            n += snprintf(list + n, cap - n, "%s%04X", n ? "," : "", node->unicast);
            continue; // vedi mqtt_publish_node_changes: un Sensore non e' anche una lampada
        }
        uint8_t onoff_n = node->onoff_count > MAX_ONOFF_ELEMENTS ? 0 : node->onoff_count;
        uint8_t level_n = node->level_count > MAX_LEVEL_ELEMENTS ? 0 : node->level_count;
        for (uint8_t i = 0; i < onoff_n && n < cap; i++)
            n += snprintf(list + n, cap - n, "%s%04X", n ? "," : "",
                          (uint16_t)(node->unicast + node->onoff_offsets[i]));
        for (uint8_t i = 0; i < level_n && n < cap; i++)
            n += snprintf(list + n, cap - n, "%s%04X", n ? "," : "",
                          (uint16_t)(node->unicast + node->level_offsets[i]));
    }
    if (n >= cap) list[cap - 1] = '\0'; // sicurezza: garantisce sempre una stringa terminata valida
}

static void mqtt_publish_meshlist(void)
{
    char list[256];
    build_mesh_addr_list(list, sizeof(list));
    char msg[320];
    snprintf(msg, sizeof(msg), "MESHLIST|%s|%s", hub_name, list);
    mqtt_publish_msg(msg);
}

// Pubblica via MQTT come e' composta la rete mesh in questo momento, con i
// nomi assegnati (es. "Cucina:000E"). A differenza di MESHLIST/MESHBOOT
// (solo indirizzi, per la pulizia fantasmi) questo serve a Manager.py per
// sapere QUANTE lampade ci sono e CHE NOME mostrare. Richiamata sia dal
// bottone "Salva configurazione mesh" (/meshsave) sia da un CONFIGREQ
// (Manager.py che chiede la config perche' e' appena partito e non ha nulla).
static int publish_meshconfig(void)
{
    char list[512] = ""; int n = 0; int count = 0;
    for (uint8_t ni = 0; ni < node_count; ni++) {
        lamp_node_t *node = &nodes[ni];
        if (!node->configured || node->is_switch || node->kind == NODE_SENSOR) continue;
        const char *nmv = get_node_name(node->unicast);
        const char *nm = nmv[0] ? nmv : "Lampada";
        // Una lampada = UN elemento (quello con onoff+level insieme, il
        // dimmer): gli altri elementi solo-onoff dello stesso nodo sono
        // uscite extra (es. relè ausiliari sullo stesso modulo), non vanno
        // contati come lampade separate - prima sbagliava cosi' (25 invece
        // di 5: 4 onoff + 1 level per ogni nodo, tutti contati uno per uno).
        if (node->level_count > 0) {
            n += snprintf(list + n, sizeof(list) - n, "%s%s:%04X", n ? "," : "", nm,
                          (uint16_t)(node->unicast + node->level_offsets[0]));
            count++;
        } else if (node->onoff_count > 0) {
            // Nessun dimmer su questo nodo: fallback al primo onoff (es.
            // lampada solo on/off, senza Level Server).
            n += snprintf(list + n, sizeof(list) - n, "%s%s:%04X", n ? "," : "", nm,
                          (uint16_t)(node->unicast + node->onoff_offsets[0]));
            count++;
        }
    }
    char msg[600];
    snprintf(msg, sizeof(msg), "MESHCONFIG|%s|%d|%s", hub_name, count, list);
    mqtt_publish_msg(msg);
    return count;
}

// MESHSENSORCFG|hub|addr1:nome1,addr2:nome2,... - nomi assegnati ai nodi
// Sensor Server della mesh (PIR/lux): publish_meshconfig() li esclude (sono
// "SENSORE", non "LAMPADA"), quindi senza questo il nome impostato con
// /setname per uno di questi nodi resta visibile solo sulla pagina web del
// gateway e non arriva mai a Manager.py.
static void publish_mesh_sensor_names(void)
{
    char list[256]; int n = 0;
    for (uint8_t ni = 0; ni < node_count; ni++) {
        lamp_node_t *node = &nodes[ni];
        if (!node->configured || node->is_switch || node->kind != NODE_SENSOR) continue;
        const char *nm = get_node_name(node->unicast);
        if (!nm[0]) continue;
        n += snprintf(list + n, sizeof(list) - n, "%s%04X:%s", n ? "," : "", node->unicast, nm);
        if (n >= (int)sizeof(list)) break;
    }
    if (n == 0) return;
    char msg[320];
    snprintf(msg, sizeof(msg), "MESHSENSORCFG|%s|%s", hub_name, list);
    mqtt_publish_msg(msg);
}

// CFG:MESHSAVE — repubblica su MQTT nomi+stato di tutte le lampade/sensori
// mesh (es. dopo aver rinominato qualcosa dalla PWA, per non aspettare il
// prossimo CONFIGREQ). Non e' nel contratto della PWA originale, aggiunta
// insieme a SETHUBNAME/RELAYCFG/SENSORCFG per lo stesso motivo.
static void cfg_meshsave(const char *line)
{
    (void)line;
    publish_meshconfig();
    publish_mesh_sensor_names();
    force_full_mqtt_resync(); // rimanda anche tutto lo stato onoff/level/sensori, non solo i nomi
    cfg_ok("MESHSAVE");
}

// CFG:STATUS — stato live di relè e sensori BLE classici (equivalente al
// vecchio /api/status, polling separato da CFG:STATE: relè/sensori BLE non
// hanno a che fare con la mesh). Non nel contratto della PWA originale.
static void cfg_status(const char *line)
{
    (void)line;
    char m[256];
    usb_cdc_send_line("CFG:STATUS_START");
    for (int i = 0; i < RELAY_COUNT; i++) {
        snprintf(m, sizeof(m), "CFG:RELAY;n=%d;enabled=%d;on=%d",
                 i, relay_is_enabled(i) ? 1 : 0, relay_get_state(i) ? 1 : 0);
        usb_cdc_send_line(m);
    }
    // "rules" e "last" possono contenere ';' al loro interno (rules: piu'
    // regole separate da ';'; last: "label=val;label2=val2;rssi=N") - vanno
    // su righe separate, da SOLE, cosi' lato client si puo' prendere tutto
    // cio' che segue "rules=root"/"last=" senza fermarsi al primo ';'
    // (stesso motivo per cui "qr=" in CFG:PAIR/ADDDEV e' sempre l'ultimo campo).
    for (int i = 0; i < BLE_CLASSIC_MAX_SENSORS; i++) {
        char mac[24] = "", rules[160] = "", last[160] = "", name[24] = "";
        ble_classic_get_config(i, mac, sizeof(mac), rules, sizeof(rules));
        ble_classic_get_last(i, last, sizeof(last));
        ble_classic_get_name(i, name, sizeof(name));
        snprintf(m, sizeof(m), "CFG:BLESENSOR;slot=%d;configured=%d;name=%s;mac=%s",
                 i, mac[0] ? 1 : 0, name, mac);
        usb_cdc_send_line(m);
        snprintf(m, sizeof(m), "CFG:BLERULES;slot=%d;rules=%s", i, rules);
        usb_cdc_send_line(m);
        snprintf(m, sizeof(m), "CFG:BLELAST;slot=%d;last=%s", i, last);
        usb_cdc_send_line(m);
    }
    usb_cdc_send_line("CFG:STATUS_END");
}

// CFG:RELAYSET;n=N;val=0|1 — accende/spegne un relè (stato live, non
// l'abilitazione - vedi CFG:RELAYCFG per quella)
static void cfg_relayset(const char *line)
{
    int n = -1, val = -1;
    if (!cfg_kv_int(line, "n", &n) || !cfg_kv_int(line, "val", &val)) {
        cfg_err("RELAYSET", "n e val richiesti"); return;
    }
    if (n < 0 || n >= RELAY_COUNT) { cfg_err("RELAYSET", "indice relè non valido"); return; }
    relay_set(n, val != 0);
    cfg_ok("RELAYSET");
}

// CFG:SNIFFERSTART / CFG:SNIFFERSTOP — vedi ble_classic_handler.c. Mentre
// attivo, mesh e scan periodico dei sensori configurati sono in pausa (un
// solo radio condiviso).
static void cfg_snifferstart(const char *line)
{
    (void)line;
    ble_classic_sniffer_start();
    cfg_ok("SNIFFERSTART");
}

static void cfg_snifferstop(const char *line)
{
    (void)line;
    ble_classic_sniffer_stop();
    cfg_ok("SNIFFERSTOP");
}

// CFG:SNIFFERDATA — dump dei dispositivi visti finora dallo sniffer (la PWA
// la richiama a polling, es. ogni 1s, mentre lo sniffer e' attivo).
static void cfg_snifferdata(const char *line)
{
    (void)line;
    usb_cdc_send_line("CFG:SNIFF_START");
    char mac[13], name[32], svc_hex[42], mfr_hex[42];
    int rssi; uint16_t svc_uuid;
    int count = ble_classic_sniffer_count();
    for (int i = 0; i < count; i++) {
        if (!ble_classic_sniffer_get_dev(i, mac, name, &rssi, &svc_uuid, svc_hex, mfr_hex)) continue;
        for (char *c = name; *c; c++) if (*c == ';' || *c == '"') *c = ' '; // niente ';' nel nome (non sanitizzato a monte per questo carattere)
        char m[220]; // mac(12)+name(31)+svc_hex(41)+mfr_hex(41)+letterale ~50, 160 troncava (-Werror=format-truncation)
        snprintf(m, sizeof(m), "CFG:SNIFFDEV;mac=%s;name=%s;rssi=%d;svc_uuid=%04X;svc_hex=%s;mfr_hex=%s",
                 mac, name, rssi, svc_uuid, svc_hex, mfr_hex);
        usb_cdc_send_line(m);
    }
    usb_cdc_send_line("CFG:SNIFF_END");
}

// Annuncio "appena connesso", come CAPS per i rele': dichiara UNA VOLTA,
// alla prima connessione MQTT, tutti gli indirizzi mesh che il gateway
// considera reali in questo momento. A differenza di MESHLIST (periodico,
// tollerante a dump persi) Manager.py lo tratta come fonte di verita'
// assoluta e immediata, per spazzare via i fantasmi appena il gateway si
// ricollega, senza aspettare i 3 "miss" di MESHLIST.
static bool mqtt_meshboot_sent = false;
static void mqtt_publish_meshboot(void)
{
    char list[256];
    build_mesh_addr_list(list, sizeof(list));
    char msg[320];
    snprintf(msg, sizeof(msg), "MESHBOOT|%s|%s", hub_name, list);
    mqtt_publish_msg(msg);
}

static void publish_relaystate(void)
{
    char bits[RELAY_COUNT + 1];
    for (int i = 0; i < RELAY_COUNT; i++) bits[i] = relay_get_state(i) ? '1' : '0';
    bits[RELAY_COUNT] = '\0';
    char msg[64];
    snprintf(msg, sizeof(msg), "RELAYSTATE|%s|%s", hub_name, bits);
    mqtt_publish_msg(msg);
    strncpy(mqtt_last_relay_bits, bits, sizeof(mqtt_last_relay_bits) - 1);
}

static void publish_caps(void)
{
    char pins[RELAY_COUNT * 2 + 8];
    int n = 0;
    for (int i = 0; i < RELAY_COUNT; i++)
        n += snprintf(pins + n, sizeof(pins) - n, "%s%d", i ? "," : "", relay_is_enabled(i) ? 1 : 0);
    char msg[96];
    snprintf(msg, sizeof(msg), "CAPS|%s|%s", hub_name, pins);
    mqtt_publish_msg(msg);
    mqtt_caps_sent = true;
}

// SENSORCFG|hub|id1:nome1,id2:nome2,... - nomi assegnati ai sensori BLE
// classici (pagina "/"), stesso ruolo di publish_meshconfig() ma per gli
// slot sensore: Manager.py la usa per mostrare il nome invece di "Sensore Sx".
static void publish_sensorcfg(void)
{
    char list[256];
    int n = 0;
    for (int i = 0; i < BLE_CLASSIC_MAX_SENSORS; i++) {
        char mac[24], rules[160], name[24];
        ble_classic_get_config(i, mac, sizeof(mac), rules, sizeof(rules));
        if (mac[0] == '\0') continue;
        ble_classic_get_name(i, name, sizeof(name));
        if (name[0] == '\0') continue;
        n += snprintf(list + n, sizeof(list) - n, "%s%d:%s", n ? "," : "", i + 1, name);
        if (n >= (int)sizeof(list)) break;
    }
    if (n == 0) return;
    char msg[320];
    snprintf(msg, sizeof(msg), "SENSORCFG|%s|%s", hub_name, list);
    mqtt_publish_msg(msg);
}

static void mqtt_publish_relay_and_sensors(void)
{
    char bits[RELAY_COUNT + 1];
    for (int i = 0; i < RELAY_COUNT; i++) bits[i] = relay_get_state(i) ? '1' : '0';
    bits[RELAY_COUNT] = '\0';
    if (strcmp(bits, mqtt_last_relay_bits) != 0) publish_relaystate();

    if (!mqtt_caps_sent && modem_mqtt_is_connected()) publish_caps();
}

// Separata da mqtt_publish_relay_and_sensors() per priorita' MQTT: i sensori
// BLE classico (beacon temp/hum) vengono pubblicati DOPO le variazioni mesh,
// cosi' se la coda MQTT e' piena i dati mesh hanno la precedenza.
static void mqtt_publish_ble_sensors(void)
{
    for (int i = 0; i < BLE_CLASSIC_MAX_SENSORS; i++) {
        char last[160];
        ble_classic_get_last(i, last, sizeof(last));
        if (last[0] == '\0' || strcmp(last, mqtt_last_sensor[i]) == 0) continue;
        char msg[256];
        snprintf(msg, sizeof(msg), "SENSOR|%s|%d|%s", hub_name, i + 1, last);
        mqtt_publish_msg(msg);
        strncpy(mqtt_last_sensor[i], last, sizeof(mqtt_last_sensor[i]) - 1);
    }
}

// Chiamata da poll_cb ogni 2s: diff-pubblica relè/mesh/sensori, e ogni 5
// minuti ripubblica il manifest MESHLIST (stessa cadenza dell'originale).
static void mqtt_periodic_publish(void)
{
    // Niente publish mentre e' in corso un provisioning/pairing (config_busy/
    // pair_active): stesso motivo di prima quando il gate era sulla modalita'
    // WiFi (non esiste piu', tolto il WiFi) - uno stato a meta' configurazione
    // non deve uscire su MQTT mentre cambia sotto mano.
    if (config_busy || pair_active) return;
    // Modalita' USB attiva (long-press BOOT, vedi user_btn_poll): ricalca
    // esattamente il vecchio comportamento SoftAP, MQTT in pausa finche' non
    // si torna in modalita' normale con un altro long-press.
    if (g_usb_mode_active) return;
    if (!modem_mqtt_is_connected()) return;
    if (!mqtt_meshboot_sent) {
        mqtt_publish_meshboot();
        mqtt_meshboot_sent = true;
    }
    mqtt_publish_relay_and_sensors(); // relay + CAPS (veloci, priorità alta)
    mqtt_publish_node_changes();      // mesh lampade/sensori (priorità alta)
    mqtt_publish_ble_sensors();       // beacon BLE classico (priorità bassa, dopo il mesh)

    // 8s (era 15s, era stato alzato da 5s perché a 5s la sessione MQTT cadeva
    // per contesa sulla UART del modem): 8s e' un buon compromesso tra
    // reattivita' e stabilita' della connessione.
    static int64_t last_lamps_us = 0;
    int64_t now_lamps = esp_timer_get_time();
    if (now_lamps - last_lamps_us > 8LL * 1000 * 1000) {
        mqtt_publish_lamps_snapshot();
        last_lamps_us = now_lamps;
    }

    static int64_t last_meshlist_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_meshlist_us > 5LL * 60 * 1000 * 1000) {
        mqtt_publish_meshlist();
        last_meshlist_us = now;
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    usb_cdc_init(); // sostituisce wifi_setup_softap()+start_webserver(): niente piu' WiFi/HTTP, solo USB CDC (vedi usb_cdc.c)
    rgb_led_init();
    rgb_led_set(0, 25, 0); // verde fisso = mesh attivo (modalita' normale di default al boot, vedi user_btn_poll per il blu USB)
    user_btn_init(); // BOOT (GPIO0): long-press toggla la modalita' USB, vedi user_btn_poll() in poll_cb
    relay_init(); // GPIO relay, indipendente dal radio - vedi relay_handler.c

    esp_timer_create_args_t timer_args = {
        .callback = config_delay_cb,
        .name     = "cfg_delay",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &config_delay_timer));

    esp_timer_create_args_t poll_args = {
        .callback = poll_cb,
        .name     = "poll",
    };
    ESP_ERROR_CHECK(esp_timer_create(&poll_args, &poll_timer));
    // Tornato a 2s: l'1s velocizzava il refresh sensori ma il client mesh
    // accetta una sola richiesta in volo per destinazione (vedi commento su
    // POLL_BATCH_SIZE) - con GET di polling sparati il doppio delle volte, i
    // comandi manuali (onoff/level) restavano in coda dietro il polling molto
    // piu' spesso, richiedendo decine di tentativi prima di passare. La
    // reattivita' dei comandi manuali vale piu' della velocita' di refresh.
    ESP_ERROR_CHECK(esp_timer_start_periodic(poll_timer, 2000000));

    ESP_ERROR_CHECK(bluetooth_init());
    esp_read_mac(dev_uuid, ESP_MAC_BT);
    ble_classic_init(); // scan beacon temp/hum a basso duty-cycle, vedi ble_classic_handler.c

    esp_ble_mesh_register_prov_callback(prov_callback);
    esp_ble_mesh_register_config_client_callback(config_client_callback);
    esp_ble_mesh_register_generic_client_callback(generic_client_callback);
    esp_ble_mesh_register_sensor_client_callback(sensor_client_callback);
    esp_ble_mesh_register_custom_model_callback(custom_model_callback);

    // Caricare i nodi salvati PRIMA di esp_ble_mesh_init(), per alzare
    // prov.prov_start_address oltre l'ultimo indirizzo gia' in uso.
    // Con CONFIG_BLE_MESH_SETTINGS abilitato lo stack persiste anche la
    // propria tabella di indirizzi in NVS, ma impostare prov_start_address
    // correttamente e' comunque necessario al primo boot dopo il reflash
    // (NVS stack vuota, solo gw_nodes valido). load_nodes_nvs() usa solo NVS
    // propria, nessuna dipendenza dallo stack BLE Mesh: sicuro chiamarla qui.
    load_nodes_nvs();
    {
        uint16_t max_end = prov.prov_start_address;
        for (uint8_t i = 0; i < node_count; i++) {
            uint16_t end = nodes[i].unicast + nodes[i].elem_num;
            if (end > max_end) max_end = end;
        }
        prov.prov_start_address = max_end;
        ESP_LOGI(TAG, "prov_start_address impostato a 0x%04x (in base a %d nodi gia' noti).",
                 prov.prov_start_address, node_count);
    }

    ESP_ERROR_CHECK(esp_ble_mesh_init(&prov, &composition));

    // Sempre attivo: disabilitarlo (anche solo per scoprire nuovi nodi) rompe
    // la ricezione di TUTTI i messaggi mesh, non solo il discovery - provato:
    // con provisioner disabilitato, ogni GET Level/Sensor va in timeout. Il
    // radio va condiviso a finestre col scanner BLE classico (vedi
    // mesh_pause_for_ble_scan/mesh_resume_after_ble_scan piu' sotto, pilotate
    // da ble_classic_handler.c).
    ESP_ERROR_CHECK(esp_ble_mesh_provisioner_prov_enable(
        ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));

    esp_err_t ak = esp_ble_mesh_provisioner_add_local_app_key(fixed_app_key, 0, 0);
    ESP_LOGI(TAG, "add_local_app_key() -> 0x%x", ak);

    // load_nodes_nvs() gia' chiamata sopra (prima di esp_ble_mesh_init, per
    // poter impostare prov_start_address) - qui resta solo l'altro NVS.
    load_unified_config_nvs();

    xTaskCreatePinnedToCore(uart_bridge_task, "uart_bridge", 4096, NULL, 5, NULL, 1);

    // Ponte MQTT via modem cellulare Air780E (GPIO15=TX/GPIO16=RX), stesso
    // broker/topic/formato del Manager.py esistente - vedi commenti sopra
    // mqtt_periodic_publish(). hub_name e' gia' caricato da NVS qui sopra.
    modem_mqtt_set_rx_callback(mqtt_rx_handler);
    modem_mqtt_set_recovery_callback(modem_recovery_handler);
    modem_mqtt_set_log_callback(usb_cdc_send_line); // log modem UART -> PWA
    modem_mqtt_init(hub_name, MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_SUB_TOPIC);
}
