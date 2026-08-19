# LoRaWAN OTAA clean `-1116` reproduction — 2026-08-19

This evidence note records the first clean post-reprovision OTAA reproduction after synchronizing local NVS and The Things Stack device state. It complements [`../LORAWAN_OTAA_DEBUG.md`](../LORAWAN_OTAA_DEBUG.md) and the earlier [`LORAWAN_OTAA_2026-08-19.md`](LORAWAN_OTAA_2026-08-19.md) capture.

## Context

The previous test had deliberately erased flash/NVS, which reset the local DevNonce while the old TTS registration still retained nonce history. The TTS end device was subsequently deleted and recreated. A later boot then detected an old local RadioLib nonce blob that no longer matched the new activation credentials and correctly failed closed with `RADIOLIB_ERR_NONCES_DISCARDED (-1119)`.

For this capture the old local state was erased once in synchronization with the freshly recreated TTS device. The test then started from a clean local activation state.

## Test environment

Observed runtime/toolchain:

```text
ESP-IDF: v6.0.2
ESP32-S3: revision v0.2
RadioLib version string: 7.7.1.0
RadioLib source state: project-pinned merge commit for PR #1811
Wio-SX1262: shared SPI1
LoRaWAN diagnostics: structured ESP_LOG + RLB_DBG/RLB_PRO enabled
RLB_SPI: disabled
```

Important: project CI/reference builds use ESP-IDF 6.1. This capture is physically valid evidence but is not yet the final toolchain-matched reproduction.

## Clean persistence state

Startup reported:

```text
LoRaWAN NVS ready: nonce_state=fresh session_state=fresh namespace=obu_lwan
RadioLib persistence attached: nonce_state=new session_restored=no
```

There was no `Configuration mismatch` and no `-1119`. `beginOTAA()` completed and the radio proceeded to an actual OTAA exchange.

## Radio configuration

The diagnostic build reported:

```text
SPI1 SCK=7 MISO=8 MOSI=9 NSS=41
DIO1=39 RESET=42 BUSY=40
RXEN=38 TXEN=NC
DIO2_RF_SWITCH=yes
TCXO=1.8V
FreeRTOS_Hz=1000
```

SX126x identification succeeded and the version-string register was readable.

## JoinRequest

RadioLib generated a fresh LoRaWAN 1.1 JoinRequest:

```text
JoinRequest (DevNonce = 0)
Frequency = 868.100 MHz, TX = 16 dBm
SF9 / BW125 / CR4/5 / uplink IQ
Uplink sent (ToA = 205 ms)
```

Because both the local activation state and the TTS end-device registration were freshly reprovisioned for this test, DevNonce 0 is valid for this clean attempt.

## RX1

RadioLib configured:

```text
Frequency = 868.100 MHz
SF9 / BW125 / CR4/5 / downlink IQ
Rx1 window open (timeout: 6 symbols / 47651 ticks + 0ms)
Rx1 window closed
```

The window opened approximately five seconds after the JoinRequest transmission, consistent with the LoRaWAN OTAA join receive delay.

## RX2

RadioLib configured:

```text
Frequency = 869.525 MHz
SF12 / BW125 / CR4/5 / downlink IQ
Rx2 window open (timeout: 6 symbols / 196660 ticks + 0ms)
Rx2 window closed
```

The RX2 frequency/data rate match the `Europe 863-870 MHz (SF12 for RX2)` diagnostic registration choice and RadioLib's EU868 pre-join defaults.

## Result

No DIO1/RxDone event was observed by the protocol path in either receive window. `activateOTAA()` returned:

```text
state=-1116 (NO_JOIN_ACCEPT)
elapsed=6626 ms
activated=no
next_uplink=19875 ms
```

This is now a clean device-side reproduction of `RADIOLIB_ERR_NO_JOIN_ACCEPT (-1116)` with synchronized fresh activation state. Unlike the earlier post-erase capture, this result is not contaminated by stale DevNonce history or an activation-checksum mismatch.

## What this capture proves

It proves that:

1. the SX1262 initializes and is accessible over SPI;
2. the firmware reaches a real OTAA attempt;
3. the local persistence layer starts clean and does not reject the current activation configuration;
4. the JoinRequest is transmitted;
5. RadioLib opens internally coherent RX1 and RX2 windows using the expected EU868 parameters;
6. RadioLib does not decode a JoinAccept in either window and returns `-1116`.

It does **not** prove that The Things Stack accepted this exact JoinRequest or that a gateway actually transmitted a JoinAccept for this exact correlation ID. The server-side events from the same attempt are still required before attributing the failure to the Wio-SX1262 receive path.

## Upstream comparison

RadioLib issue #1806 described the same high-level symptom on ESP32-S3 + SX1262 and explicitly included a Seeed XIAO ESP32-S3 + Wio-SX1262 reproduction. PR #1811 subsequently changed LoRaWAN receive-window handling to use LoRa symbol timeouts. The PR discussion reports that the same XIAO/Wio hardware joined successfully after the fix. BicycleOBU pins the merged PR #1811 state, and this trace confirms the new six-symbol receive-window behavior is active.

Therefore this clean failure should not simply be labeled as the already-fixed #1806 regression without further evidence. The remaining differences include the native ESP-IDF integration/toolchain, gateway/downlink path, RF-switch behavior during the windows, and possibly a lower-level SX1262 command/state difference.

## RF-switch cross-check

The current Wio configuration uses GPIO38 as MCU-controlled RXEN and leaves MCU TXEN unconnected, while SX1262 DIO2 controls the module's TX side/internal RF switch. This matches current Meshtastic Wio-SX1262 definitions and RadioLib's `setRfSwitchPins(rxEn, txEn)` semantics.

The current structured log samples GPIO38 only before and after the complete `activateOTAA()` call. Both samples being LOW is expected while idle and does not prove that RXEN actually went HIGH during RX1/RX2. If server-side evidence confirms a transmitted JoinAccept and the issue survives the ESP-IDF 6.1 reproduction, add/enable a non-intrusive timestamped RF-switch transition capture or use the existing full SPI trace for one attempt.

## Next discriminating test

1. Export TTS Live Data for this exact DevNonce 0 JoinRequest and preserve its correlation ID.
2. Confirm whether it contains `js.join.accept`.
3. Find the same-correlation downlink path:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

4. Rebuild and repeat under ESP-IDF 6.1 to match CI/reference tooling. Do not erase NVS again.
5. If TTS reports successful JoinAccept scheduling/transmission and IDF 6.1 still produces the same empty RX1/RX2 windows, enable `RLB_SPI` for exactly one join attempt and inspect SX1262 sleep/wake, modulation/packet parameters, IRQ setup and `SetRx` around both windows.
6. At the same point, verify GPIO38/RXEN transitions during RX1/RX2 rather than relying only on before/after snapshots.

## References

- RadioLib issue #1806: ESP32-S3 + SX1262 `NO_JOIN_ACCEPT`, including XIAO ESP32-S3 + Wio-SX1262 reproduction.
- RadioLib PR #1811: LoRa symbol-timeout receive-window fix; discussion reports successful join on the same XIAO/Wio hardware.
- RadioLib `Module::setRfSwitchPins(rxEn, txEn)`: RX enable is HIGH during receive, TX enable HIGH during transmit, both LOW while idle.
- Meshtastic XIAO/Wio definitions: Wio RXEN present, TXEN `RADIOLIB_NC`, DIO2 used as RF switch, DIO3 TCXO 1.8 V.
- The Things Stack event chain in `../LORAWAN_OTAA_DEBUG.md` for distinguishing Join Server acceptance from actual gateway downlink scheduling/transmission.
