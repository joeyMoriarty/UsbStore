#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "deskctl.h"
#include "auth.h"
#include "web_server.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static desk_want_t  s_want;
static bool         s_asked;             /* the web has asked for something since boot */
static uint8_t      s_have;              /* as last reported */
static int64_t      s_have_us = -1;      /* when */

void deskctl_reported(uint8_t bits)
{
    portENTER_CRITICAL(&s_mux);
    s_have    = bits;
    s_have_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
}

desk_want_t deskctl_wanted(void)
{
    portENTER_CRITICAL(&s_mux);
    desk_want_t w = s_want;
    if (!s_asked) {
        w.seq = 0;                       /* nothing to do - not "go back to defaults" */
    }
    portEXIT_CRITICAL(&s_mux);
    return w;
}

static esp_err_t send_state(httpd_req_t *r)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t have = s_have;
    int64_t at   = s_have_us;
    portEXIT_CRITICAL(&s_mux);
    static const char *oled[] = { "auto", "room", "news", "auto" };
    char body[200];
    if (at < 0) {
        snprintf(body, sizeof(body), "{\"seen\":false}");
    } else {
        snprintf(body, sizeof(body),
                 "{\"seen\":true,\"age_s\":%lld,\"zone\":\"%s\",\"view\":\"%s\",\"oled\":\"%s\"}",
                 (long long)((esp_timer_get_time() - at) / 1000000),
                 (have & DESK_ZONE_NL) ? "nl" : "ist",
                 (have & DESK_VIEW_SKY) ? "sky" : "clock",
                 oled[(have & DESK_OLED_MASK) >> DESK_OLED_SHIFT]);
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, body);
}

static esp_err_t h_get(httpd_req_t *r)
{
    return send_state(r);            /* modes aren't private */
}

/*
 * POST /api/desk  {"zone":"ist"|"nl", "view":"clock"|"sky", "oled":"auto"|"room"|"news"}
 * Any subset: the rest stay as Desk-Disp last reported them.
 */
static esp_err_t h_post(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }
    char body[160], v[8];
    if (!web_small_body(r, body, sizeof(body))) {
        return web_fail(r, 400, "bad body");
    }
    portENTER_CRITICAL(&s_mux);
    uint8_t bits = s_have;
    portEXIT_CRITICAL(&s_mux);

    bool any = false;
    if (web_json_str(body, "zone", v, sizeof(v))) {
        bits = !strcmp(v, "nl") ? (bits | DESK_ZONE_NL) : (bits & ~DESK_ZONE_NL);
        any = true;
    }
    if (web_json_str(body, "view", v, sizeof(v))) {
        bits = !strcmp(v, "sky") ? (bits | DESK_VIEW_SKY) : (bits & ~DESK_VIEW_SKY);
        any = true;
    }
    if (web_json_str(body, "oled", v, sizeof(v))) {
        uint8_t o = !strcmp(v, "room") ? 1 : !strcmp(v, "news") ? 2 : 0;
        bits = (bits & ~DESK_OLED_MASK) | (o << DESK_OLED_SHIFT);
        any = true;
    }
    if (!any) {
        return web_fail(r, 400, "give zone, view or oled");
    }
    portENTER_CRITICAL(&s_mux);
    s_want.bits = bits;
    s_want.seq++;
    if (s_want.seq == 0) {
        s_want.seq = 1;
    }
    s_asked = true;
    portEXIT_CRITICAL(&s_mux);
    return web_ok(r);
}

esp_err_t deskctl_start(void)
{
    /* A random start, so a request numbered before a reboot can't collide
     * with one numbered after it - Desk-Disp acts on any change of number. */
    s_want.seq  = esp_random() | 1;
    s_want.bits = 0;
    return ESP_OK;
}

void deskctl_routes(httpd_handle_t srv)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/desk", .method = HTTP_GET,  .handler = h_get  },
        { .uri = "/api/desk", .method = HTTP_POST, .handler = h_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(srv, &routes[i]);
    }
}
