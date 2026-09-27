/*
 * Remote logging, for when there is no serial adapter on the bench.
 *
 * Two independent sinks, because neither alone is enough:
 *
 *   1. A RAM ring buffer, readable over HTTP (/api/log) and broadcast over
 *      UDP. This covers everything that happens once WiFi is up.
 *   2. Core dumps to flash, read back over HTTP after the reboot
 *      (/api/crash). This covers panics, which kill the network stack before
 *      anything can be transmitted - so they never appear in sink 1.
 *
 * The UART path is left installed underneath both. It costs nothing with
 * nothing attached, and it is the only output that survives a panic live.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define NETLOG_UDP_PORT 9999

/* Install the log hook. Call as early as possible - before WiFi - so boot
 * messages are already in the ring by the time anything can read it. */
esp_err_t netlog_start(void);

/* Let the UDP sender know it has a network. Called by wifi_mgr. */
void netlog_set_network_ready(bool ready);

/*
 * Copy log bytes newer than `from` into `out`.
 *
 * `from` is a byte count since boot, not an index: pass 0 to get everything
 * still buffered, then pass back the returned `*next` to tail it. A caller
 * that falls further behind than the ring is silently fast-forwarded.
 */
size_t netlog_read(uint64_t from, char *out, size_t max, uint64_t *next);

/* Total bytes ever logged, i.e. the newest cursor value. */
uint64_t netlog_cursor(void);
