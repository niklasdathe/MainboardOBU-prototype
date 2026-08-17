# ESP32-C5 standalone V2X transmitter

`firmware/c5-tx` is a standalone development firmware for a Seeed Studio XIAO ESP32-C5. It reuses the prototype's `obu_radio` ITS-G5/802.11p driver and can transmit configured ETSI Facilities PDUs periodically, when the XIAO **BOOT** button is pressed, and optionally once at startup.

This target is deliberately separate from `firmware/c5`: the normal C5 firmware remains the S3-controlled radio endpoint, while `c5-tx` is a lab/interoperability traffic source.

## Supported profiles

| Profile | Facilities message ID | BTP destination port | GeoNetworking transport used by this test target |
|---|---:|---:|---|
| CAM | 2 | 2001 | BTP-B / SHB |
| DENM | 1 | 2002 | BTP-B / GeoBroadcast |
| MAPEM | 5 | 2003 | BTP-B / SHB |
| SPATEM | 4 | 2004 | BTP-B / SHB |
| IVIM | 6 | 2006 | BTP-B / SHB |
| CPM | 14 | 2009 | BTP-B / SHB |
| VAM | 16 | 2018 | BTP-B / SHB |

The BTP values follow ETSI TS 103 248 Release 2. VAM uses protocol version 3; the other built-in Release-2 profiles currently guard for protocol version 2. The message bytes themselves are **not invented by the transmitter**: configure a complete, already UPER-encoded Facilities PDU for each profile.

## Build and configure

```bash
idf.py -C firmware/c5-tx set-target esp32c5
idf.py -C firmware/c5-tx menuconfig
idf.py -C firmware/c5-tx build
idf.py -C firmware/c5-tx flash monitor
```

Open **BicycleOBU C5 TX test transmitter** in `menuconfig`.

Important controls:

- `Enable V2X transmission`: global TX gate.
- `ITS-G5 center frequency`: 5860…5900 MHz as supported by the shared prototype radio driver.
- `Send configured button profiles when BOOT is pressed`: XIAO ESP32-C5 BOOT is GPIO28, active low.
- `Enable periodic profile scheduling`: global gate for all per-profile periodic schedules.
- Each profile has an enable, BOOT trigger, periodic trigger, interval, and UPER-hex field.
- `UTC Unix time at boot`: allows the test target to produce a meaningful GeoNetworking timestamp and refresh CAM/VAM `generationDeltaTime`. Leave it at zero only for diagnostic receiver tests.
- `Allow header-only diagnostic probes`: intentionally emits only the six-byte ITS PDU header when no PDU is configured. It is **off by default** because these probes are not valid Facilities messages.

Hex strings may contain whitespace, `:`, `-`, or `_` separators. Do not include `0x` prefixes. The transmitter verifies the configured PDU's `protocolVersion` and `messageId` before sending. It can also patch the four-byte `stationID` in the unsecured PDU header.

## Example workflow

1. Generate a standards-valid CAM, VAM, DENM, CPM, SPATEM, MAPEM, or IVIM with the matching ETSI ASN.1 codec/test tooling.
2. Copy the complete UPER Facilities PDU as hex into the corresponding `menuconfig` field.
3. Set the GN source position, station type, station ID, source MAC and—when timing matters—the UTC boot-time base.
4. Enable that profile for BOOT, periodic transmission, or both.
5. Build and flash the XIAO C5.
6. Press BOOT for an immediate configured burst, or use the per-profile intervals.

For receiver UI/classifier development only, `Allow header-only diagnostic probes` can be enabled. This makes it easy to produce CAM/VAM/DENM/etc. identifiers without maintaining test fixtures, but the resulting packets must never be described as compliant CAM/VAM/DENM messages.

## Conformance boundary

This firmware makes the **test transport/framing configurable and standards-aware**, but a successful build or reception is not complete C-ITS conformance evidence.

Implemented/guarded here:

- IEEE 802.11 data frame + LLC/SNAP GeoNetworking EtherType `0x8947`.
- GeoNetworking Basic/Common headers and source long position vector.
- BTP-B well-known destination ports.
- SHB profiles for awareness/infrastructure test traffic and GeoBroadcast for DENM.
- Release-2 Facilities message IDs and expected PDU protocol versions.
- Optional CAM/VAM `generationDeltaTime` refresh when a valid UTC-at-boot base is supplied.
- Explicit per-boot TX arming through the existing `obu_radio` nonce mechanism.

Not implemented by this standalone test target:

- ETSI TS 103 097 secured-message generation/signing.
- EU C-ITS enrolment, Authorization Tickets, certificate rotation, pseudonym handling, SSP enforcement or PKI lifecycle.
- A full ASN.1 encoder/semantic validator for every Facilities message. The configured UPER body remains the responsibility of the test fixture/tool that produced it.
- Full DCC/access-layer conformance, RF qualification or legal authorization for over-the-air transmission.

BSI TR-03164 Part 2 requires operational C-ITS stations to use the C-ITS PKI/authorization model and to validate message signatures, format, permissions, consistency, timestamps, plausibility and data quality. Therefore this standalone transmitter is a **research/interoperability tool**, not a production or certified C-ITS station.

The shared ESP32-C5 radio driver uses private ESP32-C5 802.11p ROM hooks and already marks RF/HIL qualification as required. Use conducted/shielded RF testing where appropriate and only transmit over the air where permitted.
