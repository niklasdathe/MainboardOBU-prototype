# LoRaWAN gateway-path mapping — 2026-08-19

This note records the Packet Broker gateway lookup performed after the DR0/SF12 JoinRequest was accepted by TTS but no JoinAccept scheduling/transmission events were present.

## Legacy gateway that received the DR0 JoinRequest

Packet Broker mapper lookup for `eui-b827ebfffe61601a` returned:

```text
netID: 000013
tenantID: ttnv2
clusterID: ttn-v2-legacy-eu
EUI: B827EBFFFE61601A
online: true
location: 53.57058583, 9.97730667
frequency plan: EU_863_870
channels: 868.1, 868.3, 868.5, 867.1, 867.3, 867.5, 867.7, 867.9 MHz
rxRate: 5259.6123
txRate: 6.1016383
```

This confirms that the gateway is currently online and is capable of transmitting traffic in general. It does **not** prove that the current Packet Broker route allows/schedules OTAA JoinAccept downlinks for this device. The preceding TTS export still contains no `ns.down.join.schedule.*` or `gs.down.tx.*` events.

## Current TTN/TTS gateways found around the same area

A Packet Broker mapper query centered on the legacy gateway location, filtered to `netID=000013`, `tenantID=ttn`, `online=true`, radius 5 km, returned multiple gateways on `eu1.cloud.thethings.network`.

Closest candidates to the legacy gateway position (straight-line approximate distance):

| Gateway | EUI | Placement | Coordinates | Approx. distance |
|---|---|---|---|---:|
| `grindelberg` | `A84041FFFF21D5B4` | indoor | 53.5761665, 9.9775569 | 0.62 km |
| `dus-luchsbau-1` | `503139534B594750` | indoor | 53.56182985, 9.98634696 | 1.14 km |
| `lobaro-fruchtallee` | `A84041FFFE1D0358` | indoor | 53.57168178, 9.95254040 | 1.64 km |
| `eui-a84041fdfe27e972` | `A84041FDFE27E972` | indoor | 53.55561763, 9.97631455 | 1.67 km |
| `gw055` | `0016C001F118D3CF` | unspecified | 53.55403117, 9.98754591 | 1.96 km |

A farther but explicitly outdoor candidate was also returned:

```text
hafenmeister-port2ttn-ng
EUI: 50313953530A4750
cluster: eu1.cloud.thethings.network
placement: OUTDOOR
coordinates: 53.55484431, 9.92155427
```

## Next discriminating test

Keep the firmware unchanged at DR0/SF12 and preserve NVS. Do not use `erase-flash` again.

Physically repeat OTAA close to one of the current `tenantID=ttn` / `eu1.cloud.thethings.network` gateways, preferably `grindelberg` first because it is the closest mapped current gateway to the previous receiver location. Because most nearby candidates are marked indoor, testing very close to the listed coordinate is preferable to assuming kilometer-scale urban coverage.

Watch TTS Live Data and preserve the correlation ID. The decisive events remain:

```text
ns.down.join.schedule.attempt
ns.down.join.schedule.success | ns.down.join.schedule.fail
gs.down.send
gs.down.tx.success | gs.down.tx.fail
```

If the device joins through a current gateway, the legacy `ttnv2` Packet Broker path is the likely limitation. If a current gateway produces `schedule.success` and `gs.down.tx.success` but the device still returns `-1116`, move to one full RadioLib SPI trace plus GPIO38/RXEN transition instrumentation.
