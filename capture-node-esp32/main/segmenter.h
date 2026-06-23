// Utterance segmenter: IDLE -> SPEECH -> IDLE with pre-roll + hangover.
// Pre-roll prepends the last ~400 ms so the first word isn't clipped; hangover
// waits for sustained silence before closing so a mid-sentence pause doesn't
// split one utterance (docs/ARCHITECTURE.md / PRD §7.1).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct segmenter_t segmenter_t;

// Called once per completed utterance with its PCM16 samples and capture window
// (epoch ms). The callback must copy what it needs; the buffer is reused after.
typedef void (*segment_cb_t)(const int16_t *samples, int n,
                             int64_t start_ms, int64_t end_ms, void *ctx);

segmenter_t *segmenter_create(int sample_rate, int frame_samples,
                              int preroll_ms, int hangover_ms, int max_ms,
                              segment_cb_t cb, void *ctx);

// Feed one VAD-classified frame. `now_ms` is the wall-clock end time of the frame.
void segmenter_push(segmenter_t *s, const int16_t *frame, int n,
                    bool is_speech, int64_t now_ms);

void segmenter_destroy(segmenter_t *s);
