# MainboardOBU Prototype Firmware

ESP-IDF firmware for the BicycleOBU prototype mainboard. The repository is intentionally split into reusable components: the ESP32-C5 is a dedicated ITS-G5 radio endpoint and the ESP32-S3 is the bicycle hub/orchestrator.

## Scope and compliance stance

This implementation follows `BicycleOBU_Requirements_v1_1_ETSI_VITS_S.xlsx` as the governing architecture. It implements the prototype functions that can be implemented with the selected hardware while **refusing to claim ETSI VAM conformance when a required backend or acceptance gate is absent**.

Implemented now:

- ESP32-C5 5.9 GHz ITS-G5 receive backend using isolated ESP32-C5 802.11p ROM hooks.
- Raw frame forwarding, capture metadata, bounded queues, drop counters, C5 health/status, channel selection, raw TX request/result handling and boot-session TX arming.
- Versioned CRC-protected C5↔S3 SPI application protocol; S3 is master, C5 is slave.
- S3 canonical event model with source/provenance/acquisition timestamps, C5↔S3 clock correlation and bounded fan-out queues.
- Expansion Base SSD1306 OLED through a hardware-neutral HMI model.
- L76K GNSS UART/NMEA acquisition, raw NMEA and parsed fixes, PPS input, PCF8563 RTC holdover and monotonic/UTC time service.
- Expansion Base microSD diagnostic/fault logging with bounded rotating files.
- Optional, explicit-opt-in OpenTrafficMap live uploader from the S3 over Wi-Fi/MQTT TLS. Frames are never replayed after an outage.
- Interfaces for CAN, BLE phone/sensors, LDM, ETSI VAM codec, PoTi, security, DCC/GeoNetworking/BTP and alternate uplinks such as LoRaWAN.
- Build-time YAML hardware manifests plus a pin-conflict checker.

Not falsely claimed as complete/conformant:

- ETSI TS 103 300-3 UPER VAM encoding is behind `obu_vam_codec_t` and has no production codec in this repository yet.
- ETSI TS 103 097 signing/credential management is behind `obu_security_backend_t` and is not implemented by the prototype.
- GeoNetworking/BTP/DCC and production PoTi are explicit interfaces/gates, not mocked.
- The L76K prototype path does not satisfy the workbook's <0.5 m full-conformance positioning gate.
- 5.9 GHz TX remains experimental and must be enabled only in a lawful test environment. The C5 comes up receive-only after every reset.

## Repository layout

```text
firmware/c5/           ESP32-C5 application
firmware/s3/           ESP32-S3 hub application
components/obu_core/   canonical event/data model + bounded event bus
components/obu_ipc/    versioned SPI protocol and ESP-IDF SPI endpoints
components/obu_radio/  radio interface + ESP32-C5 ITS-G5 backend
components/obu_hmi/    abstract HMI model + SSD1306 renderer
components/obu_time/   monotonic/UTC correlation + PCF8563 RTC driver
components/obu_gnss/   GNSS interface + L76K NMEA UART driver
components/obu_log/    rotating SD diagnostic logger
components/obu_otm/    live OTM publisher + uplink boundary
components/obu_ifaces/ interfaces required by the architecture but not yet implemented
config/hardware/       human-readable hardware manifests
scripts/               pin-plan validation
```

## Build

Use ESP-IDF with ESP32-C5 support. The private ESP32-C5 802.11p ROM symbols are isolated in `obu_radio`.

```bash
idf.py -C firmware/c5 set-target esp32c5
idf.py -C firmware/c5 build

idf.py -C firmware/s3 set-target esp32s3
idf.py -C firmware/s3 menuconfig
idf.py -C firmware/s3 build
```

The S3 defaults to OpenTrafficMap upload **off**. Development Wi-Fi credentials and the opt-in switch can be supplied in `menuconfig`; production configuration belongs behind the authenticated control-plane interface.

## Prototype wiring profile

The selected Expansion Base, L76K and C5 link cannot be treated as a pile of stackable XIAO boards. The default prototype profile shares the S3 hardware SPI bus between SD and C5, uses separate chip selects, and reserves D6/D7 for L76K UART. C5 `DATA_READY` is routed to S3 GPIO42/D11 so the Expansion Base D1 user-button net is not driven. The Seeed CAN board uses D7 as its MCP2515 chip select, so it must be re-routed by the carrier if GNSS and CAN are required concurrently. The Round Display likewise requires a carrier/profile change rather than blind stacking.

```bash
python scripts/check_pin_plan.py config/hardware/prototype-expansion-base.yaml
python scripts/check_pin_plan.py config/hardware/round-display-carrier.yaml
```

See `docs/ARCHITECTURE.md` and `docs/REQUIREMENT_TRACEABILITY.md` before enabling TX.
