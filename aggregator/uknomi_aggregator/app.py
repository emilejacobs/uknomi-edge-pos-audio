"""Aggregator entry point.

Two modes:

    uknomi-aggregator                      # ingest: subscribe, transcribe, store
    uknomi-aggregator split <store> <reg>  # run LLM order-split over stored records

Ingest decodes each MQTT envelope, de-dupes on (store, register, seq), writes a
co-keyed transcript (+ audio when retained), and uploads to S3 if enabled.
"""

from __future__ import annotations

import argparse
import logging
import os
import tempfile
import wave
from datetime import UTC, datetime
from pathlib import Path

from .config import Config, load_config
from .devices import DeviceRegistry, parse_status
from .envelope import EnvelopeError, Segment, decode
from .ordersplit import OrderSplitter
from .s3 import S3Uploader, write_orders_local
from .store import Store
from .subscriber import MqttSubscriber
from .transcribe import Transcriber, build_transcriber

log = logging.getLogger("uknomi_aggregator")


def _write_temp_wav(segment: Segment) -> Path:
    fd, name = tempfile.mkstemp(suffix=".wav")
    os.close(fd)
    path = Path(name)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(segment.sample_rate)
        wav.writeframes(segment.audio)
    return path


class Ingestor:
    def __init__(self, config: Config, store: Store, transcriber: Transcriber, s3: S3Uploader):
        self.config = config
        self.store = store
        self.transcriber = transcriber
        self.s3 = s3
        self.devices = DeviceRegistry(config.storage.data_dir / "devices.json")

    def handle_message(self, topic: str, payload: bytes) -> None:
        if topic.endswith("/status"):
            self._handle_status(topic, payload)
        else:
            self._handle_segment(payload)

    def _handle_status(self, topic: str, payload: bytes) -> None:
        status = parse_status(topic, payload)
        if status is None:
            return
        self.devices.update(status)
        log.info("device %s online at %s (rssi %s, fw %s)",
                 status.device, status.url, status.rssi, status.fw)

    def _handle_segment(self, payload: bytes) -> None:
        try:
            segment = decode(payload)
        except EnvelopeError as exc:
            log.warning("dropping malformed segment: %s", exc)
            return

        if self.store.already_processed(segment):
            log.debug("skip duplicate %s", segment.key)
            return

        retain = segment.header.get("retain_audio", self.config.retain_audio_default)
        tmp_wav = _write_temp_wav(segment)
        try:
            text = self.transcriber.transcribe_file(tmp_wav)
        finally:
            tmp_wav.unlink(missing_ok=True)

        audio_path = self.store.save_audio(segment) if retain else None
        record = self.store.save_record(segment, text, audio_path)
        self.s3.upload_record(record)
        log.info("stored %s/%s seq=%s text=%r", segment.store, segment.register, segment.seq, text)


def run_ingest(config: Config) -> None:
    store = Store(config.storage.data_dir)
    transcriber = build_transcriber(config.transcribe)
    s3 = S3Uploader(config.s3)
    ingestor = Ingestor(config, store, transcriber, s3)
    subscriber = MqttSubscriber(config.mqtt, ingestor.handle_message)
    log.info("aggregator ingest starting (broker %s:%s)", config.mqtt.host, config.mqtt.port)
    subscriber.run_forever()


def run_split(config: Config, store_id: str, register: str) -> None:
    store = Store(config.storage.data_dir)
    records = store.load_register_records(store_id, register)
    if not records:
        log.warning("no records for %s/%s", store_id, register)
        return
    splitter = OrderSplitter(config.ordersplit)
    doc = splitter.split_records(records, store_id, register)
    run_id = datetime.now(UTC).strftime("%Y-%m-%dT%H-%M-%SZ")
    local = write_orders_local(config.storage.data_dir, store_id, register, run_id, doc)
    key = S3Uploader(config.s3).upload_orders(store_id, register, run_id, doc)
    where = f"{local}" + (f" (s3 key {key})" if key else "")
    log.info("wrote %d orders -> %s", len(doc["orders"]), where)


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    parser = argparse.ArgumentParser(prog="uknomi-aggregator")
    parser.add_argument("--config", help="path to aggregator.toml")
    sub = parser.add_subparsers(dest="cmd")
    sp = sub.add_parser("split", help="run LLM order-split over stored records")
    sp.add_argument("store")
    sp.add_argument("register")
    args = parser.parse_args()

    config = load_config(args.config)
    if args.cmd == "split":
        run_split(config, args.store, args.register)
    else:
        run_ingest(config)


if __name__ == "__main__":
    main()
