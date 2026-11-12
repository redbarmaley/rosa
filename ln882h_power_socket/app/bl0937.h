#ifndef POWER_SOCKET_BL0937_H
#define POWER_SOCKET_BL0937_H

#include <stdint.h>

#include "hal/hal_gpio.h"

#define BL0937_SEL_PORT         GPIOB_BASE
#define BL0937_SEL_PIN          GPIO_PIN_8
#define BL0937_CF_PORT          GPIOA_BASE
#define BL0937_CF_PIN           GPIO_PIN_7
#define BL0937_CF1_PORT         GPIOA_BASE
#define BL0937_CF1_PIN          GPIO_PIN_8

typedef struct
{
    float voltage;
    float current;
    float power;
    float energy_wh;
    uint32_t uptime_s;
} bl0937_reading_t;

void bl0937_init(void);
void bl0937_tick(float dt_ms);
void bl0937_get(bl0937_reading_t *out);
void bl0937_save_energy(void);

#endif
