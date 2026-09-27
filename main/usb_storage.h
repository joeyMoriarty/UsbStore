/*
 * USB mass-storage host: enumerates drives behind a hub and mounts each one
 * on its own VFS path (/usb0, /usb1, ...).
 *
 * Throughput ceiling is the bus, not this code. The ESP32-S3's USB is Full
 * Speed only (USB 1.1, 12 Mbit/s); Espressif measure ~540 KB/s read and
 * ~350 KB/s write, and that is shared across every drive on the hub. Treat
 * this as a document and photo drop, not a media server.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Bounded by the S3's 8 host channels once the hub takes its share. */
#define USBSTORE_MAX_DRIVES 4

typedef struct {
    bool     present;
    char     base[12];     /* VFS mount point, e.g. "/usb0" */
    char     product[40];  /* USB product string, best effort */
    uint64_t capacity;     /* bytes, 0 if unknown */
} usbstore_drive_t;

/* Every enumerated device, not just drives - except hubs, which the host
 * library claims internally and never reports. Diagnostic view. */
#define USBSTORE_MAX_BUS 8

typedef struct {
    uint8_t  addr;
    uint16_t vid;
    uint16_t pid;
    uint8_t  cls;        /* device class, or first interface class if 0 */
    char     kind[16];   /* "hub", "mass-storage", ... */
    char     speed[6];   /* "low" / "full" / "high" */
} usbstore_usbdev_t;

esp_err_t usbstore_start(void);

/* Every USB device currently enumerated. Empty means no device has reached
 * the S3's port at all - a cable, adapter or power problem, not a drive one. */
int usbstore_census(usbstore_usbdev_t *out, int max);

/* Snapshot of the mount table. Returns the number of entries filled. */
int usbstore_list(usbstore_drive_t *out, int max);

/* True if path sits inside a currently mounted drive and contains no "..".
 * Every filesystem call from the web layer must pass this first. */
bool usbstore_path_ok(const char *path);

/* FATFS and the MSC driver are not reentrant, and a drive can be yanked
 * mid-read. Every filesystem access is wrapped in this lock. */
bool usbstore_lock(uint32_t timeout_ms);
void usbstore_unlock(void);
