/*
 * LAN file server over the mounted USB drives.
 *
 * LAN only, deliberately: no TLS and no accounts. Do not port-forward this.
 * Every request that names a path goes through usbstore_path_ok() first, so a
 * URL cannot walk outside a mounted drive.
 */

#pragma once

#include "esp_err.h"

esp_err_t web_server_start(void);
