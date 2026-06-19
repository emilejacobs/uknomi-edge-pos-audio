#include "audio_capture.h"

#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";
static i2s_chan_handle_t s_rx = NULL;

esp_err_t audio_capture_init(int sample_rate, int clk_gpio, int din_gpio) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (ret != ESP_OK) return ret;

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = clk_gpio,
            .din = din_gpio,
            .invert_flags = {.clk_inv = false},
        },
    };
    ret = i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2s_channel_enable(s_rx);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "PDM mic up @ %d Hz (clk=%d din=%d)", sample_rate, clk_gpio, din_gpio);
    return ESP_OK;
}

int audio_capture_read(int16_t *buf, int samples) {
    size_t want = (size_t)samples * sizeof(int16_t);
    size_t got = 0;
    esp_err_t ret = i2s_channel_read(s_rx, buf, want, &got, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "i2s read error: %s", esp_err_to_name(ret));
        return 0;
    }
    return (int)(got / sizeof(int16_t));
}
