/*
 * UsbStore - ESP32-S3 Super Mini as a USB mass-storage host and LAN file
 * server. Drives plugged into a powered hub appear at /usb0, /usb1, ... and
 * are browsable from any device on the network.
 *
 * Start order matters: NVS, then logging (so boot lines are buffered), then
 * WiFi (so the setup AP can come up if there are no credentials), then USB,
 * then HTTP. USB is started even when the network never comes up, and a USB
 * failure never stops the web server - the page is the only way in.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "usb_storage.h"
#include "wifi_mgr.h"
#include "web_server.h"
#include "netlog.h"
#include "auth.h"
#include "ota.h"
#include "thermal.h"
#include "sysdrive.h"
#include "climate.h"
#include "devlink.h"
#include "news.h"
#include "pomodoro.h"
#include "deskctl.h"

static const char *TAG = "main";

/*
 * How long a freshly OTA'd image must run before it is confirmed. Long enough
 * to cover USB enumeration of drives already plugged in at boot - the most
 * likely place for a bad build to panic - so that still triggers a rollback.
 */
#define OTA_CONFIRM_AFTER_S 60
#define HEARTBEAT_S         30

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Before WiFi, so boot messages are already buffered by the time anything
     * can read them. Failure here is not fatal - it only costs remote logs. */
    if (netlog_start() != ESP_OK) {
        ESP_LOGE(TAG, "netlog unavailable; logs are UART-only");
    }

    ESP_LOGI(TAG, "UsbStore %s starting, reset reason %d",
             ota_running_version(), (int)esp_reset_reason());

    auth_init();
    sysdrive_init();
    ESP_ERROR_CHECK(wifi_mgr_start());

    if (usbstore_start() != ESP_OK) {
        ESP_LOGE(TAG, "USB host failed to start - check the 5V feed and hub");
    }

    ESP_ERROR_CHECK(web_server_start());

    if (climate_start() != ESP_OK) {
        ESP_LOGE(TAG, "climate logger failed to start");
    }
    deskctl_start();
    if (pomodoro_start() != ESP_OK) {
        ESP_LOGE(TAG, "pomodoro failed to start");
    }
    if (news_start() != ESP_OK) {
        ESP_LOGE(TAG, "news cache failed to start");
    }
    if (devlink_start() != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW link to Desk-Disp failed to start");
    }

    /* Last: its cool-down path uses every subsystem above. A dead sensor is
     * not fatal - the box just runs without thermal protection. */
    if (thermal_start() != ESP_OK) {
        ESP_LOGE(TAG, "thermal monitor failed to start");
    }

    char status[96];
    wifi_mgr_status(status, sizeof(status));
    ESP_LOGI(TAG, "ready: %s", status);

    /* Heartbeat on the log. With the native USB busy hosting the hub, the
     * remote log is the only running view into the box. */
    int uptime = 0;
    bool confirmed = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_S * 1000));
        uptime += HEARTBEAT_S;

        if (!confirmed && uptime >= OTA_CONFIRM_AFTER_S) {
            ota_confirm_if_pending();
            confirmed = true;
        }

        usbstore_drive_t d[USBSTORE_MAX_DRIVES];
        int n = usbstore_list(d, USBSTORE_MAX_DRIVES);
        usbstore_usbdev_t u[USBSTORE_MAX_BUS];
        int nu = usbstore_census(u, USBSTORE_MAX_BUS);
        ESP_LOGI(TAG, "%d USB device(s) on bus, %d drive(s), heap %u, chip %.1f C (%s)",
                 nu, n, (unsigned)esp_get_free_heap_size(),
                 thermal_celsius(), thermal_state_name());
        if (nu == 0) {
            ESP_LOGW(TAG, "nothing on the USB bus: no hub/drive signal is reaching the S3");
        }
        for (int i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  %s  %-6s  \"%s\"  %llu MB",
                     d[i].base, d[i].state, d[i].product,
                     d[i].capacity / (1024ULL * 1024ULL));
        }
    }
}
