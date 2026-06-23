"""LLM order reconstruction: consolidated transcript -> individual orders.

A single Claude pass per transcript chunk splits the cashier's repetitive script
into individual customer orders and extracts items, emitting **traceability**
(the source utterance ``start_utc`` values it used) so any order is auditable
back to its audio (PRD §7.3, docs/ARCHITECTURE.md §5).

Both providers use structured outputs (a strict JSON schema) so the response is
guaranteed-parseable JSON:
  - ``anthropic`` (default) — Messages API, ``output_config.format`` + adaptive
    thinking, system prompt cached. Defaults to ``claude-opus-4-8``.
  - ``openai-compatible`` — any OpenAI-compatible chat endpoint (LM Studio,
    Ollama, vLLM, OpenAI) via ``response_format`` json_schema. Set
    ``[ordersplit].base_url`` + ``model``.
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


def _extract_orders(text: str) -> list[dict]:
    """Parse the ``orders`` list out of a model reply, tolerating wrapping prose."""
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        start, end = text.find("{"), text.rfind("}")
        if start == -1 or end <= start:
            raise OrderSplitError("no JSON object in order-split response") from None
        data = json.loads(text[start : end + 1])
    return data.get("orders", [])


class _AnthropicProvider:
    """Order-split via the Anthropic Messages API (structured output + adaptive thinking)."""

    def __init__(self, config: OrderSplitConfig, client: Anthropic | None = None) -> None:
        self.config = config
        self._client = client

    @property
    def client(self) -> Anthropic:
        if self._client is None:
            from anthropic import Anthropic  # lazy: only needed when actually splitting

            self._client = Anthropic()  # reads ANTHROPIC_API_KEY from env
        return self._client

    def complete(self, system: str, user_text: str) -> list[dict]:
        resp = self.client.messages.create(
            model=self.config.model,
            max_tokens=8000,
            system=[{"type": "text", "text": system, "cache_control": {"type": "ephemeral"}}],
            thinking={"type": "adaptive"},
            output_config={
                "format": {"type": "json_schema", "schema": ORDER_SPLIT_SCHEMA},
                "effort": self.config.effort,
            },
            messages=[{"role": "user", "content": user_text}],
        )
        if resp.stop_reason == "refusal":
            raise OrderSplitError("model refused the order-split request")
        if resp.stop_reason == "max_tokens":
            raise OrderSplitError("order-split output truncated (raise max_tokens)")
        text = next((b.text for b in resp.content if b.type == "text"), None)
        if text is None:
            raise OrderSplitError("no text block in order-split response")
        return json.loads(text).get("orders", [])


class _OpenAICompatProvider:
    """Order-split via any OpenAI-compatible chat endpoint (LM Studio, Ollama, vLLM, OpenAI).

    Uses OpenAI structured outputs (``response_format`` json_schema) so the reply
    is schema-constrained JSON, mirroring the Anthropic path.
    """

    def __init__(self, config: OrderSplitConfig) -> None:
        self.config = config
        self._client = None

    @property
    def client(self):
        if self._client is None:
            from openai import OpenAI  # lazy: optional [openai] extra

            self._client = OpenAI(
                base_url=self.config.base_url,
                api_key=self.config.api_key or "not-needed",  # LM Studio ignores it
            )
        return self._client

    def complete(self, system: str, user_text: str) -> list[dict]:
        resp = self.client.chat.completions.create(
            model=self.config.model,
            messages=[
                {"role": "system", "content": system},
                {"role": "user", "content": user_text},
            ],
            response_format={
                "type": "json_schema",
                "json_schema": {
                    "name": "order_split",
                    "schema": ORDER_SPLIT_SCHEMA,
                    "strict": True,
                },
            },
            temperature=0,
            max_tokens=8000,
        )
        msg = resp.choices[0].message
        text = (msg.content or "").strip()
        if not text:
            # Some reasoning models (e.g. Qwen3 via LM Studio) route the
            # schema-constrained answer into reasoning_content, leaving content
            # empty. Recover the JSON from there.
            text = (getattr(msg, "reasoning_content", None) or "").strip()
        if not text:
            raise OrderSplitError("empty order-split response")
        return _extract_orders(text)


def _build_provider(config: OrderSplitConfig, client: Anthropic | None):
    if config.provider == "anthropic":
        return _AnthropicProvider(config, client)
    if config.provider in ("openai-compatible", "openai"):
        if not config.base_url:
            raise OrderSplitError(
                "ordersplit.base_url is required for the openai-compatible provider"
            )
        return _OpenAICompatProvider(config)
    raise OrderSplitError(f"unknown ordersplit provider: {config.provider!r}")


class OrderSplitter:
    def __init__(self, config: OrderSplitConfig, client: Anthropic | None = None) -> None:
        self.config = config
        self._client = client
        self._prompt = _load_prompt()
        self._provider = None

    def _get_provider(self):
        if self._provider is None:
            self._provider = _build_provider(self.config, self._client)
        return self._provider

    def split_chunk(self, chunk_text: str) -> list[dict]:
        """Return the raw ``orders`` list for one transcript chunk (delegates to the provider)."""
        return self._get_provider().complete(self._prompt, chunk_text)

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
