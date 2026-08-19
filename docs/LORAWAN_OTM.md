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

The current BicycleOBU diagnostic/reference registration is:

```text
Frequency plan: Europe 863-870 MHz (SF12 for RX2)
LoRaWAN MAC version: 1.1.0
Regional Parameters: RP002 1.0.4
Activation: OTAA
```

The SF12-for-RX2 plan matches RadioLib's EU868 pre-join RX2 default observed in protocol traces: 869.525 MHz / DR0 (SF12/BW125). After a successful join the network may configure other MAC parameters/data rates.

The firmware passes both `NwkKey` and `AppKey` to RadioLib, so it is configured for LoRaWAN 1.1. Do not register this build as a LoRaWAN 1.0.x-only device.

Create/copy four values during device registration:

| Firmware field | The Things Stack field | Length |
|---|---|---:|
| `OBU_LORAWAN_JOIN_EUI` | JoinEUI | 16 hex digits |
| `OBU_LORAWAN_DEV_EUI` | DevEUI | 16 hex digits |
| `OBU_LORAWAN_NWK_KEY` | NwkKey | 32 hex digits |
| `OBU_LORAWAN_APP_KEY` | AppKey | 32 hex digits |

For a programmable prototype with no manufacturer-assigned JoinEUI, an all-zero JoinEUI may be used as long as the same value is programmed in the device. Generate/store root keys securely.

Suggested End Device ID:

```text
bicycleobu-mainboard
```

## 4. Make sure a gateway is available

Before debugging firmware, verify the gateway independently in The Things Stack.

For your own gateway:

1. register it in the **Gateways** section;
2. configure EU868;
3. wait until its status shows connected/recent traffic.

If relying on community TTN coverage, the OTAA join only works where a compatible gateway can hear the Wio-SX1262 and can transmit the JoinAccept downlink. `RadioLib: Uplink sent` only confirms local radio TX completion; it is not a network acknowledgement.

Historical BicycleOBU tests reached public TTN gateways, but current public-gateway availability/link budget must be re-established whenever TTS Live Data is empty.

## 5. Configure and flash the ESP32-S3

Use ESP-IDF 6.1 to match CI exactly. A local 6.0.2 build has also compiled, but its Kconfig notes/toolchain differ from the validated CI environment.

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

- Activation and network
- Payload and buffering
- Airtime and retry scheduling
- Diagnostics

After changing the repository's RadioLib pin, update the local Component Manager lock before physical testing:

```powershell
idf.py -C firmware/s3 update-dependencies
idf.py -C firmware/s3 reconfigure
```

The current intended RadioLib dependency is:

```text
12e3ed6c4814e177a87a7b2c48ab11dd65788143
```

A build intended for the OTAA investigation must explicitly print that commit. Do not assume the manifest value automatically replaced an older `dependencies.lock` selection.

Then build/flash normally:

```powershell
idf.py -C firmware/s3 build
idf.py -C firmware/s3 -p COMx flash monitor
```

Do not use `erase-flash` during ordinary updates because the NVS partition stores LoRaWAN nonce/session persistence.

## 6. OTAA diagnostics

For protocol-level testing enable:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
     -> Diagnostics
```

Recommended first capture:

```text
[x] structured LoRaWAN ESP_LOG diagnostics
[x] RadioLib BASIC + PROTOCOL trace
[ ] RadioLib full SPI trace
```

Use full SPI trace only after the network proves that a JoinAccept was transmitted but the device still misses it.

If TTS Live Data is completely empty while the local trace says `Uplink sent`, first verify:

1. JoinEUI/DevEUI exactly match the current TTS end device;
2. the firmware is transmitting EU868 default channels;
3. antenna/IPEX connection is sound;
4. a currently active gateway is in range.

Do not diagnose RX1/RX2 from `-1116` until the same JoinRequest is visible in TTS.

## 7. Persistence/reset rule

Ordinary `flash` preserves NVS and therefore RadioLib's DevNonce/session state.

If activation credentials or EUIs are changed, RadioLib may reject the saved persistence state with `RADIOLIB_ERR_NONCES_DISCARDED (-1119)`. This is intentional fail-closed behavior.

Only use a local NVS/flash erase when the matching development end-device state in TTS is deliberately reprovisioned/synchronized as well. Then stop erasing and preserve nonce/session state for subsequent joins.

## 8. Server bridge

After successful OTAA and application uplinks, the TTS MQTT bridge in `tools/lorawan_otm_bridge/` subscribes to application uplinks, reassembles BicycleOBU raw-frame fragments, validates length/CRC and publishes the exact reconstructed C-ITS frame bytes to OpenTrafficMap MQTT.

See also:

- `docs/LORAWAN_OTAA_DEBUG.md`
- `docs/evidence/` for dated physical traces
