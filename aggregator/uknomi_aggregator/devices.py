"""Device discovery from retained MQTT status messages.

Each capture node publishes a retained status to ``uknomi/<store>/<register>/status``
(``{ip, rssi, fw, uptime_s}``) so its IP can be found without access to the store's
DHCP table. The aggregator logs these and keeps a small registry on disk.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from datetime import UTC, datetime
from pathlib import Path


@dataclass
class DeviceStatus:
    store: str
    register: str
    ip: str | None
    rssi: int | None
    fw: str | None
    uptime_s: int | None
    last_seen_utc: str

    @property
    def device(self) -> str:
        return f"{self.store}/{self.register}"

    @property
    def url(self) -> str | None:
        return f"http://{self.ip}/" if self.ip else None


def parse_status(topic: str, payload: bytes) -> DeviceStatus | None:
    """Parse a status message. Returns None if the topic/payload is malformed."""
    parts = topic.split("/")
    # uknomi/<store>/<register>/status
    if len(parts) < 4 or parts[0] != "uknomi" or parts[-1] != "status":
        return None
    try:
        data = json.loads(payload)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    return DeviceStatus(
        store=parts[1],
        register=parts[2],
        ip=data.get("ip"),
        rssi=data.get("rssi"),
        fw=data.get("fw"),
        uptime_s=data.get("uptime_s"),
        last_seen_utc=datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%S.000Z"),
    )


class DeviceRegistry:
    """In-memory map of device -> latest status, persisted to ``devices.json``."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.devices: dict[str, DeviceStatus] = {}

    def update(self, status: DeviceStatus) -> None:
        self.devices[status.device] = status
        self.path.parent.mkdir(parents=True, exist_ok=True)
        out = {
            dev: {**asdict(st), "url": st.url} for dev, st in sorted(self.devices.items())
        }
        self.path.write_text(json.dumps(out, indent=2))
