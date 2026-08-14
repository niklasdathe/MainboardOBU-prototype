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

Relevant S3 prototype options live under **BicycleOBU prototype** in Kconfig:

- local Expansion Base warning buzzer enable/mute/frequency/duration;
- optional direct OpenTrafficMap uploader;
- development Wi-Fi credentials and OTM node ID.

ESP-IDF boolean Kconfig options are not guaranteed to exist as C preprocessor symbols when disabled. Application code therefore uses `#ifdef`/explicit derived booleans instead of reading a disabled bool as an unconditional integer expression.

## Hardware-profile validation

```bash
python scripts/check_pin_plan.py config/hardware/prototype-expansion-base.yaml
python scripts/check_pin_plan.py config/hardware/round-display-carrier.yaml
```

This is a structural pin-ownership check, not electrical validation of the assembled carrier.

## CI interpretation

A green workflow proves that both firmware targets configure and compile under the pinned ESP-IDF release. It does not prove RF behavior, warning audibility, timing accuracy, storage endurance or multi-interface real-time performance.

Hardware acceptance evidence still needs the workbook-defined scenario/fault/endurance tests, including phone-disconnected warning tests and repeated-DENM deduplication tests.
