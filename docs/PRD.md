# PRD — uKnomi Edge POS Audio

**Status:** Draft v1 (viability test)
**Date:** 2026-06-18
**Owner:** Emile Jacobs
**Related:** Control Plane (`uknomi-control-plane`) — device-management substrate; Mac-resident transcriber.

---

## 1. Summary

A system that captures spoken food orders at in-store POS counters via per-station microphones, transcribes them, and reconstructs individual customer orders as transcripts in S3. No cameras, no person detection, no POS integration. Audio is captured at each station on a small WiFi device, transcribed centrally on the on-site Mac, and split into orders by an LLM.

The immediate goal is a **viability test**: prove that audio-only capture + VAD segmentation + LLM-based order reconstruction produces usable orders, and characterise the failure modes (crosstalk, rapid succession, parties, non-order chatter) before investing further.

## 2. Problem & motivation

Restaurants want a record of what was ordered at the counter. Capturing this from the existing POS is not available to us. The naïve approaches fail:
- **Camera + person detection** to trigger recording is unreliable, and adds a vision pipeline to answer a proxy question (is someone there) instead of the real one (is an order being spoken).
- **Fixed-timer or manual recording** can't know when an order starts or ends.

We need a reliable, low-cost, retrofittable way to capture and structure counter orders.

## 3. Goals

- Capture audio at each POS station with one microphone per station.
- Reliably segment continuous audio into speech utterances without presence detection.
- Transcribe utterances accurately enough to reconstruct orders.
- Reconstruct **individual orders** from a stream of utterances **without POS events**.
- Be **cheap** (~$45–75/station) and **retrofittable** (no structured cabling; power + WiFi only).
- Run on infrastructure we control (our own network, our own devices), manageable by the Control Plane.

## 4. Non-goals (v1)

- No camera / vision / person detection.
- No POS integration (not available).
- No speaker diarization or voice recognition.
- No true streaming ASR (batch per utterance is sufficient).
- No real-time / live display of orders (latency is not a requirement).
- No drive-thru scenario.
- Long-term audio retention (audio kept only during the validation phase).

## 5. Constraints

- **No POS access** — order boundaries must be inferred from the transcript.
- **No Ethernet to POS stations** — ceiling cable runs, no guaranteed roof access. Capture nodes have power + WiFi only.
- **Multiple simultaneous orders** across multiple stations → one mic + one capture node per station.
- **Cost-sensitive.**
- **Consent/legal:** recording customer audio in the US runs into wiretapping / two-party-consent law in several states. Conspicuous signage and per-site consent posture are design inputs. Transcript-only retention in production is partly a response to this. **Confirm the legal posture per client before any live deployment.**

## 6. Users

- **Primary consumer:** downstream systems / analytics that read order transcripts from S3 (out of scope for this repo).
- **Operators:** field/install staff who mount and provision capture nodes (managed via the Control Plane).

## 7. Solution overview

```
Per POS station                          On-site Mac Mini (aggregator)
Pi Zero 2 W + ReSpeaker 2-Mics HAT  ──▶  whisper → consolidate → LLM split → S3
  capture → VAD segmentation              (over a private WiFi we control)
```

- **Capture node (per station):** Raspberry Pi Zero 2 W + ReSpeaker 2-Mics Pi HAT. Captures 16 kHz mono audio, runs VAD to segment into utterances, and ships timestamped, station-tagged audio segments over WiFi.
- **Network:** a dedicated AP we control (UniFi Express recommended; GL.iNet as the budget option). **Not** the store's WiFi, and **not** the Mac configured as an access point.
- **Aggregator (Mac):** receives segments from all stations, runs whisper, consolidates a per-register chronological transcript with timing gaps preserved inline, runs an LLM pass to split into individual orders, and uploads to S3.
- **Storage:** transcripts to S3 always; audio to S3 only while `retain_audio` is enabled (validation phase).

### 7.1 Capture & VAD (on the Pi)

- Continuous mic capture into a rolling ring buffer.
- **VAD** (Silero or WebRTC) classifies speech vs. silence per ~20–30 ms frame.
- State machine `IDLE → SPEECH → IDLE`:
  - **Pre-roll buffer** ~300–500 ms prepended on speech onset (no clipped first word).
  - **Hangover** ~600–800 ms of continuous silence before closing a segment (don't split on mid-sentence pauses).
- Output: bounded audio segments (utterances), each with **start + end timestamps** and a **station/register ID**.
- VAD is a *segmenter*, not a transcriber. Whisper still does transcription downstream.

### 7.2 Transcription & consolidation (on the Mac)

- Whisper transcribes each utterance (batch). Reuse-vs-ship-own transcriber is an open item.
- Utterances are consolidated **per register** into one chronological transcript, with **timing gaps preserved inline**, e.g.:
  ```
  [12:03:15–12:03:18] hi what can I get you
  [12:03:21–12:03:26] yeah can I get a number three with a coke
  [12:03:29–12:03:31] for here or to go
  ...
  ```
- Audio and transcript are **co-keyed** (`station + ISO8601 timestamp`) so any transcript line maps back to its audio.

### 7.3 Order reconstruction (LLM)

- A single LLM pass over the consolidated (chunked) transcript:
  - splits the stream into **individual customer orders**, and
  - extracts the **items** per order.
- Boundary cues: the cashier's repetitive script (greeting → items → upsell → total → close); silence gaps as weak hints.
- The LLM **emits traceability** — each order references the source utterance timestamps — for auditing against audio.
- Prompt is **biased toward splitting** when ambiguous (over-merging conflates orders).
- Recommended model: latest Claude. Prompt engineering is the core v1 deliverable.

## 8. Data & storage

- **Audio (validation only):** `s3://<bucket>/audio/<store>/<register>/<ISO8601>.wav` — compressed (Opus/FLAC) preferred; short retention TTL.
- **Transcript (always):** co-keyed with audio key; per-utterance text + start/end timestamps + station ID.
- **Orders:** structured output of the LLM split — list of orders, each with items and source-utterance timestamp references.
- **`retain_audio` flag** (pushed from CP) gates whether audio is stored. Default OFF in production, ON during validation, overridable per site for consent.

## 9. Control Plane integration (contract)

CP is the management substrate; this product is a tenant.

- **CP provides:** device enrollment/identity, presence/health for Pi nodes + Mac, config push (incl. `retain_audio`), package/agent delivery.
- **This product provides:** capture-node software, the Mac pipeline, the order model and S3 layout.
- **New CP work:** a device role/type for the Pi capture node; decision on reuse vs. ship-own transcriber.

## 10. Bill of materials

**Per station:** Pi Zero 2 W (~$15) + ReSpeaker 2-Mics Pi HAT (~$13) + microSD/case/power (~$15–30) = **~$45–75**.
**Per site network:** UniFi Express (~$129, recommended) or GL.iNet travel router (~$30–60).
**Crosstalk upgrade (optional):** ReSpeaker USB 4-Mic Array (~$60–70) — onboard beamforming/AEC/DOA.

## 11. Viability test plan

1. One station live on a real counter (NTP-synced Pi).
2. Capture → VAD → timestamped, station-tagged segments → Mac → whisper → co-keyed audio+transcript.
3. Consolidate per-register with inline gaps → LLM split → traceable orders → S3.
4. **Measure:**
   - Transcription accuracy vs. listening to retained audio.
   - VAD boundary quality (clipping / over-splitting).
   - LLM order-split accuracy (precision/recall on order boundaries and items).
   - The four failure modes below.
5. Decide whether LLM-only stitching is sufficient or diarization is warranted.

### Failure modes to characterise
- **Rapid succession** (busy rush, near-zero gaps between customers).
- **Groups/parties** (multiple people in one order).
- **Non-order chatter** (regulars, coworkers) producing phantom orders.
- **Crosstalk** (adjacent register bleed) — only defended by mic directionality/placement in v1.

## 12. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Order boundaries wrong without POS/voices | LLM split on cashier-script cues; bias to splitting; measure and iterate; diarization as deferred upgrade |
| Crosstalk between adjacent registers | Directional HAT + placement; USB 4-Mic Array beamforming as upgrade; "is the cashier present" reasoning later |
| Clock skew scrambling chronology | NTP on all nodes — treat as a first-class dependency |
| Whisper accuracy in noisy counter audio | Near-field directional mic; retained audio to validate; model/tuning iteration |
| Legal/consent for customer audio | Signage + per-site consent; transcript-only production retention; confirm per client before go-live |
| Pi microSD reliability at scale | Read-only rootfs; managed via CP |

## 13. Open questions

- **Unit of shipping:** per-utterance vs. per-order-session from the Pi (leaning per-utterance).
- **"Order is done" boundary:** silence timeout for v1; better signal TBD (no POS).
- **Transcriber:** reuse CP-delivered transcriber on the Mac vs. ship our own.
- **LLM:** model choice + prompt design (core v1 work).
- **Consent:** the per-client legal posture that gates live deployment.

## 14. Out of scope / future

- Speaker diarization, cashier-voice anchoring, customer-voice-change boundaries.
- True streaming ASR / live order display.
- POS integration (if it ever becomes available, it is the single best boundary + reconciliation signal).
- Downstream order analytics/consumers.
