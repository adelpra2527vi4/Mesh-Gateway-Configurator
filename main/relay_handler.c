// Stessi pin/logica del client relay Arduino esistente (client_definitivo_relay.ino):
// GPIO 1,2,41,42,45,46, attivi HIGH. Nessuna interazione con il radio BLE.
#include "relay_handler.h"
#include "driver/gpio.h"

static const gpio_num_t relay_pins[RELAY_COUNT] = {
    GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_45, GPIO_NUM_46
};
static bool relay_state[RELAY_COUNT] = {false};
static bool relay_enabled[RELAY_COUNT] = {false};

void relay_init(void) {
    for (int i = 0; i < RELAY_COUNT; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << relay_pins[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(relay_pins[i], 0);
        relay_state[i] = false;
    }
}

void relay_set_enabled(uint8_t idx, bool enabled) {
    if (idx >= RELAY_COUNT) return;
    relay_enabled[idx] = enabled;
}

bool relay_is_enabled(uint8_t idx) {
    if (idx >= RELAY_COUNT) return false;
    return relay_enabled[idx];
}

void relay_set(uint8_t idx, bool on) {
    if (idx >= RELAY_COUNT) return;
    if (!relay_enabled[idx]) return;
    gpio_set_level(relay_pins[idx], on ? 1 : 0);
    relay_state[idx] = on;
}

bool relay_get_state(uint8_t idx) {
    if (idx >= RELAY_COUNT) return false;
    return relay_state[idx];
}

int relay_get_gpio_level(uint8_t idx) {
    if (idx >= RELAY_COUNT) return -1;
    return gpio_get_level(relay_pins[idx]);
}
