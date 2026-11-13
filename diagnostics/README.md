# FNK0104B Board Diagnostics

Standalone PlatformIO bring-up project for the Freenove ESP32-S3 Display FNK0104B. It is deliberately separate from the Stockfish engine port.

## Hardware profile

- ESP32-S3 dual-core Xtensa LX7, up to 240 MHz
- 16 MiB flash and expected 8 MiB OPI PSRAM (runtime report is authoritative)
- 2.8-inch 240x320 ILI9341 SPI display
- FT6336U capacitive touch controller, I2C address 0x38

Display: SCLK=12, MOSI=11, MISO=13, CS=10, DC=46, BL=45, reset tied to board reset.
Touch: SDA=16, SCL=15, INT=17, RST=18.

## Build and upload

Open this directory as a PlatformIO project:

    pio run -e fnk0104b
    pio run -e fnk0104b -t upload
    pio device monitor -b 115200

Configuration uses DIO flash + OPI PSRAM (dio_opi), 16 MiB flash and the standard 16 MiB partition layout. The target is the generic esp32-s3-devkitc-1 because the FNK0104B is not represented by a dedicated PlatformIO board manifest.

## Tests

At boot, serial output includes chip model/revision, core count, clock, SDK and flash information; internal/8-bit/PSRAM heap totals, free bytes and largest contiguous blocks; a PSRAM allocation with write/read integrity check and throughput estimate; display full-screen fill timings; and a simultaneous integer workload pinned to each CPU core.

The display shows a live hardware summary, six RGB color bars and touch coordinates. Touch events are also printed to serial.

## Interpretation

- The largest free PSRAM block is more useful than total free bytes for sizing a single hash allocation; fragmentation matters.
- The temporary PSRAM test is capped at 2 MiB so the diagnostic does not consume nearly all memory. It does not determine the final safe Stockfish hash size.
- The CPU test is a simple two-core workload, not a Stockfish NPS benchmark.
- Flash size is detected/configured capacity, not remaining filesystem space.

## Scope limits

This first version does not mount the SD card or test Wi-Fi/Bluetooth. It also does not determine the final safe hash-table size. Those are follow-up tests after the initial hardware bring-up.
