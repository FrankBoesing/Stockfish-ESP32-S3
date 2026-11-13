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
- 16 MiB flash and 8 MiB OPI PSRAM, both confirmed by the user's runtime diagnostics.
- ILI9341 SPI display, native 240x320 portrait resolution; diagnostic firmware rotates it to 320x240 landscape.

## Current status

Stockfish 19 source is vendored under `src/stockfish/` with the Smallnet P_hm feature patch forward-ported to the pinned SF19 release. The `freenove_stockfish_compile` PlatformIO environment now runs a dedicated hardware probe: it initializes the ILI9341/FT6336U diagnostic display, loads the embedded Smallnet network, constructs one search worker with a 1 MiB hash, verifies NNUE, and runs depth 1/4/8 plus a 5-second timed search. This is a validation harness, not yet the final UCI application or touchscreen chess UI.

## Planned milestones

1. Inspect upstream Stockfish build system, architecture-specific code, NNUE network loading, threading, and memory allocation.
2. Establish a reproducible PlatformIO environment for the exact Freenove board.
3. Validate Engine construction, embedded NNUE loading, and single-threaded search on hardware.
4. Measure search scaling, internal RAM/PSRAM use, and the combined engine-plus-display memory budget.
5. Build a minimal UCI-capable firmware and validate protocol behavior against a desktop build of the same upstream revision.
6. Add the touchscreen chess UI as a separate layer, avoiding a full-screen double framebuffer unless measurements justify it.

## Memory access benchmark

The FNK0104B diagnostic firmware includes a synthetic memory benchmark after the heap report:

- 128 KiB internal SRAM versus 128 KiB PSRAM, using the same working-set size.
- 1 MiB PSRAM working set for a larger single-core reference.
- 4 MiB and 7 MiB PSRAM tables, tested with one worker and with two workers pinned to different CPU cores.
- Random probe and random store operations on the same shared 16-byte-entry table. The two workers use independent PRNG streams; 32-bit atomic field accesses avoid C++ data races if both select the same entry.
- Per-worker latency and aggregate Mops/s, in addition to the smaller sequential bandwidth tests.

The 16-byte entry is a synthetic stand-in for a compact transposition-table slot. This is **not** a Stockfish benchmark and its result is not equivalent to NPS. The atomic field operations are also not a reproduction of Stockfish's actual TT protocol. The benchmark isolates the cost of randomized PSRAM access, including concurrent traffic from both cores. It allocates and frees each large test table before continuing, so it does not reserve memory for the eventual engine.

The display benchmark compares 40 MHz, 80 MHz and a final 40 MHz restore on the ILI9341. It measures repeated full-screen fills and 160x120 partial fills, then displays a four-quadrant pattern for a visual stability check at each clock. The code restores 40 MHz before the dashboard. 80 MHz is experimental: watch for corrupted pixels, flicker or incomplete updates.

Build and upload the root PlatformIO project using the `freenove_fnk0104b` environment. Capture the complete serial output, especially the lines beginning with `MEMBENCH`, `MEMSHARED`, and `DISPLAYCLOCK`. For a fair comparison, use the same firmware, CPU frequency, power conditions, and monitor setup; do not run Wi-Fi, display animation, or other memory-intensive tasks during the test.

Interpretation:
- Compare `internal_128K` and `psram_128K` first; these use equal working-set sizes.
- Compare `psram_128K` and `psram_1M` to see the effect of a larger PSRAM working set.
- Compare `MEMSHARED` probe/store results for 4 MiB and 7 MiB, first with one core and then with two. Aggregate Mops/s measures total throughput; per-core ns/op shows each worker's observed latency.
- Two-core tests use the same allocated table. They do not emulate Stockfish's complete transposition-table synchronization algorithm.
- Random probe/store ns/op is more relevant to hash-table access than sequential MiB/s, but still omits Stockfish's actual entry layout, hash distribution, and search behavior.
- Compare the two `DISPLAYCLOCK` 40 MHz lines with the 80 MHz line. Visually inspect the screen during each 1.5-second pattern display.
- Repeat the complete test after reset at least three times. Use the median, not the best run. If 80 MHz shows any visual corruption, use the 40 MHz baseline.

## Source provenance

The engine source will be obtained from the official upstream repository:

https://github.com/official-stockfish/Stockfish

The upstream revision used for each port milestone will be pinned and recorded. Local changes will be kept separate and documented.

## Licensing

Stockfish is distributed under GNU GPL v3. See the upstream repository and its Copying.txt for license terms. Any distributed firmware containing Stockfish must comply with the applicable GPL requirements.

## Engine and display memory probe

```sh
pio run -e freenove_stockfish_compile
```

This environment excludes the board-diagnostics `src/main.cpp` and uses `src/stockfish_probe.cpp` together with the Stockfish sources. The probe initializes the same LovyanGFX ILI9341 driver and FT6336U touch interface used by the board diagnostics before constructing the engine. It then loads and verifies the embedded Stockfish 19 Smallnet network, creates one worker and a 1 MiB hash, and runs depth 1, 4, 8 and a 5-second timed search. Heap checkpoints are taken with the display driver left initialized, so the reported engine/search memory is measured with the display layer resident. This remains a diagnostic dashboard, not the final touchscreen chess UI or a complete UCI application.

The panel is 240 x 320 pixels and uses RGB565. A full-screen framebuffer would require 153,600 bytes; two full-screen buffers would require 307,200 bytes. The previous engine-only probe left 327,112 bytes of free PSRAM, so double buffering would consume almost all of that reserve. The current dashboard uses direct display drawing calls and has no explicit full-screen framebuffer. The final chess UI should initially use direct/partial updates or small tile buffers, and should only allocate a full framebuffer if later combined memory measurements show sufficient headroom.

The existing `freenove_fnk0104b` environment remains the board-diagnostics target.
