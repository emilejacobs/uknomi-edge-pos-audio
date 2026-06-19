#include "spool.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "spool";

#define SPOOL_MAX_SCAN 1024

static char s_dir[96];
static int s_max_files;
static uint32_t s_next;  // next filename index (monotonic)

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

// Enumerate spooled file indices, ascending (oldest first). Returns the count.
static int list_indices(uint32_t *out, int cap) {
    DIR *d = opendir(s_dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < cap) {
        uint32_t idx;
        if (sscanf(e->d_name, "%" SCNu32 ".bin", &idx) == 1) out[n++] = idx;
    }
    closedir(d);
    qsort(out, n, sizeof(uint32_t), cmp_u32);
    return n;
}

static void path_for(uint32_t idx, char *buf, size_t len) {
    snprintf(buf, len, "%s/%08" PRIu32 ".bin", s_dir, idx);
}

esp_err_t spool_init(const char *dir, int max_files) {
    strncpy(s_dir, dir, sizeof(s_dir) - 1);
    s_dir[sizeof(s_dir) - 1] = '\0';
    s_max_files = max_files > 0 ? max_files : 500;
    mkdir(s_dir, 0777);  // harmless if it exists

    uint32_t idx[SPOOL_MAX_SCAN];
    int n = list_indices(idx, SPOOL_MAX_SCAN);
    s_next = n > 0 ? idx[n - 1] + 1 : 0;
    ESP_LOGI(TAG, "spool at %s: %d queued, next=%" PRIu32 ", max=%d", s_dir, n, s_next, s_max_files);
    return ESP_OK;
}

int spool_count(void) {
    uint32_t idx[SPOOL_MAX_SCAN];
    return list_indices(idx, SPOOL_MAX_SCAN);
}

esp_err_t spool_write(const uint8_t *data, size_t len) {
    uint32_t idx[SPOOL_MAX_SCAN];
    int n = list_indices(idx, SPOOL_MAX_SCAN);
    char path[160];
    // Drop oldest files until under the cap (bounded ring).
    while (n >= s_max_files && n > 0) {
        path_for(idx[0], path, sizeof(path));
        remove(path);
        memmove(idx, idx + 1, (n - 1) * sizeof(uint32_t));
        n--;
        ESP_LOGW(TAG, "spool full — dropped oldest");
    }

    path_for(s_next++, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", path);
        return ESP_FAIL;
    }
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    return wrote == len ? ESP_OK : ESP_FAIL;
}

void spool_flush(spool_cb_t cb, void *ctx) {
    uint32_t idx[SPOOL_MAX_SCAN];
    int n = list_indices(idx, SPOOL_MAX_SCAN);
    char path[160];
    for (int i = 0; i < n; i++) {
        path_for(idx[i], path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            fclose(f);
            remove(path);
            continue;
        }
        uint8_t *buf = malloc(size);
        if (!buf) {
            fclose(f);
            return;  // out of memory — try again later
        }
        size_t got = fread(buf, 1, size, f);
        fclose(f);
        esp_err_t ret = (got == (size_t)size) ? cb(buf, got, ctx) : ESP_FAIL;
        free(buf);
        if (ret == ESP_OK) {
            remove(path);  // re-published — drop from spool
        } else {
            return;  // still offline — stop, retry later
        }
    }
}
