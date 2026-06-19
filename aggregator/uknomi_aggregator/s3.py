"""S3 upload: transcripts + orders always; audio only when retained.

Key layout mirrors PRD §8 / docs/ARCHITECTURE.md §3. boto3 reads AWS credentials
from the environment / instance profile. A disabled uploader is a no-op so the
pipeline runs end-to-end locally without S3.
"""

from __future__ import annotations

import json
from pathlib import Path

from .config import S3Config
from .store import Record


class S3Uploader:
    def __init__(self, config: S3Config) -> None:
        self.config = config
        self._client = None

    @property
    def enabled(self) -> bool:
        return self.config.enabled and bool(self.config.bucket)

    @property
    def client(self):
        if self._client is None:
            import boto3  # lazy: only needed when S3 is enabled

            self._client = boto3.client("s3", region_name=self.config.region)
        return self._client

    def _key(self, *parts: str) -> str:
        prefix = self.config.prefix.strip("/")
        joined = "/".join(p.strip("/") for p in parts if p)
        return f"{prefix}/{joined}" if prefix else joined

    def upload_record(self, record: Record) -> None:
        """Upload one utterance's transcript JSON (+ audio if it was retained)."""
        if not self.enabled:
            return
        ts = record.start_utc.replace(":", "-")
        self.client.put_object(
            Bucket=self.config.bucket,
            Key=self._key("transcripts", record.store, record.register, f"{ts}.json"),
            Body=record.json_path.read_bytes(),
            ContentType="application/json",
        )
        if record.audio_path and record.audio_path.is_file():
            self.client.put_object(
                Bucket=self.config.bucket,
                Key=self._key("audio", record.store, record.register, f"{ts}.wav"),
                Body=record.audio_path.read_bytes(),
                ContentType="audio/wav",
            )

    def upload_orders(self, store: str, register: str, run_id: str, doc: dict) -> str | None:
        """Upload a consolidated order-split document; returns the key (or None)."""
        if not self.enabled:
            return None
        date = (doc.get("window", {}).get("start_utc") or run_id)[:10]
        key = self._key("orders", store, register, date, f"{run_id}.json")
        self.client.put_object(
            Bucket=self.config.bucket,
            Key=key,
            Body=json.dumps(doc, indent=2).encode("utf-8"),
            ContentType="application/json",
        )
        return key


def write_orders_local(data_dir: Path, store: str, register: str, run_id: str, doc: dict) -> Path:
    """Always also write the order-split document to disk for local inspection."""
    out_dir = data_dir / store / register / "orders"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{run_id}.json"
    path.write_text(json.dumps(doc, indent=2))
    return path
