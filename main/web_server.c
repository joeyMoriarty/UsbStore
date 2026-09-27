#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

#include "usb_storage.h"
#include "wifi_mgr.h"
#include "web_server.h"
#include "netlog.h"
#include "auth.h"
#include "ota.h"
#include "thermal.h"
#include "sysdrive.h"
#include "climate.h"
#include "planner.h"
#include "devlink.h"
#include "news.h"

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* One shared transfer buffer. The HTTP server runs a single request task, so
 * requests are serialised and this cannot be entered twice. Sized well above
 * the ~540 KB/s the bus can actually deliver, so USB stays the limit. */
#define XFER_SZ 8192
static char *s_buf;

/* ---------------------------------------------------------------- json out */
/*
 * Hand-rolled rather than cJSON: ESP-IDF 6.0 removed the bundled `json`
 * component, and every response here is either a handful of flat fields or a
 * directory listing that is better streamed than assembled in RAM anyway.
 */

static void json_esc(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                o += snprintf(out + o, n - o, "\\u%04x", c);
            } else {
                out[o++] = (char)c;
            }
        }
    }
    out[o] = '\0';
}

/*
 * Pull one string field out of a flat JSON object.
 *
 * Deliberately not a general parser - the only input it ever sees is the
 * two-field object the setup page posts. Anything nested is ignored.
 */
static bool json_get_str(const char *json, const char *key, char *out, size_t n)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < n) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            default:  out[o++] = *p;   break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return true;
}

/* --------------------------------------------------------------- helpers */

static esp_err_t fail(httpd_req_t *r, int code, const char *msg)
{
    const char *status = "400 Bad Request";
    if (code == 404)      status = "404 Not Found";
    else if (code == 403) status = "403 Forbidden";
    else if (code == 409) status = "409 Conflict";
    else if (code == 413) status = "413 Payload Too Large";
    else if (code == 428) status = "428 Precondition Required";
    else if (code == 500) status = "500 Internal Server Error";
    else if (code == 503) status = "503 Service Unavailable";

    char esc[160];
    json_esc(esc, sizeof(esc), msg);

    char body[224];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", esc);

    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_send(r, body, n);
    return ESP_OK;   /* handled: don't let httpd treat it as a socket error */
}

/* Pull one number field out of a flat JSON object - same deliberate limits
 * as json_get_str(). */
static bool json_get_num(const char *json, const char *key, double *out)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) {
        return false;
    }
    char *end;
    double v = strtod(p + 1, &end);
    if (end == p + 1) {
        return false;
    }
    *out = v;
    return true;
}

static esp_err_t ok_json(httpd_req_t *r)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Percent-decode in place.
 *
 * httpd_query_key_value() hands back the value still encoded: the browser
 * sends encodeURIComponent("/usb0/a b") as "%2Fusb0%2Fa%20b", and without
 * this every path fails usbstore_path_ok() for not starting with '/'.
 *
 * Decoding happens BEFORE validation on purpose, so the check sees the real
 * path - "%2E%2E" arrives there as ".." and is rejected. '+' is left alone:
 * encodeURIComponent never emits it raw, and turning it into a space would
 * corrupt filenames that genuinely contain '+'.
 */
static bool url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r != '%') {
            *w++ = *r;
            continue;
        }
        int hi = hexval(r[1]);
        int lo = hi < 0 ? -1 : hexval(r[2]);   /* never reads past a NUL */
        if (lo < 0) {
            return false;
        }
        char c = (char)((hi << 4) | lo);
        if (c == '\0') {
            return false;   /* an embedded NUL would silently truncate the path */
        }
        *w++ = c;
        r += 2;
    }
    *w = '\0';
    return true;
}

/* Pull one query parameter and percent-decode it. */
static bool get_param(httpd_req_t *r, const char *key, char *out, size_t len)
{
    size_t qlen = httpd_req_get_url_query_len(r) + 1;
    if (qlen <= 1 || qlen > 1024) {
        return false;
    }
    char *q = malloc(qlen);
    if (!q) {
        return false;
    }
    bool ok = false;
    if (httpd_req_get_url_query_str(r, q, qlen) == ESP_OK) {
        ok = httpd_query_key_value(q, key, out, len) == ESP_OK && url_decode(out);
    }
    free(q);
    return ok;
}

static const char *mime_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".log")) return "text/plain";
    if (!strcasecmp(dot, ".htm") || !strcasecmp(dot, ".html")) return "text/html";
    if (!strcasecmp(dot, ".css"))  return "text/css";
    if (!strcasecmp(dot, ".js"))   return "text/javascript";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".gif"))  return "image/gif";
    if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, ".pdf"))  return "application/pdf";
    if (!strcasecmp(dot, ".mp3"))  return "audio/mpeg";
    if (!strcasecmp(dot, ".mp4"))  return "video/mp4";
    if (!strcasecmp(dot, ".zip"))  return "application/zip";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

/*
 * Take a ref on the drive holding `path`, swapping it in if it was parked.
 * On failure the error response has already been sent and no ref is held;
 * on success the caller must usbstore_release(*ref) on every path out.
 */
static bool open_drive(httpd_req_t *r, const char *path, usbstore_ref_t *ref)
{
    char why[112];
    if (usbstore_acquire(path, why, sizeof(why), ref) != ESP_OK) {
        fail(r, 503, why);
        return false;
    }
    return true;
}

/* Close the connection after this response. For an upload refused before its
 * body was read, that beats letting httpd drain a possibly multi-GB body; for
 * a download cut short, it tells the browser the file is incomplete instead
 * of leaving it waiting on a chunked response that will never end. */
static void drop_connection(httpd_req_t *r)
{
    httpd_sess_trigger_close(r->handle, httpd_req_to_sockfd(r));
}

/* ----------------------------------------------------- transfer workers */
/*
 * esp_http_server runs every handler on ONE task. A download used to stream
 * inside that task until its last byte, so for minutes nothing else - status,
 * log, temperature, the page itself - got an answer (the board still replied
 * to ping: only the web task was stuck). Long transfers are now handed to
 * worker tasks with httpd_req_async_handler_begin(), and the web task goes
 * straight back to serving everything else.
 *
 * Two workers = two transfers at once; each has its own buffer, because the
 * shared s_buf is only safe on the web task. Pinned to core 1 with the USB
 * tasks, leaving core 0 to WiFi.
 */
#define XFER_WORKERS 2
#define XFER_QUEUE   2          /* further transfers wait here, then get 503 */

typedef void (*xfer_fn_t)(httpd_req_t *r, char *buf);
typedef struct {
    httpd_req_t *req;
    xfer_fn_t    fn;
    bool         cpu;           /* compute-bound (charts, news...), not a transfer */
} xfer_job_t;

static QueueHandle_t s_jobs;
static volatile int  s_busy;    /* workers mid-job */

static void xfer_worker(void *arg)
{
    char *buf = heap_caps_malloc(XFER_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(XFER_SZ);
    }
    xfer_job_t job;
    while (1) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /*
         * Burst the CPU for compute-bound jobs, or when every worker is busy
         * and more wait - overwhelmed. A lone download isn't worth it: the
         * USB bus caps it at ~520 KB/s, so 240 MHz would only add heat.
         */
        int busy = __atomic_add_fetch(&s_busy, 1, __ATOMIC_RELAXED);
        bool boost = job.cpu || (busy == XFER_WORKERS && uxQueueMessagesWaiting(s_jobs) > 0);
        if (boost) {
            thermal_boost_begin();
        }
        if (buf) {
            job.fn(job.req, buf);
        } else {
            fail(job.req, 500, "out of memory");
        }
        if (boost) {
            thermal_boost_end();
        }
        __atomic_sub_fetch(&s_busy, 1, __ATOMIC_RELAXED);
        httpd_req_async_handler_complete(job.req);
    }
}

static esp_err_t hand_off_job(httpd_req_t *r, xfer_fn_t fn, bool cpu)
{
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(r, &copy) != ESP_OK) {
        return fail(r, 500, "out of memory");
    }
    const xfer_job_t job = { .req = copy, .fn = fn, .cpu = cpu };
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        fail(copy, 503, "busy: too many transfers at once - try again shortly");
        drop_connection(copy);
        httpd_req_async_handler_complete(copy);
    }
    return ESP_OK;
}

/* Downloads and uploads: bus-bound, so no CPU burst of their own. */
static esp_err_t hand_off(httpd_req_t *r, xfer_fn_t fn)
{
    return hand_off_job(r, fn, false);
}

/* ---------------------------------------------------------------- routes */

/* Browsers ask for this on every load; answer quietly instead of logging a
 * 404 warning each time. */
static esp_err_t h_favicon(httpd_req_t *r)
{
    httpd_resp_set_status(r, "204 No Content");
    httpd_resp_send(r, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_index(httpd_req_t *r)
{
    return web_send_page(r, index_html_start, index_html_end);
}

static esp_err_t h_status(httpd_req_t *r)
{
    char wifi[96];
    wifi_mgr_status(wifi, sizeof(wifi));
    char wifi_esc[128];
    json_esc(wifi_esc, sizeof(wifi_esc), wifi);

    usbstore_drive_t d[USBSTORE_MAX_DRIVES];
    int n = usbstore_list(d, USBSTORE_MAX_DRIVES);

    char ver[48];
    json_esc(ver, sizeof(ver), ota_running_version());

    /* NAN (no reading yet, or no sensor) goes out as JSON null. */
    char t_now[12], t_peak[12], cool[160] = "null";
    float tn = thermal_celsius(), tp = thermal_peak_celsius();
    if (isnan(tn)) snprintf(t_now, sizeof(t_now), "null");
    else           snprintf(t_now, sizeof(t_now), "%.1f", tn);
    if (isnan(tp)) snprintf(t_peak, sizeof(t_peak), "null");
    else           snprintf(t_peak, sizeof(t_peak), "%.1f", tp);
    char sys[40], sys_esc[88];
    sysdrive_serial(sys, sizeof(sys));
    json_esc(sys_esc, sizeof(sys_esc), sys);

    char room[80] = "null";
    float rt, rh;
    uint32_t rage;
    if (climate_latest(&rt, &rh, &rage)) {
        snprintf(room, sizeof(room), "{\"t\":%.1f,\"h\":%.1f,\"age_s\":%lu}",
                 rt, rh, (unsigned long)rage);
    }

    char link[200];
    devlink_status_json(link, sizeof(link));

    therm_cooldown_t cd = thermal_last_cooldown();
    if (cd.valid) {
        snprintf(cool, sizeof(cool),
                 "{\"test\":%s,\"peak_c\":%.1f,\"end_c\":%.1f,\"slept_s\":%lu}",
                 cd.was_test ? "true" : "false", cd.peak_c, cd.end_c,
                 (unsigned long)cd.slept_s);
    }

    int o = snprintf(s_buf, XFER_SZ,
                     "{\"wifi\":\"%s\",\"provisioned\":%s,\"auth\":%s,"
                     "\"version\":\"%s\",\"heap\":%u,\"uptime_s\":%lld,"
                     "\"temp_c\":%s,\"temp_peak_c\":%s,\"thermal\":\"%s\","
                     "\"therm_warm_c\":%.0f,\"therm_hot_c\":%.0f,\"cooldown\":%s,"
                     "\"cpu_mhz\":%d,\"bursts\":%lu,"
                     "\"devkey\":%s,\"sys_serial\":\"%s\",\"room\":%s,\"link\":%s,"
                     "\"drives\":[",
                     wifi_esc,
                     wifi_mgr_is_station() ? "true" : "false",
                     auth_is_set() ? "true" : "false",
                     ver,
                     (unsigned)esp_get_free_heap_size(),
                     (long long)(esp_timer_get_time() / 1000000),
                     t_now, t_peak, thermal_state_name(),
                     THERM_WARM_C, THERM_HOT_C, cool,
                     thermal_cpu_mhz(), (unsigned long)thermal_bursts(),
                     auth_device_key_is_set() ? "true" : "false", sys_esc, room, link);

    /* 512 of headroom: one entry with an escaped name and note is ~350
     * bytes, and a truncated snprintf would push `o` past the buffer. */
    for (int i = 0; i < n && o < XFER_SZ - 512; i++) {
        char name[96], note[112];
        json_esc(name, sizeof(name), d[i].product);
        json_esc(note, sizeof(note), d[i].note);
        char serial[88];
        json_esc(serial, sizeof(serial), d[i].serial);
        bool is_sys = sys[0] && !strcmp(sys, d[i].serial);
        o += snprintf(s_buf + o, XFER_SZ - o,
                      "%s{\"path\":\"%s\",\"name\":\"%s\",\"bytes\":%llu,"
                      "\"state\":\"%s\",\"note\":\"%s\",\"serial\":\"%s\",\"system\":%s}",
                      i ? "," : "", d[i].base, name,
                      (unsigned long long)d[i].capacity, d[i].state, note,
                      serial, is_sys ? "true" : "false");
    }
    /* Everything on the bus, hub included: the diagnostic view. */
    usbstore_usbdev_t u[USBSTORE_MAX_BUS];
    int nu = usbstore_census(u, USBSTORE_MAX_BUS);
    o += snprintf(s_buf + o, XFER_SZ - o, "],\"usb\":[");
    for (int i = 0; i < nu && o < XFER_SZ - 128; i++) {
        o += snprintf(s_buf + o, XFER_SZ - o,
                      "%s{\"addr\":%u,\"id\":\"%04x:%04x\",\"kind\":\"%s\",\"speed\":\"%s\"}",
                      i ? "," : "", u[i].addr, u[i].vid, u[i].pid, u[i].kind, u[i].speed);
    }
    o += snprintf(s_buf + o, XFER_SZ - o, "]}");

    httpd_resp_set_type(r, "application/json");
    httpd_resp_send(r, s_buf, o);
    return ESP_OK;
}

/*
 * Streamed, not assembled: a directory with a few thousand files would not fit
 * in any fixed buffer, and chunking means memory use is independent of it.
 */
static esp_err_t h_list(httpd_req_t *r)
{
    char path[512];
    if (!get_param(r, "p", path, sizeof(path))) {
        return fail(r, 400, "missing p");
    }
    if (!usbstore_path_ok(path)) {
        return fail(r, 403, "path outside a mounted drive");
    }
    /* The planner's private files live in a usbstore folder; browsing it
     * must not be a way round the planner's password. */
    if (sysdrive_is_protected(path) && !auth_check(r)) {
        return ESP_OK;
    }
    usbstore_ref_t ref;
    if (!open_drive(r, path, &ref)) {
        return ESP_OK;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        usbstore_release(ref);
        return fail(r, 404, "no such directory");
    }

    char esc[600];
    json_esc(esc, sizeof(esc), path);

    httpd_resp_set_type(r, "application/json");
    int o = snprintf(s_buf, XFER_SZ, "{\"path\":\"%s\",\"entries\":[", esc);

    bool first = true;
    struct dirent *e;
    esp_err_t err = ESP_OK;

    while ((e = readdir(dir)) != NULL) {
        /* 512 path + '/' + 255 name + NUL: sized so no entry is ever cut. */
        char full[800];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);

        struct stat st;
        bool  isdir = (e->d_type == DT_DIR);
        long long sz = 0;
        if (!isdir && stat(full, &st) == 0) {
            sz = (long long)st.st_size;
        }

        json_esc(esc, sizeof(esc), e->d_name);

        /* Flush before an entry that might not fit. */
        if (o > XFER_SZ - 900) {
            if (httpd_resp_send_chunk(r, s_buf, o) != ESP_OK) {
                err = ESP_FAIL;
                break;
            }
            o = 0;
        }
        o += snprintf(s_buf + o, XFER_SZ - o,
                      "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lld}",
                      first ? "" : ",", esc, isdir ? "true" : "false", sz);
        first = false;
    }
    closedir(dir);
    usbstore_release(ref);

    if (err != ESP_OK) {
        return err;
    }

    o += snprintf(s_buf + o, XFER_SZ - o, "]}");
    if (httpd_resp_send_chunk(r, s_buf, o) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

/* Runs on a transfer worker, streaming through that worker's own buffer. */
static void do_download(httpd_req_t *r, char *buf)
{
    char path[512];
    if (!get_param(r, "p", path, sizeof(path))) {
        fail(r, 400, "missing p");
        return;
    }
    if (!usbstore_path_ok(path)) {
        fail(r, 403, "path outside a mounted drive");
        return;
    }
    usbstore_ref_t ref;
    if (!open_drive(r, path, &ref)) {
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        usbstore_release(ref);
        fail(r, 404, "no such file");
        return;
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    /* filename* (RFC 5987) carries the real UTF-8 name, so non-English names
     * survive the download; the plain filename= is the fallback. FAT forbids
     * '"' in names, so it cannot break out of the quoted string. */
    char enc[1024];
    size_t eo = 0;
    for (const unsigned char *c = (const unsigned char *)base; *c && eo + 4 < sizeof(enc); c++) {
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
            (*c >= '0' && *c <= '9') || strchr("-._~", *c)) {
            enc[eo++] = (char)*c;
        } else {
            eo += snprintf(enc + eo, sizeof(enc) - eo, "%%%02X", *c);
        }
    }
    enc[eo] = '\0';

    /* Must outlive the response: httpd keeps the pointer, not a copy. */
    char disp[1664];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"; filename*=UTF-8''%s",
             base, enc);
    httpd_resp_set_type(r, mime_for(base));
    httpd_resp_set_hdr(r, "Content-Disposition", disp);

    bool ok = true;
    size_t got;
    while ((got = fread(buf, 1, XFER_SZ, f)) > 0) {
        /* An overheat cool-down needs this drive unmounted; stop within a
         * chunk rather than make it wait out a long download. */
        if (thermal_cooling()) {
            ok = false;
            break;
        }
        if (httpd_resp_send_chunk(r, buf, got) != ESP_OK) {
            /* Browser cancelled, or the drive vanished mid-read. */
            ok = false;
            break;
        }
    }
    fclose(f);
    usbstore_release(ref);

    if (ok) {
        httpd_resp_send_chunk(r, NULL, 0);
    } else {
        drop_connection(r);
    }
}

static esp_err_t h_download(httpd_req_t *r)
{
    /* Checked here, on the web task, so the browser's login prompt comes from
     * a plain request before any worker is tied up. */
    char path[512];
    if (get_param(r, "p", path, sizeof(path)) && sysdrive_is_protected(path) &&
        !auth_check(r)) {
        return ESP_OK;
    }
    return hand_off(r, do_download);
}

/* Runs on a transfer worker. Every refusal before the body is read also
 * drops the connection, rather than letting httpd drain the whole body. */
static void do_upload(httpd_req_t *r, char *buf)
{
    char path[512];
    if (!get_param(r, "p", path, sizeof(path))) {
        fail(r, 400, "missing p");
        drop_connection(r);
        return;
    }
    if (!usbstore_path_ok(path)) {
        fail(r, 403, "path outside a mounted drive");
        drop_connection(r);
        return;
    }
    if (sysdrive_is_protected(path)) {
        fail(r, 403, "that folder is managed by UsbStore - use the Climate or Planner pages");
        drop_connection(r);
        return;
    }
    usbstore_ref_t ref;
    if (!open_drive(r, path, &ref)) {
        drop_connection(r);
        return;
    }

    /* Raw body, not multipart: the browser sends the bytes as-is so the
     * firmware never has to pull MIME boundaries out of a stream. */
    FILE *f = fopen(path, "wb");
    if (!f) {
        usbstore_release(ref);
        fail(r, 500, "cannot create file");
        drop_connection(r);
        return;
    }

    int  left = r->content_len;
    bool ok   = true;
    while (left > 0) {
        if (thermal_cooling()) {
            ok = false;          /* partial file is deleted below, as for any failure */
            break;
        }
        int want = left < XFER_SZ ? left : XFER_SZ;
        int got  = httpd_req_recv(r, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, got, f) != (size_t)got) {
            ok = false;   /* out of space, or the drive was pulled */
            break;
        }
        left -= got;
    }
    fclose(f);

    if (!ok) {
        /* A half-written file is worse than none: you would not know to retry. */
        unlink(path);
    }
    usbstore_release(ref);

    if (!ok) {
        fail(r, 500, "write failed - drive full, removed, or cooling down");
        drop_connection(r);
        return;
    }
    ok_json(r);
}

static esp_err_t h_upload(httpd_req_t *r)
{
    /* Checked here on the web task, before any hand-off: ESP_FAIL closes the
     * socket instead of draining a body we will never use. */
    if (!auth_check(r)) {
        return ESP_FAIL;
    }
    return hand_off(r, do_upload);
}

static esp_err_t h_delete(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }

    char path[512];
    if (!get_param(r, "p", path, sizeof(path))) {
        return fail(r, 400, "missing p");
    }
    if (!usbstore_path_ok(path)) {
        return fail(r, 403, "path outside a mounted drive");
    }
    if (sysdrive_is_protected(path)) {
        return fail(r, 403, "that folder is managed by UsbStore - use the Climate or Planner pages");
    }
    usbstore_ref_t ref;
    if (!open_drive(r, path, &ref)) {
        return ESP_OK;
    }

    struct stat st;
    int rc = -1;
    if (stat(path, &st) == 0) {
        /* rmdir only removes empty directories: no recursive delete is
         * exposed over the network, deliberately. */
        rc = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    }
    usbstore_release(ref);

    if (rc != 0) {
        return fail(r, 400, "delete failed (directory not empty?)");
    }
    return ok_json(r);
}

static esp_err_t h_mkdir(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }

    char path[512];
    if (!get_param(r, "p", path, sizeof(path))) {
        return fail(r, 400, "missing p");
    }
    if (!usbstore_path_ok(path)) {
        return fail(r, 403, "path outside a mounted drive");
    }
    if (sysdrive_is_protected(path)) {
        return fail(r, 403, "that folder is managed by UsbStore");
    }
    usbstore_ref_t ref;
    if (!open_drive(r, path, &ref)) {
        return ESP_OK;
    }
    int rc = mkdir(path, 0775);
    usbstore_release(ref);

    if (rc != 0) {
        return fail(r, 400, "mkdir failed");
    }
    return ok_json(r);
}

/*
 * Log tail. GET /api/log?since=N returns everything newer than cursor N as
 * plain text, with the new cursor in X-Log-Cursor so the UI can poll without
 * re-reading what it already has.
 */
static esp_err_t h_log(httpd_req_t *r)
{
    char arg[24];
    uint64_t since = 0;
    if (get_param(r, "since", arg, sizeof(arg))) {
        since = strtoull(arg, NULL, 10);
    }

    uint64_t next = since;
    size_t   n    = netlog_read(since, s_buf, XFER_SZ, &next);

    char cur[24];
    snprintf(cur, sizeof(cur), "%llu", (unsigned long long)next);
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "X-Log-Cursor", cur);
    httpd_resp_send(r, s_buf, n);
    return ESP_OK;
}

/*
 * Panic report from the previous boot.
 *
 * The only way a crash becomes visible with no serial adapter: the network
 * stack is already dead when a panic handler runs, so nothing reaches
 * /api/log. The dump goes to flash instead and is read back here.
 *
 * Resolve the addresses with:
 *   xtensa-esp32s3-elf-addr2line -pfiaC -e build/usbstore.elf <addr> ...
 */
static esp_err_t h_crash(httpd_req_t *r)
{
    int o = snprintf(s_buf, XFER_SZ, "{\"reset_reason\":%d",
                     (int)esp_reset_reason());

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_core_dump_image_check() != ESP_OK) {
        o += snprintf(s_buf + o, XFER_SZ - o, ",\"crash\":false}");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_send(r, s_buf, o);
        return ESP_OK;
    }

    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (!sum) {
        return fail(r, 500, "out of memory");
    }
    if (esp_core_dump_get_summary(sum) == ESP_OK) {
        char task[40];
        json_esc(task, sizeof(task), sum->exc_task);
        o += snprintf(s_buf + o, XFER_SZ - o,
                      ",\"crash\":true,\"task\":\"%s\",\"pc\":\"0x%08x\","
                      "\"bt_corrupted\":%s,\"backtrace\":[",
                      task, (unsigned)sum->exc_pc,
                      sum->exc_bt_info.corrupted ? "true" : "false");
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && o < XFER_SZ - 40; i++) {
            o += snprintf(s_buf + o, XFER_SZ - o, "%s\"0x%08x\"",
                          i ? "," : "", (unsigned)sum->exc_bt_info.bt[i]);
        }
        o += snprintf(s_buf + o, XFER_SZ - o, "]}");
    } else {
        o += snprintf(s_buf + o, XFER_SZ - o,
                      ",\"crash\":false,\"note\":\"dump present but unreadable\"}");
    }
    free(sum);
#else
    o += snprintf(s_buf + o, XFER_SZ - o,
                  ",\"crash\":false,\"note\":\"coredump not enabled\"}");
#endif

    httpd_resp_set_type(r, "application/json");
    httpd_resp_send(r, s_buf, o);
    return ESP_OK;
}

/* Clear the stored dump so the next crash is unambiguous. */
static esp_err_t h_crash_clear(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_core_dump_image_erase();
#endif
    return ok_json(r);
}

/* Read a small JSON body into `body`. Returns false on empty/oversized. */
static bool read_small_body(httpd_req_t *r, char *body, size_t cap)
{
    if (r->content_len <= 0 || (size_t)r->content_len >= cap) {
        return false;
    }
    int got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, body + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    body[got] = '\0';
    return true;
}

/*
 * A no-op behind the password. The UI calls this before a big upload or an
 * OTA so the browser's login prompt appears up front: once answered, the
 * browser attaches the credentials to every later /api/ request, and a large
 * body is never streamed only to be refused at the end.
 */
static esp_err_t h_auth_probe(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }
    return ok_json(r);
}

/*
 * Run the overheat cool-down now, for 30 s, regardless of temperature - the
 * only practical way to prove the sleep-and-reboot path works on real
 * hardware. Protected: it takes the box offline for about a minute.
 */
static esp_err_t h_thermal_test(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true,\"offline_s\":45}");
    vTaskDelay(pdMS_TO_TICKS(300));      /* let the reply leave first */
    thermal_request_test();
    return ESP_OK;
}

/* Set or change the admin password. auth_set() checks the current one. */
/* Set the device key other gadgets (Desk-Disp) use. Admin only; the key is
 * never sent back out - the device that needs it gets it from its own
 * firmware config. */
static esp_err_t h_devicekey(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }
    char body[160], key[72] = {0};
    if (!read_small_body(r, body, sizeof(body)) ||
        !json_get_str(body, "key", key, sizeof(key))) {
        return fail(r, 400, "missing key");
    }
    esp_err_t err = auth_device_key_set(key);
    if (err == ESP_ERR_INVALID_ARG) {
        return fail(r, 400, "device key must be 16-64 characters");
    }
    return err == ESP_OK ? ok_json(r) : fail(r, 500, "could not save");
}

static esp_err_t h_passwd(httpd_req_t *r)
{
    char body[256];
    if (!read_small_body(r, body, sizeof(body))) {
        return fail(r, 400, "bad body");
    }
    char cur[64] = {0}, next[64] = {0};
    json_get_str(body, "current", cur, sizeof(cur));
    if (!json_get_str(body, "next", next, sizeof(next))) {
        return fail(r, 400, "missing next");
    }

    esp_err_t err = auth_set(cur, next);
    if (err == ESP_ERR_INVALID_ARG) {
        return fail(r, 400, "password must be 4-63 characters");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return fail(r, 403, "current password is wrong");
    }
    if (err != ESP_OK) {
        return fail(r, 500, "could not save password");
    }
    return ok_json(r);
}

/* Setup page target: store credentials, then reboot into station mode. */
static esp_err_t h_provision(httpd_req_t *r)
{
    /* Moving the box to another network is a write action like any other.
     * On first boot no password exists yet, so the setup AP stays open. */
    if (!auth_check(r)) {
        return ESP_OK;
    }

    char body[320] = {0};
    if (!read_small_body(r, body, sizeof(body))) {
        return fail(r, 400, "bad body");
    }

    char ssid[64] = {0}, pass[80] = {0}, admin[64] = {0};
    if (!json_get_str(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return fail(r, 400, "missing ssid");
    }
    json_get_str(body, "pass", pass, sizeof(pass));   /* open networks: ok */

    /* First-time setup can set the admin password in the same step, so the
     * box is never on the real network with its write API open. */
    if (json_get_str(body, "admin", admin, sizeof(admin)) && admin[0] &&
        !auth_is_set() && auth_set(NULL, admin) != ESP_OK) {
        return fail(r, 400, "admin password must be 4-63 characters");
    }

    if (wifi_mgr_provision(ssid, pass) != ESP_OK) {
        return fail(r, 500, "could not save credentials");
    }

    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true,\"rebooting\":true}");

    vTaskDelay(pdMS_TO_TICKS(600));   /* let the response flush */
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ start */

/* ----------------------------------------- exported for the page modules */

esp_err_t web_fail(httpd_req_t *r, int code, const char *msg) { return fail(r, code, msg); }
esp_err_t web_ok(httpd_req_t *r) { return ok_json(r); }
bool web_param(httpd_req_t *r, const char *key, char *out, size_t len)
{
    return get_param(r, key, out, len);
}
bool web_small_body(httpd_req_t *r, char *body, size_t cap)
{
    return read_small_body(r, body, cap);
}
void web_json_esc(char *out, size_t n, const char *in) { json_esc(out, n, in); }
bool web_json_str(const char *json, const char *key, char *out, size_t n)
{
    return json_get_str(json, key, out, n);
}
bool web_json_num(const char *json, const char *key, double *out)
{
    return json_get_num(json, key, out);
}
esp_err_t web_hand_off(httpd_req_t *r, web_job_fn fn) { return hand_off_job(r, fn, true); }
void web_drop_connection(httpd_req_t *r) { drop_connection(r); }

esp_err_t web_send_page(httpd_req_t *r, const uint8_t *start, const uint8_t *end)
{
    /* The pages change with every firmware update; a cached copy of an old
     * page against a new API is a broken page. no-cache = always revalidate. */
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    httpd_resp_send(r, (const char *)start, end - start - 1);
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    /* PSRAM is fine for a streaming buffer and keeps internal RAM for WiFi. */
    s_buf = heap_caps_malloc(XFER_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        s_buf = malloc(XFER_SZ);
    }
    if (!s_buf) {
        return ESP_ERR_NO_MEM;
    }

    s_jobs = xQueueCreate(XFER_QUEUE, sizeof(xfer_job_t));
    if (!s_jobs) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < XFER_WORKERS; i++) {
        /* 6 KB: do_download's filename encoding buffers live on this stack. */
        if (xTaskCreatePinnedToCore(xfer_worker, "xfer", 6144, NULL, 5, NULL, 1) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 32;
    cfg.stack_size        = 8192;
    cfg.lru_purge_enable  = true;
    /* Transfers now overlap with page traffic, so allow more connections.
     * Budget: 10 + httpd's 3 internal + the UDP log socket = 14 of the 16
     * lwIP sockets (CONFIG_LWIP_MAX_SOCKETS). */
    cfg.max_open_sockets  = 10;
    /* Generous: a 100 MB upload over a ~350 KB/s bus takes minutes, and the
     * socket must not be reaped underneath it. */
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = h_index       },
        { .uri = "/system",       .method = HTTP_GET,  .handler = h_index       },
        { .uri = "/favicon.ico",  .method = HTTP_GET,  .handler = h_favicon     },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = h_status      },
        { .uri = "/api/list",     .method = HTTP_GET,  .handler = h_list        },
        { .uri = "/api/dl",       .method = HTTP_GET,  .handler = h_download    },
        { .uri = "/api/ul",       .method = HTTP_POST, .handler = h_upload      },
        { .uri = "/api/rm",       .method = HTTP_POST, .handler = h_delete      },
        { .uri = "/api/mkdir",    .method = HTTP_POST, .handler = h_mkdir       },
        { .uri = "/api/wifi",     .method = HTTP_POST, .handler = h_provision   },
        { .uri = "/api/log",      .method = HTTP_GET,  .handler = h_log         },
        { .uri = "/api/crash",    .method = HTTP_GET,  .handler = h_crash       },
        { .uri = "/api/crashclr", .method = HTTP_POST, .handler = h_crash_clear },
        { .uri = "/api/auth",     .method = HTTP_POST, .handler = h_auth_probe  },
        { .uri = "/api/passwd",   .method = HTTP_POST, .handler = h_passwd      },
        { .uri = "/api/ota",      .method = HTTP_POST, .handler = ota_http_handler },
        { .uri = "/api/thermaltest", .method = HTTP_POST, .handler = h_thermal_test },
        { .uri = "/api/devicekey", .method = HTTP_POST, .handler = h_devicekey },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &routes[i]));
    }
    sysdrive_routes(srv);
    climate_routes(srv);
    planner_routes(srv);
    news_routes(srv);

    ESP_LOGI(TAG, "http server up on port 80");
    return ESP_OK;
}
