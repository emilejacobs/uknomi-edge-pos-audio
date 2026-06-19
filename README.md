# uKnomi Edge POS Audio

Capture and transcribe **in-store** (counter, not drive-thru) food orders from a microphone at each POS
station, and reconstruct individual customer orders as transcripts in S3. No cameras, no person detection,
no POS integration.

This repo is a **v1 viability test**: prove that audio-only capture + VAD segmentation + LLM-based order
reconstruction produces usable orders, and characterise the failure modes before investing further.

See [`docs/PRD.md`](docs/PRD.md) for the product spec, [`docs/HANDOFF.md`](docs/HANDOFF.md) for the design
history and decision log, and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the component contracts.

---

## How it fits together

```
XIAO ESP32-S3 Sense (per station)        On-site Mac (aggregator)
  PDM mic → ESP-SR AFE VAD → segmenter      Mosquitto → subscribe → whisper
  → PCM16 envelope → MQTT (+ SD spool)  ──▶ → consolidate → Claude order-split → S3
```

The two halves agree only on the **MQTT contract** (`docs/ARCHITECTURE.md` §1–2), so the capture node is
swappable. v1 uses the XIAO ESP32-S3 Sense (in hand); the Raspberry Pi Zero 2 W path (managed by the
Control Plane) is documented for later and drops onto the same seam.

| Component | Path | Stack |
|-----------|------|-------|
| Capture node (firmware) | [`capture-node-esp32/`](capture-node-esp32/) | ESP-IDF (C), ESP-SR AFE VAD |
| Aggregator | [`aggregator/`](aggregator/) | Python 3.11, faster-whisper, Anthropic, boto3 |
| Broker config | [`infra/mosquitto/`](infra/mosquitto/) | Mosquitto |

---

## Quickstart (Mac side, no hardware needed)

You can exercise the entire transport + transcription + order-split pipeline on the Mac alone, replaying a
recording over MQTT with `scripts/fake_feeder.py`.

```bash
# 1. broker
brew install mosquitto
mosquitto -c infra/mosquitto/mosquitto.conf

# 2. aggregator
cd aggregator
python -m venv .venv && source .venv/bin/activate
pip install -e '.[dev]'
cp aggregator.example.toml aggregator.toml   # edit broker creds, retain_audio, S3
uknomi-aggregator

# 3. replay a recording as utterances (separate shell)
python scripts/fake_feeder.py --wav some_counter_recording.wav --store store03 --register reg2
```

## Flashing the XIAO ESP32-S3 (per device)

```bash
cd capture-node-esp32
idf.py set-target esp32s3
idf.py build
./flash.sh /dev/cu.usbmodemXXXX          # flash firmware once

# then per unit: copy config.example.json to the SD card as /config.json, set wifi + identity + broker
```

One firmware binary serves the whole fleet — identity and secrets live only on each unit's SD card.

---

## ⚠️ Legal / consent

Recording customer audio in the US implicates wiretapping / two-party-consent law in several states.
Conspicuous signage and a per-site consent posture are **required design inputs**, and production retains
**transcripts only** (`retain_audio` defaults off). **Confirm the legal posture per client before any live
deployment.** See PRD §5 and §12.

## Status

v1 in development. Not for production deployment.
