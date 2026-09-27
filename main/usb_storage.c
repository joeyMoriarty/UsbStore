#include <string.h>
#include <stdio.h>
#include <wchar.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "usb_storage.h"

static const char *TAG = "usbstore";

/* How long the less recently used of two open drives may sit idle before it
 * is parked, freeing the channels a new plug-in needs to enumerate. */
#define IDLE_PARK_MS  15000
#define IDLE_CHECK_MS 2000

/*
 * Every USB task is pinned to core 1. ESP-IDF runs WiFi on core 0, so the
 * radio never shares a core with USB traffic and the web page stays
 * responsive during transfers. It costs no throughput: transfers are bound
 * by the 12 Mbit/s bus, and these tasks mostly sleep waiting on it.
 */
#define USB_CORE 1

typedef enum { ST_EMPTY = 0, ST_PARKED, ST_OPEN, ST_FAILED } slot_state_t;

typedef struct {
    slot_state_t             st;
    uint8_t                  addr;
    uint16_t                 gen;       /* bumped on each reuse: stale refs can't match */
    int                      refs;      /* requests using the drive right now */
    bool                     gone;      /* unplugged while in use; clean up on last release */
    msc_host_device_handle_t dev;       /* only while OPEN */
    msc_host_vfs_handle_t    vfs;       /* only while OPEN */
    char                     base[12];  /* "/usbN", fixed while plugged in */
    char                     product[40];
    uint64_t                 capacity;  /* learnt on first open, then kept */
    TickType_t               last_used;
    char                     note[48];  /* why it is FAILED */
} slot_t;

static slot_t            s_slot[USBSTORE_MAX_DRIVES];
/* Guards the slot table. Held only briefly - never across a transfer; a
 * drive in use is marked by its ref count instead. */
static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_events;

typedef struct {
    enum { EV_CONNECT, EV_REMOVE, EV_GONE_ADDR } kind;
    uint8_t                  addr;
    msc_host_device_handle_t dev;
} ev_t;

/* ---------------------------------------------------------------- helpers */

/* Drives pad their names: the SanDisk 3.2Gen1 reports " SanDisk 3.2Gen1". */
static void trim(char *s)
{
    size_t j = strlen(s);
    while (j > 0 && s[j - 1] == ' ') {
        s[--j] = '\0';
    }
    size_t lead = strspn(s, " ");
    memmove(s, s + lead, j - lead + 1);
}

static int count_state(slot_state_t st)
{
    int n = 0;
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        n += s_slot[i].st == st;
    }
    return n;
}

static int count_present(void)
{
    return USBSTORE_MAX_DRIVES - count_state(ST_EMPTY);
}

/* How many drives may be mounted at once without starving enumeration:
 * 2 (hub) + present + 2 x open must stay within the 8 host channels. */
static int open_limit(void)
{
    return count_present() >= 3 ? 1 : 2;
}

static slot_t *slot_for_path(const char *path)
{
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        slot_t *s = &s_slot[i];
        if (s->st == ST_EMPTY || s->gone) {
            continue;
        }
        size_t n = strlen(s->base);
        /* The mount point itself or something beneath it, so that "/usb10"
         * cannot pass as a child of "/usb1". */
        if (strncmp(path, s->base, n) == 0 && (path[n] == '\0' || path[n] == '/')) {
            return s;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- locking */

static bool table_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void table_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* Empty a slot but keep its generation, so refs to the old drive stay stale. */
static void slot_clear(slot_t *s)
{
    uint16_t gen = s->gen;
    memset(s, 0, sizeof(*s));
    s->gen = gen;
}

/* -------------------------------------------------------------- bus census */
/*
 * A second USB host client that only watches. The MSC driver reports mass
 * storage devices and nothing else; this logs every device that reaches the
 * S3, so a non-storage device (or one that enumerates but never mounts) is
 * still visible, and an empty list means no signal reaches the port at all.
 *
 * It is also how a PARKED drive's removal is noticed: the MSC driver only
 * reports disconnects for drives it has installed, while this client holds
 * every device open and so hears every DEV_GONE.
 *
 * Hubs themselves do NOT appear here: the host library's external hub driver
 * claims them internally and never announces them to clients. Seen on real
 * hardware - two drives behind a hub, census lists only the two drives.
 */

typedef struct {
    bool                used;
    usb_device_handle_t h;
    usbstore_usbdev_t   info;
} census_t;

static census_t                 s_census[USBSTORE_MAX_BUS];
static portMUX_TYPE             s_census_mux = portMUX_INITIALIZER_UNLOCKED;
static usb_host_client_handle_t s_diag;

/* Filled by the callback, drained after usb_host_client_handle_events()
 * returns: devices are opened from the task loop, not inside the callback. */
static uint8_t             s_new[USBSTORE_MAX_BUS];
static int                 s_n_new;
static usb_device_handle_t s_gone[USBSTORE_MAX_BUS];
static int                 s_n_gone;

static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0x01: return "audio";
    case 0x02: return "cdc";
    case 0x03: return "hid";
    case 0x07: return "printer";
    case 0x08: return "mass-storage";
    case 0x09: return "hub";
    case 0x0A: return "cdc-data";
    case 0x0E: return "video";
    case 0xE0: return "wireless";
    case 0xEF: return "misc";
    case 0xFF: return "vendor";
    default:   return "other";
    }
}

static const char *speed_name(usb_speed_t s)
{
    return s == USB_SPEED_LOW ? "low" : s == USB_SPEED_FULL ? "full" :
           s == USB_SPEED_HIGH ? "high" : "?";
}

/* Pendrives report class 0 on the device and put 0x08 on the interface, so
 * fall through to the first interface descriptor in the active config. */
static uint8_t effective_class(usb_device_handle_t h, uint8_t dev_class)
{
    if (dev_class != 0x00) {
        return dev_class;
    }
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(h, &cfg) != ESP_OK || !cfg) {
        return 0x00;
    }
    const uint8_t *p   = (const uint8_t *)cfg;
    uint16_t       len = cfg->wTotalLength;
    for (uint16_t i = 0; i + 1 < len && p[i] >= 2; i += p[i]) {
        if (p[i + 1] == 0x04 && i + 5 < len) {    /* INTERFACE descriptor */
            return p[i + 5];                       /* bInterfaceClass */
        }
    }
    return 0x00;
}

/* The product string lets a parked drive be named without mounting it. */
static void str_desc_ascii(const usb_str_desc_t *d, char *out, size_t cap)
{
    out[0] = '\0';
    if (!d || d->bLength < 2) {
        return;
    }
    size_t n = (d->bLength - 2) / 2, j = 0;
    for (size_t i = 0; i < n && j + 1 < cap; i++) {
        uint16_t c = d->wData[i];
        out[j++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[j] = '\0';
    trim(out);
}

static void diag_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV && s_n_new < USBSTORE_MAX_BUS) {
        s_new[s_n_new++] = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && s_n_gone < USBSTORE_MAX_BUS) {
        s_gone[s_n_gone++] = msg->dev_gone.dev_hdl;
    }
}

static void census_add(uint8_t addr)
{
    /* The boot-time sweep and a late NEW_DEV can both name one device. */
    bool known = false;
    portENTER_CRITICAL(&s_census_mux);
    for (int i = 0; i < USBSTORE_MAX_BUS; i++) {
        known |= s_census[i].used && s_census[i].info.addr == addr;
    }
    portEXIT_CRITICAL(&s_census_mux);
    if (known) {
        return;
    }

    usb_device_handle_t h;
    if (usb_host_device_open(s_diag, addr, &h) != ESP_OK) {
        ESP_LOGW(TAG, "USB dev %u enumerated but could not be opened", addr);
        return;
    }

    usbstore_usbdev_t d = { .addr = addr };
    const usb_device_desc_t *desc = NULL;
    if (usb_host_get_device_descriptor(h, &desc) == ESP_OK && desc) {
        d.vid = desc->idVendor;
        d.pid = desc->idProduct;
        d.cls = effective_class(h, desc->bDeviceClass);
    }
    usb_device_info_t info;
    if (usb_host_device_info(h, &info) == ESP_OK) {
        snprintf(d.speed, sizeof(d.speed), "%s", speed_name(info.speed));
        str_desc_ascii(info.str_desc_product, d.product, sizeof(d.product));
    }
    snprintf(d.kind, sizeof(d.kind), "%s", class_name(d.cls));

    ESP_LOGI(TAG, "USB dev %u: %04x:%04x %s (class 0x%02x), %s-speed \"%s\"",
             addr, d.vid, d.pid, d.kind, d.cls, d.speed, d.product);

    portENTER_CRITICAL(&s_census_mux);
    for (int i = 0; i < USBSTORE_MAX_BUS; i++) {
        if (!s_census[i].used) {
            s_census[i] = (census_t){ .used = true, .h = h, .info = d };
            h = NULL;
            break;
        }
    }
    portEXIT_CRITICAL(&s_census_mux);

    if (h) {                                   /* table full: don't hold it */
        usb_host_device_close(s_diag, h);
    }
}

static void census_remove(usb_device_handle_t h)
{
    usbstore_usbdev_t gone = {0};
    bool found = false;

    portENTER_CRITICAL(&s_census_mux);
    for (int i = 0; i < USBSTORE_MAX_BUS; i++) {
        if (s_census[i].used && s_census[i].h == h) {
            gone  = s_census[i].info;
            found = true;
            s_census[i].used = false;
            break;
        }
    }
    portEXIT_CRITICAL(&s_census_mux);

    usb_host_device_close(s_diag, h);
    if (found) {
        ESP_LOGI(TAG, "USB dev %u gone: %04x:%04x %s", gone.addr, gone.vid, gone.pid, gone.kind);
        /* The storage layer needs this for parked drives, which the MSC
         * driver never reports leaving. */
        ev_t ev = { .kind = EV_GONE_ADDR, .addr = gone.addr };
        xQueueSend(s_events, &ev, pdMS_TO_TICKS(100));
    }
}

static void diag_task(void *arg)
{
    while (1) {
        usb_host_client_handle_events(s_diag, portMAX_DELAY);

        /* Copy out, then act: opening a device can itself produce events. */
        uint8_t new_addrs[USBSTORE_MAX_BUS];
        usb_device_handle_t gone[USBSTORE_MAX_BUS];
        int n_new = s_n_new, n_gone = s_n_gone;
        memcpy(new_addrs, s_new, n_new);
        memcpy(gone, s_gone, n_gone * sizeof(gone[0]));
        s_n_new = s_n_gone = 0;

        for (int i = 0; i < n_gone; i++) {
            census_remove(gone[i]);
        }
        for (int i = 0; i < n_new; i++) {
            census_add(new_addrs[i]);
        }
    }
}

int usbstore_census(usbstore_usbdev_t *out, int max)
{
    int n = 0;
    portENTER_CRITICAL(&s_census_mux);
    for (int i = 0; i < USBSTORE_MAX_BUS && n < max; i++) {
        if (s_census[i].used) {
            out[n++] = s_census[i].info;
        }
    }
    portEXIT_CRITICAL(&s_census_mux);
    return n;
}

static bool census_product(uint8_t addr, char *out, size_t cap)
{
    bool found = false;
    portENTER_CRITICAL(&s_census_mux);
    for (int i = 0; i < USBSTORE_MAX_BUS; i++) {
        if (s_census[i].used && s_census[i].info.addr == addr && s_census[i].info.product[0]) {
            snprintf(out, cap, "%s", s_census[i].info.product);
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_census_mux);
    return found;
}

/*
 * Reset the drive's bulk data toggles before the MSC driver claims it again.
 *
 * Every bulk packet carries an alternating DATA0/DATA1 bit. Parking frees the
 * host's pipes, but the drive keeps its own toggle state; a re-claimed pipe
 * starts at DATA0, the drive expects whatever came next, and it silently
 * drops the first command as a duplicate. The symptom on real hardware: a
 * drive opens fine once, then every re-open times out ("Transfer failed:
 * Status 3") on its very first SCSI command.
 *
 * CLEAR_FEATURE(ENDPOINT_HALT) resets the device-side toggle to DATA0 even
 * when the endpoint is not halted (USB 2.0 §9.4.5) - exactly what a fresh
 * host pipe expects. Sent from the census client, which holds every device
 * open anyway, so no MSC state is involved.
 */
static SemaphoreHandle_t s_ctrl_done;

static void ctrl_cb(usb_transfer_t *xfer)
{
    xSemaphoreGive((SemaphoreHandle_t)xfer->context);
}

static esp_err_t census_reset_toggles(uint8_t addr)
{
    usb_device_handle_t h = NULL;
    portENTER_CRITICAL(&s_census_mux);
    for (int i = 0; i < USBSTORE_MAX_BUS; i++) {
        if (s_census[i].used && s_census[i].info.addr == addr) {
            h = s_census[i].h;
            break;
        }
    }
    portEXIT_CRITICAL(&s_census_mux);
    if (!h) {
        return ESP_ERR_NOT_FOUND;
    }

    /* The bulk endpoints of the mass-storage interface. */
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(h, &cfg) != ESP_OK || !cfg) {
        return ESP_FAIL;
    }
    uint8_t eps[2] = {0};
    int     n_eps  = 0;
    bool    in_msc = false;
    const uint8_t *p = (const uint8_t *)cfg;
    for (uint16_t i = 0; i + 1 < cfg->wTotalLength && p[i] >= 2; i += p[i]) {
        if (p[i + 1] == 0x04 && i + 5 < cfg->wTotalLength) {        /* INTERFACE */
            in_msc = p[i + 5] == 0x08;
        } else if (in_msc && p[i + 1] == 0x05 && i + 3 < cfg->wTotalLength &&
                   (p[i + 3] & 0x03) == 0x02 && n_eps < 2) {         /* bulk ENDPOINT */
            eps[n_eps++] = p[i + 2];
        }
    }
    if (n_eps == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    usb_transfer_t *x = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &x) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    x->device_handle    = h;
    x->bEndpointAddress = 0;
    x->callback         = ctrl_cb;
    x->context          = s_ctrl_done;
    x->num_bytes        = sizeof(usb_setup_packet_t);

    esp_err_t err = ESP_OK;
    for (int i = 0; i < n_eps && err == ESP_OK; i++) {
        usb_setup_packet_t *sp = (usb_setup_packet_t *)x->data_buffer;
        sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                            USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
        sp->bRequest = USB_B_REQUEST_CLEAR_FEATURE;
        sp->wValue   = 0;                     /* ENDPOINT_HALT */
        sp->wIndex   = eps[i];
        sp->wLength  = 0;

        err = usb_host_transfer_submit_control(s_diag, x);
        if (err != ESP_OK) {
            break;
        }
        /* The census task pumps this client's events, so the callback runs
         * there while this task waits. */
        if (xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* Still in flight: freeing it now would be a use-after-free.
             * Leak one small transfer rather than risk that. */
            ESP_LOGW(TAG, "dev %u: toggle reset timed out", addr);
            return ESP_ERR_TIMEOUT;
        }
        if (x->status != USB_TRANSFER_STATUS_COMPLETED) {
            err = ESP_FAIL;
        }
    }
    usb_host_transfer_free(x);
    return err;
}

/* ------------------------------------------------------------ open / park */
/* All of these run with s_lock held, and never on a drive with refs > 0. */

static esp_err_t slot_open(slot_t *s)
{
    /* Always, not just after a park: harmless on a fresh drive, and it also
     * rescues one parked by a build that didn't reset on the way out. */
    esp_err_t tr = census_reset_toggles(s->addr);
    if (tr != ESP_OK) {
        ESP_LOGW(TAG, "%s: toggle reset skipped (%s)", s->base, esp_err_to_name(tr));
    }

    esp_err_t err = msc_host_install_device(s->addr, &s->dev);
    if (err != ESP_OK) {
        /* Stays PARKED rather than FAILED: running out of host channels
         * lands here too, and that clears once another drive is parked. */
        s->dev = NULL;
        snprintf(s->note, sizeof(s->note), "could not open (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s: %s", s->base, s->note);
        return err;
    }

    msc_host_device_info_t info;
    if (msc_host_get_device_info(s->dev, &info) == ESP_OK) {
        s->capacity = (uint64_t)info.sector_count * (uint64_t)info.sector_size;
        if (!s->product[0]) {
            size_t j = 0;
            for (size_t i = 0; info.iProduct[i] && j < sizeof(s->product) - 1; i++) {
                wchar_t c = info.iProduct[i];
                s->product[j++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
            s->product[j] = '\0';
            trim(s->product);
        }
    }

    const esp_vfs_fat_mount_config_t mnt = {
        .max_files            = 5,
        /* Never true. A mount failure means an unsupported filesystem
         * (exFAT is the common one) and formatting would erase the user's
         * data to "fix" it. Reformat to FAT32 on a PC instead. */
        .format_if_mount_failed = false,
        .allocation_unit_size = 8192,
    };
    err = msc_host_vfs_register(s->dev, s->base, &mnt, &s->vfs);
    if (err != ESP_OK) {
        msc_host_uninstall_device(s->dev);
        s->dev = NULL;
        s->st  = ST_FAILED;
        snprintf(s->note, sizeof(s->note), "not FAT32 - reformat on a PC");
        ESP_LOGE(TAG, "mount %s failed: %s (exFAT? reformat as FAT32)",
                 s->base, esp_err_to_name(err));
        return err;
    }

    s->st        = ST_OPEN;
    s->note[0]   = '\0';
    s->last_used = xTaskGetTickCount();
    ESP_LOGI(TAG, "opened %s  \"%s\"  %llu MB", s->base, s->product,
             s->capacity / (1024ULL * 1024ULL));
    return ESP_OK;
}

/*
 * Deliberately NOT msc_host_reset_recovery() here, although it looks like the
 * right tool. It is only safe on a drive whose endpoints are actually stalled:
 * its clear_feature() halts the host pipe, then returns early without
 * un-halting it when the endpoint has no STALL to flush (msc_host.c, usb_host_
 * msc 1.3.0). On a healthy drive it leaves both pipes frozen and its closing
 * readiness check times out twice - measured as ~10 s added to every swap.
 * The toggle reset that matters happens on the way back in, in slot_open().
 */
static void slot_park(slot_t *s)
{
    msc_host_vfs_unregister(s->vfs);     /* flushes FAT before letting go */
    msc_host_uninstall_device(s->dev);
    s->vfs = NULL;
    s->dev = NULL;
    s->st  = ST_PARKED;
    ESP_LOGI(TAG, "parked %s  \"%s\"", s->base, s->product);
}

/* The IDLE open drive (no refs) used longest ago, other than `keep`. A drive
 * in use is never a candidate - that is what makes parking safe. */
static slot_t *lru_idle(const slot_t *keep)
{
    slot_t *best = NULL;
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        slot_t *s = &s_slot[i];
        if (s == keep || s->st != ST_OPEN || s->refs > 0 || s->gone) {
            continue;
        }
        if (!best || (TickType_t)(s->last_used - best->last_used) > portMAX_DELAY / 2) {
            best = s;      /* wrap-safe "s is older than best" */
        }
    }
    return best;
}

/* Name of an open drive that is in use, for the "busy" message. */
static const char *busy_name(void)
{
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        if (s_slot[i].st == ST_OPEN && s_slot[i].refs > 0) {
            return s_slot[i].product[0] ? s_slot[i].product : s_slot[i].base;
        }
    }
    return "another drive";
}

/* The unmount a removal had to defer, done once nobody is using the drive. */
static void cleanup_gone(slot_t *s)
{
    ESP_LOGI(TAG, "removed %s  \"%s\"  (after its last transfer ended)", s->base, s->product);
    msc_host_vfs_unregister(s->vfs);
    msc_host_uninstall_device(s->dev);
    slot_clear(s);
}

/*
 * How long a swap waits for an in-use drive to free up. Listings, deletes and
 * new folders release their ref within milliseconds, so a tab clicked while
 * the previous folder is still loading just waits and swaps - exactly as it
 * did with the old global lock. Only a genuinely long transfer outlasts this
 * and gets "busy".
 */
#define SWAP_WAIT_MS 3000

esp_err_t usbstore_acquire(const char *path, char *why, size_t why_len,
                           usbstore_ref_t *ref)
{
    *ref = -1;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SWAP_WAIT_MS);

    while (1) {
        /* Opening a drive under the lock takes ~10 ms; 5 s only ever expires
         * if something is badly wrong. */
        if (!table_lock(5000)) {
            snprintf(why, why_len, "storage busy");
            return ESP_ERR_TIMEOUT;
        }

        slot_t *s = slot_for_path(path);
        esp_err_t err = ESP_OK;
        bool retry = false;
        if (!s) {
            snprintf(why, why_len, "drive not present");
            err = ESP_ERR_NOT_FOUND;
        } else if (s->st == ST_FAILED) {
            snprintf(why, why_len, "%s", s->note);
            err = ESP_ERR_INVALID_STATE;
        } else if (s->st == ST_PARKED) {
            /* Make room within the channel budget by parking IDLE drives
             * only - never pull a drive out from under a transfer. */
            while (count_state(ST_OPEN) >= open_limit()) {
                slot_t *victim = lru_idle(s);
                if (!victim) {
                    if ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
                        retry = true;          /* wait for it outside the lock */
                    } else {
                        snprintf(why, why_len,
                                 "busy: \"%s\" is in use by a transfer - "
                                 "try again when it finishes", busy_name());
                        err = ESP_ERR_INVALID_STATE;
                    }
                    break;
                }
                slot_park(victim);
            }
            if (!retry && err == ESP_OK && (err = slot_open(s)) != ESP_OK) {
                snprintf(why, why_len, "%s", s->note[0] ? s->note : "could not open drive");
            }
        }

        if (retry) {
            table_unlock();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (err == ESP_OK) {
            s->refs++;
            s->last_used = xTaskGetTickCount();
            *ref = ((int)(s->gen & 0x7FFF) << 8) | (int)(s - s_slot);
        }
        table_unlock();
        return err;
    }
}

void usbstore_release(usbstore_ref_t ref)
{
    if (ref < 0 || (ref & 0xFF) >= USBSTORE_MAX_DRIVES) {
        return;
    }
    table_lock(portMAX_DELAY);
    slot_t *s = &s_slot[ref & 0xFF];
    /* Generation check: if the drive was pulled and its slot reused, this
     * ref belongs to a drive that no longer exists - leave the new one be. */
    if ((s->gen & 0x7FFF) == (ref >> 8) && s->refs > 0) {
        s->refs--;
        s->last_used = xTaskGetTickCount();   /* idle clock starts at the last byte */
        if (s->refs == 0 && s->gone) {
            cleanup_gone(s);
        }
    }
    table_unlock();
}

int usbstore_park_all(uint32_t timeout_ms)
{
    /* Transfers check thermal_cooling() every chunk and release their refs
     * within milliseconds; this waits for that, then unmounts everything. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (1) {
        if (table_lock(100)) {
            bool in_use = false;
            for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
                in_use |= s_slot[i].st == ST_OPEN && s_slot[i].refs > 0;
            }
            if (!in_use) {
                int n = 0;
                for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
                    if (s_slot[i].st == ST_OPEN && !s_slot[i].gone) {
                        slot_park(&s_slot[i]);
                        n++;
                    }
                }
                table_unlock();
                return n;
            }
            table_unlock();
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ------------------------------------------------------------- queries */

bool usbstore_path_ok(const char *path)
{
    if (!path || path[0] != '/') {
        return false;
    }
    /* Reject traversal outright rather than trying to normalise it. */
    if (strstr(path, "..")) {
        return false;
    }
    /* Lock-free: a cheap early reject only. usbstore_acquire() re-checks
     * under the lock, so a drive vanishing in between is still caught. */
    return slot_for_path(path) != NULL;
}

/*
 * Deliberately lock-free. It feeds the status page, which must keep working
 * while a drive is being opened; a torn read can at worst show one stale
 * field for one poll. Nothing acts on this snapshot.
 */
int usbstore_list(usbstore_drive_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < USBSTORE_MAX_DRIVES && n < max; i++) {
        const slot_t *s = &s_slot[i];
        if (s->st == ST_EMPTY || s->gone) {
            continue;
        }
        usbstore_drive_t *d = &out[n++];
        memset(d, 0, sizeof(*d));
        d->present  = true;
        d->capacity = s->capacity;
        snprintf(d->base, sizeof(d->base), "%s", s->base);
        snprintf(d->state, sizeof(d->state), "%s",
                 s->st == ST_OPEN ? "open" : s->st == ST_FAILED ? "failed" : "parked");
        snprintf(d->note, sizeof(d->note), "%s", s->st == ST_FAILED ? s->note : "");
        if (s->product[0]) {
            snprintf(d->product, sizeof(d->product), "%s", s->product);
        } else if (!census_product(s->addr, d->product, sizeof(d->product))) {
            snprintf(d->product, sizeof(d->product), "USB drive");
        }
    }
    return n;
}

/* ------------------------------------------------------------- lifecycle */

static void on_connect(uint8_t addr)
{
    if (!table_lock(10000)) {
        ESP_LOGE(TAG, "drive %u: storage busy, not added", addr);
        return;
    }

    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        if (s_slot[i].st != ST_EMPTY && !s_slot[i].gone && s_slot[i].addr == addr) {
            table_unlock();                 /* duplicate event */
            return;
        }
    }

    int idx = -1;
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        if (s_slot[i].st == ST_EMPTY) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        ESP_LOGW(TAG, "drive %u ignored: all %d slots in use", addr, USBSTORE_MAX_DRIVES);
        table_unlock();
        return;
    }

    slot_t *s = &s_slot[idx];
    slot_clear(s);
    s->gen++;                  /* any ref to this slot's previous drive is now stale */
    s->st   = ST_PARKED;
    s->addr = addr;
    snprintf(s->base, sizeof(s->base), "/usb%d", idx);
    census_product(addr, s->product, sizeof(s->product));
    ESP_LOGI(TAG, "drive on %s  \"%s\"  (%d present)", s->base, s->product, count_present());

    /* Open it straight away only if nothing else is open. Opening a second
     * drive here would fill all 8 channels and stop the next plug-in from
     * enumerating; it opens on first use instead. */
    if (count_state(ST_OPEN) == 0) {
        slot_open(s);
    }
    table_unlock();
}

/*
 * An installed (OPEN) drive left: the MSC driver reported it.
 *
 * If a transfer is using it, don't unmount under an open file - mark it gone
 * so no new request can reach it, and let the last usbstore_release() do the
 * cleanup. The transfer itself fails fast: the device no longer answers.
 */
static void on_msc_removed(msc_host_device_handle_t dev)
{
    table_lock(portMAX_DELAY);    /* held only briefly by anyone now */
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        slot_t *s = &s_slot[i];
        if (s->st != ST_OPEN || s->dev != dev || s->gone) {
            continue;
        }
        if (s->refs > 0) {
            s->gone = true;
            ESP_LOGW(TAG, "%s \"%s\" pulled while in use - unmounting when its transfer ends",
                     s->base, s->product);
        } else {
            ESP_LOGI(TAG, "removed %s  \"%s\"", s->base, s->product);
            msc_host_vfs_unregister(s->vfs);
            msc_host_uninstall_device(s->dev);
            slot_clear(s);
        }
    }
    table_unlock();
}

/* Any device left, per the census. Only acts on drives the MSC driver was
 * not tracking - OPEN ones are cleaned up by on_msc_removed(). A parked or
 * failed drive can't hold refs (acquire opens before counting), so it is
 * always safe to drop immediately. */
static void on_gone_addr(uint8_t addr)
{
    table_lock(portMAX_DELAY);
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        slot_t *s = &s_slot[i];
        if ((s->st == ST_PARKED || s->st == ST_FAILED) && s->addr == addr) {
            ESP_LOGI(TAG, "removed %s  \"%s\"  (was %s)", s->base, s->product,
                     s->st == ST_PARKED ? "parked" : "failed");
            slot_clear(s);
        }
    }
    table_unlock();
}

/* Runs in the MSC driver's task: post to a queue, do the slow work elsewhere. */
static void msc_cb(const msc_host_event_t *event, void *arg)
{
    ev_t ev = {0};
    if (event->event == MSC_DEVICE_CONNECTED) {
        ev.kind = EV_CONNECT;
        ev.addr = event->device.address;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        ev.kind = EV_REMOVE;
        ev.dev  = event->device.handle;
    } else {
        return;
    }
    xQueueSend(s_events, &ev, 0);
}

static void worker_task(void *arg)
{
    ev_t ev;
    while (1) {
        if (xQueueReceive(s_events, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (ev.kind) {
        case EV_CONNECT:   on_connect(ev.addr);     break;
        case EV_REMOVE:    on_msc_removed(ev.dev);  break;
        case EV_GONE_ADDR: on_gone_addr(ev.addr);   break;
        }
    }
}

/*
 * Parks the less recently used of two open drives once it has been idle for
 * IDLE_PARK_MS, and enforces the one-open rule if a third drive turned up.
 * Only drives with no refs are ever candidates, so a transfer is never cut.
 */
static void idle_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(IDLE_CHECK_MS));
        if (!table_lock(50)) {
            continue;
        }
        while (count_state(ST_OPEN) > open_limit()) {
            slot_t *victim = lru_idle(NULL);
            if (!victim) {
                break;              /* all in use: try again next round */
            }
            slot_park(victim);
        }
        if (count_state(ST_OPEN) > 1) {
            slot_t *victim = lru_idle(NULL);
            if (victim && xTaskGetTickCount() - victim->last_used > pdMS_TO_TICKS(IDLE_PARK_MS)) {
                slot_park(victim);
            }
        }
        table_unlock();
    }
}

/* The USB host library needs its events pumped continuously. */
static void host_lib_task(void *arg)
{
    while (1) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* -------------------------------------------------------------------- init */

esp_err_t usbstore_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_events = xQueueCreate(16, sizeof(ev_t));
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }
    s_ctrl_done = xSemaphoreCreateBinary();
    if (!s_ctrl_done) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    if (xTaskCreatePinnedToCore(host_lib_task, "usb_host", 4096, NULL, 4, NULL, USB_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* The census client goes in straight after the host library, ahead of
     * MSC, so no device that enumerates early is missed. */
    const usb_host_client_config_t diag_cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = diag_cb,
            .callback_arg          = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&diag_cfg, &s_diag));
    if (xTaskCreatePinnedToCore(diag_task, "usb_census", 4096, NULL, 3, NULL, USB_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* NEW_DEV only goes to clients registered at the time. Anything that
     * enumerated in the gap before this point is picked up here instead. */
    uint8_t addrs[USBSTORE_MAX_BUS];
    int n_addrs = 0;
    if (usb_host_device_addr_list_fill(USBSTORE_MAX_BUS, addrs, &n_addrs) == ESP_OK) {
        for (int i = 0; i < n_addrs; i++) {
            census_add(addrs[i]);
        }
    }

    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority         = 5,
        .stack_size            = 4096,
        .core_id               = USB_CORE,
        .callback              = msc_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_cfg));

    if (xTaskCreatePinnedToCore(worker_task, "usb_mount", 5120, NULL, 3, NULL, USB_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(idle_task, "usb_idle", 4096, NULL, 2, NULL, USB_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB host up, waiting for drives (park-and-swap: max %d)",
             USBSTORE_MAX_DRIVES);
    return ESP_OK;
}
