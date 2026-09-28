# DHT Exporter (C)

Prometheus-экспортёр для датчиков температуры и влажности DHT11/DHT22/AM2302.  
Читает данные датчиков из procfs-интерфейса, создаваемого [DHT-драйвером ядра](https://github.com/chapvic/dht-driver) по пути `/proc/sensors/dht/gpio<pin>/`, и предоставляет их как метрики Prometheus на HTTP-эндпоинте. Опционально записывает показания в JSON-файл для интеграции с другими системами мониторинга.

Написан на чистом C (POSIX), без внешних зависимостей — достаточно C-компилятора и pthreads.

**Версия:** 1.0  
**Лицензия:** GNU GPLv3  
**Автор:** Chapvic  

---

## Содержание

- [Обзор](#обзор)
- [DHT-драйвер ядра](#dht-драйвер-ядра)
- [Возможности](#возможности)
- [Требования](#требования)
- [Сборка](#сборка)
- [Установка](#установка)
- [Управление сервисом](#управление-сервисом)
- [Конфигурационный файл](#конфигурационный-файл)
- [Разрешение интервала](#разрешение-интервала)
- [Параметры командной строки](#параметры-командной-строки)
- [Метрики Prometheus](#метрики-prometheus)
- [JSON-вывод](#json-вывод)
- [Примеры использования](#примеры-использования)
  - [Bash: чтение метрик через curl](#bash-чтение-метрик-через-curl)
  - [Bash: разбор JSON через jq](#bash-разбор-json-через-jq)
  - [Python: чтение метрик](#python-чтение-метрик)
  - [Python: разбор JSON](#python-разбор-json)
- [Отслеживание доступности драйвера](#отслеживание-доступности-драйвера)
- [Архитектура](#архитектура)
- [Лицензия](#лицензия)

---

## Обзор

DHT Exporter соединяет DHT-драйвер ядра и Prometheus. Драйвер предоставляет показания датчиков через procfs-интерфейс — экспортёр читает этот интерфейс с заданным интервалом и отдаёт данные как стандартные метрики Prometheus. Это позволяет мониторить температуру и влажность с датчиков DHT11, DHT22 и AM2302 с помощью любой Prometheus-совместимой системы мониторинга (Grafana, VictoriaMetrics и т.д.).

Экспортёр также поддерживает запись показаний в JSON-файл, который могут использовать скрипты, дашборды и другие инструменты, не работающие с Prometheus.

Это C-порт [Go-версии](https://github.com/chapvic/dht-exporter) с идентичным функционалом и CLI-интерфейсом.

---

## DHT-драйвер ядра

DHT Exporter требует загруженный **DHT-драйвер ядра** и зарегистрированные датчики. Драйвер создаёт procfs-интерфейс по пути `/proc/sensors/dht/`, где каждый датчик представлен подкаталогом (`gpio<pin>/`) со следующими файлами:

| Файл | Описание |
|------|----------|
| `value` | Влажность и температура в формате `H=<h>\nT=<t>` |
| `status_code` | Числовой код статуса (0 = успех) |
| `status_text | Человекочитаемый текст статуса |
| `timestamp` | Unix-временная метка последнего измерения |
| `info` | Тип датчика и время регистрации |

Драйвер также предоставляет глобальный файл `auto_interval`, управляющий интервалом опроса всех датчиков.

**Исходный код драйвера:** [https://github.com/chapvic/dht-driver](https://github.com/chapvic/dht-driver)

Регистрация датчика — запись BCM-номера пина в `/proc/sensors/dht/export`:

```bash
echo 23 > /proc/sensors/dht/export
```

Отмена регистрации:

```bash
echo 23 > /proc/sensors/dht/unexport
```

---

## Возможности

- Чтение данных из `/proc/sensors/dht/gpio<pin>/value`
- Предоставление метрик Prometheus на настраиваемом HTTP-эндпоинте
- Опциональный вывод в JSON-файл для интеграции с другими инструментами
- Динамическое отслеживание интервала: мониторинг `auto_interval` драйвера и автоматическая подстройка опроса
- Умное разрешение интервала: явный `--interval` записывается в драйвер; без него интервал читается из драйвера
- Поддержка конфигурационного файла: чтение `PARAMS` из `/etc/default/dht-exporter`
- Мягкое завершение по SIGINT/SIGTERM (мгновенное, через self-pipe и `sigwait`)
- Цветной лог в терминале, простой текст при перенаправлении
- Санитизация имени с fallback на hostname
- HTTP-таймауты на чтение/запись для защиты ресурсов
- Немедленный первый опрос при запуске, затем периодический по интервалу
- JSON-вывод со структурированной информацией о датчике (тип + время регистрации как Unix-метка)
- Атомарная запись JSON (временный файл + `rename`) — защита от частичных чтений
- Отслеживание доступности драйвера: одно сообщение при исчезновении и одно при появлении, без спама в логе
- Безопасность: `SOCK_CLOEXEC` на сокетах, `FD_CLOEXEC` на канале, валидация входных данных
- Нет внешних зависимостей — чистый C, POSIX threads, POSIX sockets

---

## Требования

- Ядро Linux с загруженным DHT-драйвером
- C-компилятор (gcc или clang) с поддержкой POSIX
- pthreads (часть glibc/musl)
- systemd (для управления сервисом, опционально)

---

## Сборка

```bash
make
```

Компилирует `dht-exporter.c` с оптимизацией (`-O2`) и всеми предупреждениями (`-Wall -Wextra`), линкуется с pthreads.

Ручная компиляция:

```bash
cc -O2 -Wall -Wextra -pthread -o dht-exporter dht-exporter.c
```

Готовый бинарник — `dht-exporter` в текущей директории.

---

## Установка

```bash
sudo make install
```

Выполняет следующие шаги:

1. Устанавливает бинарник в `/usr/local/bin/dht-exporter`
2. Устанавливает systemd-юнит в `/etc/systemd/system/dht-exporter.service`
3. Устанавливает конфигурационный файл в `/etc/default/dht-exporter`
4. Создаёт выделенного пользователя и группу `dht-exporter` (без логина и шелла)
5. Создаёт директорию состояния `/var/lib/dht-exporter/`
6. Перезагружает systemd и включает сервис для автозапуска

Удаление:

```bash
sudo make uninstall
```

Директория состояния `/var/lib/dht-exporter/` сохраняется — удалите вручную, если больше не нужна.

---

## Управление сервисом

| Команда | Действие |
|---------|----------|
| `make start` | Запустить сервис |
| `make stop` | Остановить сервис |
| `make restart` | Перезапустить сервис |
| `make status` | Показать статус сервиса |
| `make logs` | Следить за логами сервиса (`journalctl -f`) |

Systemd-юнит использует `Type=exec` — systemd считает сервис активным с момента запуска бинарника, без sd_notify. Сервис работает от имени выделенного непривилегированного пользователя с `CAP_SYS_MODULE` (для modprobe) и с включёнными опциями безопасности.

---

## Конфигурационный файл

Экспортёр опционально читает конфигурационный файл по пути `/etc/default/dht-exporter`. Файл использует переменную `PARAMS=` для передачи параметров командной строки:

```sh
# /etc/default/dht-exporter
# Параметры для dht-exporter (парсятся до аргументов CLI)
PARAMS="--interval 15 --addr 0.0.0.0:9988 --json /var/lib/dht-exporter/readings.json"
```

**Приоритет:** Параметры из конфига парсятся первыми, аргументы CLI после — CLI перекрывает конфиг. Если опция указана в обоих местах, значение CLI побеждает.

**Лог при запуске:** Экспортёр сообщает, найден ли конфигурационный файл:

```
config:   /etc/default/dht-exporter (found)
```
или
```
config:   /etc/default/dht-exporter (not found)
```

Сервис по умолчанию запускается без параметров — все опции берутся из `/etc/default/dht-exporter`.

---

## Разрешение интервала

Интервал опроса разрешается на трёх уровнях:

| Приоритет | Источник | Поведение |
|-----------|----------|-----------|
| 1 | `--interval N` (явный) | Валидирует 2–60, записывает N в `auto_interval` драйвера, использует N |
| 2 | `auto_interval` драйвера | Если значение 2–60, использует его без записи в драйвер |
| 3 | По умолчанию (10) | Fallback, если драйвер вернул `-1` или недопустимое значение |

Когда `--interval` не указан, экспортёр читает текущий `auto_interval` из драйвера и использует его. Это позволяет менять интервал драйвера независимо (например, другим инструментом или записью в sysfs), и экспортёр последует за ним.

Когда `--interval` указан, экспортёр записывает значение в драйвер, обеспечивая согласованность.

**Лог при запуске:**

```
interval: 10 (from driver)
interval: 15 (explicit)
```

---

## Параметры командной строки

| Параметр | По умолчанию | Описание |
|---------|--------------|----------|
| `--name <имя>` | hostname | Имя экспортёра, используется как метка Prometheus. Спецсимволы удаляются; если пусто после санитизации — используется hostname системы. |
| `--addr <адрес>` | `0.0.0.0:9988` | Адрес HTTP-сервера. |
| `--interval <сек>` | из драйвера | Интервал опроса в секундах. Диапазон: 2–60. Если не указан — читается из `auto_interval` драйвера; fallback на 10. |
| `--json <путь>` | отключено | Запись показаний в JSON-файл. Обновляется при каждом опросе. Типичные директории: `/etc/`, `/opt/`, `/run/`, `/tmp/`, `/var/lib/`, `/var/log/`, `/usr/local/etc/`. |
| `--driver <имя>` | `dht` | Имя модуля ядра для modprobe, если procfs-интерфейс не найден при запуске. |
| `--rt <сек>` | `5` | Таймаут HTTP-чтения. Диапазон: 1–60. |
| `--wt <сек>` | `10` | Таймаут HTTP-записи. Диапазон: 1–120. Должен быть >= `--rt`. |
| `--help` | — | Показать справку. |

---

## Метрики Prometheus

| Метрика | Тип | Метки | Описание |
|---------|-----|-------|----------|
| `dht_temperature_celsius` | Gauge | `name`, `pin` | Температура в градусах Цельсия |
| `dht_humidity_percent` | Gauge | `name`, `pin` | Относительная влажность в процентах |
| `dht_status_code` | Gauge | `name`, `pin` | Код статуса датчика (0 = успех) |
| `dht_timestamp_seconds` | Gauge | `name`, `pin` | Unix-метка последнего измерения |
| `dht_info` | Gauge | `name`, `pin`, `type`, `status` | Информация о датчике (всегда 1) |

---

## JSON-вывод

При указании `--json <путь>` экспортёр записывает JSON-файл при каждом опросе. Формат:

```json
{
  "timestamp": 1727462445,
  "sensors": [
    {
      "pin": 23,
      "humidity": 50.9,
      "temperature": 24.2,
      "status_code": 0,
      "timestamp": 1727462443,
      "info": {
        "sensor": "DHT22",
        "registered": 1727374000
      },
      "status_text": "SUCCESS"
    }
  ]
}
```

| Поле | Тип | Описание |
|------|-----|----------|
| `timestamp` | int64 | Unix-метка записи JSON |
| `sensors` | array | По одной записи на каждый зарегистрированный датчик |
| `sensors[].pin` | int | BCM-номер GPIO-пина |
| `sensors[].humidity` | float64 | Относительная влажность в процентах |
| `sensors[].temperature` | float64 | Температура в Цельсиях |
| `sensors[].status_code` | int | Код статуса (0 = успех) |
| `sensors[].timestamp` | int64 | Unix-метка последнего измерения |
| `sensors[].info.sensor` | string | Тип датчика (напр. "DHT22", "DHT11") |
| `sensors[].info.registered` | int64 | Время регистрации датчика (Unix-метка) |
| `sensors[].status_text` | string | Человекочитаемый статус (напр. "SUCCESS") |

Запись атомарна (временный файл + `rename`) — внешние читатели не увидят частичные данные.

---

## Примеры использования

### Bash: чтение метрик через curl

```bash
# Получить все метрики Prometheus
curl http://localhost:9988/metrics

# Только температура
curl -s http://localhost:9988/metrics | grep dht_temperature

# Проверка здоровья
curl http://localhost:9988/health
```

### Bash: разбор JSON через jq

```bash
# Вывести JSON красиво
jq . /var/lib/dht-exporter/readings.json

# Температура и влажность для пина 23
jq '.sensors[] | select(.pin == 23) | {temp: .temperature, hum: .humidity}' \
  /var/lib/dht-exporter/readings.json

# Все пины и их статусы
jq '.sensors[] | {pin, status_text}' /var/lib/dht-exporter/readings.json

# Unix-метки в читаемом формате
jq '.sensors[] | {pin, registered: (.info.registered | strftime("%Y-%m-%d %H:%M:%S"))}' \
  /var/lib/dht-exporter/readings.json
```

### Python: чтение метрик

```python
import urllib.request

def get_dht_metrics(host="localhost", port=9988):
    """Получить и разобрать метрики Prometheus от DHT Exporter."""
    url = f"http://{host}:{port}/metrics"
    with urllib.request.urlopen(url) as resp:
        data = resp.read().decode()

    metrics = {}
    for line in data.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        name, _, rest = line.partition("{")
        labels_str, _, value = rest.rpartition("}")
        metric_key = name.strip()

        labels = {}
        for pair in labels_str.split(","):
            pair = pair.strip()
            if "=" in pair:
                k, v = pair.split("=", 1)
                labels[k.strip()] = v.strip().strip('"')

        pin = labels.get("pin", "?")
        metrics.setdefault(pin, {})[metric_key] = float(value)

    return metrics

metrics = get_dht_metrics()
for pin, data in metrics.items():
    temp = data.get("dht_temperature_celsius", "N/A")
    hum = data.get("dht_humidity_percent", "N/A")
    print(f"Pin {pin}: T={temp} C, H={hum}%")
```

### Python: разбор JSON

```python
import json
from datetime import datetime

def read_json(path="/var/lib/dht-exporter/readings.json"):
    """Прочитать JSON-файл, записанный DHT Exporter."""
    with open(path) as f:
        data = json.load(f)

    print(f"Последнее обновление: {datetime.fromtimestamp(data['timestamp'])}")
    for s in data["sensors"]:
        registered = datetime.fromtimestamp(s["info"]["registered"])
        print(f"  Pin {s['pin']} ({s['info']['sensor']}):")
        print(f"    Температура: {s['temperature']} C")
        print(f"    Влажность:   {s['humidity']}%")
        print(f"    Статус:      {s['status_text']}")
        print(f"    Регистрация: {registered}")
        print(f"    Измерение:   {datetime.fromtimestamp(s['timestamp'])}")

read_json()
```

---

## Отслеживание доступности драйвера

Экспортёр отслеживает, загружен ли DHT-драйвер ядра:

- **Драйвер исчез:** Одно сообщение `[WARN] DHT driver not loaded: /proc/sensors/dht/ not found`. Экспортёр продолжает опрос молча, ожидая возвращения драйвера.
- **Драйвер появился:** Одно сообщение `[INFO] DHT driver ready: /proc/sensors/dht`, затем показания датчиков как при запуске (`first reading: N sensor(s) registered`).

Это предотвращает спам в логе, гарантируя уведомление об изменениях состояния драйвера.

---

## Архитектура

C-версия использует два POSIX-потока:

- **Поток опроса** — читает данные датчиков с интервалом, пишет JSON, обновляет разделяемые данные под мьютексом.
- **HTTP-поток** — принимает соединения, отдаёт `/metrics` и `/health` из снимка разделяемых данных (копирование под блокировкой, форматирование вне блокировки для минимизации конкуренции).

Обработка сигналов через `sigwait()` в главном потоке (сигналы заблокированы в рабочих потоках). При SIGINT/SIGTERM:

1. Главный поток выходит из `sigwait()`
2. Байт записывается в self-pipe для мгновенного пробуждения потока опроса из `poll()`
3. `shutdown()` вызывается на слушающем сокете для пробуждения HTTP-потока из `accept4()`
4. Оба потока присоединяются перед очисткой

Сокеты используют `SOCK_CLOEXEC` для предотвращения утечки файловых дескрипторов в дочерние процессы (напр., modprobe).

Это обеспечивает мгновенное и чистое завершение без утечек ресурсов.

---

## Лицензия

GNU General Public License v3. См. [LICENSE](LICENSE).

Copyright (c) 2026, Chapvic.
