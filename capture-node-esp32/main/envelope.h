// Segment envelope encoder — mirrors aggregator/uknomi_aggregator/envelope.py.
// Layout: [4-byte big-endian uint32 header_len][UTF-8 JSON header][PCM16 audio].
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UKNOMI_SCHEMA "uknomi.segment/1"

// Build one envelope into a freshly malloc'd buffer (caller frees). Returns NULL
// on allocation failure. *out_len receives the total byte length.
uint8_t *envelope_build(const char *store, const char *reg, uint32_t seq,
                        const char *start_utc, const char *end_utc,
                        int sample_rate, const char *codec, const char *vad,
                        bool retain_audio, const int16_t *pcm, int n_samples,
                        size_t *out_len);
