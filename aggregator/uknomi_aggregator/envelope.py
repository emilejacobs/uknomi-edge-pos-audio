"""Segment envelope: the binary wire format shared with the capture node.

Layout (see docs/ARCHITECTURE.md §2):

    [4-byte big-endian uint32 header_len][UTF-8 JSON header][audio bytes]

The header is a small JSON object; the audio payload is raw bytes in the codec
named by ``header["codec"]`` (v1: ``pcm16`` — 16 kHz mono signed 16-bit LE).

This module is intentionally dependency-free and pure so it can be unit-tested
without a broker, and so the C firmware can mirror it byte-for-byte.
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass

SCHEMA = "uknomi.segment/1"
_HEADER_LEN = struct.Struct(">I")  # 4-byte big-endian unsigned int

# Required header keys and the schema major version we accept.
_REQUIRED_KEYS = (
    "schema",
    "store",
    "register",
    "seq",
    "start_utc",
    "end_utc",
    "sample_rate",
    "codec",
)


class EnvelopeError(ValueError):
    """Raised when a payload is malformed or uses an unsupported schema."""


@dataclass(frozen=True)
class Segment:
    """A decoded utterance: its header metadata plus the raw audio bytes."""

    header: dict
    audio: bytes

    # Convenience accessors for the load-bearing fields.
    @property
    def store(self) -> str:
        return self.header["store"]

    @property
    def register(self) -> str:
        return self.header["register"]

    @property
    def seq(self) -> int:
        return int(self.header["seq"])

    @property
    def start_utc(self) -> str:
        return self.header["start_utc"]

    @property
    def end_utc(self) -> str:
        return self.header["end_utc"]

    @property
    def sample_rate(self) -> int:
        return int(self.header["sample_rate"])

    @property
    def codec(self) -> str:
        return self.header["codec"]

    @property
    def retain_audio(self) -> bool:
        return bool(self.header.get("retain_audio", False))

    @property
    def presence(self) -> bool | None:
        """Camera presence during the utterance, or None when the feature is off."""
        value = self.header.get("presence")
        return None if value is None else bool(value)

    @property
    def key(self) -> tuple[str, str, int]:
        """The idempotency key — ``(store, register, seq)`` (ARCHITECTURE §1)."""
        return (self.store, self.register, self.seq)


def encode(header: dict, audio: bytes) -> bytes:
    """Serialise a header dict + audio payload into the wire envelope."""
    body = json.dumps(header, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return _HEADER_LEN.pack(len(body)) + body + audio


def decode(payload: bytes) -> Segment:
    """Parse a wire envelope into a :class:`Segment`.

    Raises :class:`EnvelopeError` on truncation, bad JSON, missing required
    fields, or an unsupported schema major version.
    """
    if len(payload) < _HEADER_LEN.size:
        raise EnvelopeError("payload too short for header length prefix")

    (header_len,) = _HEADER_LEN.unpack_from(payload, 0)
    start = _HEADER_LEN.size
    end = start + header_len
    if end > len(payload):
        raise EnvelopeError(
            f"declared header_len={header_len} exceeds payload ({len(payload) - start} available)"
        )

    try:
        header = json.loads(payload[start:end].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EnvelopeError(f"invalid JSON header: {exc}") from exc
    if not isinstance(header, dict):
        raise EnvelopeError("header is not a JSON object")

    missing = [k for k in _REQUIRED_KEYS if k not in header]
    if missing:
        raise EnvelopeError(f"header missing required keys: {missing}")

    _check_schema(header["schema"])
    return Segment(header=header, audio=payload[end:])


def _check_schema(schema: str) -> None:
    """Accept only matching-major schema strings (``uknomi.segment/<major>``)."""
    want_major = SCHEMA.rsplit("/", 1)[-1]
    try:
        got_major = str(schema).rsplit("/", 1)[-1]
    except Exception as exc:  # noqa: BLE001 - defensive: schema may be any type
        raise EnvelopeError(f"unparseable schema {schema!r}") from exc
    if got_major != want_major:
        raise EnvelopeError(f"unsupported schema {schema!r} (want major {want_major})")
