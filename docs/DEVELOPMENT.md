# Development and verification

## ESP-IDF baseline

GitHub Actions builds both applications in Espressif's `release-v5.5` container. Local development should use an ESP-IDF 5.5 release unless a version change is made deliberately and CI is updated at the same time.

## Build C5

```bash
idf.py -C firmware/c5 set-target esp32c5
idf.py -C firmware/c5 build
```

## Build S3

```bash
idf.py -C firmware/s3 set-target esp32s3
idf.py -C firmware/s3 menuconfig
idf.py -C firmware/s3 build
```

`firmware/s3/sdkconfig.defaults` captures the prototype board assumptions that must be reproducible in CI: the Seeed Studio XIAO ESP32-S3 has 8 MB flash, and the project uses ESP-IDF's standard 1.5 MiB **single factory app (large), no OTA** partition layout. The linked S3 image is larger than ESP-IDF's default 1 MiB factory slot because it includes the hub, Wi-Fi/MQTT TLS, FAT/SD, GNSS/time, display and warning paths. OTA is not implemented in this prototype; a future OTA design must replace the partition layout deliberately rather than silently shrinking the application slots.

Relevant S3 prototype options live under **BicycleOBU prototype** in Kconfig:

- local Expansion Base warning buzzer enable/mute/frequency/duration;
- optional direct OpenTrafficMap uploader;
- development Wi-Fi credentials and OTM node ID;
- optional Wi-Fi debug diagnostics for the development uplink.

ESP-IDF boolean Kconfig options are not guaranteed to exist as C preprocessor symbols when disabled. Application code therefore uses `#ifdef`/explicit derived booleans instead of reading a disabled bool as an unconditional integer expression.

## Wi-Fi debugging

The direct Wi-Fi uplink is a development feature on the S3. To diagnose association, DHCP or Internet/MQTT failures without changing the normal connection policy:

```bash
idf.py -C firmware/s3 menuconfig
```

Under **BicycleOBU prototype**, enable **Enable direct OpenTrafficMap Wi-Fi uploader** and then **Enable Wi-Fi debug diagnostics**.

Configure the development SSID/password and node ID in the same menu, then rebuild, flash and monitor:

```bash
idf.py -C firmware/s3 build
idf.py -C firmware/s3 flash monitor
```

With Wi-Fi debug diagnostics enabled, the serial log adds:

- ESP-IDF Wi-Fi driver and netif `DEBUG` output;
- effective STA configuration including scan/sort policy, fixed channel, RSSI/auth threshold, PMF flags and power-save mode;
- association details including RSSI, channel, AP auth mode and BSSID;
- disconnect reason number plus a readable reason for common failures such as AP-not-found, authentication, association, handshake and beacon timeouts;
- DHCP `GOT_IP` data and `LOST_IP` transitions;
- explicit initialization-stage failures before association.

The configured Wi-Fi password is never printed; diagnostics expose only its length. Existing MQTT/TLS diagnostics remain separate, so the log can distinguish **Wi-Fi association -> DHCP/IP -> MQTT/TLS** failures. Disable the Wi-Fi debug option again after troubleshooting to reduce serial-log volume.

## Hardware-profile validation

```bash
python scripts/check_pin_plan.py config/hardware/prototype-expansion-base.yaml
python scripts/check_pin_plan.py config/hardware/round-display-carrier.yaml
```

This is a structural pin-ownership check, not electrical validation of the assembled carrier.

## CI interpretation

A green workflow proves that both firmware targets configure, compile, link, generate flash images and pass ESP-IDF's application-partition size check under the pinned ESP-IDF release. It does not prove RF behavior, warning audibility, timing accuracy, storage endurance or multi-interface real-time performance.

Hardware acceptance evidence still needs the workbook-defined scenario/fault/endurance tests, including phone-disconnected warning tests and repeated-DENM deduplication tests.
