# Capture node — XIAO ESP32-S3 Sense (ESP-IDF firmware)

Captures the onboard PDM microphone, segments speech with VAD, and ships each
utterance to the Mac aggregator over MQTT. See [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
for the wire contract this firmware implements.

```
I2S PDM mic ─▶ VAD (ESP-SR AFE) ─▶ segmenter (pre-roll + hangover)
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

- **VAD engine** — `ESP-SR AFE` (default; noise suppression + VAD) or an
  `energy threshold` fallback for bring-up / A/B.
- **PDM mic pins** — default CLK=42, DIN=41 (XIAO Sense).
- **microSD SPI pins** — default SCK=7, MISO=8, MOSI=9, CS=21 (XIAO Sense).
- **Sample rate**, **max utterance length**.

## ⚠️ Needs on-device validation

This firmware targets the documented ESP-IDF v5 and ESP-SR v2 APIs but has **not
been compiled or run on hardware in this commit**. Most likely to need tweaks:

- **ESP-SR AFE integration** (`vad.c`): the `afe_config_*` / `afe_fetch_result_t`
  symbols and VAD-state enum names have shifted across esp-sr versions. If the
  AFE build fails, switch to the energy VAD (`menuconfig`) to bring the rest of
  the pipeline up, then reconcile the AFE API against your pinned component.
- **Board pin assignments** — confirm the PDM and SD pins against your Sense
  revision (defaults above are the common Seeed values).

The known v1 limitation stands: the onboard mic is **omnidirectional**, so
adjacent-register crosstalk will be worse than a directional setup — fine for
proving VAD + transcription + order-split, not representative for crosstalk.
