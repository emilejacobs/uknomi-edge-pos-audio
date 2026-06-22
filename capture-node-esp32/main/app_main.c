// uKnomi capture node — XIAO ESP32-S3 Sense.
//
// Boot: mount SD -> read /config.json -> Wi-Fi -> SNTP (gate on sync) -> MQTT.
// Then a single audio task: I2S PDM -> VAD -> segmenter -> envelope -> publish
// (with SD spool fallback). Identity + secrets live only on the SD card, so one
// firmware image serves the whole fleet.

#include <stdlib.h>
#include <string.h>

#include "audio_capture.h"
#include "config_sd.h"
#include "envelope.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "netclock.h"
#include "nvs_flash.h"
#include "presence.h"
#include "publisher.h"
#include "sdkconfig.h"
#include "segmenter.h"
#include "spool.h"
#include "vad.h"
#include "webconfig.h"

static const char *TAG = "app";
#define MOUNT_POINT "/sdcard"

#if CONFIG_UKNOMI_VAD_ESPSR
#define VAD_NAME "esp_sr_vad"
#else
#define VAD_NAME "energy"
#endif

static uknomi_config_t s_cfg;

// ---- sequence numbers (durable, reboot-safe via NVS reservation) ------------
#define SEQ_RESERVE 100
static nvs_handle_t s_nvs;
static uint32_t s_seq, s_seq_reserved;

static void seq_init(void) {
    ESP_ERROR_CHECK(nvs_open("uknomi", NVS_READWRITE, &s_nvs));
    uint32_t base = 0;
    nvs_get_u32(s_nvs, "seq", &base);  // absent on first boot -> 0
    s_seq = base;
    s_seq_reserved = base + SEQ_RESERVE;
    nvs_set_u32(s_nvs, "seq", s_seq_reserved);
    nvs_commit(s_nvs);
}

static uint32_t seq_next(void) {
    if (s_seq >= s_seq_reserved) {
        s_seq_reserved += SEQ_RESERVE;
        nvs_set_u32(s_nvs, "seq", s_seq_reserved);
        nvs_commit(s_nvs);
    }
    return s_seq++;
}

// ---- Wi-Fi station ----------------------------------------------------------
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();  // auto-reconnect
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi got IP");
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// One-time Wi-Fi/netif stack init shared by both AP setup and STA modes.
static void net_init(void) {
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
}

static void wifi_start_sta(const uknomi_config_t *cfg) {
    esp_netif_create_default_wifi_sta();
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, cfg->wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, cfg->wifi_pass, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to Wi-Fi \"%s\"...", cfg->wifi_ssid);
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// SoftAP for first-time / reconfig setup. Open AP named uknomi-setup-XXXX.
static void wifi_start_ap(void) {
    esp_netif_create_default_wifi_ap();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = {0};
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "uknomi-setup-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGW(TAG, "SETUP MODE — join Wi-Fi \"%s\" and open http://192.168.4.1", ap.ap.ssid);
}

// ---- segment -> envelope -> publish ----------------------------------------
static void on_segment(const int16_t *samples, int n, int64_t start_ms, int64_t end_ms,
                       void *ctx) {
    (void)ctx;
    char start_utc[25], end_utc[25];
    netclock_iso8601(start_ms, start_utc, sizeof(start_utc));
    netclock_iso8601(end_ms, end_utc, sizeof(end_utc));

    int presence = -1;  // omit unless the camera detector is running
    if (presence_running()) presence = presence_active_during(start_ms, end_ms) ? 1 : 0;

    size_t len = 0;
    uint8_t *env = envelope_build(s_cfg.store, s_cfg.reg, seq_next(), start_utc, end_utc,
                                  CONFIG_UKNOMI_SAMPLE_RATE, s_cfg.codec, VAD_NAME,
                                  s_cfg.retain_audio, presence, samples, n, &len);
    if (!env) {
        ESP_LOGE(TAG, "envelope build failed (out of memory?)");
        return;
    }
    publisher_send(env, len);
    free(env);
    ESP_LOGI(TAG, "utterance %s (%d samples, %.1fs)", start_utc, n,
             n / (float)CONFIG_UKNOMI_SAMPLE_RATE);
}

// ---- audio task -------------------------------------------------------------
static void audio_task(void *arg) {
    uknomi_vad_t *vad = (uknomi_vad_t *)arg;
    int fs = uknomi_vad_frame_samples(vad);
    int16_t *frame = malloc(fs * sizeof(int16_t));
    int16_t *clean = malloc(fs * sizeof(int16_t));
    segmenter_t *seg = segmenter_create(CONFIG_UKNOMI_SAMPLE_RATE, fs, s_cfg.preroll_ms,
                                        s_cfg.hangover_ms, CONFIG_UKNOMI_MAX_UTTERANCE_MS,
                                        on_segment, NULL);
    if (!frame || !clean || !seg) {
        ESP_LOGE(TAG, "audio task alloc failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "capture loop running (%s VAD, %d-sample frames)", VAD_NAME, fs);
    for (;;) {
        int got = audio_capture_read(frame, fs);
        if (got != fs) continue;
        bool speech = uknomi_vad_process(vad, frame, clean, fs);
        segmenter_push(seg, clean, fs, speech, netclock_now_ms());
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    if (uknomi_sd_mount(MOUNT_POINT) != ESP_OK) {
        ESP_LOGE(TAG, "no SD card — setup portal will load but cannot save config");
    }
    uknomi_config_load(MOUNT_POINT "/config.json", &s_cfg);  // always fills defaults
    net_init();

    // First-time / incomplete config -> Wi-Fi setup portal, then wait for save+reboot.
    if (!uknomi_config_is_complete(&s_cfg)) {
        wifi_start_ap();
        ESP_ERROR_CHECK(webconfig_start(&s_cfg, true));
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    seq_init();
    wifi_start_sta(&s_cfg);
    webconfig_start(&s_cfg, false);  // LAN settings page (reconfigure without the card)

    netclock_start();
    ESP_LOGI(TAG, "waiting for NTP sync before capturing (clock is load-bearing)...");
    while (!netclock_is_synced()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "clock synced");

    ESP_ERROR_CHECK(spool_init(MOUNT_POINT "/spool", s_cfg.spool_max_files));
    ESP_ERROR_CHECK(publisher_start(&s_cfg));

    if (s_cfg.presence_enabled) {
        if (presence_init(s_cfg.presence_sensitivity, s_cfg.presence_hold_ms) != ESP_OK) {
            ESP_LOGW(TAG, "presence init failed — continuing audio-only");
        }
    }

    ESP_ERROR_CHECK(audio_capture_init(CONFIG_UKNOMI_SAMPLE_RATE,
                                       CONFIG_UKNOMI_PDM_CLK_GPIO, CONFIG_UKNOMI_PDM_DIN_GPIO));
    uknomi_vad_t *vad = uknomi_vad_create(CONFIG_UKNOMI_SAMPLE_RATE, s_cfg.vad_threshold);
    if (!vad) {
        ESP_LOGE(TAG, "VAD init failed");
        return;
    }
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, vad, 5, NULL, 1);
}
