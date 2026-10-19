# hostpulse

Состояние компьютера в браузере: загрузка процессора, температуры, вентиляторы, память, диски,
оборудование и сетевой трафик. Страница обновляется сама раз в 2 секунды, те же данные отдаются в
JSON через HTTP API. Подписи рассчитаны на обычного пользователя: «Очередь задач» вместо load average,
«SSD» вместо `Composite`, непонятные строки поясняются всплывающей подсказкой.

Дашборд в четыре колонки:

| Колонка | Плитки |
|---|---|
| 1 | Нагрузка (частота, режим питания, загрузка по потокам), вентиляторы |
| 2 | Температура (процессор, ядра, SSD, Wi-Fi, видеокарта, материнская плата) |
| 3 | Память и подкачка, разделы дисков (занято, свободно, прочитано и записано) |
| 4 | Оборудование (модель, плата, BIOS, процессор, диски, видеокарта, сетевые адаптеры, батарея), трафик сети |

Данные берутся из `/proc` и `/sys`, список видеокарт из `lspci`. Виртуальные устройства (loop, docker,
veth, мосты) и разделы загрузчика (`/boot`, `/boot/efi`) на дашборде не показываются, в API они есть.

## Установка

Пакеты для Debian 12 и 13 лежат в [релизах](https://github.com/khromenokroman/hostpulse/releases).
Скачать пакет своей версии Debian и установить через apt (зависимости apt поставит сам):

```sh
sudo apt install ./hostpulse_0.2.0.0_amd64-debian13.deb
```

Пакет ставит службу `hostpulse` и сразу её запускает: дашборд на http://адрес-машины:8080/.

- Адрес и порт: `/etc/default/hostpulse` (по умолчанию `--host 0.0.0.0 --port 8080`), после изменения
  `sudo systemctl restart hostpulse`. Входа по паролю нет: с `0.0.0.0` любой в локальной сети видит модель
  компьютера, MAC-адреса и загрузку. Только доверенная сеть, иначе `--host 127.0.0.1`.
- Служба работает от динамического пользователя без прав (systemd `DynamicUser`), файловая система
  для неё только на чтение.
- Журнал: `journalctl -u hostpulse`.
- Обновление: установить новый пакет так же, служба перезапустится, настройки сохранятся.
- `apt remove hostpulse` останавливает службу, `apt purge hostpulse` удаляет и настройки.

### Выпуск релиза

Версия в `CMakeLists.txt` уже поднята коммитом `build: Версия X.Y.Z.W`. Тег с этой версией запускает CI:

```sh
git tag v0.2.0.0 && git push origin v0.2.0.0
```

CI собирает и тестирует пакеты для Debian 12 и 13, открывает дашборд в headless-браузере, проверяет
установку, обновление и удаление в контейнере с systemd (`tests/deb_install.sh`) и создаёт релиз с
пакетами. Тег, не совпадающий с версией, CI отклоняет.

## Сборка

Debian 12 или 13:

```sh
sudo apt install build-essential cmake nlohmann-json3-dev libgtest-dev libgmock-dev nodejs
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

- `hostpulse_tests`: тесты сборщика, `Sampler` и HTTP-сервера (поднимает сервер на порту 18080).
- `web_ui_js_syntax`: `node --check` скрипта дашборда; без `nodejs` пропускается.

Дашборд в headless-браузере (нужен `chromium` или `google-chrome`): страница рисуется, данные пришли,
ни одна плитка не упала с ошибкой.

```sh
tests/web_smoke.sh build/hostpulse
```

HTTP-сервер [cpp-httplib](https://github.com/yhirose/cpp-httplib) подключён как один заголовок
`third_party/httplib.h` (MIT): во время работы программе нужна только стандартная библиотека C++.

Пакет DEB (нужен `file`): собрать с `-DCMAKE_INSTALL_PREFIX=/usr`, затем `cd build && cpack -G DEB`.
Проверка пакета в контейнере с systemd (нужен docker): `tests/deb_install.sh <каталог с .deb> bookworm|trixie`.

## Запуск

```sh
build/hostpulse                       # дашборд на http://localhost:8080, доступен из сети
build/hostpulse --host 127.0.0.1      # только с этой машины
build/hostpulse --port 9000           # другой порт (можно и просто: build/hostpulse 9000)
build/hostpulse --dump                # все данные в JSON и выход
build/hostpulse --version
build/hostpulse --help
```

Сообщения о работе идут в stdout, ошибки в stderr. Коды выхода: 1, если сервер не запустился (порт занят,
неверный адрес, нет прав), 2 при неверном аргументе. Второй экземпляр на занятом порту не запустится.

## API

Все ответы в JSON. Данные снимает фоновый поток раз в секунду, API отдаёт последний снимок.

| Путь | Что отдаёт |
|---|---|
| `/api/all` | всё целиком: `{cpu, load, temp, fans, mem, disk, hw, timestamp}` |
| `/api/health` | то же, что `/api/all` |
| `/api/cpu` | модель, ядра и потоки, частота, режим питания (governor) |
| `/api/load` | загрузка CPU в процентах, по потокам (`per_core[].percent`), load average |
| `/api/temp` | температуры: `package` (процессор), `cores`, `sensors` (остальные устройства) |
| `/api/fans` | вентиляторы, об/мин |
| `/api/mem` | память и подкачка, байты |
| `/api/disk` | `disks` (физические с `model` и `kind`: NVMe/SSD/HDD) и `mounts` (разделы) |
| `/api/hw` | плата, BIOS, батарея, видеокарты, `network` (`kind`: Wi-Fi/Ethernet, трафик в Мбит/с) |
| `/` | дашборд |

Загрузка CPU и скорость сети считаются за интервал между замерами, а не с момента включения.

## Устройство

- `src/collector.h`, `src/collector.cpp`: чтение `/proc` и `/sys`, структуры и `to_json`;
  `collector::Sampler` помнит прошлый замер и считает загрузку CPU и трафик за интервал.
- `src/server.h`, `src/server.cpp`: HTTP-сервер и фоновый поток замеров.
- `src/web_ui.h`: HTML, CSS и JS дашборда одной строкой.
- `src/main.cpp`: разбор аргументов, запуск.
- `debian/`: служба systemd, `/etc/default/hostpulse`, скрипты пакета.
- `tests/`: тесты GoogleTest, проверка JS, `web_smoke.sh`, проверка пакета `deb_install.sh`.

## Лицензия

GPL-3.0-or-later, см. `LICENSE`. `third_party/httplib.h`: MIT, см. `third_party/httplib-LICENSE`.
