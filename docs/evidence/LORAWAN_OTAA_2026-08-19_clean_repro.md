# LoRaWAN OTAA clean `-1116` reproduction — 2026-08-19

This evidence note records the first clean post-reprovision OTAA reproduction after synchronizing local NVS and The Things Stack device state. It complements [`../LORAWAN_OTAA_DEBUG.md`](../LORAWAN_OTAA_DEBUG.md) and the earlier [`LORAWAN_OTAA_2026-08-19.md`](LORAWAN_OTAA_2026-08-19.md) capture.

> **Correction after reviewing the subsequent local build log:** although the repository manifest had already been changed to RadioLib merge commit `12e3ed6c4814e177a87a7b2c48ab11dd65788143`, the local ESP-IDF Component Manager lock still resolved RadioLib to the former PR-head commit `f0fb029566c2d58a7373bb66d3a48002a5b56876`. The build explicitly printed `Following dependencies have new versions available` and then `Processing ... RadioLib (f0fb...)`. Therefore this physical capture was **not** produced from the merged RadioLib state. The six-symbol RX-window logic was already present on the PR head, so the trace remains useful, but the merged-state comparison must be repeated only after `idf.py -C firmware/s3 update-dependencies` and a build that explicitly reports `RadioLib (12e3ed6c...)`.
>
> The same trace also exposes a second configuration issue that was initially missed: the JoinRequest contains DevEUI bytes `76 8c 07 d0 7e d5 b3 70` on air, i.e. canonical DevEUI `70B3D57ED0078C76` after LoRaWAN byte-order reversal. An earlier TTS/menuconfig capture showed `70B3D57ED0078C36`. Until the currently registered TTS DevEUI is checked against the on-air `...8C76`, absence of device Live Data must be treated first as a possible identity mismatch rather than as an RF/downlink failure.

## Context

The previous test had deliberately erased flash/NVS, which reset the local DevNonce while the old TTS registration still retained nonce history. The TTS end device was subsequently deleted and recreated. A later boot then detected an old local RadioLib nonce blob that no longer matched the new activation credentials and correctly failed closed with `RADIOLIB_ERR_NONCES_DISCARDED (-1119)`.

For this capture the old local state was erased once in synchronization with the freshly recreated TTS device. The test then started from a clean local activation state.

## Test environment

Observed runtime/toolchain:

```text
ESP-IDF: v6.0.2
ESP32-S3: revision v0.2
RadioLib version string: 7.7.1.0
RadioLib source actually resolved locally: f0fb029566c2d58a7373bb66d3a48002a5b56876
Repository manifest requested: 12e3ed6c4814e177a87a7b2c48ab11dd65788143
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

The raw JoinRequest shows:

```text
00 00 00 00 00 00 00 00 00 76 8c 07 d0 7e d5 b3 70 00 00 ...
```

After the MHDR and all-zero JoinEUI, the DevEUI is carried least-significant-byte first. The canonical on-air DevEUI is therefore:

```text
70B3D57ED0078C76
```

This must be compared with the **current** TTS end-device DevEUI before interpreting an empty device Live Data page. A previously captured registration/menuconfig value was `70B3D57ED0078C36`, differing in the final byte.

Because both the local activation state and the TTS end-device registration were intended to be freshly reprovisioned for this test, DevNonce 0 is valid only if the current TTS registration also matches the transmitted DevEUI/keys.

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

This is a clean local RadioLib OTAA attempt with synchronized local NVS, but two newly identified facts prevent calling it a clean end-to-end TTS reproduction yet:

1. the local build still used RadioLib PR-head `f0fb...`, not the intended merged commit `12e3...`;
2. the transmitted DevEUI is `70B3D57ED0078C76`, which must be reconciled with the currently registered TTS DevEUI before an empty device Live Data page can be interpreted as an RF coverage problem.

## What this capture proves

It proves that:

1. the SX1262 initializes and is accessible over SPI;
2. the firmware reaches a real OTAA attempt;
3. the local persistence layer starts clean and does not reject the current activation configuration;
4. the radio reports a JoinRequest transmission;
5. RadioLib opens internally coherent RX1 and RX2 windows using the expected EU868 parameters;
6. RadioLib does not decode a JoinAccept in either window and returns `-1116`.

It does **not** prove that The Things Stack received or accepted this exact JoinRequest. With no activity in the device Live Data, identity matching and gateway reception are prerequisites before any JoinAccept/RX diagnosis.

## Upstream comparison

RadioLib issue #1806 described the same high-level symptom on ESP32-S3 + SX1262 and explicitly included a Seeed XIAO ESP32-S3 + Wio-SX1262 reproduction. PR #1811 subsequently changed LoRaWAN receive-window handling to use LoRa symbol timeouts. The PR discussion reports that the same XIAO/Wio hardware joined successfully after the fix.

The local capture here used the #1811 **PR head** (`f0fb...`), not the later merge commit (`12e3...`). The symbol-timeout behavior is visible and active, but any additional changes present in the merged state were not tested by this capture. The intended merged-state test remains outstanding.

## RF-switch cross-check

The current Wio configuration uses GPIO38 as MCU-controlled RXEN and leaves MCU TXEN unconnected, while SX1262 DIO2 controls the module's TX side/internal RF switch. This matches current Meshtastic Wio-SX1262 definitions and RadioLib's `setRfSwitchPins(rxEn, txEn)` semantics.

The current structured log samples GPIO38 only before and after the complete `activateOTAA()` call. Both samples being LOW is expected while idle and does not prove that RXEN actually went HIGH during RX1/RX2. If server-side evidence confirms a transmitted JoinAccept and the issue survives the intended dependency/toolchain reproduction, add/enable a non-intrusive timestamped RF-switch transition capture or use the existing full SPI trace for one attempt.

## Next discriminating test

1. Check the current TTS device's DevEUI. It must exactly equal the canonical DevEUI transmitted by firmware. If TTS is still `70B3D57ED0078C36`, correct either TTS or menuconfig so both sides use one value; do not continue RX debugging while they differ.
2. Update the local managed dependency explicitly:

```powershell
idf.py -C firmware/s3 update-dependencies
```

Then rebuild and require this line before treating the test as merged-state evidence:

```text
Processing ... RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
```

3. Prefer ESP-IDF 6.1 to match CI/reference tooling.
4. Do not erase NVS merely for the RadioLib dependency update. If the DevEUI/root activation configuration itself is changed, follow the synchronized TTS/local reprovision procedure because RadioLib correctly rejects nonce state from another activation configuration.
5. Once the TTS identity matches and device Live Data contains the JoinRequest, preserve its correlation ID and inspect:

```text
js.join.accept
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

6. If TTS reports successful JoinAccept scheduling/transmission and the intended RadioLib/IDF build still produces empty RX1/RX2 windows, enable `RLB_SPI` for exactly one join attempt and inspect SX1262 sleep/wake, modulation/packet parameters, IRQ setup and `SetRx` around both windows.
7. At the same point, verify GPIO38/RXEN transitions during RX1/RX2 rather than relying only on before/after snapshots.

## References

- RadioLib issue #1806: ESP32-S3 + SX1262 `NO_JOIN_ACCEPT`, including XIAO ESP32-S3 + Wio-SX1262 reproduction.
- RadioLib PR #1811: LoRa symbol-timeout receive-window fix; discussion reports successful join on the same XIAO/Wio hardware.
- ESP-IDF Component Manager: `idf.py update-dependencies` refreshes dependency resolution/`dependencies.lock`; a newer manifest dependency being merely reported does not mean the locked component was updated.
- RadioLib `Module::setRfSwitchPins(rxEn, txEn)`: RX enable is HIGH during receive, TX enable HIGH during transmit, both LOW while idle.
- Meshtastic XIAO/Wio definitions: Wio RXEN present, TXEN `RADIOLIB_NC`, DIO2 used as RF switch, DIO3 TCXO 1.8 V.
- The Things Stack event chain in `../LORAWAN_OTAA_DEBUG.md` for distinguishing Join Server acceptance from actual gateway downlink scheduling/transmission.