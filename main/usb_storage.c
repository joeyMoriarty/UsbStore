#include <string.h>
#include <stdio.h>
#include <wchar.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "usb_storage.h"

static const char *TAG = "usbstore";

typedef struct {
    bool                     present;
    uint8_t                  addr;
    msc_host_device_handle_t dev;
    msc_host_vfs_handle_t    vfs;
    char                     base[12];
    char                     product[40];
    uint64_t                 capacity;
} slot_t;

static slot_t            s_slot[USBSTORE_MAX_DRIVES];
static SemaphoreHandle_t s_lock;          /* guards s_slot and all FS access */
static QueueHandle_t     s_events;

typedef struct {
    enum { EV_CONNECT, EV_REMOVE } kind;
    uint8_t                  addr;
    msc_host_device_handle_t dev;
} ev_t;

/* ---------------------------------------------------------------- locking */

bool usbstore_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void usbstore_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ paths */

bool usbstore_path_ok(const char *path)
{
    if (!path || path[0] != '/') {
        return false;
    }
    /* Reject traversal outright rather than trying to normalise it. */
    if (strstr(path, "..")) {
        return false;
    }

    bool ok = false;
    if (!usbstore_lock(500)) {
        return false;
    }
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        if (!s_slot[i].present) {
            continue;
        }
        size_t n = strlen(s_slot[i].base);
        /* Must be the mount point itself or something beneath it, so that
         * "/usb10" cannot pass as a child of "/usb1". */
        if (strncmp(path, s_slot[i].base, n) == 0 &&
            (path[n] == '\0' || path[n] == '/')) {
            ok = true;
            break;
        }
    }
    usbstore_unlock();
    return ok;
}

int usbstore_list(usbstore_drive_t *out, int max)
{
    int n = 0;
    if (!usbstore_lock(500)) {
        return 0;
    }
    for (int i = 0; i < USBSTORE_MAX_DRIVES && n < max; i++) {
        if (!s_slot[i].present) {
            continue;
        }
        out[n].present  = true;
        out[n].capacity = s_slot[i].capacity;
        snprintf(out[n].base, sizeof(out[n].base), "%s", s_slot[i].base);
        snprintf(out[n].product, sizeof(out[n].product), "%s", s_slot[i].product);
        n++;
    }
    usbstore_unlock();
    return n;
}

/* ------------------------------------------------------------- mount/umount */

static void mount_device(uint8_t addr)
{
    if (!usbstore_lock(5000)) {
        return;
    }

    int idx = -1;
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        if (!s_slot[i].present) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        ESP_LOGW(TAG, "device %u ignored: all %d slots in use",
                 addr, USBSTORE_MAX_DRIVES);
        usbstore_unlock();
        return;
    }

    slot_t *s = &s_slot[idx];
    memset(s, 0, sizeof(*s));
    s->addr = addr;

    esp_err_t err = msc_host_install_device(addr, &s->dev);
    if (err != ESP_OK) {
        /* The usual cause is running out of host channels, not a bad drive. */
        ESP_LOGE(TAG, "install_device(%u) failed: %s", addr, esp_err_to_name(err));
        usbstore_unlock();
        return;
    }

    msc_host_device_info_t info;
    if (msc_host_get_device_info(s->dev, &info) == ESP_OK) {
        s->capacity = (uint64_t)info.sector_count * (uint64_t)info.sector_size;
        /* Product string is UTF-16; flatten the ASCII range for display. */
        size_t j = 0;
        for (size_t i = 0; info.iProduct[i] && j < sizeof(s->product) - 1; i++) {
            wchar_t c = info.iProduct[i];
            s->product[j++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
        s->product[j] = '\0';

        /* Some drives pad the name: the SanDisk 3.2Gen1 reports
         * " SanDisk 3.2Gen1". Trim both ends. */
        while (j > 0 && s->product[j - 1] == ' ') {
            s->product[--j] = '\0';
        }
        size_t lead = strspn(s->product, " ");
        memmove(s->product, s->product + lead, j - lead + 1);
    }
    if (s->product[0] == '\0') {
        snprintf(s->product, sizeof(s->product), "USB drive");
    }

    snprintf(s->base, sizeof(s->base), "/usb%d", idx);

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
        ESP_LOGE(TAG, "mount %s failed: %s (exFAT? reformat as FAT32)",
                 s->base, esp_err_to_name(err));
        msc_host_uninstall_device(s->dev);
        usbstore_unlock();
        return;
    }

    s->present = true;
    ESP_LOGI(TAG, "mounted %s  \"%s\"  %llu MB",
             s->base, s->product, s->capacity / (1024ULL * 1024ULL));
    usbstore_unlock();
}

static void unmount_device(msc_host_device_handle_t dev)
{
    if (!usbstore_lock(5000)) {
        return;
    }
    for (int i = 0; i < USBSTORE_MAX_DRIVES; i++) {
        slot_t *s = &s_slot[i];
        if (!s->present || s->dev != dev) {
            continue;
        }
        ESP_LOGI(TAG, "removing %s", s->base);
        msc_host_vfs_unregister(s->vfs);
        msc_host_uninstall_device(s->dev);
        memset(s, 0, sizeof(*s));
    }
    usbstore_unlock();
}

/* -------------------------------------------------------------- bus census */
/*
 * A second USB host client that only watches. The MSC driver reports mass
 * storage devices and nothing else; this logs every device that reaches the
 * S3, so a non-storage device (or one that enumerates but never mounts) is
 * still visible, and an empty list means no signal reaches the port at all.
 *
 * Hubs themselves do NOT appear here: the host library's external hub driver
 * claims them internally and never announces them to clients. Seen on real
 * hardware - two drives behind a hub, census lists only the two drives.
 *
 * Each device is held open until it leaves, because the host library only
 * delivers DEV_GONE to clients that have the device open.
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
    }
    snprintf(d.kind, sizeof(d.kind), "%s", class_name(d.cls));

    ESP_LOGI(TAG, "USB dev %u: %04x:%04x %s (class 0x%02x), %s-speed",
             addr, d.vid, d.pid, d.kind, d.cls, d.speed);

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

/* ------------------------------------------------------------------ events */

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
        if (ev.kind == EV_CONNECT) {
            mount_device(ev.addr);
        } else {
            unmount_device(ev.dev);
        }
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
    s_events = xQueueCreate(8, sizeof(ev_t));
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    if (xTaskCreate(host_lib_task, "usb_host", 4096, NULL, 4, NULL) != pdPASS) {
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
    if (xTaskCreate(diag_task, "usb_census", 4096, NULL, 3, NULL) != pdPASS) {
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
        .callback              = msc_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_cfg));

    if (xTaskCreate(worker_task, "usb_mount", 5120, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB host up, waiting for drives (hub support compiled in)");
    return ESP_OK;
}
