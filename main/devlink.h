/*
 * Device link: Desk-Disp <-> UsbStore over ESP-NOW.
 *
 * Why not plain HTTP: on some networks (campus, guest, many mesh systems)
 * the access point blocks WiFi clients from reaching each other - "client
 * isolation". Both boards get internet, the wired PC reaches both, but
 * Desk-Disp can't open a socket to UsbStore. ESP-NOW frames go radio to
 * radio on the channel both already share with the AP, so the router never
 * gets a say.
 *
 * Once a minute Desk-Disp broadcasts a HELLO with its room reading. UsbStore
 * logs the reading and answers with the next few planner tasks. Both packets
 * are signed with a key derived from the device key (HMAC-SHA256), and the
 * task list is encrypted, since ESP-NOW frames can be heard by anything in
 * range - WiFi's own encryption doesn't cover them.
 *
 * The task list is kept parsed in RAM and refreshed when the planner saves
 * it, so this minute-by-minute traffic never touches the USB drives (and
 * never triggers a park-and-swap).
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

/* After WiFi has joined. Does nothing in setup-AP mode. */
esp_err_t devlink_start(void);

/* The planner has just replaced upcoming.json at `path`. The caller still
 * holds its drive ref, so this reads it straight away. */
void devlink_upcoming_saved(const char *path);

/* One JSON object for /api/status. */
void devlink_status_json(char *out, size_t n);
