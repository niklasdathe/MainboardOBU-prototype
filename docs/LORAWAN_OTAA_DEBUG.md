# LoRaWAN OTAA / JoinAccept investigation log

This document is the running engineering record for the BicycleOBU Wio-SX1262 OTAA problem. Keep observations, attempted changes, evidence and next tests here so the same experiments are not repeated without purpose.

The currently observed device-side failure is:

```text
obu_lorawan: LoRaWAN OTAA join failed: RadioLib state=-1116
```

RadioLib defines `-1116` as `RADIOLIB_ERR_NO_JOIN_ACCEPT`. For OTAA this means that `activateOTAA()` did not finish with a valid decoded JoinAccept.

## Current evidence

### Device / radio

- MCU: Seeed XIAO ESP32-S3.
- Radio: Seeed Wio-SX1262 on the XIAO B2B connector.
- Region: EU868.
- Activation: OTAA, LoRaWAN 1.1 with both NwkKey and AppKey.
- JoinEUI and DevEUI parse correctly and match the registered TTS device.
- SX1262 initialization succeeds.
- JoinRequests are transmitted and received by public TTN gateways.
- NVS nonce persistence is active; observed firmware logs show `nonce_state=present` / `persisted_nonces=yes` while no session has yet been restored.
- Normal flash updates are used; NVS must not be erased while this investigation is active.

### Latest exported The Things Stack evidence, 2026-08-19

The exported device Live Data for the latest detailed attempt shows:

```text
JoinRequest frequency: 868.500 MHz
Data rate:            SF9 / BW125 / CR 4/5
DevNonce:             0x0040
Gateway:              eui-00800000a0004046
RSSI:                 -109 dBm
SNR:                  -13.2 dB
```

The same attempt contains:

```text
ns.up.join.process
js.join.accept
as.up.join.forward
```

and TTS assigned a pending session / DevAddr. This is strong evidence that:

1. the SX1262 uplink path is functional;
2. the JoinRequest reaches The Things Stack;
3. the device identifiers and root keys are valid for that attempt;
4. the Join Server accepts the DevNonce and creates session material.

The exported Live Data does **not** contain the decisive downlink scheduling events:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success
ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success
gs.down.tx.fail
```

Therefore `js.join.accept` / `as.up.join.forward` alone must not be documented as proof that a gateway actually transmitted the JoinAccept over RF.

## Changes and experiments already performed

### 1. Correct ESP32-S3 target and local component-manager recovery

Early local builds accidentally fell back to the default `esp32` target because `set-target` did not complete after a malformed build directory blocked `fullclean`. The generated build, managed components and lock file were removed and `set-target esp32s3` was run successfully. Subsequent builds explicitly report `Building ESP-IDF components for target esp32s3`.

A separate Windows `FileNotFoundError` occurred while ESP-IDF Component Manager copied RadioLib from a deeply nested OneDrive clone. A short clone at `C:\src\MainboardOBU` removed that variable. Windows long-path support and Git `core.longpaths` were also recommended for the original clone. This was a build-environment issue, not evidence about the RF problem.

### 2. Direct S3 Wi-Fi uploader parked

The experimental direct Wi-Fi/OpenTrafficMap path was removed from the normal S3 runtime/menu path and remains parked as development work. LoRaWAN is the normal modular uplink under test.

### 3. TTS registration and credentials verified

TTS receives JoinRequests for the expected device and the Join Server repeatedly accepts them. The current failure is therefore not being treated as a simple malformed EUI/key configuration problem unless a later TTS event explicitly contradicts that evidence.

Root keys are intentionally never copied into this document or firmware logs.

### 4. Persistent DevNonce/session state added

RadioLib's persistence and session buffers are stored in ESP-IDF NVS. The nonce buffer is restored after reboot and updated as RadioLib advances activation state. A session is not considered present until the device has actually accepted a JoinAccept.

This prevents reset-driven DevNonce reuse from being used as a hidden workaround. Do not use `erase-flash` during normal testing.

### 5. `-1108` duty-cycle retry was fixed

A later retry returned immediately with `-1108` (`RADIOLIB_ERR_UPLINK_UNAVAILABLE`). That was not a JoinAccept failure: RadioLib's duty-cycle scheduler was rejecting a transmit attempt before RF transmission.

`ensure_joined()` now calls `timeUntilUplink()`, waits for the legal transmit time, and retries `RADIOLIB_ERR_UPLINK_UNAVAILABLE` without incrementing the OTAA failure count. Application data uplinks use the same principle.

### 6. RadioLib issue #1806 identified

RadioLib issue #1806 documents an unusually close match to this failure:

- ESP32-S3 + SX1262;
- JoinRequest reaches the network;
- network sends a JoinAccept;
- RX1/RX2 appear to open but no `RxDone` is obtained;
- `activateOTAA()` returns `RADIOLIB_ERR_NO_JOIN_ACCEPT (-1116)`.

The issue was fixed by RadioLib PR #1811, which implements LoRa symbol timeout handling for LoRaWAN receive windows and reports testing across spreading factors on ESP32 + SX1262 + TTN.

### 7. RadioLib pin changed from PR head to merged state

The project initially pinned PR #1811 head:

```text
f0fb029566c2d58a7373bb66d3a48002a5b56876
```

It now pins the merged RadioLib commit:

```text
12e3ed6c4814e177a87a7b2c48ab11dd65788143
```

The merged state includes the updated native ESP-IDF HAL. Its delay implementation avoids relying only on coarse FreeRTOS ticks for timing-sensitive LoRaWAN RX windows. The BicycleOBU S3 configuration also uses `CONFIG_FREERTOS_HZ=1000`.

The `-1116` failure has still been observed after moving to the merged RadioLib state, so the investigation continues rather than assuming PR #1811 alone resolves the board-specific problem.

### 8. Wio-SX1262 board configuration cross-checked

The current Meshtastic board definition for the same XIAO ESP32-S3 + Wio-SX1262 combination uses:

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

This confirms the project's B2B pin mapping and the model where RadioLib drives GPIO38 as RXEN while SX1262 DIO2 controls the module's internal TX/RX RF switch.

The project previously initialized DIO3 TCXO at 3.0 V. That value is within the Seeed module's documented TCXO supply range, so it was not known to be electrically invalid. However, the exact known-working XIAO/Wio board definition and an ESP-IDF RadioLib report for this module use **1.8 V**. BicycleOBU now uses 1.8 V to remove that board-configuration difference.

### 9. Proper diagnostic mode added

`menuconfig` now provides a LoRaWAN diagnostics group with:

- structured BicycleOBU diagnostics using `ESP_LOG`;
- RadioLib basic + protocol trace (`RLB_DBG` / `RLB_PRO`);
- optional full RadioLib SPI trace (`RLB_SPI`).

RadioLib maintainers repeatedly request `RADIOLIB_DEBUG_PROTOCOL` when diagnosing LoRaWAN JoinAccept/downlink failures. Full SPI tracing is intentionally a separate option because it is extremely verbose and can perturb timing/output enough to make routine testing harder.

## Current hypotheses and discriminating tests

### A. JoinAccept is generated but not actually scheduled/transmitted by the gateway

**Status:** still possible; the current exported TTS data is insufficient.

The next TTS capture must include the same correlation ID across Network Server and Gateway Server events. The decisive events are:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail

gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

Interpretation:

- `ns.down.join.schedule.fail`: stop debugging the SX1262 receive window for that attempt; the Network/Gateway Server failed before a usable RF downlink was scheduled.
- `ns.down.join.schedule.success` followed by `gs.down.tx.success`: the server/gateway side reports successful scheduling/transmission; focus on RF link budget and device RX behavior.

### B. SX1262 receive-window timing/state still fails on this ESP-IDF integration

**Status:** plausible.

RadioLib #1806 demonstrates the same `-1116` symptom on ESP32-S3 + SX1262. The project uses the merged #1811 fix, but a board/HAL interaction may still remain.

Discriminating evidence is the `RLB_PRO` trace. It should show the JoinRequest parameters and the RX1/RX2 windows. Compare the frequency, data rate, IQ configuration and timing against the TTS `ns.down.join.schedule.attempt` payload.

### C. Weak RF/downlink link budget

**Status:** plausible and easy to test.

The latest exported uplink was heard at approximately -109 dBm / -13.2 dB SNR. That proves reception but is not a strong link. Repeat one test much closer to a known gateway (or with a controlled local multi-channel EU868 gateway) before treating every missed JoinAccept as a software defect.

### D. Wio external/internal RF switching

**Status:** configuration now matches the known XIAO/Wio board definition, but remains observable in debug.

Expected design:

- GPIO38 = external RX enable under `Module::setRfSwitchPins(rxEn, txEn)`;
- MCU TX enable = NC;
- SX1262 DIO2 = module internal RF TX/RX switch;
- DIO3 = 1.8 V TCXO control.

Structured debug logs include DIO1, BUSY and GPIO38 state before/after `activateOTAA()`. If TTS proves downlink transmission and `RLB_PRO` shows a correct RX window, a short `RLB_SPI` capture is the next escalation.

### E. DevNonce/root-key mismatch

**Status:** currently unlikely for accepted attempts.

TTS accepted a JoinRequest with DevNonce `0x0040` and created a pending session. A nonce/key rejection may still happen on another attempt, but it must be diagnosed from that attempt's TTS error rather than inferred from `-1116` alone.

## How to capture the next useful log

Run:

```text
idf.py -C firmware/s3 menuconfig
```

Navigate to:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
     -> Diagnostics
```

For the first capture use:

```text
[x] Enable structured LoRaWAN ESP_LOG diagnostics
[x] Enable RadioLib basic + protocol trace (RLB_DBG/RLB_PRO)
[ ] Enable RadioLib full SPI trace (RLB_SPI, extremely verbose)
```

Then:

```text
idf.py -C firmware/s3 build
idf.py -C firmware/s3 -p COMx flash monitor
```

Do not erase flash/NVS.

Capture from at least the following line through the final join result:

```text
Joining LoRaWAN network using OTAA
...
RLB_PRO: ...
...
LoRaWAN OTAA active ...
```

or:

```text
LoRaWAN OTAA join failed: ...
```

For `-1116`, also export the TTS Live Data around the same JoinRequest and preserve the correlation ID. The ideal server-side chain is:

```text
ns.up.join.process
js.join.accept
ns.down.join.schedule.attempt
ns.down.join.schedule.success
 gs.down.send
 gs.down.tx.success
as.up.join.forward
```

If protocol tracing does not expose why a TTS-confirmed transmitted JoinAccept is missed, enable the full SPI trace for **one join attempt only** and compare the SX1262 commands around RX1/RX2 with RadioLib issue #1806.

## Source references

Primary/maintainer sources used for this investigation:

- RadioLib issue #1806, ESP32-S3 + SX1262 JoinAccept `-1116`: https://github.com/jgromes/RadioLib/issues/1806
- RadioLib PR #1811, LoRaWAN receive-window symbol timeout fix: https://github.com/jgromes/RadioLib/pull/1811
- RadioLib debug build options: https://github.com/jgromes/RadioLib/blob/master/src/BuildOpt.h
- RadioLib troubleshooting guide: https://github.com/jgromes/RadioLib/wiki/Troubleshooting-Guide
- RadioLib discussion #1746, maintainer request for protocol debug on no-JoinAccept cases: https://github.com/jgromes/RadioLib/discussions/1746
- RadioLib discussion #1795, guidance that BASIC + PROTOCOL are normally enough and SPI is extremely verbose: https://github.com/jgromes/RadioLib/discussions/1795
- RadioLib discussion #1361, XIAO ESP32-S3/Wio-SX1262 ESP-IDF pin/TCXO report: https://github.com/jgromes/RadioLib/discussions/1361
- Meshtastic XIAO ESP32-S3 board definition: https://github.com/meshtastic/firmware/blob/develop/variants/esp32s3/seeed_xiao_s3/variant.h
- Seeed Wio-SX1262 module datasheet: https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf
- The Things Stack Events API: https://www.thethingsindustries.com/docs/api/reference/grpc/events/
- The Things Stack device troubleshooting/downlink flow: https://www.thethingsindustries.com/docs/hardware/devices/troubleshooting/

## Close criteria

Do not close this investigation merely because `js.join.accept` appears. Close it only after all of the following are observed on physical hardware:

1. TTS processes a JoinRequest and schedules/transmits a JoinAccept.
2. RadioLib reports `RADIOLIB_LORAWAN_NEW_SESSION` or a restored valid session.
3. The S3 sends an application uplink after activation.
4. After a hard power cycle, NVS reports a restorable session and the device continues without unsafe nonce/counter reuse.
5. The TTS MQTT bridge reconstructs a C-ITS frame and publishes it to OpenTrafficMap.
