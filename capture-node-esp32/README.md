# Capture node — XIAO ESP32-S3 Sense (ESP-IDF firmware)

Captures the onboard PDM microphone, segments speech with VAD, and ships each
utterance to the Mac aggregator over MQTT. See [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
for the wire contract this firmware implements.

```
I2S PDM mic ─▶ VAD (ESP-SR esp_vad) ─▶ segmenter (pre-roll + hangover)
            ─▶ PCM16 envelope ─▶ MQTT publish ─▶ (SD spool on outage)
OV2640 cam  ─▶ motion presence (optional) ─▶ tags each utterance present=yes/no
```

## Prerequisites

- **ESP-IDF v5.2+** installed (`. $IDF_PATH/export.sh`).
- A XIAO ESP32-S3 **Sense** (the expansion board provides the PDM mic + microSD).
- A FAT-formatted microSD card.

## Build & flash (per device, once)

```bash
idf.py set-target esp32s3
idf.py build
./flash.sh /dev/cu.usbmodemXXXX      # or /dev/ttyACM0 on Linux
```

## Provision (per unit — no recompile)

Two ways to set Wi-Fi, identity, broker, and tunables:

**A. Wi-Fi web portal (no card editing).** On first boot with no/incomplete
`/config.json`, the device comes up as an open SoftAP **`uknomi-setup-XXXX`**.
Join it and a captive portal opens (or browse to `http://192.168.4.1`); fill in
the form and **Save & reboot**. The settings are written to `/config.json` on the
card. Once it's on your Wi-Fi, the **same page stays reachable on the LAN at the
device's IP**, so you can reconfigure later without pulling the card.

**B. Pre-seed the card.** Copy `config.example.json` to the SD card as
**`/config.json`** and fill it in. One firmware binary serves the whole fleet;
secrets live only on each card.

Watch it come up: `idf.py -p <port> monitor`. On boot it mounts the SD, reads
config (or starts the setup portal), joins Wi-Fi, **waits for NTP sync**
(timestamps are load-bearing), then starts capturing. If the broker is
unreachable, utterances spool to `/spool/*.bin` on the card and flush on reconnect.

## Camera presence detection (optional, default off)

Set `presence.enabled = true` (config or web portal) to use the onboard OV2640 to
tag each utterance with `presence: true/false` — "was a customer in front of the
counter?" This is an **additive** second boundary/confidence signal for the LLM
order-splitter (helps drop coworker chatter and neighbour-register crosstalk); it
does **not** gate recording, and **frames are never stored or transmitted** —
only an on-device boolean leaves the chip.

- v1 engine is **motion + hold** (frame-differencing with a hold window). It
  detects movement, not a perfectly still person; `presence.hold_ms` keeps
  "present" true after the last motion. A person-detection model is the upgrade.
- Tune `presence.sensitivity` (mean-abs frame-diff threshold) on the real counter.
- Privacy/consent: a customer-facing **camera** is a much bigger consent surface
  than audio — even boolean-only. Confirm signage/consent before enabling live.

## Configuration knobs (`idf.py menuconfig` → "uKnomi Capture Node")

- **VAD engine** — `ESP-SR VAD` (default; `esp_vad.h`, WebRTC-based, aggressiveness
  mapped from the config `vad.threshold`) or an `energy threshold` fallback for
  bring-up / A/B.
- **PDM mic pins** — default CLK=42, DIN=41 (XIAO Sense).
- **microSD SPI pins** — default SCK=7, MISO=8, MOSI=9, CS=21 (XIAO Sense).
- **Sample rate**, **max utterance length**.

## Build status & on-device validation

The firmware **compiles cleanly** against ESP-IDF v5.2 + ESP-SR 2.4.6 +
esp32-camera 2.1.7 (verified in the `espressif/idf:release-v5.2` container; CI
builds the same way). It has **not yet been run on a physical board** — still to
confirm on hardware:

- **Board pin assignments** — verify the PDM mic (CLK=42, DIN=41), microSD SPI
  (SCK=7, MISO=8, MOSI=9, CS=21), and the OV2640 camera pins (`camera_pins.h`)
  against your Sense revision.
- **VAD tuning** — `esp_vad` has no noise suppression; tune `vad.threshold`
  (→ aggressiveness mode) on the real counter. If you later need denoising, the
  heavier ESP-SR **AFE** is the upgrade path (a separate engine, not wired here).
- **Presence tuning** — `presence.sensitivity` and `hold_ms` on the real counter.
- **Web portal / SoftAP setup flow** — exercise the captive portal + save+reboot.

The known v1 limitation stands: the onboard mic is **omnidirectional**, so
adjacent-register crosstalk will be worse than a directional setup — fine for
proving VAD + transcription + order-split, not representative for crosstalk.
