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
  -> always-on tools/lorawan_otm_bridge
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

The latest application-path test also demonstrates that the C5 receives real ITS-G5 traffic and the S3 locally transmits a LoRaWAN fragment. TTS application Live Data was empty for that fragment, so the current blocker is gateway/network reception of the application uplink, before the bridge.

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

DR0/SF12 is useful for controlled coverage diagnostics but is extremely slow for multi-fragment application traffic. The current firmware enables ADR after activation. Before treating this as a final mobile bicycle profile, revisit ADR/data-rate policy: moving devices experience rapidly changing RF conditions, so a configurable mobile-oriented non-ADR profile is preferable to blindly relying on stationary-device ADR behavior.

### Hardware pins

```text
SCK      GPIO7
MISO     GPIO8
MOSI     GPIO9
NSS      GPIO41
DIO1     GPIO39
RESET    GPIO42
BUSY     GPIO40
RXEN     GPIO38
TXEN     NC
DIO2     radio RF switch
DIO3     TCXO 1.8 V
GNSS PPS GPIO47
```

## What is sent over LoRaWAN

When the S3 receives `OBU_IPC_RX_FRAME` from the C5, it enqueues the raw frame before the local event-bus size conversion. The queue is bounded and non-blocking so a slow LoRaWAN link cannot stall C5 acquisition.

This is deliberately a **best-effort collection sample, not a lossless mirror of all ITS-G5 traffic**. ITS-G5 can arrive far faster than EU868 LoRaWAN can forward. When the bounded queue fills, older pending frames are discarded in favor of newer traffic; LoRaWAN duty-cycle timing always takes precedence.

One raw frame is split into versioned binary fragments. Each fragment contains:

```text
magic/version/flags
frame sequence
fragment index/count
original total length
whole-frame CRC16-CCITT
fragment data
```

The default fragment data size is 32 bytes and the application FPort is 10. Multiple LoRaWAN uplinks are normally required for one ITS-G5 frame.

The latest physical application test used a 314-byte C5 frame, producing 10 fragments. Fragment 1/10 was locally transmitted at DR0/SF12 with about 2.629 seconds of airtime, followed by a reported duty-cycle wait of about 258 seconds before fragment 2. That is suitable as a coverage diagnostic, not as a useful sustained OTM throughput profile.

## The Things Stack MQTT

Current application/device:

```text
Application ID: bicycleobu
Device ID:      bicycleobu
Cluster:        eu1.cloud.thethings.network
```

Create an application API key with permission to read application traffic. Use it only as the runtime MQTT password.

```text
MQTT host:       eu1.cloud.thethings.network
MQTT port:       8883
Username:        bicycleobu@ttn
Application UID: bicycleobu@ttn
Uplink topic:    v3/bicycleobu@ttn/devices/bicycleobu/up
```

The bridge reads `uplink_message.frm_payload`, base64-decodes it, filters FPort 10 and reassembles by device/frame sequence.

If TTS Live Data is empty, the bridge is not the cause: no application event exists for it to consume. A RadioLib `Uplink sent` line proves local radio transmission only, not gateway reception.

## OpenTrafficMap MQTT

The bridge publishes to:

```text
Broker: mqtts://cits1.opentrafficmap.org:8883
Authentication for publishing: none
```

Topics:

```text
its/<node-id>/status   retained online/offline
its/<node-id>/info     compact node/bridge metadata
its/<node-id>/packet   raw binary ITS-G5 frame
```

Use a stable unique node ID. Current development example:

```text
bicycleobu-70b3d57ed0078c82
```

## Always-on Ubuntu Docker deployment

The bridge is intended to run permanently on an Ubuntu home server. It needs no inbound ports; it only creates outbound TLS connections to TTS and OpenTrafficMap.

```bash
git clone https://github.com/niklasdathe/MainboardOBU-prototype.git
cd MainboardOBU-prototype
git switch agent/lorawan-otm-uplink
git pull --ff-only
cd tools/lorawan_otm_bridge

cp .env.example .env
nano .env
```

Set the real `TTS_MQTT_PASSWORD` in `.env`, then:

```bash
docker compose up -d --build
docker compose ps
docker compose logs -f --tail=100
```

`restart: unless-stopped` keeps the service running across container failures and Ubuntu reboots after Docker starts.

The container is intentionally small and restricted: Alpine Python, non-root user, no exposed ports, read-only root filesystem in Compose, dropped Linux capabilities and no-new-privileges.

Bridge updates:

```bash
git pull --ff-only
cd tools/lorawan_otm_bridge
docker compose up -d --build
```

The full operator guide is [`../tools/lorawan_otm_bridge/README.md`](../tools/lorawan_otm_bridge/README.md).

## End-to-end test now

Do not erase the S3 flash/NVS. A normal firmware reflash preserves the current LoRaWAN session.

1. Keep the Docker bridge running and confirm both MQTT connections.
2. Receive actual ITS-G5 traffic on the C5.
3. Confirm the S3 reports `C5 V2X RX` and a fragment transmission.
4. Confirm TTS Live Data shows FPort 10. If it does not, debug RF/gateway/session reception before the bridge.
5. Once TTS receives fragments, confirm the bridge logs each accepted fragment.
6. Wait for a complete frame and confirm `published C-ITS frame to OpenTrafficMap`.
7. Check OpenTrafficMap for the decoded traffic represented by the forwarded frame.

## Persistence rule

Routine use:

```powershell
idf.py -C firmware/s3 -p COMx flash monitor
```

Do not use `erase-flash` unless the matching TTS development end-device activation state is deliberately recreated at the same time. DevNonce/session persistence is mandatory for safe OTAA operation.
