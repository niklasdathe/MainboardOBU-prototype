# Documentation

Use the document that matches the task; keep detailed terminal captures out of the repository once their conclusions have been consolidated.

## Task guides

| Task | Document |
|---|---|
| Understand the C5/S3 responsibility split and event flow | [`ARCHITECTURE.md`](ARCHITECTURE.md) |
| Wire or change prototype hardware | [`HARDWARE.md`](HARDWARE.md) |
| Build both targets and interpret CI | [`DEVELOPMENT.md`](DEVELOPMENT.md) |
| Configure and run Wio-SX1262 LoRaWAN -> OpenTrafficMap | [`LORAWAN_OTM.md`](LORAWAN_OTM.md) |
| Diagnose OTAA / JoinAccept behavior | [`LORAWAN_OTAA_DEBUG.md`](LORAWAN_OTAA_DEBUG.md) |
| Check whether a workbook requirement is implemented or still awaiting evidence | [`REQUIREMENT_TRACEABILITY.md`](REQUIREMENT_TRACEABILITY.md) |

The executable TTS -> OpenTrafficMap bridge has its own short operator guide at [`../tools/lorawan_otm_bridge/README.md`](../tools/lorawan_otm_bridge/README.md).

## Engineering references

| Topic | Canonical repository location |
|---|---|
| C5↔S3 SPI framing and time correlation | `components/obu_ipc/` |
| Source-independent event/data model | `components/obu_core/` |
| Display-independent HMI | `components/obu_hmi/` |
| Local warning policy/output and Expansion Base buzzer | `components/obu_warning/` |
| GNSS and time holdover | `components/obu_gnss/`, `components/obu_time/` |
| SD diagnostic logging | `components/obu_log/` |
| LoRaWAN collection uplink | `components/obu_lorawan/` + `LORAWAN_OTM.md` |
| Parked direct Wi-Fi/OpenTrafficMap experiment | `components/obu_otm/` |
| Future CAN/BLE/VAM/PoTi/security/GN-BTP-DCC boundaries | `components/obu_ifaces/` |

## Evidence and machine-readable data

| Evidence | Location |
|---|---|
| ESP-IDF compiler gate for C5 and S3 | `.github/workflows/build.yml` |
| Hardware profiles | `config/hardware/*.yaml` |
| Pin-conflict validation | `scripts/check_pin_plan.py` |
| LoRaWAN OTAA conclusions | `LORAWAN_OTAA_DEBUG.md` |
| Requirement implementation state | `REQUIREMENT_TRACEABILITY.md` |

## Information ownership

- The requirement workbook in the parent BicycleOBU project governs system acceptance.
- YAML hardware profiles own prototype pin/resource assignments.
- Public component headers own reusable software contracts; applications should not bypass them to depend on a particular peripheral driver.
- `LORAWAN_OTM.md` owns the operational LoRaWAN/OpenTrafficMap path; `LORAWAN_OTAA_DEBUG.md` contains only reusable activation troubleshooting conclusions.
- `REQUIREMENT_TRACEABILITY.md` distinguishes implemented software from tests that still require real hardware/RF evidence.
