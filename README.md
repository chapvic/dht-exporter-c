# DHT Exporter (C)

A Prometheus exporter for DHT11/DHT22/AM2302 temperature and humidity sensors.  
Reads sensor data from the procfs interface created by the [DHT kernel driver](https://github.com/chapvic/dht-driver) at `/proc/sensors/dht/gpio<pin>/` and exposes it as Prometheus metrics on an HTTP endpoint. Optionally writes readings to a JSON file for integration with other monitoring systems.

Written in pure C (POSIX), no external dependencies — only a C compiler and pthreads.

**Version:** 1.0  
**License:** GNU GPLv3  
**Author:** Chapvic  

---

## Table of Contents

- [Overview](#overview)
- [DHT Kernel Driver](#dht-kernel-driver)
- [Features](#features)
- [Requirements](#requirements)
- [Building](#building)
- [Installation](#installation)
- [Service Management](#service-management)
- [Configuration File](#configuration-file)
- [Interval Resolution](#interval-resolution)
- [Command-Line Options](#command-line-options)
- [Prometheus Metrics](#prometheus-metrics)
- [JSON Output](#json-output)
- [Usage Examples](#usage-examples)
  - [Bash: Reading Metrics with curl](#bash-reading-metrics-with-curl)
  - [Bash: Parsing JSON Output with jq](#bash-parsing-json-output-with-jq)
  - [Python: Reading Metrics](#python-reading-metrics)
  - [Python: Parsing JSON Output](#python-parsing-json-output)
- [Driver Availability Tracking](#driver-availability-tracking)
- [Architecture](#architecture)
- [License](#license)

---

## Overview

DHT Exporter bridges the gap between the DHT kernel driver and Prometheus. The driver exposes sensor readings through a procfs interface — the exporter reads that interface on a configurable interval and serves the data as standard Prometheus metrics. This allows you to monitor temperature and humidity from DHT11, DHT22, and AM2302 sensors using any Prometheus-compatible monitoring stack (Grafana, VictoriaMetrics, etc.).

The exporter also supports writing sensor readings to a JSON file, which can be consumed by scripts, dashboards, or other tools that do not speak Prometheus.

This is a C port of the [Go version](https://github.com/chapvic/dht-exporter), with identical functionality and CLI interface.

---

## DHT Kernel Driver

DHT Exporter requires the **DHT kernel driver** to be loaded and sensors to be registered. The driver creates a procfs interface at `/proc/sensors/dht/` where each registered sensor appears as a subdirectory (`gpio<pin>/`) containing the following files:

| File | Description |
|------|-------------|
| `value` | Humidity and temperature in `H=<h>\nT=<t>` format |
| `status_code` | Numeric status code (0 = success) |
| `status_text` | Human-readable status text |
| `timestamp` | Unix timestamp of the last measurement |
| `info` | Sensor type and registration time |

The driver also exposes a global `auto_interval` file that controls the polling interval for all sensors.

**Driver source code:** [https://github.com/chapvic/dht-driver](https://github.com/chapvic/dht-driver)

To register a sensor, write a BCM pin number to `/proc/sensors/dht/export`:

```bash
echo 23 > /proc/sensors/dht/export
```

To unregister a sensor:

```bash
echo 23 > /proc/sensors/dht/unexport
```

---

## Features

- Reads sensor data from `/proc/sensors/dht/gpio<pin>/value`
- Exposes Prometheus metrics on a configurable HTTP endpoint
- Optional JSON file output for integration with other tools
- Dynamic interval tracking: monitors the driver's `auto_interval` and adjusts polling automatically
- Smart interval resolution: explicit `--interval` writes to the driver; without it, the interval is read from the driver
- Configuration file support: reads `PARAMS` from `/etc/default/dht-exporter`
- Graceful shutdown on SIGINT/SIGTERM (instant, via self-pipe and `sigwait`)
- ANSI color logging in terminal, plain text in redirected output
- Sensor name sanitization with hostname fallback
- HTTP read/write timeouts for resource protection
- Immediate first poll on startup, then periodic by interval
- JSON output with structured sensor info (type + registration time as Unix timestamp)
- Atomic JSON writes (temp file + rename) to prevent partial reads
- Driver availability tracking: logs once when the driver disappears and once when it reappears, avoiding log spam
- Security hardening: `SOCK_CLOEXEC` on sockets, `FD_CLOEXEC` on pipe, input validation
- No external dependencies — pure C, POSIX threads, POSIX sockets

---

## Requirements

- Linux kernel with the DHT driver loaded
- C compiler (gcc or clang) with POSIX support
- pthreads (part of glibc/musl)
- systemd (for service management, optional)

---

## Building

```bash
make
```

This compiles `dht-exporter.c` with optimizations (`-O2`) and all warnings enabled (`-Wall -Wextra`), linked with pthreads.

Alternatively, compile manually:

```bash
cc -O2 -Wall -Wextra -pthread -o dht-exporter dht-exporter.c
```

The resulting binary is `dht-exporter` in the current directory.

---

## Installation

```bash
sudo make install
```

This performs the following steps:

1. Installs the binary to `/usr/local/bin/dht-exporter`
2. Installs the systemd unit to `/etc/systemd/system/dht-exporter.service`
3. Installs the default config to `/etc/default/dht-exporter`
4. Creates a dedicated `dht-exporter` user and group (no login, no shell)
5. Creates state directory `/var/lib/dht-exporter/`
6. Reloads systemd and enables the service for boot

To uninstall:

```bash
sudo make uninstall
```

The state directory `/var/lib/dht-exporter/` is preserved — remove it manually if no longer needed.

---

## Service Management

| Command | Action |
|---------|--------|
| `make start` | Start the service |
| `make stop` | Stop the service |
| `make restart` | Restart the service |
| `make status` | Show service status |
| `make logs` | Follow service logs (`journalctl -f`) |

The systemd unit uses `Type=exec` — systemd considers the service active as soon as the binary starts, without requiring sd_notify. The service runs as a dedicated unprivileged user with `CAP_SYS_MODULE` capability (for modprobe) and security hardening options enabled.

---

## Configuration File

The exporter optionally reads a configuration file at `/etc/default/dht-exporter`. This file uses a simple `PARAMS=` variable to pass command-line options:

```sh
# /etc/default/dht-exporter
# Parameters for dht-exporter (parsed before CLI arguments)
PARAMS="--interval 15 --addr 0.0.0.0:9988 --json /var/lib/dht-exporter/readings.json"
```

**Priority:** Config params are parsed first, CLI arguments after — CLI overrides config. If the same option is specified in both, the CLI value wins.

**Startup log:** The exporter reports whether the config file was found:

```
config:   /etc/default/dht-exporter (found)
```
or
```
config:   /etc/default/dht-exporter (not found)
```

The service runs without parameters by default — all options come from `/etc/default/dht-exporter`.

---

## Interval Resolution

The polling interval is resolved in three levels:

| Priority | Source | Behavior |
|----------|--------|----------|
| 1 | `--interval N` (explicit) | Validates 2–60, writes N to the driver's `auto_interval`, uses N |
| 2 | Driver `auto_interval` | If the value is 2–60, uses it without writing to the driver |
| 3 | Default (10) | Fallback when the driver returns `-1` or an invalid value |

When `--interval` is not specified, the exporter reads the current `auto_interval` from the driver and uses it. This allows the driver's interval to be changed independently (e.g., by another tool or sysfs write) and the exporter will follow.

When `--interval` is specified, the exporter writes the value to the driver, ensuring consistency.

**Startup log:**

```
interval: 10 (from driver)
interval: 15 (explicit)
```

---

## Command-Line Options

| Option | Default | Description |
|--------|---------|-------------|
| `--name <name>` | hostname | Exporter name used as a Prometheus label. Special characters are stripped; if empty after sanitization, the system hostname is used. |
| `--addr <addr>` | `0.0.0.0:9988` | HTTP listen address. |
| `--interval <sec>` | from driver | Polling interval in seconds. Valid range: 2–60. If not specified, reads from the driver's `auto_interval`; falls back to 10 if unavailable. |
| `--json <path>` | disabled | Write sensor readings to a JSON file. Updated on each poll. Typical directories: `/etc/`, `/opt/`, `/run/`, `/tmp/`, `/var/lib/`, `/var/log/`, `/usr/local/etc/`. |
| `--driver <name>` | `dht` | Kernel module name for modprobe if the procfs interface is not found at startup. |
| `--rt <sec>` | `5` | HTTP read timeout. Range: 1–60. |
| `--wt <sec>` | `10` | HTTP write timeout. Range: 1–120. Must be >= `--rt`. |
| `--help` | — | Show help message. |

---

## Prometheus Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `dht_temperature_celsius` | Gauge | `name`, `pin` | Temperature in Celsius |
| `dht_humidity_percent` | Gauge | `name`, `pin` | Relative humidity in percent |
| `dht_status_code` | Gauge | `name`, `pin` | Sensor status code (0 = success) |
| `dht_timestamp_seconds` | Gauge | `name`, `pin` | Unix timestamp of last measurement |
| `dht_info` | Gauge | `name`, `pin`, `type`, `status` | Sensor info (always 1) |

---

## JSON Output

When `--json <path>` is specified, the exporter writes a JSON file on each poll. The format:

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

| Field | Type | Description |
|-------|------|-------------|
| `timestamp` | int64 | Unix timestamp of this JSON write |
| `sensors` | array | One entry per registered sensor |
| `sensors[].pin` | int | BCM GPIO pin number |
| `sensors[].humidity` | float64 | Relative humidity in percent |
| `sensors[].temperature` | float64 | Temperature in Celsius |
| `sensors[].status_code` | int | Status code (0 = success) |
| `sensors[].timestamp` | int64 | Unix timestamp of the last sensor measurement |
| `sensors[].info.sensor` | string | Sensor type (e.g. "DHT22", "DHT11") |
| `sensors[].info.registered` | int64 | Sensor registration time as Unix timestamp |
| `sensors[].status_text` | string | Human-readable status (e.g. "SUCCESS") |

Writes are atomic (temp file + `rename`) — external readers never see partial data.

---

## Usage Examples

### Bash: Reading Metrics with curl

```bash
# Get all Prometheus metrics
curl http://localhost:9988/metrics

# Filter temperature only
curl -s http://localhost:9988/metrics | grep dht_temperature

# Health check
curl http://localhost:9988/health
```

### Bash: Parsing JSON Output with jq

```bash
# Read the latest JSON file and pretty-print
jq . /var/lib/dht-exporter/readings.json

# Extract temperature and humidity for pin 23
jq '.sensors[] | select(.pin == 23) | {temp: .temperature, hum: .humidity}' \
  /var/lib/dht-exporter/readings.json

# Get all sensor pins and their status
jq '.sensors[] | {pin, status_text}' /var/lib/dht-exporter/readings.json

# Convert Unix timestamps to human-readable format
jq '.sensors[] | {pin, registered: (.info.registered | strftime("%Y-%m-%d %H:%M:%S"))}' \
  /var/lib/dht-exporter/readings.json
```

### Python: Reading Metrics

```python
import urllib.request

def get_dht_metrics(host="localhost", port=9988):
    """Fetch and parse Prometheus metrics from DHT Exporter."""
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

### Python: Parsing JSON Output

```python
import json
from datetime import datetime

def read_json(path="/var/lib/dht-exporter/readings.json"):
    """Read the JSON file written by DHT Exporter."""
    with open(path) as f:
        data = json.load(f)

    print(f"Last update: {datetime.fromtimestamp(data['timestamp'])}")
    for s in data["sensors"]:
        registered = datetime.fromtimestamp(s["info"]["registered"])
        print(f"  Pin {s['pin']} ({s['info']['sensor']}):")
        print(f"    Temperature: {s['temperature']} C")
        print(f"    Humidity:    {s['humidity']}%")
        print(f"    Status:      {s['status_text']}")
        print(f"    Registered:  {registered}")
        print(f"    Measured:    {datetime.fromtimestamp(s['timestamp'])}")

read_json()
```

---

## Driver Availability Tracking

The exporter tracks whether the DHT kernel driver is loaded and available:

- **Driver disappears:** A single `[WARN] DHT driver not loaded: /proc/sensors/dht/ not found` message is logged. The exporter continues polling silently, waiting for the driver to return.
- **Driver reappears:** A single `[INFO] DHT driver ready: /proc/sensors/dht` message is logged, followed by sensor readings as on startup (`first reading: N sensor(s) registered`).

This prevents log spam while ensuring you are notified of driver state changes.

---

## Architecture

The C version uses two POSIX threads:

- **Poll thread** — reads sensor data on interval, writes JSON, updates shared data protected by a mutex.
- **HTTP thread** — accepts connections, serves `/metrics` and `/health` endpoints from a snapshot of shared data (copied under lock, formatted outside lock to minimize contention).

Signal handling uses `sigwait()` in the main thread (signals are blocked in worker threads). On SIGINT/SIGTERM:

1. Main thread wakes from `sigwait()`
2. A byte is written to a self-pipe to instantly wake the poll thread from `poll()`
3. `shutdown()` is called on the listen socket to wake the HTTP thread from `accept4()`
4. Both threads are joined before cleanup

Sockets use `SOCK_CLOEXEC` to prevent file descriptor leakage to child processes (e.g., modprobe).

This ensures instant and clean shutdown with no resource leaks.

---

## License

GNU General Public License v3. See [LICENSE](LICENSE) for details.

Copyright (c) 2026, Chapvic.
