# LoRaWAN OTAA clean `-1116` reproduction — 2026-08-19

This evidence note records the first clean post-reprovision OTAA reproduction after synchronizing local NVS and The Things Stack device state. It complements [`../LORAWAN_OTAA_DEBUG.md`](../LORAWAN_OTAA_DEBUG.md) and the earlier [`LORAWAN_OTAA_2026-08-19.md`](LORAWAN_OTAA_2026-08-19.md) capture.

> Historical qualification: this capture was initially described as using the intended RadioLib PR #1811 merge commit. A later build-log review proved the local `dependencies.lock` still selected former PR-head commit `f0fb0295...`. The first true merged-state physical build is documented in [`LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md`](LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md).

## Context

A previous test deliberately erased flash/NVS, resetting local DevNonce while an older TTS registration still retained nonce history. The TTS end device was subsequently deleted/recreated. A later boot detected an old local RadioLib nonce blob that no longer matched the new activation credentials and correctly failed closed with `RADIOLIB_ERR_NONCES_DISCARDED (-1119)`.

For this capture the old local state had been erased once in synchronization with the freshly recreated TTS device, so the test started from a clean local activation state.

## Test environment

Observed runtime/toolchain:

```text
ESP-IDF: v6.0.2
ESP32-S3: revision v0.2
RadioLib version string: 7.7.1.0
RadioLib source actually selected by local lock: f0fb029566c2d58a7373bb66d3a48002a5b56876
Wio-SX1262: shared SPI1
LoRaWAN diagnostics: structured ESP_LOG + RLB_DBG/RLB_PRO enabled
RLB_SPI: disabled
```

Important: project CI/reference builds use ESP-IDF 6.1. This capture remains useful RF/protocol evidence but is not the final toolchain/dependency-matched reproduction.

## Clean persistence state

Startup reported:

```text
LoRaWAN NVS ready: nonce_state=fresh session_state=fresh namespace=obu_lwan
RadioLib persistence attached: nonce_state=new session_restored=no
```

There was no `Configuration mismatch` and no `-1119`. `beginOTAA()` completed and the radio proceeded to an actual OTAA exchange.

## Radio configuration

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

A fresh LoRaWAN 1.1 JoinRequest was generated with DevNonce 0. Across this diagnostic phase RadioLib selected EU868 default join frequencies and SF9/BW125. One captured attempt used 868.100 MHz / SF9 / BW125 / CR4/5 and completed local TX with about 205 ms time-on-air.

Because both local activation state and the then-current TTS end-device registration were freshly reprovisioned for that test, DevNonce 0 was appropriate.

## RX1 / RX2

RadioLib configured internally coherent Class-A join windows:

```text
RX1: corresponding uplink channel / SF9 / BW125 / inverted IQ
RX2: 869.525 MHz / SF12 / BW125 / inverted IQ
```

Both windows used the PR #1811-style six-symbol timeout behavior and closed without a decoded downlink. `activateOTAA()` returned:

```text
state=-1116 (NO_JOIN_ACCEPT)
```

## What this capture proves

It proves that:

1. the SX1262 initializes and is accessible over SPI;
2. the firmware reaches a real OTAA attempt;
3. the local persistence layer starts clean and does not reject the activation configuration;
4. RadioLib completes a local JoinRequest transmission;
5. RadioLib opens internally coherent EU868 RX1/RX2 windows;
6. no JoinAccept is decoded in those windows.

It does **not** prove that TTS received the exact JoinRequest or that a gateway transmitted a JoinAccept for that attempt.

## Later corrections

### Current DevEUI

One trace decoded to canonical DevEUI `70B3D57ED0078C76`, while an older screenshot had shown `70B3D57ED0078C36`. A later current TTS/menuconfig comparison resolved this: the recreated/current device and firmware both use `70B3D57ED0078C76`. The `...8C36` value was historical and is not the present blocker.

### RadioLib dependency lock

The most important correction is that this capture was **not** built from the intended merge commit `12e3ed6c...`; local `dependencies.lock` still selected `f0fb0295...`. After `idf.py -C firmware/s3 update-dependencies`, the first true merged-state build reported both:

```text
RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
Platform: "ESP-IDF"
```

That later capture is documented separately.

## Upstream comparison

RadioLib issue #1806 described the same high-level symptom on ESP32-S3 + SX1262 and explicitly included Seeed XIAO ESP32-S3 + Wio-SX1262. PR #1811 changed LoRaWAN receive-window handling to LoRa symbol timeouts and reports successful join on the same XIAO/Wio combination.

The six-symbol behavior seen here confirms the relevant receive-window work was already present on PR head `f0fb...`, but this file must not be used as evidence about the merged ESP-IDF HAL state.

## Next evidence

See [`LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md`](LORAWAN_OTAA_2026-08-19_merged_radiolib_no_tts.md) for the later capture where:

- current TTS and firmware DevEUI match;
- the local dependency is actually `12e3ed6c...`;
- RadioLib reports native `Platform: "ESP-IDF"`;
- the device still returns `-1116` locally;
- but TTS Live Data shows no activity, moving the immediate blocker to gateway/uplink reception rather than JoinAccept RX.
