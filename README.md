# Stockfish ESP32-S3

An experimental, native port of the official Stockfish chess engine to the Espressif ESP32-S3.

## Project goals

- Start from the official Stockfish source tree; do not inherit code, patches, or limitations from a Teensy port.
- Keep upstream engine logic and NNUE evaluation intact wherever technically possible.
- Isolate ESP32-specific adaptations in a small platform layer and documented build integration.
- Build and test the engine independently of the display UI.
- Use PlatformIO for reproducible ESP32-S3 firmware builds.

## Target hardware

- Freenove ESP32-S3 Display, 2.8-inch 320x240 capacitive-touch variant
- ESP32-S3, dual-core Xtensa LX7
- PSRAM: expected 8 MiB; verify actual board revision and runtime detection before relying on it.

## Current status

Repository and project scaffolding initialized. No Stockfish source has been copied or modified yet.

## Planned milestones

1. Inspect upstream Stockfish build system, architecture-specific code, NNUE network loading, threading, and memory allocation.
2. Establish a reproducible PlatformIO environment for the exact Freenove board.
3. Build a minimal UCI-capable firmware with the fewest possible platform changes.
4. Validate UCI behavior and benchmark results against a desktop build of the same upstream revision.
5. Measure internal RAM and PSRAM use; only then decide whether any limitations or adaptations are necessary.
6. Add the touchscreen chess UI as a separate layer.

## Memory access benchmark

The FNK0104B diagnostic firmware includes a synthetic memory benchmark after the heap report:

- 128 KiB internal SRAM versus 128 KiB PSRAM, using the same working-set size.
- 1 MiB PSRAM working set to show behavior with a larger table.
- Sequential 32-bit read and write throughput.
- Randomized reads and writes to 16-byte entries, reporting ns/op and Mops/s.

The 16-byte entry is a deliberately simple stand-in for a compact transposition-table slot. This is **not** a Stockfish benchmark and its result is not equivalent to NPS. It isolates memory behavior under a reproducible single-core workload. The benchmark runs after the allocation/integrity test and before the display benchmark. It allocates and frees its test buffers, so it does not reserve memory for the eventual engine.

Build and upload the root PlatformIO project using the `freenove_fnk0104b` environment. Capture the complete serial output, especially the lines beginning with `MEMBENCH`. For a fair comparison, use the same firmware, CPU frequency, power conditions, and monitor setup; do not run Wi-Fi, display animation, or other memory-intensive tasks during the test.

Interpretation:
- Compare `internal_128K` and `psram_128K` first; these use equal working-set sizes.
- Compare `psram_128K` and `psram_1M` to see the effect of a larger PSRAM working set.
- Random probe/store ns/op is more relevant to hash-table access than sequential MiB/s, but still omits Stockfish's actual entry layout, hash distribution, search behavior, and multicore contention.
- Repeat the test after reset at least three times. Use the median, not the best run.

## Source provenance

The engine source will be obtained from the official upstream repository:

https://github.com/official-stockfish/Stockfish

The upstream revision used for each port milestone will be pinned and recorded. Local changes will be kept separate and documented.

## Licensing

Stockfish is distributed under GNU GPL v3. See the upstream repository and its Copying.txt for license terms. Any distributed firmware containing Stockfish must comply with the applicable GPL requirements.