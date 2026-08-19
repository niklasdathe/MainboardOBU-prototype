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

OpenTrafficMap expects the packet payload as raw binary, not JSON or base64. The bridge also publishes the retained `its/<node-id>/status` state and a small `its/<node-id>/info` JSON document.

## 1. Create a TTS application API key

In The Things Stack Console, open the `bicycleobu` application and create an API key that can read application traffic. Keep the key outside Git and use it as the MQTT password.

For The Things Network Sandbox the application MQTT username/topic prefix includes the tenant suffix, for example:

```text
bicycleobu@ttn
```

## 2. Install the bridge

PowerShell from the repository root:

```powershell
cd tools\lorawan_otm_bridge
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

## 3. Configure the current BicycleOBU test device

The current TTS application is `bicycleobu` and the current end-device ID is `bicycleobu`. Use a stable, globally distinctive OpenTrafficMap node ID; the example below derives it from the current DevEUI without exposing any root key.

```powershell
$env:TTS_MQTT_HOST = 'eu1.cloud.thethings.network'
$env:TTS_MQTT_USERNAME = 'bicycleobu@ttn'
$env:TTS_APPLICATION_UID = 'bicycleobu@ttn'
$env:TTS_MQTT_PASSWORD = '<TTS application API key>'
$env:LORAWAN_DEVICE_ID = 'bicycleobu'
$env:LORAWAN_FRAME_FPORT = '10'

$env:OTM_NODE_ID = 'bicycleobu-70b3d57ed0078c82'
```

OpenTrafficMap publishing currently uses TLS on `cits1.opentrafficmap.org:8883` and does not require a username/password. Node registration is optional for packet ingestion; use the OpenTrafficMap node-registration/self-service process if a friendly receiver name, map-visible receiver location, or subscriber credentials are wanted.

Optional metadata overrides:

```powershell
$env:OTM_VERSION = 'MainboardOBU-prototype/lorawan-otm-bridge'
$env:OTM_HW_VARIANT = 'xiao-esp32s3+wio-sx1262'
```

Do not invent an Ethernet MAC for `OTM_EMAC`. Set it only if a real MAC is intentionally associated with this OTM node.

## 4. Run

```powershell
python bridge.py
```

Expected startup:

```text
connected to OpenTrafficMap MQTT at cits1.opentrafficmap.org:8883
connected to The Things Stack MQTT; subscribing to v3/bicycleobu@ttn/devices/bicycleobu/up
```

When all fragments of one C5 frame arrive:

```text
published C-ITS frame to OpenTrafficMap: device=bicycleobu bytes=<n> topic=its/bicycleobu-70b3d57ed0078c82/packet
```

One received ITS-G5 frame usually spans multiple LoRaWAN uplinks, so seeing an individual TTS uplink does not imply an immediate OTM publish.

## 5. End-to-end check

1. Keep the S3 session/NVS intact; normal `flash` is fine, `erase-flash` is not.
2. Start this bridge and confirm both MQTT connections.
3. Put the C5 where it receives ITS-G5 traffic.
4. Confirm the S3 log reports C5 RX frames and LoRaWAN fragments/application uplinks.
5. Confirm TTS application Live Data shows FPort 10 uplinks.
6. Confirm the bridge logs one completed `published C-ITS frame` line.
7. Check OpenTrafficMap for the decoded traffic represented by those frames. A node does not have to be registered for ingestion, but registration/friendly naming makes receiver attribution and map placement easier.

If TTS receives no FPort 10 application uplinks, debug the LoRaWAN/application path first. If TTS receives all fragments but the bridge does not publish, run with `$env:LOG_LEVEL='DEBUG'` and check FPort/device filtering and reassembly errors. If the bridge publishes successfully but nothing is rendered by OTM, verify that the C5 payload is a valid raw ITS-G5/802.11p frame and that the OTM node/topic is correct.
