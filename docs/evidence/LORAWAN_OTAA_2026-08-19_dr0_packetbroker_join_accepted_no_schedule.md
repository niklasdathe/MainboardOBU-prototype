# LoRaWAN OTAA DR0 / Packet Broker join-accepted capture — 2026-08-19

This note records the first controlled OTAA attempt in which all of the following were simultaneously true:

- current TTS registration and firmware activation identity matched;
- the local RadioLib dependency was the intended PR #1811 merge commit `12e3ed6c4814e177a87a7b2c48ab11dd65788143`;
- RadioLib reported native `Platform: "ESP-IDF"`;
- initial OTAA data rate was deliberately forced to EU868 DR0 / SF12;
- the JoinRequest was actually received by The Things Stack and accepted by the Join Server.

Root keys are intentionally not copied into repository documentation.

## Device / TTS identity for this capture

The test used a newly created development end device with:

```text
JoinEUI: 1111111111111111
DevEUI:  70B3D57ED0078C82
Frequency plan: Europe 863-870 MHz (SF12 for RX2)
LoRaWAN: 1.1.0
Regional Parameters: RP002 1.0.4
```

The firmware menuconfig contained the same JoinEUI and DevEUI. Because the TTS device was freshly recreated, a one-time synchronized `erase-flash` was used before this attempt; the firmware therefore started with fresh nonce/session NVS and transmitted `DevNonce=0`. Do not continue erasing flash after this synchronized reset.

## Local firmware / RadioLib trace

The build resolved:

```text
RadioLib (12e3ed6c4814e177a87a7b2c48ab11dd65788143)
RadioLib Platform: ESP-IDF
```

The local ESP-IDF version for this physical capture was still 6.0.2. CI/reference remains ESP-IDF 6.1.

Startup reported:

```text
LoRaWAN NVS ready: nonce_state=fresh session_state=fresh
RadioLib persistence attached: nonce_state=new session_restored=no
join_dr=0
```

The JoinRequest trace was:

```text
JoinRequest DevNonce=0
JoinEUI=1111111111111111
DevEUI=70B3D57ED0078C82
frequency=868.100 MHz
TX=16 dBm
SF12 / BW125 / CR4/5
ToA=1482 ms
```

Receive windows were internally coherent with the selected EU868 SF12 RX2 plan:

```text
RX1: 868.100 MHz / SF12 / BW125 / inverted IQ
RX2: 869.525 MHz / SF12 / BW125 / inverted IQ
```

Both windows closed without `RxDone`, and RadioLib returned:

```text
RADIOLIB_ERR_NO_JOIN_ACCEPT (-1116)
```

## TTS evidence for the exact same JoinRequest

The exported device Live Data contains the same DevNonce, frequency and data rate:

```text
ns.up.join.process
JoinEUI: 1111111111111111
DevEUI: 70B3D57ED0078C82
DevNonce: 0000
868.100 MHz
SF12 / BW125 / CR4/5
consumed airtime: 1.482752 s
```

The uplink was received through Packet Broker by exactly one forwarding gateway:

```text
forwarder tenant: ttnv2
forwarder cluster: ttn-v2-legacy-eu
gateway EUI: B827EBFFFE61601A
gateway ID: eui-b827ebfffe61601a
RSSI: -119 dBm
SNR: -5.5 dB
```

TTS then emitted:

```text
js.join.accept
ns.up.join.process
as.up.join.forward
```

and allocated a pending DevAddr/session. This proves, for this attempt, that:

1. the SX1262 TX path works at DR0/SF12;
2. the current JoinEUI/DevEUI are correct;
3. the current root keys are accepted by TTS;
4. TTS generated a JoinAccept at the Join Server/Application Server level.

## Crucial missing evidence

The export was created about 26 seconds after the JoinRequest, so it extends well beyond the Class-A join receive-window times. It contains **none** of the expected Network/Gateway Server downlink scheduling/transmission events:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success
ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success
gs.down.tx.fail
```

Therefore this capture does **not** show a JoinAccept being scheduled or transmitted over RF. The device-side `-1116` cannot yet be attributed to the SX1262 receive path.

The active uplink path is unusual enough to matter: the only receiver is a legacy TTN V2 forwarder reached through Packet Broker (`ttnv2` / `ttn-v2-legacy-eu`). Packet Broker supports OTAA join-accept downlinks in principle, but Packet Broker routing policies separately control whether a forwarding network allows join-accept downlinks. The current application export gives no evidence that this particular legacy forwarding path produced a schedulable downlink path.

## Current interpretation

The evidence boundary is now:

```text
XIAO/Wio sends DR0 JoinRequest
        |
        v
legacy TTN-v2 gateway receives it at -119 dBm / SNR -5.5
        |
        v
Packet Broker forwards uplink to TTS
        |
        v
TTS validates keys and creates JoinAccept/pending session
        |
        v
NO ns.down.join.schedule.* or gs.down.tx.* evidence
        |
        +--> downlink path / gateway scheduling is now the primary blocker
```

The result is stronger than the previous `no TTS activity` capture: uplink/gateway reception is now proven. The next discriminating test is not another RX-window parameter change.

## Next tests

1. Preserve NVS from now on; use normal `flash`, not `erase-flash`.
2. Keep DR0/SF12 and BASIC/PROTOCOL diagnostics enabled for the immediate comparison.
3. Repeat the same activation close to a known **current TTS/TTN V3 EU868 gateway**, or preferably a controlled local multi-channel gateway connected directly to the current TTS Gateway Server rather than through `ttn-v2-legacy-eu`.
4. If a direct/current gateway produces `ns.down.join.schedule.success` and `gs.down.tx.success`, compare the exact downlink parameters with RadioLib RX1/RX2.
5. Only if TTS proves the JoinAccept was transmitted and the device still returns `-1116`, enable one full RadioLib SPI trace and instrument GPIO38/RXEN transitions during RX1/RX2.
6. If the device joins through a direct/current gateway, treat the legacy Packet Broker forwarding path as the cause/limitation and do not change the working SX1262 RX configuration.
7. Repeat the final controlled test under ESP-IDF 6.1 to match CI/reference after the gateway-path question is resolved.

## Related sources

- The Things Stack Packet Broker documentation: Packet Broker supports OTAA device activation and join-accept downlinks, subject to routing policies.
- The Things Stack Events API: `ns.down.join.schedule.attempt`, `.success`, and `.fail` are the authoritative Network Server scheduling events for JoinAccept transmission.
- The Things Stack gateway troubleshooting guide: if the Network Server processes joins but no downlink is scheduled, inspect scheduling/gateway events before debugging the end device.
- Historical TTN community reports describe similar cases where joins were processed through legacy/Packet-Broker gateway paths but JoinAccept scheduling depended on the usable gateway/downlink path. These are supporting anecdotes, not authoritative proof for this gateway.
