# AGENTS.md

Instructions for coding agents working in this repository. Read this file before changing code, and read the relevant document in `docs/` before changing behavior.

## What this project is

An ESP32-S3 firmware that reads a 25-series SPI NOR flash and streams the raw image to a host over USB-Serial-JTAG. The host is the orchestrator; the device is a stateless SPI engine.

Current state:

- Firmware in `main/spi_dumper_main.c` implements **protocol v1**.
- `docs/protocol/protocol.md` specifies **protocol v2**, which is **not implemented yet**.
- `dump_firmware.py` is the current minimal host tool.
- The full host CLI and file-based configuration are planned, not implemented.

Do not describe v2 features as available. If you implement part of v2, update the docs in the same change.

## Commands

```bash
# Build
idf.py set-target esp32s3      # first time only
idf.py build

# Flash (use the native USB port)
idf.py -p /dev/ttyACM0 flash

# Monitor (do NOT run while dump_firmware.py is using the port)
idf.py -p /dev/ttyACM0 monitor

# Current host dump
python3 dump_firmware.py --port /dev/ttyACM0 --size 4194304 --out dumps/router.bin
```

On macOS the port is usually `/dev/cu.usbmodem*`. On Linux it is usually `/dev/ttyACM*` or `/dev/ttyUSB*`.

## Layout

- `main/spi_dumper_main.c`: the whole firmware (SPI init, JEDEC read, dump loop, CRC, v1 handshake).
- `main/CMakeLists.txt`: component dependencies (`esp_driver_spi`, `esp_driver_usb_serial_jtag`, `vfs`).
- `dump_firmware.py`: minimal host script; reads exactly `--size` bytes and compares the firmware CRC32 with the local one.
- `docs/protocol/protocol.md`: protocol v2 specification.
- `docs/system/settings.md`: configuration/chunk-size notes.
- `docs/system/state-less.md`: host-side resume metadata.
- `README.md`: user-facing overview, wiring, build, usage, limitations.
- `HUMAN_IDEAS.md`: idea inbox; not a specification.

## Invariants

These are non-negotiable unless the user explicitly asks to change them:

1. **The dump payload contains only flash bytes.** No logs, `printf`, `ESP_LOG`, progress text, or error messages between the `DATA` header and the end of the payload. Any text there corrupts the image.
2. **Keep CS asserted for the whole SPI read.** Build command + address + data as one `spi_device_transmit` transaction per chunk.
3. **Use `spi_device_transmit`, not `spi_device_polling_transmit`, in the dump loop.** Polling busy-waits and starves the IDLE task; the 5 s task watchdog then prints into the stream.
4. **SPI DMA buffers must use `heap_caps_malloc(..., MALLOC_CAP_DMA)`.** Do not put DMA buffers on the stack.
5. **`spi_bus_config_t.max_transfer_sz` must be at least `header + chunk size`.** Update it when increasing the chunk size.
6. **Bounds-check every host-provided range.** Reject `bytes == 0`, `bytes > max_chunk`, and `addr + bytes > max_flash` with `ERR 2 invalid-parameters`.
7. **The device must never write or erase the target flash.** Read-only is a safety property.
8. **Do not change the wire protocol silently.** Update `docs/protocol/protocol.md`, bump `proto` if incompatible, and update the host at the same time.
9. **Do not assume the target flash voltage.** The current hardware assumes 3.3 V. Never connect a 1.8 V flash directly to 3.3 V.
10. **Do not use SPI0/SPI1 for the target.** They are connected to the ESP32's own flash/PSRAM. Use SPI2 (or the board-specific free host).
11. **Do not use the ESP32's USB pins for SPI.** They are needed for the console.
12. **Keep the host and firmware sizes in sync for v1.** `FLASH_SIZE` in the firmware must match `--size` in `dump_firmware.py`.

## Protocol rules

- v1 (current): `READY\n` → host sends one byte `D` → 4 MiB raw → `\nCRC32:XXXXXXXX\n` → `READY\n`.
- v2 (specified): ASCII header, optional raw payload, optional ASCII trailer. `INFO`, `READ`, `DATA`, `CRC`, `QUIT`, `OK`, `ERR`.
- v2 `DATA` frame: `DATA <bytes> <addr>\n` + exactly `<bytes>` payload + `CRC <crc32>\n`.
- CRC32 is the standard CRC-32/ISO-HDLC variant, same as `zlib.crc32`, over the payload only.
- Addresses are sent as text (`0x...`) but are encoded big-endian on the SPI wire (24-bit for `0x03`, 32-bit for `0x13`).
- The host must read the payload with an exact-length read, never `readline()`.
- The host owns retries and resume state. The device is stateless.

## Known pitfalls

- **USB-Serial-JTAG init order:** install the driver with `usb_serial_jtag_driver_install()` **before** `usb_serial_jtag_vfs_use_driver()`. Reversing the order causes recursion/panic when the VFS logs an error through itself.
- **TX buffer:** the default USB TX buffer is too small for the dump; the current code uses 8192 bytes.
- **`printf` is not binary-safe for the dump:** the stdio/VFS path can buffer or drop bytes when the ring buffer is full. Use `usb_serial_jtag_write_bytes()` for payload data.
- **JEDEC quirk:** the Spansion S25FL032P reports `01 02 15` but is 4 MiB; `0x15` would generically mean 2 MiB. Use a chip table, not `1 << capacity`.
- **`CONFIG_ESPTOOLPY_FLASHSIZE` is the ESP32's own flash size**, not the target router flash.
- **24-bit addressing:** with `0x03` + 3 address bytes the maximum is 16 MiB. Larger chips need 4-byte addressing (`0x13` or 4-byte mode).
- **Deep power-down/QPI:** if the target SoC left the flash in DPD or QPI, a plain `0x03` read may fail. Wake/reset the flash first.
- **Watchdog:** the task watchdog is 5 s and checks the IDLE tasks. Avoid long busy-waits in the dump path.
- **Do not run `idf.py monitor` and `dump_firmware.py` at the same time.**
- **The current host script has known handshake/timeout limitations.** Do not treat it as the reference implementation for v2; use the spec.
- **`__pycache__/`, `build/`, and `dumps/` are generated.** Do not commit them.

## Verification / definition of done

For firmware changes:

1. `idf.py build` completes without errors.
2. Flash the board and run a dump against a known chip.
3. Verify the size is exactly the expected number of bytes.
4. Verify the host CRC matches the device CRC.
5. Run the dump twice and compare SHA256; they must match.
6. Confirm no logs or text appear inside the dumped binary.
7. If hardware is not available, say so explicitly and provide a manual test plan.

For host/protocol changes:

1. Test the pure logic (framing, CRC, chip lookup, resume) without hardware.
2. Test error paths: timeout, short read, CRC mismatch, invalid range, unknown message.
3. Test at least one end-to-end dump if hardware is available.

## Style

- Firmware: C11, 4-space indentation, identifiers in English, comments may be Spanish (the existing code uses Spanish comments).
- Host: Python 3.11+, type hints, small modules, no global mutable state.
- Keep protocol/framing/chip-database logic pure and testable without serial ports.
- Avoid new dependencies unless they are clearly justified.
- Prefer small, focused changes over broad refactors.
- Do not reformat unrelated code.

## Docs to update

When behavior changes, update the relevant document:

- Protocol/framing/messages: `docs/protocol/protocol.md`.
- Configuration/chunk size: `docs/system/settings.md`.
- Resume/metadata: `docs/system/state-less.md`.
- User-facing usage/wiring/limitations: `README.md`.
- Engineering rules: this file.

## Ask before

- Adding write/erase support.
- Changing the assumed voltage (1.8 V vs 3.3 V).
- Changing pins, SPI host, clock, flash size, or chunk size.
- Making an incompatible protocol change.
- Adding a new target/board or claiming multi-target support.
- Running binwalk/unblob on untrusted firmware outside a sandbox.

## Do not

- Do not commit firmware dumps or third-party firmware.
- Do not run extraction tools as root.
- Do not power a 1.8 V flash at 3.3 V.
- Do not use SPI0/SPI1 for the target flash.
- Do not add logs to the binary dump path.
- Do not silently change `FLASH_SIZE` or the host `--size`.
- Do not claim v2 or the CLI exists until it is implemented and tested.
