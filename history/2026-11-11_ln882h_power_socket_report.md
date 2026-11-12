# Отчёт: standalone-прошивка LN882H PowerSocket

Дата: 2026-11-11
План: `plans/ln882h_power_socket_plan.md`
Проект: `ln882h_power_socket/`
Сборка: `build_power_socket.sh`

## Итог

План реализован и собран. Прошивка собирается без ошибок, артефакты получены:

| Артефакт | Размер |
|---|---:|
| `ln882h_power_socket/output/flashimage.bin` | 628 168 B (613.45 KiB) |
| `ln882h_power_socket/output/flashimage-ota-xz-v0.1.bin` | 347 029 B (338.9 KiB) |
| APP `PowerSocket.bin` | 599 240 B (585.2 KiB), 48.77% раздела APP 1200 KiB |

Проверки на реальном железе не выполнялись: BL0937, Wi-Fi, HTTPS, KV и OTA
требуют запуска на устройстве.

## Этапы

### Этап 1. Интеграция приложения

Переработан `app/usr_app.c`:

- Wi-Fi SSID/пароль и URL берутся из `app/config.h`.
- `power_task` (стек 2048 B): инициализация BL0937 и цикл 1 Гц с фактическим
  `dt` через `OS_GetTicks()` / `OS_TicksToMSecs()`.
- `net_task` (стек 16384 B):
  - Wi-Fi STA и ожидание IP;
  - переподключение при потере IP не чаще раза в 30 с через
    `wifi_sta_disconnect()` + `wifi_sta_connect_v2(..., 30)`;
  - первый POST сразу после получения IP, далее каждые 600 с;
  - до 3 попыток POST с паузой 10 с;
  - сохранение энергии в KV после успешного POST.
- `temp_cal` задача сохранена, `PM_DEFAULT_SLEEP_MODE = ACTIVE`.
- Добавлены совместимые с prebuilt lwIP/FreeRTOS символы
  `g_STA_static_IP = false` и пустой `vApplicationIdleHook()`.

### Этап 2. Сборочная инфраструктура

Создан `build_power_socket.sh`:

- проверяет `cmake`, `python3`, `CROSS_TOOLCHAIN_ROOT`;
- по умолчанию использует `/opt/gcc-arm-none-eabi-10.3-2021.10`;
- создаёт симлинк `OpenBK7231T_App/sdk/OpenLN882H/project/PowerSocket` →
  `ln882h_power_socket`;
- выполняет `cmake -S ... -B build_power_socket -DUSER_PROJECT=PowerSocket`
  и `cmake --build`;
- копирует `flashimage.bin` и `flashimage-ota-xz-v0.1.bin` в
  `ln882h_power_socket/output/`.

Локальное окружение для проверки (без sudo):

```bash
export PATH="/home/www/Downloads/tuya/.tools/venv/bin:$PATH"
export CMAKE_BIN="/home/www/Downloads/tuya/.tools/venv/bin/cmake"
export CROSS_TOOLCHAIN_ROOT="/home/www/Downloads/tuya/.tools/gcc-arm-none-eabi-10.3-2021.10"
cd /home/www/Downloads/tuya/rosa
./build_power_socket.sh
```

### Этап 3. Первая сборка

Сборка прошла успешно. Исправления:

- добавлены `g_STA_static_IP` и `vApplicationIdleHook()`, на которые ссылаются
  `liblwip.a` и FreeRTOS;
- устранены предупреждения форматов в `app/usr_app.c`.

### Этап 4. Ревью

Ревью выявило:

- потерю дробной части при накоплении `uptime_s`;
- чтение HTTPS-ответа одним `mbedtls_ssl_read()` без цикла;
- UB в `djb_hash_hexdata()` (чтение за границей массива);
- расхождение имени флага `BL0937_INVERT_SEL`;
- необходимость явного сброса AFIO для `GPIOB8`.

### Этап 5. Исправления

- `app/bl0937.c`:
  - добавлен `s_uptime_rem_ms` и корректное накопление секунд;
  - добавлен `hal_gpio_pin_afio_en(GPIOB_BASE, GPIO_PIN_8, HAL_DISABLE)`;
  - флаг переименован в `BL0937_INVERT_SEL`.
- `app/config.h`: `BL0937_INVERT_SEL = 0`.
- `app/https_client.c`:
  - цикл отправки обрабатывает `WANT_READ` / `WANT_WRITE`;
  - ответ читается циклом до `\r\n\r\n`, заполнения буфера, таймаута или
    закрытия соединения;
  - статус проверяется по первой строке `HTTP/...`, успех — 2xx.
- `app/usr_app.c`: `djb_hash_hexdata()` переписан без чтения за границей.

### Этап 6. Контрольная пересборка

Выполнена полная чистая пересборка (`build_power_socket` удалён):

- exit code 0;
- 0 ошибок компиляции/линковки;
- APP < 1200 KiB;
- `app/*` — 0 предупреждений;
- в файлах проекта остались только 3 предупреждения `-Wmissing-prototypes`
  в `bsp/serial_hw.c` и 1 `-Wpedantic` в `startup/startup_ln882h_gcc.c`;
- лог: `/tmp/opencode/build_socket_final.log`.

## Соответствие плану

- Пины: `SEL=GPIOB8`, `CF=GPIOA7`, `CF1=GPIOA8`.
- UART0: только TX `B9`, `B8` не используется, RX/`USART_IT_RXNE` отключены,
  скорость 115200.
- BL0937: формулы V/I/P, фильтр `p > 3680`, энергия Wh, ключ KV `pwr_wh`,
  фаза SEL как в OpenBeken, флаг инверсии `BL0937_INVERT_SEL`.
- HTTPS: mbedTLS 2.16, TLS 1.2, SNI, `MBEDTLS_SSL_VERIFY_NONE`, read timeout
  5000 мс, POST JSON с `ts` = uptime.
- Задачи: `power_task` 2 КБ, `net_task` 16 КБ, `temp_cal` включена,
  `PM_DEFAULT_SLEEP_MODE = ACTIVE`.
- Сборка: модули mbedTLS и `port_ln` подключены через
  `ProjModuleCollect.cmake`, `MBEDTLS_CONFIG_FILE=<mbedtls_socket_config.h>`.

## Что осталось проверить на железе

1. Прошивка `flashimage.bin` через J-Link/JFlash или UART-загрузчик.
2. Старт UART 115200: Wi-Fi, получение IP.
3. Сравнение V/I/P с эталонным ваттметром и калибровка при необходимости.
4. Локальный HTTPS-сервер с self-signed сертификатом, временно интервал 10 с.
5. Перезагрузка: восстановление энергии из KV.
6. Потеря Wi-Fi: переподключение и повторная отправка.
7. Проверка OTA-образа.

## Примечания

- Проверка сертификата отключена по решению пользователя.
- `srecord` фактически не требуется: образы собираются Python-скриптами SDK.
- Git-коммиты не создавались.
