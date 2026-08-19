# LoRaWAN OTAA merged-RadioLib / no-TTS-reception capture — 2026-08-19

This note records the first physical capture after the local ESP-IDF Component Manager lock was actually updated to the intended RadioLib PR #1811 merge commit. It supersedes the earlier assumption that the on-air DevEUI differed from the current TTS registration.

## Current TTS registration

The current recreated end device is configured as:

```text
End device ID: bicycleobu-mainboard
Frequency plan: Europe 863-870 MHz (SF12 for RX2)
LoRaWAN version: 1.1.0
Regional Parameters: RP002 1.0.4
JoinEUI: 0000000000000000
DevEUI: 70B3D57ED0078C76
```

The current firmware `menuconfig` also contains `DevEUI=70B3D57ED0078C76` and the same JoinEUI. The visible prefixes of the masked TTS root keys match the firmware values; root keys are intentionally not copied into this repository.

Therefore the earlier `...8C76` versus `...8C36` identity discrepancy was historical: the current TTS device and current firmware identity now match.

## RadioLib dependency is now the intended merged state

The local dependency lock was updated with:

```powershell
idf.py -C firmware/s3 update-dependencies
idf.py -C firmware/s3 reconfigure
```

The build then explicitly resolved:

```text
RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
```

and the compiler/runtime RadioLib banner changed from `Platform: "Generic"` to:

```text
Platform: "ESP-IDF"
```

This is the first physical build in the investigation that can legitimately be treated as using the intended PR #1811 merge state and native ESP-IDF HAL.

The build still used ESP-IDF 6.0.2. Project CI/reference is ESP-IDF 6.1, so a toolchain-matched physical test remains outstanding.

## Build warnings

The merged RadioLib build reports three `-Wunused-variable` warnings in `LoRaWAN.cpp` (`modem`, `tOpen`, `mod`). They are upstream warnings in the pinned RadioLib source and do not stop the build. The firmware image is produced successfully.

The recurring ESP-IDF 6.0.2 Kconfig notes for NimBLE/FATFS are also unrelated to the RF problem.

## Physical OTAA attempt

Startup reported restored nonce state and no restored session:

```text
LoRaWAN NVS ready: nonce_state=present session_state=fresh
RadioLib persistence attached: nonce_state=restored session_restored=no
```

The Wio-SX1262 initialized successfully with:

```text
SCK=7 MISO=8 MOSI=9 NSS=41
DIO1=39 RESET=42 BUSY=40
RXEN=38 TXEN=NC
DIO2 RF switch enabled
TCXO=1.8 V
RadioLib Platform=ESP-IDF
```

The JoinRequest was:

```text
DevNonce = 14
canonical DevEUI = 70B3D57ED0078C76
frequency = 868.500 MHz
TX = 16 dBm
SF9 / BW125 / CR4/5
ToA = 205 ms
```

RadioLib then opened:

```text
RX1: 868.500 MHz / SF9 / BW125 / inverted IQ
RX2: 869.525 MHz / SF12 / BW125 / inverted IQ
```

and returned:

```text
RADIOLIB_ERR_NO_JOIN_ACCEPT (-1116)
```

## Crucial server-side observation

At the time of these merged-state attempts, the current TTS end-device Live Data dashboard showed **no activity at all**.

Because the current TTS DevEUI/JoinEUI match the on-air JoinRequest, this is no longer explained by an identity mismatch. `RLB_PRO: Uplink sent` only means the SX1262 completed its local transmit operation; it does not prove any gateway received the packet.

Therefore this capture is **not** evidence that PR #1811 still fails to receive a TTS JoinAccept. There is no server-side evidence that TTS received this JoinRequest or generated a JoinAccept for this attempt.

The immediate blocker has moved upstream to current RF uplink/gateway reception.

## Historical contrast

Earlier firmware attempts were received by public TTN gateways and accepted by TTS. Those captures included gateway RSSI values around -109 to -113 dBm and at least one weak SNR observation. The current absence of all TTS events is therefore compatible with public-gateway availability, changed RF conditions/location, antenna/connector state, or an uplink-side radio-path difference.

Known-good historical server reception does prove the Wio hardware has transmitted receivable LoRaWAN frames before, but does not prove a public gateway is currently online/in range.

## Hardware configuration cross-check

Seeed's Wio-SX1262 datasheet specifies:

- DIO2 controls TX/RX selection internally (HIGH = TX, LOW = RX);
- DIO3 powers the TCXO;
- TCXO supply range is 1.7–3.3 V and must stay at least 200 mV below VCC;
- the module uses a 3.3 V typical supply and supports EU868.

The current 1.8 V DIO3 setting is therefore electrically valid. Current Meshtastic XIAO/Wio support also uses DIO3 TCXO 1.8 V, GPIO38 RXEN, TXEN NC, and DIO2 RF-switch control.

## Follow-up diagnostic added after this capture

The firmware now exposes a configurable initial OTAA JoinRequest data rate under:

```text
BicycleOBU prototype
  -> LoRaWAN uplink
     -> Activation and network
        -> Initial OTAA uplink data rate (EU868 DR0..DR5)
```

EU868 mapping used by the option is:

```text
DR0 = SF12/BW125
DR1 = SF11/BW125
DR2 = SF10/BW125
DR3 = SF9/BW125
DR4 = SF8/BW125
DR5 = SF7/BW125
```

Default is DR3 to preserve the previous behavior. DR0 is intended as the next coverage diagnostic because it maximizes receiver link budget at the cost of much higher airtime. RadioLib `setDatarate()` is called before activation while ADR is temporarily disabled; ADR is then re-enabled for normal network-controlled operation. A restored valid session is not overwritten by the diagnostic join-DR setting.

## Next discriminating tests

1. Do **not** erase NVS. The local DevNonce state is valid and should continue advancing.
2. Keep the current matching TTS registration and root keys.
3. Set the initial OTAA data rate to **DR0** for one coverage diagnostic and verify `RLB_PRO` shows an SF12 JoinRequest.
4. Test physically much closer to a known active TTN EU868 gateway, or preferably against a controlled local multi-channel gateway.
5. Watch TTS Live Data. Only when a JoinRequest appears should the investigation return to `js.join.accept`, `ns.down.join.schedule.*`, and `gs.down.tx.*`.
6. If TTS proves a JoinAccept was transmitted and the merged RadioLib build still closes RX1/RX2 empty, perform one full `RLB_SPI` capture plus GPIO38/RXEN transition measurement.
7. Repeat the final comparison under ESP-IDF 6.1 to match CI/reference tooling.

## Interpretation boundary

Current evidence supports:

```text
SX1262 local TX completes
        |
        v
no TTS device event observed
        |
        +--> gateway reception / current RF uplink path must be proven first
```

It does not yet support:

```text
TTS transmitted JoinAccept -> Wio failed to receive it
```

for this merged-RadioLib attempt.
