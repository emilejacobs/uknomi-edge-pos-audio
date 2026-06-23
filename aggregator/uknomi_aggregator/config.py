"""Aggregator configuration: loaded from a TOML file with env overrides.

In production the Control Plane writes ``/etc/uknomi/aggregator.toml``; in dev
you point ``UKNOMI_AGGREGATOR_CONFIG`` at a local copy of
``aggregator.example.toml``. Secrets may be supplied via env vars so they never
have to live in the file:

    UKNOMI_MQTT_PASSWORD   overrides [mqtt].password
    ANTHROPIC_API_KEY      read directly by the Anthropic SDK (not stored here)
    AWS_*                  read directly by boto3 (not stored here)
"""

from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_CONFIG_PATHS = (
    os.environ.get("UKNOMI_AGGREGATOR_CONFIG"),
    "aggregator.toml",
    "/etc/uknomi/aggregator.toml",
)


@dataclass
class MqttConfig:
    host: str = "localhost"
    port: int = 1883
    username: str | None = None
    password: str | None = None
    topic: str = "uknomi/+/+/segment"  # subscribe pattern: all stores/registers
    status_topic: str = "uknomi/+/+/status"  # device IP/health announcements
    client_id: str = "uknomi-aggregator"
    keepalive: int = 60


@dataclass
class StorageConfig:
    data_dir: Path = Path("data")


@dataclass
class TranscribeConfig:
    engine: str = "faster-whisper"  # "faster-whisper" | "whisper-cpp"
    model: str = "small.en"  # faster-whisper model name, or path for whisper-cpp
    language: str = "en"
    whisper_cpp_bin: str | None = None  # required when engine == "whisper-cpp"


@dataclass
class OrderSplitConfig:
    provider: str = "anthropic"  # "anthropic" | "openai-compatible"
    model: str = "claude-opus-4-8"
    effort: str = "medium"  # low | medium | high | max (anthropic only)
    max_chunk_chars: int = 8000
    overlap_chars: int = 600
    # openai-compatible only (LM Studio / Ollama / vLLM / OpenAI):
    base_url: str | None = None  # e.g. "http://localhost:1234/v1"
    api_key: str | None = None  # any string for LM Studio; env OPENAI_API_KEY also works


@dataclass
class S3Config:
    enabled: bool = False
    bucket: str | None = None
    region: str | None = None
    prefix: str = ""


@dataclass
class Config:
    mqtt: MqttConfig = field(default_factory=MqttConfig)
    storage: StorageConfig = field(default_factory=StorageConfig)
    transcribe: TranscribeConfig = field(default_factory=TranscribeConfig)
    ordersplit: OrderSplitConfig = field(default_factory=OrderSplitConfig)
    s3: S3Config = field(default_factory=S3Config)
    # Fallback when an envelope omits retain_audio. Default OFF (transcript-only).
    retain_audio_default: bool = False


def _section(data: dict, name: str) -> dict:
    value = data.get(name, {})
    if not isinstance(value, dict):
        raise ValueError(f"[{name}] must be a table")
    return value


def load_config(path: str | os.PathLike | None = None) -> Config:
    """Load config from ``path`` (or the first existing default), apply env overrides."""
    chosen = _resolve_path(path)
    data: dict = {}
    if chosen is not None:
        with open(chosen, "rb") as fh:
            data = tomllib.load(fh)

    mqtt_raw = _section(data, "mqtt")
    cfg = Config(
        mqtt=MqttConfig(
            host=mqtt_raw.get("host", MqttConfig.host),
            port=int(mqtt_raw.get("port", MqttConfig.port)),
            username=mqtt_raw.get("username"),
            password=os.environ.get("UKNOMI_MQTT_PASSWORD", mqtt_raw.get("password")),
            topic=mqtt_raw.get("topic", MqttConfig.topic),
            status_topic=mqtt_raw.get("status_topic", MqttConfig.status_topic),
            client_id=mqtt_raw.get("client_id", MqttConfig.client_id),
            keepalive=int(mqtt_raw.get("keepalive", MqttConfig.keepalive)),
        ),
        storage=StorageConfig(
            data_dir=Path(_section(data, "storage").get("data_dir", "data")),
        ),
        transcribe=TranscribeConfig(**_kwargs(_section(data, "transcribe"), TranscribeConfig)),
        ordersplit=OrderSplitConfig(**_kwargs(_section(data, "ordersplit"), OrderSplitConfig)),
        s3=S3Config(**_kwargs(_section(data, "s3"), S3Config)),
        retain_audio_default=bool(data.get("retain_audio_default", False)),
    )
    return cfg


def _resolve_path(path: str | os.PathLike | None) -> Path | None:
    candidates = [path] if path is not None else DEFAULT_CONFIG_PATHS
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def _kwargs(raw: dict, cls: type) -> dict:
    """Pick only keys the dataclass knows, so unknown TOML keys don't crash."""
    known = cls.__dataclass_fields__.keys()  # type: ignore[attr-defined]
    return {k: v for k, v in raw.items() if k in known}
