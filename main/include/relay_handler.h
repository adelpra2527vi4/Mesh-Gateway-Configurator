#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RELAY_COUNT 6

void relay_init(void);
void relay_set(uint8_t idx, bool on);
bool relay_get_state(uint8_t idx);

// Legge il livello GPIO realmente impostato sul pin (-1 se idx invalido).
// Diagnostica: a differenza di relay_get_state() (cache software) verifica
// cosa il driver GPIO ha davvero scritto sul pin.
int relay_get_gpio_level(uint8_t idx);

// Relè non abilitati nel wizard di setup vengono ignorati da relay_set()
// (stessa logica di relayEnabled/wizardPinAbilitati nel relay-controller originale).
void relay_set_enabled(uint8_t idx, bool enabled);
bool relay_is_enabled(uint8_t idx);
