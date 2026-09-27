#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"

#include "auth.h"

static const char *TAG    = "auth";
static const char *NVS_NS = "auth";

/* The expected "Authorization" header value, precomputed once so each
 * request is a single comparison. Empty when no password is set. */
static char s_expected[160];

/*
 * Base64 encoder, local rather than mbedtls: the only thing needed is to
 * build the one header value to compare against, and this avoids tying the
 * build to whichever mbedtls API a given IDF release ships.
 */
static void b64(const char *in, char *out, size_t outlen)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;

    for (size_t i = 0; i < n && o + 5 < outlen; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];

        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

static void rebuild_expected(const char *pass)
{
    if (!pass || !pass[0]) {
        s_expected[0] = '\0';
        return;
    }
    char cred[112];
    snprintf(cred, sizeof(cred), "%s:%s", AUTH_USER, pass);

    char enc[152];
    b64(cred, enc, sizeof(enc));
    snprintf(s_expected, sizeof(s_expected), "Basic %s", enc);
}

/* Constant-time compare, so response timing doesn't leak how much of a
 * guess was right. */
static bool same(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = la ^ lb;
    for (size_t i = 0; i < la && i < lb; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

static bool load(char *pass, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, "pass", pass, &len) == ESP_OK && pass[0];
    nvs_close(h);
    return ok;
}

esp_err_t auth_init(void)
{
    char pass[64] = {0};
    if (load(pass, sizeof(pass))) {
        rebuild_expected(pass);
        ESP_LOGI(TAG, "admin password set; write routes protected");
    } else {
        ESP_LOGW(TAG, "no admin password - write routes are OPEN until one is set");
    }
    return ESP_OK;
}

bool auth_is_set(void)
{
    return s_expected[0] != '\0';
}

esp_err_t auth_set(const char *current, const char *next)
{
    if (!next || strlen(next) < 4 || strlen(next) > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    if (auth_is_set()) {
        char have[64] = {0};
        if (!load(have, sizeof(have)) || !current || !same(current, have)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "pass", next);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        rebuild_expected(next);
        ESP_LOGI(TAG, "admin password updated");
    }
    return err;
}

bool auth_check(httpd_req_t *r)
{
    if (!auth_is_set()) {
        return true;
    }

    char hdr[160] = {0};
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        same(hdr, s_expected)) {
        return true;
    }

    /* The challenge makes the browser show its own login prompt, and it then
     * resends the credentials on every later request to this origin. */
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"UsbStore\"");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"error\":\"password required\"}");
    return false;
}
