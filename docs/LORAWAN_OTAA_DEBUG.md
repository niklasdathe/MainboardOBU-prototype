# LoRaWAN OTAA / JoinAccept investigation log

This is the canonical running engineering record for the BicycleOBU Wio-SX1262 OTAA investigation. Keep observations, attempted changes, evidence and next tests here so experiments are not repeated without purpose.

The recurring device-side result is:

```text
obu_lorawan: LoRaWAN OTAA join failed: RadioLib state=-1116
```

RadioLib defines `-1116` as `RADIOLIB_ERR_NO_JOIN_ACCEPT`: `activateOTAA()` did not finish with a valid decoded JoinAccept. The code alone does not prove whether TTS received the JoinRequest or whether any gateway transmitted a JoinAccept.

## Current state — 2026-08-19

### Hardware / protocol

- MCU: Seeed XIAO ESP32-S3.
- Radio: Seeed Wio-SX1262 on the XIAO B2B connector.
- Region: EU868.
- Activation: OTAA, LoRaWAN 1.1 with NwkKey and AppKey.
- Current diagnostic TTS frequency plan: `Europe 863-870 MHz (SF12 for RX2)`.
- Current TTS Regional Parameters: RP002 1.0.4.
- SX1262 initialization succeeds.
- NVS nonce/session persistence is implemented and fail-closed.
- Normal firmware flashing preserves NVS; `erase-flash` must not be used casually.

### Current diagnostic identity

The current fresh development registration used for the DR0 capture is:

```text
JoinEUI: 1111111111111111
DevEUI:  70B3D57ED0078C82
```

The firmware menuconfig and the on-air JoinRequest contain the same values. Root keys are intentionally not copied into repository documentation. TTS accepted the JoinRequest, which independently proves the configured root keys for this capture are valid.

### RadioLib dependency

The repository and physical build now use the intended RadioLib PR #1811 merge commit:

```text
12e3ed6c4814e177a87a7b2c48ab11dd65788143
```

The local build explicitly resolves this commit and RadioLib reports:

```text
Platform: "ESP-IDF"
```

The earlier stale `dependencies.lock` problem with `f0fb0295...` is resolved. The current physical build still uses ESP-IDF 6.0.2; CI/reference uses ESP-IDF 6.1.

## Latest decisive DR0 / TTS capture

The initial OTAA data rate was deliberately set to EU868 DR0 to maximize link budget:

```text
DR0 = SF12 / BW125
```

The firmware started with synchronized fresh NVS after a newly created TTS device and transmitted:

```text
DevNonce = 0
JoinEUI = 1111111111111111
DevEUI = 70B3D57ED0078C82
Frequency = 868.100 MHz
TX = 16 dBm
SF12 / BW125 / CR4/5
ToA = 1482 ms
```

RadioLib opened coherent receive windows:

```text
RX1: 868.100 MHz / SF12 / BW125 / inverted IQ
RX2: 869.525 MHz / SF12 / BW125 / inverted IQ
```

Both windows closed without RxDone and RadioLib returned `-1116`.

### TTS received and accepted this exact JoinRequest

The exported TTS Live Data contains the same DevNonce, frequency and data rate and shows:

```text
ns.up.join.process
js.join.accept
as.up.join.forward
```

The only receiving path was through Packet Broker from a legacy TTN V2 forwarder:

```text
forwarder tenant: ttnv2
forwarder cluster: ttn-v2-legacy-eu
gateway EUI: B827EBFFFE61601A
gateway ID: eui-b827ebfffe61601a
RSSI: -119 dBm
SNR: -5.5 dB
```

This proves current SX1262 TX, current activation identity and current root keys are valid. TTS generated a JoinAccept/pending session.

### Crucial missing downlink evidence

The export was taken about 26 seconds after the JoinRequest and contains none of:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success
ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success
gs.down.tx.fail
```

Therefore there is currently **no evidence that TTS scheduled or a gateway transmitted the JoinAccept over RF**. The primary blocker has moved from uplink reception to the **downlink scheduling/gateway path**.

Packet Broker supports OTAA join-accept downlinks in principle, but forwarding networks control join-accept downlink permission through routing policies. The current uplink came through the legacy `ttn-v2-legacy-eu` Packet Broker path, so a successful forwarded uplink does not by itself prove that this gateway/path is usable for JoinAccept downlink scheduling.

See [`evidence/LORAWAN_OTAA_2026-08-19_dr0_packetbroker_join_accepted_no_schedule.md`](evidence/LORAWAN_OTAA_2026-08-19_dr0_packetbroker_join_accepted_no_schedule.md).

## Changes / experiments already performed

1. **ESP32-S3 target / Windows path recovery:** corrected accidental target fallback and used a short clone after Component Manager hit Windows/OneDrive path issues. Long-path support was also enabled/recommended.
2. **Direct S3 Wi-Fi uploader parked:** direct Wi-Fi/OpenTrafficMap settings removed from the normal runtime/menu path.
3. **Persistent DevNonce/session state:** RadioLib Nonces and session buffers are stored in NVS; activation mismatch fails closed with `-1119` instead of silently resetting nonces.
4. **Duty-cycle `-1108` retry fixed:** `RADIOLIB_ERR_UPLINK_UNAVAILABLE` is treated as a legal wait/retry condition using `timeUntilUplink()`.
5. **RadioLib issue #1806 / PR #1811:** pinned to the merged receive-window symbol-timeout fix tested upstream on ESP32-S3 + SX1262, including XIAO ESP32-S3 + Wio-SX1262 evidence.
6. **Wio board settings cross-checked:** SCK7, MISO8, MOSI9, NSS41, DIO1=39, RESET42, BUSY40, GPIO38 RXEN, TXEN NC, DIO2 RF switch, DIO3 TCXO 1.8 V.
7. **Diagnostics added:** structured ESP_LOG, RadioLib BASIC/PROTOCOL trace and optional full SPI trace are menuconfig-controlled under `BicycleOBU prototype -> LoRaWAN uplink -> Diagnostics`.
8. **Frequency-plan alignment:** diagnostic TTS device uses SF12 for RX2, matching RadioLib pre-join EU868 RX2 of 869.525 MHz / DR0.
9. **Configurable initial JoinRequest DR:** `Initial OTAA uplink data rate (EU868 DR0..DR5)` added under `Activation and network`; DR0/SF12 was successfully observed by TTS.
10. **Current device identity/root keys revalidated:** a freshly created diagnostic registration with new JoinEUI/DevEUI was accepted by TTS at DevNonce 0.

## Current hypotheses and discriminating tests

### A. Legacy Packet Broker forwarding path cannot currently provide a usable JoinAccept downlink

**Status: highest priority.**

Evidence:

- uplink is received by one `ttnv2` / `ttn-v2-legacy-eu` Packet Broker gateway;
- TTS validates the join and creates a pending session;
- no `ns.down.join.schedule.*` or `gs.down.tx.*` event is present well after the join windows.

Next test: repeat the same DR0 activation close to a known current TTS/TTN V3 EU868 gateway or, preferably, a controlled local multi-channel gateway connected directly to the current TTS Gateway Server.

If a direct/current gateway produces scheduling/transmission events and the device joins, treat the legacy Packet Broker path as the limitation and do not alter the working radio RX configuration.

### B. TTS schedules/transmits JoinAccept but SX1262 does not receive it

**Status: not yet demonstrated.**

Only investigate this if the same correlation path proves:

```text
ns.down.join.schedule.success
gs.down.send
gs.down.tx.success
```

Then:

1. enable `RLB_SPI` for exactly one JoinRequest;
2. instrument GPIO38/RXEN transitions during RX1/RX2 without timing-disturbing logging from the ISR;
3. compare TTS downlink frequency/data rate/timestamp with RadioLib RX1/RX2;
4. inspect SX1262 IRQ setup and SetRx sequence.

### C. Toolchain mismatch

**Status: secondary.**

RadioLib dependency mismatch is resolved. Physical testing still uses ESP-IDF 6.0.2 while CI/reference is 6.1. Repeat the final controlled comparison under 6.1, but do not attribute the missing server scheduling events to device RX timing.

## Next test sequence

1. Do **not** erase flash again; preserve the current nonce state.
2. Keep DR0/SF12 and BASIC/PROTOCOL diagnostics for the immediate gateway-path comparison.
3. Use the same TTS device/keys unless deliberately recreating both server and device state together.
4. Test near a known current V3/TTS gateway or a controlled local gateway, not only the observed `ttn-v2-legacy-eu` Packet Broker gateway.
5. Watch TTS Live Data and preserve the correlation ID.
6. If `ns.down.join.schedule.success` and `gs.down.tx.success` appear but the device still returns `-1116`, perform one full SPI + RXEN diagnostic capture.
7. If the device joins, verify an application uplink, then hard-power-cycle and confirm NVS session restoration.
8. Finally test C5 raw V2X frame -> S3 -> LoRaWAN -> TTS -> reassembly bridge -> OpenTrafficMap.
9. Repeat the final validated path under ESP-IDF 6.1 to match CI/reference.

## Evidence files

- [`evidence/LORAWAN_OTAA_2026-08-19.md`](evidence/LORAWAN_OTAA_2026-08-19.md): first protocol-debug capture, erase/DevNonce caveat and later `-1119` mismatch.
- [`evidence/LORAWAN_OTAA_2026-08-19_clean_repro.md`](evidence/LORAWAN_OTAA_2026-08-19_clean_repro.md): post-reprovision trace; later corrected because local RadioLib was still locked to the PR-head commit.
- [`evidence/LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md`](evidence/LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md): first true merged-RadioLib/native-ESP-IDF trace where the then-current TTS device showed no activity.
- [`evidence/LORAWAN_OTAA_2026-08-19_dr0_packetbroker_join_accepted_no_schedule.md`](evidence/LORAWAN_OTAA_2026-08-19_dr0_packetbroker_join_accepted_no_schedule.md): current decisive DR0 capture; TTS accepts the exact JoinRequest via a legacy Packet Broker gateway but no join-downlink scheduling/transmission event is present.

## Source references

- RadioLib issue #1806: https://github.com/jgromes/RadioLib/issues/1806
- RadioLib PR #1811: https://github.com/jgromes/RadioLib/pull/1811
- RadioLib troubleshooting guide: https://github.com/jgromes/RadioLib/wiki/Troubleshooting-Guide
- Seeed Wio-SX1262 datasheet: https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf
- Seeed Wio-SX1262/XIAO schematic: https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Schematic_Diagram_Wio-SX1262_for_XIAO.pdf
- ESP-IDF Component Manager update dependencies: https://docs.espressif.com/projects/idf-component-manager/en/latest/use/how_to_update_dependencies.html
- The Things Stack Packet Broker: https://www.thethingsindustries.com/docs/concepts/packet-broker/
- The Things Stack Events API: https://www.thethingsindustries.com/docs/api/reference/grpc/events/
- The Things Stack gateway/device troubleshooting documentation.

## Close criteria

Do not close this investigation merely because `js.join.accept` appears. Close only after:

1. firmware identity exactly matches the TTS device;
2. intended RadioLib dependency and reference toolchain are used;
3. TTS receives a JoinRequest and a gateway schedules/transmits a JoinAccept;
4. RadioLib reports a new/restored LoRaWAN session;
5. the S3 sends an application uplink;
6. hard power-cycle restores the session without unsafe nonce/counter reuse;
7. the TTS MQTT bridge reconstructs a C-ITS frame and publishes it to OpenTrafficMap.
