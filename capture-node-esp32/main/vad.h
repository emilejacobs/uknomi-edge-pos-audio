// Voice Activity Detection behind a small interface so the engine is swappable.
// Default: ESP-SR AFE (noise suppression + VAD). Fallback: energy threshold.
// VAD only decides speech/non-speech per frame — it does NOT transcribe.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct vad_t vad_t;

vad_t *vad_create(int sample_rate, float threshold);

// Number of int16 samples each vad_process() call expects (the engine's feed
// chunk size). The audio loop must read exactly this many samples per frame.
int vad_frame_samples(const vad_t *v);

// Process one frame. Returns true if speech is present. `out` (may be NULL or
// alias `in`) receives the cleaned samples to record; if NULL, `in` is recorded.
bool vad_process(vad_t *v, const int16_t *in, int16_t *out, int n);

void vad_destroy(vad_t *v);
