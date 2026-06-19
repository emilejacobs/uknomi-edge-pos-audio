"""Co-keyed on-disk storage for utterances (audio + metadata/transcript).

Layout (docs/ARCHITECTURE.md §3):

    <data_dir>/<store>/<register>/<ISO8601>.wav    # only when retain_audio
    <data_dir>/<store>/<register>/<ISO8601>.json   # always

The ISO timestamp has ``:`` replaced with ``-`` for filesystem safety; the
canonical value is preserved inside the JSON. Audio is written as a WAV
container around the raw PCM16 so it's directly playable during validation.
"""

from __future__ import annotations

import json
import wave
from dataclasses import dataclass
from pathlib import Path

from .envelope import Segment


def _fs_timestamp(iso_utc: str) -> str:
    """Make an ISO-8601 timestamp safe for a filename (``:`` -> ``-``)."""
    return iso_utc.replace(":", "-")


@dataclass
class Record:
    """A persisted utterance: paths + metadata + (optional) transcript."""

    store: str
    register: str
    start_utc: str
    end_utc: str
    seq: int
    json_path: Path
    audio_path: Path | None
    text: str | None


class Store:
    def __init__(self, data_dir: Path) -> None:
        self.data_dir = Path(data_dir)

    def _dir(self, store: str, register: str) -> Path:
        return self.data_dir / store / register

    def _stem(self, segment: Segment) -> str:
        return _fs_timestamp(segment.start_utc)

    def json_path(self, segment: Segment) -> Path:
        return self._dir(segment.store, segment.register) / f"{self._stem(segment)}.json"

    def audio_path(self, segment: Segment) -> Path:
        return self._dir(segment.store, segment.register) / f"{self._stem(segment)}.wav"

    def already_processed(self, segment: Segment) -> bool:
        """Idempotency check — has this (store, register, seq) been written?

        We key the de-dupe on the JSON existing for this utterance's timestamp.
        A QoS-1 redelivery or spool re-flush of the same utterance is a no-op.
        """
        path = self.json_path(segment)
        if not path.is_file():
            return False
        try:
            existing = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            return False
        return int(existing.get("seq", -1)) == segment.seq

    def save_audio(self, segment: Segment) -> Path:
        """Write the PCM16 payload as a WAV file. Call only when retaining audio."""
        path = self.audio_path(segment)
        path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)  # 16-bit
            wav.setframerate(segment.sample_rate)
            wav.writeframes(segment.audio)
        return path

    def save_record(
        self, segment: Segment, text: str | None, audio_path: Path | None
    ) -> Record:
        """Write the per-utterance JSON record (metadata + transcript)."""
        json_path = self.json_path(segment)
        json_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            **segment.header,
            "text": text,
            "audio_file": audio_path.name if audio_path else None,
        }
        json_path.write_text(json.dumps(payload, indent=2, sort_keys=True))
        return Record(
            store=segment.store,
            register=segment.register,
            start_utc=segment.start_utc,
            end_utc=segment.end_utc,
            seq=segment.seq,
            json_path=json_path,
            audio_path=audio_path,
            text=text,
        )

    def load_register_records(self, store: str, register: str) -> list[Record]:
        """Load all utterance records for one register, sorted by start time.

        Ordering is by ``start_utc`` (never arrival order — spool flushes arrive
        late), per ARCHITECTURE §4.
        """
        directory = self._dir(store, register)
        records: list[Record] = []
        if not directory.is_dir():
            return records
        for json_path in directory.glob("*.json"):
            try:
                data = json.loads(json_path.read_text())
            except (OSError, json.JSONDecodeError):
                continue
            audio_name = data.get("audio_file")
            records.append(
                Record(
                    store=data.get("store", store),
                    register=data.get("register", register),
                    start_utc=data["start_utc"],
                    end_utc=data["end_utc"],
                    seq=int(data.get("seq", -1)),
                    json_path=json_path,
                    audio_path=(directory / audio_name) if audio_name else None,
                    text=data.get("text"),
                )
            )
        records.sort(key=lambda r: r.start_utc)
        return records
