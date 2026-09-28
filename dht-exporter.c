/*
 * dht-exporter.c - Prometheus Exporter for DHT11/DHT22/AM2302 Sensors (C port)
 *
 * C port of the Go dht-exporter. Reads temperature and humidity data from
 * the procfs interface created by the dht kernel driver at /proc/sensors/dht/.
 *
 * Copyright (c) 2026, Chapvic
 * License: GPLv3
 *
 * Build: cc -O2 -Wall -Wextra -pthread -o dht-exporter dht-exporter.c
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define VERSION "1.0"
#define DEFAULT_ADDR "0.0.0.0:9988"
#define DEFAULT_INTERVAL 10
#define DEFAULT_READ_TO 5
#define DEFAULT_WRITE_TO 10
#define DEFAULT_DRIVER "dht"
#define MIN_INTERVAL 2
#define MAX_INTERVAL 60
#define MIN_READ_TO 1
#define MAX_READ_TO 60
#define MIN_WRITE_TO 1
#define MAX_WRITE_TO 120
#define PROC_BASE_DIR "/proc/sensors/dht"
#define AUTO_INTERVAL_FILE "/proc/sensors/dht/auto_interval"
#define CONFIG_FILE "/etc/default/dht-exporter"

#define MAX_SENSORS 64
#define MAX_NAME_LEN 128
#define MAX_PATH_LEN 1024
#define MAX_LINE_LEN 4096
#define MAX_STATUS_TEXT 128
#define MAX_INFO_LEN 512
#define MAX_METRICS_BUF (256 * 1024)
#define MAX_JSON_BUF (64 * 1024)
#define BACKLOG 16
#define MAX_HTTP_BUF 4096

/* ── Logging ────────────────────────────────────────────────────────────── */

static int g_use_color = 0;

static void log_msg(const char *level, const char *color, const char *fmt, ...) {
    char msg[MAX_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y/%m/%d %H:%M:%S", &tm);

    if (g_use_color)
        fprintf(stderr, "%s %s%s%s %s\n", ts, color, level, "\033[0m", msg);
    else
        fprintf(stderr, "%s [%s] %s\n", ts, level, msg);
    fflush(stderr);
}

#define logInfo(...)  log_msg("[INFO]",  "\033[36m", __VA_ARGS__)
#define logWarn(...)  log_msg("[WARN]",  "\033[33m", __VA_ARGS__)
#define logError(...) log_msg("[ERROR]", "\033[31m", __VA_ARGS__)

/* ── Data structures ────────────────────────────────────────────────────── */

typedef struct {
    int pin;
    double humidity;
    double temperature;
    int status_code;
    long timestamp;
    char info[MAX_INFO_LEN];
    char status_text[MAX_STATUS_TEXT];
} sensor_reading_t;

typedef struct {
    char sensor[MAX_NAME_LEN];
    long registered;
} json_sensor_info_t;

typedef struct {
    char name[MAX_NAME_LEN];
    char addr[64];
    int interval;
    int interval_explicit;
    char json_path[MAX_PATH_LEN];
    char driver_name[64];
    int read_timeout;
    int write_timeout;
} exporter_t;

typedef struct {
    sensor_reading_t readings[MAX_SENSORS];
    int count;
    int valid;
} shared_data_t;

/* ── Globals ────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop = 0;
static int g_listen_fd = -1;
static int g_sigpipe[2] = { -1, -1 };
static shared_data_t g_data;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_exporter_name[MAX_NAME_LEN];

/* ── Helper: safe string copy ───────────────────────────────────────────── */

static void safe_copy(char *dst, const char *src, size_t dstsz) {
    if (dstsz == 0) return;
    size_t len = strlen(src);
    if (len >= dstsz) len = dstsz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ── Helper: build path ────────────────────────────────────────────────── */

static int build_path(char *dst, size_t dstsz, const char *dir, const char *suffix) {
    int n = snprintf(dst, dstsz, "%s/%s", dir, suffix);
    if (n < 0 || (size_t)n >= dstsz) return -1;
    return 0;
}

/* ── Helper: read file to string ────────────────────────────────────────── */

static int read_file_to_buf(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t total = 0;
    while (total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, bufsz - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';
    return (int)total;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* ── sanitize_name ──────────────────────────────────────────────────────── */

static void sanitize_name(char *name, size_t name_sz) {
    char tmp[MAX_NAME_LEN];
    size_t j = 0;
    for (size_t i = 0; name[i] && j < sizeof(tmp) - 1; i++) {
        char c = name[i];
        if (isalpha((unsigned char)c) || isdigit((unsigned char)c) ||
            c == '-' || c == '.' || c == '_') {
            tmp[j++] = c;
        }
    }
    tmp[j] = '\0';

    if (tmp[0] == '\0') {
        char hostname[256] = {0};
        gethostname(hostname, sizeof(hostname) - 1);
        size_t hj = 0;
        for (size_t k = 0; hostname[k] && hj < sizeof(tmp) - 1; k++) {
            char c = hostname[k];
            if (isalpha((unsigned char)c) || isdigit((unsigned char)c) ||
                c == '-' || c == '.' || c == '_')
                tmp[hj++] = c;
        }
        tmp[hj] = '\0';
        if (tmp[0] == '\0')
            safe_copy(tmp, "localhost", sizeof(tmp));
    }
    safe_copy(name, tmp, name_sz);
}

/* ── validate_json_path ────────────────────────────────────────────────── */

static void validate_json_path(const char *path) {
    if (!path || !*path) return;
    const char *prefixes[] = {
        "/etc/", "/opt/", "/run/", "/tmp/",
        "/var/lib/", "/var/log/", "/usr/local/etc/", NULL
    };
    for (int i = 0; prefixes[i]; i++) {
        if (strncmp(path, prefixes[i], strlen(prefixes[i])) == 0)
            return;
    }
    logWarn("JSON path '%s' is outside typical directories", path);
}

/* ── read_auto_interval ────────────────────────────────────────────────── */

static int read_auto_interval(void) {
    char buf[64];
    if (read_file_to_buf(AUTO_INTERVAL_FILE, buf, sizeof(buf)) < 0)
        return -1;
    char *s = trim(buf);
    char *end;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || s == end)
        return -1;
    return (int)val;
}

/* ── write_auto_interval ───────────────────────────────────────────────── */

static int write_auto_interval(int interval) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", interval);
    int fd = open(AUTO_INTERVAL_FILE, O_WRONLY);
    if (fd < 0) {
        logWarn("cannot write auto_interval: %s", strerror(errno));
        return -1;
    }
    ssize_t n = write(fd, buf, len);
    close(fd);
    if (n != len) {
        logWarn("short write to auto_interval");
        return -1;
    }
    return 0;
}

/* ── read_config_params ────────────────────────────────────────────────── */

static int read_config_params(char ***out_argv, int *out_argc) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return -1;

    char line[MAX_LINE_LEN];
    char *params_val = NULL;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == '\0') continue;
        if (strncmp(s, "PARAMS=", 7) == 0) {
            char *val = s + 7;
            /* Strip surrounding quotes */
            size_t vlen = strlen(val);
            if (vlen >= 2 && ((val[0] == '"' && val[vlen-1] == '"') ||
                              (val[0] == '\'' && val[vlen-1] == '\''))) {
                val++;
                val[strlen(val) - 1] = '\0';
            }
            params_val = strdup(val);
            break;
        }
    }
    fclose(f);

    if (!params_val) return 0;  /* file found, but no PARAMS */

    /* Tokenize params_val into argv array */
    char **argv = NULL;
    int argc = 0;
    int cap = 8;
    argv = malloc(cap * sizeof(char *));
    if (!argv) { free(params_val); return -1; }

    char *p = params_val;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char *token_start;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            token_start = p;
            while (*p && *p != quote) p++;
            if (*p) *p++ = '\0';
        } else {
            token_start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
        }

        if (argc >= cap) {
            cap *= 2;
            char **tmp = realloc(argv, cap * sizeof(char *));
            if (!tmp) { free(argv); free(params_val); return -1; }
            argv = tmp;
        }
        argv[argc++] = strdup(token_start);
    }

    free(params_val);

    if (argc == 0) { free(argv); return 0; }

    *out_argv = argv;
    *out_argc = argc;
    return 1;
}

/* ── ensure_driver ──────────────────────────────────────────────────────── */

static void ensure_driver(const char *driver_name) {
    struct stat st;
    if (stat(PROC_BASE_DIR, &st) == 0)
        return;

    logInfo("driver path %s/ not found, attempting modprobe %s",
            PROC_BASE_DIR, driver_name);

    pid_t pid = fork();
    if (pid < 0) {
        logWarn("modprobe fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        /* Child */
        execlp("modprobe", "modprobe", driver_name, (char *)NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        logInfo("modprobe %s completed", driver_name);
    } else {
        logWarn("modprobe %s failed — driver may need to be loaded manually",
                driver_name);
    }
}

/* ── parse_value_file ──────────────────────────────────────────────────── */

static int parse_value_file(const char *data, double *humidity, double *temperature) {
    int h_found = 0, t_found = 0;
    *humidity = 0.0;
    *temperature = 0.0;

    char buf[MAX_LINE_LEN];
    safe_copy(buf, data, sizeof(buf));

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        line = trim(line);
        if (strncmp(line, "H=", 2) == 0) {
            char *end;
            double val = strtod(line + 2, &end);
            if (end != line + 2) {
                *humidity = val;
                h_found = 1;
            }
        }
        if (strncmp(line, "T=", 2) == 0) {
            char *end;
            double val = strtod(line + 2, &end);
            if (end != line + 2) {
                *temperature = val;
                t_found = 1;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return h_found && t_found;
}

/* ── read_sensors ──────────────────────────────────────────────────────── */

static int read_sensors(sensor_reading_t *readings, int max_count) {
    DIR *dir = opendir(PROC_BASE_DIR);
    if (!dir) {
        if (errno == ENOENT)
            return -1;  /* driver not loaded */
        return -2;       /* other error */
    }

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && count < max_count) {
        const char *name = entry->d_name;
        if (strncmp(name, "gpio", 4) != 0)
            continue;

        char *end;
        errno = 0;
        long pin = strtol(name + 4, &end, 10);
        if (errno == ERANGE || *end != '\0' || end == name + 4)
            continue;

        char sensor_dir[MAX_PATH_LEN];
        int n = snprintf(sensor_dir, sizeof(sensor_dir), "%s/%s",
                         PROC_BASE_DIR, name);
        if (n < 0 || (size_t)n >= sizeof(sensor_dir))
            continue;

        char file_path[MAX_PATH_LEN];
        char buf[MAX_LINE_LEN];

        /* Read value */
        if (build_path(file_path, sizeof(file_path), sensor_dir, "value") < 0)
            continue;
        if (read_file_to_buf(file_path, buf, sizeof(buf)) < 0) {
            logWarn("cannot read value for pin %ld", pin);
            continue;
        }
        double humidity, temperature;
        if (!parse_value_file(buf, &humidity, &temperature)) {
            logWarn("cannot parse value for pin %ld", pin);
            continue;
        }

        sensor_reading_t *r = &readings[count];
        memset(r, 0, sizeof(*r));
        r->pin = (int)pin;
        r->humidity = humidity;
        r->temperature = temperature;

        /* Read status_code */
        if (build_path(file_path, sizeof(file_path), sensor_dir, "status_code") == 0 &&
            read_file_to_buf(file_path, buf, sizeof(buf)) >= 0) {
            char *s = trim(buf);
            r->status_code = (int)strtol(s, NULL, 10);
        }

        /* Read status_text */
        if (build_path(file_path, sizeof(file_path), sensor_dir, "status_text") == 0 &&
            read_file_to_buf(file_path, buf, sizeof(buf)) >= 0) {
            char *s = trim(buf);
            safe_copy(r->status_text, s, sizeof(r->status_text));
        }

        /* Read timestamp */
        if (build_path(file_path, sizeof(file_path), sensor_dir, "timestamp") == 0 &&
            read_file_to_buf(file_path, buf, sizeof(buf)) >= 0) {
            char *s = trim(buf);
            r->timestamp = strtol(s, NULL, 10);
        }

        /* Read info */
        if (build_path(file_path, sizeof(file_path), sensor_dir, "info") == 0 &&
            read_file_to_buf(file_path, buf, sizeof(buf)) >= 0) {
            char *s = trim(buf);
            safe_copy(r->info, s, sizeof(r->info));
        }

        count++;
    }

    closedir(dir);
    return count;
}

/* ── parse_info ────────────────────────────────────────────────────────── */

static void parse_info(const char *raw_info, json_sensor_info_t *out) {
    memset(out, 0, sizeof(*out));
    out->sensor[0] = '\0';
    out->registered = 0;

    char buf[MAX_INFO_LEN];
    safe_copy(buf, raw_info, sizeof(buf));

    char registered_str[64] = {0};

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        line = trim(line);
        if (strncmp(line, "Sensor type:", 12) == 0) {
            char *val = trim(line + 12);
            safe_copy(out->sensor, val, sizeof(out->sensor));
        } else if (strncmp(line, "Register time:", 14) == 0) {
            char *val = trim(line + 14);
            safe_copy(registered_str, val, sizeof(registered_str));
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Parse ISO 8601 → Unix timestamp (UTC) */
    if (registered_str[0]) {
        int y, mo, d, h, mi, se;
        if (sscanf(registered_str, "%d-%d-%dT%d:%d:%dZ",
                   &y, &mo, &d, &h, &mi, &se) == 6) {
            struct tm tm = {0};
            tm.tm_year = y - 1900;
            tm.tm_mon = mo - 1;
            tm.tm_mday = d;
            tm.tm_hour = h;
            tm.tm_min = mi;
            tm.tm_sec = se;
            out->registered = timegm(&tm);
        }
    }

    if (out->sensor[0] == '\0' && out->registered == 0) {
        safe_copy(out->sensor, raw_info, sizeof(out->sensor));
    }
}

/* ── JSON escape ───────────────────────────────────────────────────────── */

static void json_escape(char *dst, size_t dstsz, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstsz - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j < dstsz - 2) { dst[j++] = '\\'; dst[j++] = c; }
        } else if (c == '\n') {
            if (j < dstsz - 2) { dst[j++] = '\\'; dst[j++] = 'n'; }
        } else if (c == '\r') {
            if (j < dstsz - 2) { dst[j++] = '\\'; dst[j++] = 'r'; }
        } else if (c == '\t') {
            if (j < dstsz - 2) { dst[j++] = '\\'; dst[j++] = 't'; }
        } else if ((unsigned char)c < 0x20) {
            int w = snprintf(dst + j, dstsz - j, "\\u%04x", (unsigned char)c);
            if (w < 0 || (size_t)w >= dstsz - j) { dst[j] = '\0'; return; }
            j += (size_t)w;
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

/* ── write_json ────────────────────────────────────────────────────────── */

static void write_json(const sensor_reading_t *readings, int count, const char *path) {
    char *buf = malloc(MAX_JSON_BUF);
    if (!buf) return;
    size_t bufsz = MAX_JSON_BUF;

    size_t pos = 0;
#define JW(fmt, ...) do { \
        int _w = snprintf(buf + pos, bufsz - pos, fmt, ##__VA_ARGS__); \
        if (_w < 0 || (size_t)_w >= bufsz - pos) { goto jw_done; } \
        pos += (size_t)_w; \
    } while (0)

    JW("{\n  \"timestamp\": %ld,\n  \"sensors\": [", (long)time(NULL));

    for (int i = 0; i < count; i++) {
        const sensor_reading_t *r = &readings[i];
        json_sensor_info_t info;
        parse_info(r->info, &info);

        char esc_sensor[MAX_NAME_LEN * 2];
        char esc_status[MAX_STATUS_TEXT * 2];
        json_escape(esc_sensor, sizeof(esc_sensor), info.sensor);
        json_escape(esc_status, sizeof(esc_status), r->status_text);

        if (i > 0) JW(",");
        JW("\n    {\n"
            "      \"pin\": %d,\n"
            "      \"humidity\": %.1f,\n"
            "      \"temperature\": %.1f,\n"
            "      \"status_code\": %d,\n"
            "      \"timestamp\": %ld,\n"
            "      \"info\": {\n"
            "        \"sensor\": \"%s\",\n"
            "        \"registered\": %ld\n"
            "      },\n"
            "      \"status_text\": \"%s\"\n"
            "    }",
            r->pin, r->humidity, r->temperature,
            r->status_code, r->timestamp,
            esc_sensor, info.registered, esc_status);

        if (pos >= bufsz - 256) break;
    }

    JW("\n  ]\n}\n");
#undef JW

jw_done:
    /* Atomic write: temp file + rename */
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        logError("cannot write JSON file '%s': %s", tmp_path, strerror(errno));
        free(buf);
        return;
    }
    fwrite(buf, 1, pos, f);
    if (fclose(f) != 0) {
        logError("cannot close JSON file '%s': %s", tmp_path, strerror(errno));
        unlink(tmp_path);
        free(buf);
        return;
    }
    if (rename(tmp_path, path) != 0) {
        logError("cannot rename '%s' to '%s': %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
    }
    free(buf);
}

/* ── log_sensor_readings ────────────────────────────────────────────────── */

static void log_sensor_readings(const sensor_reading_t *readings, int count,
                                int prev_count, int is_first) {
    if (is_first)
        logInfo("first reading: %d sensor(s) registered", count);
    else
        logInfo("sensors changed: %d -> %d", prev_count, count);

    for (int i = 0; i < count; i++) {
        const sensor_reading_t *r = &readings[i];
        char status[MAX_STATUS_TEXT];
        if (r->status_text[0])
            safe_copy(status, r->status_text, sizeof(status));
        else if (r->status_code == 0)
            safe_copy(status, "SUCCESS", sizeof(status));
        else
            snprintf(status, sizeof(status), "ERROR(%d)", r->status_code);

        logInfo("gpio%d: T=%.1f C, H=%.1f%%, status: %s",
                r->pin, r->temperature, r->humidity, status);
    }
}

/* ── build_metrics ──────────────────────────────────────────────────────── */

static int build_metrics(char *buf, size_t bufsz, const sensor_reading_t *readings,
                         int count, const char *name) {
    size_t pos = 0;
#define MW(fmt, ...) do { \
        int _w = snprintf(buf + pos, bufsz - pos, fmt, ##__VA_ARGS__); \
        if (_w < 0 || (size_t)_w >= bufsz - pos) { buf[pos < bufsz ? pos : bufsz - 1] = '\0'; return (int)pos; } \
        pos += (size_t)_w; \
    } while (0)

    MW("# HELP dht_temperature_celsius Temperature in Celsius\n"
       "# TYPE dht_temperature_celsius gauge\n");
    MW("# HELP dht_humidity_percent Relative humidity in percent\n"
       "# TYPE dht_humidity_percent gauge\n");
    MW("# HELP dht_status_code Sensor status code (0 = success)\n"
       "# TYPE dht_status_code gauge\n");
    MW("# HELP dht_timestamp_seconds Unix timestamp of last measurement\n"
       "# TYPE dht_timestamp_seconds gauge\n");
    MW("# HELP dht_info Sensor info (always 1)\n"
       "# TYPE dht_info gauge\n");

    for (int i = 0; i < count; i++) {
        const sensor_reading_t *r = &readings[i];

        /* Determine sensor type */
        char lower_info[MAX_INFO_LEN];
        safe_copy(lower_info, r->info, sizeof(lower_info));
        for (char *p = lower_info; *p; p++) *p = tolower((unsigned char)*p);

        const char *sensor_type = "unknown";
        if (strstr(lower_info, "dht22") || strstr(lower_info, "am2302"))
            sensor_type = "dht22";
        else if (strstr(lower_info, "dht11"))
            sensor_type = "dht11";

        const char *status_str = (r->status_code != 0) ? "error" : "success";

        MW("dht_temperature_celsius{name=\"%s\",pin=\"%d\"} %.1f\n",
           name, r->pin, r->temperature);
        MW("dht_humidity_percent{name=\"%s\",pin=\"%d\"} %.1f\n",
           name, r->pin, r->humidity);
        MW("dht_status_code{name=\"%s\",pin=\"%d\"} %d\n",
           name, r->pin, r->status_code);
        MW("dht_timestamp_seconds{name=\"%s\",pin=\"%d\"} %ld\n",
           name, r->pin, r->timestamp);
        MW("dht_info{name=\"%s\",pin=\"%d\",type=\"%s\",status=\"%s\"} 1\n",
           name, r->pin, sensor_type, status_str);

        if (pos >= bufsz - 512) break;
    }
#undef MW

    buf[pos < bufsz ? pos : bufsz - 1] = '\0';
    return (int)pos;
}

/* ── HTTP helpers ──────────────────────────────────────────────────────── */

static void http_send_response(int fd, int code, const char *content_type,
                               const char *body, size_t body_len) {
    char header[512];
    const char *status = (code == 200) ? "200 OK" :
                         (code == 404) ? "404 Not Found" : "500 Internal Server Error";

    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", status, content_type, body_len);

    if (hlen < 0) return;

    /* Send header with retry on EINTR */
    size_t total_sent = 0;
    while (total_sent < (size_t)hlen) {
        ssize_t s = send(fd, header + total_sent, (size_t)hlen - total_sent, MSG_NOSIGNAL);
        if (s < 0) { if (errno == EINTR) continue; return; }
        if (s == 0) return;
        total_sent += (size_t)s;
    }

    /* Send body with retry on EINTR */
    if (body && body_len) {
        total_sent = 0;
        while (total_sent < body_len) {
            ssize_t s = send(fd, body + total_sent, body_len - total_sent, MSG_NOSIGNAL);
            if (s < 0) { if (errno == EINTR) continue; return; }
            if (s == 0) return;
            total_sent += (size_t)s;
        }
    }
}

/* ── HTTP thread ───────────────────────────────────────────────────────── */

static void *http_thread(void *arg) {
    exporter_t *e = (exporter_t *)arg;
    struct pollfd pfd;
    pfd.fd = g_listen_fd;
    pfd.events = POLLIN;

    while (!g_stop) {
        pfd.revents = 0;
        int ret = poll(&pfd, 1, 200);  /* 200ms poll for quick shutdown */
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;  /* timeout */
        if (!(pfd.revents & POLLIN)) break;

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept4(g_listen_fd, (struct sockaddr *)&client_addr, &addr_len, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (g_stop) break;
            continue;
        }

        /* Set read/write timeouts on client socket */
        struct timeval rtv = { .tv_sec = e->read_timeout, .tv_usec = 0 };
        struct timeval wtv = { .tv_sec = e->write_timeout, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &wtv, sizeof(wtv));

        /* Read request — loop until we get the full HTTP header */
        char req_buf[MAX_HTTP_BUF];
        size_t req_total = 0;
        int got_header = 0;
        while (req_total < sizeof(req_buf) - 1) {
            ssize_t n = recv(client_fd, req_buf + req_total,
                            sizeof(req_buf) - 1 - req_total, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            req_total += (size_t)n;
            req_buf[req_total] = '\0';
            if (strstr(req_buf, "\r\n\r\n")) { got_header = 1; break; }
        }
        if (req_total == 0 || !got_header) {
            close(client_fd);
            continue;
        }

        /* Parse first line: METHOD PATH HTTP/x.y */
        char method[16], path[MAX_PATH_LEN];
        method[0] = path[0] = '\0';
        sscanf(req_buf, "%15s %1023s", method, path);

        if (path[0] == '\0') {
            http_send_response(client_fd, 404, "text/plain", "Not Found\n", 9);
            close(client_fd);
            continue;
        }

        if (strncmp(path, "/metrics", 8) == 0) {
            /* Copy shared data under lock, then format outside lock */
            sensor_reading_t snap[MAX_SENSORS];
            int snap_count;
            pthread_mutex_lock(&g_mutex);
            snap_count = g_data.count;
            memcpy(snap, g_data.readings, sizeof(snap));
            pthread_mutex_unlock(&g_mutex);

            char *metrics_buf = malloc(MAX_METRICS_BUF);
            if (metrics_buf) {
                int mlen = build_metrics(metrics_buf, MAX_METRICS_BUF,
                                        snap, snap_count,
                                        g_exporter_name);
                http_send_response(client_fd, 200, "text/plain; version=0.0.4",
                                  metrics_buf, mlen);
                free(metrics_buf);
            } else {
                http_send_response(client_fd, 500, "text/plain", "Internal error", 14);
            }
        } else if (strncmp(path, "/health", 7) == 0) {
            http_send_response(client_fd, 200, "text/plain", "OK\n", 3);
        } else {
            http_send_response(client_fd, 404, "text/plain", "Not Found\n", 9);
        }

        close(client_fd);
    }

    logInfo("HTTP server stopped");
    return NULL;
}

/* ── Poll thread ───────────────────────────────────────────────────────── */

static void *poll_thread(void *arg) {
    exporter_t *e = (exporter_t *)arg;

    /* Interval already resolved in main() */
    int interval = e->interval;

    /* Write to driver only if user explicitly specified --interval */
    if (e->interval_explicit) {
        if (write_auto_interval(interval) == 0)
            logInfo("driver interval set to %d seconds", interval);
    }

    int prev_sensor_count = -1;
    int last_driver_interval = read_auto_interval();
    int driver_available = 1;
    int force_first_log = 0;

    /* Immediate first poll */
    sensor_reading_t local[MAX_SENSORS];
    int rc = read_sensors(local, MAX_SENSORS);

    if (rc < 0) {
        if (rc == -1) {
            logWarn("DHT driver not loaded: %s/ not found", PROC_BASE_DIR);
            driver_available = 0;
        } else {
            logWarn("cannot read sensors: %s", strerror(errno));
        }
    } else {
        int count = rc;
        pthread_mutex_lock(&g_mutex);
        memcpy(g_data.readings, local, sizeof(local));
        g_data.count = count;
        g_data.valid = 1;
        pthread_mutex_unlock(&g_mutex);

        prev_sensor_count = count;
        log_sensor_readings(local, count, 0, 1);
        if (e->json_path[0])
            write_json(local, count, e->json_path);
    }

    /* Main poll loop */
    struct pollfd pfd;
    pfd.fd = g_sigpipe[0];
    pfd.events = POLLIN;

    while (!g_stop) {
        /* Wait for interval or signal */
        int remaining = interval;
        while (remaining > 0 && !g_stop) {
            int timeout = (remaining > 1) ? 1000 : (remaining * 1000);
            pfd.revents = 0;
            int ret = poll(&pfd, 1, timeout);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret > 0 && (pfd.revents & POLLIN)) {
                /* Signal received */
                goto poll_exit;
            }
            remaining -= (timeout / 1000);
            if (timeout % 1000) remaining -= 1;
        }
        if (g_stop) break;

        /* Dynamic interval tracking */
        int driver_interval = read_auto_interval();
        if (driver_interval >= MIN_INTERVAL && driver_interval <= MAX_INTERVAL) {
            if (driver_interval != last_driver_interval && last_driver_interval >= 0) {
                logInfo("driver interval changed: %d -> %d, adjusting",
                        last_driver_interval, driver_interval);
                interval = driver_interval;
            }
            last_driver_interval = driver_interval;
        }

        /* Read sensors */
        rc = read_sensors(local, MAX_SENSORS);
        if (rc < 0) {
            if (rc == -1) {
                if (driver_available) {
                    logWarn("DHT driver not loaded: %s/ not found", PROC_BASE_DIR);
                    driver_available = 0;
                }
            } else {
                logWarn("cannot read sensors: %s", strerror(errno));
            }
            continue;
        }

        int count = rc;

        /* Driver recovery: log once when driver reappears */
        if (!driver_available) {
            logInfo("DHT driver ready: %s", PROC_BASE_DIR);
            driver_available = 1;
            force_first_log = 1;
        }

        /* Update shared data */
        pthread_mutex_lock(&g_mutex);
        memcpy(g_data.readings, local, sizeof(local));
        g_data.count = count;
        g_data.valid = 1;
        pthread_mutex_unlock(&g_mutex);

        /* Log on first poll, count change, or driver recovery */
        if (force_first_log) {
            log_sensor_readings(local, count, 0, 1);
            force_first_log = 0;
        } else if (prev_sensor_count != count) {
            log_sensor_readings(local, count, prev_sensor_count, 0);
        }
        prev_sensor_count = count;

        /* Write JSON if configured */
        if (e->json_path[0])
            write_json(local, count, e->json_path);
    }

poll_exit:
    logInfo("poll loop stopped");
    return NULL;
}

/* ── print_help ─────────────────────────────────────────────────────────── */

static void print_help(void) {
    fprintf(stderr,
"DHT Exporter, version %s\n"
"\n"
"A Prometheus exporter for DHT11/DHT22/AM2302 temperature and humidity sensors.\n"
"Reads sensor data from the procfs interface created by the dht kernel driver\n"
"at /proc/sensors/dht/gpio<pin>/.\n"
"\n"
"Copyright (c) 2026, Chapvic\n"
"\n"
"Usage:\n"
"  dht-exporter [options]\n"
"\n"
"Options:\n"
"  --name <name>        Exporter name used as a Prometheus label.\n"
"  --addr <addr>        HTTP listen address (default: 0.0.0.0:9988)\n"
"  --interval <sec>     Polling interval 2-60 (default: 10, from driver)\n"
"  --json <path>        Write sensor readings to JSON file\n"
"  --driver <name>      Kernel module name for modprobe (default: dht)\n"
"  --rt <sec>           HTTP read timeout 1-60 (default: 5)\n"
"  --wt <sec>           HTTP write timeout 1-120, >= --rt (default: 10)\n"
"  --help               Show this help message\n"
"\n"
"Configuration:\n"
"  /etc/default/dht-exporter  Optional config file (PARAMS=\"--interval 15 ...\")\n"
"  Config params are parsed first; CLI arguments override them.\n"
"\n"
"Endpoints:\n"
"  /metrics             Prometheus metrics\n"
"  /health              Health check (200 OK)\n"
"\n"
"License: GNU GPLv3\n", VERSION);
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Detect terminal for color */
    g_use_color = isatty(STDERR_FILENO);

    exporter_t e;
    memset(&e, 0, sizeof(e));
    safe_copy(e.addr, DEFAULT_ADDR, sizeof(e.addr));
    e.interval = DEFAULT_INTERVAL;
    e.interval_explicit = 0;
    safe_copy(e.driver_name, DEFAULT_DRIVER, sizeof(e.driver_name));
    e.read_timeout = DEFAULT_READ_TO;
    e.write_timeout = DEFAULT_WRITE_TO;

    /* Read config file /etc/default/dht-exporter (PARAMS=...) */
    char **config_argv = NULL;
    int config_argc = 0;
    int config_found = read_config_params(&config_argv, &config_argc);

    /* Build merged argv: [argv[0]] + config_params + cli_args[1..]
     * CLI arguments override config (parsed after, so last wins) */
    if (config_found > 0 && config_argc > 0) {
        int new_argc = 1 + config_argc + (argc - 1);
        char **new_argv = malloc(new_argc * sizeof(char *));
        if (new_argv) {
            new_argv[0] = argv[0];
            for (int i = 0; i < config_argc; i++)
                new_argv[1 + i] = config_argv[i];
            for (int i = 1; i < argc; i++)
                new_argv[1 + config_argc + (i - 1)] = argv[i];
            argc = new_argc;
            argv = new_argv;
        }
    }

    /* Parse command-line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            safe_copy(e.name, argv[++i], sizeof(e.name));
        } else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            safe_copy(e.addr, argv[++i], sizeof(e.addr));
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            e.interval = atoi(argv[++i]);
            e.interval_explicit = 1;
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            safe_copy(e.json_path, argv[++i], sizeof(e.json_path));
        } else if (strcmp(argv[i], "--driver") == 0 && i + 1 < argc) {
            safe_copy(e.driver_name, argv[++i], sizeof(e.driver_name));
        } else if (strcmp(argv[i], "--rt") == 0 && i + 1 < argc) {
            e.read_timeout = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--wt") == 0 && i + 1 < argc) {
            e.write_timeout = atoi(argv[++i]);
        } else {
            logError("unknown option: %s", argv[i]);
            return 1;
        }
    }

    /* Validate timeouts */
    if (e.read_timeout < MIN_READ_TO || e.read_timeout > MAX_READ_TO) {
        logError("--rt must be between %d and %d (got %d)",
                 MIN_READ_TO, MAX_READ_TO, e.read_timeout);
        return 1;
    }
    if (e.write_timeout < MIN_WRITE_TO || e.write_timeout > MAX_WRITE_TO) {
        logError("--wt must be between %d and %d (got %d)",
                 MIN_WRITE_TO, MAX_WRITE_TO, e.write_timeout);
        return 1;
    }
    if (e.write_timeout < e.read_timeout) {
        logError("--wt (%d) must be >= --rt (%d)", e.write_timeout, e.read_timeout);
        return 1;
    }

    /* Sanitize name */
    sanitize_name(e.name, sizeof(e.name));
    safe_copy(g_exporter_name, e.name, sizeof(g_exporter_name));

    /* Attempt to load driver if needed */
    ensure_driver(e.driver_name);

    /* Print startup banner */
    logInfo("DHT Exporter (v%s) starting...", VERSION);
    logInfo("  name:      %s", e.name);
    logInfo("  address:   %s", e.addr);
    /* Resolve interval before logging */
    if (e.interval_explicit) {
        if (e.interval < MIN_INTERVAL || e.interval > MAX_INTERVAL) {
            logWarn("interval %d is outside valid range %d-%d, falling back to default %d",
                    e.interval, MIN_INTERVAL, MAX_INTERVAL, DEFAULT_INTERVAL);
            e.interval = DEFAULT_INTERVAL;
            e.interval_explicit = 0;
        }
    }
    if (!e.interval_explicit) {
        int driver_iv = read_auto_interval();
        if (driver_iv >= MIN_INTERVAL && driver_iv <= MAX_INTERVAL)
            e.interval = driver_iv;
        else
            e.interval = DEFAULT_INTERVAL;
    }
    logInfo("  interval:  %d seconds (%s)", e.interval,
            e.interval_explicit ? "explicit" : "from driver");
    logInfo("  driver:    %s", e.driver_name);
    if (e.json_path[0])
        logInfo("  json:      %s", e.json_path);
    else
        logInfo("  json:      disabled (no --json specified)");
    logInfo("  timeouts:  read=%ds, write=%ds", e.read_timeout, e.write_timeout);
    logInfo("  procfs:    %s", PROC_BASE_DIR);
    logInfo("  config:    %s (%s)", CONFIG_FILE,
            config_found >= 0 ? "found" : "not found");

    if (e.json_path[0])
        validate_json_path(e.json_path);

    logInfo("DHT Exporter ready");

    /* Create self-pipe for signal → poll wakeup */
    if (pipe(g_sigpipe) < 0) {
        logError("cannot create pipe: %s", strerror(errno));
        return 1;
    }
    /* Non-blocking write end (don't block in signal handler) */
    int flags = fcntl(g_sigpipe[1], F_GETFL, 0);
    fcntl(g_sigpipe[1], F_SETFL, flags | O_NONBLOCK);
    /* Close-on-exec so modprobe child doesn't inherit */
    fcntl(g_sigpipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_sigpipe[1], F_SETFD, FD_CLOEXEC);

    /* Create listening socket */
    g_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listen_fd < 0) {
        logError("cannot create socket: %s", strerror(errno));
        return 1;
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Parse address */
    char addr_copy[64];
    safe_copy(addr_copy, e.addr, sizeof(addr_copy));
    char *colon = strrchr(addr_copy, ':');
    char *host_str = "0.0.0.0";
    int port = 9988;
    if (colon) {
        *colon = '\0';
        if (colon != addr_copy)
            host_str = addr_copy;
        port = atoi(colon + 1);
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    if (inet_pton(AF_INET, host_str, &saddr.sin_addr) != 1) {
        logError("invalid address: %s", e.addr);
        close(g_listen_fd);
        return 1;
    }

    if (bind(g_listen_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        logError("cannot bind %s: %s", e.addr, strerror(errno));
        close(g_listen_fd);
        return 1;
    }

    if (listen(g_listen_fd, BACKLOG) < 0) {
        logError("cannot listen: %s", strerror(errno));
        close(g_listen_fd);
        return 1;
    }

    logInfo("HTTP server listening on %s", e.addr);

    /* Block SIGINT/SIGTERM in all threads */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    /* Also ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Initialize shared data */
    memset(&g_data, 0, sizeof(g_data));

    /* Start threads */
    pthread_t poll_tid, http_tid;

    if (pthread_create(&poll_tid, NULL, poll_thread, &e) != 0) {
        logError("cannot create poll thread: %s", strerror(errno));
        close(g_listen_fd);
        return 1;
    }

    if (pthread_create(&http_tid, NULL, http_thread, &e) != 0) {
        logError("cannot create HTTP thread: %s", strerror(errno));
        g_stop = 1;
        write(g_sigpipe[1], "x", 1);
        pthread_join(poll_tid, NULL);
        close(g_listen_fd);
        return 1;
    }

    /* Wait for signal in main thread */
    int sig;
    sigwait(&mask, &sig);
    logInfo("received signal %d, shutting down...", sig);

    /* Signal threads to stop */
    g_stop = 1;
    write(g_sigpipe[1], "x", 1);          /* wake poll_thread */
    shutdown(g_listen_fd, SHUT_RDWR);     /* wake http_thread */

    /* Wait for threads to finish */
    pthread_join(poll_tid, NULL);
    pthread_join(http_tid, NULL);

    /* Cleanup */
    close(g_listen_fd);
    close(g_sigpipe[0]);
    close(g_sigpipe[1]);
    pthread_mutex_destroy(&g_mutex);

    logInfo("shutdown complete");
    return 0;
}
