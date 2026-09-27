/*
 * Admin password for the write side of the web API.
 *
 * Reads (browse, download, status, log) stay open on the LAN. Anything that
 * changes the drive or the firmware - upload, delete, mkdir, OTA, clearing the
 * crash dump - needs HTTP Basic auth as user "admin". With OTA on the table,
 * an open write API would let anyone on the WiFi reflash the board.
 *
 * Honest limit: this is plain HTTP, so the password crosses the LAN in the
 * clear. It stops a housemate or a guest device, not someone sniffing traffic.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define AUTH_USER "admin"

esp_err_t auth_init(void);

/* True once a password has been set. Until then write routes are open, and
 * the UI nags you to set one. */
bool auth_is_set(void);

/* Set or change the password. `current` must match if one is already set. */
esp_err_t auth_set(const char *current, const char *next);

/* Gate for write routes. Returns true if the request may proceed; otherwise
 * it has already sent a 401 with a Basic challenge, and the handler must
 * return ESP_OK without doing anything else. */
bool auth_check(httpd_req_t *r);
