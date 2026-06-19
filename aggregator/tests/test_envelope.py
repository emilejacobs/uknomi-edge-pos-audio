import json
import struct

import pytest

from uknomi_aggregator.envelope import SCHEMA, EnvelopeError, decode, encode


def _header(**over):
    h = {
        "schema": SCHEMA,
        "store": "store03",
        "register": "reg2",
        "seq": 7,
        "start_utc": "2026-06-18T12:03:15.123Z",
        "end_utc": "2026-06-18T12:03:18.512Z",
        "sample_rate": 16000,
        "codec": "pcm16",
        "retain_audio": True,
    }
    h.update(over)
    return h


def test_round_trip_preserves_header_and_audio():
    audio = b"\x01\x02\x03\x04" * 100
    seg = decode(encode(_header(), audio))
    assert seg.audio == audio
    assert seg.store == "store03"
    assert seg.register == "reg2"
    assert seg.seq == 7
    assert seg.sample_rate == 16000
    assert seg.codec == "pcm16"
    assert seg.retain_audio is True
    assert seg.key == ("store03", "reg2", 7)


def test_empty_audio_is_valid():
    seg = decode(encode(_header(), b""))
    assert seg.audio == b""


def test_truncated_payload_rejected():
    with pytest.raises(EnvelopeError):
        decode(b"\x00\x00")  # shorter than the 4-byte length prefix


def test_header_length_overruns_payload():
    payload = struct.pack(">I", 9999) + b"{}"
    with pytest.raises(EnvelopeError):
        decode(payload)


def test_missing_required_key_rejected():
    h = _header()
    del h["seq"]
    with pytest.raises(EnvelopeError):
        decode(encode(h, b""))


def test_unsupported_schema_major_rejected():
    with pytest.raises(EnvelopeError):
        decode(encode(_header(schema="uknomi.segment/2"), b""))


def test_bad_json_header_rejected():
    payload = struct.pack(">I", 3) + b"not"
    with pytest.raises(EnvelopeError):
        decode(payload)


def test_retain_audio_defaults_false_when_absent():
    h = _header()
    del h["retain_audio"]
    seg = decode(encode(h, b""))
    assert seg.retain_audio is False


def test_header_is_deterministic_sorted_json():
    # Sorted keys keep the prefix stable (matters for the firmware mirror).
    body = encode(_header(), b"")
    (n,) = struct.unpack_from(">I", body, 0)
    parsed = json.loads(body[4 : 4 + n])
    assert list(parsed) == sorted(parsed)
