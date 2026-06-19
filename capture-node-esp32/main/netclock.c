#include "netclock.h"

#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "netclock";

// Any epoch beyond this (2023-11-14) means the clock has been set from NTP.
#define UKNOMI_SYNCED_AFTER 1700000000LL

void netclock_start(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started; gating capture until time is synced");
}

int64_t netclock_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool netclock_is_synced(void) {
    return netclock_now_ms() / 1000 > UKNOMI_SYNCED_AFTER;
}

void netclock_iso8601(int64_t epoch_ms, char *buf, size_t len) {
    time_t secs = epoch_ms / 1000;
    int ms = (int)(epoch_ms % 1000);
    struct tm tm_utc;
    gmtime_r(&secs, &tm_utc);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, ms);
}
