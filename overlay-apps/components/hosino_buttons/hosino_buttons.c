// SPDX-License-Identifier: Apache-2.0
#include "hosino_buttons.h"

#include <stddef.h>
#include <string.h>
#include "driver/gpio.h"

enum {
    BUTTON_COUNT = 3,
    EVENT_CAPACITY = 8,
    DEBOUNCE_MS = 40,
    LONG_PRESS_MS = 800,
    MAX_SAMPLE_GAP_MS = 120,
};

typedef struct {
    gpio_num_t gpio;
    bool active_high;
    hosino_button_id_t id;
} button_pin_t;

typedef struct {
    uint32_t candidate_since;
    uint32_t pressed_since;
    bool initialized;
    bool candidate_pressed;
    bool stable_known;
    bool stable_pressed;
    bool armed;
    bool valid_press;
} button_state_t;

static const button_pin_t pins[BUTTON_COUNT] = {
    {GPIO_NUM_16, true, HOSINO_BUTTON_POWER},
    {GPIO_NUM_0, false, HOSINO_BUTTON_BOOT},
    {GPIO_NUM_18, false, HOSINO_BUTTON_USER},
};
static button_state_t states[BUTTON_COUNT];
static hosino_button_event_t events[EVENT_CAPACITY];
static unsigned event_head;
static unsigned event_count;
static uint32_t last_poll_ms;
static bool have_last_poll;
static bool initialized;
static bool active;

static void reset_input(void)
{
    memset(states, 0, sizeof(states));
    memset(events, 0, sizeof(events));
    event_head = 0;
    event_count = 0;
    have_last_poll = false;
}

esp_err_t hosino_buttons_init(void)
{
    if (initialized) {
        return ESP_OK;
    }
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_16) | (1ULL << GPIO_NUM_18),
        .mode = GPIO_MODE_INPUT,
        /* The board supplies R9/R18 input pull-ups and the R8/T1 inverter.
         * Do not apply a contradictory pull to the power-key circuit. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) {
        return error;
    }
    reset_input();
    active = false;
    initialized = true;
    return ESP_OK;
}

void hosino_buttons_set_active(bool enabled)
{
    if (active == enabled) {
        return;
    }
    active = enabled;
    reset_input();
}

static void queue_release(hosino_button_id_t id, uint32_t released_at_ms, uint32_t held_ms)
{
    if (event_count == EVENT_CAPACITY) {
        return;
    }
    const unsigned index = (event_head + event_count) % EVENT_CAPACITY;
    events[index] = (hosino_button_event_t) {
        .button = id,
        .released_at_ms = released_at_ms,
        .held_ms = (uint16_t)held_ms,
    };
    ++event_count;
}

void hosino_buttons_poll(uint32_t now_ms)
{
    if (!initialized || !active) {
        return;
    }
    if (have_last_poll && (uint32_t)(now_ms - last_poll_ms) > MAX_SAMPLE_GAP_MS) {
        /* An unsampled interval cannot establish a short press safely. */
        reset_input();
    }
    last_poll_ms = now_ms;
    have_last_poll = true;
    for (unsigned i = 0; i < BUTTON_COUNT; ++i) {
        button_state_t *state = &states[i];
        const bool pressed = (gpio_get_level(pins[i].gpio) != 0) == pins[i].active_high;
        if (!state->initialized || pressed != state->candidate_pressed) {
            state->initialized = true;
            state->candidate_pressed = pressed;
            state->candidate_since = now_ms;
        }
        if ((uint32_t)(now_ms - state->candidate_since) < DEBOUNCE_MS) {
            continue;
        }
        if (!state->stable_known) {
            state->stable_known = true;
            state->stable_pressed = pressed;
            state->armed = !pressed;
            continue;
        }
        if (pressed == state->stable_pressed) {
            continue;
        }
        state->stable_pressed = pressed;
        if (pressed) {
            state->valid_press = state->armed;
            state->armed = false;
            state->pressed_since = state->candidate_since;
        } else {
            const uint32_t held_ms = state->candidate_since - state->pressed_since;
            if (state->valid_press && held_ms < LONG_PRESS_MS) {
                queue_release(pins[i].id, state->candidate_since, held_ms);
            }
            state->valid_press = false;
            state->armed = true;
        }
    }
}

bool hosino_buttons_take_event(hosino_button_event_t *event)
{
    if (!initialized || !active || event == NULL || event_count == 0) {
        return false;
    }
    *event = events[event_head];
    memset(&events[event_head], 0, sizeof(events[event_head]));
    event_head = (event_head + 1) % EVENT_CAPACITY;
    --event_count;
    return true;
}
