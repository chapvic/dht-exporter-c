# Changelog

## [1.0] - 2026-09-28

First public release.

### Features
- Prometheus metrics: temperature, humidity, status code, timestamp, sensor info
- JSON output with structured sensor data (type + registration time), atomic writes
- Smart interval resolution: explicit `--interval` → driver `auto_interval` → default 10
- Configuration file support: reads `PARAMS` from `/etc/default/dht-exporter`
- Dynamic interval tracking: follows driver `auto_interval` changes automatically
- Driver availability tracking: single log entry on disappear/reappear, no spam
- Graceful shutdown via SIGINT/SIGTERM with self-pipe and `sigwait`
- ANSI color logging in terminal, plain text when redirected
- Sensor name sanitization with hostname fallback
- HTTP read/write timeouts (`SO_RCVTIMEO`/`SO_SNDTIMEO`)
- Immediate first poll on startup
- systemd integration with security hardening (Type=exec, CAP_SYS_MODULE)

### Security
- Buffer overflow protection in `json_escape` (snprintf return check)
- Hostname sanitization through character filter
- Overflow-checking macros (`MW`/`JW`) for all metrics/JSON buffer writes
- Atomic JSON writes (temp file + rename)
- `SOCK_CLOEXEC` on all sockets, `FD_CLOEXEC` on pipe
- Input validation: `--rt`/`--wt` range checks, `strtol` overflow detection
- HTTP robustness: full request read loop, `send()` retry on EINTR, negative length check
- Empty HTTP path → 404 before route matching
- Mutex held only during data copy, not during metrics formatting
