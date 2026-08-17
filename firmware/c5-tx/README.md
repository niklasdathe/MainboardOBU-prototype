# ESP32-C5 standalone V2X transmitter

`firmware/c5-tx` is a standalone development firmware for a Seeed Studio XIAO ESP32-C5. It reuses the prototype's `obu_radio` ITS-G5/802.11p driver and can transmit ETSI Facilities traffic periodically, when the XIAO **BOOT** button is pressed, and optionally once at startup.

This target is deliberately separate from `firmware/c5`: the normal C5 firmware remains the S3-controlled radio endpoint, while `c5-tx` is a lab/interoperability traffic source.

## Supported profiles

| Profile | Facilities message ID | BTP destination port | GeoNetworking transport used by this test target | Facilities source |
|---|---:|---:|---|---|
| CAM | 2 | 2001 | BTP-B / SHB | complete UPER hex |
| DENM | 1 | 2002 | BTP-B / GeoBroadcast | complete UPER hex |
| MAPEM | 5 | 2003 | BTP-B / SHB | complete UPER hex |
| SPATEM | 4 | 2004 | BTP-B / SHB | complete UPER hex |
| IVIM | 6 | 2006 | BTP-B / SHB | complete UPER hex |
| CPM | 14 | 2009 | BTP-B / SHB | complete UPER hex |
| VAM | 16 | 2018 | BTP-B / SHB | **built-in generator (default)** or complete UPER hex |

The BTP values follow ETSI TS 103 248 Release 2. The generated VAM uses protocol version 3 and message ID 16.

## Build and configure

```bash
idf.py -C firmware/c5-tx set-target esp32c5
idf.py -C firmware/c5-tx menuconfig
idf.py -C firmware/c5-tx build
idf.py -C firmware/c5-tx flash monitor
```

Open **BicycleOBU C5 TX test transmitter** in `menuconfig`.

Important global controls:

- `Enable V2X transmission`: global TX gate.
- `ITS-G5 center frequency`: 5860…5900 MHz as supported by the shared prototype radio driver.
- `Send configured button profiles when BOOT is pressed`: XIAO ESP32-C5 BOOT is GPIO28, active low.
- `Enable periodic profile scheduling`: global gate for all per-profile periodic schedules.
- `Station type`, `Source latitude`, `Source longitude`, `Source speed` and `Source heading` are shared between the generated VAM and the GeoNetworking source position vector, so both layers describe the same configured state.
- `UTC Unix time at boot` plus `Leap seconds since the ITS epoch` produce the elapsed ITS/TAI time used for GeoNetworking timestamps and VAM `generationDeltaTime`. The current default leap-second count is 5 (leap seconds introduced after 2004-01-01 through 2016). If UTC-at-boot is zero, the firmware falls back to uptime and explicitly marks the run as unsuitable for timing-conformance evidence.
- `Allow header-only diagnostic probes` remains off by default. Header-only probes are not valid Facilities messages.

## Built-in VAM generator

Under **VAM profile**, leave **VAM Facilities PDU source → Generate a minimal ETSI VAM from menuconfig fields** selected. No `VAM_PDU_HEX` is needed.

Each VAM is regenerated immediately before transmission. The generator emits a 34-byte UPER Facilities PDU containing:

- `ItsPduHeaderVam`: `protocolVersion = 3`, `messageId = 16`, configured `stationId`;
- `generationDeltaTime`;
- mandatory `BasicContainer`: configured `StationType` and `ReferencePosition`;
- mandatory `VruHighFrequencyContainer`: configured heading, speed and longitudinal acceleration;
- no VAM extension additions and no optional low-frequency, cluster or motion-prediction containers in this minimal fixture.

The configurable VAM-specific fields are position-confidence ellipse values, altitude and altitude confidence, heading confidence, speed confidence, longitudinal acceleration and its confidence. Defaults use the standardized `unavailable` values where suitable rather than inventing measurement confidence. Heading `3600` and longitude `-1800000000` are deliberately rejected because those CDD values are prohibited; the corresponding standardized `unavailable` values remain usable where the data element permits them.

For a cyclist test station, use `Station type = 2`. Enable `VAM profile → Send VAM on BOOT trigger`, disable the BOOT trigger for other profiles if you want exactly one frame per button press, then build/flash and press BOOT. A successful send is logged as:

```text
TX VAM reason=BOOT frame=...B pdu=34B BTP=2018 GN=SHB ok=1 failed=0
```

To test an externally encoded VAM instead, select **Use manually supplied complete VAM UPER hex** and paste the complete Facilities PDU into the manual VAM field. Manual CAM/DENM/CPM/SPATEM/MAPEM/IVIM profiles continue to accept complete UPER hex strings. Hex strings may contain whitespace, `:`, `-`, or `_` separators; do not include `0x` prefixes.

## Encoder regression test

The VAM encoder has a deterministic host-side UPER vector test in `firmware/c5-tx/tests/vam_encoder_test.c`. CI compiles it with warnings-as-errors, checks the exact 34-byte encoded vector and verifies that prohibited CDD sentinel values are rejected before the ESP32-C5 firmware is built.

## Conformance boundary

The built-in generator produces a structurally complete UPER VAM Facilities PDU for the implemented minimal root-container subset. This is substantially different from the optional header-only classifier probe, which remains explicitly non-conformant. A successful encoded/transmitted VAM is still **not** complete C-ITS station or VAM-service conformance evidence.

Implemented/guarded here:

- IEEE 802.11 data frame + LLC/SNAP GeoNetworking EtherType `0x8947`;
- GeoNetworking Basic/Common headers and source long position vector;
- BTP-B well-known destination ports;
- SHB for VAM and the other awareness/infrastructure test profiles, GeoBroadcast for DENM;
- Release-2 Facilities message IDs and expected PDU protocol versions;
- built-in minimal VAM UPER encoding with mandatory Basic and VRU high-frequency containers;
- configurable ITS/TAI time base and regenerated VAM `generationDeltaTime`;
- explicit per-boot TX arming through the existing `obu_radio` nonce mechanism.

Not implemented by this standalone test target:

- the complete ETSI TS 103 300-3 VAM Basic Service state machine and all dissemination/container-inclusion rules (for example periodic/conditional low-frequency-container inclusion);
- ETSI TS 103 097 secured-message generation/signing;
- EU C-ITS enrolment, Authorization Tickets, certificate rotation, pseudonym handling, SSP enforcement or PKI lifecycle;
- a general ASN.1 encoder/semantic validator for every Facilities message or every optional VAM container;
- full DCC/access-layer conformance, RF qualification or legal authorization for over-the-air transmission.

BSI TR-03164 Part 2 requires operational C-ITS stations to use the C-ITS PKI/authorization model and to validate message signatures, format, permissions, consistency, timestamps, plausibility and data quality. Therefore this standalone transmitter remains a **research/interoperability tool**, not a production or certified C-ITS station.

The shared ESP32-C5 radio driver uses private ESP32-C5 802.11p ROM hooks and already marks RF/HIL qualification as required. Use conducted/shielded RF testing where appropriate and only transmit over the air where permitted.
