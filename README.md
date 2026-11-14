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

Stockfish 19 source is vendored under `src/stockfish/` with the Smallnet P_hm feature patch forward-ported to the pinned SF19 release. Hardware tests have verified Engine construction, embedded NNUE loading, depth-1/4/8 searches and a 5-second timed search with the ILI9341/FT6336U display driver initialized. The single search worker is now detected and pinned to Core 0 while the Arduino application runs on Core 1. The single Stockfish worker runs on Core 0 while the Arduino application runs on Core 1. The tested 30,000-node start-position and Kiwipete searches, plus a 5-second timed search, completed without a task-watchdog report. A one-tick ESP-specific yield is performed at most once per second so Core 0's IDLE task can service the task watchdog. The original 32 KiB search stack overflowed; 128 KiB completed the initial tests but left only 17.7 KiB free in the earlier middlegame position. The 192 KiB PSRAM-backed pthread stack now completes both 30,000-node tests with a measured minimum free stack of 98,320 bytes. This is a measured reserve for these positions, not a guarantee for arbitrary/deep searches. The next probe adds node-bounded max-depth-16 searches in the start position and Kiwipete, and logs the CPU core used by the search worker. This is a validation harness, not yet the final UCI application or touchscreen chess UI.

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

## Compiler optimization comparison

The Stockfish probe can be built with three compiler optimization levels while keeping the source, board settings, CPU clock, NNUE network, hash size, and search tests unchanged:

| PlatformIO environment | Optimization |
| --- | --- |
| `freenove_stockfish_compile` | `-Os` (baseline) |
| `freenove_stockfish_o2` | `-O2` |
| `freenove_stockfish_o3` | `-O3` |

The baseline explicitly pins `-Os`, which is the Arduino-ESP32 3.3.12 release default. The `-O2` and `-O3` environments remove that default before applying the requested level. They also use separate PlatformIO output directories and linker maps. No `-Ofast` or fast-math options are used.

Build and upload one variant at a time:

```sh
pio run -e freenove_stockfish_compile -t upload
pio run -e freenove_stockfish_o2 -t upload
pio run -e freenove_stockfish_o3 -t upload
```

The serial startup message identifies the optimization level. Each variant runs the same 30,000-node start-position search, 30,000-node Kiwipete search, and 5-second timed search. For useful hardware results, repeat each variant after three independent resets and compare the median `nps` values from the three `[SEARCH]` summaries. Keep the board, display state, power supply, monitor connection, and other running tasks unchanged. Also compare the reported firmware size and check for successful completion, watchdog reports, and stack warnings. A CI compile confirms buildability only; it does not establish which optimization is fastest on the ESP32-S3.

GitHub Actions compiles all three variants and retains the firmware binaries and linker maps as a single artifact.

## NNUE cycle profiler

A diagnostic-only build enables cycle counters inside the real Stockfish NNUE evaluation path:

```sh
pio run -e freenove_stockfish_nnubench -t upload
```

The profiler uses the same `-Os` optimization level as the baseline. It instruments the following stages on the search worker:

- Accumulator update (`AccumulatorStack::evaluate`)
- Feature transformation (both perspectives)
- FC0, SqrClippedReLU0, ClippedReLU0
- FC1, SqrClippedReLU1, ClippedReLU1
- FC2
- Complete `Network::evaluate` call

After each 30,000-node search and the timed search, the serial output contains `[NNUEBENCH]` records with invocation count, total cycles, average cycles per invocation, and share of total NNUE evaluation cycles. The counters are reset before each search. Cycle counts are read using ESP-IDF's `esp_cpu_get_cycle_count()` API on the pinned Core 0 worker.

**Important:** This build adds measurement overhead to each NNUE evaluation. Do not compare its search NPS directly with the uninstrumented `freenove_stockfish_compile` build. Use the per-stage cycle distribution to identify expensive components; later optimization changes must be compared with the uninstrumented baseline and checked for identical chess results.

The ordinary `freenove_stockfish_compile` environment does not define `STOCKFISH_NNUE_BENCHMARK`; the instrumentation is compiled out of the normal diagnostic firmware.

The accumulator total is further divided into mutually exclusive update paths: lookup of the last usable accumulator, forward incremental update for one perspective, backward incremental update after a cache refresh, combined forward update for both perspectives, and Finny-cache refresh. These path timings help distinguish the cost of incremental updates from full cache refreshes. Small dispatcher and branch overhead remains included only in the enclosing accumulator total.

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

This environment excludes the board-diagnostics `src/main.cpp` and uses `src/stockfish_probe.cpp` together with the Stockfish sources. The probe initializes the same LovyanGFX ILI9341 driver and FT6336U touch interface used by the board diagnostics before constructing the engine. It then loads and verifies the embedded Stockfish 19 Smallnet network, creates one worker and a 1 MiB hash, and runs max-depth-16 searches capped at 30,000 nodes in both the starting position and canonical Kiwipete, followed by a 5-second timed search. The 30,000-node cap bounds test duration; reaching depth 16 is not guaranteed. Heap and worker-stack high-water checkpoints are recorded with the display driver left initialized, and the CPU core used by the search thread is reported. This remains a diagnostic dashboard, not the final touchscreen chess UI or a complete UCI application.

The panel is 240 x 320 pixels and uses RGB565. A full-screen framebuffer would require 153,600 bytes; two full-screen buffers would require 307,200 bytes. The previous engine-only probe left 327,112 bytes of free PSRAM, so double buffering would consume almost all of that reserve. The current dashboard uses direct display drawing calls and has no explicit full-screen framebuffer. The final chess UI should initially use direct/partial updates or small tile buffers, and should only allocate a full framebuffer if later combined memory measurements show sufficient headroom.

The ESP32-S3 is dual-core, but the current Stockfish port deliberately sets `MaxThreads = 1`. The single Stockfish worker is explicitly pinned to Core 0 so the Arduino/UI task can remain on Core 1. The tested 30,000-node searches and timed search completed without a task-watchdog report, so the ESP-specific one-tick yield is throttled to at most once per second. Stockfish obtains the configured physical core count from `CONFIG_FREERTOS_NUMBER_OF_CORES`, since this Arduino toolchain reported only one through `std::thread::hardware_concurrency()`. The probe reports the setup task's core and the search worker's actual core. True parallel Stockfish search with one worker per core is not enabled. A `Search::Worker` currently has `sizeof(Search::Worker) = 3,451,840` bytes, while the complete one-worker engine run with the 192 KiB search stack leaves about 125 KiB of PSRAM free. A second full worker therefore cannot fit in the present memory layout. Enabling two-worker search will require a separate reduction or redesign of per-worker memory before raising `MaxThreads`.

The search pthread currently requests a 192 KiB stack from PSRAM through the ESP-IDF 5.5 pthread `stack_alloc_caps` setting. This is an experimental starting size: the probe samples `uxTaskGetStackHighWaterMark(nullptr)` from search callbacks and reports the minimum free worker-stack bytes after each completed search. It must be increased if the measured reserve is small; it is not yet validated for arbitrary/deep searches.

PSRAM-backed task stacks have important ESP-IDF restrictions: they are inaccessible while the flash cache is disabled. The search worker must therefore not perform flash erase/write, OTA, NVS operations, or deep-sleep transitions while running. The probe keeps serial output and display updates on the Arduino loop task rather than invoking those drivers from the PSRAM-stacked worker. Treat any future feature that calls ROM routines or disables the flash cache from the search task as requiring a separate safety review. See the ESP-IDF 5.5 external-RAM restrictions and stack-sizing guidance.

The existing `freenove_fnk0104b` environment remains the board-diagnostics target.
