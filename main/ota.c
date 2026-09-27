#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"

#include "auth.h"
#include "ota.h"

static const char *TAG = "ota";

#define OTA_BUF 4096

const char *ota_running_version(void)
{
    return esp_app_get_description()->version;
}

void ota_confirm_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed on %s", running->label);
    }
}

static esp_err_t reply(httpd_req_t *r, const char *status, const char *json)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json);
    return ESP_OK;
}

esp_err_t ota_http_handler(httpd_req_t *r)
{
    /* ESP_FAIL closes the socket rather than draining a ~1 MB image we are
     * refusing anyway. */
    if (!auth_check(r)) {
        return ESP_FAIL;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return reply(r, "500 Internal Server Error",
                     "{\"error\":\"no OTA partition - check partitions.csv\"}");
    }
    if (r->content_len <= 0 || (size_t)r->content_len > target->size) {
        return reply(r, "400 Bad Request",
                     "{\"error\":\"image is empty or larger than the OTA slot\"}");
    }

    ESP_LOGI(TAG, "receiving %d bytes into %s", r->content_len, target->label);

    char *buf = malloc(OTA_BUF);
    if (!buf) {
        return reply(r, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    }

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) {
        free(buf);
        return reply(r, "500 Internal Server Error", "{\"error\":\"ota begin failed\"}");
    }

    int left = r->content_len;
    bool first = true;
    while (left > 0) {
        int got = httpd_req_recv(r, buf, left < OTA_BUF ? left : OTA_BUF);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            err = ESP_FAIL;
            break;
        }
        /* Reject the wrong file up front: every ESP image starts 0xE9. A
         * pendrive photo would otherwise be written in full before failing. */
        if (first) {
            first = false;
            if ((unsigned char)buf[0] != 0xE9) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
        }
        err = esp_ota_write(h, buf, got);
        if (err != ESP_OK) {
            break;
        }
        left -= got;
    }
    free(buf);

    if (err != ESP_OK) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "upload failed: %s", esp_err_to_name(err));
        return reply(r, "400 Bad Request",
                     err == ESP_ERR_INVALID_ARG
                         ? "{\"error\":\"not an ESP32 firmware image\"}"
                         : "{\"error\":\"upload interrupted\"}");
    }

    /* esp_ota_end validates the image, including that it targets this chip. */
    err = esp_ota_end(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(err));
        return reply(r, "400 Bad Request",
                     "{\"error\":\"image failed validation (wrong chip or corrupt)\"}");
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return reply(r, "500 Internal Server Error",
                     "{\"error\":\"could not select new image\"}");
    }

    ESP_LOGI(TAG, "update written to %s, rebooting", target->label);
    reply(r, "200 OK", "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}
