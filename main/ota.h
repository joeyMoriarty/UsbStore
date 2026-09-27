/*
 * Firmware updates over WiFi.
 *
 * The S3's only USB port is permanently the hub's host port, so without this
 * every update means unplugging the hub, flashing over USB-C, and replugging.
 *
 * Rollback is enabled: a new image boots as "pending verify" and is only
 * confirmed once WiFi and the web server are up. If it crashes or hangs before
 * that, the next reset boots the previous image - so a bad build can never
 * cost you the only way back in.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* POST /api/ota handler: body is the raw firmware .bin. */
esp_err_t ota_http_handler(httpd_req_t *r);

/* Call once the board is reachable again after boot. Confirms a freshly
 * flashed image so the bootloader stops treating it as on probation. */
void ota_confirm_if_pending(void);

/* Running image version string, for the status page. */
const char *ota_running_version(void);
