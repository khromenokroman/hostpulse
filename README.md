# hostpulse

Пульс машины: процессор, нагрузка, температуры, память, диски и сеть
в живом веб-дашборде и через HTTP API.

## Возможности

- Информация о процессоре (модель, ядра, частота, governor)
- Загрузка CPU (load average, jiffies)
- Температуры (hwmon, thermal zones)
- Вентиляторы (hwmon)
- Оперативная память и swap
- Диски (размеры, IO, смонтированные разделы с занято/свободно)
- Оборудование (BIOS, материнская плата, GPU, сеть, батарея)

## Зависимости

- CMake 3.16+
- C++20
- nlohmann_json
- fmt
- GoogleTest (опционально, для тестов)
- Node.js (опционально, проверка синтаксиса JS дашборда в ctest)

## Сборка

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

## Запуск

```bash
./hostpulse          # дашборд на порту 8080
./hostpulse 9000     # на другом порту
./hostpulse --dump   # все данные в JSON и выход
./hostpulse --help   # справка
```

Откройте http://localhost:8080 в браузере. Сообщения о работе выводятся в stdout,
ошибки — в stderr; при занятом порте или неверном аргументе программа завершается
с ненулевым кодом (1 — сервер не запустился, 2 — неверный аргумент).

## API

| Endpoint     | Описание              |
|-------------|----------------------|
| `/api/cpu`   | Информация о CPU     |
| `/api/load`  | Загрузка CPU         |
| `/api/temp`  | Температуры          |
| `/api/fans`  | Вентиляторы          |
| `/api/mem`   | Память               |
| `/api/disk`  | Диски и разделы      |
| `/api/hw`    | Оборудование         |
| `/api/all`   | Всё целиком: `{cpu, load, temp, fans, mem, disk, hw, timestamp}` |
| `/api/health`| То же что `/api/all` |
| `/`          | HTML-дашборд         |

Данные снимает фоновый поток раз в секунду, API отдаёт последний снимок.
Загрузка CPU (`active_percent`, `per_core[].percent`) и скорость сети
(`rate_rx_mbps`, `rate_tx_mbps`) посчитаны за интервал между замерами.

## Тесты

```bash
cmake --build .
ctest --output-on-failure
```

- `hostpulse_tests` — юнит-тесты сборщика и интеграционные тесты HTTP-сервера (порт 18080)
- `web_ui_js_syntax` — `node --check` скрипта дашборда (если найден node)

## Структура

- `src/collector.h` — структуры данных и to_json
- `src/collector.cpp` — чтение /sys, /proc
- `src/server.cpp` — HTTP-сервер, фоновый поток замеров
- `src/web_ui.h` — HTML/CSS/JS дашборда
- `src/main.cpp` — точка входа
- `tests/test_collector.cpp` — юнит-тесты сборщика и Sampler
- `tests/test_server.cpp` — интеграционные тесты HTTP API и дашборда
- `tests/check_web_ui_js.cmake` — проверка синтаксиса JS дашборда
