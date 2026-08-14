# Prototype hardware

## Expansion Base profile

The default S3 profile is `config/hardware/prototype-expansion-base.yaml`. It treats the Seeed Expansion Base peripherals as replaceable drivers behind project interfaces rather than as permanent product architecture.

| Peripheral | XIAO signal | ESP32-S3 GPIO in this prototype | Interface |
|---|---:|---:|---|
| SSD1306 OLED | D4/D5 | GPIO5/GPIO6 | `obu_display_driver_t` |
| PCF8563 RTC | D4/D5 | GPIO5/GPIO6 | `obu_time_service_t` |
| microSD | D8/D9/D10 + D2 CS | GPIO7/GPIO8/GPIO9 + GPIO3 CS | `obu_diag_logger` |
| Passive buzzer | A3/D3 | GPIO4 | `obu_warning_output_t` |
| Expansion Base user button | D1 | GPIO2 | reserved by Expansion Base |
| C5 chip select | D0 | GPIO1 | `obu_ipc` |
| GNSS PPS carrier signal | D12 | GPIO41 | `obu_time_service_t` |
| L76K UART | D6/D7 | GPIO43/GPIO44 | `obu_l76k` |

## C5-to-S3 wiring

The prototype uses the S3 as SPI master and the C5 as SPI slave at 8 MHz. The C5 link shares the S3 SPI bus with the Expansion Base microSD card, using a separate chip-select line.

| Signal | XIAO ESP32-S3 | XIAO ESP32-C5 |
|---|---|---|
| SCK | D8 / GPIO7 | D8 / GPIO8 |
| MISO | D9 / GPIO8 | D9 / GPIO9 |
| MOSI | D10 / GPIO9 | D10 / GPIO10 |
| C5 CS | D0 / GPIO1 | D0 / GPIO1 |
| Ground | GND | GND |

No `DATA_READY` wire is required in the default prototype profile. The S3 polls the SPI slave at a maximum interval of 20 ms and also initiates a transaction immediately when it has outbound data. This avoids using S3 GPIO42/D11, which is not on the normal XIAO side header, and avoids D1, which is already connected to the Expansion Base user button.

The IPC abstraction still supports an optional dedicated `DATA_READY` GPIO for a future carrier PCB: set `gpio_data_ready` to a non-negative GPIO number on both endpoints. The default Expansion Base build sets it to `-1` on both MCUs.

## Local warning buzzer

The Expansion Base passive buzzer is deliberately separate from the display component. `obu_warning` owns an abstract warning-output interface and the `obu_expansion_buzzer_create()` driver is one implementation.

The buzzer worker owns PWM timing so the S3 event consumer does not block for the length of a beep. Canonical `obu_warning_notification_t` records carry a stable `notification_id`; for DENM this should be derived from the DENM ActionID. The warning controller remembers active IDs in a bounded table so repeated frames for the same active event do not produce repeated audible episodes. An inactive record removes the ID and allows a later episode to notify again.

Audible output can be disabled at build time or muted at runtime. Buzzer initialization failure is logged and does not stop the rest of S3 acquisition.

## Pin conflicts

The L76K prototype uses D6/D7 for UART. The unmodified Seeed XIAO CAN expansion uses D7 as MCP2515 chip select, so that board cannot be blindly stacked with the selected GNSS wiring. A carrier PCB must reroute the CAN chip select or select a different interface assignment.

D1 is intentionally left to the Expansion Base user button and must not be driven by the C5. The XIAO Round Display also consumes several signals used by the Expansion Base profile. Use `config/hardware/round-display-carrier.yaml` and a deliberate carrier design rather than assuming the two profiles are physically stack-compatible.

Validate a profile before changing firmware pin constants:

```bash
python scripts/check_pin_plan.py config/hardware/prototype-expansion-base.yaml
python scripts/check_pin_plan.py config/hardware/round-display-carrier.yaml
```
