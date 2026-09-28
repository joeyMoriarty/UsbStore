/*
 * Pomodoro: focus / break timer, run by the box and shown on Desk-Disp's
 * clock. Started, paused and set up from the Planner page's Focus tab.
 *
 * The box owns the state and moves between phases on its own - focus,
 * short break, focus, ..., long break after the last round - so it keeps
 * time with the web page closed. Desk-Disp asks for the state every few
 * seconds over the ESP-NOW link and counts down locally from ends_at in
 * between. Settings are remembered in NVS; the running state lives in RAM.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef enum {
    POMO_IDLE = 0,
    POMO_FOCUS,
    POMO_SHORT,          /* short break */
    POMO_LONG,           /* long break, after the last round */
    POMO_DONE,           /* the set is finished */
} pomo_phase_t;

typedef struct {
    uint32_t     seq;          /* bumps on every change, so a display can react */
    pomo_phase_t phase;
    bool         paused;
    uint8_t      round;        /* the focus round, 1-based */
    uint8_t      rounds;       /* focus rounds per set */
    uint32_t     ends_at;      /* epoch; valid while running */
    uint32_t     left_s;       /* valid while paused */
    uint32_t     len_s;        /* the whole phase, for progress */
    char         label[40];
} pomo_state_t;

esp_err_t pomodoro_start(void);
void      pomodoro_routes(httpd_handle_t srv);
void      pomodoro_get(pomo_state_t *out);
