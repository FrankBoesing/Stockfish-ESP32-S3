# Project TODO

This list tracks planned features and verification work that is not part of the validated default firmware. Experimental engine kernels must remain separate from the normal build until they have passed correctness and performance checks.

## Features

- [ ] **Optional opening book**
  - Add an opening-book lookup to the chess application, with normal Stockfish search as the fallback when no book move is available.
  - Prefer a documented format such as Polyglot `.bin`, or convert a supplied source book into a compact sorted format optimized for this target.
  - Keep book data separate from the application binary. Evaluate replacing the currently configured SPIFFS partition with a dedicated read-only `book` data partition and map it for reads with `esp_partition_mmap()`; do not load the complete book into internal RAM.
  - Add a host-side conversion/validation tool, book enable/disable setting, deterministic lookup tests and on-device lookup timing.
  - Do not commit a commercial or otherwise unlicensed opening-book data file. Keep the format/tooling separate from the actual book data.

## Flash and partition layout

- [ ] **Verify the physical flash capacity before changing the partition table.**
  - Read the exact module marking and capture `esptool.py flash_id` output; add a runtime `ESP.getFlashChipSize()` diagnostic.
  - Current PlatformIO configuration assumes 16 MiB: `board_upload.flash_size = 16MB` and `default_16MB.csv`.
  - The current 16 MiB table allocates two 6.25 MiB OTA app slots, a 3.375 MiB SPIFFS partition, plus NVS, OTA metadata and coredump. The current project does not appear to use SPIFFS, so that data area is a candidate for the opening book after checking all source references.
  - If 32 MiB flash is actually fitted, verify the exact flash part and ESP-IDF 5.5.5 addressing support before placing a partition above the 16 MiB boundary. Do not infer 32 MiB capacity from the ESP32-S3's address-mapping capability.
  - Record application binary size separately from physical flash size and partition size. A roughly 1.4 MiB firmware image does not establish that the chip itself is only 1.4 MiB or that the remaining address space is already available to a data partition.
  - Decide whether to preserve dual-slot OTA. Keeping two OTA slots constrains each app partition; a single-app layout could trade OTA rollback for more book storage.


## Current flash configuration reference

The current `default_16MB.csv` partition table uses:

| Partition | Offset | Size |
|---|---:|---:|
| NVS | 0x9000 | 20 KiB |
| OTA metadata | 0xE000 | 8 KiB |
| app0 | 0x10000 | 6.25 MiB |
| app1 | 0x650000 | 6.25 MiB |
| SPIFFS | 0xC90000 | 3.375 MiB |
| Coredump | 0xFF0000 | 64 KiB |

The table exactly spans the configured 16 MiB address range. A change to the partition table requires a full erase/reflash and should be done only after physical flash identification and a decision about OTA requirements.
