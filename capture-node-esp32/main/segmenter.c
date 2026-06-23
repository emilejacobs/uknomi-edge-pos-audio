#include "segmenter.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "segmenter";

typedef enum { ST_IDLE, ST_SPEECH } state_t;

struct segmenter_t {
    int sample_rate;
    int hangover_frames;
    int max_samples;

    // Pre-roll ring (most recent preroll_samples), so onset doesn't clip word 1.
    int16_t *preroll;
    int preroll_cap;
    int preroll_head;   // next write index
    int preroll_count;  // valid samples in the ring

    // Utterance accumulation buffer (PSRAM).
    int16_t *utt;
    int utt_len;

    state_t state;
    int silence_frames;
    int64_t end_ms;

    segment_cb_t cb;
    void *ctx;
};

segmenter_t *segmenter_create(int sample_rate, int frame_samples, int preroll_ms,
                              int hangover_ms, int max_ms, segment_cb_t cb, void *ctx) {
    segmenter_t *s = calloc(1, sizeof(segmenter_t));
    if (!s) return NULL;
    s->sample_rate = sample_rate;
    s->cb = cb;
    s->ctx = ctx;
    s->hangover_frames = (hangover_ms * sample_rate / 1000 + frame_samples - 1) / frame_samples;
    if (s->hangover_frames < 1) s->hangover_frames = 1;
    s->max_samples = max_ms * sample_rate / 1000;
    s->preroll_cap = preroll_ms * sample_rate / 1000;

    s->preroll = malloc(s->preroll_cap * sizeof(int16_t));
    s->utt = heap_caps_malloc(s->max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s->preroll || !s->utt) {
        ESP_LOGE(TAG, "buffer alloc failed (preroll=%d utt=%d samples)",
                 s->preroll_cap, s->max_samples);
        segmenter_destroy(s);
        return NULL;
    }
    s->state = ST_IDLE;
    return s;
}

static void preroll_push(segmenter_t *s, const int16_t *frame, int n) {
    for (int i = 0; i < n; i++) {
        s->preroll[s->preroll_head] = frame[i];
        s->preroll_head = (s->preroll_head + 1) % s->preroll_cap;
        if (s->preroll_count < s->preroll_cap) s->preroll_count++;
    }
}

static void utt_append(segmenter_t *s, const int16_t *frame, int n) {
    int room = s->max_samples - s->utt_len;
    int copy = n < room ? n : room;
    if (copy > 0) {
        memcpy(s->utt + s->utt_len, frame, copy * sizeof(int16_t));
        s->utt_len += copy;
    }
}

static void utt_seed_from_preroll(segmenter_t *s) {
    // Replay the ring in chronological order into the utterance buffer.
    int start = (s->preroll_head - s->preroll_count + s->preroll_cap) % s->preroll_cap;
    for (int i = 0; i < s->preroll_count && s->utt_len < s->max_samples; i++) {
        s->utt[s->utt_len++] = s->preroll[(start + i) % s->preroll_cap];
    }
}

static void close_utterance(segmenter_t *s) {
    if (s->utt_len > 0) {
        int64_t dur_ms = (int64_t)s->utt_len * 1000 / s->sample_rate;
        int64_t start_ms = s->end_ms - dur_ms;
        s->cb(s->utt, s->utt_len, start_ms, s->end_ms, s->ctx);
    }
    s->utt_len = 0;
    s->silence_frames = 0;
    s->state = ST_IDLE;
}

void segmenter_push(segmenter_t *s, const int16_t *frame, int n, bool is_speech,
                    int64_t now_ms) {
    s->end_ms = now_ms;

    if (s->state == ST_IDLE) {
        if (is_speech) {
            s->state = ST_SPEECH;
            s->utt_len = 0;
            s->silence_frames = 0;
            utt_seed_from_preroll(s);  // pre-roll lead-in
            utt_append(s, frame, n);
        } else {
            preroll_push(s, frame, n);  // keep buffering recent silence
        }
        return;
    }

    // ST_SPEECH
    utt_append(s, frame, n);
    preroll_push(s, frame, n);  // keep ring warm for the next utterance
    if (is_speech) {
        s->silence_frames = 0;
    } else if (++s->silence_frames >= s->hangover_frames) {
        close_utterance(s);
        return;
    }
    if (s->utt_len >= s->max_samples) {
        ESP_LOGW(TAG, "max utterance length hit — forcing cut");
        close_utterance(s);
    }
}

void segmenter_destroy(segmenter_t *s) {
    if (!s) return;
    free(s->preroll);
    heap_caps_free(s->utt);
    free(s);
}
