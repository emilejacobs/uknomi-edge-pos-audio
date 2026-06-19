"""LLM order reconstruction: consolidated transcript -> individual orders.

A single Claude pass per transcript chunk splits the cashier's repetitive script
into individual customer orders and extracts items, emitting **traceability**
(the source utterance ``start_utc`` values it used) so any order is auditable
back to its audio (PRD §7.3, docs/ARCHITECTURE.md §5).

Uses structured outputs (``output_config.format`` with a strict JSON schema) so
the response is guaranteed-parseable JSON. The system prompt is cached across
chunks. Defaults to ``claude-opus-4-8`` per the Anthropic SDK guidance; override
via ``[ordersplit].model`` for cost/throughput (e.g. ``claude-sonnet-4-6``).
"""

from __future__ import annotations

import json
from importlib import resources
from typing import TYPE_CHECKING

from .config import OrderSplitConfig
from .consolidate import build_lines, chunk_lines
from .store import Record

if TYPE_CHECKING:  # avoid importing anthropic at module import time
    from anthropic import Anthropic

# Strict schema (every object sets additionalProperties:false, as required by
# structured outputs). order_index is assigned by us at merge time, not the LLM.
ORDER_SPLIT_SCHEMA = {
    "type": "object",
    "properties": {
        "orders": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "items": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "name": {"type": "string"},
                                "modifiers": {"type": "array", "items": {"type": "string"}},
                                "qty": {"type": "integer"},
                            },
                            "required": ["name", "modifiers", "qty"],
                            "additionalProperties": False,
                        },
                    },
                    "source_utterances": {"type": "array", "items": {"type": "string"}},
                    "confidence": {"type": "string", "enum": ["high", "medium", "low"]},
                },
                "required": ["items", "source_utterances", "confidence"],
                "additionalProperties": False,
            },
        }
    },
    "required": ["orders"],
    "additionalProperties": False,
}


def _load_prompt() -> str:
    return resources.files("uknomi_aggregator.prompts").joinpath("order_split.md").read_text()


class OrderSplitError(RuntimeError):
    pass


class OrderSplitter:
    def __init__(self, config: OrderSplitConfig, client: Anthropic | None = None) -> None:
        self.config = config
        self._client = client
        self._prompt = _load_prompt()

    @property
    def client(self) -> Anthropic:
        if self._client is None:
            from anthropic import Anthropic  # lazy: only needed when actually splitting

            self._client = Anthropic()  # reads ANTHROPIC_API_KEY from env
        return self._client

    def split_chunk(self, chunk_text: str) -> list[dict]:
        """Return the raw ``orders`` list for one transcript chunk."""
        resp = self.client.messages.create(
            model=self.config.model,
            max_tokens=8000,
            system=[
                {
                    "type": "text",
                    "text": self._prompt,
                    "cache_control": {"type": "ephemeral"},
                }
            ],
            thinking={"type": "adaptive"},
            output_config={
                "format": {"type": "json_schema", "schema": ORDER_SPLIT_SCHEMA},
                "effort": self.config.effort,
            },
            messages=[{"role": "user", "content": chunk_text}],
        )
        if resp.stop_reason == "refusal":
            raise OrderSplitError("model refused the order-split request")
        if resp.stop_reason == "max_tokens":
            raise OrderSplitError("order-split output truncated (raise max_tokens)")
        text = next((b.text for b in resp.content if b.type == "text"), None)
        if text is None:
            raise OrderSplitError("no text block in order-split response")
        return json.loads(text).get("orders", [])

    def split_records(self, records: list[Record], store: str, register: str) -> dict:
        """Consolidate, chunk, split, and merge into the S3 order-split document."""
        lines = build_lines(records)
        chunks = chunk_lines(lines, self.config.max_chunk_chars, self.config.overlap_chars)

        merged: list[dict] = []
        seen: set[tuple[str, ...]] = set()
        for chunk in chunks:
            for order in self.split_chunk(chunk):
                # De-dupe orders that recur across overlapping chunks by their
                # source-utterance set (overlap re-presents boundary orders).
                key = tuple(sorted(order.get("source_utterances", [])))
                if key and key in seen:
                    continue
                seen.add(key)
                merged.append(order)

        merged.sort(key=lambda o: (o.get("source_utterances") or [""])[0])
        for i, order in enumerate(merged):
            order["order_index"] = i

        window = {
            "start_utc": records[0].start_utc if records else None,
            "end_utc": records[-1].end_utc if records else None,
        }
        return {
            "store": store,
            "register": register,
            "window": window,
            "model": self.config.model,
            "orders": merged,
        }
