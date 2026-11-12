#include "control.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"
#include "config.h"
#include "ln882h.h"

static bool s_relay_on;
static bool s_led_on;
static bool s_button_stable;
static bool s_button_candidate;
static uint32_t s_button_change_ms;
static bool s_led_blink_state;
static uint32_t s_led_blink_ms;

static void control_apply_relay(void)
{
    bool level = s_relay_on;

    if (!POWER_SOCKET_RELAY_ACTIVE_HIGH) {
        level = !level;
    }

    if (level) {
        hal_gpio_pin_set(BOARD_RELAY_PORT, BOARD_RELAY_PIN);
    } else {
        hal_gpio_pin_reset(BOARD_RELAY_PORT, BOARD_RELAY_PIN);
    }
}

static void control_apply_led(void)
{
    bool level = s_led_on;

    if (!POWER_SOCKET_WIFI_LED_ACTIVE_HIGH) {
        level = !level;
    }

    if (level) {
        hal_gpio_pin_set(BOARD_WIFI_LED_PORT, BOARD_WIFI_LED_PIN);
    } else {
        hal_gpio_pin_reset(BOARD_WIFI_LED_PORT, BOARD_WIFI_LED_PIN);
    }
}

static void control_set_led(bool on)
{
    if (s_led_on == on) {
        return;
    }

    s_led_on = on;
    control_apply_led();
}

static bool control_button_pressed(void)
{
    uint8_t raw = hal_gpio_pin_read(BOARD_BUTTON_PORT, BOARD_BUTTON_PIN);

    if (POWER_SOCKET_BUTTON_ACTIVE_LOW) {
        return raw == 0u;
    }

    return raw != 0u;
}

void control_init(void)
{
    gpio_init_t_def gpio_cfg = {0};

    hal_gpio_pin_afio_en(BOARD_WIFI_LED_PORT, BOARD_WIFI_LED_PIN, HAL_DISABLE);
    hal_gpio_pin_afio_en(BOARD_BUTTON_PORT, BOARD_BUTTON_PIN, HAL_DISABLE);
    hal_gpio_pin_afio_en(BOARD_RELAY_PORT, BOARD_RELAY_PIN, HAL_DISABLE);

    s_relay_on = false;
    s_led_on = false;
    s_led_blink_state = false;
    s_led_blink_ms = 0;

    control_apply_relay();
    control_apply_led();

    gpio_cfg.pin = BOARD_WIFI_LED_PIN;
    gpio_cfg.dir = GPIO_OUTPUT;
    gpio_cfg.mode = GPIO_MODE_DIGITAL;
    gpio_cfg.pull = GPIO_PULL_NONE;
    gpio_cfg.speed = GPIO_NORMAL_SPEED;
    hal_gpio_init(BOARD_WIFI_LED_PORT, &gpio_cfg);

    gpio_cfg.pin = BOARD_RELAY_PIN;
    gpio_cfg.dir = GPIO_OUTPUT;
    gpio_cfg.mode = GPIO_MODE_DIGITAL;
    gpio_cfg.pull = GPIO_PULL_NONE;
    gpio_cfg.speed = GPIO_NORMAL_SPEED;
    hal_gpio_init(BOARD_RELAY_PORT, &gpio_cfg);

    control_apply_relay();
    control_apply_led();

    gpio_cfg.pin = BOARD_BUTTON_PIN;
    gpio_cfg.dir = GPIO_INPUT;
    gpio_cfg.mode = GPIO_MODE_DIGITAL;
#if POWER_SOCKET_BUTTON_ACTIVE_LOW
    gpio_cfg.pull = GPIO_PULL_UP;
#else
    gpio_cfg.pull = GPIO_PULL_DOWN;
#endif
    gpio_cfg.speed = GPIO_NORMAL_SPEED;
    hal_gpio_init(BOARD_BUTTON_PORT, &gpio_cfg);

    s_button_stable = control_button_pressed();
    s_button_candidate = s_button_stable;
    s_button_change_ms = 0;
}

void control_tick(uint32_t now_ms, bool wifi_connected)
{
    bool pressed = control_button_pressed();

    if (pressed != s_button_candidate) {
        s_button_candidate = pressed;
        s_button_change_ms = now_ms;
    }

    if ((s_button_candidate != s_button_stable) &&
        ((uint32_t)(now_ms - s_button_change_ms) >= POWER_SOCKET_BUTTON_DEBOUNCE_MS)) {
        s_button_stable = s_button_candidate;
        if (s_button_stable) {
            s_relay_on = !s_relay_on;
            control_apply_relay();
        }
    }

    if (!wifi_connected) {
        if ((uint32_t)(now_ms - s_led_blink_ms) >= POWER_SOCKET_WIFI_LED_BLINK_MS) {
            s_led_blink_ms = now_ms;
            s_led_blink_state = !s_led_blink_state;
            control_set_led(s_led_blink_state);
        }
    } else {
        s_led_blink_state = false;
        s_led_blink_ms = now_ms;
        control_set_led(s_relay_on);
    }
}

bool control_relay_is_on(void)
{
    return s_relay_on;
}
