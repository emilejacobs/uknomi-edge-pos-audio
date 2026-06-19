// Durable on-SD spool for envelopes that couldn't be published (WiFi/broker down).
// Bounded ring: once max_files is reached, the oldest spooled file is dropped.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t spool_init(const char *dir, int max_files);

// Persist one envelope. Drops the oldest file first if at capacity.
esp_err_t spool_write(const uint8_t *data, size_t len);

// Visit each spooled envelope oldest-first. If cb returns ESP_OK the file is
// deleted (successfully re-published); otherwise iteration stops (still offline).
typedef esp_err_t (*spool_cb_t)(const uint8_t *data, size_t len, void *ctx);
void spool_flush(spool_cb_t cb, void *ctx);

int spool_count(void);
