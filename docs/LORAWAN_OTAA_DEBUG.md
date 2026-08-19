# LoRaWAN OTAA / JoinAccept investigation log

This is the running engineering record for the BicycleOBU Wio-SX1262 OTAA investigation. Keep observations, attempted changes, evidence and next tests here so experiments are not repeated without purpose.

The recurring device-side result is:

```text
obu_lorawan: LoRaWAN OTAA join failed: RadioLib state=-1116
```

RadioLib defines `-1116` as `RADIOLIB_ERR_NO_JOIN_ACCEPT`: `activateOTAA()` did not finish with a valid decoded JoinAccept. This code alone does **not** prove that The Things Stack received the JoinRequest or transmitted a JoinAccept.

## Current state — 2026-08-19

### Hardware / protocol

- MCU: Seeed XIAO ESP32-S3.
- Radio: Seeed Wio-SX1262 on the XIAO B2B connector.
- Region: EU868.
- Activation: OTAA, LoRaWAN 1.1 with NwkKey and AppKey.
- Current TTS frequency plan: `Europe 863-870 MHz (SF12 for RX2)`.
- Current TTS Regional Parameters: RP002 1.0.4.
- SX1262 initialization succeeds.
- NVS nonce/session persistence is implemented and fail-closed.
- Normal firmware flashing preserves NVS; `erase-flash` must not be used casually.

### Current identity is verified

The current recreated TTS device uses:

```text
JoinEUI: 0000000000000000
DevEUI:  70B3D57ED0078C76
```

The current firmware `menuconfig` contains the same JoinEUI/DevEUI, and the latest RadioLib JoinRequest trace decodes to canonical DevEUI `70B3D57ED0078C76`.

The earlier `...8C36` value belonged to an older registration/configuration. The previously suspected `...8C76` versus `...8C36` identity mismatch is therefore **resolved** for the current device.

Root keys are intentionally not copied into repository documentation. The visible prefixes shown in the current TTS/menuconfig capture are consistent; full masked TTS values cannot be independently compared from the screenshot.

### RadioLib dependency is now the intended merged state

The repository manifest requests RadioLib merge commit:

```text
12e3ed6c4814e177a87a7b2c48ab11dd65788143
```

Earlier local builds remained locked to former PR-head commit `f0fb0295...`. The local dependency lock has now been updated with:

```powershell
idf.py -C firmware/s3 update-dependencies
idf.py -C firmware/s3 reconfigure
```

The physical build now explicitly reports:

```text
RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
```

and RadioLib reports:

```text
Platform: "ESP-IDF"
```

rather than the former `Platform: "Generic"`. This is the first physical capture that legitimately uses the intended PR #1811 merged state and native ESP-IDF HAL.

The physical build still uses ESP-IDF 6.0.2. Project CI/reference builds use ESP-IDF 6.1, so the final software-comparison test still needs to be repeated on 6.1.

## Latest merged-RadioLib physical trace

The latest attempt starts with valid restored nonce state and no session:

```text
LoRaWAN NVS ready: nonce_state=present session_state=fresh
RadioLib persistence attached: nonce_state=restored session_restored=no
```

Radio configuration:

```text
SPI SCK=7 MISO=8 MOSI=9 NSS=41
DIO1=39 RESET=42 BUSY=40
RXEN=38 TXEN=NC
DIO2 RF switch=yes
TCXO=1.8 V
FreeRTOS_Hz=1000
```

JoinRequest:

```text
DevNonce = 14
DevEUI = 70B3D57ED0078C76
Frequency = 868.500 MHz
TX = 16 dBm
SF9 / BW125 / CR4/5 / uplink IQ
Uplink sent (ToA = 205 ms)
```

RX1:

```text
868.500 MHz
SF9 / BW125 / CR4/5 / downlink IQ
Rx1 window open (6 symbols)
Rx1 window closed
```

RX2:

```text
869.525 MHz
SF12 / BW125 / CR4/5 / downlink IQ
Rx2 window open (6 symbols)
Rx2 window closed
```

Result:

```text
state=-1116 (NO_JOIN_ACCEPT)
```

### Crucial current server-side observation

The current TTS end-device Live Data dashboard shows **no activity** during these merged-state attempts.

Because the current JoinEUI/DevEUI match exactly, the empty dashboard is no longer explained by device identity. `RLB_PRO: Uplink sent` only proves that the local SX1262 transmit operation completed; it does not acknowledge reception by a gateway.

Therefore the latest `-1116` must **not** yet be treated as evidence that RadioLib PR #1811 still misses a TTS-transmitted JoinAccept. For this exact attempt there is currently no evidence that TTS received the JoinRequest at all.

The immediate blocker is now current **uplink/gateway reception**, not RX1/RX2 decoding.

See [`evidence/LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md`](evidence/LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md).

## Historical TTS evidence

Earlier attempts were received by public TTN gateways and accepted by The Things Stack. One recorded attempt used:

```text
868.500 MHz
SF9 / BW125 / CR4/5
RSSI around -109 dBm
SNR around -13 dB
```

Another attempt was heard by two gateways, with RSSI around -110/-113 dBm. Historical events included:

```text
ns.up.join.process
js.join.accept
as.up.join.forward
```

This proves that the hardware has produced LoRaWAN JoinRequests that public TTN gateways could receive, and that the historical device identities/keys were accepted. It does not prove that a public gateway is currently online or in range.

The historical exports did not contain enough same-correlation evidence to prove actual JoinAccept RF transmission (`ns.down.join.schedule.*` / `gs.down.tx.*`).

## Changes / experiments already performed

### 1. ESP32-S3 target and Component Manager recovery

Early local builds accidentally fell back to `esp32` because `set-target` never completed after a malformed build directory blocked `fullclean`. Generated build/managed-component state was removed and `set-target esp32s3` completed successfully.

A Windows `FileNotFoundError` occurred when ESP-IDF Component Manager copied RadioLib below a deeply nested OneDrive path. A short clone at `C:\src\MainboardOBU` removed that variable. Windows long-path support and Git `core.longpaths` were also enabled/recommended for the original clone.

### 2. Direct S3 Wi-Fi uploader parked

The experimental direct Wi-Fi/OpenTrafficMap path was removed from the normal runtime/menu path. LoRaWAN is the active modular uplink under test.

### 3. Persistent DevNonce/session state

RadioLib persistence/session buffers are stored in ESP-IDF NVS. Nonce state survives normal reflash/reboot and is updated as RadioLib advances activation state. Session state is valid only after the device accepts a JoinAccept.

Saved activation state is fail-closed. If JoinEUI/DevEUI/root keys no longer match the persisted state, RadioLib returns `RADIOLIB_ERR_NONCES_DISCARDED (-1119)` rather than silently resetting nonces.

### 4. Duty-cycle `-1108` retry fixed

`RADIOLIB_ERR_UPLINK_UNAVAILABLE (-1108)` is handled as a legal duty-cycle wait/retry condition, not as a failed OTAA exchange. `ensure_joined()` checks `timeUntilUplink()` and does not increment join failures for this condition.

### 5. RadioLib issue #1806 / PR #1811

RadioLib issue #1806 closely matches the original high-level symptom:

- ESP32-S3 + SX1262;
- JoinRequest reaches network;
- JoinAccept is transmitted in the reported reproduction;
- RX1/RX2 open but do not get `RxDone`;
- `activateOTAA()` returns `-1116`.

The issue explicitly included Seeed XIAO ESP32-S3 + Wio-SX1262. PR #1811 implements LoRa symbol timeout handling for receive windows and reports successful testing on the same XIAO/Wio hardware.

The project now actually builds the PR #1811 merge commit `12e3ed6c...`. The current six-symbol windows confirm that receive-window changes are active.

### 6. Wio-SX1262 board configuration cross-check

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

Seeed's Wio-SX1262 datasheet states that DIO2 determines TX/RX selection (HIGH TX, LOW RX), DIO3 powers the TCXO, and TCXO supply may be 1.7–3.3 V while staying at least 200 mV below VCC. The current 1.8 V setting is electrically valid and matches current Meshtastic XIAO/Wio support.

### 7. Diagnostic mode

`menuconfig` provides:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
     -> Diagnostics
```

with:

- structured `ESP_LOG` diagnostics;
- RadioLib BASIC + PROTOCOL trace (`RLB_DBG` / `RLB_PRO`);
- optional full RadioLib SPI trace (`RLB_SPI`).

Full SPI tracing remains an escalation only after TTS proves a JoinAccept was actually transmitted.

### 8. TTS registration aligned with RadioLib pre-join RX2

The current diagnostic device uses:

```text
Europe 863-870 MHz (SF12 for RX2)
LoRaWAN 1.1.0
RP002 1.0.4
```

RadioLib's pre-join EU868 RX2 trace is 869.525 MHz / DR0 (SF12/BW125), so the current TTS frequency-plan choice removes the prior potential SF9-vs-SF12 RX2 ambiguity for the join itself.

### 9. Configurable initial JoinRequest data rate added

To separate public-gateway/link-budget failures from protocol/RX-window failures, the firmware now exposes:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
     -> Activation and network
        -> Initial OTAA uplink data rate (EU868 DR0..DR5)
```

Mapping:

```text
DR0=SF12, DR1=SF11, DR2=SF10, DR3=SF9, DR4=SF8, DR5=SF7
```

all at BW125. Default is DR3 to preserve previous behavior. For a new/pending activation RadioLib applies the configured DR with ADR temporarily disabled, then ADR is re-enabled. A restored session is not overwritten. DR0 is the next intended coverage diagnostic when TTS receives no DR3 JoinRequest.

## Current hypotheses and discriminating tests

### A. Current JoinRequest is not reaching a TTN gateway

**Status: highest priority.**

Current identity matches, but TTS shows no event. RadioLib uses EU868 default JoinRequest channels (868.1/868.3/868.5 MHz). Historical reception was through public gateways and some recorded links were weak.

Likely variables now include:

- public gateway offline/unavailable;
- location/indoor attenuation changed;
- antenna/IPEX connection or antenna orientation;
- RF interference;
- current uplink link budget at SF9.

Next tests:

1. set initial OTAA DR to **DR0/SF12** and verify the protocol trace changes accordingly;
2. test much closer to a known active TTN EU868 gateway, or use a controlled local multi-channel gateway;
3. verify antenna/IPEX seating and use an 868 MHz antenna;
4. only proceed to JoinAccept investigation after the JoinRequest appears in TTS.

### B. JoinAccept is accepted by TTS but not actually transmitted

**Status: not currently testable because latest JoinRequest is absent from TTS.**

Once an uplink appears, preserve its correlation ID and inspect:

```text
js.join.accept
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

### C. SX1262 receive path / RF switch / timing

**Status: downstream hypothesis.**

Only return here if TTS proves the same JoinAccept was scheduled/transmitted and the merged RadioLib/ESP-IDF build still closes both RX windows empty.

Then:

1. enable `RLB_SPI` for exactly one join attempt;
2. inspect SX1262 sleep/wake, modulation/packet parameters, IRQ setup and `SetRx`;
3. capture GPIO38/RXEN transitions during RX1/RX2 rather than just before/after `activateOTAA()`.

### D. Toolchain mismatch

**Status: dependency mismatch resolved; ESP-IDF version still differs.**

RadioLib is now the intended `12e3ed6c...` merge commit. Current hardware tests still use ESP-IDF 6.0.2 while CI/reference uses 6.1. Repeat the final controlled comparison under 6.1, but do not attribute an empty TTS dashboard to RX-window timing before proving gateway reception.

## Next test sequence

1. Keep the current matching TTS identity/keys and **do not erase NVS**.
2. Keep RadioLib `12e3ed6c...`; after any dependency cleanup verify the build prints this exact commit.
3. In menuconfig set **Initial OTAA uplink data rate = 0 (DR0/SF12)** for one coverage test.
4. Flash normally, not `erase-flash`, and verify `RLB_PRO` reports the JoinRequest at SF12/BW125.
5. Prefer ESP-IDF 6.1 for the final controlled run.
6. Move close to a known active TTN gateway or use a controlled EU868 gateway.
7. Watch TTS Live Data during the JoinRequest.
8. If no event appears even at DR0 near a known gateway, debug the uplink antenna/RF path rather than RX1/RX2.
9. If an event appears and `js.join.accept` succeeds, inspect `ns.down.join.schedule.*` and `gs.down.tx.*` for the same correlation ID.
10. Only after a gateway-transmitted JoinAccept is proven should full SPI/RXEN diagnostics be enabled.

## Evidence files

- [`evidence/LORAWAN_OTAA_2026-08-19.md`](evidence/LORAWAN_OTAA_2026-08-19.md): first protocol-debug capture, erase/DevNonce caveat and later `-1119` state mismatch.
- [`evidence/LORAWAN_OTAA_2026-08-19_clean_repro.md`](evidence/LORAWAN_OTAA_2026-08-19_clean_repro.md): post-reprovision trace; later corrected because local RadioLib was still locked to the PR-head commit.
- [`evidence/LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md`](evidence/LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md): first true `12e3...`/native-ESP-IDF physical trace; current TTS identity matches but no TTS event is observed.

## Source references

Primary/maintainer sources used for this investigation:

- RadioLib issue #1806: https://github.com/jgromes/RadioLib/issues/1806
- RadioLib PR #1811: https://github.com/jgromes/RadioLib/pull/1811
- RadioLib `LoRaWANNode` API / `setDatarate()`: https://github.com/jgromes/RadioLib/blob/master/src/protocols/LoRaWAN/LoRaWAN.h
- RadioLib troubleshooting guide: https://github.com/jgromes/RadioLib/wiki/Troubleshooting-Guide
- RadioLib discussion #1361, XIAO ESP32-S3/Wio-SX1262 ESP-IDF setup: https://github.com/jgromes/RadioLib/discussions/1361
- Meshtastic XIAO/Wio board definition: https://github.com/meshtastic/firmware/blob/develop/variants/esp32s3/seeed_xiao_s3/variant.h
- Seeed Wio-SX1262 datasheet: https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf
- Seeed Wio-SX1262/XIAO schematic: https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Schematic_Diagram_Wio-SX1262_for_XIAO.pdf
- ESP-IDF Component Manager update dependencies: https://docs.espressif.com/projects/idf-component-manager/en/latest/use/how_to_update_dependencies.html
- The Things Stack device troubleshooting: https://www.thethingsindustries.com/docs/hardware/devices/troubleshooting/
- The Things Stack events API: https://www.thethingsindustries.com/docs/api/reference/grpc/events/

## Close criteria

Do not close this investigation merely because `js.join.accept` appears. Close only after all of the following are observed on physical hardware:

1. current firmware identity exactly matches the registered TTS device;
2. intended RadioLib dependency and ESP-IDF reference toolchain are used;
3. TTS processes a JoinRequest and schedules/transmits a JoinAccept;
4. RadioLib reports `RADIOLIB_LORAWAN_NEW_SESSION` or restores a valid session;
5. the S3 sends an application uplink after activation;
6. after a hard power cycle, NVS restores the session without unsafe nonce/counter reuse;
7. the TTS MQTT bridge reconstructs a C-ITS frame and publishes it to OpenTrafficMap.
