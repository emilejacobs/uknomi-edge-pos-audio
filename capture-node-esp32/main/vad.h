// Voice Activity Detection behind a small interface so the engine is swappable.
// Default: ESP-SR's WebRTC-based VAD (esp_vad.h). Fallback: energy threshold.
// VAD only decides speech/non-speech per frame — it does NOT transcribe.
//
// NOTE: symbols are prefixed uknomi_ to avoid clashing with ESP-SR's own
// global vad_create/vad_process/vad_destroy in esp_vad.h.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct uknomi_vad uknomi_vad_t;

uknomi_vad_t *uknomi_vad_create(int sample_rate, float threshold);

// Number of int16 samples each process() call expects (one VAD frame). The
// audio loop must read exactly this many samples per frame.
int uknomi_vad_frame_samples(const uknomi_vad_t *v);

// Process one frame. Returns true if speech is present. `out` (may be NULL or
// alias `in`) receives the samples to record; if NULL, `in` is recorded.
bool uknomi_vad_process(uknomi_vad_t *v, const int16_t *in, int16_t *out, int n);

void uknomi_vad_destroy(uknomi_vad_t *v);
