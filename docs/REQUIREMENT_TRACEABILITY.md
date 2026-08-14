# Requirement traceability — prototype implementation

This document maps the main implemented boundaries to the v1.1 workbook. It is not a declaration that every system acceptance criterion is already verified.

| Requirement area | Implementation | State |
|---|---|---|
| ARCH-001 / V2X-TX-003 | `obu_radio` C5 endpoint contains no bicycle/facilities semantics | Implemented |
| ARCH-002 / ARCH-005 | S3 hub + versioned `obu_ipc` transport | Implemented |
| SYS-003 / DISP-004 / DISP-010 | phone-independent `obu_warning` output; Expansion Base passive buzzer on D3/GPIO4; build enable + runtime mute boundary | Implemented actuator path; phone-less DENM scenario test pending |
| WARN-003 | canonical `notification_id` + bounded active/recent-ID dedup in `obu_warning_controller_t`; raw receptions remain unaffected | Implemented warning-output dedup contract; DENM ActionID mapping belongs to embedded DENM decoder |
| SYS-004 / V2X-RX-006 | C5 status and separate RX/drop/TX counters | Implemented; HIL verification pending |
| V2X-RX-001/002/003/008/009 | ESP32-C5 802.11p RX, configured ITS frequency, raw frame bytes forwarded | Implemented; RF verification pending |
| V2X-RX-004/005 | RX timestamp, frequency, RSSI, Wi-Fi type/RX state, raw bytes retained | Implemented; PCAP export belongs downstream |
| V2X-RX-007 | bounded SPI queues/protocol | Implemented; 2× representative-rate test pending |
| V2X-TX-002/004/006/007 | TX API, boot-session arming, TX result/suppression hooks | Radio endpoint implemented; complete VAM policy depends on VBS backends |
| V2X-TX-008/011/013–026 | security, UPER VAM, PoTi, GN/BTP/DCC and conformance gates | Interfaces/gates only; **no conformance claim** |
| GNSS-001/003/005/007/008 | L76K UART/NMEA source, parsed fix + raw NMEA, stale/validity model | Implemented |
| GNSS-004 / TIME-* | D12 PPS capture, monotonic/UTC model, PCF8563 holdover, C5/S3 four-timestamp probes and offset/RTT provenance events | Implemented core; measured HIL accuracy still pending |
| GNSS-010/011 | PoTi service and <0.5 m gate | Interface/gate only; L76K prototype path does not meet the full gate |
| CAN-001–013 | canonical CAN event/interface; raw frame fields/provenance preserved | Interface prepared; MCP2515 driver intentionally not implemented here |
| BSENS/PBLE | canonical BLE source/control interfaces | Interface prepared; implementation deferred |
| DISP-001/003/005/006/008/009 | abstract HMI model + SSD1306 driver, non-blocking consumer; display init failure is non-fatal | Implemented baseline renderer; warning decode scenario verification pending |
| DISP-007 | round-display separation | Interface + carrier manifest prepared; LVGL driver deferred |
| DATA-003 | right-handed rear-axle bicycle frame (+X forward, +Y left, +Z up) + versioned sensor-pose model | Documented/model prepared |
| DATA-001/002 + TIME-001/005/007 | source/acquisition timestamp, separate S3 hub-domain timestamp, derived/raw flags, UTC quality and explicit clock-sync records | Implemented in `obu_event_t` / `obu_clock_sync_status_t` |
| LOG-010/011/012 | SD diagnostic-only bounded rotation/retention including boot/radio/TX/clock-sync lifecycle records | Implemented foundations; injected-fault reconstruction test pending |
| REL-001/002/003/005 | bounded queues, overflow accounting, reset/version status, failure visibility | Implemented foundations; 4 h HIL test pending |
| OTM-001–010 | phone baseline remains the requirement path; optional S3 direct uploader is an extra opt-in/live-only/non-critical extension with counters and no replay buffer | Baseline interface preserved; direct extension implemented, phone app integration remains outside this repo |
| PCB-009 | pin conflicts explicit, carrier profiles required | Implemented as YAML + validator |

## Verification still required before calling the complete system compliant

1. Hardware warning tests for audible output enable/mute, phone-disconnected operation and repeated-DENM deduplication.
2. HIL/RF throughput, queue-overflow and 4-hour endurance tests.
3. Legal/regulatory TX configuration verification for the actual test location.
4. Complete ETSI VAM ASN.1 UPER codec and Release-2 transport stack.
5. ETSI security backend with valid credentials, signing and identity-change behavior.
6. PoTi implementation and a positioning source that satisfies the workbook's full-conformance accuracy gate.
7. DCC/GN/BTP timing/latency/reliability tests and the workbook's dense-load criteria.
8. Final carrier pin assignment and electrical verification with GNSS + SD/RTC/display/buzzer + CAN + C5 link connected concurrently.
