import wave

from uknomi_aggregator.envelope import SCHEMA, decode, encode
from uknomi_aggregator.store import Store


def _segment(seq=1, start="2026-06-18T12:03:15.123Z", retain=True, audio=b"\x10\x00" * 1600):
    header = {
        "schema": SCHEMA, "store": "store03", "register": "reg2", "seq": seq,
        "start_utc": start, "end_utc": "2026-06-18T12:03:18.512Z",
        "sample_rate": 16000, "codec": "pcm16", "retain_audio": retain,
    }
    return decode(encode(header, audio))


def test_save_record_and_idempotency(tmp_path):
    store = Store(tmp_path)
    seg = _segment()
    assert store.already_processed(seg) is False
    store.save_record(seg, "hello there", None)
    assert store.already_processed(seg) is True

    # JSON co-keyed by filesystem-safe timestamp.
    expected = tmp_path / "store03" / "reg2" / "2026-06-18T12-03-15.123Z.json"
    assert expected.is_file()


def test_save_audio_writes_playable_wav(tmp_path):
    store = Store(tmp_path)
    seg = _segment(audio=b"\x10\x00" * 1600)  # 1600 frames
    path = store.save_audio(seg)
    with wave.open(str(path), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getsampwidth() == 2
        assert wav.getframerate() == 16000
        assert wav.getnframes() == 1600


def test_load_register_records_sorted_by_start(tmp_path):
    store = Store(tmp_path)
    store.save_record(_segment(seq=2, start="2026-06-18T12:05:00.000Z"), "second", None)
    store.save_record(_segment(seq=1, start="2026-06-18T12:03:00.000Z"), "first", None)
    records = store.load_register_records("store03", "reg2")
    assert [r.text for r in records] == ["first", "second"]
    assert records[0].start_utc < records[1].start_utc
