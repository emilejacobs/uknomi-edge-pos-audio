#include "envelope.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

uint8_t *envelope_build(const char *store, const char *reg, uint32_t seq,
                        const char *start_utc, const char *end_utc,
                        int sample_rate, const char *codec, const char *vad,
                        bool retain_audio, int presence,
                        const int16_t *pcm, int n_samples, size_t *out_len) {
    cJSON *h = cJSON_CreateObject();
    if (!h) return NULL;
    cJSON_AddStringToObject(h, "schema", UKNOMI_SCHEMA);
    cJSON_AddStringToObject(h, "store", store);
    cJSON_AddStringToObject(h, "register", reg);
    cJSON_AddNumberToObject(h, "seq", (double)seq);
    cJSON_AddStringToObject(h, "start_utc", start_utc);
    cJSON_AddStringToObject(h, "end_utc", end_utc);
    cJSON_AddNumberToObject(h, "sample_rate", sample_rate);
    cJSON_AddStringToObject(h, "codec", codec);
    cJSON_AddStringToObject(h, "vad", vad);
    cJSON_AddBoolToObject(h, "retain_audio", retain_audio);
    if (presence >= 0) cJSON_AddBoolToObject(h, "presence", presence != 0);

    char *json = cJSON_PrintUnformatted(h);
    cJSON_Delete(h);
    if (!json) return NULL;

    size_t hlen = strlen(json);
    size_t alen = (size_t)n_samples * sizeof(int16_t);
    size_t total = 4 + hlen + alen;
    uint8_t *buf = malloc(total);
    if (!buf) {
        cJSON_free(json);
        return NULL;
    }
    // 4-byte big-endian header length, then JSON, then little-endian PCM16 bytes.
    buf[0] = (uint8_t)(hlen >> 24);
    buf[1] = (uint8_t)(hlen >> 16);
    buf[2] = (uint8_t)(hlen >> 8);
    buf[3] = (uint8_t)(hlen);
    memcpy(buf + 4, json, hlen);
    memcpy(buf + 4 + hlen, pcm, alen);
    cJSON_free(json);

    *out_len = total;
    return buf;
}
