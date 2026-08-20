# LoRaWAN OTAA troubleshooting notes

This document keeps only the conclusions that remain useful for future BicycleOBU LoRaWAN work. Detailed one-off terminal captures were removed after their findings were consolidated here and in the pull request.

## Current working state

Hardware and protocol:

```text
MCU:        Seeed XIAO ESP32-S3
Radio:      Wio-SX1262
Region:     EU868
LoRaWAN:    1.1 OTAA
RadioLib:   12e3ed6c4814e177a87a7b2c48ab11dd65788143
ESP-IDF:    6.0.2
```

Wio-SX1262 wiring/configuration:

```text
SCK=7 MISO=8 MOSI=9 NSS=41
DIO1=39 RESET=42 BUSY=40
RXEN=38 TXEN=NC
DIO2 RF switch enabled
DIO3 TCXO 1.8 V
```

With the Wio fitted, GPIO41 is radio NSS; GNSS PPS is therefore on GPIO47.

The current development identity uses JoinEUI `1111111111111111` and DevEUI `70B3D57ED0078C82`. Root keys are not stored in repository documentation.

Physical testing has now demonstrated:

- TTS receiving and accepting the device JoinRequest;
- DR0/SF12 uplink reception at 868.100 MHz;
- a valid LoRaWAN session being persisted in NVS;
- normal firmware reflash preserving that state;
- RadioLib restoring the session after reboot/reflash and returning `RADIOLIB_LORAWAN_SESSION_RESTORED (-1117)` with `activated=yes`.

A representative restored boot reports:

```text
LoRaWAN NVS ready: nonce_state=present session_state=present
RadioLib persistence attached: nonce_state=restored session_restored=yes
activateOTAA returned state=-1117 (SESSION_RESTORED) activated=yes
LoRaWAN OTAA active
```

This is stronger evidence than the earlier `-1116` captures: OTAA has succeeded at least once and session restoration is functional.

## How gateway selection works

The end device does not select or bind to a gateway. It broadcasts a LoRaWAN uplink; every compatible gateway in range may forward it and the network server deduplicates receptions and chooses an eligible downlink path.

This means BicycleOBU firmware must not contain a preferred gateway ID. Mobility between gateways is normal LoRaWAN operation.

During debugging, one DR0 JoinRequest was received only through a legacy `ttnv2` Packet Broker path at roughly -119 dBm. TTS accepted the JoinRequest, but that exported capture did not contain downlink-scheduling events. Later the device acquired a valid session. The practical lesson is to distinguish gateway/network-path problems from radio RX problems before changing the SX1262 configuration.

## Changes made during the investigation

The following changes are intentional and should not be casually reverted:

1. **RadioLib pin:** use merge commit `12e3ed6c...`, which contains the SX126x LoRaWAN receive-window symbol-timeout fix from RadioLib PR #1811.
2. **Native ESP-IDF HAL:** after changing the manifest pin, `idf.py -C firmware/s3 update-dependencies` was required to replace a stale `dependencies.lock`; the build must report `Platform: "ESP-IDF"`.
3. **Persistent LoRaWAN state:** RadioLib persistence/session buffers are stored in NVS namespace `obu_lwan` and fail closed on corrupt or mismatched activation state.
4. **Duty-cycle retry:** `RADIOLIB_ERR_UPLINK_UNAVAILABLE (-1108)` is a wait condition, not a failed join. Join retries use `timeUntilUplink()`.
5. **Configurable join DR:** menuconfig exposes EU868 DR0..DR5. DR0/SF12 is useful for link-budget diagnostics; normal network ADR remains enabled after activation.
6. **RX2 test profile:** the diagnostic TTS registration uses `Europe 863-870 MHz (SF12 for RX2)`, matching RadioLib's pre-join EU868 RX2 default at 869.525 MHz/DR0.
7. **Diagnostics:** structured ESP_LOG and RadioLib BASIC/PROTOCOL traces are menuconfig-controlled. Full SPI tracing is reserved for short low-level captures.

## NVS rules

Normal updates:

```powershell
idf.py -C firmware/s3 -p COMx flash monitor
```

Do not use `erase-flash` as part of routine development. It destroys DevNonce/session state.

An erase is only appropriate when the corresponding TTS development device is deliberately reprovisioned/reset at the same time. If activation credentials change while old state remains, the component intentionally fails closed instead of silently reusing nonces.

## Interpreting the main RadioLib states

```text
-1108  RADIOLIB_ERR_UPLINK_UNAVAILABLE
       Legal duty-cycle wait; retry later.

-1116  RADIOLIB_ERR_NO_JOIN_ACCEPT
       No valid JoinAccept reached RadioLib. Do not assume device RX failure until
       the server/gateway path is proven.

-1117  RADIOLIB_LORAWAN_SESSION_RESTORED
       Persisted active session successfully restored.

-1119  RADIOLIB_ERR_NONCES_DISCARDED
       Saved activation state does not match the current JoinEUI/DevEUI/root keys.
```

## If OTAA fails again

Check in this order:

1. Build resolves RadioLib `12e3ed6c...` and reports `Platform: "ESP-IDF"`.
2. JoinEUI, DevEUI, AppKey and NwkKey match the current TTS device.
3. NVS is healthy and has not been casually erased.
4. The JoinRequest appears in TTS Live Data.
5. At least one current EU868 gateway has adequate link margin.
6. For a server-accepted join that still returns `-1116`, inspect the same correlation path for Network/Gateway Server downlink scheduling before changing radio timing.
7. Only after a gateway-transmitted JoinAccept is proven should one full RadioLib SPI capture and GPIO38/RXEN transition measurement be taken.

The normal next development focus is no longer OTAA: it is the application path described in [`LORAWAN_OTM.md`](LORAWAN_OTM.md), from received C5 frames through LoRaWAN application uplinks and the server-side bridge to OpenTrafficMap MQTT.
