#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "nvs.h"

#include "news.h"
#include "web_server.h"

static const char *TAG = "news";

extern const uint8_t news_html_start[] asm("_binary_news_html_start");
extern const uint8_t news_html_end[]   asm("_binary_news_html_end");

#define NEWS_MAX      (256 * 1024)   /* today's document is ~30 KB */
#define NEWS_TTL_S    300            /* the bridge itself refreshes every 15 min */
#define REFRESH_MIN_S 30             /* the Refresh button can't hammer the PC */
#define NVS_NS        "news"

/* Where the bridge is. Written by the link task, read by web workers; six
 * bytes, and a torn read at worst means one failed fetch. */
static uint8_t  s_ip[4];
static uint16_t s_port;
static bool     s_have_bridge;

/* The last good document, in PSRAM. s_lock covers the cache and the fetch,
 * so two page loads at once don't both go to the PC. */
static SemaphoreHandle_t s_lock;
static char    *s_doc;
static size_t   s_len;
static int64_t  s_fetched_us = -1;   /* when s_doc arrived */
static int64_t  s_tried_us   = -1;   /* last attempt, good or not */
static char     s_err[80];

void news_set_bridge(const uint8_t ip[4], uint16_t port)
{
    if (s_have_bridge && !memcmp(ip, s_ip, 4) && port == s_port) {
        return;
    }
    memcpy(s_ip, ip, 4);
    s_port = port;
    s_have_bridge = true;
    ESP_LOGI(TAG, "PC bridge at %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);

    /* Kept across reboots, so the page works before Desk-Disp's next HELLO. */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t blob[6] = { ip[0], ip[1], ip[2], ip[3], (uint8_t)(port >> 8), (uint8_t)port };
        nvs_set_blob(h, "bridge", blob, sizeof(blob));
        nvs_commit(h);
        nvs_close(h);
    }
}

/* One GET of /api/news.json into a fresh PSRAM buffer. Caller holds s_lock. */
static bool fetch(void)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%u/api/news.json",
             s_ip[0], s_ip[1], s_ip[2], s_ip[3], s_port);
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(s_err, sizeof(s_err), "out of memory");
        return false;
    }

    bool ok = false;
    char *buf = NULL;
    size_t len = 0;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "PC bridge unreachable");
        goto done;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        snprintf(s_err, sizeof(s_err), "PC bridge answered HTTP %d", status);
        goto done;
    }
    buf = heap_caps_malloc(NEWS_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        snprintf(s_err, sizeof(s_err), "out of memory");
        goto done;
    }
    while (len < NEWS_MAX) {
        int n = esp_http_client_read(c, buf + len, NEWS_MAX - len);
        if (n < 0) {
            snprintf(s_err, sizeof(s_err), "connection lost mid-download");
            goto done;
        }
        if (n == 0) {
            break;
        }
        len += n;
    }
    buf[len] = '\0';
    if (len == 0 || buf[0] != '{' || len >= NEWS_MAX) {
        snprintf(s_err, sizeof(s_err), len >= NEWS_MAX ? "news too large" : "not a news document");
        goto done;
    }

    free(s_doc);
    s_doc = buf;
    s_len = len;
    buf = NULL;
    s_fetched_us = esp_timer_get_time();
    s_err[0] = '\0';
    ok = true;
    ESP_LOGI(TAG, "%u bytes of news from the bridge", (unsigned)len);

done:
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

/*
 * GET /api/news[?refresh=1]
 *   {"bridge":"a.b.c.d:port"|null, "live":bool, "age_s":n|null,
 *    "error":"..."|null, "doc":{...the bridge's document...}|null}
 */
static void do_news(httpd_req_t *r, char *buf)
{
    char q[4] = "";
    bool force = web_param(r, "refresh", q, sizeof(q)) && q[0] == '1';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool stale = s_fetched_us < 0 || now - s_fetched_us > NEWS_TTL_S * 1000000LL;
    bool may   = s_tried_us < 0 || now - s_tried_us > REFRESH_MIN_S * 1000000LL;
    bool live  = false;
    if (s_have_bridge && (stale || force) && may) {
        s_tried_us = now;
        live = fetch();
    } else if (!stale) {
        live = true;                     /* fresh enough: still counts as live */
    }
    if (!s_have_bridge) {
        snprintf(s_err, sizeof(s_err), "waiting for Desk-Disp to say where its PC bridge is");
    }

    char bridge[32] = "null", err[112] = "null", age[16] = "null";
    if (s_have_bridge) {
        snprintf(bridge, sizeof(bridge), "\"%u.%u.%u.%u:%u\"",
                 s_ip[0], s_ip[1], s_ip[2], s_ip[3], s_port);
    }
    if (s_err[0]) {
        char esc[96];
        web_json_esc(esc, sizeof(esc), s_err);
        snprintf(err, sizeof(err), "\"%s\"", esc);
    }
    if (s_fetched_us >= 0) {
        snprintf(age, sizeof(age), "%lld", (long long)((esp_timer_get_time() - s_fetched_us) / 1000000));
    }

    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    int o = snprintf(buf, WEB_BUF_SZ, "{\"bridge\":%s,\"live\":%s,\"age_s\":%s,\"error\":%s,\"doc\":",
                     bridge, live ? "true" : "false", age, err);
    esp_err_t e = httpd_resp_send_chunk(r, buf, o);
    if (e == ESP_OK) {
        e = s_doc ? httpd_resp_send_chunk(r, s_doc, s_len) : httpd_resp_send_chunk(r, "null", 4);
    }
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) {
        httpd_resp_send_chunk(r, "}", 1);
        httpd_resp_send_chunk(r, NULL, 0);
    } else {
        web_drop_connection(r);
    }
}

static esp_err_t h_news(httpd_req_t *r)
{
    return web_hand_off(r, do_news);
}

static esp_err_t h_page(httpd_req_t *r)
{
    return web_send_page(r, news_html_start, news_html_end);
}

esp_err_t news_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t blob[6];
        size_t n = sizeof(blob);
        if (nvs_get_blob(h, "bridge", blob, &n) == ESP_OK && n == sizeof(blob)) {
            memcpy(s_ip, blob, 4);
            s_port = (uint16_t)(blob[4] << 8 | blob[5]);
            s_have_bridge = true;
        }
        nvs_close(h);
    }
    return ESP_OK;
}

void news_routes(httpd_handle_t srv)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/news",     .method = HTTP_GET, .handler = h_page },
        { .uri = "/api/news", .method = HTTP_GET, .handler = h_news },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(srv, &routes[i]);
    }
}
