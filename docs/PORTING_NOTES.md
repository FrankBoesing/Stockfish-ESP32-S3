# Porting notes

## Principles

- Upstream-first: preserve official Stockfish engine logic.
- No Teensy source, patches, build flags, or assumptions are used.
- Do not disable NNUE, Syzygy, threading, or search features speculatively.
- Every ESP32-specific change must be minimal, isolated, justified, and measured.
- Pin the upstream Stockfish commit used for each successful milestone.

## Confirmed board identity

The target is the Freenove ESP32-S3 Display 2.8-inch capacitive-touch variant, model **FNK0104B**.

Freenove's documentation identifies:
- Display controller: ILI9341
- Native panel resolution: 240 x 320 (landscape operation can rotate the panel)
- Capacitive touch: touch variant FNK0104B

## Hardware configuration caveat

Do not confuse the FNK0104B display board with the generic PlatformIO board
`freenove_esp32_s3_wroom`, which is documented as an N8R8 board (8 MB flash /
8 MB PSRAM). Other FNK0104B hardware references identify an N16R8 configuration
(16 MB flash / 8 MB PSRAM). The exact module marking and actual flash/PSRAM
configuration must be read from the user's physical unit before finalizing the
PlatformIO board manifest and flash settings.

The current PlatformIO environment therefore uses a generic ESP32-S3 DevKit
profile strictly as a temporary compile baseline. It is not a validated
FNK0104B configuration.

## Initial investigation checklist

- [ ] Read the module marking and determine exact flash/PSRAM configuration.
- [ ] Confirm physical flash size and mode using `esptool flash_id` and a runtime flash-size probe; current PlatformIO settings assume 16 MiB.
- [x] Confirm PSRAM capacity with runtime heap diagnostics: 8 MiB OPI PSRAM is present.
- [ ] Record the exact Stockfish upstream commit and network filenames.
- [ ] Inspect Stockfish's current C++ language requirements and compiler assumptions.
- [ ] Inspect SIMD use in NNUE and determine whether a scalar fallback is available.
- [ ] Inspect thread implementation and available ESP-IDF/FreeRTOS primitives.
- [ ] Inspect memory allocation and alignment requirements.
- [ ] Determine whether the embedded NNUE network fits in flash and how it is accessed.
- [ ] Build a minimal serial UCI target before display integration.

## PlatformIO caution

The target board definition must match the actual module, not just the display
product name. Avoid forcing QIO/OPI, octal PSRAM, partition layout, or flash size
until these have been verified.

## Baseline validation

The initial firmware should support at least:

- `uci`
- `isready`
- `ucinewgame`
- `position startpos`
- `go depth 1` (subject to memory feasibility)
- `stop`
- `quit`

Compare legal best moves and protocol behavior with a desktop build from the same
upstream revision.
