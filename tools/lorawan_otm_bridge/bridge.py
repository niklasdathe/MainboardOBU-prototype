#!/usr/bin/env python3
"""Relay BicycleOBU LoRaWAN uplinks from The Things Stack to OpenTrafficMap MQTT."""

from __future__ import annotations

import base64
import json
import logging
import os
import signal
import sys
import threading
from dataclasses import dataclass

import paho.mqtt.client as mqtt

from protocol import FragmentError, Reassembler


LOG = logging.getLogger("lorawan_otm_bridge")


@dataclass(frozen=True)
class Config:
    tts_host: str
    tts_port: int
    tts_username: str
    tts_password: str
    tts_application_uid: str
    otm_host: str
    otm_port: int
    otm_node_id: str
    otm_version: str
    otm_hw_variant: str
    otm_emac: str | None
    fport: int
    device_id: str | None
    reassembly_timeout_s: float

    @classmethod
    def from_env(cls) -> "Config":
        required = {
            "TTS_MQTT_HOST": os.getenv("TTS_MQTT_HOST", ""),
            "TTS_MQTT_USERNAME": os.getenv("TTS_MQTT_USERNAME", ""),
            "TTS_MQTT_PASSWORD": os.getenv("TTS_MQTT_PASSWORD", ""),
            "OTM_NODE_ID": os.getenv("OTM_NODE_ID", ""),
        }
        missing = [name for name, value in required.items() if not value]
        if missing:
            raise ValueError("missing required environment variables: " + ", ".join(missing))

        node_id = required["OTM_NODE_ID"]
        if any(char in node_id for char in "/+#"):
            raise ValueError("OTM_NODE_ID must not contain '/', '+' or '#'")

        username = required["TTS_MQTT_USERNAME"]
        return cls(
            tts_host=required["TTS_MQTT_HOST"],
            tts_port=int(os.getenv("TTS_MQTT_PORT", "8883")),
            tts_username=username,
            tts_password=required["TTS_MQTT_PASSWORD"],
            tts_application_uid=os.getenv("TTS_APPLICATION_UID", username),
            otm_host=os.getenv("OTM_MQTT_HOST", "cits1.opentrafficmap.org"),
            otm_port=int(os.getenv("OTM_MQTT_PORT", "8883")),
            otm_node_id=node_id,
            otm_version=os.getenv("OTM_VERSION", "MainboardOBU-prototype/lorawan-otm-bridge"),
            otm_hw_variant=os.getenv("OTM_HW_VARIANT", "xiao-esp32s3+wio-sx1262"),
            otm_emac=os.getenv("OTM_EMAC") or None,
            fport=int(os.getenv("LORAWAN_FRAME_FPORT", "10")),
            device_id=os.getenv("LORAWAN_DEVICE_ID") or None,
            reassembly_timeout_s=float(os.getenv("LORAWAN_REASSEMBLY_TIMEOUT_S", "600")),
        )


class Bridge:
    def __init__(self, config: Config) -> None:
        self.config = config
        self.reassembler = Reassembler(timeout_seconds=config.reassembly_timeout_s)
        self.stop_event = threading.Event()
        self.otm_connected = threading.Event()

        self.otm = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"bicycleobu-otm-{os.getpid()}",
            protocol=mqtt.MQTTv311,
        )
        self.otm.tls_set()
        self.otm.will_set(self.status_topic, payload="offline", qos=0, retain=True)
        self.otm.on_connect = self._on_otm_connect
        self.otm.on_disconnect = self._on_disconnect

        self.tts = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"bicycleobu-tts-{os.getpid()}",
            protocol=mqtt.MQTTv311,
        )
        self.tts.username_pw_set(config.tts_username, config.tts_password)
        self.tts.tls_set()
        self.tts.on_connect = self._on_tts_connect
        self.tts.on_disconnect = self._on_disconnect
        self.tts.on_message = self._on_tts_message

    @property
    def packet_topic(self) -> str:
        return f"its/{self.config.otm_node_id}/packet"

    @property
    def status_topic(self) -> str:
        return f"its/{self.config.otm_node_id}/status"

    @property
    def info_topic(self) -> str:
        return f"its/{self.config.otm_node_id}/info"

    @property
    def tts_topic(self) -> str:
        device = self.config.device_id or "+"
        return f"v3/{self.config.tts_application_uid}/devices/{device}/up"

    def _publish_otm_info(self) -> None:
        info = {
            "ver": self.config.otm_version,
            "hwv": self.config.otm_hw_variant,
            "transport": "lorawan",
        }
        if self.config.otm_emac:
            info["emac"] = self.config.otm_emac
        self.otm.publish(
            self.info_topic,
            payload=json.dumps(info, separators=(",", ":")),
            qos=0,
            retain=False,
        )

    def _on_otm_connect(self, client, userdata, flags, reason_code, properties) -> None:
        del userdata, flags, properties
        if reason_code.is_failure:
            LOG.error("OpenTrafficMap MQTT connection rejected: %s", reason_code)
            return
        self.otm_connected.set()
        LOG.info("connected to OpenTrafficMap MQTT at %s:%d", self.config.otm_host, self.config.otm_port)
        client.publish(self.status_topic, payload="online", qos=0, retain=True)
        self._publish_otm_info()

    def _on_tts_connect(self, client, userdata, flags, reason_code, properties) -> None:
        del userdata, flags, properties
        if reason_code.is_failure:
            LOG.error("The Things Stack MQTT connection rejected: %s", reason_code)
            return
        LOG.info("connected to The Things Stack MQTT; subscribing to %s", self.tts_topic)
        client.subscribe(self.tts_topic, qos=0)

    def _on_disconnect(self, client, userdata, disconnect_flags, reason_code, properties) -> None:
        del userdata, disconnect_flags, properties
        if client is self.otm:
            self.otm_connected.clear()
        if reason_code.is_failure:
            LOG.warning("MQTT disconnected unexpectedly: %s", reason_code)

    def _on_tts_message(self, client, userdata, message) -> None:
        del client, userdata
        try:
            envelope = json.loads(message.payload)
            uplink = envelope["uplink_message"]
            if int(uplink.get("f_port", 0)) != self.config.fport:
                return

            device_id = envelope["end_device_ids"]["device_id"]
            if self.config.device_id and device_id != self.config.device_id:
                return

            payload = base64.b64decode(uplink["frm_payload"], validate=True)
            frame = self.reassembler.add(device_id, payload)
            if frame is None:
                return

            if not self.otm_connected.is_set():
                LOG.warning("dropping completed frame because OpenTrafficMap MQTT is disconnected")
                return

            result = self.otm.publish(self.packet_topic, payload=frame, qos=0, retain=False)
            if result.rc != mqtt.MQTT_ERR_SUCCESS:
                LOG.error("OpenTrafficMap publish enqueue failed: rc=%s", result.rc)
                return
            LOG.info(
                "published C-ITS frame to OpenTrafficMap: device=%s bytes=%d topic=%s",
                device_id,
                len(frame),
                self.packet_topic,
            )
        except (KeyError, TypeError, ValueError, json.JSONDecodeError, FragmentError) as exc:
            LOG.warning("discarding malformed LoRaWAN uplink: %s", exc)

    def run(self) -> None:
        LOG.info("connecting OpenTrafficMap MQTT %s:%d", self.config.otm_host, self.config.otm_port)
        self.otm.connect(self.config.otm_host, self.config.otm_port, keepalive=60)
        self.otm.loop_start()
        if not self.otm_connected.wait(timeout=10):
            raise RuntimeError("OpenTrafficMap MQTT did not connect within 10 seconds")

        LOG.info("connecting The Things Stack MQTT %s:%d", self.config.tts_host, self.config.tts_port)
        self.tts.connect(self.config.tts_host, self.config.tts_port, keepalive=60)
        self.tts.loop_start()

        self.stop_event.wait()

        try:
            self.otm.publish(self.status_topic, payload="offline", qos=0, retain=True).wait_for_publish(timeout=2)
        except RuntimeError:
            pass
        self.tts.disconnect()
        self.otm.disconnect()
        self.tts.loop_stop()
        self.otm.loop_stop()


def main() -> int:
    logging.basicConfig(
        level=os.getenv("LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        config = Config.from_env()
    except (ValueError, TypeError) as exc:
        LOG.error("%s", exc)
        return 2

    bridge = Bridge(config)

    def stop_handler(signum, frame) -> None:
        del signum, frame
        bridge.stop_event.set()

    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)

    try:
        bridge.run()
    except Exception:
        LOG.exception("bridge terminated due to an unrecoverable error")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())