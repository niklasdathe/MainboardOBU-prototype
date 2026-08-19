# LoRaWAN -> OpenTrafficMap

This is the operational path for forwarding ITS-G5 frames received by the C5 to OpenTrafficMap through the optional Wio-SX1262 module.

```text
ITS-G5 air
  -> ESP32-C5 raw RX frame
  -> S3 SPI IPC
  -> bounded LoRaWAN fragment queue
  -> Wio-SX1262 / EU868
  -> any usable LoRaWAN gateway
  -> The Things Stack
  -> tools/lorawan_otm_bridge
  -> mqtts://cits1.opentrafficmap.org:8883
  -> its/<node-id>/packet
```

The bridge reconstructs the original raw frame and publishes it as binary. It does not convert the C-ITS packet to JSON or invent missing data.

## Current verified state

Physical testing has demonstrated a valid LoRaWAN session being persisted and restored after a normal firmware reflash:

```text
nonce_state=present session_state=present
session_restored=yes
RADIOLIB_LORAWAN_SESSION_RESTORED (-1117)
activated=yes
```

The remaining end-to-end work is therefore application traffic: receive a real C5 frame, carry all of its fragments through TTS, reassemble it in the bridge and observe the resulting raw packet on OpenTrafficMap.

See [`LORAWAN_OTAA_DEBUG.md`](LORAWAN_OTAA_DEBUG.md) only if activation stops working again.

## Firmware configuration

Build with ESP-IDF 6.0.2 and the pinned RadioLib dependency. If the manifest pin has changed locally, refresh the component lock first:

```powershell
idf.py -C firmware/s3 update-dependencies
idf.py -C firmware/s3 reconfigure
```

The build must resolve:

```text
RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
Platform: "ESP-IDF"
```

Configure under:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
```

The menu is grouped into activation/network, payload/buffering, airtime/retries and diagnostics.

Current development profile:

```text
Region:      EU868
LoRaWAN:     1.1 OTAA
JoinEUI:     1111111111111111
DevEUI:      70B3D57ED0078C82
TTS plan:    Europe 863-870 MHz (SF12 for RX2)
TTS RP:      RP002 1.0.4
FPort:       10
```

Root keys are secrets and must match TTS exactly; do not commit them.

DR0/SF12 is currently useful for the controlled coverage test. The initial join DR is configurable from DR0..DR5, while ADR is enabled for the active session.

### Hardware pins

```text
SCK     GPIO7
MISO    GPIO8
MOSI    GPIO9
NSS     GPIO41
DIO1    GPIO39
RESET   GPIO42
BUSY    GPIO40
RXEN    GPIO38
TXEN    NC
DIO2    radio RF switch
DIO3    TCXO 1.8 V
GNSS PPS GPIO47
```

## What is sent over LoRaWAN

When the S3 receives `OBU_IPC_RX_FRAME` from the C5, it enqueues the raw frame before the local event-bus size conversion. The queue is bounded and non-blocking so a slow LoRaWAN link cannot stall C5 acquisition.

One raw frame is split into versioned binary fragments. Each fragment contains:

```text
magic/version/flags
frame sequence
fragment index/count
original total length
whole-frame CRC16-CCITT
fragment data
```

The default fragment data size is 32 bytes and the application FPort is 10. Multiple LoRaWAN uplinks are therefore normally required for one ITS-G5 frame.

## The Things Stack MQTT

The current application is:

```text
Application ID: bicycleobu
Device ID:      bicycleobu
Cluster:        eu1.cloud.thethings.network
```

Create an application API key in the TTS Console with permission to read application traffic. The API key is the MQTT password.

For The Things Network tenant, use:

```text
MQTT host:      eu1.cloud.thethings.network
MQTT port:      8883
Username:       bicycleobu@ttn
Application UID bicycleobu@ttn
Uplink topic:   v3/bicycleobu@ttn/devices/bicycleobu/up
```

TTS application MQTT uses MQTT 3.1.1/QoS 0. The bridge reads `uplink_message.frm_payload`, base64-decodes it, filters FPort 10 and reassembles by device/frame sequence.

## OpenTrafficMap MQTT

Current OpenTrafficMap receiver documentation uses:

```text
Broker: mqtts://cits1.opentrafficmap.org:8883
Authentication for publishing: none
```

The bridge publishes:

```text
its/<node-id>/status   retained online/offline
its/<node-id>/info     compact node/bridge metadata
its/<node-id>/packet   raw binary ITS-G5 frame
```

OpenTrafficMap node registration is optional for packet ingestion. Registration/self-service is useful when a friendly receiver name, map location or subscriber credentials are wanted.

Use a stable unique node ID. For the current development device:

```text
bicycleobu-70b3d57ed0078c82
```

Do not invent an Ethernet MAC just to fill OTM metadata.

## Run the bridge on Windows

The bridge has its own concise guide at [`../tools/lorawan_otm_bridge/README.md`](../tools/lorawan_otm_bridge/README.md).

From the repository root:

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

Expected startup:

```text
connected to OpenTrafficMap MQTT at cits1.opentrafficmap.org:8883
connected to The Things Stack MQTT; subscribing to v3/bicycleobu@ttn/devices/bicycleobu/up
```

A completed frame produces:

```text
published C-ITS frame to OpenTrafficMap: device=bicycleobu bytes=<n> topic=its/bicycleobu-70b3d57ed0078c82/packet
```

## End-to-end test now

Do not erase the S3 flash/NVS. A normal firmware reflash preserves the current LoRaWAN session.

1. Start `python bridge.py` and confirm both MQTT connections.
2. Put the C5 where it receives actual ITS-G5 traffic.
3. Confirm the S3 reports `C5 V2X RX` frames.
4. Confirm TTS Live Data shows application uplinks on FPort 10.
5. Wait for all fragments of at least one frame; DR0 can make this slow because of duty-cycle limits.
6. Confirm the bridge logs `published C-ITS frame to OpenTrafficMap`.
7. Check OpenTrafficMap for the decoded object/message represented by the forwarded frame.

If step 3 works but step 4 does not, inspect LoRaWAN queue/session statistics. If step 4 works but step 6 does not, inspect device/FPort filtering and fragment reassembly. If step 6 works but OTM shows nothing, verify that the C5 frame is the raw ITS-G5/802.11p packet format expected by OTM and that the node/topic is correct.

## Persistence rule

Routine use:

```powershell
idf.py -C firmware/s3 -p COMx flash monitor
```

Do not use `erase-flash` unless the matching TTS development end-device activation state is deliberately recreated at the same time. DevNonce/session persistence is mandatory for safe OTAA operation.
