#include "vad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "vad";

#if CONFIG_UKNOMI_VAD_AFE
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#endif

struct vad_t {
    int frame_samples;
    float threshold;
#if CONFIG_UKNOMI_VAD_AFE
    esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t *afe_data;
#endif
};

vad_t *vad_create(int sample_rate, float threshold) {
    vad_t *v = calloc(1, sizeof(vad_t));
    if (!v) return NULL;
    v->threshold = threshold;

#if CONFIG_UKNOMI_VAD_AFE
    // ESP-SR AFE: single mic ("M"), noise suppression + VAD, no AEC/wakenet.
    // NOTE: the esp-sr AFE config/result API has shifted across versions — this
    // targets the afe_config_init() interface (esp-sr v2.x). Validate the exact
    // symbol/enum names against the pinned component version on first device build.
    afe_config_t *cfg = afe_config_init("M", NULL, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    cfg->aec_init = false;
    cfg->se_init = true;   // noise suppression — partially offsets the omni mic
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_3;
    cfg->wakenet_init = false;
    v->afe = esp_afe_handle_from_config(cfg);
    v->afe_data = v->afe->create_from_config(cfg);
    v->frame_samples = v->afe->get_feed_chunksize(v->afe_data);
    afe_config_free(cfg);
    ESP_LOGI(TAG, "ESP-SR AFE VAD: feed=%d samples", v->frame_samples);
#else
    (void)sample_rate;
    v->frame_samples = sample_rate / 50;  // 20 ms frames
    ESP_LOGI(TAG, "energy VAD: frame=%d samples, rms threshold=%.3f",
             v->frame_samples, v->threshold);
#endif
    return v;
}

int vad_frame_samples(const vad_t *v) { return v->frame_samples; }

bool vad_process(vad_t *v, const int16_t *in, int16_t *out, int n) {
#if CONFIG_UKNOMI_VAD_AFE
    v->afe->feed(v->afe_data, in);
    afe_fetch_result_t *r = v->afe->fetch(v->afe_data);
    bool speech = false;
    if (r && r->ret_value != ESP_FAIL) {
        speech = (r->vad_state == VAD_SPEECH);
        if (out && r->data) {
            int copy = (r->data_size / (int)sizeof(int16_t));
            if (copy > n) copy = n;
            memcpy(out, r->data, copy * sizeof(int16_t));
        }
    }
    return speech;
#else
    // RMS energy in [0,1] vs threshold (interpreted as an RMS cutoff, ~0.02).
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        double s = (double)in[i] / 32768.0;
        sum += s * s;
    }
    double rms = sqrt(sum / n);
    if (out && out != in) memcpy(out, in, n * sizeof(int16_t));
    return rms > v->threshold;
#endif
}

void vad_destroy(vad_t *v) {
    if (!v) return;
#if CONFIG_UKNOMI_VAD_AFE
    if (v->afe && v->afe_data) v->afe->destroy(v->afe_data);
#endif
    free(v);
}
