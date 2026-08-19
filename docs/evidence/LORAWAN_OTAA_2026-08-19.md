# LoRaWAN OTAA protocol-debug capture — 2026-08-19

This dated evidence note belongs to the running investigation in [`../LORAWAN_OTAA_DEBUG.md`](../LORAWAN_OTAA_DEBUG.md). It records the first BicycleOBU physical capture with structured `ESP_LOG` diagnostics plus RadioLib BASIC/PROTOCOL tracing enabled.

## Test command and environment

The S3 was flashed from the short Windows clone using:

```powershell
idf.py -C firmware/s3 -p COM13 erase-flash flash monitor
```

Observed toolchain/runtime:

```text
ESP-IDF v6.0.2
RadioLib 7.7.1.0 source state pinned by the BicycleOBU component manifest
ESP32-S3 revision v0.2
Wio-SX1262 on shared SPI1
```

Important: the project CI/reference toolchain is ESP-IDF 6.1. This capture therefore records real hardware behavior but must not be treated as the final toolchain-matched reproduction.

## Flash/NVS consequence

`erase-flash` erased the NVS partition. Startup consequently reported:

```text
LoRaWAN NVS ready: nonce_state=fresh session_state=fresh
RadioLib persistence attached: nonce_state=new session_restored=no
```

RadioLib then created:

```text
JoinRequest (DevNonce = 0)
```

Earlier TTS evidence for this same registered device had already shown an accepted LoRaWAN 1.1 JoinRequest with DevNonce `0x0040`. The Things Stack stores `last_dev_nonce` for LoRaWAN 1.1 devices and rejects a same-or-lower DevNonce as too small. Therefore this post-erase attempt cannot be used as clean evidence of a device RX failure unless the Join Server/device registration was also deliberately reprovisioned/reset consistently.

Do not repeat `erase-flash` during normal OTAA debugging. If NVS is intentionally erased, reprovision the matching TTS end-device/Join Server state before the next join attempt.

## Radio initialization evidence

The diagnostic build reported:

```text
SPI1 SCK=7 MISO=8 MOSI=9 NSS=41
DIO1=39 RESET=42 BUSY=40
RXEN=38 TXEN=NC
DIO2_RF_SWITCH=yes
TCXO=1.8V
FreeRTOS_Hz=1000
```

RadioLib successfully read the SX126x version string and initialized the radio. The `SX1261 V2D 2D02` register string is also seen in published RadioLib SX1262 traces and is not, by itself, evidence that the board contains the wrong radio.

ESP-IDF printed `SPI bus already initialized` and `GPIO isr service already installed`. The pinned RadioLib `EspHal` explicitly accepts these two conditions for a shared SPI/application-wide ISR setup; radio initialization continued successfully.

## JoinRequest and receive-window trace

RadioLib protocol tracing showed:

```text
JoinRequest (DevNonce = 0)
Frequency = 868.300 MHz, TX = 16 dBm
[LoRa] SF = 9, BW = 125.000 kHz, CR = 4/5, IQ: U
Uplink sent (ToA = 205 ms)
```

RX1:

```text
Frequency = 868.300 MHz
[LoRa] SF = 9, BW = 125.000 kHz, CR = 4/5, IQ: D
Rx1 window open (timeout: 6 symbols / 47651 ticks + 0ms)
Rx1 window closed
```

RX2:

```text
Frequency = 869.525 MHz
[LoRa] SF = 12, BW = 125.000 kHz, CR = 4/5, IQ: D
Rx2 window open (timeout: 6 symbols / 196660 ticks + 0ms)
Rx2 window closed
```

`activateOTAA()` returned:

```text
state=-1116 (NO_JOIN_ACCEPT)
elapsed=6638 ms
activated=no
```

The timing in this capture is internally coherent: RX1 opens approximately five seconds after the JoinRequest uplink finishes and RX2 approximately one second after RX1. The EU868 frequencies/data rates and downlink IQ inversion shown by the trace are also plausible/default LoRaWAN join-window values. This substantially narrows the device-side problem: there is no obvious gross RX1/RX2 frequency, spreading-factor, IQ or join-delay misconfiguration in this capture.

However, because NVS was erased and DevNonce rolled back to zero, this particular `-1116` must not be attributed to the RF receive path until TTS Live Data from the same correlation ID proves that the Join Server accepted DevNonce 0 and that a gateway downlink was actually scheduled/transmitted.

## Follow-up after device reprovision / configuration change

A subsequent normal flash (without another erase) started with:

```text
LoRaWAN NVS ready: nonce_state=present session_state=fresh
RLB_PRO: Configuration mismatch (key checksum: AA23E8D5, got: 0A39F08B)
RadioLib rejected saved LoRaWAN state (state=-1119)
```

RadioLib defines `-1119` as `RADIOLIB_ERR_NONCES_DISCARDED`: the persisted Nonces buffer does not match the current activation configuration. RadioLib stores an activation checksum derived from JoinEUI, DevEUI, AppKey and NwkKey in the persistence buffer and rejects the saved buffer when the current credentials do not match. This is expected when root credentials or EUIs are changed while an older nonce blob remains in NVS.

BicycleOBU intentionally treats this as fail-closed. It does **not** silently discard the nonce history because doing so could cause unsafe DevNonce reuse against an existing Join Server registration.

Recovery depends on server state:

- If the TTS end device was deliberately deleted/re-registered/reprovisioned and the firmware now contains that fresh registration's matching credentials, the old local nonce blob belongs to the previous activation configuration. In that specific synchronized reprovisioning case, erase the local NVS/flash once, flash the new matching configuration, and then stop erasing NVS for subsequent tests.
- If the TTS registration was **not** freshly reprovisioned, do not erase local persistence merely to bypass `-1119`; first restore a consistent device/server activation state.

This `-1119` event occurs before any OTAA RF exchange and is therefore separate from the unresolved `-1116` JoinAccept receive investigation.

## Next clean test

1. Restore server/device nonce consistency. Safest development path after the full flash erase is to delete/re-register (reprovision) the TTS end device so Join Server nonce history starts consistently with the erased device. Preserve matching JoinEUI/DevEUI/root-key configuration; rotate root keys if desired and update firmware accordingly.
2. If credentials were changed during that reprovisioning and the device still contains an old `nonce_state=present` blob, perform the one synchronized local erase described above.
3. Use ESP-IDF 6.1 to match CI/reference builds.
4. Flash normally after synchronization; do **not** erase NVS again.
5. Leave structured LoRaWAN diagnostics and `RLB_DBG/RLB_PRO` enabled; leave `RLB_SPI` disabled initially.
6. Capture the TTS events for the same JoinRequest, especially `ns.up.join.process`, Join Server acceptance/rejection details, `ns.down.join.schedule.*`, and `gs.down.*`/TX acknowledgement events when visible.
7. If TTS proves a JoinAccept was scheduled/transmitted and RadioLib still closes both windows empty, enable full `RLB_SPI` for one attempt and compare the command sequence around RX1/RX2 against RadioLib issue #1806 / PR #1811.

## References

- RadioLib issue #1806: ESP32-S3 + SX1262 JoinAccept/downlink `-1116`, including reproduction on XIAO ESP32-S3 + Wio-SX1262.
- RadioLib PR #1811: LoRa symbol-timeout receive-window fix, tested ESP32 + SX1262 + TTN.
- RadioLib LoRaWAN persistence implementation: the Nonces buffer stores an activation checksum and rejects a buffer when keys/mode/plan do not match the current configuration (`RADIOLIB_ERR_NONCES_DISCARDED`, `-1119`).
- The Things Stack device troubleshooting: LoRaWAN 1.1 DevNonce must increase; same/lower values can be rejected as `DevNonce is too small`.
- The Things Stack End Device API: `last_dev_nonce` is stored in the Join Server for LoRaWAN 1.1+ devices.
