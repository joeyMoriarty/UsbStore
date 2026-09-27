#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "nvs.h"

#include "sysdrive.h"
#include "usb_storage.h"
#include "auth.h"
#include "web_server.h"

static const char *TAG    = "sysdrive";
static const char *NVS_NS = "sys";

static char s_serial[40];

esp_err_t sysdrive_init(void)
{
    nvs_handle_t h;
    s_serial[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_serial);
        if (nvs_get_str(h, "serial", s_serial, &len) != ESP_OK) {
            s_serial[0] = '\0';
        }
        nvs_close(h);
    }
    if (s_serial[0]) {
        ESP_LOGI(TAG, "system drive: serial %s", s_serial);
    } else {
        ESP_LOGW(TAG, "no system drive chosen - climate logging and planner are off");
    }
    return ESP_OK;
}

static esp_err_t save_serial(const char *serial)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = serial[0] ? nvs_set_str(h, "serial", serial) : nvs_erase_key(h, "serial");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;                         /* clearing what was never set */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        snprintf(s_serial, sizeof(s_serial), "%s", serial);
    }
    return err;
}

bool sysdrive_serial(char *out, size_t len)
{
    snprintf(out, len, "%s", s_serial);
    return s_serial[0] != '\0';
}

bool sysdrive_path(const char *sub, char *out, size_t len)
{
    char base[16];
    if (!usbstore_base_for_serial(s_serial, base, sizeof(base))) {
        return false;
    }
    int n = snprintf(out, len, "%s/" SYSDRIVE_DIR "%s%s", base,
                     sub && sub[0] ? "/" : "", sub ? sub : "");
    return n > 0 && (size_t)n < len;
}

bool sysdrive_is_protected(const char *path)
{
    /* "/usbN/usbstore" or anything beneath it. Paths reaching here have
     * passed usbstore_path_ok(), so no "//" or "\" spellings to worry about. */
    if (strncmp(path, "/usb", 4) != 0) {
        return false;
    }
    const char *p = strchr(path + 1, '/');
    if (!p) {
        return false;                         /* the drive root itself */
    }
    p++;
    size_t n = strlen(SYSDRIVE_DIR);
    return strncasecmp(p, SYSDRIVE_DIR, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

esp_err_t sysdrive_mkdirs(const char *path)
{
    char tmp[160];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Skip "/usbN": the mount point is not a directory we can create. */
    char *p = strchr(tmp + 1, '/');
    while (p) {
        char *next = strchr(p + 1, '/');
        if (next) {
            *next = '\0';
        }
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                return ESP_FAIL;
            }
        }
        if (next) {
            *next = '/';
        }
        p = next;
    }
    return ESP_OK;
}

/*
 * POST /api/sysdrive?p=/usbN - make that drive the system drive.
 * POST /api/sysdrive?p=      - stop using one (the data stays on the stick).
 */
static esp_err_t h_set(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_OK;
    }
    char path[64] = {0};
    web_param(r, "p", path, sizeof(path));

    char serial[40] = "";
    if (path[0]) {
        if (!usbstore_path_ok(path) || strchr(path + 1, '/')) {
            return web_fail(r, 400, "give a drive root like /usb0");
        }
        if (!usbstore_serial_for_path(path, serial, sizeof(serial))) {
            return web_fail(r, 400, "this drive reports no serial number, so it "
                                    "can't be recognised again after a replug");
        }
    }
    if (save_serial(serial) != ESP_OK) {
        return web_fail(r, 500, "could not save");
    }
    ESP_LOGI(TAG, "system drive %s", serial[0] ? serial : "cleared");
    return web_ok(r);
}

void sysdrive_routes(httpd_handle_t srv)
{
    static const httpd_uri_t set = {
        .uri = "/api/sysdrive", .method = HTTP_POST, .handler = h_set,
    };
    httpd_register_uri_handler(srv, &set);
}
