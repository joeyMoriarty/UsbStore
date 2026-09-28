#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"

#include "pomodoro.h"
#include "auth.h"
#include "web_server.h"

static const char *TAG = "pomo";

#define NVS_NS       "pomo"
#define CLOCK_VALID  1735689600

typedef struct {
    uint8_t focus_min, short_min, long_min, rounds;
    uint8_t auto_next;         /* go straight into the next phase */
    char    label[40];
} pomo_settings_t;

static pomo_settings_t s_set = { 25, 5, 15, 4, 1, "" };
static pomo_state_t    s_st;
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_s(void) { return (uint32_t)time(NULL); }

static uint32_t phase_len(pomo_phase_t p)
{
    switch (p) {
    case POMO_FOCUS: return s_set.focus_min * 60u;
    case POMO_SHORT: return s_set.short_min * 60u;
    case POMO_LONG:  return s_set.long_min * 60u;
    default:         return 0;
    }
}

static const char *phase_name(pomo_phase_t p)
{
    static const char *n[] = { "idle", "focus", "short", "long", "done" };
    return p <= POMO_DONE ? n[p] : "idle";
}

/* Caller holds s_mux. Enter phase p; running unless auto-continue is off
 * (then it waits, paused, at the full length for a Resume). */
static void enter(pomo_phase_t p, bool run)
{
    s_st.phase  = p;
    s_st.len_s  = phase_len(p);
    s_st.paused = !run;
    s_st.left_s = s_st.len_s;
    s_st.ends_at = run ? now_s() + s_st.len_s : 0;
    s_st.seq++;
}

/* Caller holds s_mux. The phase that follows the current one. */
static void advance(bool run)
{
    switch (s_st.phase) {
    case POMO_FOCUS:
        enter(s_st.round < s_st.rounds ? POMO_SHORT : POMO_LONG, run);
        break;
    case POMO_SHORT:
        s_st.round++;
        enter(POMO_FOCUS, run);
        break;
    case POMO_LONG:
        s_st.phase = POMO_DONE;
        s_st.paused = false;
        s_st.ends_at = 0;
        s_st.left_s = 0;
        s_st.len_s = 0;
        s_st.seq++;
        break;
    default:
        break;
    }
}

void pomodoro_get(pomo_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

static void pomo_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        pomo_phase_t from = POMO_IDLE, to = POMO_IDLE;
        portENTER_CRITICAL(&s_mux);
        if ((s_st.phase == POMO_FOCUS || s_st.phase == POMO_SHORT || s_st.phase == POMO_LONG) &&
            !s_st.paused && now_s() >= s_st.ends_at) {
            from = s_st.phase;
            advance(s_set.auto_next);
            to = s_st.phase;
        }
        portEXIT_CRITICAL(&s_mux);
        if (from != to) {
            ESP_LOGI(TAG, "%s over -> %s", phase_name(from), phase_name(to));
        }
    }
}

/* ------------------------------------------------------------------ http */

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "set", &s_set, sizeof(s_set));
        nvs_commit(h);
        nvs_close(h);
    }
}

static int clamp(double v, int lo, int hi)
{
    int i = (int)v;
    return i < lo ? lo : i > hi ? hi : i;
}

static esp_err_t send_state(httpd_req_t *r)
{
    pomo_state_t st;
    pomodoro_get(&st);
    char lab[96], slab[96];
    web_json_esc(lab, sizeof(lab), st.label);
    web_json_esc(slab, sizeof(slab), s_set.label);
    char body[512];
    snprintf(body, sizeof(body),
             "{\"now\":%lu,\"clock_ok\":%s,"
             "\"state\":{\"seq\":%lu,\"phase\":\"%s\",\"paused\":%s,\"round\":%u,\"rounds\":%u,"
             "\"ends_at\":%lu,\"left_s\":%lu,\"len_s\":%lu,\"label\":\"%s\"},"
             "\"settings\":{\"focus\":%u,\"short\":%u,\"long\":%u,\"rounds\":%u,\"auto\":%s,"
             "\"label\":\"%s\"}}",
             (unsigned long)now_s(), now_s() > CLOCK_VALID ? "true" : "false",
             (unsigned long)st.seq, phase_name(st.phase), st.paused ? "true" : "false",
             st.round, st.rounds, (unsigned long)st.ends_at, (unsigned long)st.left_s,
             (unsigned long)st.len_s, lab,
             s_set.focus_min, s_set.short_min, s_set.long_min, s_set.rounds,
             s_set.auto_next ? "true" : "false", slab);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, body);
}

static esp_err_t h_get(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }
    return send_state(r);
}

/*
 * POST /api/pomodoro  {"action": "start"|"pause"|"resume"|"skip"|"stop",
 *                      "focus":25, "short":5, "long":15, "rounds":4,
 *                      "auto":true, "label":"..."}   (settings only on start)
 */
static esp_err_t h_post(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }
    char body[384], action[12] = "";
    if (!web_small_body(r, body, sizeof(body)) ||
        !web_json_str(body, "action", action, sizeof(action))) {
        return web_fail(r, 400, "need an action");
    }

    if (!strcmp(action, "start")) {
        if (now_s() < CLOCK_VALID) {
            return web_fail(r, 503, "clock not set yet - waiting for internet time");
        }
        double v;
        pomo_settings_t n = s_set;
        if (web_json_num(body, "focus", &v))  n.focus_min = clamp(v, 1, 60);
        if (web_json_num(body, "short", &v))  n.short_min = clamp(v, 1, 30);
        if (web_json_num(body, "long", &v))   n.long_min  = clamp(v, 1, 60);
        if (web_json_num(body, "rounds", &v)) n.rounds    = clamp(v, 1, 8);
        if (strstr(body, "\"auto\":false"))   n.auto_next = 0;
        if (strstr(body, "\"auto\":true"))    n.auto_next = 1;
        if (!web_json_str(body, "label", n.label, sizeof(n.label))) n.label[0] = '\0';
        s_set = n;
        save_settings();

        portENTER_CRITICAL(&s_mux);
        s_st.round  = 1;
        s_st.rounds = s_set.rounds;
        memcpy(s_st.label, s_set.label, sizeof(s_st.label));
        enter(POMO_FOCUS, true);
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "start: %u x %u min focus", s_set.rounds, s_set.focus_min);
    } else {
        bool ok = true;
        portENTER_CRITICAL(&s_mux);
        bool running = s_st.phase == POMO_FOCUS || s_st.phase == POMO_SHORT || s_st.phase == POMO_LONG;
        uint32_t now = now_s();
        if (!strcmp(action, "pause") && running && !s_st.paused) {
            s_st.left_s  = s_st.ends_at > now ? s_st.ends_at - now : 0;
            s_st.paused  = true;
            s_st.ends_at = 0;
            s_st.seq++;
        } else if (!strcmp(action, "resume") && running && s_st.paused) {
            s_st.ends_at = now + s_st.left_s;
            s_st.paused  = false;
            s_st.seq++;
        } else if (!strcmp(action, "skip") && running) {
            advance(true);
        } else if (!strcmp(action, "stop")) {
            s_st.phase   = POMO_IDLE;
            s_st.paused  = false;
            s_st.ends_at = 0;
            s_st.left_s  = 0;
            s_st.len_s   = 0;
            s_st.seq++;
        } else if (strcmp(action, "pause") && strcmp(action, "resume") && strcmp(action, "skip")) {
            ok = false;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!ok) {
            return web_fail(r, 400, "action must be start, pause, resume, skip or stop");
        }
    }
    return send_state(r);
}

esp_err_t pomodoro_start(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        pomo_settings_t n;
        size_t len = sizeof(n);
        if (nvs_get_blob(h, "set", &n, &len) == ESP_OK && len == sizeof(n)) {
            n.label[sizeof(n.label) - 1] = '\0';
            s_set = n;
        }
        nvs_close(h);
    }
    s_st.rounds = s_set.rounds;
    if (xTaskCreatePinnedToCore(pomo_task, "pomo", 2560, NULL, 3, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void pomodoro_routes(httpd_handle_t srv)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/pomodoro", .method = HTTP_GET,  .handler = h_get  },
        { .uri = "/api/pomodoro", .method = HTTP_POST, .handler = h_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(srv, &routes[i]);
    }
}
