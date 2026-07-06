# Architettura del Sistema BLE Mesh Gateway

## Indice
1. [Panoramica](#1-panoramica)
2. [Hardware](#2-hardware)
3. [Firmware ESP32-S3 — `main.c`](#3-firmware-esp32-s3--mainc)
   - 3.1 Strutture dati principali
   - 3.2 Flusso di provisioning BLE Mesh
   - 3.3 Bind queue e configurazione nodi
   - 3.4 Polling dello stato lampade
   - 3.5 Comandi ONOFF/LEVEL
   - 3.6 Companion switch (Silvair EnOcean proxy)
   - 3.7 Sensori (Sensor Server)
   - 3.8 Protocollo UART interno (legacy / non usato)
   - 3.9 Protocollo USB CDC — comandi CFG:
   - 3.10 Modalità USB (LED blu)
   - 3.11 MQTT via modem cellulare
4. [Manager.py — Raspberry Pi](#4-managerpy--raspberry-pi)
   - 4.1 Strutture dati globali
   - 4.2 MQTT — ricezione messaggi
   - 4.3 Loop di automazione HVAC e lampade mesh
   - 4.4 Logica di override manuale
   - 4.5 Flask web server e API REST
   - 4.6 Nomi e cache
5. [Protocolli di comunicazione](#5-protocolli-di-comunicazione)
   - 5.1 Topic MQTT
   - 5.2 Formato messaggi MQTT
   - 5.3 Protocollo UART interno (legacy)
   - 5.4 Protocollo USB CDC (ESP32 ↔ PWA)
6. [Flussi end-to-end](#6-flussi-end-to-end)
   - 6.1 Provisioning di un nuovo nodo
   - 6.2 Ciclo di polling e pubblicazione MQTT
   - 6.3 Automazione luci: PIR → accensione → spegnimento
   - 6.4 Companion switch: pressione → spegnimento → override
   - 6.5 Cambio tipo nodo (SETKIND): lampada ↔ sensore

---

## 1. Panoramica

Il sistema è composto da due componenti principali:

```
┌──────────────────────────────────────────┐                      ┌──────────────────────────┐
│     ESP32-S3 Waveshare                    │                      │   Raspberry Pi           │
│     (BLE Mesh Provisioner)                │                      │   Manager.py             │
│                                           │   UART               │   (Flask + automazione)  │
│  ┌──────────────────────────┐             │◄─────────────────►  └──────────────────────────┘
│  │  BLE Mesh Network        │             │  Modem 4G
│  │  ┌────────┐ ┌────────┐  │             │  (SIM7600 o simile)   MQTT broker
│  │  │ Lampada│ │ Lampada│  │             │◄────────────────────► 91.241.86.224:1883
│  │  │ DALI   │ │ DALI   │  │             │  topic modem/display  ▲
│  │  └────────┘ └────────┘  │   GPIO      │  topic modem/invio    │
│  │  ┌────────┐             │◄──────────► │  Modulo relay         │
│  │  │Sensore │             │             │  (riscald., AC, luce) ▼
│  │  │ PIR/lux│             │   USB CDC   │                      Raspberry Pi
│  │  └────────┘             │◄──────────► │  PWA Configuratore
│  └──────────────────────────┘             │  (browser PC/tablet)
└──────────────────────────────────────────┘
```

**Responsabilità:**

| Componente | Ruolo |
|---|---|
| **ESP32-S3** | Provisioner BLE Mesh: provisiona i nodi, li configura, legge/comanda lo stato. Pubblica via MQTT tramite modem cellulare. Comanda direttamente i relè fisici via GPIO. |
| **Modem Air780E** | Modulo cellulare 4G connesso all'ESP via UART. Fornisce la connettività TCP/IP per il broker MQTT. Gestito da `modem_mqtt.c` (fuori da `main.c`). |
| **Modulo relay** | Relay board esterna comandata direttamente dai GPIO dell'ESP (riscaldamento, AC, luce). Non è un ESP separato. |
| **Manager.py** | Orchestratore su Raspberry Pi: riceve i dati MQTT, esegue l'automazione HVAC e luci, espone la dashboard web e le API REST per il controllo manuale. |
| **PWA** | Progressive Web App servita dall'ESP in modalità USB. Permette il provisioning, la rinomina, il test e la configurazione dei nodi mesh. |

---

## 2. Hardware

### ESP32-S3-Zero (Waveshare)

| Elemento | Pin/Valore | Note |
|---|---|---|
| Pulsante BOOT | GPIO0 | Attivo basso; long-press 2s → toggle modalità USB |
| LED RGB WS2812 | GPIO38 | Driver RMT; verde = mesh attivo, blu = modalità USB |
| Modem 4G TX/RX | UART dedicato | Air780E; gestito da `modem_mqtt.c` (fuori da `main.c`) |
| Relay GPIO | GPIO (modulo esterno) | Pin di comando diretti verso il modulo relay fisico (riscaldamento, AC, luce) |

### LED RGB — significato colori

| Colore | Significato |
|---|---|
| Verde fisso | Normale, mesh attivo |
| Blu fisso | Modalità USB / configuratore |

---

## 3. Firmware ESP32-S3 — `main.c`

Unico file sorgente (~4000 righe). Non è suddiviso in moduli separati: tutte le funzionalità convivono nello stesso translation unit per semplicità di build/flash.

### 3.1 Strutture dati principali

#### `lamp_node_t` — nodo mesh

Struttura che descrive ogni nodo provisionato (lampada, sensore, switch).

```c
typedef struct {
    uint16_t unicast;        // indirizzo unicast BLE Mesh base
    uint8_t  elem_num;       // numero di elementi del nodo
    bool     configured;     // bind queue completato con successo
    bool     failed;         // bind fallito (MAX_CFG_RETRY superato)
    bool     is_switch;      // nodo puro OnOff Client (switch fisico)

    // Elementi OnOff Server (lampade)
    uint8_t  onoff_offsets[8];   // offset dall'unicast base per ogni elemento
    uint8_t  onoff_states[8];    // stato corrente (0/1)
    uint8_t  onoff_count;

    // Elementi Level Server (dimmer)
    uint8_t  level_offsets[8];
    int16_t  level_states[8];    // -32768..32767 (Generic Level)
    uint8_t  level_count;

    // Elementi OnOff Client (companion)
    uint8_t  cli_offsets[8];
    uint8_t  cli_count;
    bool     companion_paired;
    uint16_t companion_lamp_elem;

    // Elementi Sensor Server (PIR/lux/potenza/energia)
    uint8_t  sensor_offsets[4];
    uint8_t  sensor_count;
    int8_t   sensor_presence[4]; // -1=sconosciuto, 0/1
    int32_t  sensor_light_cl[4]; // centilux (lux×100), -1=sconosciuto
    int32_t  sensor_power_dw[4]; // deciwatt (W×10), -1=sconosciuto
    int32_t  sensor_energy_wh[4];// Wh, -1=sconosciuto
    int32_t  sensor_light_cal;   // offset calibrazione luce (centilux)

    uint8_t  kind;               // NODE_LAMP=0 / NODE_SENSOR=1
    uint8_t  uuid[16];           // UUID fisso di fabbrica (salvato al provisioning)
} lamp_node_t;
```

- `nodes[]`: array di massimo 15 nodi (`MAX_NODES`)
- `node_count`: numero di nodi validi nell'array

I nomi dei nodi sono in una struttura separata (`name_addrs[]` + `node_names[]`) indicizzata per indirizzo unicast, **non** per indice nell'array — per evitare la rotazione degli indici dopo `CFG:FORGET`.

#### `bind_entry_t` — coda di operazioni di configurazione

```c
typedef struct {
    uint16_t  elem_addr;   // indirizzo elemento destinatario
    uint16_t  model_id;    // ID del modello da configurare
    bind_op_t op;          // tipo di operazione (vedi enum)
    uint16_t  group_addr;  // per SUB_ADD/SUB_DEL/PUB_SET
} bind_entry_t;

typedef enum {
    BIND_APP_KEY,   // lega la AppKey al modello
    BIND_PUB_SET,   // imposta publish address (solo per switch)
    BIND_SUB_ADD,   // aggiunge subscription a un gruppo
    BIND_SUB_DEL,   // rimuove subscription da un gruppo
    BIND_RELAY_SET, // abilita Relay feature sul nodo
} bind_op_t;
```

La coda (`bind_queue[]`, max 20 elementi) è eseguita **una voce alla volta**, aspettando la risposta (o timeout + retry) prima di passare alla successiva. Questo è il meccanismo centrale che rende la configurazione robusta.

#### Flag globali di sincronizzazione

| Variabile | Tipo | Significato |
|---|---|---|
| `config_busy` | bool | bind queue in corso (anche per companion) |
| `pair_active` | bool | pairing companion switch in corso |
| `g_node_provisioning_active` | bool | handshake BLE provisioning in corso |
| `g_usb_mode_active` | bool | ESP in modalità USB (LED blu) |
| `g_mesh_poll_paused` | bool | polling BLE Mesh sospeso (scan classico in corso) |

#### Tracking comandi inviati (per MESHOVERRIDE detection)

```c
bool    s_cmd_level_valid[MAX_NODES][MAX_LEVEL_ELEMENTS];
int16_t s_cmd_level_val  [MAX_NODES][MAX_LEVEL_ELEMENTS];
```

Registra l'ultimo livello/onoff comandato dal gateway a ogni elemento lampada. Se al successivo poll il livello letto differisce, qualcuno (companion switch o utente fisico) ha cambiato lo stato → il gateway pubblica `MESHOVERRIDE` su MQTT.

---

### 3.2 Flusso di provisioning BLE Mesh

Il provisioning si attiva tramite il comando USB CDC `CFG:PROVISION;uuid=<hex>` dalla PWA.

```
PWA                  ESP32 (Gateway)                Nuovo Nodo
 │                        │                              │
 │── CFG:PROVISION ───────►│                              │
 │                        │── add_unprov_dev() ──────────►│
 │                        │    (g_node_provisioning_active=true)
 │                        │◄── Provisioning handshake ────│
 │                        │    PROV_COMPLETE_EVT          │
 │                        │    (aggiunge nodo in nodes[]) │
 │                        │◄── PROV_LINK_CLOSE_EVT ───────│
 │                        │                              │
 │                        │    Aspetta 3 secondi          │
 │                        │    (config_delay_timer)       │
 │                        │                              │
 │                        │ ╔════════════ CONFIGURAZIONE ══════════════╗
 │                        │ ║ 1. Composition Data Get                  ║
 │                        │ ║    (legge modelli disponibili)           ║
 │                        │ ║ 2. App Key Add                           ║
 │                        │ ║    (installa la chiave di rete)          ║
 │                        │ ║ 3. Bind Queue sequenziale:               ║
 │                        │ ║    a) RELAY_SET (abilita relay)          ║
 │                        │ ║    b) APP_BIND per ogni modello          ║
 │                        │ ║    c) SUB_ADD a GROUP_ALL (0xC000)       ║
 │                        │ ║       per OnOff e Level Server           ║
 │                        │ ╚══════════════════════════════════════════╝
 │                        │
 │◄── CFG:NODE; ... ───────│  (stato pubblicato su USB CDC)
```

**Watchdog:** Se `PROV_COMPLETE_EVT` non arriva entro 25s, il gateway chiama `esp_ble_mesh_provisioner_delete_node_with_uuid()` e resetta `g_node_provisioning_active`. Se `config_busy` rimane bloccata per 45s, viene forzato a false.

**OOB statico (per switch con QR code):** Il comando `CFG:ADDDEV;qr=<QR>` estrae il campo `Z` (32 hex = 16 byte) dal QR Sunricher e lo carica in `oob_buf[]`. Viene usato al prossimo provisioning che richiede Static OOB.

---

### 3.3 Bind queue e configurazione nodi

#### `build_bind_queue(node, base)` — costruzione della coda

Per ogni nodo non-switch la coda è sempre nella stessa sequenza:

1. `BIND_RELAY_SET` — abilita il Relay feature (relay_retransmit=0x11: 1 retry, 30ms intervallo)
2. `BIND_APP_KEY` — per ogni elemento OnOff Server
3. `BIND_APP_KEY` — per ogni elemento Level Server
4. `BIND_APP_KEY` — per ogni elemento Sensor Server
5. `BIND_SUB_ADD` a `GROUP_ALL=0xC000` — per ogni elemento OnOff Server
6. `BIND_SUB_ADD` a `GROUP_ALL=0xC000` — per ogni elemento Level Server

Per gli switch invece: `BIND_APP_KEY` + `BIND_PUB_SET` verso `GROUP_ALL` per i client OnOff/Level.

#### `send_next_bind(dst_node)` — invio del prossimo elemento

Legge `bind_queue[bind_idx]` e invia il messaggio BLE Mesh Config Client appropriato. **Non passa mai al successivo automaticamente:** aspetta la callback `config_client_callback` con `SET_STATE_EVT` o `TIMEOUT_EVT`.

#### Retry logic

- Timeout per ogni operazione: 8s (`msg_timeout` nel common param)
- Max retry: 5 (`MAX_CFG_RETRY`)
- Dopo 5 timeout consecutivi: nodo marcato `failed`, si passa al successivo

#### `SETKIND` — cambio tipo nodo (lampada ↔ sensore)

Quando l'utente cambia il tipo di un nodo dalla PWA (`CFG:SETKIND;node=N;kind=K`), oltre ad aggiornare `nodes[ni].kind` in RAM e NVS, il gateway:

- Se il nodo diventa **sensore**: costruisce una mini-coda con `BIND_SUB_DEL` per tutti gli elementi OnOff e Level del nodo → li rimuove da `GROUP_ALL` → il relè integrato non risponde più ai comandi broadcast
- Se il nodo torna **lampada**: costruisce una mini-coda con `BIND_SUB_ADD` → li riaggiunte a `GROUP_ALL`

---

### 3.4 Polling dello stato lampade

Il timer `poll_cb` scatta ogni **2 secondi** e ha due compiti principali:

1. **Debounce pulsante BOOT** (`user_btn_poll()`)
2. **Pubblicazione periodica MQTT** (`mqtt_periodic_publish()`)
3. **Poll stato nodi mesh**

#### Strategia di polling

- **Sensori PIR/lux** (`NODE_SENSOR`): interrogati **ogni tick**, fuori dal round-robin, per ridurre la latenza della rilevazione presenza a ~2s
- **Lampade** (`NODE_LAMP`): round-robin a blocchi di 3 (`POLL_BATCH_SIZE`), con rotazione `poll_idx`. Su tick **alterni** (`poll_phase_sensor`): un tick interroga il Level, il successivo il Sensor Server della lampada (potenza/energia) — mai entrambi nello stesso tick per evitare conflitti sul radio

Con 6 lampade e batch 3, il ciclo completo è ~4s.

#### MESHOVERRIDE detection

Dopo ogni risposta Level Status, se il valore ricevuto differisce dall'ultimo valore comandato dal gateway (`s_cmd_level_val`), viene pubblicato su MQTT:

```
MESHOVERRIDE|<hub>|<addrHex>|onoff=<0|1>;pct=<0-100>
```

Questo segnala a Manager.py che un utente (companion switch o pulsante fisico sulla lampada) ha cambiato lo stato manualmente.

---

### 3.5 Comandi ONOFF/LEVEL

#### Unicast (verso singolo elemento)

- `send_onoff_elem(node_idx, elem_idx, onoff)`: invia `GEN_ONOFF_SET_UNACK`
  - Se si accende (`onoff=1`) e il Level è 0, prima porta il Level al 50% — altrimenti la lampada DALI risulta "logicamente accesa" ma fisicamente buia
  - Aggiorna `onoff_states[]` e `s_cmd_level_val[]` localmente (ottimistico)
- `send_level_elem_ex(...)`: invia `GEN_LEVEL_SET_UNACK` con trans_time e delay opzionali

#### Broadcast (verso GROUP_ALL)

- `send_onoff_group(group_addr, onoff)`: un solo pacchetto radio raggiunge tutte le lampade iscritte → no scaglioni
- `send_level_group_ex(...)`: idem per il dimmer, con parametri di transizione

Tutti i comandi usano `_UNACK` (unacknowledged) per non aspettare i messaggi di risposta, che causerebbero ritardi a cascata.

---

### 3.6 Companion switch (Silvair EnOcean proxy)

Le lampade DALI supportano il protocollo **Silvair EnOcean** (vendor model `0x0136/0x0001`): il companion switch BLE invia beacon EnOcean che la lampada riceve e converte in comandi mesh.

#### Flusso di pairing (`CFG:PAIR;node=N;qr=<QR>`)

1. La PWA legge il QR code del companion e lo invia all'ESP
2. L'ESP estrae dal QR il campo `Z` (chiave 16 byte) e `30S` (MAC 6 byte)
3. **Stage 1**: `BIND_APP_KEY` del vendor model sulla lampada target
4. **SET**: invia `[0x01][chiave_16][mac_6]` al vendor model → la lampada "impara" il companion
5. **Stage 2**: `BIND_APP_KEY` + `BIND_PUB_SET` verso `GROUP_ALL` per i client OnOff/Level dello switch → alla pressione, il companion pubblica su GROUP_ALL

Il companion ha due pulsanti:
- Placca A (elem0): OnOff Client → ON/OFF di tutto il gruppo
- Placca B (elem1): Level Client → DIM (dimmeraggio)

#### Unpairing (`CFG:UNPAIR;node=N`)

Invia `[0x02]` (DELETE) al vendor model della lampada + imposta PUB_SET a `0x0000` (unassigned) per i client dello switch.

---

### 3.7 Sensori (Sensor Server)

Il modello `0x1100` (Sensor Server) è presente su due tipi di nodi:

| Tipo nodo | Dati esposti |
|---|---|
| Sensore PIR/lux (`NODE_SENSOR`) | Presenza (`PROP_PRESENCE_DETECTED=0x004D`) + luce ambientale (`PROP_PRESENT_AMBIENT_LIGHT=0x004E`) |
| Lampada con energy meter | Potenza istantanea (`PROP_PRESENT_INPUT_POWER=0x0052`) + energia cumulata (`PROP_TOTAL_ENERGY=0x0072`) |

Il parsing dei dati "marshalled" (`parse_sensor_status()`) decodifica il formato a-tag BLE Mesh Sensor Status (Format A a 2 byte di header, Format B a 3 byte).

**Calibrazione luce:** `sensor_light_cal` (centilux, int32) è un offset aggiunto al valore grezzo per allinearlo a un luxmetro di riferimento. Salvato in NVS insieme al nodo.

---

### 3.8 Protocollo UART interno (legacy)

Il codice in `main.c` contiene un task `uart_bridge_task` (GPIO4=TX, GPIO5=RX, 115200 8N1) e le funzioni `uart_push_level()`, `uart_push_sensor()`, `bridge_handle_line()` che implementano un protocollo testuale per scambiare stato mesh e comandi con un dispositivo esterno.

> **Nota architetturale:** Non esiste un ESP separato come "relay-controller" collegato via UART. L'ESP32-S3 che gira `main.c` gestisce direttamente i relè fisici onboard. Il codice UART è presente come residuo di una configurazione precedente o come canale di debug/estensione; in produzione i comandi arrivano **esclusivamente via MQTT** (`bridge_handle_line()` viene chiamata internamente dopo aver ricevuto `MESHCMD` dal broker).

Il protocollo UART rimane comunque funzionale: se un dispositivo è fisicamente collegato su GPIO4/5 può inviare comandi nel formato:

```
SET;addr=0006;onoff=1
SET;addr=C000;onoff=0
SET;addr=0007;pct=50;trans=2000;delay=100
GET;addr=0006
DUMP
```

E ricevere aggiornamenti di stato:

```
ONOFF;addr=0006;val=1
LEVEL;addr=0007;val=16383;pct=50
SENSOR;addr=001F;presence=1;lux=235.50
DUMPEND
```

---

### 3.9 Protocollo USB CDC — comandi CFG:

La PWA, aperta con ESP in modalità USB, comunica tramite USB CDC. Ogni messaggio è una riga di testo terminata da `\n`.

#### Comandi dalla PWA → ESP

| Comando | Descrizione |
|---|---|
| `CFG:STATE` | Richiede lo stato completo (nodi + discovered) |
| `CFG:PROVISION;uuid=<hex>` | Provisiona il dispositivo con quell'UUID |
| `CFG:FORGET;node=N` | Rimuove il nodo N dalla mesh e dalla NVS |
| `CFG:SETNAME;node=N;name=<nome>` | Assegna un nome al nodo N |
| `CFG:SETKIND;node=N;kind=K` | Cambia tipo (0=lampada, 1=sensore) |
| `CFG:CMD;node=N;elem=E;val=V` | Comanda OnOff (0/1/2=GET) di un elemento |
| `CFG:LEVEL;node=N;elem=I;val=0-100[;trans;delay]` | Dimmer 0-100% con parametri di transizione |
| `CFG:ADDDEV;qr=<QR>` | Carica OOB statico dal QR di un switch |
| `CFG:PAIR;node=N;qr=<QR>[;kr;mr]` | Abbina un companion switch alla lampada N |
| `CFG:UNPAIR;node=N` | Scollega il companion dalla lampada N |
| `CFG:RESET` | Cancella NVS e riavvia |
| `CFG:REBIND;node=N` | Ripete il bind queue su un nodo già provisionato |
| `CFG:LIGHTCAL;node=N;cal=<centilux>` | Imposta la calibrazione luce del nodo N |

#### Risposte ESP → PWA

```
CFG:STATE_START
CFG:BUSY;true|false
CFG:OOB;true|false
CFG:USBMODE;active=true|false
CFG:NODE;i=N;base=0xADDR;cfg=0|1;fail=0|1;sw=0|1;kind=0|1;grp=0|1;paired=0|1;...
CFG:ELEM;node=N;e=E;addr=0xADDR;on=0|1
CFG:LVL;node=N;li=I;e=E;addr=0xADDR;pct=P
CFG:SENSOR_DATA;node=N;pres=-1|0|1;light=L;hassens=0|1
CFG:DISCOVERED;uuid=<hex>;addr=<hex>;rssi=R;oob=0|1[;known=1;knownname=<nome>]
CFG:STATE_END

CFG:OK;SETKIND
CFG:ERR;SETKIND;nodo non valido
CFG:BUSY;PROVISION

DBG;<messaggio debug>
```

---

### 3.10 Modalità USB (LED blu)

**Attivazione:** long-press del pulsante BOOT (GPIO0) per 2+ secondi.

**Effetti in modalità USB (`g_usb_mode_active = true`):**
- LED RGB → blu fisso
- `mqtt_periodic_publish()` non pubblica (niente stato MESH verso il Raspberry)
- `mqtt_rx_handler()` ignora **tutti** i messaggi MQTT in ingresso (guard al primo rigo della funzione) — nessun comando di automazione può raggiungere la mesh
- Comandi CFG: di scrittura (PROVISION, SETNAME, PAIR, ecc.) sono operativi

**Effetti in modalità normale (`g_usb_mode_active = false`):**
- LED RGB → verde fisso
- `mqtt_periodic_publish()` attivo
- MQTT in ingresso processato normalmente

Un secondo long-press riporta alla modalità normale.

---

### 3.11 MQTT via modem cellulare

Il file `modem_mqtt.h` (implementazione separata, non in questo sorgente) gestisce la connessione TCP via modem 4G. Il gateway pubblica periodicamente (`mqtt_periodic_publish()`) lo stato di tutti i nodi sul topic `modem/display`.

**Broker:** `91.241.86.224:1883`  
**Modem:** Air780E — connesso via UART all'ESP, gestito da `modem_mqtt.c`

| Topic | Dir | Frequenza | Contenuto |
|---|---|---|---|
| `modem/display` | ESP→Pi | ad evento | Stato singoli nodi, MESHLIST, MESHCONFIG, ecc. (testo pipe-separated) |
| `modem/lampade` | ESP→Pi | ogni 8s | JSON snapshot di tutte le lampade (nome, onoff, pct, w, wh) |
| `modem/invio` | Pi→ESP | su richiesta | MESHCMD, RELAYCMD, CONFIGREQ |

---

## 4. Manager.py — Raspberry Pi

Script Python (~2500 righe) che gira come servizio su Raspberry Pi. Combina:
- Client MQTT in background (thread `paho.mqtt`)
- Loop di automazione HVAC e mesh (thread daemon)
- Web server Flask (thread principale)

---

### 4.1 Strutture dati globali

Tutte in memoria RAM (non persistite su disco tranne `config.json` e `names_cache.json`):

| Variabile | Tipo | Contenuto |
|---|---|---|
| `DATA_STORE` | `list[dict]` | Storico sensori classici BLE (max 10.000 righe) |
| `LAST_RELAY_STATUS` | `{hub: "000000"}` | Stato bitmask (0/1) dei 6 relè fisici per hub |
| `ENABLED_RELAYS` | `{hub: [bool×6]}` | Relè abilitati fisicamente (da CAPS) |
| `LAST_MOTION_TIMES` | `{hub: datetime}` | Ultimo orario di rilevazione movimento per hub |
| `MESH_STATUS` | `{hub: {addrHex: {campo: valore, time: str}}}` | Stato lampade/sensori mesh |
| `MESH_LIST_MISSES` | `{hub: {addrHex: int}}` | Contatore MESHLIST miss consecutivi |
| `MESH_LAMP_NAMES` | `{hub: {addrHex: nome}}` | Nomi lampade mesh (da MESHCONFIG) |
| `SENSOR_NAMES` | `{hub: {slotId: nome}}` | Nomi sensori BLE classici (da SENSORCFG) |
| `MESH_SENSOR_NAMES` | `{hub: {addrHex: nome}}` | Nomi sensori mesh PIR/lux (da MESHSENSORCFG) |
| `MANUAL_OVERRIDE_UNTIL` | `{(hub,addr): datetime}` | Override per lampada (da MESHOVERRIDE) |
| `MESH_PROVISIONAL_OVERRIDE` | `{(hub,addr): datetime}` | Override provvisorio (rilevazione companion anticipata) |
| `LAST_AUTO_MESH_STATE` | `{(hub,addr): {onoff,pct}}` | Ultimo stato comandato dall'automazione |
| `HUB_MESH_OVERRIDE_UNTIL` | `{hub: datetime}` | Override a livello hub intero (da prima MESHOVERRIDE ricevuta) |

---

### 4.2 MQTT — ricezione messaggi

`on_message()` processa ogni messaggio su `modem/display`. Formato unificato: `TIPO|hub|campo1|campo2...`

| Tipo messaggio | Azione |
|---|---|
| `SENSOR\|hub\|id\|k=v;...` | Aggiunge a `DATA_STORE` (sensori BLE classici: temp, hum, pir, lux) |
| `MESH\|hub\|addr\|k=v;...` | Aggiorna `MESH_STATUS[hub][addr]` + rileva mismatch → provisional hold |
| `MESHOVERRIDE\|hub\|addr\|k=v` | Imposta `MANUAL_OVERRIDE_UNTIL` e `HUB_MESH_OVERRIDE_UNTIL` per `manual_override_minutes` minuti |
| `MESHLIST\|hub\|addr1,addr2,...` | Rimuove "lampade fantasma" dopo `MESH_LIST_MISS_THRESHOLD` assenze consecutive |
| `MESHBOOT\|hub\|addr1,addr2,...` | Pulizia immediata: rimuove da `MESH_STATUS` tutti gli indirizzi non in lista |
| `MESHCONFIG\|hub\|count\|nome:addr,...` | Aggiorna `MESH_LAMP_NAMES` (sovrascrittura totale, non merge) |
| `SENSORCFG\|hub\|id:nome,...` | Aggiorna `SENSOR_NAMES` |
| `MESHSENSORCFG\|hub\|addr:nome,...` | Aggiorna `MESH_SENSOR_NAMES` |
| `CAPS\|hub\|bitmask` | Aggiorna `ENABLED_RELAYS` |
| `RELAYSTATE\|hub\|bitmask` | Aggiorna `LAST_RELAY_STATUS` |
| `MODEMRECOVERY\|hub\|secondi` | Logga buco di connettività su `modem_recovery.log` |

Al connect MQTT, Manager.py invia `CONFIGREQ|ALL` su `modem/invio`: tutti i gateway rispondono con MESHCONFIG + SENSORCFG + MESHSENSORCFG + RELAYSTATE + CAPS. Un CONFIGREQ periodico (`CONFIGREQ_RESEND_SEC=120s`) gestisce i casi in cui il primo va perso.

---

### 4.3 Loop di automazione HVAC e lampade mesh

`hvac_automation_loop()` gira ogni **2 secondi** in un thread daemon.

#### Logica HVAC (relè fisici)

```
Per ogni area configurata:
  temp_media = media temperatura dei sensori dell'area
  presenza_hvac = c'è stato movimento negli ultimi timeout_hvac_min minuti?

  if stagione == "inverno":
    target = t_inverno se presenza_hvac, altrimenti (t_inverno - 2°C)
    if temp < (target - isteresi): accendi hvac_caldo
    if temp > (target + isteresi): spegni hvac_caldo

  if stagione == "estate":
    target = t_estate se presenza_hvac, altrimenti (t_estate + 2°C)
    if temp > (target + isteresi): accendi hvac_freddo
    if temp < (target - isteresi): spegni hvac_freddo

  Relè tipo "luce": ON se presenza_luci (timeout_luci_min), OFF altrimenti
  Relè tipo "ventola": logica ventilazione in estate
```

#### Logica lampade mesh

```
Per ogni area configurata:
  lux_soglia = area["lux_{giorno|notte}_{stagione}"]
  lux_media = media lux da tutti i sensori mesh dell'area

  Per ogni hub nell'area:
    SKIP se modalita_mesh == "manuale"
    SKIP se HUB_MESH_OVERRIDE_UNTIL[hub] non scaduto        ← hub-level override

    if not presenza_luci:
      Spegni lampade non in override → MESHCMD|hub|C000|onoff|0 (gruppo)

    else:
      pct_target = 100 * max(0, 1 - lux_media / lux_soglia)
      → rampa lineare: più luce ambientale c'è, meno brillano le lampade

      if nessuna lampada in override:
        → MESHCMD|hub|C000|onoff|1   (gruppo)
        → MESHCMD|hub|C000|pct|N     (gruppo)
      else:
        → MESHCMD unicast solo per le lampade non in override
```

I comandi MQTT pubblicati su `modem/invio` vengono ricevuti dall'ESP che chiama `bridge_handle_line()` internamente — la stessa funzione che sarebbe richiamabile via UART, ma il percorso produzione è esclusivamente MQTT.

---

### 4.4 Logica di override manuale

**`MANUAL_OVERRIDE_UNTIL[(hub, addr)]`** — per singola lampada

Impostato quando arriva `MESHOVERRIDE` dal gateway. Dura `manual_override_minutes` minuti (configurabile, default 30). L'automazione salta le lampade con override attivo.

**`MESH_PROVISIONAL_OVERRIDE[(hub, addr)]`** — hold provvisorio

Impostato quando un messaggio `MESH|` riporta uno stato diverso da quello che l'automazione ha comandato (`LAST_AUTO_MESH_STATE`). Dura 8 secondi — tempo sufficiente al `MESHOVERRIDE` di arrivare via cellulare. Se `MESHOVERRIDE` non arriva (falso allarme), l'automazione riprende automaticamente.

**`HUB_MESH_OVERRIDE_UNTIL[hub]`** — override a livello hub intero

La chiave del problema: il companion preme → manda ON/OFF a `GROUP_ALL` → l'ESP genera una `MESHOVERRIDE` **per ogni lampada** → via modem cellulare arrivano scaglionate ogni 0.5-2s. Nel frattempo il loop di automazione (2s) vedeva alcune lampade senza override e le riaccendeva.

**Soluzione:** la prima `MESHOVERRIDE` ricevuta per qualsiasi lampada di un hub setta `HUB_MESH_OVERRIDE_UNTIL[hub] = now + manual_override_minutes`. Il loop salta l'intero hub immediatamente. Le successive MESHOVERRIDE aggiornano solo i timer per-lampada.

---

### 4.5 Flask web server e API REST

#### Pagine HTML

| URL | Descrizione |
|---|---|
| `/` | Redirect a `/tutte` |
| `/tutte` | Dashboard panoramica: tutti i sensori e le aree |
| `/area/<nome>` | Dashboard per area: relè, lampade mesh, sensori |
| `/config` | Pagina di configurazione (aree, ESP, impostazioni generali) |

#### API REST

| URL | Metodo | Descrizione |
|---|---|---|
| `/api/data/<area>` | GET | JSON: sensori, stato relè, lampade mesh, modalità |
| `/api/chart/<area>` | GET | JSON: serie temporali ultime 24h per Chart.js |
| `/api/relay` | POST | Invia comando relè: `RELAYCMD|hub|N|ON/OFF` |
| `/api/mesh` | POST | Invia comando lampada: `MESHCMD|hub|addr|onoff|val` |
| `/api/mesh/<hub>` | GET | Stato MESH_STATUS per un hub |
| `/api/esp_mode` | POST | Imposta modalità auto/manuale per hub (relè o mesh) |
| `/api/config` | POST | Salva `config.json` |
| `/api/mesh_clear` | POST | Svuota `MESH_STATUS` e `MESH_LIST_MISSES` |
| `/api/reboot` | POST | Riavvia il Raspberry Pi (`sudo reboot`) |

La dashboard si aggiorna ogni **2 secondi** tramite polling `/api/data/<area>`. I grafici si aggiornano ogni **60 secondi** tramite `/api/chart/<area>` senza distruggere/ricreare i chart (zoom/pan dell'utente preservato).

#### Comandi manuale lampade mesh (lato JS)

Il client web usa `sendMeshCmdUntilConfirmed()`: invia il comando e ri-invia ogni 1.5s fino a che lo stato confermato da `/api/mesh/<hub>` non corrisponde al valore atteso (max 15 tentativi, ~22s). Compensa il problema "una sola richiesta in volo per destinazione" del client BLE Mesh.

---

### 4.6 Nomi e cache

I nomi (lampade, sensori) vivono solo in RAM ma vengono salvati su `names_cache.json` per sopravvivere ai riavvii di Manager.py. La cache è caricata all'avvio (`load_names_cache()`) ed aggiornata ogni volta che arriva `MESHCONFIG`/`SENSORCFG`/`MESHSENSORCFG`, o quando `MESHBOOT` fa la potatura dei nomi orfani.

Il percorso di verità è sempre l'ESP: `CONFIGREQ` forza la risfedizione completa ad ogni (ri)connessione MQTT.

---

## 5. Protocolli di comunicazione

### 5.1 Topic MQTT

| Topic | Direzione | Descrizione |
|---|---|---|
| `modem/display` | ESP → Raspberry | Dati sensori, stato lampade, manifest mesh (testo pipe-separated) |
| `modem/lampade` | ESP → Raspberry | Snapshot JSON di tutte le lampade, pubblicato ogni 8s |
| `modem/invio` | Raspberry → ESP | Comandi relay, comandi mesh, CONFIGREQ |

### 5.2 Formato messaggi MQTT

#### Da ESP a Raspberry (`modem/display`)

```
SENSOR|<hub>|<id>|temp=22.5;hum=60;rssi=-70
MESH|<hub>|<addrHex>|onoff=1;pct=75;lux=320.50;presence=0
MESH|<hub>|<addrHex>|onoff=0;pct=0;power=12.5;energy=3.14
MESHOVERRIDE|<hub>|<addrHex>|onoff=0;pct=0
MESHLIST|<hub>|0006,0007,000A,000B
MESHBOOT|<hub>|0006,0007,000A,000B
MESHCONFIG|<hub>|4|AltoDX:0006,BassoSX:0007,AltoCentro:000A,BassoSX:000B
SENSORCFG|<hub>|1:Ingresso,2:Soggiorno
MESHSENSORCFG|<hub>|001F:PIR-Corridoio
RELAYSTATE|<hub>|010000
CAPS|<hub>|1,1,1,0,0,0
MODEMRECOVERY|<hub>|47
```

#### Da ESP a Raspberry — snapshot lampade (`modem/lampade`)

Pubblicato ogni **8 secondi**. Payload JSON con tutte le lampade mesh configurate (esclusi sensori e switch), in un singolo messaggio:

```json
{
  "hub": "ESP_XXXXXX",
  "count": 4,
  "lamps": [
    {"name": "Sala Alta DX", "onoff": 1, "pct": 46, "w": 4.0,  "wh": 0.003},
    {"name": "Sala Bassa SX","onoff": 1, "pct": 46, "w": 3.8,  "wh": 0.003},
    {"name": "Corridoio",    "onoff": 0, "pct": 0,  "w": null,  "wh": null},
    {"name": "",             "onoff": 1, "pct": 75, "w": null,  "wh": null}
  ]
}
```

- `name`: nome assegnato via `CFG:SETNAME`; stringa vuota se non assegnato
- `w` / `wh`: potenza istantanea (W) ed energia cumulata (Wh); `null` se il nodo non ha Sensor Server di potenza
- Nessun indirizzo unicast nel payload (solo nome)
- Le virgolette nel JSON vengono escapate con `\22` dalla libreria Air780E in `modem_mqtt.c`

---

#### Da Raspberry a ESP (`modem/invio`)

```
RELAYCMD|<hub>|<N>|ON|OFF
MESHCMD|<hub>|<addrHex>|onoff|1
MESHCMD|<hub>|C000|onoff|0          (comando di gruppo)
MESHCMD|<hub>|<addrHex>|pct|75
MESHCMD|<hub>|C000|pct|50,2000,100  (pct,trans_ms,delay_ms)
CONFIGREQ|ALL
```

### 5.3 Protocollo UART interno (legacy)

GPIO4=TX, GPIO5=RX, 115200 8N1. Non più in uso in produzione (vedi §3.8). I comandi al gateway arrivano via MQTT. Il canale UART rimane disponibile per debug o estensioni future.

### 5.4 Protocollo USB CDC (ESP32 ↔ PWA)

Righe di testo `\n`-terminated su porta USB seriale virtuale.

**PWA → ESP:** comandi `CFG:<CMD>;param=val;...` (vedi §3.9)

**ESP → PWA:** risposte `CFG:OK/ERR/BUSY/STATE...` e `DBG;...` per il debug

---

## 6. Flussi end-to-end

### 6.1 Provisioning di un nuovo nodo

```
1. Utente preme BOOT 2s → LED blu → modalità USB
2. Apre PWA → pagina "Provisioning"
3. [Opzionale] Se switch con QR: scansiona QR → PWA manda CFG:ADDDEV;qr=...
4. Avvicina nodo → compare in "Discovered"
5. Clic "Provisiona" → PWA manda CFG:PROVISION;uuid=<hex>
6. ESP: add_unprov_dev() → handshake BLE (3-10s)
7. PROV_COMPLETE_EVT → nodo inserito in nodes[]
8. PROV_LINK_CLOSE_EVT → attesa 3s → Composition Data Get
9. AppKey Add → RELAY_SET → BIND_APP_KEY × N → SUB_ADD × N
10. nodes[ni].configured = true → save_nodes_nvs()
11. ESP pubblica CFG:NODE;... via USB → PWA mostra il nodo come configurato
```

### 6.2 Ciclo di polling e pubblicazione MQTT

```
poll_cb() ogni 2s:
  1. Debounce pulsante BOOT
  2. mqtt_periodic_publish():
     → pubblica MESH|hub|addr|... per ogni nodo cambiato
     → ogni 5min pubblica MESHLIST e MESHCONFIG
     → se LED blu: NON pubblica nulla
  3. Poll sensori PIR (ogni tick)
  4. Poll lampade Level (tick pari) / Sensor power (tick dispari)
  
on Level STATUS:
  → aggiorna level_states[]
  → se diverso da s_cmd_level_val: pubblica MESHOVERRIDE
  → uart_push_level() → LEVEL;addr=...;val=...;pct=...
```

### 6.3 Automazione luci: PIR → accensione → spegnimento

```
1. Sensore PIR rileva movimento
2. ESP legge Sensor Status → sensor_presence[ei] = 1
3. uart_push_sensor() → SENSOR;addr=001F;presence=1;lux=320.50
4. mqtt_periodic_publish() → MESH|hub|001F|presence=1;lux=320.50
5. Manager.py on_message → LAST_MOTION_TIMES[hub] = now
6. hvac_automation_loop():
   presenza_luci = True (entro timeout_luci_min)
   pct_target = 100 * max(0, 1 - lux_media / lux_soglia)
   → MESHCMD|hub|C000|onoff|1
   → MESHCMD|hub|C000|pct|<pct_target>
7. ESP riceve MESHCMD → bridge_handle_line() → send_level_group()
8. Tutte le lampade si accendono simultaneamente (GROUP_ALL broadcast)

[Timeout scattato, nessun movimento da timeout_luci_min minuti]
9. presenza_luci = False
   → MESHCMD|hub|C000|onoff|0
10. Lampade si spengono
```

### 6.4 Companion switch: pressione → spegnimento → override

```
1. Utente preme companion switch → beacon EnOcean
2. La lampada ricevente converte → pubblica OnOff SET 0 su GROUP_ALL
3. Tutte le lampade si spengono (in mesh, senza passare dal Raspberry)
4. Ogni lampada aggiorna il proprio stato → l'ESP lo legge al poll
5. ESP rileva mismatch con s_cmd_level_val → per ogni lampada:
   → pubblica MESHOVERRIDE|hub|<addr>|onoff=0;pct=0
6. Le MESHOVERRIDE arrivano scaglionate via cellulare (0.5-2s l'una)
7. Manager.py: alla PRIMA MESHOVERRIDE:
   HUB_MESH_OVERRIDE_UNTIL[hub] = now + manual_override_minutes
   MANUAL_OVERRIDE_UNTIL[(hub, addr)] = same
8. Loop automazione vede HUB_MESH_OVERRIDE_UNTIL[hub] non scaduto → SKIP
9. Per i successivi manual_override_minutes minuti, l'automazione non riaccende
10. Alla scadenza, se c'è ancora presenza e poca luce → riaccende normalmente
```

### 6.5 Cambio tipo nodo (SETKIND): lampada ↔ sensore

```
[Sensore provisionato come LAMPADA per default → ha subscription a GROUP_ALL]

1. Utente apre PWA → seleziona "Sensore" dal dropdown del nodo N
2. PWA manda: CFG:SETKIND;node=N;kind=1
3. ESP: cfg_setkind():
   was_sensor = false, now_sensor = true → cambiamento rilevato
   nodes[N].kind = NODE_SENSOR → save_nodes_nvs()
   
   Costruisce bind_queue con BIND_SUB_DEL per ogni elemento OnOff+Level:
   [ {addr=0x0006, model=GEN_ONOFF_SRV, op=SUB_DEL, group=0xC000},
     {addr=0x0007, model=GEN_LEVEL_SRV, op=SUB_DEL, group=0xC000} ]
   
   send_next_bind() → MODEL_SUB_DELETE → nodo risponde con SUB_STATUS
   → bind_idx++ → prossima voce → ... → coda esaurita
   
4. Il nodo mesh ora NON è più iscritto a GROUP_ALL
5. Comando broadcast (companion OFF, automazione spegnimento) → lampade ricevono, sensore NO
6. Il relè integrato del sensore non risponde più ai comandi di luce
```

---

## Appendice — Costanti chiave

| Costante | Valore | Significato |
|---|---|---|
| `GROUP_ALL` | `0xC000` | Indirizzo di gruppo BLE Mesh per tutte le lampade |
| `MAX_NODES` | `15` | Massimo nodi gestibili simultaneamente |
| `MAX_BIND_QUEUE` | `20` | Massimo operazioni in coda per il bind |
| `POLL_BATCH_SIZE` | `3` | Lampade interrogate per tick di polling |
| `poll_cb` interval | `2s` | Tick del timer di polling |
| `DISCOVERED_EXPIRE_US` | `30s` | Scadenza nodi non-provisionati dalla lista |
| `MESH_PRESENCE_MAX_AGE_SEC` | `90s` | Età massima lettura presenza mesh considerata valida |
| `MESH_GHOST_TIMEOUT_SEC` | `660s` | Pulizia automatica lampade fantasma da MESH_STATUS |
| `MESH_PROVISIONAL_HOLD_SEC` | `8s` | Durata hold provvisorio (attesa MESHOVERRIDE via cellulare) |
| `relay_retransmit` | `0x11` | 1 retry, intervallo 30ms (evita congestione radio) |
| `default_ttl` | `7` | Time-to-live messaggi BLE Mesh |
| `CONFIGREQ_RESEND_SEC` | `120s` | Frequenza CONFIGREQ periodico |
| `MQTT_BROKER` | `91.241.86.224:1883` | Broker MQTT pubblico |
