#pragma once

#include <stdbool.h>
#include <stdint.h>

// Driver generico per il modem cellulare Air780E via comandi AT (set "Luat"),
// usato come trasporto MQTT quando non c'e' WiFi con accesso a internet.
// Stessa sequenza AT di B_ModemMQTT.ino (ESP32-terminal-controller-main):
// ATE0 x2 -> attesa registrazione rete (AT+CGREG?) -> AT+MCONFIG -> AT+MQTTMSGSET=0
// -> AT+MIPSTART -> AT+MCONNECT -> AT+MSUB. Gestisce anche la riconnessione
// automatica (errore "767" = sessione MQTT caduta, TCP ancora valido).
//
// UART dedicata (vedi client_definitivo_relay.ino, EXT_SERIAL_TX/RX): GPIO15=TX,
// GPIO16=RX, 115200 8N1 verso il modem.

// client_id: usato in AT+MCONFIG (tipicamente l'hub_name del gateway).
// broker_host/port, sub_topic: server e topic di sottoscrizione MQTT.
void modem_mqtt_init(const char *client_id, const char *broker_host,
                     uint16_t broker_port, const char *sub_topic);

bool modem_mqtt_is_connected(void);

// Pubblica un payload sul topic indicato (AT+MPUB). Le virgolette doppie e i
// backslash nel payload (es. JSON) vengono escapati automaticamente con la
// sintassi Air780E (\22 per '"', \5C per '\', non lo stile C \"). Troncato se
// troppo lungo per il buffer interno del modem (vedi pub_item_t.payload in
// modem_mqtt.c).
void modem_mqtt_publish(const char *topic, const char *payload);

// Callback invocata per ogni messaggio ricevuto sul topic sottoscritto
// (payload gia' estratto dalla riga "+MSUB:...").
typedef void (*modem_mqtt_rx_cb_t)(const char *payload);
void modem_mqtt_set_rx_callback(modem_mqtt_rx_cb_t cb);

// Callback invocata UNA VOLTA, subito dopo che la sessione MQTT torna su in
// seguito a una caduta (non al primo aggancio dopo il boot): serve a
// segnalare via MQTT stesso quanto e' durato un buco di connettivita' (es.
// campo cellulare assente) anche se nessuno aveva il monitor seriale aperto
// in quel momento - vedi MODEMRECOVERY in main.c.
typedef void (*modem_mqtt_recovery_cb_t)(uint32_t downtime_sec);
void modem_mqtt_set_recovery_callback(modem_mqtt_recovery_cb_t cb);

// Callback opzionale per il log diagnostico del modem: invocata per ogni riga
// non-OK ricevuta dalla UART del modem (+MSUB, 767, ERROR, CONNACK OK, ecc.).
// Utile quando il monitor IDF non e' accessibile (es. USB occupata dalla PWA):
// basta collegare usb_cdc_send_line per vedere il traffico modem nella PWA.
typedef void (*modem_mqtt_log_cb_t)(const char *line);
void modem_mqtt_set_log_callback(modem_mqtt_log_cb_t cb);
