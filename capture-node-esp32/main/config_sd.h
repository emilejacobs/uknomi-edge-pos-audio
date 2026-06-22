// SD-card configuration: identity + secrets + tunables, read from /sdcard/config.json
// at boot and editable via the Wi-Fi web portal. One firmware image serves the
// whole fleet; per-unit values live here.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char store[32];
    char reg[32];
    char broker_host[64];
    int broker_port;
    char broker_user[64];
    char broker_pass[64];
    float vad_threshold;
    int preroll_ms;
    int hangover_ms;
    char codec[16];
    bool retain_audio;
    int spool_max_files;
    // Camera presence detection (optional, additive — see presence.h).
    bool presence_enabled;
    int presence_hold_ms;       // keep "present" this long after last motion
    float presence_sensitivity; // motion threshold (mean abs frame diff, 0..255)
} uknomi_config_t;

// Mount the microSD (SPI) at the given mount point. Pins come from Kconfig.
esp_err_t uknomi_sd_mount(const char *mount_point);

// Populate `out` with defaults, then overlay any values found in the JSON file.
// Always leaves `out` in a valid (default-filled) state; returns ESP_ERR_NOT_FOUND
// if the file is absent and ESP_OK if it was read (complete or not).
esp_err_t uknomi_config_load(const char *path, uknomi_config_t *out);

// True once the config has the minimum needed to operate (Wi-Fi SSID + broker host).
bool uknomi_config_is_complete(const uknomi_config_t *cfg);

// Serialize `cfg` to config.json at `path` (used by the web portal on save).
esp_err_t uknomi_config_save(const char *path, const uknomi_config_t *cfg);
