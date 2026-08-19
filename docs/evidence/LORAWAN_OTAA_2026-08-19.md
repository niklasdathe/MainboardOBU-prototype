# LoRaWAN OTAA protocol-debug capture — 2026-08-19

This dated evidence note belongs to the running investigation in [`../LORAWAN_OTAA_DEBUG.md`](../LORAWAN_OTAA_DEBUG.md). It records the first BicycleOBU physical capture with structured `ESP_LOG` diagnostics plus RadioLib BASIC/PROTOCOL tracing enabled.

> Later evidence: this first capture used ESP-IDF 6.0.2 and predates the local dependency-lock correction. The first physical build that **actually** resolves RadioLib PR #1811 merge commit `12e3ed6c...` and reports native `Platform: "ESP-IDF"` is documented in [`LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md`](LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md). The current TTS/firmware DevEUI is also now confirmed as `70B3D57ED0078C76`; older `...8C36` evidence is historical.

## Test command and environment

The S3 was flashed from the short Windows clone using:

```powershell
idf.py -C firmware/s3 -p COM13 erase-flash flash monitor
```

Observed toolchain/runtime:

```text
ESP-IDF v6.0.2
RadioLib 7.7.1.0 source state selected by the then-current local dependency lock
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

Earlier TTS evidence for the then-registered device had already shown an accepted LoRaWAN 1.1 JoinRequest with a much higher DevNonce. The Things Stack tracks DevNonce for LoRaWAN 1.1 devices, so this post-erase attempt could not be treated as clean RX-failure evidence unless server/device activation state was reprovisioned consistently.

Do not repeat `erase-flash` during normal OTAA debugging. If NVS is intentionally erased, synchronize/reprovision the matching TTS end-device/Join Server state before the next join attempt.

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

RadioLib successfully read the SX126x version string and initialized the radio. `SPI bus already initialized` and `GPIO isr service already installed` were accepted shared-resource conditions rather than fatal initialization errors.

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

The timing/frequency/data-rate trace is internally coherent, but this particular attempt was contaminated by the local DevNonce rollback caused by `erase-flash`. It cannot isolate the receive path.

## Follow-up `-1119` after reprovision/configuration change

A subsequent normal flash started with persisted local nonce state but different activation configuration and RadioLib reported:

```text
Configuration mismatch
RadioLib rejected saved LoRaWAN state (state=-1119)
```

This is `RADIOLIB_ERR_NONCES_DISCARDED`: the persisted activation checksum does not match current JoinEUI/DevEUI/root keys. BicycleOBU intentionally fails closed instead of silently resetting nonce history.

Recovery is allowed only when server/device state is deliberately synchronized—for example, after deleting/re-registering the TTS device and applying the new matching firmware credentials—then one local erase may be used to remove the previous activation's NVS state. After that, normal flashing must preserve NVS.

## What happened next

The TTS device was recreated and clean post-reprovision tests followed. A later build-log review then found that local `dependencies.lock` had still selected RadioLib PR-head `f0fb0295...` despite the manifest requesting merge commit `12e3ed6c...`.

After `idf.py -C firmware/s3 update-dependencies`, a true merged-state physical build was obtained. In that newer capture:

- RadioLib resolves `12e3ed6c...`;
- RadioLib reports `Platform: "ESP-IDF"`;
- current TTS and firmware DevEUI both equal `70B3D57ED0078C76`;
- local OTAA still ends in `-1116`;
- but TTS Live Data shows no activity for those attempts.

That changes the immediate diagnosis: current gateway/uplink reception must be proven before using `-1116` as evidence about JoinAccept RX. See the merged-state evidence file linked above.

## References

- RadioLib issue #1806: ESP32-S3 + SX1262 JoinAccept/downlink `-1116`, including XIAO ESP32-S3 + Wio-SX1262.
- RadioLib PR #1811: LoRa symbol-timeout receive-window fix.
- RadioLib persistence: activation checksum and fail-closed `RADIOLIB_ERR_NONCES_DISCARDED (-1119)` behavior.
- The Things Stack LoRaWAN 1.1 DevNonce handling.
