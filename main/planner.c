#include <string.h>
#include <stdio.h>
#include <sys/unistd.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "planner.h"
#include "sysdrive.h"
#include "usb_storage.h"
#include "auth.h"
#include "web_server.h"
#include "devlink.h"

static const char *TAG = "planner";

extern const uint8_t planner_html_start[] asm("_binary_planner_html_start");
extern const uint8_t planner_html_end[]   asm("_binary_planner_html_end");

#define DOC_MAX (2 * 1024 * 1024)   /* per document; notes rarely pass 100 KB */

static bool doc_name_ok(const char *d)
{
    return !strcmp(d, "notes") || !strcmp(d, "events") ||
           !strcmp(d, "tables") || !strcmp(d, "upcoming");
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/*
 * Crash-safe replace, in three steps: write "<doc>.tmp" completely, move the
 * current file to "<doc>.bak", move .tmp into place. FAT has no atomic
 * rename-over, so there is a moment with no <doc> at all - this puts the
 * house back in order on the next access:
 *   doc missing, .tmp present  -> crash between the renames; .tmp is the new,
 *                                 complete version (it was closed first)
 *   doc missing, .bak only     -> fall back to the previous version
 *   doc present, .tmp present  -> crash before committing; the save was never
 *                                 acknowledged, so discard it
 */
static void recover(const char *path)
{
    char tmp[128], bak[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    if (!exists(path)) {
        if (exists(tmp)) {
            rename(tmp, path);
            ESP_LOGW(TAG, "recovered %s from an interrupted save", path);
        } else if (exists(bak)) {
            rename(bak, path);
            ESP_LOGW(TAG, "restored %s from its backup", path);
        }
    } else if (exists(tmp)) {
        unlink(tmp);
    }
}

/* FNV-1a over the file: the version tag a browser must quote to save, so
 * two tabs editing at once can't silently overwrite each other. */
static bool file_etag(const char *path, char *buf, char out[12])
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint32_t h = 2166136261u;
    size_t n;
    while ((n = fread(buf, 1, WEB_BUF_SZ, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            h = (h ^ (uint8_t)buf[i]) * 16777619u;
        }
    }
    fclose(f);
    snprintf(out, 12, "%08lx", (unsigned long)h);
    return true;
}

/* Resolve ?doc= to its path on the system drive and take a ref on it. */
static bool open_doc(httpd_req_t *r, char *doc, size_t dlen, char *path, size_t plen,
                     usbstore_ref_t *ref)
{
    if (!web_param(r, "doc", doc, dlen) || !doc_name_ok(doc)) {
        web_fail(r, 400, "doc must be notes, events, tables or upcoming");
        return false;
    }
    char sub[48];
    snprintf(sub, sizeof(sub), "planner/%s.json", doc);
    if (!sysdrive_path(sub, path, plen)) {
        web_fail(r, 503, "no system drive - choose one on the Files page");
        return false;
    }
    char why[112];
    if (usbstore_acquire(path, why, sizeof(why), ref) != ESP_OK) {
        web_fail(r, 503, why);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------- get */

static void do_get(httpd_req_t *r, char *buf)
{
    char doc[16], path[112], etag[12];
    usbstore_ref_t ref;
    if (!open_doc(r, doc, sizeof(doc), path, sizeof(path), &ref)) {
        return;
    }
    recover(path);

    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (!file_etag(path, buf, etag)) {
        usbstore_release(ref);
        /* Never saved yet: "null" tells the page to start empty. */
        httpd_resp_set_hdr(r, "ETag", "none");
        httpd_resp_sendstr(r, "null");
        return;
    }
    httpd_resp_set_hdr(r, "ETag", etag);   /* etag outlives the response */

    FILE *f = fopen(path, "rb");
    if (!f) {
        usbstore_release(ref);
        web_fail(r, 500, "could not open document");
        return;
    }
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, WEB_BUF_SZ, f)) > 0) {
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) {
            ok = false;
            break;
        }
    }
    fclose(f);
    usbstore_release(ref);
    if (ok) {
        httpd_resp_send_chunk(r, NULL, 0);
    } else {
        web_drop_connection(r);
    }
}

static esp_err_t h_get(httpd_req_t *r)
{
    /* Notes, events and tables are private. The derived upcoming list is
     * also readable with the device key, for Desk-Disp. */
    char doc[16] = {0};
    web_param(r, "doc", doc, sizeof(doc));
    size_t has_key = httpd_req_get_hdr_value_len(r, "X-Device-Key");
    if (!strcmp(doc, "upcoming") && has_key) {
        if (!auth_device_check(r)) {
            return ESP_OK;
        }
    } else if (!auth_check(r)) {
        return ESP_OK;
    }
    return web_hand_off(r, do_get);
}

/* ------------------------------------------------------------------- put */

static void do_put(httpd_req_t *r, char *buf)
{
    char doc[16], path[112], cur[12] = "none", want[16] = {0};
    if (httpd_req_get_hdr_value_str(r, "If-Match", want, sizeof(want)) != ESP_OK) {
        web_fail(r, 428, "If-Match required - reload and try again");
        web_drop_connection(r);
        return;
    }
    if (r->content_len <= 0 || r->content_len > DOC_MAX) {
        web_fail(r, 413, "document empty or over 2 MB");
        web_drop_connection(r);
        return;
    }
    usbstore_ref_t ref;
    if (!open_doc(r, doc, sizeof(doc), path, sizeof(path), &ref)) {
        web_drop_connection(r);
        return;
    }
    recover(path);
    file_etag(path, buf, cur);
    if (strcmp(cur, want) != 0) {
        usbstore_release(ref);
        char msg[80];
        snprintf(msg, sizeof(msg), "{\"error\":\"changed elsewhere - reload\",\"etag\":\"%s\"}", cur);
        httpd_resp_set_status(r, "409 Conflict");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_sendstr(r, msg);
        web_drop_connection(r);
        return;
    }

    char dir[112];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
    }
    sysdrive_mkdirs(dir);

    char tmp[128], bak[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        usbstore_release(ref);
        web_fail(r, 500, "cannot write to the system drive");
        web_drop_connection(r);
        return;
    }

    uint32_t h = 2166136261u;
    int  left  = r->content_len;
    bool ok    = true;
    bool first = true;
    while (left > 0) {
        int got = httpd_req_recv(r, buf, left < WEB_BUF_SZ ? left : WEB_BUF_SZ);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            ok = false;
            break;
        }
        if (first) {
            /* Cheap sanity check that it's a JSON document at all. */
            int i = 0;
            while (i < got && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t')) i++;
            if (i < got && buf[i] != '{' && buf[i] != '[') {
                ok = false;
                break;
            }
            first = false;
        }
        for (int i = 0; i < got; i++) {
            h = (h ^ (uint8_t)buf[i]) * 16777619u;
        }
        if (fwrite(buf, 1, got, f) != (size_t)got) {
            ok = false;
            break;
        }
        left -= got;
    }
    if (fclose(f) != 0) {
        ok = false;
    }

    if (ok) {
        unlink(bak);
        if (exists(path) && rename(path, bak) != 0) {
            ok = false;
        } else if (rename(tmp, path) != 0) {
            ok = false;
            rename(bak, path);                 /* put the old one back */
        }
    }
    if (!ok) {
        unlink(tmp);
    } else if (!strcmp(doc, "upcoming")) {
        devlink_upcoming_saved(path);          /* Desk-Disp's copy, while the drive's open */
    }
    usbstore_release(ref);

    if (!ok) {
        web_fail(r, 500, "save failed - drive full, removed, or not JSON");
        web_drop_connection(r);
        return;
    }
    char reply[48];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"etag\":\"%08lx\"}", (unsigned long)h);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, reply);
}

static esp_err_t h_put(httpd_req_t *r)
{
    if (!auth_check(r)) {
        return ESP_FAIL;          /* close rather than drain the body */
    }
    return web_hand_off(r, do_put);
}

static esp_err_t h_page(httpd_req_t *r)
{
    return web_send_page(r, planner_html_start, planner_html_end);
}

void planner_routes(httpd_handle_t srv)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/planner",     .method = HTTP_GET,  .handler = h_page },
        { .uri = "/api/planner", .method = HTTP_GET,  .handler = h_get  },
        { .uri = "/api/planner", .method = HTTP_POST, .handler = h_put  },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(srv, &routes[i]);
    }
}
