from pathlib import Path

from uknomi_aggregator.config import OrderSplitConfig
from uknomi_aggregator.ordersplit import OrderSplitter
from uknomi_aggregator.store import Record


def _rec(start, end, text):
    return Record(
        store="store03", register="reg2", start_utc=start, end_utc=end, seq=0,
        json_path=Path("x.json"), audio_path=None, text=text,
    )


def test_merge_dedupes_overlapping_orders_and_reindexes(monkeypatch):
    splitter = OrderSplitter(OrderSplitConfig(max_chunk_chars=120, overlap_chars=60))

    # Two chunks will both surface the boundary order (same source_utterances);
    # the merge must keep it once and assign sequential order_index values.
    canned = {
        "chunk_a": [
            {"items": [{"name": "number 3", "modifiers": [], "qty": 1}],
             "source_utterances": ["2026-06-18T12:00:00.000Z"], "confidence": "high"},
            {"items": [{"name": "coke", "modifiers": ["large"], "qty": 1}],
             "source_utterances": ["2026-06-18T12:00:30.000Z"], "confidence": "medium"},
        ],
        "chunk_b": [
            {"items": [{"name": "coke", "modifiers": ["large"], "qty": 1}],
             "source_utterances": ["2026-06-18T12:00:30.000Z"], "confidence": "medium"},
            {"items": [{"name": "fries", "modifiers": [], "qty": 2}],
             "source_utterances": ["2026-06-18T12:01:00.000Z"], "confidence": "high"},
        ],
    }
    calls = iter(["chunk_a", "chunk_b"])
    monkeypatch.setattr(splitter, "split_chunk", lambda text: canned[next(calls)])
    # Force two chunks regardless of the real chunker.
    monkeypatch.setattr(
        "uknomi_aggregator.ordersplit.chunk_lines", lambda lines, a, b: ["x", "y"]
    )

    records = [_rec("2026-06-18T12:00:00.000Z", "2026-06-18T12:00:02.000Z", "hi")]
    doc = splitter.split_records(records, "store03", "reg2")

    assert doc["store"] == "store03"
    assert doc["model"] == "claude-opus-4-8"
    assert len(doc["orders"]) == 3  # boundary "coke" order de-duped
    assert [o["order_index"] for o in doc["orders"]] == [0, 1, 2]
    names = [o["items"][0]["name"] for o in doc["orders"]]
    assert names == ["number 3", "coke", "fries"]  # sorted by first source utterance


def test_prompt_bundled_and_loadable():
    splitter = OrderSplitter(OrderSplitConfig())
    assert "order" in splitter._prompt.lower()
    assert "source_utterances" in splitter._prompt
