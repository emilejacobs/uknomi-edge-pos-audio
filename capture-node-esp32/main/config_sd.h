// SD-card configuration: identity + secrets + tunables, read from /sdcard/config.json
// at boot. One firmware image serves the whole fleet; per-unit values live here.
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
} uknomi_config_t;

// Mount the microSD (SPI) at the given mount point. Pins come from Kconfig.
esp_err_t uknomi_sd_mount(const char *mount_point);

// Parse a config.json file into `out`. Missing optional fields get sane defaults.
esp_err_t uknomi_config_load(const char *path, uknomi_config_t *out);
