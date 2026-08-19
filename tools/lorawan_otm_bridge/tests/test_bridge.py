import os
import unittest
from unittest.mock import patch

from bridge import Bridge, Config


class BridgeConfigTests(unittest.TestCase):
    def test_topics_use_current_tts_and_otm_layout(self):
        cfg = Config(
            tts_host="eu1.cloud.thethings.network",
            tts_port=8883,
            tts_username="bicycleobu@ttn",
            tts_password="secret",
            tts_application_uid="bicycleobu@ttn",
            otm_host="cits1.opentrafficmap.org",
            otm_port=8883,
            otm_node_id="bicycleobu-test",
            otm_version="test",
            otm_hw_variant="test",
            otm_emac=None,
            fport=10,
            device_id="bicycleobu",
            reassembly_timeout_s=600,
        )
        bridge = Bridge(cfg)
        self.assertEqual(bridge.tts_topic, "v3/bicycleobu@ttn/devices/bicycleobu/up")
        self.assertEqual(bridge.packet_topic, "its/bicycleobu-test/packet")
        self.assertEqual(bridge.status_topic, "its/bicycleobu-test/status")
        self.assertEqual(bridge.info_topic, "its/bicycleobu-test/info")

    def test_config_rejects_topic_wildcards_in_node_id(self):
        env = {
            "TTS_MQTT_HOST": "eu1.cloud.thethings.network",
            "TTS_MQTT_USERNAME": "bicycleobu@ttn",
            "TTS_MQTT_PASSWORD": "secret",
            "OTM_NODE_ID": "bad/node",
        }
        with patch.dict(os.environ, env, clear=True):
            with self.assertRaisesRegex(ValueError, "OTM_NODE_ID"):
                Config.from_env()


if __name__ == "__main__":
    unittest.main()
