// I2S PDM-mode capture of the XIAO ESP32-S3 Sense onboard digital microphone.
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_capture_init(int sample_rate, int clk_gpio, int din_gpio);

// Block until `samples` int16 mono samples are read into `buf`.
// Returns the number of samples actually read (== samples on success).
int audio_capture_read(int16_t *buf, int samples);
