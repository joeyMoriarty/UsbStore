/*
 * Desk-Disp's modes, from the web app: which zone the clock shows, clock or
 * sky view, and what the OLED shows.
 *
 * Two copies of the state live here: what Desk-Disp last REPORTED (it sends
 * its current modes in every poll, so a button press on the desk shows up
 * too) and what the web app last ASKED FOR, with a sequence number. Desk-
 * Disp applies a request only when the sequence number changes, so it never
 * undoes a button press - it only acts on something new from the web.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Packed into one byte on the link. */
#define DESK_ZONE_NL   0x01        /* else India */
#define DESK_VIEW_SKY  0x02        /* else the clock */
#define DESK_OLED_MASK 0x0C        /* 0 follow the clock, 1 room + Nifty, 2 headlines */
#define DESK_OLED_SHIFT 2

typedef struct {
    uint32_t seq;                  /* 0 = nothing asked for yet */
    uint8_t  bits;
} desk_want_t;

esp_err_t deskctl_start(void);
void      deskctl_routes(httpd_handle_t srv);

/* From the link: Desk-Disp's modes as it reports them. */
void        deskctl_reported(uint8_t bits);
desk_want_t deskctl_wanted(void);
