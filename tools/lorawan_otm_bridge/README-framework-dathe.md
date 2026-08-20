# LoRaWAN -> OpenTrafficMap bridge

This bridge subscribes to BicycleOBU application uplinks in The Things Stack, reassembles the fragmented raw ITS-G5 frame, verifies its length and CRC16, and republishes the original binary frame to OpenTrafficMap MQTT.

```text
C5 raw ITS-G5 frame
  -> S3/Wio-SX1262 LoRaWAN fragments (FPort 10)
  -> The Things Stack application MQTT
  -> this bridge
  -> mqtts://cits1.opentrafficmap.org:8883
  -> its/<node-id>/packet
```

OpenTrafficMap receives the raw binary frame, not JSON or base64. The bridge also publishes retained `its/<node-id>/status` and a compact `its/<node-id>/info` document.

## Recommended: always-on Ubuntu Docker deployment

The container has no listening ports. It only creates outbound TLS connections to TTS and OpenTrafficMap, so no router port-forwarding is required.

Install Docker Engine + the Compose plugin on the Ubuntu server, clone the repository, then:

```bash
git switch agent/lorawan-otm-uplink
git pull --ff-only
cd tools/lorawan_otm_bridge
cp .env.example .env
nano .env
```

Set `TTS_MQTT_PASSWORD` to a TTS application API key that can read application traffic. Keep `.env` private; it is ignored by Git.

Current prototype defaults in `.env.example` are already set for:

```text
TTS host:       eu1.cloud.thethings.network
TTS user/UID:   bicycleobu@ttn
TTS device:     bicycleobu
FPort:          10
OTM broker:     cits1.opentrafficmap.org:8883
OTM node:       bicycleobu-70b3d57ed0078c82
```

Build and start once:

```bash
docker compose up -d --build
```

The service uses `restart: unless-stopped`, so Docker starts it again after an Ubuntu reboot or container failure.

Useful operations:

```bash
docker compose ps
docker compose logs -f --tail=100
docker compose restart
docker compose up -d --build    # after pulling bridge updates
docker compose down             # intentionally stop/remove it
```

Expected startup log:

```text
connected to OpenTrafficMap MQTT at cits1.opentrafficmap.org:8883
connected to The Things Stack MQTT; subscribing to v3/bicycleobu@ttn/devices/bicycleobu/up
```

The image is based on `python:3.13-alpine`, installs only `paho-mqtt`, runs as a non-root user, uses a read-only root filesystem under Compose, drops Linux capabilities and needs no exposed ports.

## TTS API key

In The Things Stack Console, open application `bicycleobu` and create an application API key with permission to read application traffic. Use the generated key only as `TTS_MQTT_PASSWORD` in the local `.env` file.

For the TTN Sandbox tenant, the application MQTT identity/topic prefix includes `@ttn`:

```text
bicycleobu@ttn
```

The bridge subscribes to the exact current device topic when `LORAWAN_DEVICE_ID` is configured:

```text
v3/bicycleobu@ttn/devices/bicycleobu/up
```

## OpenTrafficMap side

The bridge publishes with TLS to:

```text
cits1.opentrafficmap.org:8883
```

Packet ingestion does not require publisher username/password. The topics are:

```text
its/<node-id>/status
its/<node-id>/info
its/<node-id>/packet
```

`packet` contains the reconstructed original C5 frame. Use a stable node ID. Do not invent an Ethernet MAC merely to populate metadata.

## Local development without Docker

Windows example:

```powershell
cd tools\lorawan_otm_bridge
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt

$env:TTS_MQTT_HOST = 'eu1.cloud.thethings.network'
$env:TTS_MQTT_USERNAME = 'bicycleobu@ttn'
$env:TTS_APPLICATION_UID = 'bicycleobu@ttn'
$env:TTS_MQTT_PASSWORD = '<TTS application API key>'
$env:LORAWAN_DEVICE_ID = 'bicycleobu'
$env:LORAWAN_FRAME_FPORT = '10'
$env:OTM_NODE_ID = 'bicycleobu-70b3d57ed0078c82'

python bridge.py
```

## End-to-end check

1. Keep the S3 session/NVS intact; normal `flash` is fine, routine `erase-flash` is not.
2. Confirm the bridge is connected to both MQTT brokers.
3. Receive a real ITS-G5 frame on the C5.
4. Confirm the S3 reports `C5 V2X RX` and a LoRaWAN fragment/application uplink.
5. Confirm TTS application Live Data shows FPort 10.
6. Wait until every fragment of one frame arrives.
7. Confirm the bridge logs:

```text
published C-ITS frame to OpenTrafficMap: device=bicycleobu bytes=<n> topic=its/bicycleobu-70b3d57ed0078c82/packet
```

8. Check OpenTrafficMap for the decoded traffic represented by that frame.

One ITS-G5 frame normally spans multiple LoRaWAN uplinks. The bridge intentionally stays silent at INFO level for incomplete frames and publishes only after length and whole-frame CRC verification.

### If TTS Live Data is empty

The bridge cannot receive anything until TTS receives an application uplink. Check the radio path first. A local RadioLib line such as `Uplink sent` only proves that the SX1262 completed its own transmit operation; it does not prove that a gateway heard the packet.

For the current DR0/SF12 diagnostic session, a 44-byte LoRaWAN fragment can occupy several seconds of airtime and RadioLib may then wait several minutes for legal duty-cycle availability. This is expected and also means a complete 10-fragment C-ITS frame can take a long time at DR0.

If the S3 reports a fragment as sent but TTS remains empty, move close to a known online TTN V3 gateway or use a controlled local gateway before changing the bridge. Once TTS receives FPort-10 uplinks, use `docker compose logs -f` to distinguish TTS/MQTT filtering from fragment-reassembly problems.
