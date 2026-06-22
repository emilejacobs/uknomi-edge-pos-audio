// Wi-Fi web config portal. In setup mode (no/incomplete config) the device is a
// SoftAP with a captive portal; in normal mode the same settings page is served
// on the LAN so you can reconfigure without pulling the SD card. Saving writes
// /sdcard/config.json and reboots.
#pragma once

#include <stdbool.h>
#include "config_sd.h"
#include "esp_err.h"

// Start the HTTP server (non-blocking). If `captive`, also start a DNS hijack so
// the captive portal opens automatically on phones/laptops. `cfg` is the current
// config used to pre-fill the form; the save handler overlays edits onto a copy.
esp_err_t webconfig_start(uknomi_config_t *cfg, bool captive);
