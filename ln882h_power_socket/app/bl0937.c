#include "bl0937.h"

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "ln882h.h"
#include "ln_kv_api.h"
#include "utils/debug/log.h"

static volatile uint32_t s_cf_pulses;
static volatile uint32_t s_cf1_pulses;

static uint32_t s_raw_v;
static uint32_t s_raw_c;
static uint32_t s_raw_p;
static float s_last_power;
static bool s_sel_high;
static uint32_t s_uptime_rem_ms;

static bl0937_reading_t s_reading;

void GPIOA_IRQHandler(void);

static void bl0937_set_sel(bool high)
{
    if (high) {
        hal_gpio_pin_set(BL0937_SEL_PORT, BL0937_SEL_PIN);
    } else {
        hal_gpio_pin_reset(BL0937_SEL_PORT, BL0937_SEL_PIN);
    }
}

void bl0937_init(void)
{
    gpio_init_t_def gpio_cfg = {0};
    size_t value_len = 0;
    float saved_energy = 0.0f;

    s_cf_pulses = 0;
    s_cf1_pulses = 0;
    s_raw_v = 0;
    s_raw_c = 0;
    s_raw_p = 0;
    s_last_power = 0.0f;
    s_sel_high = true;
    s_uptime_rem_ms = 0;

    hal_gpio_pin_afio_en(GPIOB_BASE, GPIO_PIN_8, HAL_DISABLE);

    gpio_cfg.pin = BL0937_SEL_PIN;
    gpio_cfg.dir = GPIO_OUTPUT;
    gpio_cfg.mode = GPIO_MODE_DIGITAL;
    gpio_cfg.pull = GPIO_PULL_NONE;
    gpio_cfg.speed = GPIO_NORMAL_SPEED;
    hal_gpio_init(BL0937_SEL_PORT, &gpio_cfg);
    bl0937_set_sel(true);

    gpio_cfg.pin = (gpio_pin_t)(BL0937_CF_PIN | BL0937_CF1_PIN);
    gpio_cfg.dir = GPIO_INPUT;
    gpio_cfg.mode = GPIO_MODE_DIGITAL;
    gpio_cfg.pull = GPIO_PULL_UP;
    gpio_cfg.speed = GPIO_NORMAL_SPEED;
    hal_gpio_init(BL0937_CF_PORT, &gpio_cfg);

    hal_gpio_pin_clr_it_flag(BL0937_CF_PORT, BL0937_CF_PIN);
    hal_gpio_pin_clr_it_flag(BL0937_CF1_PORT, BL0937_CF1_PIN);
    hal_gpio_pin_it_cfg(BL0937_CF_PORT, BL0937_CF_PIN, GPIO_INT_FALLING);
    hal_gpio_pin_it_cfg(BL0937_CF1_PORT, BL0937_CF1_PIN, GPIO_INT_FALLING);
    hal_gpio_pin_it_en(BL0937_CF_PORT, BL0937_CF_PIN, HAL_ENABLE);
    hal_gpio_pin_it_en(BL0937_CF1_PORT, BL0937_CF1_PIN, HAL_ENABLE);

    NVIC_ClearPendingIRQ(GPIOA_IRQn);
    NVIC_EnableIRQ(GPIOA_IRQn);

    if ((ln_kv_get("pwr_wh", &saved_energy, sizeof(saved_energy), &value_len) == KV_ERR_NONE) &&
        (value_len == sizeof(saved_energy)) && (saved_energy >= 0.0f)) {
        s_reading.energy_wh = saved_energy;
    } else {
        s_reading.energy_wh = 0.0f;
    }
    s_reading.voltage = 0.0f;
    s_reading.current = 0.0f;
    s_reading.power = 0.0f;
    s_reading.uptime_s = 0;

    LOG(LOG_LVL_INFO, "BL0937 init: energy=%.3f Wh\r\n", (double)s_reading.energy_wh);
}

void GPIOA_IRQHandler(void)
{
    if (hal_gpio_pin_get_it_flag(BL0937_CF_PORT, BL0937_CF_PIN)) {
        hal_gpio_pin_clr_it_flag(BL0937_CF_PORT, BL0937_CF_PIN);
        s_cf_pulses++;
    }

    if (hal_gpio_pin_get_it_flag(BL0937_CF1_PORT, BL0937_CF1_PIN)) {
        hal_gpio_pin_clr_it_flag(BL0937_CF1_PORT, BL0937_CF1_PIN);
        s_cf1_pulses++;
    }
}

void bl0937_tick(float dt_ms)
{
    uint32_t cf1_count;
    uint32_t cf_count;
    bool sampled_high;
    float scale;
    float voltage;
    float current;
    float power;
    float energy_delta;

    if (dt_ms <= 0.0f) {
        dt_ms = 1.0f;
    }

    CRITICAL_SECT_START();
    cf1_count = s_cf1_pulses;
    s_cf1_pulses = 0;
    cf_count = s_cf_pulses;
    s_cf_pulses = 0;

    sampled_high = s_sel_high;
    s_sel_high = !s_sel_high;
    bl0937_set_sel(s_sel_high);

    if (BL0937_INVERT_SEL) {
        if (sampled_high) {
            s_raw_c = cf1_count;
        } else {
            s_raw_v = cf1_count;
        }
    } else {
        if (sampled_high) {
            s_raw_v = cf1_count;
        } else {
            s_raw_c = cf1_count;
        }
    }
    s_raw_p = cf_count;
    CRITICAL_SECT_END();

    scale = 1000.0f / dt_ms;
    voltage = (float)s_raw_v * POWER_SOCKET_VOLTAGE_CAL * scale;
    current = (float)s_raw_c * POWER_SOCKET_CURRENT_CAL * scale;
    power = (float)s_raw_p * POWER_SOCKET_POWER_CAL * scale;

    if (power > POWER_SOCKET_POWER_MAX) {
        power = s_last_power;
    } else {
        s_last_power = power;
    }

    energy_delta = power * dt_ms / 3600000.0f;

    CRITICAL_SECT_START();
    s_reading.voltage = voltage;
    s_reading.current = current;
    s_reading.power = power;
    s_reading.energy_wh += energy_delta;
    s_uptime_rem_ms += (uint32_t)(dt_ms + 0.5f);
    s_reading.uptime_s += s_uptime_rem_ms / 1000u;
    s_uptime_rem_ms %= 1000u;
    CRITICAL_SECT_END();
}

void bl0937_get(bl0937_reading_t *out)
{
    if (out == NULL) {
        return;
    }

    CRITICAL_SECT_START();
    *out = s_reading;
    CRITICAL_SECT_END();
}

void bl0937_save_energy(void)
{
    float energy;

    CRITICAL_SECT_START();
    energy = s_reading.energy_wh;
    CRITICAL_SECT_END();

    if (ln_kv_set("pwr_wh", &energy, sizeof(energy)) != KV_ERR_NONE) {
        LOG(LOG_LVL_ERROR, "BL0937: failed to save energy\r\n");
    }
}
