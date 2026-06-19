#include "config_sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char *TAG = "config_sd";

esp_err_t uknomi_sd_mount(const char *mount_point) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_UKNOMI_SD_MOSI_GPIO,
        .miso_io_num = CONFIG_UKNOMI_SD_MISO_GPIO,
        .sclk_io_num = CONFIG_UKNOMI_SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_UKNOMI_SD_CS_GPIO;
    slot_config.host_id = host.slot;

    sdmmc_card_t *card = NULL;
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (is a FAT-formatted card inserted?)",
                 esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SD mounted at %s", mount_point);
    return ESP_OK;
}

static void get_str(const cJSON *obj, const char *key, char *dst, size_t n, const char *dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    const char *s = (cJSON_IsString(v) && v->valuestring) ? v->valuestring : dflt;
    strncpy(dst, s ? s : "", n - 1);
    dst[n - 1] = '\0';
}

static int get_int(const cJSON *obj, const char *key, int dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

static double get_num(const cJSON *obj, const char *key, double dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static bool get_bool(const cJSON *obj, const char *key, bool dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return dflt;
}

esp_err_t uknomi_config_load(const char *path, uknomi_config_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8192) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read = fread(buf, 1, size, f);
    fclose(f);
    buf[read] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "config.json is not valid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    get_str(wifi, "ssid", out->wifi_ssid, sizeof(out->wifi_ssid), "");
    get_str(wifi, "password", out->wifi_pass, sizeof(out->wifi_pass), "");

    get_str(root, "store", out->store, sizeof(out->store), "store00");
    get_str(root, "register", out->reg, sizeof(out->reg), "reg0");

    const cJSON *broker = cJSON_GetObjectItemCaseSensitive(root, "broker");
    get_str(broker, "host", out->broker_host, sizeof(out->broker_host), "");
    out->broker_port = get_int(broker, "port", 1883);
    get_str(broker, "username", out->broker_user, sizeof(out->broker_user), "");
    get_str(broker, "password", out->broker_pass, sizeof(out->broker_pass), "");

    const cJSON *vad = cJSON_GetObjectItemCaseSensitive(root, "vad");
    out->vad_threshold = (float)get_num(vad, "threshold", 0.6);
    out->preroll_ms = get_int(vad, "preroll_ms", 400);
    out->hangover_ms = get_int(vad, "hangover_ms", 700);

    get_str(root, "codec", out->codec, sizeof(out->codec), "pcm16");
    out->retain_audio = get_bool(root, "retain_audio", false);

    const cJSON *spool = cJSON_GetObjectItemCaseSensitive(root, "spool");
    out->spool_max_files = get_int(spool, "max_files", 500);

    cJSON_Delete(root);

    if (out->wifi_ssid[0] == '\0' || out->broker_host[0] == '\0') {
        ESP_LOGE(TAG, "config.json missing required wifi.ssid or broker.host");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "config: store=%s register=%s broker=%s:%d retain_audio=%d",
             out->store, out->reg, out->broker_host, out->broker_port, out->retain_audio);
    return ESP_OK;
}
