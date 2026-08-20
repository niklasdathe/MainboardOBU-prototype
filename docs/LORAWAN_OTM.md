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

The physical end-to-end path has now been demonstrated, including complete frame reassembly and OpenTrafficMap publication. The bridge has logged complete examples such as:

```text
published C-ITS frame to OpenTrafficMap: device=bicycleobu bytes=456 topic=its/bicycleobu-70b3d57ed0078c82/packet
published C-ITS frame to OpenTrafficMap: device=bicycleobu bytes=314 topic=its/bicycleobu-70b3d57ed0078c82/packet
published C-ITS frame to OpenTrafficMap: device=bicycleobu bytes=225 topic=its/bicycleobu-70b3d57ed0078c82/packet
```

Physical testing has also demonstrated:

```text
nonce_state=present session_state=present
session_restored=yes
RADIOLIB_LORAWAN_SESSION_RESTORED (-1117)
activated=yes
```

and TTS Network Server reception of multiple unconfirmed FPort-10 uplinks through both Packet Broker and a current eu1 gateway.

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

### Throughput-oriented defaults

The application uplink now defaults to the fastest data rate that every currently configured TTS uplink channel supports:

```text
JoinRequest DR:             DR3 / SF9 BW125
Application uplink policy: fixed DR5 / SF7 BW125
ADR:                        disabled
Configured raw fragment:    up to 230 bytes
Extra fragment delay:       0 ms
RadioLib duty cycle:        enabled
```

The join data rate remains separate from the application rate so OTAA can use a more coverage-balanced DR without forcing slow application traffic afterward.

`OBU_LORAWAN_ADR_ENABLE` can be enabled to return application data-rate control to the network. When ADR is disabled, `OBU_LORAWAN_UPLINK_DATARATE` selects DR0..DR5 and defaults to DR5. For a moving bicycle, fixed DR5 is useful as the maximum-throughput experiment; reduce it when the RF link cannot sustain SF7.

The current TTS MAC state advertises `max_data_rate_index=5` on the active uplink channels, so DR5 is the fastest rate used by this default profile. DR6/DR7 are not forced onto a session whose active channels are limited to DR5.

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

The fragment protocol header remains 12 bytes. The configured raw-fragment ceiling is now 230 bytes instead of 32 bytes. Before each frame, the worker calls RadioLib `getMaxPayloadLen()` and automatically clamps the raw fragment size to:

```text
min(configured raw fragment bytes,
    RadioLib maximum application payload - 12-byte BicycleOBU header)
```

This keeps lower configured data rates valid while allowing DR5 to use the full available application payload. In the pinned RadioLib EU868 definition, DR5 permits a 242-byte payload, so the maximum-throughput case is 12 bytes of BicycleOBU header plus 230 bytes of raw C-ITS data.

### Measured baseline before throughput optimization

The previous 32-byte raw fragment setting produced roughly one byte per second of reconstructed C-ITS payload in the physical test. Representative complete frames were:

| Raw C-ITS frame | Old fragments | First -> final fragment | Approx. reconstructed rate |
| ---: | ---: | ---: | ---: |
| 456 B | 15 | ~459.7 s | ~0.99 B/s |
| 314 B | 10 | ~281.6 s | ~1.12 B/s |
| 225 B | 8 | ~241.2 s | ~0.93 B/s |

With a 230-byte raw fragment ceiling at DR5, the same sizes require only:

```text
456 B -> 2 LoRaWAN uplinks
314 B -> 2 LoRaWAN uplinks
225 B -> 1 LoRaWAN uplink
```

This removes most of the fragmentation overhead and substantially reduces the probability that one missing LoRaWAN fragment prevents reconstruction of an entire C-ITS frame.

The firmware does **not** disable EU868 duty-cycle enforcement in order to achieve these numbers. Legal airtime remains the dominant hard limit after the fragment/data-rate optimization.

## Is LoRaWAN suitable for this path?

This optimization is intentionally designed to answer that experimentally. Even at DR5, LoRaWAN remains a low-bandwidth link and cannot losslessly mirror normal ITS-G5 reception rates. A continuous raw V2X feed should therefore not be assumed to be a viable LoRaWAN product architecture.

The useful cases are more likely to be sparse research sampling, selected message forwarding, event-triggered packets, or sending compact derived observations rather than every received raw ITS-G5 frame.

When using the public The Things Network rather than a private LoRaWAN network, its separate fair-use policy must also be respected. The maximum-throughput profile is for controlled evaluation, not permission to bypass network or regulatory airtime limits.

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
git switch main
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

## Throughput test procedure

Do not erase the S3 flash/NVS. A normal firmware reflash preserves the current LoRaWAN session.

1. Build the S3 with the maximum-throughput defaults or select fixed DR5 manually in menuconfig.
2. Keep the Docker bridge running and confirm both MQTT connections.
3. Receive actual ITS-G5 traffic on the C5.
4. Confirm the S3 startup log reports `application rate policy: fixed EU868 DR5` and a current maximum payload near the DR5 value.
5. Confirm a raw frame TX start reports the reduced fragment count and effective raw bytes per fragment.
6. Measure from the first bridge fragment to `published C-ITS frame to OpenTrafficMap`.
7. Compare reconstructed bytes/second and complete-frame success rate with the baseline table above.

## Persistence rule

Routine use:

```powershell
idf.py -C firmware/s3 -p COMx flash monitor
```

Do not use `erase-flash` unless the matching TTS development end-device activation state is deliberately recreated at the same time. DevNonce/session persistence is mandatory for safe OTAA operation.
