# LoRaWAN uplink to OpenTrafficMap

This prototype replaces the parked direct S3 Wi-Fi/MQTT path with a LoRaWAN transport based on the Seeed XIAO ESP32-S3 + Wio-SX1262 kit.

For the active OTAA / JoinAccept investigation, including every attempted fix and the exact next diagnostic capture, see [`LORAWAN_OTAA_DEBUG.md`](LORAWAN_OTAA_DEBUG.md).

## Architecture

```text
ITS-G5 air
  -> ESP32-C5 raw RX frame
  -> SPI2 to ESP32-S3
  -> bounded obu_lorawan queue
  -> Wio-SX1262 / LoRaWAN EU868
  -> LoRaWAN gateway
  -> The Things Stack / The Things Network
  -> tools/lorawan_otm_bridge
  -> TLS MQTT
  -> cits1.opentrafficmap.org
     its/<OTM_NODE_ID>/packet
```

OpenTrafficMap still receives the original raw C-ITS frame bytes. LoRaWAN is only the constrained transport between the bicycle and a server with Internet access; it is not MQTT carried directly by the end device.

## Hardware

The radio component follows the known XIAO ESP32-S3 + Wio-SX1262 board configuration used by current Meshtastic support and cross-checked against Seeed's module documentation.

| Signal | ESP32-S3 GPIO | Notes |
|---|---:|---|
| SPI SCK | 7 | shared SPI2 bus |
| SPI MISO | 8 | shared SPI2 bus |
| SPI MOSI | 9 | shared SPI2 bus |
| SX1262 NSS | 41 | B2B radio chip select |
| SX1262 DIO1 | 39 | radio IRQ |
| SX1262 RESET | 42 | radio reset |
| SX1262 BUSY | 40 | radio busy |
| Wio RX enable | 38 | external RF switch RXEN; TXEN is NC |
| SX1262 DIO2 | internal | module TX/RX RF-switch control |
| SX1262 DIO3 | internal | TCXO control at 1.8 V |
| GNSS PPS | 47 | moved from GPIO41/D12 |

**Required hardware change:** do not leave the L76K PPS signal on D12/GPIO41 when the Wio-SX1262 is fitted. GPIO41 is electrically connected to SX1262 NSS. Move the PPS carrier to GPIO47. If PPS is not wired on your L76K setup, leave it disconnected rather than connecting it to GPIO41.

The Wio radio shares SCK/MISO/MOSI with the C5 and microSD. Each device has its own chip select. RadioLib's native ESP-IDF HAL attaches the SX1262 as another device on the already-initialized SPI bus.

The module uses two RF-control mechanisms which must not be confused:

- SX1262 DIO2 controls the module's internal TX/RX switch;
- GPIO38 is the external RX-enable line and is configured as `RXEN` with MCU `TXEN = NC`.

`obu_lorawan` uses a 1.8 V DIO3 TCXO setting. The previous prototype value was 3.0 V; that was inside the Seeed module's documented TCXO range, but 1.8 V matches the known-working XIAO ESP32-S3/Wio-SX1262 board definition and removes a board-specific difference during the JoinAccept investigation.

A LoRaWAN **gateway is required** between the bicycle and The Things Stack. The Wio-SX1262 end device does not provide Internet connectivity by itself. A nearby public TTN gateway is sufficient if coverage is good; for controlled testing use a real multi-channel EU868 gateway. Seeed's single-channel XIAO/Wio gateway example is useful for experiments but is not the recommended field gateway architecture.

## RadioLib pin

The component intentionally pins the merged RadioLib receive-window fix state:

```text
12e3ed6c4814e177a87a7b2c48ab11dd65788143
```

This is the merge commit for RadioLib PR #1811, which addresses SX126x LoRaWAN receive-window timing/symbol-timeout behavior and was tested by its author on ESP32 + SX1262 + TTN. The merged state also carries the native ESP-IDF HAL timing changes used here. `CONFIG_FREERTOS_HZ=1000` remains enabled for deterministic timing.

Do not replace this pin with the former PR-head commit `f0fb029566c2d58a7373bb66d3a48002a5b56876` merely because it was the head of #1811; the project now intentionally uses the merged state.

## Persistent LoRaWAN state

LoRaWAN state persistence is mandatory when this uplink is enabled. `obu_lorawan_persistence` uses the default ESP-IDF NVS partition and the private namespace:

```text
obu_lwan
```

Two RadioLib-owned blobs are stored:

| NVS key | Contents | Durability reason |
|---|---|---|
| `persist` | RadioLib persistence/nonces buffer | DevNonce, JoinNonce and other activation state must survive reset/power loss |
| `session` | RadioLib session buffer | active session keys, frame counters and MAC/session state |

The component does not reinterpret these buffers. It registers RadioLib's persistence callbacks and stores exactly the serialized buffers provided by the pinned RadioLib version.

The `persist` blob is committed when RadioLib advances activation state, including after DevNonce is incremented for a transmitted JoinRequest. The session blob is committed as RadioLib updates the active session and frame counters. Duplicate unchanged buffers are not rewritten.

Persistence is **fail closed**:

- LoRaWAN startup fails if NVS cannot be initialized/opened.
- A session blob without its nonce/persistence blob is treated as unsafe corruption.
- A saved RadioLib state rejected because of version/signature/credential mismatch is not silently discarded.
- If an NVS write or commit fails while operating, further application LoRaWAN transmissions are blocked.

Normal `idf.py flash` updates do not intentionally erase the NVS partition, so LoRaWAN state survives normal firmware reflashing.

### Important erase/reprovision rule

Do **not** casually run `idf.py erase-flash` after this device has joined or while OTAA nonce testing is in progress. That erases the local DevNonce/session history. If you intentionally erase the S3 flash, also reprovision/reset the corresponding development activation state in The Things Stack so network and device start consistently.

## 1. Check out the integration branch

Fresh clone:

```bash
git clone https://github.com/niklasdathe/MainboardOBU-prototype.git
cd MainboardOBU-prototype
git fetch origin
git switch --track origin/agent/lorawan-otm-uplink
git pull --ff-only
git status
git rev-parse HEAD
```

If the repository already exists locally:

```bash
cd MainboardOBU-prototype
git fetch origin
git switch agent/lorawan-otm-uplink 2>/dev/null || git switch --track origin/agent/lorawan-otm-uplink
git pull --ff-only
git status
git rev-parse HEAD
```

Do not put real LoRaWAN root keys in a committed `sdkconfig.defaults`, shell script or Git commit.

## 2. Create a The Things Stack application

For Germany/EU868 the reference setup uses The Things Network / The Things Stack Sandbox EU1 cluster.

1. Sign in to The Things Network / The Things Stack Console.
2. Open **Applications**.
3. Create an application, for example:

```text
Application ID: bicycleobu
Name: BicycleOBU
```

For The Things Network tenant, the MQTT application UID/username will normally be:

```text
bicycleobu@ttn
```

Use your actual Application ID instead of `bicycleobu`.

## 3. Register the Wio-SX1262 end device

Within the application choose **Register end device** and use the manual device-registration path.

Use these protocol settings:

```text
Frequency plan: Europe 863-870 MHz (EU868)
LoRaWAN MAC version: 1.1.0
Regional Parameters / PHY version: 1.1.0-b / RP001 1.1 Rev B
Activation: OTAA
```

The firmware passes both `NwkKey` and `AppKey` to RadioLib, so it is configured for LoRaWAN 1.1. Do not register this build as a LoRaWAN 1.0.x-only device.

Create/copy four values during device registration:

| Firmware field | The Things Stack field | Length |
|---|---|---:|
| `OBU_LORAWAN_JOIN_EUI` | JoinEUI | 16 hex digits |
| `OBU_LORAWAN_DEV_EUI` | DevEUI | 16 hex digits |
| `OBU_LORAWAN_NWK_KEY` | NwkKey | 32 hex digits |
| `OBU_LORAWAN_APP_KEY` | AppKey | 32 hex digits |

For a programmable prototype with no manufacturer-assigned JoinEUI, all-zero JoinEUI is allowed by The Things Stack documentation as long as the same value is programmed in the device. Generate and store the root keys securely.

Suggested End Device ID:

```text
bicycleobu-mainboard
```

## 4. Make sure a gateway is available

Before debugging firmware, verify the gateway independently in The Things Stack.

For your own gateway:

1. register it in the **Gateways** section;
2. configure the same EU868 frequency plan;
3. wait until its status shows connected/recent traffic.

If relying on community TTN coverage, the OTAA join will only work where a compatible gateway can hear the Wio-SX1262 and can also transmit the JoinAccept downlink.

## 5. Configure and flash the ESP32-S3

Use ESP-IDF 6.1 to match CI exactly. A local 6.0.2 build has also compiled, but its Kconfig notes and toolchain differ from the validated CI environment.

From the repository root:

```bash
idf.py -C firmware/s3 set-target esp32s3
idf.py -C firmware/s3 menuconfig
```

Open:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
```

The LoRaWAN settings are grouped into:

```text
Activation and network
Payload and buffering
Airtime and retry scheduling
Diagnostics
```

Initial deployment settings:

```text
Enable Wio-SX1262 LoRaWAN uplink = yes
Activation and network -> JoinEUI = <your 16 hex digits>
Activation and network -> DevEUI = <your 16 hex digits>
Activation and network -> NwkKey = <your 32 hex digits>
Activation and network -> AppKey = <your 32 hex digits>
Payload and buffering -> Raw-frame fragment FPort = 10
```

Keep the default payload/throttling values for initial testing.

Build and flash:

```bash
idf.py -C firmware/s3 build
idf.py -C firmware/s3 -p COMx flash monitor
```

Do not use `erase-flash` as part of an ordinary firmware update.

On a **fresh** NVS state, expected log progression includes messages similar to:

```text
LoRaWAN NVS ready: nonce_state=fresh session_state=fresh
RadioLib persistence attached: nonce_state=new session_restored=no
Wio-SX1262 ready ...
Joining LoRaWAN network using OTAA
LoRaWAN OTAA active ...
```

After a successful join/uplink, power-cycle the S3. Expected persistence evidence becomes:

```text
LoRaWAN NVS ready: nonce_state=present session_state=present
RadioLib persistence attached: nonce_state=restored session_restored=yes
Wio-SX1262 ready ... restored_session=yes
```

A restored active session should not require a new OTAA join solely because the S3 was reset.

### Diagnostic mode

For an OTAA/downlink investigation open:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
     -> Diagnostics
```

First use:

```text
[x] Enable structured LoRaWAN ESP_LOG diagnostics
[x] Enable RadioLib basic + protocol trace (RLB_DBG/RLB_PRO)
[ ] Enable RadioLib full SPI trace (RLB_SPI, extremely verbose)
```

The structured diagnostics intentionally do not print root keys. RadioLib protocol tracing is a compile-time option and is enabled in the dedicated LoRaWAN CI build so that this diagnostic path cannot silently rot. Only enable full SPI tracing for a short capture when protocol tracing is insufficient.

See [`LORAWAN_OTAA_DEBUG.md`](LORAWAN_OTAA_DEBUG.md) for the expected capture and decision tree.

## 6. Verify OTAA and uplinks in The Things Stack before involving OpenTrafficMap

Open the end device's **Live data** page.

A server-side `js.join.accept` or `as.up.join.forward` proves the Join Server accepted the JoinRequest and created session material; it does **not by itself** prove that a gateway transmitted the JoinAccept over RF.

For a stuck join, correlate the same uplink correlation ID with:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail

gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

The Things Stack documents `ns.down.join.schedule.success` as successful scheduling of the JoinAccept on the Gateway Server. Its troubleshooting guide additionally recommends checking Gateway Server/gateway transmit events when a device is stuck in a join loop.

Once OTAA succeeds, trigger an ITS-G5 frame from the C5/C5-TX setup. Application uplinks should use FPort 10. The raw application payload is the BicycleOBU fragmentation protocol; do not add a TTS payload formatter for the bridge path.

## 7. Create the TTS MQTT credential for the bridge

In the The Things Stack application:

1. open **API Keys**;
2. choose **Add API Key**;
3. give it a name such as `bicycleobu-otm-bridge`;
4. grant the minimum right required to read application traffic/uplink messages;
5. create the key;
6. copy it immediately.

For The Things Network Sandbox, the MQTT values are normally:

```text
Host: eu1.cloud.thethings.network
Port: 8883 (TLS)
Username: <application-id>@ttn
Password: <application API key>
```

The bridge subscribes to:

```text
v3/<application-id>@ttn/devices/+/up
```

## 8. Choose an OpenTrafficMap node ID

Choose a stable unique node ID and keep it unchanged, for example:

```text
bicycleobu-<DevEUI>
```

Set that exact string as `OTM_NODE_ID`. If you want the node associated with a friendly name or shown on the OpenTrafficMap map, use OpenTrafficMap's node-registration/self-service process for the same identifier where applicable.

## 9. Run the TTS -> OpenTrafficMap bridge

The bridge can run on a development PC, Raspberry Pi, server or other always-on Internet-connected system. It is not intended to run on the bicycle S3.

Linux/macOS shell:

```bash
cd tools/lorawan_otm_bridge
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt

export TTS_MQTT_HOST='eu1.cloud.thethings.network'
export TTS_MQTT_USERNAME='bicycleobu@ttn'
export TTS_MQTT_PASSWORD='NNSXS.REPLACE_WITH_YOUR_APPLICATION_API_KEY'
export TTS_APPLICATION_UID='bicycleobu@ttn'
export LORAWAN_DEVICE_ID='bicycleobu-mainboard'
export OTM_NODE_ID='bicycleobu-REPLACE_WITH_YOUR_DEVEUI'
export LORAWAN_FRAME_FPORT='10'

python bridge.py
```

Windows PowerShell:

```powershell
cd tools/lorawan_otm_bridge
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt

$env:TTS_MQTT_HOST = 'eu1.cloud.thethings.network'
$env:TTS_MQTT_USERNAME = 'bicycleobu@ttn'
$env:TTS_MQTT_PASSWORD = 'NNSXS.REPLACE_WITH_YOUR_APPLICATION_API_KEY'
$env:TTS_APPLICATION_UID = 'bicycleobu@ttn'
$env:LORAWAN_DEVICE_ID = 'bicycleobu-mainboard'
$env:OTM_NODE_ID = 'bicycleobu-REPLACE_WITH_YOUR_DEVEUI'
$env:LORAWAN_FRAME_FPORT = '10'

python bridge.py
```

Optional variables:

| Variable | Default | Purpose |
|---|---|---|
| `TTS_MQTT_PORT` | `8883` | TTS MQTT TLS port |
| `OTM_MQTT_HOST` | `cits1.opentrafficmap.org` | OTM broker |
| `OTM_MQTT_PORT` | `8883` | OTM MQTT TLS port |
| `LORAWAN_FRAME_FPORT` | `10` | fragment FPort |
| `LORAWAN_DEVICE_ID` | unset | accept only one LoRaWAN device |
| `LORAWAN_REASSEMBLY_TIMEOUT_S` | `600` | incomplete-frame expiry |

Expected bridge logs progress through:

```text
connected to OpenTrafficMap MQTT ...
connected to The Things Stack MQTT; subscribing to v3/.../devices/+/up
published reassembled C-ITS frame: device=... bytes=... topic=its/.../packet
```

## 10. End-to-end acceptance test

Use this order so failures are isolated:

1. S3 boots and reports NVS healthy.
2. SX1262 initializes with the expected Wio pins, GPIO38 RXEN and 1.8 V TCXO.
3. TTS Live Data shows a JoinRequest.
4. TTS shows the JoinAccept scheduled/transmitted by the gateway.
5. S3 reports a new/restored active LoRaWAN session.
6. TTS Live Data shows FPort 10 application uplinks.
7. Bridge connects to TTS MQTT and reconstructs one complete C-ITS frame.
8. Bridge publishes that frame to `its/<OTM_NODE_ID>/packet`.
9. Hard power-cycle the S3.
10. Confirm `session_restored=yes` and send another frame without resetting network counters/nonces.

Only after step 10 should reset/session persistence and physical end-to-end operation be considered hardware-validated.

## Reprovisioning after an intentional flash erase

If the device has **never joined**, `idf.py erase-flash` is harmless apart from removing firmware/configuration.

If it **has joined**, use this deliberate recovery sequence:

1. stop/power off the S3;
2. record the old device ID for traceability;
3. delete/reprovision the TTS end device or explicitly perform the appropriate development nonce-reset procedure in TTS;
4. run `idf.py -C firmware/s3 erase-flash`;
5. recreate/generate a consistent JoinEUI, DevEUI, NwkKey and AppKey in TTS;
6. enter those exact values in S3 `menuconfig`;
7. rebuild and flash;
8. verify a fresh OTAA join and new NVS state.

Do not erase only the local state and then assume the old network activation state will accept a reused DevNonce.

## Fragment protocol

LoRaWAN application payloads are intentionally small. A raw C-ITS frame is split into fragments with a 12-byte header:

| Byte(s) | Field |
|---|---|
| 0..1 | ASCII `BO` magic |
| 2 | protocol version (`1`) |
| 3 | flags (`0`) |
| 4..5 | frame sequence, big endian |
| 6 | fragment index |
| 7 | fragment count |
| 8..9 | original frame length, big endian |
| 10..11 | CRC16-CCITT of the complete original frame |
| 12.. | raw frame fragment |

The default carries 32 raw bytes per uplink, giving a 44-byte application payload. The bridge accepts out-of-order fragments, ignores identical duplicates, rejects conflicting duplicates, verifies final length and CRC, and publishes only a complete valid frame.

## Throughput and behavior under load

LoRaWAN is much lower bandwidth than ITS-G5. This implementation is therefore a **best-effort collection uplink**, not a lossless mirror of every received 802.11p frame. The S3 never blocks C5 acquisition waiting for LoRa airtime. Its queue is bounded; when it fills, the oldest pending raw frame is discarded so newer traffic can still be sampled.

The application adds a 10 s default spacing between fragments and leaves RadioLib duty-cycle enforcement enabled. `timeUntilUplink()` is used for both OTAA and application uplinks. `RADIOLIB_ERR_UPLINK_UNAVAILABLE` is treated as a legal scheduling wait, not as an OTAA failure.

Increase sampling selectivity rather than disabling regulatory duty-cycle handling.

The current implementation is fixed to EU868. Do not transmit with this configuration outside a region where EU868 parameters are permitted; add a region configuration before supporting other deployments.

## Known prototype limitations

- Physical OTAA is currently under active investigation because repeated tests have reached TTS Join Server acceptance while the device still reports `RADIOLIB_ERR_NO_JOIN_ACCEPT (-1116)`. See `LORAWAN_OTAA_DEBUG.md` rather than repeating old experiments.
- The latest captured TTS export proves Join Server acceptance but did not include the Network/Gateway Server JoinAccept scheduling/transmit events required to prove the RF downlink path.
- CI verifies compilation, pin-plan consistency and bridge reassembly tests; it cannot prove RF output, gateway coverage, downlink reception, persistence across physical power loss or end-to-end OpenTrafficMap receipt.
- Root OTAA credentials are configured through ESP-IDF `menuconfig`; they are not provisioned through a secure element.
- NVS persistence uses the ESP-IDF default NVS partition. NVS encryption depends on the broader firmware/flash-security configuration and is not enabled specifically by `obu_lorawan`.
- The uplink currently forwards raw received frames without traffic prioritization. A later collection policy should select message types or sampling rates based on research requirements and available LoRaWAN airtime.
- The direct S3 Wi-Fi/OpenTrafficMap component remains in the repository as a parked development path and is not reused by this transport.

## Tests

```bash
cd tools/lorawan_otm_bridge
python -m unittest discover -s tests -v
python -m py_compile protocol.py bridge.py
```

GitHub Actions builds the S3 once with LoRaWAN disabled and once with `OBU_LORAWAN_ENABLE=y` using deliberately fake credentials. The LoRaWAN CI configuration also enables structured diagnostics and RadioLib BASIC/PROTOCOL trace at compile time so the diagnostic build path is continuously checked. Full SPI tracing remains disabled in CI.

## Primary references

- RadioLib issue #1806: https://github.com/jgromes/RadioLib/issues/1806
- RadioLib PR #1811: https://github.com/jgromes/RadioLib/pull/1811
- RadioLib debug options: https://github.com/jgromes/RadioLib/blob/master/src/BuildOpt.h
- Meshtastic XIAO ESP32-S3/Wio-SX1262 board definition: https://github.com/meshtastic/firmware/blob/develop/variants/esp32s3/seeed_xiao_s3/variant.h
- Seeed Wio-SX1262 module datasheet: https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf
- The Things Stack Events API: https://www.thethingsindustries.com/docs/api/reference/grpc/events/
- The Things Stack device troubleshooting: https://www.thethingsindustries.com/docs/hardware/devices/troubleshooting/
