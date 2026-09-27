/*
 * Chip temperature and thermal protection.
 *
 * Reads the ESP32-S3's built-in sensor, which measures the silicon die - not
 * the board. The board's warmest part is usually the 5 V -> 3.3 V regulator,
 * which this sensor cannot see; it does catch the chip itself overheating.
 *
 *   normal  -> warm      at THERM_WARM_C: throttle but stay online
 *                        (CPU down to 80 MHz, WiFi radio power saving)
 *   warm    -> normal    below THERM_CALM_C (hysteresis, no flapping)
 *   any     -> cooling   at THERM_HOT_C: stop transfers, unmount every
 *                        drive, suspend USB, WiFi off, light sleep in
 *                        rounds until below THERM_RESUME_C, then reboot
 *
 * The cool-down ends in a reboot rather than a resume: USB and WiFi both
 * come back cleanly from a restart, which cannot be said for every state
 * they might be in after sleep. A record in RTC memory survives the restart,
 * so the web page can say what happened.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define THERM_WARM_C   70.0f
#define THERM_CALM_C   62.0f
#define THERM_HOT_C    85.0f
#define THERM_RESUME_C 60.0f

typedef enum {
    THERM_NORMAL,
    THERM_WARM,
    THERM_COOLING,
} therm_state_t;

typedef struct {
    bool     valid;       /* a cool-down happened before this boot */
    bool     was_test;    /* triggered from the web page, not by heat */
    float    peak_c;      /* hottest reading before it slept */
    float    end_c;       /* reading when it woke for good */
    uint32_t slept_s;     /* total time in light sleep */
} therm_cooldown_t;

esp_err_t thermal_start(void);

/* Latest die temperature; NAN until the first reading or if the sensor failed. */
float thermal_celsius(void);
float thermal_peak_celsius(void);      /* hottest since boot */

therm_state_t thermal_state(void);
const char   *thermal_state_name(void);

/* True once a cool-down has begun: transfers check this and stop. */
bool thermal_cooling(void);

/* Run the cool-down sequence now, for a short fixed time, regardless of
 * temperature - proves the sleep path works without heating the board. */
void thermal_request_test(void);

/* What the previous cool-down (if any) recorded. */
therm_cooldown_t thermal_last_cooldown(void);
