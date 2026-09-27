/*
 * USB mass-storage host: enumerates drives behind a hub and mounts each one
 * on its own VFS path (/usb0, /usb1, ...).
 *
 * Throughput ceiling is the bus, not this code. The ESP32-S3's USB is Full
 * Speed only (USB 1.1, 12 Mbit/s): measured ~520 KB/s read over WiFi, shared
 * across every drive on the hub. A document and photo drop, not a media server.
 *
 * ---- Park and swap -------------------------------------------------------
 * The S3 has 8 USB host channels and every open pipe holds one permanently:
 * the hub 2, each drive's control pipe 1, and each *mounted* drive another 2
 * (bulk in + out). Two mounted drives behind a hub use all 8, and a third
 * cannot even enumerate.
 *
 * So a drive can be recognised without being mounted. States:
 *   PARKED - enumerated, control pipe only (1 channel), not mounted
 *   OPEN   - MSC installed + FAT mounted (3 channels)
 *   FAILED - could not be mounted (unsupported filesystem, etc.)
 *
 * Budget: 2 (hub) + drives + 2 x open <= 8. With 1-2 drives present, both
 * may be open; with 3-4, one at a time, and touching a parked drive swaps it
 * in. At plug-in a drive is opened only if nothing else is open, and when two
 * are open the less recently used one is parked after a short idle period -
 * which keeps a channel free so the NEXT plug-in can enumerate.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Bounded by the channel budget above: 2 + 4 + 2 = 8 with one open. */
#define USBSTORE_MAX_DRIVES 4

typedef struct {
    bool     present;
    char     base[12];     /* VFS mount point, e.g. "/usb0" - stable while plugged in */
    char     product[40];  /* USB product string, best effort */
    uint64_t capacity;     /* bytes; 0 until the drive has been opened once */
    char     state[8];     /* "open", "parked" or "failed" */
    char     note[48];     /* why it failed, when state is "failed" */
} usbstore_drive_t;

/* Every enumerated device, not just drives - except hubs, which the host
 * library claims internally and never reports. Diagnostic view. */
#define USBSTORE_MAX_BUS 8

typedef struct {
    uint8_t  addr;
    uint16_t vid;
    uint16_t pid;
    uint8_t  cls;          /* device class, or first interface class if 0 */
    char     kind[16];     /* "hub", "mass-storage", ... */
    char     speed[6];     /* "low" / "full" / "high" */
    char     product[40];  /* from the string descriptor, if the device has one */
} usbstore_usbdev_t;

esp_err_t usbstore_start(void);

/* Every USB device currently enumerated. Empty means no device has reached
 * the S3's port at all - a cable, adapter or power problem, not a drive one. */
int usbstore_census(usbstore_usbdev_t *out, int max);

/* Snapshot of the drive table, open and parked alike. Never blocks: it is
 * read while transfers run, and is for display only. */
int usbstore_list(usbstore_drive_t *out, int max);

/* True if path sits inside a known drive (open or parked) and contains no
 * "..". A cheap first filter; usbstore_acquire() re-checks under the lock. */
bool usbstore_path_ok(const char *path);

/*
 * ---- Using a drive ------------------------------------------------------
 * Every filesystem access is bracketed by acquire/release. Holding a ref is
 * what marks the drive IN USE: it cannot be parked, and if it is unplugged
 * its unmount waits until the last ref is released. Refs are counted, so any
 * number of requests may use one drive at once - FatFs already serialises
 * access within a volume (FF_FS_REENTRANT).
 *
 * This replaced a single global lock, which was simpler but froze the whole
 * web server for the length of any transfer.
 */
typedef int usbstore_ref_t;          /* opaque; negative = none */

/*
 * Make sure the drive holding `path` is mounted - parking an IDLE drive if
 * the channel budget requires it - and take a ref on it. Fails, with a
 * human-readable reason in `why`, if the only drives that could be parked
 * are in use.
 */
esp_err_t usbstore_acquire(const char *path, char *why, size_t why_len,
                           usbstore_ref_t *ref);

/* Drop a ref. Restarts the drive's idle clock, so a long download counts as
 * use right up to its last byte. Safe to call with a negative ref. */
void usbstore_release(usbstore_ref_t ref);

/* Unmount every open drive (flushing FAT) so the box can sleep or power off
 * without leaving a filesystem half-written. Waits up to timeout_ms for every
 * ref to be released. Returns drives parked, or -1 if it timed out. */
int usbstore_park_all(uint32_t timeout_ms);
