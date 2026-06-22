// Camera presence detection (optional, additive). Answers "was a customer in
// front of the counter during this utterance?" as a boolean tag in the segment
// envelope — a second weak boundary/confidence signal for the LLM order-splitter
// (helps separate real orders from coworker chatter and neighbour-register
// crosstalk). It does NOT gate recording, and frames are never stored or sent.
//
// v1 engine is motion + hold: frame-differencing detects movement (a customer
// walking up / shuffling / paying), and a hold window keeps "present" true after
// the last motion so a momentarily-still customer still counts. Frame-diff does
// NOT detect a perfectly still person — a person-detection model is the upgrade.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Init the camera and start the detector task. threshold = mean-abs frame diff
// (0..255) above which a frame counts as motion; hold_ms keeps presence true
// after the last motion.
esp_err_t presence_init(float threshold, int hold_ms);

bool presence_running(void);

// True if motion was seen within hold_ms before/around [start_ms, end_ms].
bool presence_active_during(int64_t start_ms, int64_t end_ms);
