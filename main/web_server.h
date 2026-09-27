/*
 * LAN file server over the mounted USB drives, plus the pages built on it
 * (climate, planner).
 *
 * LAN only, deliberately: no TLS and no accounts. Do not port-forward this.
 * Every request that names a path goes through usbstore_path_ok() first, so a
 * URL cannot walk outside a mounted drive.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_server_start(void);

/*
 * ---- Helpers shared with the page modules ------------------------------
 * climate.c, planner.c and sysdrive.c register their own routes but answer
 * through these, so errors, JSON and threading behave the same everywhere.
 */

#define WEB_BUF_SZ 8192

/* JSON error reply; returns ESP_OK (handled) so httpd keeps the socket. */
esp_err_t web_fail(httpd_req_t *r, int code, const char *msg);
esp_err_t web_ok(httpd_req_t *r);                       /* {"ok":true} */

/* One query parameter, percent-decoded. */
bool web_param(httpd_req_t *r, const char *key, char *out, size_t len);

/* Whole small body (< cap) into a NUL-terminated buffer. */
bool web_small_body(httpd_req_t *r, char *body, size_t cap);

/* Escape for a JSON string value. */
void web_json_esc(char *out, size_t n, const char *in);

/* Pull a string / number field out of a FLAT JSON object. Deliberately not
 * a general parser: the firmware never parses anything nested - rich
 * documents stay opaque blobs that only the browser interprets. */
bool web_json_str(const char *json, const char *key, char *out, size_t n);
bool web_json_num(const char *json, const char *key, double *out);

/* Run a slow handler on a transfer worker (core 1) with its own WEB_BUF_SZ
 * buffer, so the web task stays free for the page, status and log. */
typedef void (*web_job_fn)(httpd_req_t *r, char *buf);
esp_err_t web_hand_off(httpd_req_t *r, web_job_fn fn);

/* Close the connection after this response (e.g. a refused upload whose
 * body was never read). */
void web_drop_connection(httpd_req_t *r);

/* Serve an embedded page. */
esp_err_t web_send_page(httpd_req_t *r, const uint8_t *start, const uint8_t *end);
