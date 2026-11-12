# План: standalone-прошивка LN882H (BL0937 → HTTPS POST)

## Зафиксированные решения

- Проект: `rosa/ln882h_power_socket/` (отдельно от апстрима `OpenBK7231T_App`).
- Сборка: отдельный скрипт `rosa/build_power_socket.sh`.
- Лог: UART0 только TX `B9`; `B8` больше не используется.
- Wi-Fi, URL, калибровки: хардкод в `app/config.h`.
- HTTPS: собственный минимальный клиент на mbedTLS, без проверки сертификата.
- Отправка: POST JSON раз в 600 с; `ts` = uptime в секундах.
- Энергия: Wh, сохраняется в KV.

## Аппаратные пины

Соответствие индексов OpenBeken и LN882H:

| OpenBeken | Назначение | Порт / пин |
|---:|---|---|
| 6 | WifiLED | `GPIOA_BASE`, `GPIO_PIN_6` |
| 7 | Btn | `GPIOA_BASE`, `GPIO_PIN_7` |
| 19 | Rel | `GPIOB_BASE`, `GPIO_PIN_3` |
| 20 | BL0937CF | `GPIOB_BASE`, `GPIO_PIN_4` |
| 21 | BL0937CF1 | `GPIOB_BASE`, `GPIO_PIN_5` |
| 22 | BL0937SEL | `GPIOB_BASE`, `GPIO_PIN_6` |

`SEL` — выход, `CF/CF1` — входы с pull-up, прерывания по спаду.
Реле и Wi-Fi LED — выходы, кнопка — вход с pull-up.

## Структура проекта

```
ln882h_power_socket/
  CMakeLists.txt
  ProjModuleCollect.cmake
  app/
    main.c
    usr_app.c
    usr_app.h
    board_pins.h
    config.h
    control.c
    control.h
    bl0937.c
    bl0937.h
    https_client.c
    https_client.h
    entropy_ln.c
    mbedtls_socket_config.h
  bsp/
    serial_hw.c
    serial_hw.h
    drv_adc_measure.c
    drv_adc_measure.h
  startup/
    startup_ln882h_gcc.c
    startup_ln882h.S
  cfg/
    flash_partition_cfg.json
    proj_config.h
    ln882h.sct
  gcc/
    gcc-toolchain-setup.cmake
    gcc-compiler-flags.cmake
    gcc-custom-build-stage.cmake
    ln882h.ld
```

## Сборка

`ProjModuleCollect.cmake` получает блок mbedTLS:

- `${COMP_MBEDTLS_DIR}/library/*.c`
- `${COMP_MBEDTLS_DIR}/port_ln/library/*.c`
- include-пути `mbedtls/include`, `mbedtls/port_ln/include`

`gcc-compiler-flags.cmake`:

```cmake
add_compile_definitions("MBEDTLS_CONFIG_FILE=<mbedtls_socket_config.h>")
```

`app/mbedtls_socket_config.h`:

```c
#ifndef MBEDTLS_SOCKET_CONFIG_H
#define MBEDTLS_SOCKET_CONFIG_H
#include "mbedtls_config.h"
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#endif
```

`app/entropy_ln.c` реализует `mbedtls_hardware_poll()` через
`ln_generate_true_random_16bytes()`.

## UART

В копии `bsp/serial_hw.c`:

- для UART0 настраивается только `GPIOB_BASE / GPIO_PIN_9 / UART0_TX`;
- `GPIO_PIN_8` (`B8`) не используется;
- RX mode и `USART_IT_RXNE` для UART0 отключены;
- `CFG_UART_BAUDRATE_LOG` = `115200`.

## BL0937

- `SEL` — выход, начальное состояние high.
- `CF/CF1` — входы pull-up, прерывания по спаду на `GPIOB_IRQn`.
- Задача 1 Гц:
  - в критической секции забрать и обнулить счётчики;
  - при `SEL high` записать `CF1` в `res_v`, иначе в `res_c` (как в OpenBeken);
  - переключить `SEL`;
  - `res_p` — счётчик `CF`;
  - `v = res_v * 0.13253012048 * (1000 / dt_ms)`;
  - `c = res_c * 0.0118577075 * (1000 / dt_ms)`;
  - `p = res_p * 1.5 * (1000 / dt_ms)`;
  - фильтр `p > 3680` → оставить предыдущее значение;
  - `energy_wh += p * dt_ms / 3600000`;
  - `uptime_s += dt_ms / 1000`.
- Флаг `BL0937_INVERT_SEL` (по умолчанию 0).
- KV-ключ `pwr_wh`: загрузка при старте, сохранение после успешного POST.

## Управление (реле, кнопка, Wi-Fi LED)

- При старте реле выключено; состояние реле не сохраняется между перезагрузками.
- Пока Wi-Fi не получил IP, Wi-Fi LED мигает с периодом
  `POWER_SOCKET_WIFI_LED_BLINK_MS`.
- После получения IP LED включён только при включённом реле.
- Кнопка переключает реле с антидребезгом
  `POWER_SOCKET_BUTTON_DEBOUNCE_MS`.
- `control_task` вызывает `control_tick()` с периодом
  `POWER_SOCKET_CONTROL_PERIOD_MS`.

Полярность задаётся в `config.h`:

- `POWER_SOCKET_WIFI_LED_ACTIVE_HIGH`
- `POWER_SOCKET_RELAY_ACTIVE_HIGH`
- `POWER_SOCKET_BUTTON_ACTIVE_LOW`

## HTTPS

- Разбор только `https://host[:port]/path`.
- `mbedtls_net_connect` → `mbedtls_ssl_setup` → SNI → handshake.
- `MBEDTLS_SSL_VERIFY_NONE`, read timeout 5000 мс.
- POST:

```
POST <path> HTTP/1.1
Host: <host[:port]>
Content-Type: application/json
Content-Length: <len>
Connection: close
```

JSON:

```json
{"voltage":230.12,"current":0.510,"power":117.30,"energy_wh":1234.560,"ts":3600}
```

## Задачи

- `bl0937_init()` и `control_init()` вызываются один раз в
  `creat_usr_app_task()` до создания потоков FreeRTOS.
- `power_task` (стек 2 КБ): цикл 1 Гц с вызовом `bl0937_tick()`.
- `control_task` (стек 2048 B): период
  `POWER_SOCKET_CONTROL_PERIOD_MS`, опрос IP, кнопки и LED.
- `net_task` (стек 16 КБ):
  - Wi-Fi STA, ожидание IP;
  - при потере IP — `wifi_sta_disconnect()` и повторный
    `wifi_sta_connect_v2(..., 30)` не чаще раза в 30 с;
  - POST каждые 600 с, до 3 попыток с паузой 10 с;
  - после успешного POST — сохранение энергии в KV.
- `temp_cal` задача остаётся включённой (как в SDK-примере).
- `PM_DEFAULT_SLEEP_MODE` = `ACTIVE` — важно для счёта импульсов.

## Скрипт сборки

`build_power_socket.sh`:

1. Проверяет `cmake`, `python3`, `CROSS_TOOLCHAIN_ROOT`
   (по умолчанию `/opt/gcc-arm-none-eabi-10.3-2021.10`).
2. Создаёт симлинк
   `OpenBK7231T_App/sdk/OpenLN882H/project/PowerSocket` →
   `rosa/ln882h_power_socket`.
3. Выполняет:

```bash
cmake -S OpenBK7231T_App/sdk/OpenLN882H -B build_power_socket -DUSER_PROJECT=PowerSocket
cmake --build build_power_socket -j"$(nproc)"
```

4. Копирует в `ln882h_power_socket/output/`:
   - `build_power_socket/bin/flashimage.bin`
   - `build_power_socket/bin/flashimage-ota-xz-v0.1.bin`

## Проверка

1. Сборка без ошибок; размер APP < 1200 КБ.
2. Прошивка `flashimage.bin` через J-Link/JFlash или UART-загрузчик.
3. UART 115200: старт, Wi-Fi, IP.
4. До получения IP Wi-Fi LED мигает; после IP LED горит только при
   включённом реле.
5. Кнопка переключает реле без ложных срабатываний (антидребезг).
6. Сравнение V/I/P с эталонным ваттметром; правка калибровок при
   необходимости.
7. Локальный HTTPS-сервер с self-signed сертификатом, временно интервал 10 с.
8. Перезагрузка: энергия восстанавливается из KV, реле выключено.
9. Потеря Wi-Fi: переподключение и повторная отправка.

## Риски

- mbedTLS 2.16: TLS 1.2, без TLS 1.3.
- Проверка сертификата отключена по решению пользователя.
- Фаза `SEL` взята 1:1 из OpenBeken; при инверсии исправляется флагом
  `BL0937_INVERT_SEL`.
- Полярность LED, реле и кнопки задаётся макросами `config.h`; при
  несовпадении с платой их нужно скорректировать.
- Для сборки нужны `cmake`, `srecord`, Python 3 и ARM GCC 10.3-2021.10.
- URL пока `https://example.com/api/power`; Wi-Fi — заглушки в `config.h`.
