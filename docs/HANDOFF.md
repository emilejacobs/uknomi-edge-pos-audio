# Handoff — uKnomi Edge POS Audio (in-store order capture)

**Date:** 2026-06-18
**Status:** Design converged, no code yet. This is the founding doc for a new project.
**Relationship to other repos:** New, standalone product. Rides on top of the **Control Plane (CP)** (`uknomi-control-plane`) for device management; integrates with the Mac-resident transcriber. It is NOT part of CP. See "Integration with the Control Plane" below.

---

## What we're building

Detect and transcribe **in-store** food orders placed at a POS counter (not drive-thru), using a microphone at each POS station. Audio is captured at the station, transcribed centrally, and reconstructed into individual customer orders. Output is order transcripts in S3 for downstream use.

This started as a camera + person-detection idea and was deliberately narrowed to **audio-only** — see decisions below.

---

## How we got here (the reasoning, condensed)

1. **Person/vision detection was the original trigger idea — we dropped it.** Person detection is unreliable, which makes start/stop of recording unreliable, and it adds a whole vision pipeline to answer a proxy question (presence) when what we actually care about is *speech*. Audio-only is cleaner.

2. **We do NOT have POS access.** This is a hard constraint, not a "maybe later." It means we cannot key recording windows or order boundaries off transaction events. Order boundaries must be inferred from the transcript itself. This is the single hardest part of the product (see "The hard problem").

3. **Don't gate recording at all — capture continuously and segment.** Instead of deciding "start now / stop now," the capture node runs always-on and uses **VAD (Voice Activity Detection)** to carve the stream into speech segments (utterances). VAD is cheap, robust, and far more reliable than person detection.

4. **Mic-per-POS, wireless, on a network we own.** Multiple simultaneous orders at multiple stations means one mic per station. Wiring Ethernet to each POS is not viable (ceiling runs, no guaranteed roof access). So each station gets a small **WiFi capture node** that needs only power (which the POS already has). We decouple "get the mic to the Mac" into "get the *audio* to the Mac over WiFi."

5. **Whisper stays the transcriber; VAD just draws the boundaries.** The existing batch whisper.cpp mental model is intact (record audio → whisper → text). VAD replaces the manual/fixed-timer "when to record" decision. We are NOT doing true streaming ASR in v1 — latency isn't critical for order capture, so VAD-segmented batch is the right call.

---

## Final architecture

```
PER POS STATION                         CENTRAL (Mac Mini, already on site)
┌────────────────────────┐
│ Raspberry Pi Zero 2 W  │   WiFi      ┌─────────────────────────────────┐
│ + ReSpeaker 2-Mics HAT │  ───────▶   │ Mac aggregator                  │
│                        │  (private)  │  • receives segments from all   │
│ mic → 16kHz PCM        │             │    stations                     │
│   → VAD segmentation   │             │  • whisper → transcript turns   │
│   → timestamped,       │             │  • consolidate per-register,    │
│     station-tagged     │             │    chronological, inline gaps   │
│     audio segments     │             │  • LLM split → individual orders│
└────────────────────────┘             │  • push to S3                   │
                                        └─────────────────────────────────┘
        Dedicated AP we control                          │
        (UniFi Express / GL.iNet)                        ▼
        — NOT the store WiFi, NOT Mac-as-AP             S3
                                          (transcripts always;
                                           audio only during validation)
```

**Division of labor**
- **Pi (per POS):** capture + VAD segmentation only. Dumb and light. Emits bounded, timestamped, station-tagged audio segments over WiFi.
- **Mac (aggregator):** whisper transcription, per-register consolidation, LLM order-splitting, S3 upload. The existing Mac transcriber slots in here.
- **S3:** durable store / hand-off to downstream consumers.

---

## How VAD + ASR fit together (so the model is clear)

- **VAD ≠ ASR.** VAD only answers "is there speech in this ~20ms frame, yes/no." It does NOT transcribe. Whisper still does transcription.
- **Model used: VAD-segmented batch.** Mic runs continuously into a rolling ring buffer. VAD detects speech onset → start accumulating a segment; detects sustained silence → close the segment. That bounded segment is what goes to whisper. Functionally identical to the existing flow, except VAD draws the start/stop lines instead of a person or a timer.
- **Two details that keep it robust:**
  - **Pre-roll buffer (~300–500ms):** always keep the last fraction of a second buffered so when VAD fires you don't clip the first word.
  - **Hangover (~600–800ms):** wait for *continuous* silence before closing, so a mid-sentence pause doesn't split one utterance into two.
- **State machine:** `IDLE → SPEECH (accumulating) → IDLE`, pre-roll on entry, hangover on exit.
- VAD library: Silero VAD or WebRTC VAD — both run comfortably on a Pi Zero 2 W.
- **Not doing:** true streaming ASR (continuous partial hypotheses). Whisper isn't natively streaming and we don't need sub-second latency for order capture.

---

## The hard problem: stitching utterances into orders (no POS, no voices in v1)

VAD gives **turns** ("can I get a number 3 with a Coke"), not orders. An order is several turns over ~30–90s. With no POS event to bound it and (by decision) no speaker diarization in v1, order boundaries are reconstructed **semantically from the transcript**.

**v1 approach (viability test):**
1. Transcribe every utterance with **start + end timestamps** and a **station/register ID**.
2. Consolidate per register into one chronological transcript, with **timing gaps preserved inline** (e.g. `[12:03:15–12:03:18] <text>`) so the LLM can see pauses.
3. Single **LLM pass** splits the consolidated transcript into individual orders and extracts items, using the cashier's repetitive script (greeting → items → upsell → total → close) as natural delimiters, with silence gaps and "is there a long pause here" as weak hints.
4. LLM emits **traceability** — each order tagged with the source utterance timestamps it used — so wrong splits are auditable back to the audio.
5. Bias the prompt **toward splitting** when ambiguous (over-merging conflates two orders, which is worse than over-splitting).

**Why this is viable without voices:** a cashier runs the same script dozens of times an hour. "That'll be $X… have a good one" + the next "hi, what can I get you" is a far more reliable boundary than any silence threshold. The LLM is good at this precisely because the scaffolding is so regular.

**Deferred (not v1):** speaker diarization, cashier-voice anchoring, customer-voice-change boundaries. These are real upgrades if v1 quality is insufficient, but we test the simple approach first.

---

## Decisions log

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Audio-only; **no** person/vision detection | Person detection unreliable; adds vision pipeline for a proxy signal |
| D2 | Continuous capture + **VAD segmentation**, no gating | More reliable than presence detection; never have to decide "start now" |
| D3 | **VAD-segmented batch**, not true streaming ASR | Latency not critical; keeps existing whisper flow intact |
| D4 | Whisper runs **on the Mac**; Pi only captures + VADs | Pi Zero 2 W is slow at whisper; keeps node dumb (open item D-OPEN-3) |
| D5 | **Mic-per-POS**, WiFi capture node | Multiple simultaneous orders; Ethernet to each POS not viable |
| D6 | **No Bluetooth** | Doesn't scale past 1–2 mics on one host; poor mic-direction quality; weak range |
| D7 | **Own private network via dedicated AP**, NOT Mac-as-AP | Mac has one radio + weakest antenna; macOS Internet Sharing is flaky |
| D8 | Capture node = **Pi Zero 2 W + ReSpeaker 2-Mics Pi HAT** | Clean small form factor, GPIO HAT (no OTG dongle), real Linux endpoint, fleet-manageable |
| D9 | **No POS integration** | Not available to us — hard constraint |
| D10 | Order stitching is a **downstream LLM job** on the transcript | Only viable boundary signal without POS; segmentation + extraction in one call |
| D11 | **No diarization in v1** | Keep viability test minimal; add only if quality demands it |
| D12 | **Transcript-only to S3 in production; audio retained only during validation** | Lighter storage + cleaner consent posture; audio needed to validate the pipeline |
| D13 | Audio retention controlled by a **`retain_audio` config flag** (pushed via CP), not a code path | Flip per-device for test→prod; per-site consent overrides |
| D14 | **Separate project** from the Control Plane | CP manages/observes the fleet; this produces a restaurant business output. Different domain/lifecycle |

---

## Crucial implementation notes (easy to get wrong)

- **Timestamps are load-bearing.** Capture **start AND end** per utterance — the inter-utterance gap is the only weak prior standing in for the POS event. Put timestamps in the filename **and** inline in the consolidated transcript.
- **NTP on every Pi.** If timestamps drive chronology, clock skew/drift across stations will scramble order reconstruction. This is a silent dependency — get it right first.
- **Co-key audio + transcript** by `station + ISO8601 timestamp` (e.g. `store03_reg2_2026-06-18T12-03-15.123Z.{wav,txt}`). During the audio-retention phase this is what makes validation a real feedback loop — click from a bad split straight to the audio.
- **Consolidate per register**, never blend stations into one transcript.
- **Chunk the consolidated transcript** for LLM context limits by overlapping chunks or cutting on long silence gaps, so an order straddling a boundary isn't sliced in half.
- **Crosstalk:** in v1 the *only* defense is the HAT's directionality + physical placement. Aim the mic at the customer side of the counter. Expect some neighbor-register bleed in the transcript — it's a thing we measure, not fix, in v1.

---

## Integration with the Control Plane (the seam)

CP is the **management substrate**; this product is a **tenant** on it. Integration is a defined contract, not shared code.

**CP owns (mostly already exists):**
- Device identity, enrollment, presence/health for the new Pi capture nodes and the Mac
- Config push — the `retain_audio` flag rides CP's existing device-config mechanism
- Package/agent delivery — the Mac transcriber is already a CP-delivered package

**This project owns:**
- Capture-node software (VAD + segmentation)
- Transcription → consolidation → LLM-split pipeline and prompts
- Order data model, S3 transcript schema/layout, downstream consumers

**CP-side work items this creates:**
- A **new device role/type** for the Pi Zero capture node (CP agent/enrollment must recognize it)
- Decide: **reuse the CP-delivered transcriber** on the Mac, or ship our own (see D-OPEN-3)

---

## Open items for v1

- **D-OPEN-1: Unit of shipping** — Pi ships each utterance (Mac stitches), or Pi accumulates an order session and ships a bundle. Leaning per-utterance (simpler Pi, Mac owns "what's an order").
- **D-OPEN-2: "Order is done" boundary** — v1 uses a silence timeout; keep a hook for a better signal later. No POS event available.
- **D-OPEN-3: Reuse vs. ship-own transcriber** on the Mac.
- **D-OPEN-4: LLM provider/model + prompt** for order-splitting. Recommend defaulting to Claude (latest model) given the ecosystem; prompt engineering is the core v1 work.

---

## Bill of materials

**Per station**
| Item | Approx. cost |
|------|-------------|
| Raspberry Pi Zero 2 W | ~$15 |
| ReSpeaker 2-Mics Pi HAT | ~$13 |
| microSD, case, USB power | ~$15–30 |
| **Per-station total** | **~$45–75** |

**Per site (networking — we own it, not the store)**
| Item | Approx. cost | Notes |
|------|-------------|-------|
| UniFi Express (gateway+AP+controller) | ~$129 | Fits existing UniFi tooling — recommended |
| *or* GL.iNet travel router | ~$30–60 | Cheapest; runs OpenWrt |

**Crosstalk upgrade (only if needed)**
| Item | Approx. cost | Notes |
|------|-------------|-------|
| ReSpeaker USB 4-Mic Array | ~$60–70 | Onboard beamforming + AEC + DOA; swap in if adjacent-register bleed is the limiting factor |

> Prices are rough and vary by supplier. Buy one 2-Mics HAT and one USB 4-Mic Array for the first station to A/B them on the real counter. Mounting/placement matters more than mic spec — test it.

---

## Suggested first steps

1. Stand up one station (Pi Zero 2 W + 2-Mics HAT) on a real counter.
2. Implement capture → VAD → timestamped, station-tagged segment files (NTP synced).
3. Ship segments to the Mac; run whisper; write co-keyed audio+transcript.
4. Consolidate per-register with inline gaps; run the LLM split; emit traceable orders.
5. Measure: transcription quality, crosstalk, and the four failure modes — rapid succession, groups/parties, non-order chatter, crosstalk.
6. Decide whether v1 LLM-only stitching is good enough, or whether diarization (deferred) is needed.

See `docs/PRD.md` for the full product spec.
