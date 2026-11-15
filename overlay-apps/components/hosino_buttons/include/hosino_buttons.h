#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOSINO_BUTTON_POWER = 0, /* GPIO16 / SYS_OUT: active high. */
    HOSINO_BUTTON_BOOT,      /* GPIO0: active low. */
    HOSINO_BUTTON_USER,      /* GPIO18: active low. */
} hosino_button_id_t;

typedef struct {
    hosino_button_id_t button;
    uint32_t released_at_ms;
    uint16_t held_ms;
} hosino_button_event_t;

/* All functions must run on the same GUI/control thread. No heap or tasks.
 * Configures only GPIO0/16/18 as inputs, leaving PMU and boot configuration
 * unchanged. Idempotent after success; events start disabled.
 */
esp_err_t hosino_buttons_init(void);

/* Call from a 20 ms GUI timer using a monotonic millisecond clock.
 * A level must remain stable for 40 ms. Only released presses shorter than
 * 800 ms generate events. Sampling gaps over 120 ms disarm pending presses.
 */
void hosino_buttons_poll(uint32_t now_ms);

/* Both activation transitions discard queued/partial presses. After activation,
 * every key must first be observed released for 40 ms before it is accepted.
 * Repeating the current setting is harmless. Call false when the app is inactive.
 */
void hosino_buttons_set_active(bool active);

/* FIFO, at most eight pending events; an overflowing new event is discarded.
 * Returns false while inactive or uninitialized. No callbacks or UI actions.
 */
bool hosino_buttons_take_event(hosino_button_event_t *event);

#ifdef __cplusplus
}
#endif
