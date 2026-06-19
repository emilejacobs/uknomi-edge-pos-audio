from pathlib import Path

from uknomi_aggregator.consolidate import build_lines, chunk_lines, consolidate, gap_seconds
from uknomi_aggregator.store import Record


def _rec(start, end, text):
    return Record(
        store="s", register="r", start_utc=start, end_utc=end, seq=0,
        json_path=Path("x.json"), audio_path=None, text=text,
    )


def test_gap_seconds():
    assert gap_seconds("2026-06-18T12:00:00.000Z", "2026-06-18T12:00:05.000Z") == 5.0


def test_lines_have_start_utc_prefix():
    lines = build_lines([_rec("2026-06-18T12:00:00.000Z", "2026-06-18T12:00:02.000Z", "hi")])
    assert lines == ["[2026-06-18T12:00:00.000Z] hi"]


def test_pause_marker_inserted_for_long_gap_only():
    recs = [
        _rec("2026-06-18T12:00:00.000Z", "2026-06-18T12:00:02.000Z", "first"),
        _rec("2026-06-18T12:00:03.000Z", "2026-06-18T12:00:05.000Z", "second"),  # 1s gap
        _rec("2026-06-18T12:01:00.000Z", "2026-06-18T12:01:02.000Z", "third"),   # 55s gap
    ]
    lines = build_lines(recs)
    assert any(line.startswith("(—") for line in lines)
    assert sum(line.startswith("(—") for line in lines) == 1  # only the big gap


def test_empty_text_skipped():
    assert build_lines([_rec("2026-06-18T12:00:00.000Z", "2026-06-18T12:00:02.000Z", None)]) == []


def test_consolidate_joins_lines():
    recs = [
        _rec("2026-06-18T12:00:00.000Z", "2026-06-18T12:00:02.000Z", "a"),
        _rec("2026-06-18T12:00:03.000Z", "2026-06-18T12:00:05.000Z", "b"),
    ]
    assert consolidate(recs) == "[2026-06-18T12:00:00.000Z] a\n[2026-06-18T12:00:03.000Z] b"


def test_chunking_splits_and_covers_all_content():
    lines = [f"[2026-06-18T12:00:{i:02d}.000Z] order line number {i}" for i in range(40)]
    chunks = chunk_lines(lines, max_chars=200, overlap_chars=60)
    assert len(chunks) > 1
    # Every original line appears in at least one chunk.
    joined = "\n".join(chunks)
    for line in lines:
        assert line in joined


def test_short_transcript_is_single_chunk():
    lines = ["[2026-06-18T12:00:00.000Z] just one"]
    assert chunk_lines(lines, max_chars=8000, overlap_chars=600) == lines
