/*
 * The system drive: one pendrive, chosen once from the web page, that holds
 * UsbStore's own data - climate logs and the planner - in a folder at its
 * root.
 *
 * It is remembered by USB serial number, not by /usbN: drive numbers follow
 * plug-in order, so after a replug "/usb1" can be a different stick. Two
 * identical Cruzer Blades still have different serials.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * Exactly 8 characters, on purpose. FAT gives long names a short 8.3 alias
 * ("usbstore-system" would also answer to "USBSTO~1"), and an alias would
 * walk straight past a name check. A name that already fits 8.3 has none.
 */
#define SYSDRIVE_DIR "usbstore"

esp_err_t sysdrive_init(void);

/* The designated drive's serial, whether or not it is plugged in. */
bool sysdrive_serial(char *out, size_t len);

/* "/usbN/usbstore/<sub>" if the system drive is plugged in; false if not
 * designated or absent. The caller still has to usbstore_acquire() it. */
bool sysdrive_path(const char *sub, char *out, size_t len);

/* True if path is inside a "usbstore" folder at the root of ANY drive.
 * The file browser keeps out of these: the planner lives there, and it
 * needs the password even to read. Case-insensitive, like FAT itself. */
bool sysdrive_is_protected(const char *path);

/* mkdir -p for an absolute path on a drive the caller holds a ref on. */
esp_err_t sysdrive_mkdirs(const char *path);

void sysdrive_routes(httpd_handle_t srv);
