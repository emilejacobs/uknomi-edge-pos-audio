import json

from uknomi_aggregator.devices import DeviceRegistry, parse_status


def test_parse_status_extracts_fields_and_url():
    payload = json.dumps({"ip": "192.168.1.57", "rssi": -52, "fw": "0.1.0", "uptime_s": 1840})
    st = parse_status("uknomi/store03/reg2/status", payload.encode())
    assert st is not None
    assert st.store == "store03" and st.register == "reg2"
    assert st.device == "store03/reg2"
    assert st.ip == "192.168.1.57"
    assert st.url == "http://192.168.1.57/"
    assert st.rssi == -52 and st.fw == "0.1.0" and st.uptime_s == 1840
    assert st.last_seen_utc.endswith("Z")


def test_parse_status_rejects_wrong_topic_and_bad_json():
    good = b'{"ip": "10.0.0.5"}'
    assert parse_status("uknomi/s/r/segment", good) is None  # not a status topic
    assert parse_status("other/s/r/status", good) is None  # wrong root
    assert parse_status("uknomi/s/r/status", b"not json") is None
    assert parse_status("uknomi/s/r/status", b"[1,2,3]") is None  # not an object


def test_registry_writes_devices_json(tmp_path):
    reg = DeviceRegistry(tmp_path / "devices.json")
    reg.update(parse_status("uknomi/store03/reg2/status", b'{"ip": "192.168.1.57"}'))
    reg.update(parse_status("uknomi/store03/reg1/status", b'{"ip": "192.168.1.58"}'))
    # An update for an existing device replaces, not duplicates.
    reg.update(parse_status("uknomi/store03/reg2/status", b'{"ip": "192.168.1.99"}'))

    data = json.loads((tmp_path / "devices.json").read_text())
    assert set(data) == {"store03/reg2", "store03/reg1"}
    assert data["store03/reg2"]["ip"] == "192.168.1.99"
    assert data["store03/reg2"]["url"] == "http://192.168.1.99/"
