#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "wifi_mgr.h"
#include "netlog.h"

static const char *TAG = "wifi";
static const char *NVS_NS = "wifi";

#define GOT_IP   BIT0
#define GAVE_UP  BIT1
#define MAX_TRY  5

#define BACKOFF_MIN_S 2
#define BACKOFF_MAX_S 30

static EventGroupHandle_t  s_events;
static bool                s_station;
static char                s_ip[16] = "0.0.0.0";
static char                s_ssid[33];
static int                 s_tries;
static esp_timer_handle_t  s_retry;
static int                 s_backoff_s = BACKOFF_MIN_S;
static volatile bool       s_paused;    /* WiFi deliberately off (cool-down) */

static void retry_cb(void *arg)
{
    if (!s_paused) {
        esp_wifi_connect();
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_paused) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_paused) {
            return;
        }
        if (s_station) {
            /* Joined once already, so the credentials are right - the network
             * just went away (router reboot, signal). Retry forever with a
             * growing delay; giving up here would leave the box offline until
             * someone power-cycles it. The event handler must not block, so
             * the wait lives in a one-shot timer. */
            ESP_LOGW(TAG, "lost \"%s\", reconnecting in %d s", s_ssid, s_backoff_s);
            esp_timer_start_once(s_retry, (uint64_t)s_backoff_s * 1000000ULL);
            s_backoff_s = s_backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : s_backoff_s * 2;
            return;
        }
        /* First boot: a few quick tries, then fall back to the setup AP -
         * the credentials themselves may be wrong. */
        if (++s_tries <= MAX_TRY) {
            ESP_LOGW(TAG, "retry %d/%d", s_tries, MAX_TRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, GAVE_UP);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        if (s_station) {
            ESP_LOGI(TAG, "reconnected as %s", s_ip);
        }
        s_tries     = 0;
        s_backoff_s = BACKOFF_MIN_S;
        xEventGroupSetBits(s_events, GOT_IP);
    }
}

static bool load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(h, "pass", pass, &pass_len) == ESP_OK &&
              ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

esp_err_t wifi_mgr_provision(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = nvs_set_str(h, "ssid", ssid)) == ESP_OK) {
        err = nvs_set_str(h, "pass", pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void start_setup_ap(void)
{
    ESP_LOGW(TAG, "no usable credentials - raising setup AP \"%s\"",
             WIFI_SETUP_SSID);

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", WIFI_SETUP_SSID);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", WIFI_SETUP_PASS);
    ap.ap.ssid_len       = strlen(WIFI_SETUP_SSID);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_station = false;
    snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    /* Broadcast still reaches anything joined to our own AP, so remote logs
     * work during setup too - which is when they are needed most. */
    netlog_set_network_ready(true);
}

esp_err_t wifi_mgr_start(void)
{
    s_events = xEventGroupCreate();
    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);

        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(sta, WIFI_HOSTNAME);

        /* memcpy, not snprintf: these are fixed-width fields, not C strings.
         * A 32-char SSID or 64-char hex PSK fills them exactly with no NUL,
         * and snprintf would silently drop the last character. */
        wifi_config_t cfg = {0};
        memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
        memcpy(cfg.sta.password, pass, strnlen(pass, sizeof(cfg.sta.password)));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        /* Bounded wait: a retry loop must never stop the drives mounting. */
        EventBits_t bits = xEventGroupWaitBits(
            s_events, GOT_IP | GAVE_UP, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

        if (bits & GOT_IP) {
            s_station = true;
            netlog_set_network_ready(true);
            ESP_LOGI(TAG, "joined \"%s\" as %s", ssid, s_ip);
            wifi_mgr_power_save(false);        /* reachable power saving: see there */

            /* Internet time, for timestamping climate readings. Runs in the
             * background and re-syncs by itself; readings are refused until
             * the clock is set rather than logged as 1970. */
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&sntp) != ESP_OK) {
                ESP_LOGW(TAG, "SNTP did not start - climate logging will wait");
            }
        } else {
            ESP_LOGW(TAG, "could not join \"%s\"", ssid);
            esp_wifi_stop();
            esp_wifi_set_mode(WIFI_MODE_NULL);
            start_setup_ap();
        }
    } else {
        start_setup_ap();
    }

    /* mDNS only helps on a real LAN; in AP mode the address is fixed anyway. */
    if (s_station && mdns_init() == ESP_OK) {
        mdns_hostname_set(WIFI_HOSTNAME);
        mdns_instance_name_set("UsbStore");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "http://%s.local", WIFI_HOSTNAME);
    }
    return ESP_OK;
}

bool wifi_mgr_is_station(void)
{
    return s_station;
}

void wifi_mgr_pause(void)
{
    s_paused = true;
    esp_timer_stop(s_retry);
    esp_wifi_stop();
}

void wifi_mgr_power_save(bool on)
{
    /*
     * MIN_MODEM, always: the radio naps between the router's beacons but
     * wakes for every DTIM beacon, so it still gets the router's buffered
     * broadcasts - including a new device's "who has 192.168.50.194?" ARP,
     * without which a phone can't reach the box at all.
     *
     * Two things are deliberately NOT used, because each left the box deaf
     * to those broadcasts (phones couldn't connect; the PC, which already
     * knew the box's address, carried on fine):
     *  - an ESP-NOW wake window (esp_now_set_wake_window), whose schedule
     *    competed with the DTIM wake-ups;
     *  - MAX_MODEM when throttling, which sleeps through DTIM beacons.
     * WIFI_PS_NONE also works, but keeps the radio on permanently: measured
     * ~10 C hotter (65 C vs ~55 C), five degrees from the throttle point.
     *
     * So `on` (throttling) no longer changes the radio; the CPU clock drop is
     * what cools a throttled box. No effect in setup-AP mode.
     */
    (void)on;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void wifi_mgr_status(char *out, size_t len)
{
    if (s_station) {
        snprintf(out, len, "station:%s:%s", s_ssid, s_ip);
    } else {
        snprintf(out, len, "setup:%s:%s", WIFI_SETUP_SSID, s_ip);
    }
}
