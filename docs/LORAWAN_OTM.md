# LoRaWAN uplink to OpenTrafficMap

This prototype replaces the stalled direct S3 Wi-Fi/MQTT path with a LoRaWAN transport based on the Seeed XIAO ESP32-S3 + Wio-SX1262 kit.

## Architecture

```text
ITS-G5 air
  -> ESP32-C5 raw RX frame
  -> SPI2 to ESP32-S3
  -> bounded obu_lorawan queue
  -> Wio-SX1262 / LoRaWAN EU868
  -> LoRaWAN network server (The Things Stack reference implementation)
  -> tools/lorawan_otm_bridge
  -> TLS MQTT
  -> cits1.opentrafficmap.org
     its/<OTM_NODE_ID>/packet
```

OpenTrafficMap still receives the original raw C-ITS frame bytes. LoRaWAN is only the constrained transport between the bicycle and a server with Internet access; it is not MQTT carried directly by the end device.

## Hardware

The radio component follows Seeed's XIAO ESP32-S3 + SX1262 board support values.

| Signal | ESP32-S3 GPIO | Notes |
|---|---:|---|
| SPI SCK | 7 | shared SPI2 bus |
| SPI MISO | 8 | shared SPI2 bus |
| SPI MOSI | 9 | shared SPI2 bus |
| SX1262 NSS | 41 | B2B radio chip select |
| SX1262 DIO1 | 39 | radio IRQ |
| SX1262 RESET | 42 | radio reset |
| SX1262 BUSY | 40 | radio busy |
| Wio RF switch | 38 | external antenna switch control |
| GNSS PPS | 47 | moved from GPIO41/D12 |

**Required hardware change:** do not leave the L76K PPS signal on D12/GPIO41 when the Wio-SX1262 is fitted. GPIO41 is electrically connected to SX1262 NSS. Move the PPS carrier to GPIO47 before using this hardware profile.

The Wio radio shares SCK/MISO/MOSI with the C5 and microSD. Each device has its own chip select. RadioLib's native ESP-IDF HAL attaches the SX1262 as another device on the already-initialized SPI bus.

The Seeed BSP configures the SX1262 TCXO supply for 3.0 V and uses GPIO38 for the board RF switch; `obu_lorawan` mirrors those values.

## RadioLib pin

The component uses RadioLib from commit:

```text
f0fb029566c2d58a7373bb66d3a48002a5b56876
```

This is intentionally pinned rather than following a moving branch. It contains the SX1262 LoRaWAN Class-A receive-window timing fix used by this ESP-IDF integration. `CONFIG_FREERTOS_HZ=1000` is enabled for the RadioLib ESP-IDF timing implementation.

## Configure the S3

```bash
idf.py -C firmware/s3 set-target esp32s3
idf.py -C firmware/s3 menuconfig
```

Under **BicycleOBU LoRaWAN uplink**:

- enable `OBU_LORAWAN_ENABLE`;
- set the OTAA JoinEUI;
- set the DevEUI;
- set the NwkKey;
- set the AppKey;
- leave FPort 10 unless the bridge is configured to match another port.

Then build and flash normally:

```bash
idf.py -C firmware/s3 build
idf.py -C firmware/s3 flash monitor
```

Credentials are not committed. The CI LoRaWAN build uses deliberately fake values only to compile the enabled code path.

## LoRaWAN network server

The reference bridge expects The Things Stack MQTT application integration. Register the end device for OTAA with the same JoinEUI, DevEUI and root keys configured on the S3. Create an application API key that can read application uplinks.

The bridge subscribes to:

```text
v3/<TTS_APPLICATION_UID>/devices/+/up
```

It reads `uplink_message.f_port` and the base64 encoded `uplink_message.frm_payload` from each application uplink.

## Run the OpenTrafficMap bridge

```bash
cd tools/lorawan_otm_bridge
python -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt

export TTS_MQTT_HOST='eu1.cloud.thethings.network'
export TTS_MQTT_USERNAME='your-application@ttn'
export TTS_MQTT_PASSWORD='NNSXS.your-api-key'
export TTS_APPLICATION_UID='your-application@ttn'
export OTM_NODE_ID='your-opentrafficmap-node-id'

python bridge.py
```

Optional variables:

| Variable | Default | Purpose |
|---|---|---|
| `TTS_MQTT_PORT` | `8883` | TTS MQTT TLS port |
| `OTM_MQTT_HOST` | `cits1.opentrafficmap.org` | OTM broker |
| `OTM_MQTT_PORT` | `8883` | OTM MQTT TLS port |
| `LORAWAN_FRAME_FPORT` | `10` | fragment FPort |
| `LORAWAN_DEVICE_ID` | unset | accept only one LoRaWAN device |
| `LORAWAN_REASSEMBLY_TIMEOUT_S` | `600` | incomplete-frame expiry |

The bridge publishes the reconstructed binary frame to:

```text
its/<OTM_NODE_ID>/packet
```

and maintains `its/<OTM_NODE_ID>/status` with `online`/`offline` retained state.

## Fragment protocol

LoRaWAN application payloads are intentionally small. A raw C-ITS frame is split into fragments with a 12-byte header:

| Byte(s) | Field |
|---|---|
| 0..1 | ASCII `BO` magic |
| 2 | protocol version (`1`) |
| 3 | flags (`0`) |
| 4..5 | frame sequence, big endian |
| 6 | fragment index |
| 7 | fragment count |
| 8..9 | original frame length, big endian |
| 10..11 | CRC16-CCITT of the complete original frame |
| 12.. | raw frame fragment |

The default carries 32 raw bytes per uplink, giving a 44-byte application payload. The bridge accepts out-of-order fragments, ignores identical duplicates, rejects conflicting duplicates, verifies final length and CRC, and publishes only a complete valid frame.

## Throughput and behavior under load

LoRaWAN is much lower bandwidth than ITS-G5. This implementation is therefore a **best-effort collection uplink**, not a lossless mirror of every received 802.11p frame. The S3 never blocks C5 acquisition waiting for LoRa airtime. Its queue is bounded; when it fills, the oldest pending raw frame is discarded so newer traffic can still be sampled.

The application adds a 10 s default spacing between fragments and leaves RadioLib duty-cycle enforcement enabled. Increase sampling selectivity rather than disabling regulatory duty-cycle handling.

The current implementation is fixed to EU868. Do not transmit with this configuration outside a region where EU868 parameters are permitted; add a region configuration before supporting other deployments.

## Known prototype limitations

- CI verifies compilation, pin-plan consistency and bridge reassembly tests; it cannot prove RF output, gateway coverage, OTAA acceptance or end-to-end OpenTrafficMap receipt.
- LoRaWAN session state is not yet persisted to NVS. Before a field/product implementation, persist the RadioLib LoRaWAN session/nonces across reset as required by the chosen network-server policy.
- The uplink currently forwards raw received frames without traffic prioritization. A later collection policy should select message types or sampling rates based on research requirements and available LoRaWAN airtime.
- The direct S3 Wi-Fi/OpenTrafficMap component remains in the repository as a parked development path and is not reused by this transport.

## Tests

```bash
cd tools/lorawan_otm_bridge
python -m unittest discover -s tests -v
python -m py_compile protocol.py bridge.py
```

GitHub Actions also builds the S3 once with LoRaWAN disabled and once with `OBU_LORAWAN_ENABLE=y` using dummy credentials.
