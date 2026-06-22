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

static void set_defaults(uknomi_config_t *out) {
    memset(out, 0, sizeof(*out));
    strcpy(out->store, "store00");
    strcpy(out->reg, "reg0");
    out->broker_port = 1883;
    out->vad_threshold = 0.6f;
    out->preroll_ms = 400;
    out->hangover_ms = 700;
    strcpy(out->codec, "pcm16");
    out->retain_audio = false;
    out->spool_max_files = 500;
    out->presence_enabled = false;
    out->presence_hold_ms = 30000;
    out->presence_sensitivity = 8.0f;
}

esp_err_t uknomi_config_load(const char *path, uknomi_config_t *out) {
    set_defaults(out);  // always leave a valid struct, even with no/empty card

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "%s not found — starting Wi-Fi setup portal", path);
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

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    get_str(wifi, "ssid", out->wifi_ssid, sizeof(out->wifi_ssid), out->wifi_ssid);
    get_str(wifi, "password", out->wifi_pass, sizeof(out->wifi_pass), out->wifi_pass);

    get_str(root, "store", out->store, sizeof(out->store), out->store);
    get_str(root, "register", out->reg, sizeof(out->reg), out->reg);

    const cJSON *broker = cJSON_GetObjectItemCaseSensitive(root, "broker");
    get_str(broker, "host", out->broker_host, sizeof(out->broker_host), out->broker_host);
    out->broker_port = get_int(broker, "port", out->broker_port);
    get_str(broker, "username", out->broker_user, sizeof(out->broker_user), out->broker_user);
    get_str(broker, "password", out->broker_pass, sizeof(out->broker_pass), out->broker_pass);

    const cJSON *vad = cJSON_GetObjectItemCaseSensitive(root, "vad");
    out->vad_threshold = (float)get_num(vad, "threshold", out->vad_threshold);
    out->preroll_ms = get_int(vad, "preroll_ms", out->preroll_ms);
    out->hangover_ms = get_int(vad, "hangover_ms", out->hangover_ms);

    get_str(root, "codec", out->codec, sizeof(out->codec), out->codec);
    out->retain_audio = get_bool(root, "retain_audio", out->retain_audio);

    const cJSON *spool = cJSON_GetObjectItemCaseSensitive(root, "spool");
    out->spool_max_files = get_int(spool, "max_files", out->spool_max_files);

    const cJSON *presence = cJSON_GetObjectItemCaseSensitive(root, "presence");
    out->presence_enabled = get_bool(presence, "enabled", out->presence_enabled);
    out->presence_hold_ms = get_int(presence, "hold_ms", out->presence_hold_ms);
    out->presence_sensitivity = (float)get_num(presence, "sensitivity", out->presence_sensitivity);

    cJSON_Delete(root);
    ESP_LOGI(TAG, "config: store=%s register=%s broker=%s:%d retain_audio=%d presence=%d",
             out->store, out->reg, out->broker_host, out->broker_port,
             out->retain_audio, out->presence_enabled);
    return ESP_OK;
}

bool uknomi_config_is_complete(const uknomi_config_t *cfg) {
    return cfg->wifi_ssid[0] != '\0' && cfg->broker_host[0] != '\0';
}

esp_err_t uknomi_config_save(const char *path, const uknomi_config_t *cfg) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(wifi, "password", cfg->wifi_pass);
    cJSON_AddStringToObject(root, "store", cfg->store);
    cJSON_AddStringToObject(root, "register", cfg->reg);

    cJSON *broker = cJSON_AddObjectToObject(root, "broker");
    cJSON_AddStringToObject(broker, "host", cfg->broker_host);
    cJSON_AddNumberToObject(broker, "port", cfg->broker_port);
    cJSON_AddStringToObject(broker, "username", cfg->broker_user);
    cJSON_AddStringToObject(broker, "password", cfg->broker_pass);

    cJSON *vad = cJSON_AddObjectToObject(root, "vad");
    cJSON_AddNumberToObject(vad, "threshold", cfg->vad_threshold);
    cJSON_AddNumberToObject(vad, "preroll_ms", cfg->preroll_ms);
    cJSON_AddNumberToObject(vad, "hangover_ms", cfg->hangover_ms);

    cJSON_AddStringToObject(root, "codec", cfg->codec);
    cJSON_AddBoolToObject(root, "retain_audio", cfg->retain_audio);

    cJSON *spool = cJSON_AddObjectToObject(root, "spool");
    cJSON_AddNumberToObject(spool, "max_files", cfg->spool_max_files);

    cJSON *presence = cJSON_AddObjectToObject(root, "presence");
    cJSON_AddBoolToObject(presence, "enabled", cfg->presence_enabled);
    cJSON_AddNumberToObject(presence, "hold_ms", cfg->presence_hold_ms);
    cJSON_AddNumberToObject(presence, "sensitivity", cfg->presence_sensitivity);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    FILE *f = fopen(path, "wb");
    if (!f) {
        cJSON_free(json);
        ESP_LOGE(TAG, "cannot write %s", path);
        return ESP_FAIL;
    }
    fwrite(json, 1, strlen(json), f);
    fclose(f);
    cJSON_free(json);
    ESP_LOGI(TAG, "saved config to %s", path);
    return ESP_OK;
}
