// OV2640 init for presence detection — low-res grayscale, just enough to tell
// whether someone is in front of the counter. Frames are never stored or sent.
#pragma once

#include "esp_err.h"

esp_err_t camera_init(void);
