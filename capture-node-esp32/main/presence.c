#include "presence.h"

#include <stdint.h>
#include <string.h>

#include "camera.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netclock.h"

static const char *TAG = "presence";

static float s_threshold;
static int s_hold_ms;
static volatile int64_t s_last_active_ms = INT64_MIN;
static uint8_t *s_prev;
static int s_npx;
static bool s_running;

static void presence_task(void *arg) {
    (void)arg;
    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            if (fb->format == PIXFORMAT_GRAYSCALE && fb->len > 0) {
                if (!s_prev) {
                    s_prev = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
                    s_npx = fb->len;
                    if (s_prev) memcpy(s_prev, fb->buf, fb->len);
                } else if ((int)fb->len == s_npx) {
                    uint64_t sum = 0;
                    for (int i = 0; i < s_npx; i++) {
                        int d = (int)fb->buf[i] - (int)s_prev[i];
                        sum += d < 0 ? -d : d;
                    }
                    double mad = (double)sum / s_npx;  // mean abs diff, 0..255
                    if (mad > s_threshold) s_last_active_ms = netclock_now_ms();
                    memcpy(s_prev, fb->buf, s_npx);
                }
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(500));  // ~2 fps — leaves the CPU for audio
    }
}

esp_err_t presence_init(float threshold, int hold_ms) {
    s_threshold = threshold;
    s_hold_ms = hold_ms;
    esp_err_t ret = camera_init();
    if (ret != ESP_OK) return ret;
    s_running = true;
    // Core 0 — audio runs on core 1, so detection never starves capture.
    xTaskCreatePinnedToCore(presence_task, "presence", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "presence motion detector running (threshold=%.1f hold=%dms)",
             threshold, hold_ms);
    return ESP_OK;
}

bool presence_running(void) { return s_running; }

bool presence_active_during(int64_t start_ms, int64_t end_ms) {
    (void)end_ms;
    if (!s_running) return false;
    int64_t last = s_last_active_ms;
    return last != INT64_MIN && last >= start_ms - s_hold_ms;
}
