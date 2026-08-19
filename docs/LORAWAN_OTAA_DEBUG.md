# LoRaWAN OTAA / JoinAccept investigation log

This document is the running engineering record for the BicycleOBU Wio-SX1262 OTAA problem. Keep observations, attempted changes, evidence and next tests here so the same experiments are not repeated without purpose.

The currently observed device-side failure is:

```text
obu_lorawan: LoRaWAN OTAA join failed: RadioLib state=-1116
```

RadioLib defines `-1116` as `RADIOLIB_ERR_NO_JOIN_ACCEPT`: `activateOTAA()` did not finish with a valid decoded JoinAccept. This code by itself does **not** prove that The Things Stack received the JoinRequest or transmitted a JoinAccept.

## Current evidence

### Device / radio

- MCU: Seeed XIAO ESP32-S3.
- Radio: Seeed Wio-SX1262 on the XIAO B2B connector.
- Region: EU868.
- Activation: OTAA, LoRaWAN 1.1 with NwkKey and AppKey.
- SX1262 initialization succeeds.
- Historical attempts were received and accepted by public TTN gateways/TTS.
- NVS nonce/session persistence is implemented and fail-closed.
- Normal flash updates preserve NVS; `erase-flash` must not be used casually during nonce/session testing.

### Important correction from latest local build/log review

Two facts were missed in the first interpretation of the 2026-08-19 clean trace.

#### A. Local RadioLib dependency was still locked to the old PR-head commit

The repository manifest requests:

```text
12e3ed6c4814e177a87a7b2c48ab11dd65788143
```

but the local build explicitly reported:

```text
Following dependencies have new versions available:
Dependency "RadioLib": "f0fb029566c2d58a7373bb66d3a48002a5b56876"
                    -> "12e3ed6c4814e177a87a7b2c48ab11dd65788143"
Consider running "idf.py update-dependencies" to update your lock file.

Processing ... RadioLib (f0fb029566c2d58a7373bb66d3a48002a5b56876)
```

Therefore the physical captures to date were still built from PR-head `f0fb...`, not from the intended merged state `12e3...`. The six-symbol RX-window behavior was already present on that PR head, so the traces are still useful, but no capture may be described as a merged-state test until the local dependency lock is updated and the build explicitly resolves `12e3...`.

Required local command after changing the manifest pin:

```powershell
idf.py -C firmware/s3 update-dependencies
```

A subsequent build must print:

```text
Processing ... RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
```

#### B. Latest on-air DevEUI differs from the earlier TTS/menuconfig capture

The latest RadioLib JoinRequest trace contains:

```text
00 00 00 00 00 00 00 00 00 76 8c 07 d0 7e d5 b3 70 ...
```

After MHDR + all-zero JoinEUI, LoRaWAN carries DevEUI least-significant-byte first. The canonical transmitted DevEUI is therefore:

```text
70B3D57ED0078C76
```

An earlier TTS/menuconfig capture showed:

```text
70B3D57ED0078C36
```

Until the **current** TTS end-device registration is checked against the transmitted `...8C76`, an empty device Live Data page must be diagnosed first as a possible identity mismatch. A JoinRequest for one DevEUI will not appear as activity for a different registered end device.

## Latest detailed local RF trace

The post-reprovision local trace starts with clean or restored local nonce state and reaches a real OTAA attempt. Example:

```text
JoinRequest (DevNonce = 6)
Frequency = 868.300 MHz, TX = 16 dBm
SF9 / BW125 / CR4/5 / uplink IQ
Uplink sent (ToA = 205 ms)
```

RX1:

```text
Frequency = 868.300 MHz
SF9 / BW125 / CR4/5 / downlink IQ
Rx1 window open (timeout: 6 symbols / 47651 ticks + 0ms)
Rx1 window closed
```

RX2:

```text
Frequency = 869.525 MHz
SF12 / BW125 / CR4/5 / downlink IQ
Rx2 window open (timeout: 6 symbols / 196660 ticks + 0ms)
Rx2 window closed
```

Result:

```text
state=-1116 (NO_JOIN_ACCEPT)
```

This proves that RadioLib stages internally coherent EU868 receive windows, but it does not prove the JoinRequest was received over the air. The current report of **no activity in TTS device Live Data** moves the immediate diagnosis back to identity/uplink/gateway reception rather than JoinAccept downlink reception.

## Historical TTS evidence

An earlier accepted attempt showed:

```text
JoinRequest frequency: 868.500 MHz
Data rate:            SF9 / BW125 / CR 4/5
DevNonce:             0x0040
Gateway:              eui-00800000a0004046
RSSI:                 about -109 dBm
SNR:                  about -13 dB
```

and events including:

```text
ns.up.join.process
js.join.accept
as.up.join.forward
```

That proves the hardware was capable of reaching TTN and that the historical registration/credentials were valid for those attempts. It does **not** prove that the current registration/configuration is identical, nor that a JoinAccept was transmitted by a gateway.

The historical export did not contain the decisive downlink scheduling/transmit events:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

## Changes and experiments already performed

### 1. Correct ESP32-S3 target and local component-manager recovery

Early local builds accidentally fell back to default target `esp32` because `set-target` never completed after a malformed build directory blocked `fullclean`. The generated build, managed components and lock file were removed and `set-target esp32s3` then completed successfully.

A separate Windows `FileNotFoundError` occurred while ESP-IDF Component Manager copied RadioLib from a deeply nested OneDrive clone. A short clone at `C:\src\MainboardOBU` removed that variable. Windows long-path support and Git `core.longpaths` were also enabled/recommended for the original clone.

### 2. Direct S3 Wi-Fi uploader parked

The experimental direct Wi-Fi/OpenTrafficMap path was removed from the normal S3 runtime/menu path and remains parked as development work. LoRaWAN is the active modular uplink under test.

### 3. Persistent DevNonce/session state added

RadioLib's persistence and session buffers are stored in ESP-IDF NVS. Nonce state survives ordinary resets/reflashes and is updated as RadioLib advances activation state. Session state is only valid after an accepted JoinAccept.

Persistence is intentionally fail-closed: a saved state whose activation checksum does not match current JoinEUI/DevEUI/root keys returns `RADIOLIB_ERR_NONCES_DISCARDED (-1119)` instead of silently resetting nonces.

### 4. `-1108` duty-cycle retry fixed

`RADIOLIB_ERR_UPLINK_UNAVAILABLE (-1108)` is treated as a legal duty-cycle wait/retry condition rather than as an OTAA failure. `ensure_joined()` uses `timeUntilUplink()` before retrying.

### 5. RadioLib issue #1806 / PR #1811 investigated

RadioLib issue #1806 closely matches the high-level symptom:

- ESP32-S3 + SX1262;
- JoinRequest reaches network;
- JoinAccept path appears to run;
- RX1/RX2 do not yield `RxDone`;
- `activateOTAA()` returns `-1116`.

The issue explicitly included the Seeed XIAO ESP32-S3 + Wio-SX1262 and was fixed by PR #1811, which implements LoRa symbol timeouts for LoRaWAN receive windows. The PR discussion reports that the same XIAO/Wio hardware joined successfully after the fix.

The project manifest now requests merge commit:

```text
12e3ed6c4814e177a87a7b2c48ab11dd65788143
```

However, the latest local build evidence proves the local lock still selected former PR head:

```text
f0fb029566c2d58a7373bb66d3a48002a5b56876
```

Therefore a true merged-state physical reproduction remains outstanding.

### 6. Wio-SX1262 board configuration cross-checked

Current configuration:

```text
MISO        GPIO8
SCK         GPIO7
MOSI        GPIO9
CS/NSS      GPIO41
RESET       GPIO42
DIO1        GPIO39
BUSY        GPIO40
RXEN        GPIO38
TXEN        NC
DIO2        RF switch control
DIO3 TCXO   1.8 V
```

This matches the known XIAO/Wio arrangement used by current Meshtastic support. GPIO38 is MCU-controlled RX enable; SX1262 DIO2 controls the module's internal RF switch.

### 7. Diagnostic mode added

`menuconfig` provides:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
     -> Diagnostics
```

with:

- structured BicycleOBU `ESP_LOG` diagnostics;
- RadioLib BASIC + PROTOCOL trace (`RLB_DBG` / `RLB_PRO`);
- optional full RadioLib SPI trace (`RLB_SPI`).

Full SPI trace stays disabled unless a TTS-confirmed downlink is still missed after identity/dependency/toolchain variables are removed.

## Current hypotheses and discriminating tests

### A. Device identity mismatch

**Status: highest priority until checked.**

Latest transmitted canonical DevEUI decodes to `70B3D57ED0078C76`; earlier TTS/menuconfig evidence showed `70B3D57ED0078C36`.

Next action: open the currently registered TTS end device and compare its DevEUI exactly with the on-air value. Do not continue RX-window debugging until these match.

If activation fields are changed in firmware, RadioLib's existing nonce state may correctly become invalid (`-1119`). In that case use the synchronized TTS/local reprovision procedure rather than bypassing persistence checks.

### B. JoinRequest is not reaching a gateway

**Status: possible, especially if identity matches but device Live Data remains empty.**

TTS troubleshooting states that if no Join Requests appear, verify that the device is actually transmitting on the band's default channels and that gateway coverage exists. Current RadioLib traces use EU868 default JoinRequest frequencies such as 868.100/868.300 MHz, so after identity is verified the next check is gateway reception/coverage.

Historical gateway receptions were weak (roughly -109 to -113 dBm RSSI on some attempts), so public-gateway availability and RF conditions remain variables.

### C. JoinAccept generated but not scheduled/transmitted

**Status: only test after the JoinRequest is visible in current TTS device events.**

Correlate the exact uplink ID with:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

### D. SX1262 receive path / RF switch / timing

**Status: downstream hypothesis; do not test yet while current JoinRequest is absent from TTS.**

Only focus here when TTS proves the exact JoinAccept was scheduled/transmitted and the intended RadioLib/IDF build still closes both RX windows without RxDone.

At that stage:

1. enable `RLB_SPI` for one join attempt;
2. inspect SX1262 sleep/wake, packet/modulation parameters, IRQ setup and `SetRx`;
3. capture GPIO38/RXEN transitions during RX1/RX2 rather than only sampling before/after `activateOTAA()`.

### E. Toolchain/dependency mismatch

**Status: confirmed local build mismatch; must be corrected.**

The project CI/reference toolchain is ESP-IDF 6.1, while current physical logs still show ESP-IDF 6.0.2. Separately, the local Component Manager lock is still using RadioLib `f0fb...` despite the repository manifest requesting `12e3...`.

Before the next software-comparison capture:

```powershell
idf.py -C firmware/s3 update-dependencies
idf.py -C firmware/s3 reconfigure
idf.py -C firmware/s3 build
```

Require the build log to contain:

```text
Processing ... RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
```

Prefer ESP-IDF 6.1 for the actual reference reproduction.

## Next test sequence

1. Check the **current TTS DevEUI**. Compare it with `70B3D57ED0078C76`, the DevEUI decoded from the latest JoinRequest trace.
2. Correct the identity mismatch if present.
3. Run:

```powershell
idf.py -C firmware/s3 update-dependencies
idf.py -C firmware/s3 reconfigure
```

4. Verify the build resolves RadioLib `12e3ed6c...`, not `f0fb0295...`.
5. Prefer/activate ESP-IDF 6.1.
6. Flash normally; do not erase NVS unless changing activation identity/keys as part of a synchronized TTS reprovision.
7. Watch TTS device Live Data. If the JoinRequest appears, preserve its correlation ID and proceed to JoinAccept scheduling events.
8. If no device event appears despite matching EUI, inspect gateway/coverage rather than RX1/RX2.
9. Only enable full SPI/RF-switch tracing after a gateway-transmitted JoinAccept is proven.

## Evidence files

- [`evidence/LORAWAN_OTAA_2026-08-19.md`](evidence/LORAWAN_OTAA_2026-08-19.md): first protocol-debug capture, including erase/DevNonce caveat and later `-1119` activation-checksum mismatch.
- [`evidence/LORAWAN_OTAA_2026-08-19_clean_repro.md`](evidence/LORAWAN_OTAA_2026-08-19_clean_repro.md): post-reprovision trace, corrected to record the stale RadioLib lock and decoded on-air DevEUI.

## Source references

Primary/maintainer sources used for this investigation:

- RadioLib issue #1806: https://github.com/jgromes/RadioLib/issues/1806
- RadioLib PR #1811: https://github.com/jgromes/RadioLib/pull/1811
- RadioLib LoRaWAN versions/RP guidance: https://github.com/jgromes/RadioLib/wiki/LoRaWAN%3A-versions-and-revisions
- RadioLib TTN setup guide: https://github.com/jgromes/RadioLib/wiki/LoRaWAN%3A-Device-setup-on-TTN
- RadioLib troubleshooting guide: https://github.com/jgromes/RadioLib/wiki/Troubleshooting-Guide
- RadioLib discussion #1361, XIAO ESP32-S3/Wio-SX1262 ESP-IDF setup: https://github.com/jgromes/RadioLib/discussions/1361
- Meshtastic XIAO ESP32-S3 board definition: https://github.com/meshtastic/firmware/blob/develop/variants/esp32s3/seeed_xiao_s3/variant.h
- Seeed Wio-SX1262 module datasheet: https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf
- ESP-IDF Component Manager update dependencies: https://docs.espressif.com/projects/idf-component-manager/en/latest/use/how_to_update_dependencies.html
- ESP-IDF `dependencies.lock` reference: https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/dependencies_lock.html
- The Things Stack device troubleshooting: https://www.thethingsindustries.com/docs/hardware/devices/troubleshooting/
- The Things Stack events API: https://www.thethingsindustries.com/docs/api/reference/grpc/events/

## Close criteria

Do not close this investigation merely because `js.join.accept` appears. Close only after all of the following are observed on physical hardware:

1. current firmware identity exactly matches the registered TTS device;
2. the intended RadioLib dependency and ESP-IDF reference toolchain are actually used;
3. TTS processes a JoinRequest and schedules/transmits a JoinAccept;
4. RadioLib reports `RADIOLIB_LORAWAN_NEW_SESSION` or a restored valid session;
5. the S3 sends an application uplink after activation;
6. after a hard power cycle, NVS restores the session without unsafe nonce/counter reuse;
7. the TTS MQTT bridge reconstructs a C-ITS frame and publishes it to OpenTrafficMap.
