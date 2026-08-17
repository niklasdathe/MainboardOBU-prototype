# LoRaWAN uplink to OpenTrafficMap

This prototype replaces the stalled direct S3 Wi-Fi/MQTT path with a LoRaWAN transport based on the Seeed XIAO ESP32-S3 + Wio-SX1262 kit.

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

The radio component follows Seeed's XIAO ESP32-S3 + SX1262 board support values.

| Signal | ESP32-S3 GPIO | Notes |
|---|---:|---|
| SPI SCK | 7 | shared SPI2 bus |
| SPI MISO | 8 | shared SPI2 bus |
| SPI MOSI | 9 | shared SPI2 bus |
| SX1262 NSS | 41 | B2B radio chip select |
| SX1262 DIO1 | 39 | radio IRQ |
| SX1262 RESET | 42 | radio reset |
| SX1262 BUSY | 40 | radio busy |
| Wio RF switch | 38 | external antenna switch control |
| GNSS PPS | 47 | moved from GPIO41/D12 |

**Required hardware change:** do not leave the L76K PPS signal on D12/GPIO41 when the Wio-SX1262 is fitted. GPIO41 is electrically connected to SX1262 NSS. Move the PPS carrier to GPIO47. If PPS is not wired on your L76K setup, leave it disconnected rather than connecting it to GPIO41.

The Wio radio shares SCK/MISO/MOSI with the C5 and microSD. Each device has its own chip select. RadioLib's native ESP-IDF HAL attaches the SX1262 as another device on the already-initialized SPI bus.

The Seeed BSP configures the SX1262 TCXO supply for 3.0 V and uses GPIO38 for the board RF switch; `obu_lorawan` mirrors those values.

A LoRaWAN **gateway is required** between the bicycle and The Things Stack. The Wio-SX1262 end device does not provide Internet connectivity by itself. A nearby public TTN gateway is sufficient if coverage is good; for controlled testing use a real multi-channel EU868 gateway. Seeed's single-channel XIAO/Wio gateway example is useful for experiments but is not the recommended field gateway architecture.

## RadioLib pin

The component uses RadioLib from commit:

```text
f0fb029566c2d58a7373bb66d3a48002a5b56876
```

This is intentionally pinned rather than following a moving branch. It contains the SX1262 LoRaWAN Class-A receive-window timing fix used by this ESP-IDF integration. `CONFIG_FREERTOS_HZ=1000` is enabled for the RadioLib ESP-IDF timing implementation.

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

Do **not** casually run `idf.py erase-flash` after this device has joined a LoRaWAN network. That erases the local DevNonce/session history. If you intentionally erase the S3 flash, also reprovision the end device in The Things Stack so the network and device start with a consistent activation state. Development-only server settings that permit nonce resets should not be relied on for normal operation.

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

For a programmable prototype, use EUIs/root keys generated or allocated by The Things Stack rather than inventing arbitrary identifiers. Use the Console's Generate/request functions where available and program the exact same values into the S3.

Suggested End Device ID:

```text
bicycleobu-mainboard
```

Save the four OTAA values securely before leaving the registration flow.

## 4. Make sure a gateway is available

Before debugging firmware, verify the gateway independently in The Things Stack.

For your own gateway:

1. register it in the **Gateways** section;
2. configure the same EU868 frequency plan;
3. wait until its status shows connected/recent traffic.

If relying on community TTN coverage, the OTAA join will only work where a compatible gateway can hear the Wio-SX1262 and forward to the same network.

## 5. Configure and flash the ESP32-S3

Use an ESP-IDF 6.1 environment. On Windows, the easiest option is the ESP-IDF PowerShell/Command Prompt installed by Espressif so `idf.py` and the toolchain are already exported.

From the repository root:

```bash
idf.py -C firmware/s3 fullclean
idf.py -C firmware/s3 set-target esp32s3
idf.py -C firmware/s3 menuconfig
```

Open:

```text
BicycleOBU LoRaWAN uplink
```

Configure:

```text
Enable Wio-SX1262 LoRaWAN uplink = yes
JoinEUI = <your 16 hex digits>
DevEUI = <your 16 hex digits>
NwkKey = <your 32 hex digits>
AppKey = <your 32 hex digits>
Raw-frame fragment FPort = 10
```

Keep the default fragment size/throttling for initial testing.

Build:

```bash
idf.py -C firmware/s3 build
```

Flash and monitor. Replace `COMx` on Windows with the actual serial port; on Linux/macOS use the corresponding `/dev/...` device.

```bash
idf.py -C firmware/s3 -p COMx flash monitor
```

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

## 6. Verify uplinks in The Things Stack before involving OpenTrafficMap

Open the end device's **Live data** page. Trigger an ITS-G5 frame from the C5/C5-TX setup.

You should first see the OTAA join, then application uplinks on FPort 10. The raw application payload is the BicycleOBU fragmentation protocol; do not add a TTS payload formatter for the bridge path.

If no join appears at all, debug RF/gateway coverage before debugging MQTT.

If join requests appear but are rejected, compare JoinEUI, DevEUI, NwkKey, AppKey and LoRaWAN version exactly.

## 7. Create the TTS MQTT credential for the bridge

In the The Things Stack application:

1. open **API Keys**;
2. choose **Add API Key**;
3. give it a name such as `bicycleobu-otm-bridge`;
4. grant the minimum right required to **read application traffic/uplink messages**;
5. create the key;
6. copy it immediately. It begins with a value similar to `NNSXS...` and is only displayed once.

This application API key is the MQTT **password**.

For The Things Network Sandbox, the MQTT values are normally:

```text
Host: eu1.cloud.thethings.network
Port: 8883 (TLS)
Username: <application-id>@ttn
Password: <application API key beginning NNSXS...>
```

The bridge subscribes to:

```text
v3/<application-id>@ttn/devices/+/up
```

## 8. Choose an OpenTrafficMap node ID

OpenTrafficMap does not currently require an MQTT username/password for publishing receiver packets. It identifies the receiver from the node portion of the topic.

Choose a stable unique node ID and keep it unchanged, for example:

```text
bicycleobu-<DevEUI>
```

Set that exact string as `OTM_NODE_ID`. If you want the node associated with a friendly name or shown on the OpenTrafficMap map, use OpenTrafficMap's node-registration/self-service process for that same identifier where applicable.

OpenTrafficMap self-service credentials, when provided, are relevant for managing/subscribing to a node; the bridge does not need them to publish.

## 9. Run the TTS -> OpenTrafficMap bridge

The bridge can run on your development PC, Raspberry Pi, server or other always-on Internet-connected system. It is not intended to run on the bicycle S3.

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

Replace every example value with the values from your own TTS application/end device.

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
2. SX1262 initializes with the expected Seeed pins.
3. TTS Live Data shows an OTAA join.
4. TTS Live Data shows FPort 10 uplinks.
5. Bridge reports connection to TTS MQTT.
6. Bridge receives enough fragments to reconstruct one C-ITS frame.
7. Bridge reports publication to `its/<OTM_NODE_ID>/packet`.
8. OpenTrafficMap/self-service confirms the node data if the node has been registered for viewing.
9. Hard power-cycle the S3.
10. Confirm `session_restored=yes` and send another frame without manually resetting TTS counters/nonces.

Only after step 10 should reset/session persistence be considered hardware-validated.

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

The application adds a 10 s default spacing between fragments and leaves RadioLib duty-cycle enforcement enabled. `timeUntilUplink()` is used to wait for the legal RadioLib transmit window and retry the same fragment instead of treating duty-cycle throttling as packet loss.

Increase sampling selectivity rather than disabling regulatory duty-cycle handling.

The current implementation is fixed to EU868. Do not transmit with this configuration outside a region where EU868 parameters are permitted; add a region configuration before supporting other deployments.

## Known prototype limitations

- CI verifies compilation, pin-plan consistency and bridge reassembly tests; it cannot prove RF output, gateway coverage, OTAA acceptance, persistence across physical power loss or end-to-end OpenTrafficMap receipt.
- Root OTAA credentials are currently configured through ESP-IDF `menuconfig`; they are not provisioned through a secure element.
- NVS persistence uses the ESP-IDF default NVS partition. NVS encryption depends on the broader firmware/flash-security configuration and is not enabled specifically by `obu_lorawan`.
- The uplink currently forwards raw received frames without traffic prioritization. A later collection policy should select message types or sampling rates based on research requirements and available LoRaWAN airtime.
- The direct S3 Wi-Fi/OpenTrafficMap component remains in the repository as a parked development path and is not reused by this transport.

## Tests

```bash
cd tools/lorawan_otm_bridge
python -m unittest discover -s tests -v
python -m py_compile protocol.py bridge.py
```

GitHub Actions builds the S3 once with LoRaWAN disabled and once with `OBU_LORAWAN_ENABLE=y` using deliberately fake credentials, so the NVS-backed enabled code path is compiled without committing real secrets.
