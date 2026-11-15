# Project TODO

This list tracks planned features and verification work that is not part of the validated default firmware. Experimental engine kernels must remain separate from the normal build until they have passed correctness and performance checks.

## Features

- [ ] **Optional opening book**
  - Add an opening-book lookup to the chess application, with normal Stockfish search as the fallback when no book move is available.
  - Use Polyglot `.bin` format initially; keep the book separate from the application in the dedicated raw read-only `book` partition and access it with `esp_partition_mmap()` rather than copying the complete file into RAM.
  - [x] Select the initial licensed book: `lichess_2100_rapid_2026-05.bin` (CC0 release, 1,601,616 bytes).
  - [x] Document and verify the separate raw-book flash procedure; see [Flashing an opening book](FLASHING-OPENING-BOOK.md).
  - [ ] Implement Polyglot lookup and add a book enable/disable setting, deterministic lookup tests and on-device lookup timing.
  - [ ] Add a host-side book validation tool.
  - Do not commit the raw book binary to the firmware source repository.

## Flash and partition layout

- [x] **Confirm physical flash capacity: 16 MiB.** The user checked the fitted module.
- [x] **Remove the OTA layout.** Firmware updates are performed over USB/serial; the two OTA app slots and `otadata` partition are no longer required. This is a USB-update design choice, not a consequence of Wi-Fi being disabled.
- [x] **Allocate one 4 MiB factory application partition.** The latest QIO build reports 1,884,600 bytes of Flash Data + Flash Code, leaving approximately 2.20 MiB within the 4 MiB app partition.
- [x] **Replace SPIFFS with a raw book partition.** The `book` partition is `data/undefined` with the read-only flag; it is not a filesystem. It is mapped read-only with `esp_partition_mmap()` when the reader is implemented.
- [ ] Implement Polyglot hash and move decoding, direct mmap lookup, validation and tests.
- [ ] Add a host-side book conversion/validation tool and choose the final licensed book.
- [ ] Add a separate book flashing command at offset `0x410000`.
- [ ] Add runtime `ESP.getFlashChipSize()` reporting as a diagnostic (not a blocker; physical capacity is already confirmed).

The old NVS partition remains at exactly `0x9000` with size `0x5000` (20 KiB), preserving the existing Preferences data layout. The former `otadata` bytes are now an unused 8 KiB gap; they are deliberately not merged into NVS.

### Current `partitions_stockfish_16MB.csv`

| Partition | Type / subtype | Offset | Size |
|---|---|---:|---:|
| NVS | data / nvs | `0x9000` | 20 KiB |
| Factory app | app / factory | `0x10000` | 4 MiB |
| Opening book | data / undefined (`0x06`), read-only | `0x410000` | 11.875 MiB |
| Coredump | data / coredump | `0xFF0000` | 64 KiB |

The book partition ends at `0xFF0000`; the coredump partition ends exactly at the 16 MiB flash boundary (`0x1000000`). There are no `app1`, `otadata` or filesystem partitions. PlatformIO's `build.app_partition_name` is explicitly set to `factory`, so upload-size checks apply to the 4 MiB app partition.

Changing the partition table requires flashing the new table and firmware. Do **not** run a full-chip erase as part of this layout change: the existing NVS offset and size are intentionally preserved so saved games and settings remain in place. The initial raw book image has already been programmed separately at `0x410000`; the application-side reader is still pending. See [the flashing guide](FLASHING-OPENING-BOOK.md).