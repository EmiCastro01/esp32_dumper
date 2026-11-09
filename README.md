# esp32-dumper

Dump SPI NOR flash chips (25-series) with an ESP32 and stream the raw image to a host over USB-Serial-JTAG.

## Status

| Part | State |
|---|---|
| Firmware (v1) | Works for the S25FL032P, 4 MiB, 3.3 V |
| Protocol v2 | Specified in [`docs/protocol/protocol.md`](docs/protocol/protocol.md); not implemented |
| Host tool | Minimal `dump_firmware.py`; CLI planned |
| Multi-target support | Planned |
| Tests / CI | Not yet |

## What it does

- Initializes an SPI bus on the ESP32-S3 and reads a 25-series SPI NOR flash.
- Prints the flash **JEDEC ID** and waits for the host.
- On the host command `D`, dumps the configured flash size as **raw bytes** over USB-Serial-JTAG.
- Prints a final **CRC32** so the host can verify the transfer.
- Returns to `READY`, so you can dump again without reflashing.

It is a **read-only** tool. It does not write, erase, or modify the target flash.

## Hardware

- ESP32-S3 board with the native USB-Serial-JTAG port (the USB port, not the UART bridge).
- Target SPI NOR flash (25-series), 3.3 V.
- SOIC-8 clip or soldered wires, short and with a common ground.
- A way to hold the router SoC in reset while reading its flash.

### Default wiring

The pinout is defined in [`main/spi_dumper_main.c`](main/spi_dumper_main.c). The current defaults are:

| ESP32-S3 | Flash SOIC-8 | Signal | Notes |
|---|---|---|---|
| 3.3 V | 8 (VCC) | Power | Check that the target is **3.3 V**, not 1.8 V |
| GND | 4 (GND) | Ground | Common ground |
| GPIO 10 | 1 (CS#) | Chip select | 10 kΩ pull-up recommended |
| GPIO 13 | 2 (SO / IO1) | MISO | — |
| GPIO 11 | 5 (SI / IO0) | MOSI | — |
| GPIO 12 | 6 (SCLK) | Clock | 22–33 Ω series resistor optional |
| 3.3 V | 3 (WP# / IO2) | Write protect | 10 kΩ pull-up |
| 3.3 V | 7 (HOLD# / IO3) | Hold / reset | 10 kΩ pull-up |

Always verify the package and pinout against the flash datasheet. Pin 1 is the reference.

### Safety notes

- **Do not power a 1.8 V flash from 3.3 V.** Many modern routers use 1.8 V SPI flash. Measure VCC and check the chip marking before connecting.
- **Hold the router SoC in reset** while reading. Otherwise the SoC may drive the SPI bus and cause contention or corrupt reads.
- **Do not connect two active 3.3 V supplies** together. Power one side, or use a proper isolation/level-shifting setup.
- Keep WP# and HOLD# high. If they float, the flash may not respond correctly.
- At 1 MHz the wiring is forgiving. If you raise the clock, use short wires, a ground return, and series resistors.
- The tool is read-only, but incorrect wiring or voltage can still damage hardware.

## Repository layout

```text
.
├── main/
│   ├── CMakeLists.txt
│   └── spi_dumper_main.c      # firmware (protocol v1)
├── dump_firmware.py           # minimal host script
├── docs/
│   ├── protocol/protocol.md   # protocol v2 specification
│   └── system/
│       ├── settings.md
│       └── state-less.md
├── HUMAN_IDEAS.md             # idea inbox
├── AGENTS.md                  # engineering rules for agents
├── CONTRIBUTING.md            # in progress
├── CHANGELOG.md               # in progress
├── CMakeLists.txt
└── sdkconfig                  # generated for ESP32-S3 / IDF 6.1.0
```

## Requirements

- **ESP-IDF 6.1.0** (the version used to generate `sdkconfig`; pin the version you test).
- Python 3.11+ for the host script.
- `pyserial` (`python3 -m pip install pyserial`).
- Linux: your user must have access to the serial port (usually the `dialout` group).
- macOS: the port is usually `/dev/cu.usbmodem*`.
- Linux: the port is usually `/dev/ttyACM*` or `/dev/ttyUSB*`.

ESP-IDF is not vendored in this repository. Install it and export the environment before building:

```bash
. "$IDF_PATH/export.sh"
```

## Build and flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash
```

On macOS:

```bash
idf.py -p /dev/cu.usbmodemXXXX flash
```

Use the ESP32-S3 **native USB** port, not the UART bridge. `idf.py monitor` is useful for debugging, but **do not run it while the dump script is using the port**: they compete for the same serial device.

## Dump

```bash
python3 -m pip install pyserial
mkdir -p dumps
python3 dump_firmware.py --port /dev/ttyACM0 --size 4194304 --out dumps/router.bin
```

The script waits for `READY`, sends `D`, reads exactly `--size` bytes, compares the firmware's CRC32 with the local one, and writes:

```text
dumps/router.bin
dumps/router.bin.sha256
```

Run the dump twice and compare the SHA256 values. Two identical hashes are a good sign that the read is stable:

```bash
sha256sum dumps/router.bin
```

> The current firmware has `FLASH_SIZE = 4 * 1024 * 1024`. `--size` must match it. If you change the firmware's size, change the host argument too. Protocol v2 will remove this coupling.

## Protocol

### Current firmware: v1

```text
Device -> READY\n
Host   -> D (one byte, 0x44)
Device -> 4 MiB raw bytes
Device -> \nCRC32:XXXXXXXX\n
Device -> READY\n
```

- Fixed 4 MiB dump from address `0x000000`.
- One CRC32 at the end, over the whole image.
- No address, no length, no per-chunk CRC, no resume.
- The device reads internally in 4 KiB SPI chunks, but the host sees one continuous stream.

### Planned: v2

The v2 specification in [`docs/protocol/protocol.md`](docs/protocol/protocol.md) is message-based and host-driven:

```text
Host   -> INFO
Device -> INFO proto=2 jedec=010215 ...
Host   -> READ <bytes> <addr>
Device -> DATA <bytes> <addr>
Device -> <bytes> raw
Device -> CRC <crc32>
```

The host looks up the JEDEC ID in a chip database, chooses the dump ranges, verifies each chunk, and can resume a partial dump. The device remains stateless. See also [`docs/system/state-less.md`](docs/system/state-less.md).

## Configuration

Today the firmware is configured at compile time in `main/spi_dumper_main.c`:

- SPI pins,
- SPI clock (default 1 MHz),
- flash size (default 4 MiB),
- internal chunk size (4 KiB),
- read command (`0x03`).

Planned configuration will be file-based (YAML/JSON) and exposed through the host CLI, including chunk size and SPI clock. See [`docs/system/settings.md`](docs/system/settings.md).

## Current limitations

- **Fixed 4 MiB** flash size; no auto-detection.
- **24-bit addressing**, so the current path tops out at 16 MiB.
- **3.3 V only**; 1.8 V flash needs level shifting and a different supply.
- **Hardcoded pins**, clock, and size.
- **One CRC at the end**; a single lost byte invalidates the whole dump.
- **No resume**, no per-chunk CRC, no error frames.
- **No chip database** yet.
- **Host script is minimal** and trusts `--size`.
- **No tests or CI** yet.
- **No license file** yet. Add one before distributing.

## Roadmap

1. **Protocol v2**: `INFO`, `READ`, `DATA`, per-chunk `CRC`, error frames, resume.
2. **Host CLI**: `detect`, `dump`, `verify`, `resume`, `raw`, `doctor`, `unpack`.
3. **Chip database** (JEDEC → size/voltage/address width) and auto-detection.
4. **Multi-target support** for the ESP32 family, with a documented support matrix.
5. **Hardware rev B**: 1.8 V/3.3 V selection, level shifters, protection, target reset.
6. **Tests, CI, packaging, and documentation**.

## Documentation

- [`docs/protocol/protocol.md`](docs/protocol/protocol.md) — protocol v2 specification.
- [`docs/system/settings.md`](docs/system/settings.md) — configuration and chunk size.
- [`docs/system/state-less.md`](docs/system/state-less.md) — stateless device and host-side resume metadata.
- [`HUMAN_IDEAS.md`](HUMAN_IDEAS.md) — idea inbox.
- [`AGENTS.md`](AGENTS.md) — engineering rules for agents and contributors.

`CONTRIBUTING.md` and `CHANGELOG.md` are still placeholders.

## Contributing

Contributions are welcome, but `CONTRIBUTING.md` is still being written. For now:

- Open an issue with the chip model, JEDEC ID, voltage, package, wiring description, and the exact commands you ran.
- Do not commit firmware dumps of third-party devices.
- Respect local laws and the copyright of the firmware you dump.

## License

No license has been added yet. Until one is added, the code is not formally licensed for redistribution.
