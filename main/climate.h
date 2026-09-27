/*
 * Climate log: the room, the weather outside, and the sun and moon - all
 * sent by Desk-Disp (its own AHT10, plus what its PC bridge fetches and
 * computes), kept on the system drive for 92 days, and served as day /
 * week / month chart data.
 *
 * On the drive, under usbstore/climate/ - one small CSV per UTC day:
 *   raw/YYYYMMDD.csv            room      "epoch,temp,hum"
 *   hourly/YYYYMMDD.csv         room, rolled up once the day is over
 *                               "hour_epoch,n,tmin,tavg,tmax,hmin,havg,hmax"
 *   outdoor/raw|hourly/...      "epoch,temp,hum,feels,cloud,wind,rain,pressure"
 *   sky/raw|hourly/...          "epoch,sun_alt,sun_az,moon_alt,moon_az,illum"
 *   days/YYYYMM.csv             one line per local day
 *                               "ymd,sunrise,sunset,moonrise,moonset,illum,phase"
 *   forecast/YYYYMMDD.csv       each new forecast "epoch,ymd,code,hi,lo,pop"
 *
 * The room files are where they always were, so older logs carry on. Days
 * are UTC days; the browser shows local time. Hourly rollups derive purely
 * from raw, so they can always be rebuilt.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t climate_start(void);
void      climate_routes(httpd_handle_t srv);

/* Take one room reading, from HTTP or the ESP-NOW link. Readings closer than
 * 20 s to the last stored one update "latest" but aren't logged. */
typedef enum {
    CLIMATE_STORED,
    CLIMATE_TOO_SOON,
    CLIMATE_OUT_OF_RANGE,
    CLIMATE_NO_CLOCK,
} climate_result_t;
climate_result_t climate_ingest(double t, double h);

/* Most recent room reading; false if none has arrived since boot. */
bool climate_latest(float *temp, float *hum, uint32_t *age_s);

/* ---- sky and weather, from Desk-Disp's bridge ---- */

typedef struct {
    uint32_t at;                  /* when the bridge computed it */
    uint32_t ymd;                 /* local date the rise/set times belong to */
    char     place[13];
    float    sun_alt, sun_az;     /* degrees; altitude includes refraction */
    float    moon_alt, moon_az;
    float    illum;               /* % of the disc lit */
    float    phase;               /* 0 new, 0.25 first quarter, 0.5 full, ... */
    uint32_t sunrise, sunset;     /* epoch; 0 if it doesn't happen that day */
    uint32_t moonrise, moonset;
} climate_sky_t;

typedef struct {
    float t, h, feels;            /* degC, %, degC */
    float cloud;                  /* % */
    float wind, wdir;             /* km/h, degrees */
    float rain;                   /* mm in the last hour */
    float pres;                   /* hPa at sea level */
    int   code;                   /* WMO weather code */
} climate_wx_t;

typedef struct {
    uint32_t ymd;
    int      code;
    float    hi, lo;              /* degC */
    float    pop;                 /* % chance of rain */
} climate_fc_t;

#define CLIMATE_FC_MAX 3

/* wx may be NULL (the bridge couldn't reach the weather service). */
void climate_ingest_sky(const climate_sky_t *sky, const climate_wx_t *wx,
                        const climate_fc_t *fc, int nfc);
