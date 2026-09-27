#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mbedtls/md.h"

#include "devlink.h"
#include "auth.h"
#include "climate.h"
#include "news.h"
#include "sysdrive.h"
#include "usb_storage.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "devlink";

/* ---- wire format: keep in step with Desk-Disp's include/UsbStoreLink.h ---- */

#define DL_MAGIC  0x5355          /* "US" */
#define DL_VER    3               /* 2: + sky, weather, forecast; 3: + bridge address */
#define DL_HELLO  1               /* Desk-Disp -> UsbStore, broadcast */
#define DL_TASKS  2               /* UsbStore -> Desk-Disp, unicast reply */
#define DL_TAG    16              /* HMAC-SHA256, truncated */
#define DL_ITEMS  4
#define DL_TITLE  39

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  type;
    uint32_t nonce;               /* HELLO: random per request; TASKS: echoed */
    uint32_t epoch;               /* sender's clock, 0 if not set */
} dl_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t at;                  /* when the bridge computed it */
    uint32_t ymd;                 /* local date of the rise/set times */
    char     place[12];           /* not terminated */
    int16_t  sun_alt, moon_alt;   /* 1/100 degree */
    uint16_t sun_az, moon_az;     /* 1/100 degree */
    uint16_t illum;               /* 1/100 % */
    uint16_t phase;               /* cycle * 10000: 0 new, 5000 full */
    uint32_t sunrise, sunset, moonrise, moonset;   /* epoch, 0 = not that day */
} dl_sky_t;

typedef struct __attribute__((packed)) {
    int16_t  t, feels;            /* 1/100 degC */
    uint16_t h;                   /* 1/100 % */
    uint8_t  code;                /* WMO weather code */
    uint8_t  cloud;               /* % */
    uint16_t wind;                /* 1/10 km/h */
    uint16_t wdir;                /* degrees */
    uint16_t rain;                /* 1/10 mm */
    uint16_t pres;                /* 1/10 hPa */
} dl_wx_t;

typedef struct __attribute__((packed)) {
    uint32_t ymd;
    uint8_t  code;
    int16_t  hi, lo;              /* 1/10 degC */
    uint8_t  pop;                 /* % */
} dl_fc_t;

typedef struct __attribute__((packed)) {
    dl_hdr_t h;
    int16_t  t_c;                 /* 1/100 degC */
    uint16_t h_c;                 /* 1/100 %RH */
    uint8_t  flags;               /* HF_* */
    uint8_t  nfc;                 /* forecast days that follow */
    dl_sky_t sky;
    dl_wx_t  wx;
    dl_fc_t  fc[3];
    uint8_t  bridge_ip[4];        /* Desk-Disp's PC bridge, a.b.c.d */
    uint16_t bridge_port;
    uint8_t  tag[DL_TAG];
} dl_hello_t;

#define HF_ROOM  0x01             /* t_c/h_c are a real reading */
#define HF_SKY   0x02             /* sky is fresh from the bridge */
#define HF_WX    0x04             /* wx too (the weather service answered) */
#define HF_BRIDGE 0x08            /* bridge_ip/port: where the news comes from */

typedef struct __attribute__((packed)) {
    uint32_t at;                  /* start, UTC epoch; local midnight if all-day */
    uint32_t ymd;                 /* local date as 20261003 */
    uint16_t mins;                /* local minutes past midnight; 0xFFFF all-day */
    uint8_t  tlen;
    char     title[DL_TITLE];     /* UTF-8, not terminated */
} dl_item_t;

typedef struct __attribute__((packed)) {
    dl_hdr_t  h;
    uint8_t   status;             /* ST_* - encrypted from here ... */
    uint8_t   count;
    dl_item_t items[DL_ITEMS];    /* ... to here */
    uint8_t   tag[DL_TAG];
} dl_tasks_t;

_Static_assert(sizeof(dl_sky_t) == 48 && sizeof(dl_wx_t) == 16 && sizeof(dl_fc_t) == 10,
               "sky/weather layout changed");
_Static_assert(sizeof(dl_hello_t) == 134, "HELLO layout changed");
_Static_assert(sizeof(dl_tasks_t) <= ESP_NOW_MAX_DATA_LEN, "TASKS too big for ESP-NOW v1");

#define ST_STORED   0x01          /* the reading was logged */
#define ST_CLOCK    0x02          /* UsbStore's clock is set */
#define ST_KNOWN    0x04          /* the task list is loaded (possibly empty) */
#define ST_REFUSED  0x08          /* reading out of range, or clocks disagree */

/* ------------------------------------------------------------------------ */

#define UP_MAX       32           /* the page writes at most 30 */
#define UP_FILE_MAX  (64 * 1024)
#define CLOCK_VALID  1735689600   /* 2025-01-01 */
#define SKEW_MAX_S   300
#define RETRY_S      30           /* a failed task-list load is retried this often */
#define SEEN         8            /* recent nonces, to drop replays */

typedef struct {
    uint32_t at, ymd;
    uint16_t mins;
    char     title[DL_TITLE + 1];
} up_t;

typedef struct {
    uint8_t    mac[6];
    dl_hello_t pkt;
} rx_t;

static QueueHandle_t     s_rx;
static SemaphoreHandle_t s_up_mux;
static up_t              s_up[UP_MAX];
static int               s_up_n;
static bool              s_up_known;
static char              s_up_serial[40];   /* the system drive it came from */
static int64_t           s_up_tried_us = -1;

static bool       s_on;
static uint8_t    s_peer[6];
static int64_t    s_last_us;
static uint32_t   s_hellos, s_bad;
static uint32_t   s_seen[SEEN];
static int        s_seen_i;
static uint32_t   s_last_nonce;
static dl_tasks_t s_last_reply;

/* ------------------------------------------------------------------ crypto */

/* SHA-256 over up to three pieces. */
static void sha256(const void *a, size_t alen, const void *b, size_t blen,
                   const void *c, size_t clen, uint8_t out[32])
{
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&md);
    mbedtls_md_update(&md, a, alen);
    if (blen) {
        mbedtls_md_update(&md, b, blen);
    }
    if (clen) {
        mbedtls_md_update(&md, c, clen);
    }
    mbedtls_md_finish(&md, out);
    mbedtls_md_free(&md);
}

/*
 * HMAC-SHA256 over a || b, spelled out (RFC 2104). mbedTLS 4 made its own
 * HMAC calls private; plain SHA-256 is still public, and HMAC is only two
 * hashes on top of it. Checked against RFC 4231 at start-up.
 */
static void hmac(const uint8_t *key, size_t klen, const void *a, size_t alen,
                 const void *b, size_t blen, uint8_t out[32])
{
    uint8_t k0[64] = {0}, pad[64], inner[32];
    if (klen > sizeof(k0)) {
        sha256(key, klen, NULL, 0, NULL, 0, k0);
    } else {
        memcpy(k0, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        pad[i] = k0[i] ^ 0x36;
    }
    sha256(pad, 64, a, alen, b, blen, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = k0[i] ^ 0x5c;
    }
    sha256(pad, 64, inner, 32, NULL, 0, out);
}

static bool hmac_selftest(void)
{
    static const uint8_t want[8] = { 0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e };
    static const char    msg[]   = "what do ya want for nothing?";
    uint8_t got[32];
    hmac((const uint8_t *)"Jefe", 4, msg, strlen(msg), NULL, 0, got);
    return memcmp(got, want, sizeof(want)) == 0;
}

/* Separate signing and encryption keys, both derived from the device key,
 * so the key itself never goes on air in any form. */
static bool keys(uint8_t km[32], uint8_t ke[32])
{
    static const char MAC_LABEL[] = "usbstore-link mac";
    static const char ENC_LABEL[] = "usbstore-link enc";
    char key[72];
    if (!auth_device_key_get(key, sizeof(key))) {
        return false;
    }
    hmac((const uint8_t *)key, strlen(key), MAC_LABEL, strlen(MAC_LABEL), NULL, 0, km);
    hmac((const uint8_t *)key, strlen(key), ENC_LABEL, strlen(ENC_LABEL), NULL, 0, ke);
    memset(key, 0, sizeof(key));
    return true;
}

/* Keystream = HMAC(ke, header || block#). The header holds the requester's
 * random nonce and our clock, so no two replies share a keystream. */
static void keystream_xor(const uint8_t ke[32], const dl_hdr_t *h, uint8_t *p, size_t n)
{
    uint8_t ks[32];
    for (size_t off = 0; off < n; off += 32) {
        uint8_t blk = (uint8_t)(off / 32);
        hmac(ke, 32, h, sizeof(*h), &blk, 1, ks);
        for (size_t i = 0; i < 32 && off + i < n; i++) {
            p[off + i] ^= ks[i];
        }
    }
}

static bool tag_ok(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    for (int i = 0; i < DL_TAG; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

/* -------------------------------------------------------------- task list */

/* End of the JSON object that starts at p ('{'), honouring strings. */
static const char *obj_end(const char *p)
{
    int  depth = 0;
    bool str   = false;
    for (; *p; p++) {
        if (str) {
            if (*p == '\\' && p[1]) {
                p++;
            } else if (*p == '"') {
                str = false;
            }
        } else if (*p == '"') {
            str = true;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}' && --depth == 0) {
            return p;
        }
    }
    return NULL;
}

/* Don't leave half a UTF-8 character at the end of a cut title. */
static void utf8_trim(char *s)
{
    size_t i = strlen(s), back = 0;
    while (i > 0 && back < 4 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) {
        i--;
        back++;
    }
    if (i == 0) {
        return;
    }
    uint8_t lead = (uint8_t)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && back + 1 < need) {
        s[i - 1] = '\0';
    }
}

/* {"v":1,"generated":..,"items":[{"at","date","time","allDay","title"},..]}
 * The page writes it; this only needs the flat item objects. */
static int parse_upcoming(const char *json, up_t *out, int max)
{
    const char *p = strstr(json, "\"items\"");
    if (!p || !(p = strchr(p, '['))) {
        return 0;
    }
    p++;
    int  n = 0;
    char obj[512];
    while (n < max) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') {
            p++;
        }
        if (*p != '{') {
            break;                                   /* ']' or junk: done */
        }
        const char *e = obj_end(p);
        if (!e) {
            break;
        }
        size_t len = (size_t)(e - p) + 1;
        if (len < sizeof(obj)) {
            memcpy(obj, p, len);
            obj[len] = '\0';
            double at;
            char   date[16], tm[8] = "";
            int    y, mo, d, hh, mm;
            up_t  *u = &out[n];
            if (web_json_num(obj, "at", &at) && at > 0 &&
                web_json_str(obj, "date", date, sizeof(date)) &&
                sscanf(date, "%d-%d-%d", &y, &mo, &d) == 3) {
                web_json_str(obj, "time", tm, sizeof(tm));
                u->at   = (uint32_t)at;
                u->ymd  = (uint32_t)(y * 10000 + mo * 100 + d);
                u->mins = (strstr(obj, "\"allDay\":true") == NULL &&
                           sscanf(tm, "%d:%d", &hh, &mm) == 2)
                              ? (uint16_t)(hh * 60 + mm) : 0xFFFF;
                if (!web_json_str(obj, "title", u->title, sizeof(u->title))) {
                    u->title[0] = '\0';
                }
                utf8_trim(u->title);
                n++;
            }
        }
        p = e + 1;
    }
    /* The page sorts them already; don't rely on it. */
    for (int i = 1; i < n; i++) {
        up_t k = out[i];
        int  j = i - 1;
        while (j >= 0 && out[j].at > k.at) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = k;
    }
    return n;
}

/* Caller holds a ref on the system drive. */
static void load_file(const char *path)
{
    static up_t tmp[UP_MAX];          /* only ever touched with s_up_mux held */
    char serial[40];
    sysdrive_serial(serial, sizeof(serial));

    int   n    = 0;
    bool  ok   = false;
    char *text = NULL;
    struct stat st;
    if (stat(path, &st) != 0) {
        ok = true;                    /* never saved: an empty list is an answer */
    } else if (st.st_size > 0 && st.st_size <= UP_FILE_MAX &&
               (text = malloc(st.st_size + 1)) != NULL) {
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t got = fread(text, 1, st.st_size, f);
            fclose(f);
            text[got] = '\0';
            ok = got == (size_t)st.st_size;
        }
    }

    xSemaphoreTake(s_up_mux, portMAX_DELAY);
    if (ok) {
        n = text ? parse_upcoming(text, tmp, UP_MAX) : 0;
        memcpy(s_up, tmp, sizeof(up_t) * n);
        s_up_n     = n;
        s_up_known = true;
        snprintf(s_up_serial, sizeof(s_up_serial), "%s", serial);
    }
    xSemaphoreGive(s_up_mux);
    free(text);

    if (ok) {
        ESP_LOGI(TAG, "%d upcoming task(s) loaded", n);
    } else {
        ESP_LOGW(TAG, "could not read %s", path);
    }
}

void devlink_upcoming_saved(const char *path)
{
    if (s_up_mux) {
        load_file(path);
    }
}

/* First request after boot, or after the system drive changed. Runs after
 * the reply has gone, since it may wait for a drive to be unparked. */
static void ensure_loaded(void)
{
    char serial[40];
    sysdrive_serial(serial, sizeof(serial));
    xSemaphoreTake(s_up_mux, portMAX_DELAY);
    if (s_up_known && strcmp(serial, s_up_serial) != 0) {
        s_up_known = false;           /* a different drive: its own planner */
        s_up_n     = 0;
    }
    bool known = s_up_known;
    xSemaphoreGive(s_up_mux);

    int64_t now = esp_timer_get_time();
    if (known || (s_up_tried_us >= 0 && now - s_up_tried_us < RETRY_S * 1000000LL)) {
        return;
    }

    /* No system drive yet - not chosen, or (just after boot) not mounted -
     * doesn't count as an attempt: the next HELLO, a minute later, tries
     * again. Starting the retry clock here once left Desk-Disp without its
     * tasks for five minutes after every reboot. */
    char path[112], why[112];
    if (!sysdrive_path("planner/upcoming.json", path, sizeof(path))) {
        return;
    }
    s_up_tried_us = now;
    usbstore_ref_t ref;
    if (usbstore_acquire(path, why, sizeof(why), &ref) != ESP_OK) {
        ESP_LOGW(TAG, "task list: %s", why);
        return;
    }
    load_file(path);
    usbstore_release(ref);
}

/* ------------------------------------------------------------------ packets */

static void send_to(const uint8_t mac[6], const dl_tasks_t *out)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = { .ifidx = WIFI_IF_STA, .channel = 0, .encrypt = false };
        memcpy(peer.peer_addr, mac, 6);
        esp_now_add_peer(&peer);
    }
    esp_err_t err = esp_now_send(mac, (const uint8_t *)out, sizeof(*out));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send: %s", esp_err_to_name(err));
    }
}

/* Unpack the fixed-point wire format into the climate log's floats. */
static void ingest_sky(const dl_hello_t *in)
{
    const dl_sky_t *k = &in->sky;
    climate_sky_t sky = {
        .at = k->at, .ymd = k->ymd,
        .sun_alt = k->sun_alt / 100.0f, .sun_az = k->sun_az / 100.0f,
        .moon_alt = k->moon_alt / 100.0f, .moon_az = k->moon_az / 100.0f,
        .illum = k->illum / 100.0f, .phase = k->phase / 10000.0f,
        .sunrise = k->sunrise, .sunset = k->sunset,
        .moonrise = k->moonrise, .moonset = k->moonset,
    };
    memcpy(sky.place, k->place, sizeof(k->place));
    sky.place[sizeof(k->place)] = '\0';

    climate_wx_t wx;
    const dl_wx_t *w = &in->wx;
    if (in->flags & HF_WX) {
        wx = (climate_wx_t){
            .t = w->t / 100.0f, .h = w->h / 100.0f, .feels = w->feels / 100.0f,
            .cloud = w->cloud, .wind = w->wind / 10.0f, .wdir = w->wdir,
            .rain = w->rain / 10.0f, .pres = w->pres / 10.0f, .code = w->code,
        };
    }

    climate_fc_t fc[CLIMATE_FC_MAX];
    int nfc = in->nfc < 3 ? in->nfc : 3;
    for (int i = 0; i < nfc; i++) {
        fc[i] = (climate_fc_t){ .ymd = in->fc[i].ymd, .code = in->fc[i].code,
                                .hi = in->fc[i].hi / 10.0f, .lo = in->fc[i].lo / 10.0f,
                                .pop = in->fc[i].pop };
    }
    climate_ingest_sky(&sky, (in->flags & HF_WX) ? &wx : NULL, fc, nfc);
}

static void on_hello(const uint8_t mac[6], const dl_hello_t *in)
{
    uint8_t km[32], ke[32], full[32];
    if (!keys(km, ke)) {
        return;                       /* no device key set: nothing to check against */
    }
    hmac(km, 32, in, offsetof(dl_hello_t, tag), NULL, 0, full);
    if (!tag_ok(full, in->tag)) {
        s_bad++;
        return;
    }

    /* Desk-Disp repeats a HELLO until it hears back. A repeat of the one just
     * answered gets the same reply again; an older one is a replay. */
    if (in->h.nonce == s_last_nonce && !memcmp(mac, s_peer, 6)) {
        send_to(mac, &s_last_reply);
        return;
    }
    for (int i = 0; i < SEEN; i++) {
        if (s_seen[i] == in->h.nonce) {
            s_bad++;
            return;
        }
    }
    s_seen[s_seen_i] = in->h.nonce;
    s_seen_i = (s_seen_i + 1) % SEEN;
    s_hellos++;
    s_last_us = esp_timer_get_time();
    memcpy(s_peer, mac, 6);

    uint32_t now   = (uint32_t)time(NULL);
    bool     clock = now > CLOCK_VALID;
    uint8_t  st    = clock ? ST_CLOCK : 0;
    /* A signed packet can still be recorded and played back later; the
     * sender's clock has to be close to ours before anything in it counts. */
    long skew  = (long)in->h.epoch - (long)now;
    bool fresh = clock && in->h.epoch >= CLOCK_VALID && skew <= SKEW_MAX_S && skew >= -SKEW_MAX_S;
    if (in->flags & HF_ROOM) {
        if (!fresh) {
            st |= ST_REFUSED;
        } else {
            switch (climate_ingest(in->t_c / 100.0, in->h_c / 100.0)) {
            case CLIMATE_STORED:   st |= ST_STORED;  break;
            case CLIMATE_TOO_SOON:                   break;
            default:               st |= ST_REFUSED; break;
            }
        }
    }
    if ((in->flags & HF_SKY) && fresh) {
        ingest_sky(in);
    }
    if ((in->flags & HF_BRIDGE) && fresh) {
        news_set_bridge(in->bridge_ip, in->bridge_port);
    }

    dl_tasks_t out;
    memset(&out, 0, sizeof(out));
    out.h = (dl_hdr_t){ .magic = DL_MAGIC, .ver = DL_VER, .type = DL_TASKS,
                        .nonce = in->h.nonce, .epoch = clock ? now : 0 };

    xSemaphoreTake(s_up_mux, portMAX_DELAY);
    if (s_up_known) {
        st |= ST_KNOWN;
    }
    for (int i = 0; i < s_up_n && out.count < DL_ITEMS; i++) {
        const up_t *u = &s_up[i];
        uint32_t ends = u->mins == 0xFFFF ? u->at + 86400 : u->at;
        if (clock && ends <= now) {
            continue;                 /* already started (timed) or over (all-day) */
        }
        dl_item_t *o = &out.items[out.count++];
        o->at   = u->at;
        o->ymd  = u->ymd;
        o->mins = u->mins;
        o->tlen = (uint8_t)strnlen(u->title, DL_TITLE);
        memcpy(o->title, u->title, o->tlen);
    }
    xSemaphoreGive(s_up_mux);
    out.status = st;

    keystream_xor(ke, &out.h, &out.status, offsetof(dl_tasks_t, tag) - offsetof(dl_tasks_t, status));
    hmac(km, 32, &out, offsetof(dl_tasks_t, tag), NULL, 0, full);
    memcpy(out.tag, full, DL_TAG);

    s_last_nonce = in->h.nonce;
    s_last_reply = out;
    send_to(mac, &out);
}

/* WiFi task context: copy and hand over, nothing more. */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != sizeof(dl_hello_t)) {
        return;
    }
    rx_t rx;
    memcpy(rx.mac, info->src_addr, 6);
    memcpy(&rx.pkt, data, sizeof(rx.pkt));
    if (rx.pkt.h.magic != DL_MAGIC || rx.pkt.h.ver != DL_VER || rx.pkt.h.type != DL_HELLO) {
        return;
    }
    xQueueSend(s_rx, &rx, 0);
}

static void devlink_task(void *arg)
{
    rx_t rx;
    for (;;) {
        if (xQueueReceive(s_rx, &rx, portMAX_DELAY) == pdTRUE) {
            on_hello(rx.mac, &rx.pkt);
            ensure_loaded();
        }
    }
}

esp_err_t devlink_start(void)
{
    if (!wifi_mgr_is_station()) {
        return ESP_OK;                /* setup-AP mode: no LAN, no Desk-Disp */
    }
    if (!hmac_selftest()) {
        ESP_LOGE(TAG, "HMAC self-test failed - link disabled");
        return ESP_FAIL;
    }
    s_up_mux = xSemaphoreCreateMutex();
    s_rx     = xQueueCreate(4, sizeof(rx_t));
    if (!s_up_mux || !s_rx) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        return err;
    }
    /* Left alone, ESP-NOW keeps the radio awake all the time - more heat
     * for a board that already runs warm. Listening 25 ms in every 100 is
     * plenty: Desk-Disp repeats its HELLO for a few seconds until answered. */
    esp_wifi_connectionless_module_set_wake_interval(100);
    esp_now_set_wake_window(25);
    esp_now_register_recv_cb(on_recv);

    if (xTaskCreatePinnedToCore(devlink_task, "devlink", 6144, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_on = true;

    uint8_t ch = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&ch, &sec);
    ESP_LOGI(TAG, "ESP-NOW listening on channel %u", ch);
    return ESP_OK;
}

void devlink_status_json(char *out, size_t n)
{
    if (!s_on) {
        snprintf(out, n, "{\"on\":false}");
        return;
    }
    uint8_t ch = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&ch, &sec);
    long long age = s_last_us ? (esp_timer_get_time() - s_last_us) / 1000000 : -1;
    snprintf(out, n,
             "{\"on\":true,\"ch\":%u,\"peer\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
             "\"age_s\":%lld,\"hellos\":%lu,\"bad\":%lu,\"tasks\":%d,\"known\":%s}",
             ch, s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5],
             age, (unsigned long)s_hellos, (unsigned long)s_bad, s_up_n,
             s_up_known ? "true" : "false");
}
