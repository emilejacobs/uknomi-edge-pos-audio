# Architecture & Contracts — uKnomi Edge POS Audio

This document defines the **stable seams** between components. The capture node and the aggregator are
developed independently and only agree on what is written here. The MQTT contract is deliberately
hardware-agnostic so the capture node can be a XIAO ESP32-S3 (v1) or a Raspberry Pi (future, Appendix A in
the build plan) without the aggregator changing.

```
PER STATION                                 ON-SITE MAC (aggregator)
┌────────────────────────────┐
│ XIAO ESP32-S3 Sense        │   MQTT      ┌──────────────────────────────────────┐
│  I2S PDM mic → ring buffer │  (QoS1)     │ Mosquitto broker                      │
│  → ESP-SR AFE VAD          │ ──────────▶ │   → subscriber (decode envelope)      │
│  → segmenter (preroll/     │  store      │   → store co-keyed audio+meta         │
│     hangover)              │  WiFi       │   → whisper transcribe                │
│  → PCM16 envelope          │             │   → consolidate per-register          │
│  → MQTT publish + SD spool │             │   → Claude order-split (traceable)    │
└────────────────────────────┘             │   → S3 (transcripts always; audio iff │
                                            │        retain_audio)                  │
                                            └──────────────────────────────────────┘
```

---

## 1. MQTT transport

| Property | Value |
|----------|-------|
| Broker | Mosquitto on the on-site Mac, TCP `1883` (TLS is a documented follow-up) |
| Auth | username + password, **required** (shared store WiFi). Per-device creds recommended |
| Discovery | `broker.host` in the node's SD `config.json` (static). mDNS is a follow-up |
| Topic | `uknomi/<store>/<register>/segment` |
| QoS | **1** (at-least-once). Aggregator must be idempotent on `seq` (see below) |
| Retain | `false` |
| Direction | node → broker only. **No inbound connections to the node.** |

One MQTT publish == one VAD-bounded utterance.

### Idempotency
`(store, register, seq)` uniquely identifies an utterance. `seq` is a monotonic counter the node persists
across reconnects (and, best-effort, across reboots via NVS). QoS-1 redelivery or spool re-flush can
deliver the same utterance twice; the aggregator de-dupes on `(store, register, seq)`.

---

## 2. Segment envelope (binary, no base64)

The MQTT payload is a single binary blob:

```
┌────────────────┬──────────────────────────┬───────────────────────────┐
│ 4 bytes        │ header_len bytes         │ remaining bytes           │
│ header_len     │ UTF-8 JSON header        │ audio payload             │
│ (big-endian    │                          │ (codec per header.codec)  │
│  uint32)       │                          │                           │
└────────────────┴──────────────────────────┴───────────────────────────┘
```

### Header JSON
```json
{
  "schema": "uknomi.segment/1",
  "store": "store03",
  "register": "reg2",
  "seq": 1487,
  "start_utc": "2026-06-18T12:03:15.123Z",
  "end_utc":   "2026-06-18T12:03:18.512Z",
  "sample_rate": 16000,
  "codec": "pcm16",
  "vad": "esp_sr_afe",
  "retain_audio": true
}
```

- `schema` — version tag; bump on breaking changes. Aggregator rejects unknown major versions.
- `codec` — `pcm16` (v1, 16 kHz mono signed 16-bit little-endian). Reserved: `flac` (Pi path), `adpcm`.
- `start_utc` / `end_utc` — ISO-8601 UTC, millisecond precision. **Load-bearing** (see §4).
- `retain_audio` — node-local flag (from SD config) telling the aggregator whether to keep the audio.
- `vad` — which VAD produced the boundary (provenance for failure analysis).

---

## 3. Aggregator storage layout (co-keyed)

Audio and transcript share one key: `store + register + start_utc`.

```
data/
  <store>/<register>/
    2026-06-18T12-03-15.123Z.wav     # audio, only if retain_audio (validation phase)
    2026-06-18T12-03-15.123Z.json    # per-utterance metadata + transcript
```

`:` in the ISO timestamp is replaced with `-` for filesystem safety; the canonical timestamp is preserved
inside the JSON. Any transcript line maps back to its audio by this key — the feedback loop that makes a
bad order-split auditable against what was actually said.

### S3 layout (mirrors PRD §8)
```
s3://<bucket>/audio/<store>/<register>/<ISO8601>.wav        # iff retain_audio
s3://<bucket>/transcripts/<store>/<register>/<ISO8601>.json # always
s3://<bucket>/orders/<store>/<register>/<date>/<run>.json   # consolidated order-split output
```

---

## 4. Clock discipline (NTP is a first-class dependency)

Order reconstruction is driven by chronology and inter-utterance gaps — the only weak prior standing in
for the absent POS event. Clock skew across stations scrambles it.

- The node runs **SNTP at boot** and **gates segmentation until time is synced** — it will not stamp or
  emit utterances with an unsynced clock.
- All timestamps are **UTC with millisecond precision**.
- The aggregator orders utterances by `start_utc`, never by arrival time (spool flushes arrive late).

---

## 5. Order-split output schema (aggregator → S3)

The LLM pass emits structured JSON, biased toward splitting, with traceability back to source utterances:

```json
{
  "store": "store03",
  "register": "reg2",
  "window": { "start_utc": "...", "end_utc": "..." },
  "model": "claude-sonnet-4-6",
  "orders": [
    {
      "order_index": 0,
      "items": [ { "name": "number 3", "modifiers": ["no onions"], "qty": 1 } ],
      "source_utterances": ["2026-06-18T12:03:15.123Z", "2026-06-18T12:03:21.040Z"],
      "confidence": "high"
    }
  ]
}
```

`source_utterances` are the co-key timestamps (§3), so any order is auditable back to its audio.

---

## 6. Device roles (Control Plane seam)

| Role | v1 management |
|------|---------------|
| XIAO ESP32-S3 capture node | **Unmanaged** in v1. Local SD `config.json`; flash over USB. OTA is a follow-up. |
| Mac aggregator | Existing CP-delivered host. `retain_audio` default lives in `aggregator.toml`. |

When the managed Pi capture node (build-plan Appendix A) is built, CP must add a **new capture-node device
role** (enrollment, health, config push including `retain_audio`, package/OTA delivery). The MQTT contract
above does not change.
