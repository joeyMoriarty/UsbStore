/*
 * WiFi with a setup fallback.
 *
 * Credentials live in NVS, not in the source, so the firmware is not rebuilt
 * to move the box to another network. With none stored (or when the stored
 * ones fail), the board raises its own access point and the same web server
 * serves the setup page - which is how you get it onto a network the first
 * time without a console.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define WIFI_SETUP_SSID "usbstore-setup"
#define WIFI_SETUP_PASS "usbstore123"   /* WPA2 needs >= 8 chars */
#define WIFI_HOSTNAME   "usbstore"      /* http://usbstore.local */

esp_err_t wifi_mgr_start(void);

/* True when joined to a real network; false while in setup-AP mode. */
bool wifi_mgr_is_station(void);

/* Printable current state for the status endpoint. */
void wifi_mgr_status(char *out, size_t len);

/* Store credentials and reboot into station mode. Called by the setup page. */
esp_err_t wifi_mgr_provision(const char *ssid, const char *pass);
