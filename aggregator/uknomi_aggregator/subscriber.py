"""MQTT ingest: subscribe to segment + status topics and route to a callback.

Thin wrapper over paho-mqtt v2. The callback receives ``(topic, payload)``;
decoding/idempotency/routing live in the app so this stays testable and dumb.
"""

from __future__ import annotations

import logging
from collections.abc import Callable

import paho.mqtt.client as mqtt

from .config import MqttConfig

log = logging.getLogger(__name__)

MessageHandler = Callable[[str, bytes], None]


class MqttSubscriber:
    def __init__(self, config: MqttConfig, handler: MessageHandler) -> None:
        self.config = config
        self.handler = handler
        self._client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2, client_id=config.client_id
        )
        if config.username:
            self._client.username_pw_set(config.username, config.password)
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        if reason_code != 0:
            log.error("MQTT connect failed: %s", reason_code)
            return
        topics = [(self.config.topic, 1), (self.config.status_topic, 1)]
        log.info("connected to broker; subscribing to %s", [t for t, _ in topics])
        client.subscribe(topics)

    def _on_message(self, client, userdata, msg) -> None:
        try:
            self.handler(msg.topic, msg.payload)
        except Exception:  # noqa: BLE001 - never let one bad message kill the loop
            log.exception("failed to handle message on %s", msg.topic)

    def run_forever(self) -> None:
        self._client.connect(self.config.host, self.config.port, self.config.keepalive)
        self._client.loop_forever()

    def stop(self) -> None:
        self._client.disconnect()
