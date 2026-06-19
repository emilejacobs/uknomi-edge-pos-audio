"""Consolidate per-register utterances into a chronological transcript.

The output preserves **timing gaps inline** so the LLM order-splitter can see
pauses (PRD §7.2). Each line is keyed by the utterance's full ``start_utc`` so
the model can cite it verbatim in ``source_utterances`` for traceability.

    [2026-06-18T12:03:15.123Z] hi what can I get you
    [2026-06-18T12:03:21.040Z] yeah can I get a number three with a coke
    (— 47s pause —)
    [2026-06-18T12:04:08.000Z] hi, what can I get you

Long transcripts are split into overlapping chunks, preferring to cut on a long
silence so an order straddling a boundary isn't sliced in half (PRD §11).
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from datetime import datetime

from .store import Record

# A gap at least this long between consecutive utterances is rendered as a pause
# marker — a weak "order boundary" hint for the LLM.
PAUSE_MARKER_SECONDS = 8.0


def _parse(iso_utc: str) -> datetime:
    return datetime.fromisoformat(iso_utc.replace("Z", "+00:00"))


def gap_seconds(prev_end_utc: str, next_start_utc: str) -> float:
    """Seconds of silence between one utterance ending and the next starting."""
    return (_parse(next_start_utc) - _parse(prev_end_utc)).total_seconds()


def build_lines(records: Sequence[Record]) -> list[str]:
    """Render records (assumed time-sorted) into transcript lines with pause markers."""
    lines: list[str] = []
    prev_end: str | None = None
    for rec in records:
        if not rec.text:
            continue
        if prev_end is not None:
            gap = gap_seconds(prev_end, rec.start_utc)
            if gap >= PAUSE_MARKER_SECONDS:
                lines.append(f"(— {round(gap)}s pause —)")
        lines.append(f"[{rec.start_utc}] {rec.text.strip()}")
        prev_end = rec.end_utc
    return lines


def consolidate(records: Sequence[Record]) -> str:
    """Full single-string transcript for one register."""
    return "\n".join(build_lines(records))


def chunk_lines(
    lines: Iterable[str], max_chars: int, overlap_chars: int
) -> list[str]:
    """Split transcript lines into overlapping chunks under ``max_chars``.

    Greedy accumulation; prefers to start a new chunk right after a pause marker
    so order boundaries fall between chunks. Carries ``overlap_chars`` of trailing
    context into the next chunk so an order spanning a cut is still recoverable.
    """
    lines = list(lines)
    chunks: list[str] = []
    current: list[str] = []
    size = 0
    fresh = False  # has a non-overlap line been added since the last flush?

    def overlap_tail() -> tuple[list[str], int]:
        tail: list[str] = []
        tail_size = 0
        for line in reversed(current):
            if tail_size + len(line) + 1 > overlap_chars:
                break
            tail.insert(0, line)
            tail_size += len(line) + 1
        return tail, tail_size

    for line in lines:
        line_size = len(line) + 1
        is_pause = line.startswith("(—")
        # Cut when over budget, preferring to break at a pause marker.
        if current and (size + line_size > max_chars or (is_pause and size > max_chars * 0.6)):
            chunks.append("\n".join(current))
            current, size = overlap_tail()
            fresh = False
        current.append(line)
        size += line_size
        if not is_pause:
            fresh = True

    # Emit the trailing chunk only if it carries content beyond the overlap tail.
    if current and fresh:
        chunks.append("\n".join(current))
    return [c for c in chunks if c.strip()]
