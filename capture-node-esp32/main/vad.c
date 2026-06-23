#include "vad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "vad";

#if CONFIG_UKNOMI_VAD_ESPSR
#include "esp_vad.h"  // ESP-SR's WebRTC-based VAD (vad_create/vad_process/vad_destroy)
#define VAD_FRAME_MS 30  // WebRTC VAD requires 10/20/30 ms frames
#endif

struct uknomi_vad {
    int frame_samples;
    int sample_rate;
    float threshold;
#if CONFIG_UKNOMI_VAD_ESPSR
    vad_handle_t handle;
#endif
};

#if CONFIG_UKNOMI_VAD_ESPSR
// Map a 0..1 threshold knob onto ESP-SR aggressiveness modes VAD_MODE_0..4
// (higher = more aggressive about rejecting non-speech).
static vad_mode_t map_mode(float threshold) {
    int m = (int)lroundf(threshold * 4.0f);
    if (m < 0) m = 0;
    if (m > 4) m = 4;
    return (vad_mode_t)m;
}
#endif

uknomi_vad_t *uknomi_vad_create(int sample_rate, float threshold) {
    uknomi_vad_t *v = calloc(1, sizeof(uknomi_vad_t));
    if (!v) return NULL;
    v->sample_rate = sample_rate;
    v->threshold = threshold;

#if CONFIG_UKNOMI_VAD_ESPSR
    v->frame_samples = sample_rate / 1000 * VAD_FRAME_MS;  // 480 @ 16 kHz
    v->handle = vad_create(map_mode(threshold));
    if (!v->handle) {
        ESP_LOGE(TAG, "vad_create failed");
        free(v);
        return NULL;
    }
    ESP_LOGI(TAG, "ESP-SR VAD: mode=%d frame=%d samples (%d ms)",
             (int)map_mode(threshold), v->frame_samples, VAD_FRAME_MS);
#else
    v->frame_samples = sample_rate / 50;  // 20 ms frames
    ESP_LOGI(TAG, "energy VAD: frame=%d samples, rms threshold=%.3f",
             v->frame_samples, v->threshold);
#endif
    return v;
}

int uknomi_vad_frame_samples(const uknomi_vad_t *v) { return v->frame_samples; }

bool uknomi_vad_process(uknomi_vad_t *v, const int16_t *in, int16_t *out, int n) {
    if (out && out != in) memcpy(out, in, n * sizeof(int16_t));
#if CONFIG_UKNOMI_VAD_ESPSR
    // esp_vad has no noise suppression, so the recorded samples are the raw input
    // (already copied to `out` above). Returns VAD_SILENCE / VAD_SPEECH.
    vad_state_t st = vad_process(v->handle, (int16_t *)in, v->sample_rate, VAD_FRAME_MS);
    return st == VAD_SPEECH;
#else
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        double s = (double)in[i] / 32768.0;
        sum += s * s;
    }
    return sqrt(sum / n) > v->threshold;
#endif
}

void uknomi_vad_destroy(uknomi_vad_t *v) {
    if (!v) return;
#if CONFIG_UKNOMI_VAD_ESPSR
    if (v->handle) vad_destroy(v->handle);
#endif
    free(v);
}
