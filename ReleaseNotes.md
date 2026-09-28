# DHT Exporter (C) — Release 1.0

First public release of DHT Exporter (C) — a Prometheus exporter for DHT11/DHT22/AM2302 temperature and humidity sensors, written in pure C.

## About

DHT Exporter reads sensor data from the procfs interface created by the [DHT kernel driver](https://github.com/chapvic/dht-driver) and exposes it as Prometheus metrics on an HTTP endpoint. It also supports writing readings to a JSON file for integration with other monitoring systems.

This is a C port of the [Go version](https://github.com/chapvic/dht-exporter), with identical functionality and CLI interface — but with zero runtime dependencies.

## Highlights

- **Prometheus metrics**: temperature, humidity, status code, timestamp, and sensor info
- **JSON output**: structured sensor data with type and registration time (Unix timestamp), written atomically (temp file + rename)
- **Smart interval resolution**: explicit `--interval` writes to the driver; without it, the interval is read from the driver's `auto_interval`; falls back to 10 if unavailable
- **Configuration file**: reads `PARAMS` from `/etc/default/dht-exporter`; CLI arguments override config
- **Dynamic interval tracking**: automatically adjusts polling when the driver changes `auto_interval`
- **Driver availability tracking**: logs once when the driver disappears and once when it reappears — no log spam
- **Graceful shutdown**: clean thread join and socket cleanup on SIGINT/SIGTERM via self-pipe
- **Security hardening**: runs as unprivileged user with `CAP_SYS_MODULE` under systemd; `SOCK_CLOEXEC` on all sockets, `FD_CLOEXEC` on pipe; input validation on all parameters
- **No dependencies**: pure C, POSIX threads, POSIX sockets — only a C compiler needed

## Endpoints

| Endpoint | Description |
|----------|-------------|
| `/metrics` | Prometheus metrics |
| `/health` | Health check (200 OK) |

## Metrics

| Metric | Description |
|--------|-------------|
| `dht_temperature_celsius` | Temperature in Celsius |
| `dht_humidity_percent` | Relative humidity in percent |
| `dht_status_code` | Sensor status code (0 = success) |
| `dht_timestamp_seconds` | Unix timestamp of last measurement |
| `dht_info` | Sensor info (always 1) |

## Configuration

The service runs without parameters by default. All options come from `/etc/default/dht-exporter`:

```sh
# /etc/default/dht-exporter
PARAMS="--interval 15 --json /var/lib/dht-exporter/readings.json"
```

CLI arguments override config file parameters.

## Interval Resolution

| Priority | Source | Behavior |
|----------|--------|----------|
| 1 | `--interval N` | Validates 2–60, writes to driver, uses N |
| 2 | Driver `auto_interval` | Uses driver value if in range 2–60 |
| 3 | Default (10) | Fallback when driver unavailable |

## Installation

```bash
make
sudo make install
sudo make start
```

Or build manually:

```bash
cc -O2 -Wall -Wextra -pthread -o dht-exporter dht-exporter.c
```

## Security

This release has been through a security audit. The following issues were identified and fixed:

- **Buffer overflow in `json_escape`**: `snprintf` return value now checked to prevent out-of-bounds writes
- **Hostname injection**: hostname is now sanitized through the same character filter as other names
- **Buffer overflow in metrics/JSON builders**: all `snprintf` calls wrapped in overflow-checking macros
- **Non-atomic JSON writes**: writes now use temp file + `rename` to prevent partial reads
- **Missing socket timeouts**: `SO_RCVTIMEO`/`SO_SNDTIMEO` set on client sockets
- **File descriptor leakage**: `SOCK_CLOEXEC` on sockets, `FD_CLOEXEC` on pipe
- **Input validation**: `--rt`/`--wt` range checks, `strtol` overflow detection, HTTP path validation
- **HTTP robustness**: full request read loop (until `\r\n\r\n`), `send()` retry on `EINTR`, negative length check

## Files

| File | Description |
|------|-------------|
| `dht-exporter.c` | Source code (single file, ~1250 lines) |
| `Makefile` | Build, install, and service management |
| `dht-exporter.service` | systemd unit with security hardening |
| `dht-exporter.default` | Example config file for `/etc/default/dht-exporter` |
| `README.md` | Full documentation (English) |
| `README_RU.md` | Full documentation (Russian) |
| `CHANGELOG.md` | Change history |
| `LICENSE` | GNU GPLv3 |

## License

GNU General Public License v3.

Copyright (c) 2026, Chapvic.
