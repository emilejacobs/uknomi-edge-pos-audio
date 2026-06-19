#!/usr/bin/env python3
"""Run the configured order-split provider over a sample transcript and print orders.

Handy for validating a local LM Studio (or any OpenAI-compatible) model and for
iterating on the prompt — no MQTT, whisper, or hardware needed.

    pip install -e 'aggregator[openai]'        # for the openai-compatible provider
    UKNOMI_AGGREGATOR_CONFIG=aggregator/aggregator.toml python scripts/try_ordersplit.py

Reads provider/model/base_url from the aggregator config (same as the real run).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "aggregator"))
from uknomi_aggregator.config import load_config  # noqa: E402
from uknomi_aggregator.ordersplit import OrderSplitter  # noqa: E402
from uknomi_aggregator.store import Record  # noqa: E402

# Two clear orders + an upsell + a close/greeting boundary + a non-order line.
SAMPLE = [
    ("12:03:15", "12:03:18", "hi there what can I get started for you"),
    ("12:03:21", "12:03:26", "yeah can I get a number three with a coke no onions"),
    ("12:03:29", "12:03:31", "for here or to go"),
    ("12:03:33", "12:03:34", "to go"),
    ("12:03:40", "12:03:44", "alright that's nine forty out of twenty"),
    ("12:03:50", "12:03:52", "have a good one"),
    ("12:04:05", "12:04:07", "hi what can I get you"),
    ("12:04:10", "12:04:15", "two large fries and a chocolate shake please"),
    ("12:04:18", "12:04:20", "anything else for you today"),
    ("12:04:21", "12:04:22", "no that's it"),
    ("12:04:28", "12:04:31", "okay that'll be eight seventy five"),
]


def _rec(start: str, end: str, text: str) -> Record:
    return Record(
        store="store03", register="reg2",
        start_utc=f"2026-06-18T{start}.000Z", end_utc=f"2026-06-18T{end}.000Z",
        seq=0, json_path=Path("x"), audio_path=None, text=text,
    )


def main() -> None:
    cfg = load_config()
    records = [_rec(*row) for row in SAMPLE]
    splitter = OrderSplitter(cfg.ordersplit)
    print(
        f"provider={cfg.ordersplit.provider} model={cfg.ordersplit.model} "
        f"base_url={cfg.ordersplit.base_url}",
        file=sys.stderr,
    )
    doc = splitter.split_records(records, "store03", "reg2")
    print(json.dumps(doc, indent=2))


if __name__ == "__main__":
    main()
