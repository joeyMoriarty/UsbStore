#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_pm.h"
#include "driver/temperature_sensor.h"
#include "usb/usb_host.h"

#include "thermal.h"
#include "usb_storage.h"
#include "wifi_mgr.h"

static const char *TAG = "thermal";

#define POLL_MS          5000
#define SLEEP_ROUND_S    15       /* one light-sleep round; measured between */
#define TEST_SLEEP_S     30       /* how long the web page's test sleeps */
#define MAX_COOL_S       1800     /* give up waiting and reboot anyway */
#define THROTTLED_MHZ    80
#define BOOST_MHZ        240      /* the S3's clock steps are 80, 160, 240 */
#define BOOST_MAX_C      65.0f    /* no bursts from a chip this warm */
#define BOOST_MAX_S      20       /* longest burst... */
#define BOOST_REST_S     20       /* ...then at least this long at the normal clock */
#define REC_MAGIC        0x5EA7C001u

/*
 * RTC_NOINIT memory survives esp_restart() but is garbage after power-on,
 * hence the magic number - and it is only trusted after a software reset.
 */
typedef struct {
    uint32_t magic;
    float    peak_c;
    float    end_c;
    uint32_t slept_s;
    uint32_t was_test;
} rec_t;
static RTC_NOINIT_ATTR rec_t s_rec;

static temperature_sensor_handle_t s_tsens;
static TaskHandle_t                s_task;
static volatile float              s_temp = NAN;
static volatile float              s_peak = NAN;
static volatile therm_state_t      s_state = THERM_NORMAL;
static therm_cooldown_t            s_last;

/* ---------------------------------------------------------------- reading */

static bool read_celsius(float *out)
{
    if (!s_tsens) {
        return false;
    }
    float t;
    if (temperature_sensor_get_celsius(s_tsens, &t) != ESP_OK) {
        /* Seen after light sleep on some parts: re-enable and try once more. */
        temperature_sensor_disable(s_tsens);
        temperature_sensor_enable(s_tsens);
        if (temperature_sensor_get_celsius(s_tsens, &t) != ESP_OK) {
            return false;
        }
    }
    *out = t;
    return true;
}

static void sample(void)
{
    float t;
    if (read_celsius(&t)) {
        s_temp = t;
        if (isnan(s_peak) || t > s_peak) {
            s_peak = t;
        }
    }
}

/* ------------------------------------------------------------- throttling */

static void set_cpu_mhz(int mhz)
{
#if CONFIG_PM_ENABLE
    /* min == max pins the clock: no dynamic scaling, and light_sleep_enable
     * stays false, so nothing sleeps behind the USB stack's back. */
    const esp_pm_config_t pm = {
        .max_freq_mhz       = mhz,
        .min_freq_mhz       = mhz,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CPU clock change to %d MHz failed: %s", mhz, esp_err_to_name(err));
    }
#else
    (void)mhz;
#endif
}

/*
 * Bursts: while there's heavy work, the clock goes up to 240 MHz - but only
 * as a burst, and only from a cool chip. A burst ends after BOOST_MAX_S even
 * if the work hasn't, and the next can't start for BOOST_REST_S, so a long
 * busy spell averages well under the full-speed heat. Throttling always wins.
 *
 * Every clock decision goes through apply_clock(), under one lock, so a burst
 * starting on a web worker can never undo a throttle from this task.
 */
static SemaphoreHandle_t s_clk_lock;
static portMUX_TYPE      s_boost_mux = portMUX_INITIALIZER_UNLOCKED;
static int               s_boost_refs;           /* heavy jobs running now */
static int               s_cur_mhz;
static int64_t           s_burst_since = -1;     /* us; -1 = not bursting */
static int64_t           s_rest_until;           /* us */
static uint32_t          s_bursts;

static void apply_clock(void)
{
    if (!s_clk_lock) {
        return;
    }
    xSemaphoreTake(s_clk_lock, portMAX_DELAY);
    if (s_state == THERM_COOLING) {              /* cooldown() owns everything */
        xSemaphoreGive(s_clk_lock);
        return;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_boost_mux);
    int refs = s_boost_refs;
    portEXIT_CRITICAL(&s_boost_mux);

    int want = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    if (s_state == THERM_WARM) {
        want = THROTTLED_MHZ;
    } else if (refs > 0 && !isnan(s_temp) && s_temp < BOOST_MAX_C && now >= s_rest_until &&
               (s_burst_since < 0 || now - s_burst_since < BOOST_MAX_S * 1000000LL)) {
        want = BOOST_MHZ;
    }

    if (want == BOOST_MHZ && s_burst_since < 0) {
        s_burst_since = now;
        s_bursts++;
    } else if (want != BOOST_MHZ && s_burst_since >= 0) {
        if (now - s_burst_since >= BOOST_MAX_S * 1000000LL) {
            s_rest_until = now + BOOST_REST_S * 1000000LL;   /* used it all: rest */
        }
        s_burst_since = -1;
    }
    if (want != s_cur_mhz) {
        set_cpu_mhz(want);
        s_cur_mhz = want;
    }
    xSemaphoreGive(s_clk_lock);
}

void thermal_boost_begin(void)
{
    portENTER_CRITICAL(&s_boost_mux);
    s_boost_refs++;
    portEXIT_CRITICAL(&s_boost_mux);
    apply_clock();
}

void thermal_boost_end(void)
{
    portENTER_CRITICAL(&s_boost_mux);
    if (s_boost_refs > 0) {
        s_boost_refs--;
    }
    portEXIT_CRITICAL(&s_boost_mux);
    apply_clock();
}

int      thermal_cpu_mhz(void) { return s_cur_mhz; }
uint32_t thermal_bursts(void)  { return s_bursts; }

static void throttle(bool on)
{
    apply_clock();                    /* s_state already says which */
    wifi_mgr_power_save(on);
    ESP_LOGW(TAG, "%.1f C: %s", s_temp,
             on ? "throttling (CPU 80 MHz, WiFi power save)" : "back to full speed");
}

/* --------------------------------------------------------------- cool-down */

static void cooldown(bool test)
{
    s_state = THERM_COOLING;          /* transfers see this and stop */
    float peak = isnan(s_peak) ? s_temp : s_peak;
    ESP_LOGW(TAG, "%s at %.1f C: unmounting drives, suspending USB, WiFi off",
             test ? "cool-down TEST" : "OVERHEAT", s_temp);

    /* Unmount first, so a power cut during the sleep can't leave FAT
     * half-written. Transfers abort within one chunk once cooling is set,
     * so the lock frees quickly. */
    int parked = usbstore_park_all(15000);
    if (parked < 0) {
        ESP_LOGE(TAG, "a transfer would not stop; sleeping without unmounting");
    }

    /* Suspend the bus: drives drop to their <2.5 mA suspend current and the
     * USB controller goes quiet before its clock is gated. Asynchronous -
     * the USB host task carries it out - so give it a moment. */
    esp_err_t e = usb_host_lib_root_port_suspend();
    if (e != ESP_OK && e != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "USB suspend: %s", esp_err_to_name(e));
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    /* The radio is the biggest heat source; off entirely. */
    wifi_mgr_pause();
    vTaskDelay(pdMS_TO_TICKS(200));

    uint32_t slept = 0;
    while (1) {
        esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_ROUND_S * 1000000ULL);
        if (esp_light_sleep_start() != ESP_OK) {
            /* Rejected (a wake source was already pending): stay awake but
             * idle for the round instead, which still sheds most of the load. */
            vTaskDelay(pdMS_TO_TICKS(SLEEP_ROUND_S * 1000));
        }
        slept += SLEEP_ROUND_S;
        sample();
        ESP_LOGI(TAG, "after %lu s asleep: %.1f C", (unsigned long)slept, s_temp);

        if (test ? slept >= TEST_SLEEP_S
                 : (!isnan(s_temp) && s_temp <= THERM_RESUME_C)) {
            break;
        }
        if (slept >= MAX_COOL_S) {
            ESP_LOGE(TAG, "still hot after %d s - rebooting anyway", MAX_COOL_S);
            break;
        }
    }

    s_rec = (rec_t){
        .magic    = REC_MAGIC,
        .peak_c   = peak,
        .end_c    = s_temp,
        .slept_s  = slept,
        .was_test = test,
    };
    ESP_LOGW(TAG, "cool-down done after %lu s, restarting", (unsigned long)slept);
    esp_restart();
}

/* ------------------------------------------------------------------ task */

static void thermal_task(void *arg)
{
    while (1) {
        /* A test request wakes this early via a task notification. */
        /* Wake sooner mid-burst, so one that runs out is ended on time. */
        uint32_t wait = s_burst_since >= 0 ? 1000 : POLL_MS;
        bool test = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait)) > 0;
        sample();
        if (isnan(s_temp) && !test) {
            continue;
        }

        if (test || s_temp >= THERM_HOT_C) {
            cooldown(test);                    /* does not return */
        }
        if (s_state == THERM_NORMAL && s_temp >= THERM_WARM_C) {
            s_state = THERM_WARM;
            throttle(true);
        } else if (s_state == THERM_WARM && s_temp <= THERM_CALM_C) {
            s_state = THERM_NORMAL;
            throttle(false);
        } else {
            apply_clock();            /* burst ran out, or the chip warmed past 65 C */
        }
    }
}

/* ------------------------------------------------------------------ API */

esp_err_t thermal_start(void)
{
    /* Pick up a record from a cool-down that ended in esp_restart(). */
    if (esp_reset_reason() == ESP_RST_SW && s_rec.magic == REC_MAGIC) {
        s_last = (therm_cooldown_t){
            .valid    = true,
            .was_test = s_rec.was_test,
            .peak_c   = s_rec.peak_c,
            .end_c    = s_rec.end_c,
            .slept_s  = s_rec.slept_s,
        };
        ESP_LOGW(TAG, "previous boot ended in a %s cool-down: peak %.1f C, %lu s asleep",
                 s_last.was_test ? "test" : "thermal", s_last.peak_c,
                 (unsigned long)s_last.slept_s);
    }
    s_rec.magic = 0;          /* report it once, not after the next reset too */

    /* With CONFIG_PM_ENABLE the idle CPU clock could otherwise drop below the
     * default; pin it until throttling says otherwise. */
    s_clk_lock = xSemaphoreCreateMutex();
    set_cpu_mhz(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    s_cur_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    /* 20-100 C is the sensor range with the best accuracy that still covers
     * every threshold above. */
    const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_tsens);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temperature sensor unavailable: %s", esp_err_to_name(err));
        s_tsens = NULL;       /* the test cool-down still works without it */
    } else {
        sample();
        ESP_LOGI(TAG, "chip at %.1f C (throttle %.0f, cool-down %.0f)",
                 s_temp, THERM_WARM_C, THERM_HOT_C);
    }

    if (xTaskCreate(thermal_task, "thermal", 4096, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

float thermal_celsius(void)       { return s_temp; }
float thermal_peak_celsius(void)  { return s_peak; }
therm_state_t thermal_state(void) { return s_state; }
bool thermal_cooling(void)        { return s_state == THERM_COOLING; }
therm_cooldown_t thermal_last_cooldown(void) { return s_last; }

const char *thermal_state_name(void)
{
    switch (s_state) {
    case THERM_WARM:    return "warm";
    case THERM_COOLING: return "cooling";
    default:            return "normal";
    }
}

void thermal_request_test(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}
