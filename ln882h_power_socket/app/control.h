#ifndef POWER_SOCKET_CONTROL_H
#define POWER_SOCKET_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

void control_init(void);
void control_tick(uint32_t now_ms, bool wifi_connected);
bool control_relay_is_on(void);

#endif
