#!/usr/bin/env python3
"""Replay a recording as MQTT utterances — test the Mac aggregator with no hardware.

Reads a WAV, carves it into speech segments with a simple energy VAD (a stand-in
for the device's ESP-SR AFE), and publishes each as an envelope on
``uknomi/<store>/<register>/segment`` exactly as the XIAO firmware would.

    pip install -e 'aggregator[dev]'      # gets numpy + soundfile + paho
    python scripts/fake_feeder.py --wav counter.wav --store store03 --register reg2

Requires numpy + soundfile (the aggregator's [dev] extra) and a running broker.
"""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
import paho.mqtt.client as mqtt
import soundfile as sf

# Import the shared envelope codec from the aggregator package without installing.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "aggregator"))
from uknomi_aggregator.envelope import SCHEMA, encode  # noqa: E402

TARGET_RATE = 16000


def load_mono16k(path: Path) -> np.ndarray:
    audio, rate = sf.read(str(path), dtype="float32", always_2d=True)
    mono = audio.mean(axis=1)  # downmix to mono
    if rate != TARGET_RATE:
        # Linear resample — good enough for a test feeder.
        n = int(round(len(mono) * TARGET_RATE / rate))
        mono = np.interp(np.linspace(0, len(mono), n, endpoint=False), np.arange(len(mono)), mono)
    return mono


def segment(audio: np.ndarray, threshold: float, hangover_ms: int, min_ms: int) -> list[tuple[int, int]]:
    """Energy VAD: return (start_sample, end_sample) spans of speech."""
    frame = TARGET_RATE * 20 // 1000  # 20 ms frames
    hang_frames = max(1, hangover_ms // 20)
    n_frames = len(audio) // frame
    energy = np.array(
        [float(np.sqrt(np.mean(audio[i * frame : (i + 1) * frame] ** 2))) for i in range(n_frames)]
    )
    speech = energy > threshold

    spans: list[tuple[int, int]] = []
    in_speech = False
    start = 0
    silence = 0
    for i, is_speech in enumerate(speech):
        if is_speech:
            if not in_speech:
                in_speech, start = True, i
            silence = 0
        elif in_speech:
            silence += 1
            if silence >= hang_frames:
                spans.append((start * frame, (i - silence + 1) * frame))
                in_speech = False
    if in_speech:
        spans.append((start * frame, n_frames * frame))

    min_samples = TARGET_RATE * min_ms // 1000
    return [(s, e) for s, e in spans if e - s >= min_samples]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", required=True, type=Path)
    ap.add_argument("--store", default="store03")
    ap.add_argument("--register", default="reg2")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--username")
    ap.add_argument("--password")
    ap.add_argument("--retain-audio", action="store_true")
    ap.add_argument("--threshold", type=float, default=0.02, help="VAD RMS threshold")
    ap.add_argument("--hangover-ms", type=int, default=700)
    ap.add_argument("--min-ms", type=int, default=300)
    ap.add_argument("--realtime", action="store_true", help="sleep to mimic real timing")
    args = ap.parse_args()

    audio = load_mono16k(args.wav)
    spans = segment(audio, args.threshold, args.hangover_ms, args.min_ms)
    if not spans:
        print("no speech segments found — lower --threshold?", file=sys.stderr)
        sys.exit(1)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="uknomi-fake-feeder")
    if args.username:
        client.username_pw_set(args.username, args.password)
    client.connect(args.host, args.port)
    client.loop_start()

    topic = f"uknomi/{args.store}/{args.register}/segment"
    base = datetime.now(timezone.utc)
    prev_end = 0.0
    for seq, (s, e) in enumerate(spans):
        if args.realtime:
            time.sleep(max(0.0, s / TARGET_RATE - prev_end))
        prev_end = e / TARGET_RATE
        start_utc = _iso(base + timedelta(seconds=s / TARGET_RATE))
        end_utc = _iso(base + timedelta(seconds=e / TARGET_RATE))
        pcm16 = (np.clip(audio[s:e], -1.0, 1.0) * 32767).astype("<i2").tobytes()
        header = {
            "schema": SCHEMA,
            "store": args.store,
            "register": args.register,
            "seq": seq,
            "start_utc": start_utc,
            "end_utc": end_utc,
            "sample_rate": TARGET_RATE,
            "codec": "pcm16",
            "vad": "fake_feeder_energy",
            "retain_audio": args.retain_audio,
        }
        client.publish(topic, encode(header, pcm16), qos=1)
        print(f"seq={seq} {start_utc} -> {end_utc} ({(e - s) / TARGET_RATE:.1f}s)")

    client.loop_stop()
    client.disconnect()
    print(f"published {len(spans)} segments to {topic}")


def _iso(dt: datetime) -> str:
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


if __name__ == "__main__":
    main()
