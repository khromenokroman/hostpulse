# sys_info

Сбор системной информации и вывод через HTTP-дашборд.

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
- GoogleTest (опционально)

## Сборка

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

## Запуск

```bash
./sys_info
```

Сервер стартовал на порту 8080. Откройте http://localhost:8080 в браузере.

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
| `/api/all`   | Всё целиком         |
| `/api/health`| То же что `/api/all` |
| `/`          | HTML-дашборд         |

## Тесты

```bash
cmake --build . --target sys_info_tests
./sys_info_tests
```

## Структура

- `src/collector.h` — структуры данных и to_json
- `src/collector.cpp` — чтение /sys, /proc
- `src/server.cpp` — HTTP-сервер, HTML-дашборд
- `src/main.cpp` — точка входа
- `tests/test_collector.cpp` — юнит-тесты
