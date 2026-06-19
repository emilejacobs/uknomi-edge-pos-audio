// MQTT publisher: QoS-1 publish of envelopes, with SD spool fallback + flush.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config_sd.h"
#include "esp_err.h"

esp_err_t publisher_start(const uknomi_config_t *cfg);
bool publisher_is_connected(void);

// Publish one envelope. If the broker is unreachable, the envelope is spooled to
// SD and flushed on reconnect. The caller retains ownership of `data`.
void publisher_send(const uint8_t *data, size_t len);
