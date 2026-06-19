# Capture node — XIAO ESP32-S3 Sense (ESP-IDF firmware)

Captures the onboard PDM microphone, segments speech with VAD, and ships each
utterance to the Mac aggregator over MQTT. See [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
for the wire contract this firmware implements.

```
I2S PDM mic ─▶ VAD (ESP-SR esp_vad) ─▶ segmenter (pre-roll + hangover)
            ─▶ PCM16 envelope ─▶ MQTT publish ─▶ (SD spool on outage)
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

Copy `config.example.json` to the SD card as **`/config.json`** and fill in
Wi-Fi, identity (`store`/`register`), and the broker host/credentials. Insert the
card and power-cycle. One firmware binary serves the whole fleet; secrets live
only on each card.

Watch it come up: `idf.py -p <port> monitor`. On boot it mounts the SD, reads
config, joins Wi-Fi, **waits for NTP sync** (timestamps are load-bearing), then
starts capturing. If the broker is unreachable, utterances spool to
`/spool/*.bin` on the card and flush on reconnect.

## Configuration knobs (`idf.py menuconfig` → "uKnomi Capture Node")

- **VAD engine** — `ESP-SR VAD` (default; `esp_vad.h`, WebRTC-based, aggressiveness
  mapped from the config `vad.threshold`) or an `energy threshold` fallback for
  bring-up / A/B.
- **PDM mic pins** — default CLK=42, DIN=41 (XIAO Sense).
- **microSD SPI pins** — default SCK=7, MISO=8, MOSI=9, CS=21 (XIAO Sense).
- **Sample rate**, **max utterance length**.

## Build status & on-device validation

The firmware **compiles cleanly** against ESP-IDF v5.2 + ESP-SR 2.4.6 (verified in
the `espressif/idf:release-v5.2` container; CI builds the same way). It has **not
yet been run on a physical board** — still to confirm on hardware:

- **Board pin assignments** — verify the PDM mic (CLK=42, DIN=41) and microSD SPI
  pins (SCK=7, MISO=8, MOSI=9, CS=21) against your Sense revision via `menuconfig`.
- **VAD tuning** — `esp_vad` has no noise suppression; tune `vad.threshold`
  (→ aggressiveness mode) on the real counter. If you later need denoising, the
  heavier ESP-SR **AFE** is the upgrade path (a separate engine, not wired here).

The known v1 limitation stands: the onboard mic is **omnidirectional**, so
adjacent-register crosstalk will be worse than a directional setup — fine for
proving VAD + transcription + order-split, not representative for crosstalk.
