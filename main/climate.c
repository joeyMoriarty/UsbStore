#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "climate.h"
#include "sysdrive.h"
#include "usb_storage.h"
#include "auth.h"
#include "web_server.h"

static const char *TAG = "climate";

extern const uint8_t climate_html_start[] asm("_binary_climate_html_start");
extern const uint8_t climate_html_end[]   asm("_binary_climate_html_end");

#define RETAIN_DAYS   92
#define FLUSH_AFTER_S 600         /* write to the drive every 10 minutes */
#define RING_MAX      720         /* readings that can wait for a missing drive */
#define TASK_TICK_S   30
#define CLOCK_VALID   1735689600  /* 2025-01-01: before this, NTP hasn't answered */
#define DAY_S         86400
#define NF_MAX        7           /* most fields in any series */
#define CSV_LINE_MAX      192

typedef struct {
    uint32_t at;                  /* epoch seconds, UTC */
    float    v[NF_MAX];
} sample_t;

/*
 * One logged series: a fixed set of numeric fields, sampled no more often
 * than min_gap. Room, outdoor and sky all go through the same code - the
 * RAM buffer, the day files, the hourly rollups, the pruning and the charts
 * - and differ only in this table.
 */
typedef struct {
    const char *name;             /* ?series= */
    const char *dir;              /* under usbstore/ on the system drive */
    int         nf;
    uint32_t    min_gap;
    const char *fields;           /* for the chart JSON */

    /* Readings waiting to be written, oldest first. Buffering in RAM and
     * writing in batches means the system drive is touched six times an hour
     * instead of sixty - with park-and-swap, each touch may mean swapping it
     * in. The cost is honest: a power cut loses up to 10 minutes. */
    sample_t   *ring;
    int         head, count;
    uint64_t    dropped;          /* oldest readings pushed out by a full ring */
    sample_t    latest;
    bool        have_latest;
} series_t;

enum { SER_ROOM, SER_OUTDOOR, SER_SKY, SER_N };

static series_t s_ser[SER_N] = {
    [SER_ROOM]    = { "room",    "climate",         2, 20,
                      "[\"t\",\"h\"]" },
    [SER_OUTDOOR] = { "outdoor", "climate/outdoor", 7, 300,
                      "[\"t\",\"h\",\"feels\",\"cloud\",\"wind\",\"rain\",\"pres\"]" },
    [SER_SKY]     = { "sky",     "climate/sky",     5, 120,
                      "[\"sun_alt\",\"sun_az\",\"moon_alt\",\"moon_az\",\"illum\"]" },
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Latest sky, weather and forecast, for /api/sky - and the two once-a-day
 * lines (day summary, forecast) waiting for the next flush. All under s_mux. */
static climate_sky_t s_sky;
static climate_wx_t  s_wx;
static climate_fc_t  s_fc[CLIMATE_FC_MAX];
static int           s_nfc;
static bool          s_have_sky, s_have_wx;
static uint32_t      s_sky_rx;               /* when it arrived, our clock */

static uint32_t      s_day_logged;           /* ymd of the last day line written */
static bool          s_day_pending;
static climate_sky_t s_day;
static bool          s_fc_pending;
static climate_fc_t  s_fc_logged[CLIMATE_FC_MAX];
static int           s_nfc_logged;

/* Serialises climate file I/O: a flush, the daily maintenance and a chart
 * request must never interleave on the same files. */
static SemaphoreHandle_t s_io;

static uint32_t s_last_maint_day;
static uint32_t s_last_flush;

/* --------------------------------------------------------------- helpers */

static bool clock_ok(void)
{
    return time(NULL) > CLOCK_VALID;
}

static void day_name(uint32_t day, char *out, size_t n)
{
    time_t t = (time_t)day * DAY_S;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, n, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* <dir>/<kind>/YYYYMMDD.csv on the system drive. */
static bool day_file(const char *dir, const char *kind, uint32_t day, char *out, size_t n)
{
    char name[12], sub[64];
    day_name(day, name, sizeof(name));
    snprintf(sub, sizeof(sub), "%s/%s/%s.csv", dir, kind, name);
    return sysdrive_path(sub, out, n);
}

static series_t *series_named(const char *name)
{
    for (int i = 0; i < SER_N; i++) {
        if (!strcmp(s_ser[i].name, name)) {
            return &s_ser[i];
        }
    }
    return NULL;
}

bool climate_latest(float *temp, float *hum, uint32_t *age_s)
{
    series_t *s = &s_ser[SER_ROOM];
    portENTER_CRITICAL(&s_mux);
    bool have = s->have_latest;
    sample_t l = s->latest;
    portEXIT_CRITICAL(&s_mux);
    if (!have) {
        return false;
    }
    *temp = l.v[0];
    *hum  = l.v[1];
    uint32_t now = (uint32_t)time(NULL);
    *age_s = now > l.at ? now - l.at : 0;
    return true;
}

/* ---------------------------------------------------------------- ingest */

static bool push(series_t *s, uint32_t at, const float *v)
{
    if (!s->ring) {
        return false;
    }
    sample_t smp = { .at = at };
    memcpy(smp.v, v, sizeof(float) * s->nf);
    bool stored = false;
    portENTER_CRITICAL(&s_mux);
    if (!s->have_latest || at >= s->latest.at + s->min_gap) {
        if (s->count == RING_MAX) {                /* drive missing for hours: */
            s->head = (s->head + 1) % RING_MAX;    /* drop the oldest */
            s->count--;
            s->dropped++;
        }
        s->ring[(s->head + s->count) % RING_MAX] = smp;
        s->count++;
        stored = true;
    }
    s->latest      = smp;
    s->have_latest = true;
    portEXIT_CRITICAL(&s_mux);
    return stored;
}

climate_result_t climate_ingest(double t, double h)
{
    /* The AHT10's rated range. Anything outside it is a wiring or parsing
     * fault, and one bad point would wreck a chart's scale. */
    if (!(t >= -40 && t <= 85 && h >= 0 && h <= 100)) {
        return CLIMATE_OUT_OF_RANGE;
    }
    if (!clock_ok()) {
        return CLIMATE_NO_CLOCK;
    }
    float v[NF_MAX] = { (float)t, (float)h };
    return push(&s_ser[SER_ROOM], (uint32_t)time(NULL), v) ? CLIMATE_STORED
                                                            : CLIMATE_TOO_SOON;
}

static bool fc_same(const climate_fc_t *a, int na, const climate_fc_t *b, int nb)
{
    if (na != nb) {
        return false;
    }
    for (int i = 0; i < na; i++) {
        if (a[i].ymd != b[i].ymd || a[i].code != b[i].code || a[i].hi != b[i].hi ||
            a[i].lo != b[i].lo || a[i].pop != b[i].pop) {
            return false;
        }
    }
    return true;
}

void climate_ingest_sky(const climate_sky_t *sky, const climate_wx_t *wx,
                        const climate_fc_t *fc, int nfc)
{
    if (!clock_ok()) {
        return;
    }
    uint32_t now = (uint32_t)time(NULL);
    /* Stamp with the bridge's own time if it's believable - the sun moves a
     * quarter of a degree a minute - otherwise with ours. */
    uint32_t at = (sky->at + 600 > now && sky->at < now + 600) ? sky->at : now;
    if (nfc > CLIMATE_FC_MAX) {
        nfc = CLIMATE_FC_MAX;
    }

    float v[NF_MAX] = { sky->sun_alt, sky->sun_az, sky->moon_alt, sky->moon_az, sky->illum };
    push(&s_ser[SER_SKY], at, v);
    if (wx) {
        float w[NF_MAX] = { wx->t, wx->h, wx->feels, wx->cloud, wx->wind, wx->rain, wx->pres };
        push(&s_ser[SER_OUTDOOR], at, w);
    }

    portENTER_CRITICAL(&s_mux);
    s_sky      = *sky;
    s_have_sky = true;
    s_sky_rx   = now;
    if (wx) {
        s_wx      = *wx;
        s_have_wx = true;
    }
    if (nfc > 0) {
        memcpy(s_fc, fc, sizeof(climate_fc_t) * nfc);
        s_nfc = nfc;
        if (!fc_same(fc, nfc, s_fc_logged, s_nfc_logged)) {
            s_fc_pending = true;
        }
    }
    if (sky->ymd && sky->ymd != s_day_logged) {
        s_day         = *sky;
        s_day_pending = true;
    }
    portEXIT_CRITICAL(&s_mux);
}

static esp_err_t h_ingest(httpd_req_t *r)
{
    if (!auth_device_check(r)) {
        return ESP_OK;
    }
    char body[160];
    if (!web_small_body(r, body, sizeof(body))) {
        return web_fail(r, 400, "bad body");
    }
    double t, h;
    if (!web_json_num(body, "t", &t) || !web_json_num(body, "h", &h)) {
        return web_fail(r, 400, "need t and h");
    }
    switch (climate_ingest(t, h)) {
    case CLIMATE_OUT_OF_RANGE:
        return web_fail(r, 400, "reading out of range");
    case CLIMATE_NO_CLOCK:
        return web_fail(r, 503, "clock not set yet - waiting for internet time");
    case CLIMATE_STORED:
        httpd_resp_set_type(r, "application/json");
        httpd_resp_sendstr(r, "{\"ok\":true,\"stored\":true}");
        return ESP_OK;
    default:
        httpd_resp_set_type(r, "application/json");
        httpd_resp_sendstr(r, "{\"ok\":true,\"stored\":false}");
        return ESP_OK;
    }
}

/* ----------------------------------------------------------------- flush */

static int fmt_sample(char *out, size_t n, const sample_t *s, int nf)
{
    int o = snprintf(out, n, "%lu", (unsigned long)s->at);
    for (int i = 0; i < nf && o < (int)n; i++) {
        o += snprintf(out + o, n - o, ",%.2f", s->v[i]);
    }
    if (o < (int)n) {
        o += snprintf(out + o, n - o, "\n");
    }
    return o;
}

/* Append one series' pending readings to its day files. Caller holds the
 * drive ref and s_io. Readings stay in RAM if anything fails. */
static void flush_series(series_t *s, sample_t *snap)
{
    uint64_t dropped_before;
    portENTER_CRITICAL(&s_mux);
    int n = s->count;
    for (int i = 0; i < n; i++) {
        snap[i] = s->ring[(s->head + i) % RING_MAX];
    }
    dropped_before = s->dropped;
    portEXIT_CRITICAL(&s_mux);
    if (n == 0) {
        return;
    }

    char dir[96], sub[48];
    snprintf(sub, sizeof(sub), "%s/raw", s->dir);
    if (!sysdrive_path(sub, dir, sizeof(dir))) {
        return;
    }
    sysdrive_mkdirs(dir);

    int written = 0;
    bool ok = true;
    char line[CSV_LINE_MAX];
    while (ok && written < n) {
        /* One day file per run of same-day readings. */
        uint32_t day = snap[written].at / DAY_S;
        char path[112];
        if (!day_file(s->dir, "raw", day, path, sizeof(path))) {
            break;
        }
        FILE *f = fopen(path, "a");
        if (!f) {
            break;
        }
        while (written < n && snap[written].at / DAY_S == day) {
            fmt_sample(line, sizeof(line), &snap[written], s->nf);
            if (fputs(line, f) < 0) {
                ok = false;
                break;
            }
            written++;
        }
        if (fclose(f) != 0) {
            ok = false;
        }
    }

    /* Drop only what reached the drive. If the ring overflowed meanwhile,
     * some of those were already dropped from the front - don't drop twice. */
    portENTER_CRITICAL(&s_mux);
    int gone = (int)(s->dropped - dropped_before);
    int drop = written > gone ? written - gone : 0;
    if (drop > s->count) {
        drop = s->count;
    }
    s->head   = (s->head + drop) % RING_MAX;
    s->count -= drop;
    portEXIT_CRITICAL(&s_mux);

    if (written) {
        ESP_LOGI(TAG, "%s: logged %d reading(s)%s", s->name, written,
                 ok ? "" : " (then a write failed)");
    }
}

/* The once-a-day sun/moon line, and a forecast when it changes. */
static void flush_extras(void)
{
    portENTER_CRITICAL(&s_mux);
    bool day_p = s_day_pending, fc_p = s_fc_pending;
    climate_sky_t d = s_day;
    climate_fc_t fc[CLIMATE_FC_MAX];
    int nfc = s_nfc;
    memcpy(fc, s_fc, sizeof(fc));
    portEXIT_CRITICAL(&s_mux);

    char sub[48], path[112];
    if (day_p) {
        snprintf(sub, sizeof(sub), "climate/days/%06lu.csv", (unsigned long)(d.ymd / 100));
        if (sysdrive_path("climate/days", path, sizeof(path))) {
            sysdrive_mkdirs(path);
        }
        FILE *f = sysdrive_path(sub, path, sizeof(path)) ? fopen(path, "a") : NULL;
        if (f) {
            bool ok = fprintf(f, "%lu,%lu,%lu,%lu,%lu,%.1f,%.4f\n",
                              (unsigned long)d.ymd, (unsigned long)d.sunrise,
                              (unsigned long)d.sunset, (unsigned long)d.moonrise,
                              (unsigned long)d.moonset, d.illum, d.phase) > 0;
            if (fclose(f) == 0 && ok) {
                portENTER_CRITICAL(&s_mux);
                s_day_logged = d.ymd;
                if (s_day.ymd == d.ymd) {
                    s_day_pending = false;
                }
                portEXIT_CRITICAL(&s_mux);
            }
        }
    }
    if (fc_p && nfc > 0) {
        uint32_t now = (uint32_t)time(NULL);
        if (sysdrive_path("climate/forecast", path, sizeof(path))) {
            sysdrive_mkdirs(path);
        }
        FILE *f = day_file("climate", "forecast", now / DAY_S, path, sizeof(path))
                      ? fopen(path, "a") : NULL;
        if (f) {
            bool ok = true;
            for (int i = 0; i < nfc; i++) {
                ok &= fprintf(f, "%lu,%lu,%d,%.1f,%.1f,%.0f\n", (unsigned long)now,
                              (unsigned long)fc[i].ymd, fc[i].code, fc[i].hi, fc[i].lo,
                              fc[i].pop) > 0;
            }
            if (fclose(f) == 0 && ok) {
                portENTER_CRITICAL(&s_mux);
                memcpy(s_fc_logged, fc, sizeof(fc));
                s_nfc_logged = nfc;
                s_fc_pending = !fc_same(s_fc, s_nfc, fc, nfc);
                portEXIT_CRITICAL(&s_mux);
            }
        }
    }
}

static void flush(void)
{
    char dir[96];
    if (!sysdrive_path("climate", dir, sizeof(dir))) {
        return;
    }
    sample_t *snap = heap_caps_malloc(RING_MAX * sizeof(sample_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) {
        return;
    }
    usbstore_ref_t ref;
    char why[112];
    if (usbstore_acquire(dir, why, sizeof(why), &ref) != ESP_OK) {
        ESP_LOGW(TAG, "flush deferred: %s", why);
        free(snap);
        return;
    }
    xSemaphoreTake(s_io, portMAX_DELAY);
    for (int i = 0; i < SER_N; i++) {
        flush_series(&s_ser[i], snap);
    }
    flush_extras();
    xSemaphoreGive(s_io);
    usbstore_release(ref);
    free(snap);
    s_last_flush = (uint32_t)time(NULL);
}

static bool flush_due(void)
{
    uint32_t now = (uint32_t)time(NULL);
    bool due = false;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SER_N; i++) {
        const series_t *s = &s_ser[i];
        if (s->count > 0 && now - s->ring[s->head].at >= FLUSH_AFTER_S) {
            due = true;
        }
    }
    if ((s_day_pending || s_fc_pending) && now - s_last_flush >= FLUSH_AFTER_S) {
        due = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return due;
}

/* ----------------------------------------------------------- aggregation */

typedef struct {
    uint32_t n;
    float    min[NF_MAX], max[NF_MAX], sum[NF_MAX];
} acc_t;

/* Feeds buckets of `bucket_s` seconds starting at `start`. */
typedef struct {
    acc_t   *acc;
    int      nb, nf;
    uint32_t start, bucket_s;
} feed_t;

/* One reading (n == 1, mn == av == mx) or one pre-summarised hour. */
static void feed_one(feed_t *f, uint32_t at, uint32_t n,
                     const float *mn, const float *av, const float *mx)
{
    if (at < f->start) {
        return;
    }
    uint32_t i = (at - f->start) / f->bucket_s;
    if (i >= (uint32_t)f->nb) {
        return;
    }
    acc_t *a = &f->acc[i];
    for (int k = 0; k < f->nf; k++) {
        if (a->n == 0) {
            a->min[k] = mn[k];
            a->max[k] = mx[k];
        } else {
            a->min[k] = fminf(a->min[k], mn[k]);
            a->max[k] = fmaxf(a->max[k], mx[k]);
        }
        a->sum[k] += av[k] * n;
    }
    a->n += n;
}

/* "at,v1,v2,..." - exactly nf values, or the line is skipped. */
static bool parse_raw(const char *line, int nf, uint32_t *at, float *v)
{
    char *e;
    *at = (uint32_t)strtoul(line, &e, 10);
    for (int k = 0; k < nf; k++) {
        if (*e != ',') {
            return false;
        }
        v[k] = strtof(e + 1, &e);
    }
    return true;
}

static bool feed_raw_file(feed_t *f, const char *path, char *line, size_t cap)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    uint32_t at;
    float v[NF_MAX];
    while (fgets(line, cap, fp)) {
        if (parse_raw(line, f->nf, &at, v)) {
            feed_one(f, at, 1, v, v, v);
        }
    }
    fclose(fp);
    return true;
}

/* "hour,n,min1,avg1,max1,min2,..." */
static bool feed_hourly_file(feed_t *f, const char *path, char *line, size_t cap)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    while (fgets(line, cap, fp)) {
        char *e;
        uint32_t at = (uint32_t)strtoul(line, &e, 10);
        if (*e != ',') continue;
        uint32_t n = (uint32_t)strtoul(e + 1, &e, 10);
        float mn[NF_MAX], av[NF_MAX], mx[NF_MAX];
        bool ok = n > 0;
        for (int k = 0; ok && k < f->nf; k++) {
            if (*e != ',') { ok = false; break; } mn[k] = strtof(e + 1, &e);
            if (*e != ',') { ok = false; break; } av[k] = strtof(e + 1, &e);
            if (*e != ',') { ok = false; break; } mx[k] = strtof(e + 1, &e);
        }
        if (ok) {
            feed_one(f, at, n, mn, av, mx);
        }
    }
    fclose(fp);
    return true;
}

/* Summarise one finished day's raw file into 24 hourly lines. Written to a
 * temp name and renamed, so a crash leaves no half-written summary - and
 * since it only derives from raw, a missing one is simply rebuilt. */
static void rollup_day(const series_t *s, uint32_t day, char *line, size_t cap)
{
    char raw[112], hourly[112], tmp[120], sub[48], dir[96];
    if (!day_file(s->dir, "raw", day, raw, sizeof(raw)) ||
        !day_file(s->dir, "hourly", day, hourly, sizeof(hourly))) {
        return;
    }
    if (access(hourly, F_OK) == 0 || access(raw, F_OK) != 0) {
        return;                                   /* done already, or no data */
    }

    acc_t acc[24];
    memset(acc, 0, sizeof(acc));
    feed_t f = { .acc = acc, .nb = 24, .nf = s->nf, .start = day * DAY_S, .bucket_s = 3600 };
    feed_raw_file(&f, raw, line, cap);

    snprintf(sub, sizeof(sub), "%s/hourly", s->dir);
    if (!sysdrive_path(sub, dir, sizeof(dir))) {
        return;
    }
    sysdrive_mkdirs(dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", hourly);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        return;
    }
    for (int i = 0; i < 24; i++) {
        const acc_t *a = &acc[i];
        if (!a->n) continue;
        fprintf(fp, "%lu,%lu", (unsigned long)(day * DAY_S + i * 3600), (unsigned long)a->n);
        for (int k = 0; k < s->nf; k++) {
            fprintf(fp, ",%.2f,%.2f,%.2f", a->min[k], a->sum[k] / a->n, a->max[k]);
        }
        fputc('\n', fp);
    }
    if (fclose(fp) == 0) {
        rename(tmp, hourly);
    } else {
        unlink(tmp);
    }
}

/* Delete dated files older than the cutoff. Names start YYYYMMDD (or YYYYMM
 * for the monthly day summaries), so comparing that many characters of the
 * name against the cutoff's does it. */
static void prune(const char *sub, const char *cutoff, size_t digits)
{
    char dir[96];
    if (!sysdrive_path(sub, dir, sizeof(dir))) {
        return;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    int gone = 0;
    while ((e = readdir(d)) != NULL) {
        if (strlen(e->d_name) >= digits && e->d_name[0] >= '0' && e->d_name[0] <= '9' &&
            strncmp(e->d_name, cutoff, digits) < 0) {
            char path[400];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            if (unlink(path) == 0) gone++;
        }
    }
    closedir(d);
    if (gone) {
        ESP_LOGI(TAG, "pruned %d file(s) from %s older than %d days", gone, sub, RETAIN_DAYS);
    }
}

/* Once per UTC day: roll up the last few finished days, drop old files. */
static void maintenance(char *line, size_t cap)
{
    uint32_t today = (uint32_t)time(NULL) / DAY_S;
    if (today == s_last_maint_day) {
        return;
    }
    char dir[96];
    if (!sysdrive_path("climate", dir, sizeof(dir))) {
        return;
    }
    usbstore_ref_t ref;
    char why[112];
    if (usbstore_acquire(dir, why, sizeof(why), &ref) != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_io, portMAX_DELAY);
    char cutoff[12], sub[48];
    day_name(today - RETAIN_DAYS, cutoff, sizeof(cutoff));
    for (int i = 0; i < SER_N; i++) {
        for (uint32_t back = 1; back <= 3; back++) {
            rollup_day(&s_ser[i], today - back, line, cap);
        }
        snprintf(sub, sizeof(sub), "%s/raw", s_ser[i].dir);
        prune(sub, cutoff, 8);
        snprintf(sub, sizeof(sub), "%s/hourly", s_ser[i].dir);
        prune(sub, cutoff, 8);
    }
    prune("climate/forecast", cutoff, 8);
    prune("climate/days", cutoff, 6);            /* whole months past the window */
    xSemaphoreGive(s_io);
    usbstore_release(ref);
    s_last_maint_day = today;
}

static void climate_task(void *arg)
{
    static char line[CSV_LINE_MAX];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TASK_TICK_S * 1000));
        if (!clock_ok()) {
            continue;
        }
        if (flush_due()) {
            flush();
        }
        maintenance(line, sizeof(line));
    }
}

/* ---------------------------------------------------------------- charts */

static void feed_ram(feed_t *f, const series_t *s)
{
    portENTER_CRITICAL(&s_mux);
    int n = s->count, head = s->head;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
        sample_t smp = s->ring[(head + i) % RING_MAX];
        feed_one(f, smp.at, 1, smp.v, smp.v, smp.v);
    }
}

/*
 * GET /api/climate?series=room|outdoor|sky&range=day|week|month
 *
 *   day    last 24 h in 5-minute buckets, from the raw files
 *   week   last 7 days in 1-hour buckets
 *   month  last 30 days in 3-hour buckets
 *
 * Each point is [epoch, min1, avg1, max1, min2, avg2, max2, ...] in the
 * order of "fields". Week and month read the small hourly rollups where they
 * exist and fall back to raw for days not rolled up yet (today, always).
 * Readings still waiting in RAM are included, so the chart is live between
 * flushes. Public: anyone on the LAN can see the room's temperature.
 */
static void do_chart(httpd_req_t *r, char *buf)
{
    char range[8] = "day", name[12] = "room";
    web_param(r, "range", range, sizeof(range));
    web_param(r, "series", name, sizeof(name));
    series_t *s = series_named(name);
    if (!s || !s->ring) {
        web_fail(r, 400, "series must be room, outdoor or sky");
        return;
    }

    uint32_t span, bucket;
    if (!strcmp(range, "week"))       { span = 7 * DAY_S;  bucket = 3600;  }
    else if (!strcmp(range, "month")) { span = 30 * DAY_S; bucket = 10800; }
    else { strcpy(range, "day");        span = DAY_S;      bucket = 300;   }

    uint32_t now   = (uint32_t)time(NULL);
    uint32_t end   = (now / bucket + 1) * bucket;
    uint32_t start = end - span;
    int      nb    = span / bucket;

    acc_t *acc = heap_caps_calloc(nb, sizeof(acc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!acc) {
        acc = calloc(nb, sizeof(acc_t));
    }
    if (!acc) {
        web_fail(r, 500, "out of memory");
        return;
    }
    feed_t f = { .acc = acc, .nb = nb, .nf = s->nf, .start = start, .bucket_s = bucket };

    bool have_drive = false;
    char dir[96];
    usbstore_ref_t ref = -1;
    char why[112];
    if (clock_ok() && sysdrive_path("climate", dir, sizeof(dir)) &&
        usbstore_acquire(dir, why, sizeof(why), &ref) == ESP_OK) {
        have_drive = true;
        xSemaphoreTake(s_io, portMAX_DELAY);
        char line[CSV_LINE_MAX], path[112];
        for (uint32_t day = start / DAY_S; day <= now / DAY_S; day++) {
            bool done = false;
            if (bucket >= 3600 && day_file(s->dir, "hourly", day, path, sizeof(path))) {
                done = feed_hourly_file(&f, path, line, sizeof(line));
            }
            if (!done && day_file(s->dir, "raw", day, path, sizeof(path))) {
                feed_raw_file(&f, path, line, sizeof(line));
            }
        }
        feed_ram(&f, s);          /* pending readings, while files can't change */
        xSemaphoreGive(s_io);
        usbstore_release(ref);
    } else {
        feed_ram(&f, s);          /* no system drive: still chart what's in RAM */
    }

    char now_json[160] = "null";
    portENTER_CRITICAL(&s_mux);
    bool have = s->have_latest;
    sample_t l = s->latest;
    portEXIT_CRITICAL(&s_mux);
    if (have) {
        int o = snprintf(now_json, sizeof(now_json), "{\"age_s\":%lu,\"v\":[",
                         (unsigned long)(now > l.at ? now - l.at : 0));
        for (int k = 0; k < s->nf; k++) {
            o += snprintf(now_json + o, sizeof(now_json) - o, "%s%.2f", k ? "," : "", l.v[k]);
        }
        snprintf(now_json + o, sizeof(now_json) - o, "]}");
    }

    httpd_resp_set_type(r, "application/json");
    int o = snprintf(buf, WEB_BUF_SZ,
                     "{\"series\":\"%s\",\"fields\":%s,\"range\":\"%s\",\"bucket_s\":%lu,"
                     "\"start\":%lu,\"now\":%s,\"clock_ok\":%s,\"logging\":%s,\"pts\":[",
                     s->name, s->fields, range, (unsigned long)bucket, (unsigned long)start,
                     now_json, clock_ok() ? "true" : "false", have_drive ? "true" : "false");
    bool first = true;
    for (int i = 0; i < nb; i++) {
        const acc_t *a = &acc[i];
        if (!a->n) continue;
        if (o > WEB_BUF_SZ - 320) {
            if (httpd_resp_send_chunk(r, buf, o) != ESP_OK) {
                free(acc);
                web_drop_connection(r);
                return;
            }
            o = 0;
        }
        o += snprintf(buf + o, WEB_BUF_SZ - o, "%s[%lu", first ? "" : ",",
                      (unsigned long)(start + i * bucket));
        for (int k = 0; k < s->nf; k++) {
            o += snprintf(buf + o, WEB_BUF_SZ - o, ",%.1f,%.1f,%.1f",
                          a->min[k], a->sum[k] / a->n, a->max[k]);
        }
        o += snprintf(buf + o, WEB_BUF_SZ - o, "]");
        first = false;
    }
    free(acc);
    o += snprintf(buf + o, WEB_BUF_SZ - o, "]}");
    httpd_resp_send_chunk(r, buf, o);
    httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t h_chart(httpd_req_t *r)
{
    return web_hand_off(r, do_chart);
}

/* ------------------------------------------------------------------- sky */

typedef struct {
    uint32_t ymd, sr, ss, mr, ms;
    float    illum, phase;
} day_t;

#define DAYS_MAX 100

/* Day lines from one monthly file, later lines for the same day winning
 * (a reboot can log a day twice). */
static int read_days(const char *path, day_t *d, int n, char *line, size_t cap)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return n;
    }
    while (fgets(line, cap, fp)) {
        day_t x;
        unsigned long ymd, sr, ss, mr, ms;
        if (sscanf(line, "%lu,%lu,%lu,%lu,%lu,%f,%f", &ymd, &sr, &ss, &mr, &ms,
                   &x.illum, &x.phase) != 7) {
            continue;
        }
        x.ymd = ymd; x.sr = sr; x.ss = ss; x.mr = mr; x.ms = ms;
        int i = 0;
        while (i < n && d[i].ymd != x.ymd) i++;
        if (i == n) {
            if (n == DAYS_MAX) continue;
            n++;
        }
        d[i] = x;
    }
    fclose(fp);
    return n;
}

/*
 * GET /api/sky - the latest sun, moon, weather and forecast (from RAM), plus
 * sunrise/sunset and moon data for up to the last 92 days (from the drive).
 */
static void do_sky(httpd_req_t *r, char *buf)
{
    static day_t days[DAYS_MAX];         /* only touched with s_io held */
    int nd = 0;

    char dir[96];
    usbstore_ref_t ref;
    char why[112];
    bool locked = false;
    if (clock_ok() && sysdrive_path("climate", dir, sizeof(dir)) &&
        usbstore_acquire(dir, why, sizeof(why), &ref) == ESP_OK) {
        xSemaphoreTake(s_io, portMAX_DELAY);
        locked = true;
        /* This month and the three before it. */
        time_t t = time(NULL);
        struct tm tm;
        gmtime_r(&t, &tm);
        int y = tm.tm_year + 1900, m = tm.tm_mon + 1 - 3;
        if (m < 1) { m += 12; y--; }
        char line[CSV_LINE_MAX], sub[48], path[112];
        for (int k = 0; k < 4; k++) {
            snprintf(sub, sizeof(sub), "climate/days/%04d%02d.csv", y, m);
            if (sysdrive_path(sub, path, sizeof(path))) {
                nd = read_days(path, days, nd, line, sizeof(line));
            }
            if (++m > 12) { m = 1; y++; }
        }
        /* Not in the file yet: today's still in RAM. */
        portENTER_CRITICAL(&s_mux);
        bool pend = s_day_pending;
        climate_sky_t d = s_day;
        portEXIT_CRITICAL(&s_mux);
        if (pend) {
            int i = 0;
            while (i < nd && days[i].ymd != d.ymd) i++;
            if (i < DAYS_MAX) {
                days[i] = (day_t){ d.ymd, d.sunrise, d.sunset, d.moonrise, d.moonset,
                                   d.illum, d.phase };
                if (i == nd) nd++;
            }
        }
        usbstore_release(ref);
    }

    portENTER_CRITICAL(&s_mux);
    bool have_sky = s_have_sky, have_wx = s_have_wx;
    climate_sky_t sk = s_sky;
    climate_wx_t  wx = s_wx;
    climate_fc_t  fc[CLIMATE_FC_MAX];
    int nfc = s_nfc;
    memcpy(fc, s_fc, sizeof(fc));
    uint32_t rx = s_sky_rx;
    portEXIT_CRITICAL(&s_mux);
    uint32_t now = (uint32_t)time(NULL);

    httpd_resp_set_type(r, "application/json");
    int o = snprintf(buf, WEB_BUF_SZ, "{\"clock_ok\":%s,\"logging\":%s,\"sky\":",
                     clock_ok() ? "true" : "false", locked ? "true" : "false");
    if (have_sky) {
        char place[32];
        web_json_esc(place, sizeof(place), sk.place);
        o += snprintf(buf + o, WEB_BUF_SZ - o,
                      "{\"age_s\":%lu,\"place\":\"%s\",\"ymd\":%lu,"
                      "\"sun_alt\":%.2f,\"sun_az\":%.2f,\"moon_alt\":%.2f,\"moon_az\":%.2f,"
                      "\"illum\":%.1f,\"phase\":%.4f,\"sunrise\":%lu,\"sunset\":%lu,"
                      "\"moonrise\":%lu,\"moonset\":%lu}",
                      (unsigned long)(now > rx ? now - rx : 0), place, (unsigned long)sk.ymd,
                      sk.sun_alt, sk.sun_az, sk.moon_alt, sk.moon_az, sk.illum, sk.phase,
                      (unsigned long)sk.sunrise, (unsigned long)sk.sunset,
                      (unsigned long)sk.moonrise, (unsigned long)sk.moonset);
    } else {
        o += snprintf(buf + o, WEB_BUF_SZ - o, "null");
    }
    if (have_wx) {
        o += snprintf(buf + o, WEB_BUF_SZ - o,
                      ",\"wx\":{\"t\":%.1f,\"h\":%.0f,\"feels\":%.1f,\"cloud\":%.0f,"
                      "\"wind\":%.1f,\"wdir\":%.0f,\"rain\":%.1f,\"pres\":%.1f,\"code\":%d}",
                      wx.t, wx.h, wx.feels, wx.cloud, wx.wind, wx.wdir, wx.rain, wx.pres,
                      wx.code);
    } else {
        o += snprintf(buf + o, WEB_BUF_SZ - o, ",\"wx\":null");
    }
    o += snprintf(buf + o, WEB_BUF_SZ - o, ",\"fc\":[");
    for (int i = 0; i < nfc; i++) {
        o += snprintf(buf + o, WEB_BUF_SZ - o, "%s{\"ymd\":%lu,\"code\":%d,\"hi\":%.1f,"
                      "\"lo\":%.1f,\"pop\":%.0f}", i ? "," : "", (unsigned long)fc[i].ymd,
                      fc[i].code, fc[i].hi, fc[i].lo, fc[i].pop);
    }
    o += snprintf(buf + o, WEB_BUF_SZ - o, "],\"days\":[");

    /* Oldest first. */
    for (int i = 1; i < nd; i++) {
        day_t k = days[i];
        int j = i - 1;
        while (j >= 0 && days[j].ymd > k.ymd) { days[j + 1] = days[j]; j--; }
        days[j + 1] = k;
    }
    for (int i = 0; i < nd; i++) {
        if (o > WEB_BUF_SZ - 160) {
            if (httpd_resp_send_chunk(r, buf, o) != ESP_OK) {
                if (locked) xSemaphoreGive(s_io);
                web_drop_connection(r);
                return;
            }
            o = 0;
        }
        const day_t *d = &days[i];
        o += snprintf(buf + o, WEB_BUF_SZ - o, "%s[%lu,%lu,%lu,%lu,%lu,%.1f,%.4f]",
                      i ? "," : "", (unsigned long)d->ymd, (unsigned long)d->sr,
                      (unsigned long)d->ss, (unsigned long)d->mr, (unsigned long)d->ms,
                      d->illum, d->phase);
    }
    if (locked) {
        xSemaphoreGive(s_io);
    }
    o += snprintf(buf + o, WEB_BUF_SZ - o, "]}");
    httpd_resp_send_chunk(r, buf, o);
    httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t h_sky(httpd_req_t *r)
{
    return web_hand_off(r, do_sky);
}

static esp_err_t h_page(httpd_req_t *r)
{
    return web_send_page(r, climate_html_start, climate_html_end);
}

/* ------------------------------------------------------------------ init */

esp_err_t climate_start(void)
{
    s_io = xSemaphoreCreateMutex();
    if (!s_io) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < SER_N; i++) {
        s_ser[i].ring = heap_caps_calloc(RING_MAX, sizeof(sample_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ser[i].ring) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreatePinnedToCore(climate_task, "climate", 6144, NULL, 2, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void climate_routes(httpd_handle_t srv)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/climate",            .method = HTTP_GET,  .handler = h_page   },
        { .uri = "/api/climate",        .method = HTTP_GET,  .handler = h_chart  },
        { .uri = "/api/sky",            .method = HTTP_GET,  .handler = h_sky    },
        { .uri = "/api/climate/ingest", .method = HTTP_POST, .handler = h_ingest },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(srv, &routes[i]);
    }
}
