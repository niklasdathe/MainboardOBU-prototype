# Prototype hardware

## Expansion Base profile

The default S3 profile is `config/hardware/prototype-expansion-base.yaml`. It treats the Seeed Expansion Base peripherals as replaceable drivers behind project interfaces rather than as permanent product architecture.

| Peripheral | XIAO signal | ESP32-S3 GPIO in this prototype | Interface |
|---|---:|---:|---|
| SSD1306 OLED | D4/D5 | GPIO5/GPIO6 | `obu_display_driver_t` |
| PCF8563 RTC | D4/D5 | GPIO5/GPIO6 | `obu_time_service_t` |
| microSD | D8/D9/D10 + D2 CS | GPIO7/GPIO8/GPIO9 + GPIO3 CS | `obu_diag_logger` |
| Passive buzzer | A3/D3 | GPIO4 | `obu_warning_output_t` |
| C5 chip select | D0 | GPIO1 | `obu_ipc` |
| C5 DATA_READY | D11 | GPIO42 | `obu_ipc` |
| GNSS PPS carrier signal | D12 | GPIO41 | `obu_time_service_t` |
| L76K UART | D6/D7 | GPIO43/GPIO44 | `obu_l76k` |

## Local warning buzzer

The Expansion Base passive buzzer is deliberately separate from the display component. `obu_warning` owns an abstract warning-output interface and the `obu_expansion_buzzer_create()` driver is one implementation.

The buzzer worker owns PWM timing so the S3 event consumer does not block for the length of a beep. Canonical `obu_warning_notification_t` records carry a stable `notification_id`; for DENM this should be derived from the DENM ActionID. The warning controller remembers active IDs in a bounded table so repeated frames for the same active event do not produce repeated audible episodes. An inactive record removes the ID and allows a later episode to notify again.

Audible output can be disabled at build time or muted at runtime. Buzzer initialization failure is logged and does not stop the rest of S3 acquisition.

## Pin conflicts

The L76K prototype uses D6/D7 for UART. The unmodified Seeed XIAO CAN expansion uses D7 as MCP2515 chip select, so that board cannot be blindly stacked with the selected GNSS wiring. A carrier PCB must reroute the CAN chip select or select a different interface assignment.

The XIAO Round Display also consumes several signals used by the Expansion Base profile. Use `config/hardware/round-display-carrier.yaml` and a deliberate carrier design rather than assuming the two profiles are physically stack-compatible.

Validate a profile before changing firmware pin constants:

```bash
python scripts/check_pin_plan.py config/hardware/prototype-expansion-base.yaml
python scripts/check_pin_plan.py config/hardware/round-display-carrier.yaml
```
