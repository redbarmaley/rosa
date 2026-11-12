# Отчёт: управление реле, кнопкой и Wi-Fi LED в LN882H PowerSocket

Дата: 2026-11-12
План: `plans/ln882h_power_socket_plan.md`
Проект: `ln882h_power_socket/`
Сборка: `build_power_socket.sh`

## Итог

Добавлено управление реле, кнопкой и Wi-Fi LED. BL0937 переведён на новые
пины. Прошивка собирается без ошибок. Проверка на реальном железе не
выполнялась.

## Изменённые файлы

| Файл | Изменение |
|---|---|
| `app/board_pins.h` | Новый файл: единое соответствие пинов платы |
| `app/control.c` | Новый файл: управление реле, кнопкой и Wi-Fi LED |
| `app/control.h` | Новый файл: API `control_init()`, `control_tick()`, `control_relay_is_on()` |
| `app/config.h` | Добавлены макросы полярности и таймингов управления |
| `app/bl0937.c` | BL0937 переведён на `B4/B5/B6`, прерывания на `GPIOB_IRQn` |
| `app/bl0937.h` | Пины BL0937 берутся из `board_pins.h` |
| `app/usr_app.c` | `bl0937_init()` и `control_init()` перенесены в `creat_usr_app_task()` до создания потоков; добавлена `control_task` со стеком 2048 B |
| `bsp/serial_hw.c` | UART0 оставлен TX-only на `B9`; `B8` не используется |
| `CMakeLists.txt` | В сборку добавлен `app/control.c` |
| `plans/ln882h_power_socket_plan.md` | Обновлены пины, структура, управление и проверка |

## Новое соответствие пинов

| OpenBeken | Назначение | Порт / пин |
|---:|---|---|
| 6 | WifiLED | `GPIOA_BASE`, `GPIO_PIN_6` |
| 7 | Btn | `GPIOA_BASE`, `GPIO_PIN_7` |
| 19 | Rel | `GPIOB_BASE`, `GPIO_PIN_3` |
| 20 | BL0937CF | `GPIOB_BASE`, `GPIO_PIN_4` |
| 21 | BL0937CF1 | `GPIOB_BASE`, `GPIO_PIN_5` |
| 22 | BL0937SEL | `GPIOB_BASE`, `GPIO_PIN_6` |

UART0 — только TX на `B9`. `B8` больше не используется.

## Поведение управления

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

## Результат сборки

Сборка завершена успешно:

- APP flash: 599856 B (48.83% раздела APP 1200 KiB);
- RAM0: 141048 B (46.69%).

| Артефакт | Размер |
|---|---:|
| `ln882h_power_socket/output/flashimage.bin` | 628784 B (614.05 KiB) |
| `ln882h_power_socket/output/flashimage-ota-xz-v0.1.bin` | 347287 B (339.15 KiB) |

Лог сборки: `/tmp/opencode/build_control_final.log`.

## Примечания

- Поведение реле, кнопки, LED и BL0937 на реальном железе не проверялось.
- Git-коммит не создавался.
