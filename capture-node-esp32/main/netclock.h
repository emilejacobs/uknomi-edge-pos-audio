// SNTP time sync + ISO-8601 stamping. NTP is a first-class dependency:
// segmentation is gated until the clock is synced (docs/ARCHITECTURE.md §4).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void netclock_start(void);                 // kick off SNTP (call after Wi-Fi up)
bool netclock_is_synced(void);             // has the clock been set from NTP?
int64_t netclock_now_ms(void);             // wall-clock epoch milliseconds (UTC)

// Format an epoch-ms value as "YYYY-MM-DDTHH:MM:SS.mmmZ". buf must be >= 25 bytes.
void netclock_iso8601(int64_t epoch_ms, char *buf, size_t len);
