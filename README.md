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

Stockfish 19 source is vendored under `src/stockfish/` with the Smallnet P_hm feature patch forward-ported to the pinned SF19 release. Hardware tests have verified Engine construction, embedded NNUE loading, depth-1/4/8 searches and a 5-second timed search with the ILI9341/FT6336U display driver initialized. The original 32 KiB search stack overflowed; a 128 KiB PSRAM-backed pthread stack now completes those searches, with more than 61 KiB free in the deepest completed test. The next probe adds node-bounded max-depth-16 searches in the start position and Kiwipete, and logs the CPU core used by the search worker. This is a validation harness, not yet the final UCI application or touchscreen chess UI.

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

This environment excludes the board-diagnostics `src/main.cpp` and uses `src/stockfish_probe.cpp` together with the Stockfish sources. The probe initializes the same LovyanGFX ILI9341 driver and FT6336U touch interface used by the board diagnostics before constructing the engine. It then loads and verifies the embedded Stockfish 19 Smallnet network, creates one worker and a 1 MiB hash, and runs max-depth-16 searches capped at 30,000 nodes in both the starting position and canonical Kiwipete, followed by a 5-second timed search. The 10,000-node cap bounds test duration; reaching depth 16 is not guaranteed. Heap and worker-stack high-water checkpoints are recorded with the display driver left initialized, and the CPU core used by the search thread is reported. This remains a diagnostic dashboard, not the final touchscreen chess UI or a complete UCI application.

The panel is 240 x 320 pixels and uses RGB565. A full-screen framebuffer would require 153,600 bytes; two full-screen buffers would require 307,200 bytes. The previous engine-only probe left 327,112 bytes of free PSRAM, so double buffering would consume almost all of that reserve. The current dashboard uses direct display drawing calls and has no explicit full-screen framebuffer. The final chess UI should initially use direct/partial updates or small tile buffers, and should only allocate a full framebuffer if later combined memory measurements show sufficient headroom.

The ESP32-S3 is dual-core, but the current Stockfish port deliberately sets `MaxThreads = 1`. The single Stockfish worker is now explicitly pinned to Core 0 so the Arduino/UI task can remain on Core 1. Stockfish obtains the configured physical core count from `CONFIG_FREERTOS_NUMBER_OF_CORES`, since this Arduino toolchain reported only one through `std::thread::hardware_concurrency()`. The probe reports the setup task's core and the search worker's actual core. True parallel Stockfish search with one worker per core is not enabled. A `Search::Worker` currently has `sizeof(Search::Worker) = 3,451,840` bytes, while the complete one-worker engine run leaves about 190 KiB of PSRAM free. A second full worker therefore cannot fit in the present memory layout. Enabling two-worker search will require a separate reduction or redesign of per-worker memory before raising `MaxThreads`.

The search pthread currently requests a 128 KiB stack from PSRAM through the ESP-IDF 5.5 pthread `stack_alloc_caps` setting. This is an experimental starting size: the probe samples `uxTaskGetStackHighWaterMark(nullptr)` from search callbacks and reports the minimum free worker-stack bytes after each completed search. It must be increased if the measured reserve is small; it is not yet validated for arbitrary/deep searches.

PSRAM-backed task stacks have important ESP-IDF restrictions: they are inaccessible while the flash cache is disabled. The search worker must therefore not perform flash erase/write, OTA, NVS operations, or deep-sleep transitions while running. The probe keeps serial output and display updates on the Arduino loop task rather than invoking those drivers from the PSRAM-stacked worker. Treat any future feature that calls ROM routines or disables the flash cache from the search task as requiring a separate safety review. See the ESP-IDF 5.5 external-RAM restrictions and stack-sizing guidance.

The existing `freenove_fnk0104b` environment remains the board-diagnostics target.
