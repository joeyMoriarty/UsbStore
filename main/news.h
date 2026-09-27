/*
 * News: the full stories behind Desk-Disp's scrolling headlines, for the
 * web app's News page.
 *
 * The stories come from Desk-Disp's PC bridge (/api/news.json), which does
 * the RSS work. Desk-Disp tells us where the bridge is in its HELLO - the
 * PC's address changes with DHCP, and Desk-Disp already tracks it. We fetch
 * over plain HTTP (the PC is wired, so the WiFi's client isolation doesn't
 * get in the way), keep the last copy in RAM, and serve it with its age, so
 * the page still shows something while the PC is off.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t news_start(void);
void      news_routes(httpd_handle_t srv);

/* From the ESP-NOW link: the bridge is at a.b.c.d:port. */
void news_set_bridge(const uint8_t ip[4], uint16_t port);
