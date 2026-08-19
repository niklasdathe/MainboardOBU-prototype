# LoRaWAN uplink to OpenTrafficMap

This document describes the BicycleOBU prototype path from the S3/Wio-SX1262 through The Things Network / The Things Stack to the server-side OpenTrafficMap bridge.

For the running OTAA failure investigation and dated hardware evidence, see [`LORAWAN_OTAA_DEBUG.md`](LORAWAN_OTAA_DEBUG.md) and [`evidence/`](evidence/).

## Architecture

```text
C5 ITS-G5 receiver
        |
        | raw C-ITS frame over S3<->C5 SPI IPC
        v
ESP32-S3
        |
        | bounded/versioned binary fragments
        v
Wio-SX1262 / EU868 LoRaWAN
        |
        v
LoRaWAN gateway
        |
        v
The Things Stack
        |
        | application MQTT uplinks
        v
lorawan_otm_bridge
        |
        | reconstructed original binary frame
        v
OpenTrafficMap MQTT
```

The bridge publishes the exact reconstructed raw C-ITS frame bytes. It does not convert the ITS packet to JSON.

## 1. Checkout and toolchain

Use the feature branch while the LoRaWAN implementation is still under physical validation:

```powershell
git fetch origin
git switch agent/lorawan-otm-uplink
git pull --ff-only
git status
git rev-parse HEAD
```

Use ESP-IDF 6.1 for reference builds because CI uses 6.1. ESP-IDF 6.0.2 has also compiled locally, but physical comparison results should note the toolchain explicitly.

Do not commit real LoRaWAN root keys in `sdkconfig.defaults`, shell scripts or Git history.

## 2. Create a The Things Stack application

For Germany/EU868 the reference setup uses The Things Network / The Things Stack Sandbox EU1 cluster.

Create an application, for example:

```text
Application ID: bicycleobu
Name: BicycleOBU
```

For the TTN tenant the MQTT application UID/username is normally:

```text
bicycleobu@ttn
```

Use the actual Application ID if it differs.

## 3. Register the end device

Use the manual end-device registration path.

The current BicycleOBU diagnostic/reference profile is:

```text
Frequency plan: Europe 863-870 MHz (SF12 for RX2)
LoRaWAN MAC version: 1.1.0
Regional Parameters: RP002 1.0.4
Activation: OTAA
```

The SF12-for-RX2 plan matches RadioLib's observed EU868 pre-join RX2 default:

```text
869.525 MHz / DR0 / SF12 / BW125
```

The firmware uses both `NwkKey` and `AppKey`, so register it as LoRaWAN 1.1 rather than a LoRaWAN 1.0.x-only device.

Map the fields as follows:

| Firmware field | TTS field | Length |
|---|---|---:|
| `OBU_LORAWAN_JOIN_EUI` | JoinEUI | 16 hex digits |
| `OBU_LORAWAN_DEV_EUI` | DevEUI | 16 hex digits |
| `OBU_LORAWAN_NWK_KEY` | NwkKey | 32 hex digits |
| `OBU_LORAWAN_APP_KEY` | AppKey | 32 hex digits |

For this programmable prototype, an all-zero JoinEUI is used as long as the exact same value is configured on both sides.

Suggested End Device ID:

```text
bicycleobu-mainboard
```

Root keys are secrets. Do not copy them into issue/PR text, committed config or debug logs.

## 4. Gateway requirement

A real EU868 LoRaWAN gateway must be in range. Public TTN community coverage can work, but it is not a deterministic test setup.

`RLB_PRO: Uplink sent` means only that the local SX1262 completed the transmit operation. It is **not** acknowledgement that a gateway heard the frame.

When TTS Live Data is completely empty during a local JoinRequest, verify in this order:

1. JoinEUI/DevEUI match the current registered device exactly;
2. the JoinRequest uses EU868 default channels;
3. the antenna/IPEX connection is secure and the antenna is suitable for 868 MHz;
4. an active gateway is actually in range;
5. for controlled debugging, move close to a known gateway or use a local multi-channel EU868 gateway.

Historical BicycleOBU tests did reach public TTN gateways, but current gateway availability must be proven again whenever Live Data is empty.

## 5. Configure the S3

Run:

```powershell
idf.py -C firmware/s3 set-target esp32s3
idf.py -C firmware/s3 menuconfig
```

Open:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
```

Settings are grouped into:

```text
Activation and network
Payload and buffering
Airtime and retry scheduling
Diagnostics
```

The Wio-SX1262 hardware profile is:

```text
SCK    GPIO7
MISO   GPIO8
MOSI   GPIO9
NSS    GPIO41
DIO1   GPIO39
RESET  GPIO42
BUSY   GPIO40
RXEN   GPIO38
TXEN   NC
DIO2   internal RF switch control
DIO3   TCXO at 1.8 V
```

With the Wio fitted, GNSS PPS must not use GPIO41. The prototype moves PPS to GPIO47.

## 6. RadioLib dependency

The current manifest requests RadioLib commit:

```text
12e3ed6c4814e177a87a7b2c48ab11dd65788143
```

This is the merged state of RadioLib PR #1811, which contains the SX126x LoRaWAN receive-window symbol-timeout work used in the OTAA investigation.

ESP-IDF Component Manager may keep an older exact selection in `firmware/s3/dependencies.lock`. After changing the manifest pin, run:

```powershell
idf.py -C firmware/s3 update-dependencies
idf.py -C firmware/s3 reconfigure
```

A physical test intended to use the merge commit must explicitly show:

```text
RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
```

The native merged ESP-IDF HAL also reports:

```text
Platform: "ESP-IDF"
```

Do not infer the selected RadioLib source from the manifest alone.

## 7. Build and flash

Build:

```powershell
idf.py -C firmware/s3 build
```

Flash and monitor:

```powershell
idf.py -C firmware/s3 -p COMx flash monitor
```

Replace `COMx` with the current Windows port.

### Do not erase NVS during normal updates

The LoRaWAN component persists RadioLib nonce/session buffers in the default NVS partition under namespace:

```text
obu_lwan
```

Ordinary `flash` preserves this state. `erase-flash` destroys it.

If JoinEUI/DevEUI/root keys are intentionally changed, RadioLib may reject the old buffer with:

```text
RADIOLIB_ERR_NONCES_DISCARDED (-1119)
```

That is intentional fail-closed behavior. Only erase local state when the matching development end-device state in TTS is deliberately reprovisioned/synchronized as well.

## 8. OTAA diagnostic mode

Navigate to:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
     -> Diagnostics
```

Recommended first diagnostic capture:

```text
[x] Enable structured LoRaWAN ESP_LOG diagnostics
[x] Enable RadioLib basic + protocol trace
[ ] Enable RadioLib full SPI trace
```

The structured log includes radio control-line snapshots, activation duration, duty-cycle timing, persistence state and human-readable RadioLib errors. Root keys are never intentionally logged.

Use full SPI trace only for one short attempt after TTS proves a JoinAccept was actually transmitted and the device still misses it.

For `-1116`, do not assume an RX failure until the same JoinRequest appears in TTS. The useful server-side sequence is:

```text
ns.up.join.process
js.join.accept
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

## 9. Persistent activation acceptance test

After a successful OTAA join/application uplink, fully power-cycle the board without reflashing.

Expected persistence startup is approximately:

```text
LoRaWAN NVS ready: nonce_state=present session_state=present
RadioLib persistence attached: nonce_state=restored session_restored=yes
```

The next application uplink must continue with valid session/frame-counter state. A reset must not cause unsafe nonce/counter reuse.

## 10. TTS MQTT integration

Create an application API key that can read application traffic and configure the bridge with the TTS MQTT details shown by the Console.

Typical EU1 values are:

```text
TTS_MQTT_HOST=eu1.cloud.thethings.network
TTS_MQTT_USERNAME=<application-id>@ttn
TTS_APPLICATION_UID=<application-id>@ttn
LORAWAN_DEVICE_ID=bicycleobu-mainboard
LORAWAN_FRAME_FPORT=10
```

`TTS_MQTT_PASSWORD` is the generated TTS API key and must be treated as a secret.

The bridge subscribes to:

```text
v3/<TTS_APPLICATION_UID>/devices/+/up
```

It base64-decodes `uplink_message.frm_payload`, checks FPort, reassembles BicycleOBU fragments by device/frame sequence, verifies original length and CRC16, then publishes the reconstructed raw C-ITS frame.

## 11. Run the OpenTrafficMap bridge

From the repository root on Windows:

```powershell
cd tools\lorawan_otm_bridge
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

Set the environment:

```powershell
$env:TTS_MQTT_HOST = 'eu1.cloud.thethings.network'
$env:TTS_MQTT_USERNAME = '<application-id>@ttn'
$env:TTS_MQTT_PASSWORD = '<generated TTS API key>'
$env:TTS_APPLICATION_UID = '<application-id>@ttn'
$env:LORAWAN_DEVICE_ID = 'bicycleobu-mainboard'
$env:LORAWAN_FRAME_FPORT = '10'
$env:OTM_NODE_ID = '<OpenTrafficMap node id>'
```

Then run:

```powershell
python bridge.py
```

OpenTrafficMap defaults used by the bridge are the project-documented TLS MQTT endpoint and `its/<node>/packet`/status topic structure. Reuse an existing stable OTM node ID when appropriate rather than creating a new identity on every run.

One raw C-ITS frame can require multiple LoRaWAN application uplinks, so one TTS uplink does not necessarily produce one OpenTrafficMap publish.

## 12. Physical end-to-end close criteria

The LoRaWAN path is not considered complete until physical hardware demonstrates all of the following:

1. TTS receives the JoinRequest from the current registered identity.
2. A JoinAccept is scheduled/transmitted by a gateway.
3. RadioLib reports a new/restored valid session.
4. The S3 sends application uplinks.
5. A hard power-cycle restores the session safely.
6. The bridge reconstructs the raw C-ITS frame.
7. OpenTrafficMap receives that frame on the expected MQTT node topic.

The current PR remains draft until those physical checks are complete.
