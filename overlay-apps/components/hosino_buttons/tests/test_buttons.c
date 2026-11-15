#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "hosino_buttons.h"
#include "driver/gpio.h"

static int levels[19];
static unsigned config_calls;
static uint32_t now;
static esp_err_t config_result = ESP_FAIL;

esp_err_t gpio_config(const gpio_config_t *config)
{
    ++config_calls;
    assert(config->pin_bit_mask == ((1ULL << 0) | (1ULL << 16) | (1ULL << 18)));
    assert(config->mode == GPIO_MODE_INPUT);
    assert(config->pull_up_en == GPIO_PULLUP_DISABLE);
    assert(config->pull_down_en == GPIO_PULLDOWN_DISABLE);
    assert(config->intr_type == GPIO_INTR_DISABLE);
    return config_result;
}

int gpio_get_level(gpio_num_t gpio) { assert(gpio == 0 || gpio == 16 || gpio == 18); return levels[gpio]; }

static void advance(unsigned ms)
{
    while (ms != 0) {
        const unsigned step = ms > 20 ? 20 : ms;
        now += step;
        hosino_buttons_poll(now);
        ms -= step;
    }
}

static void empty(void)
{
    hosino_button_event_t event;
    assert(!hosino_buttons_take_event(&event));
}

static void reset_at(uint32_t start)
{
    hosino_buttons_set_active(false);
    levels[0] = 1; levels[16] = 0; levels[18] = 1;
    now = start;
    hosino_buttons_set_active(true);
    hosino_buttons_poll(now);
    advance(40);
}

static void set_press(int pin, int pressed)
{
    levels[pin] = pin == 16 ? pressed : !pressed;
    hosino_buttons_poll(now);
}

static void click(int pin, unsigned duration)
{
    set_press(pin, 1);
    advance(duration);
    set_press(pin, 0);
    advance(40);
}

static void expect(hosino_button_id_t id, unsigned duration)
{
    hosino_button_event_t event;
    assert(hosino_buttons_take_event(&event));
    assert(event.button == id);
    assert(event.held_ms == duration);
    assert(event.released_at_ms == now - 40);
}

int main(void)
{
    empty();
    assert(hosino_buttons_init() == ESP_FAIL);
    config_result = ESP_OK;
    assert(hosino_buttons_init() == ESP_OK);
    assert(hosino_buttons_init() == ESP_OK);
    assert(config_calls == 2);
    levels[0] = levels[18] = 1;
    hosino_buttons_poll(0); empty(); /* Init is inactive. */

    reset_at(0);
    click(16, 100); expect(HOSINO_BUTTON_POWER, 100); empty();
    click(0, 60); expect(HOSINO_BUTTON_BOOT, 60); empty();
    click(18, 799); expect(HOSINO_BUTTON_USER, 799); empty();
    click(18, 800); empty();
    click(16, 1200); empty();

    /* Bounce shorter than 40 ms cannot create a press or release. */
    reset_at(0);
    set_press(18, 1); advance(20); set_press(18, 0); advance(60); empty();
    set_press(18, 1); advance(60); set_press(18, 0); advance(20);
    set_press(18, 1); advance(20); set_press(18, 0); advance(40);
    expect(HOSINO_BUTTON_USER, 100); empty();

    /* A key held at activation must be released before the first click. */
    hosino_buttons_set_active(false);
    levels[0] = 0; levels[16] = 1; levels[18] = 0;
    hosino_buttons_set_active(true); hosino_buttons_poll(now); advance(80);
    levels[0] = 1; levels[16] = 0; levels[18] = 1;
    hosino_buttons_poll(now); advance(40); empty();
    click(0, 60); expect(HOSINO_BUTTON_BOOT, 60); empty();

    /* Idempotent foreground updates preserve a valid press. */
    reset_at(0); set_press(18, 1); advance(60);
    hosino_buttons_set_active(true); advance(40);
    set_press(18, 0); advance(40); expect(HOSINO_BUTTON_USER, 100);

    /* Deactivation discards both queued and partially held presses. */
    click(0, 60); hosino_buttons_set_active(false); empty();
    set_press(18, 1); hosino_buttons_set_active(true); hosino_buttons_poll(now);
    advance(60); set_press(18, 0); advance(40); empty();
    set_press(18, 1); advance(60); hosino_buttons_set_active(false);
    set_press(18, 0); advance(40); hosino_buttons_set_active(true);
    hosino_buttons_poll(now); advance(40); empty();

    /* Eight-event FIFO, discard new events on overflow. */
    reset_at(0);
    for (unsigned i = 0; i < 10; ++i) { click(18, 60); }
    hosino_button_event_t event;
    for (unsigned i = 0; i < 8; ++i) {
        assert(hosino_buttons_take_event(&event));
        assert(event.button == HOSINO_BUTTON_USER && event.held_ms == 60);
    }
    empty(); assert(!hosino_buttons_take_event(NULL));

    reset_at(UINT32_MAX - 70); click(16, 100); expect(HOSINO_BUTTON_POWER, 100); empty();

    /* Unknown long sampling interval suppresses release, then recovers. */
    reset_at(0); set_press(18, 1); advance(60);
    now += 121; hosino_buttons_poll(now); set_press(18, 0); advance(40); empty();
    click(18, 60); expect(HOSINO_BUTTON_USER, 60); empty();

    puts("PASS: production button state machine; polarity, debounce, 799/800ms, activation, FIFO, wrap, gap, init retry");
    return 0;
}
