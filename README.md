# Stockfish ESP32-S3

An experimental, native port of the official Stockfish chess engine to the Espressif ESP32-S3.

## Development checkpoint

The engine/NNUE bring-up and the ESP32-S3 PIE optimization have reached a validated baseline. QIO is now the default flash mode. The touchscreen GUI now has a first Polyglot opening-book integration: it maps only the used data bytes from a separate flash partition, validates the image and chooses a legal weighted book move before falling back to Stockfish. Native tests cover the hash and move-decoding logic; hardware validation is still pending. The exact branch, commits, measurements and implementation constraints are recorded in [docs/STATUS-2026-11-14.md](docs/STATUS-2026-11-14.md). See [the book flashing guide](docs/FLASHING-OPENING-BOOK.md) and [the project TODO](docs/TODO.md).

## Project goals

- Start from the official Stockfish source tree; do not inherit code, patches, or limitations from a Teensy port.
- Keep upstream engine logic and NNUE evaluation intact wherever technically possible.
- Isolate ESP32-specific adaptations in a small platform layer and documented build integration.
- Build and test the engine independently of the display UI.
- Use PlatformIO for reproducible ESP32-S3 firmware builds.

## Target hardware

- Freenove ESP32-S3 Display, 2.8-inch 320x240 capacitive-touch variant
- ESP32-S3, dual-core Xtensa LX7
- OPI PSRAM: 8 MiB, confirmed by runtime heap diagnostics.
- Flash: 16 MiB physical capacity confirmed by the user. The project uses `partitions_stockfish_16MB.csv`: one 4 MiB factory app (USB/serial updates, no OTA slots), a dedicated 11.875 MiB read-only raw `book` partition, the existing 20 KiB NVS partition and a 64 KiB coredump partition. Flash mode is QIO.
- ILI9341 SPI display, native 240x320 portrait resolution; diagnostic firmware rotates it to 320x240 landscape.

## Current status

Stockfish 19 Smallnet P_hm is running on the Freenove ESP32-S3 display board. The standard `freenove_stockfish_compile` firmware now enables the ESP32-S3 PIE accumulator kernel by default, guarded by a network-load-time int16 range proof and automatic scalar fallback. The current Smallnet network passed an on-device scalar-vs-PIE comparison across 82,976 updates (84,967,424 accumulator lanes) with zero mismatches and zero out-of-range intermediate values. The regular, uninstrumented PIE firmware completed the start-position, Kiwipete, and 5-second searches at 1,223, 1,297, and 1,561 NPS respectively, with a minimum worker-stack reserve of 98,416 bytes. Engine, NNUE, display, touch, and single-worker search initialization are validated. The project is now moving to the touchscreen chess GUI; further engine/NNUE performance optimization is deliberately deferred and will resume from this recorded baseline. See [the development checkpoint and handoff notes](docs/STATUS-2026-11-14.md) for exact commits, measurements, constraints, and the next work phase.

## Touchscreen GUI prototype

The interactive GUI is built separately from the diagnostic engine probe:

```sh
pio run -e freenove_stockfish_gui -t upload
```

The `freenove_stockfish_compile` environment remains the known-good engine/NNUE
probe and is not replaced by the GUI target. The first GUI implementation uses
the established ILI9341/FT6336U hardware configuration, a dark navy / blue /
orange TeensyFish-inspired theme, a three-point affine touch calibration saved
in NVS, a new-game screen, and a directly rendered 8x8 board. The board rotates
180 degrees for a Black player so the human side is always at the bottom.

Moves are stored in a compact 16-bit UCI move history. Its backing buffer
prefers PSRAM and falls back to internal 8-bit-capable RAM. Stockfish receives
the move list when rebuilding a position and retains its full `StateInfo`
chain for repetition checks. Pawn promotion is handled by an on-screen chooser
for Queen, Rook, Bishop or Knight. Legal destination squares are highlighted
from Stockfish's legal-move list when a piece is selected. The GUI detects
check, checkmate, stalemate, the 50-move rule, threefold repetition and
supported insufficient-material cases from the live Stockfish position.
The GUI automatically ends the game at the 50-move and threefold claim
thresholds; an interactive draw-claim action is not yet implemented.

The selected skill level for the next game is saved immediately in NVS
whenever it changes in the menu. The strength of the current game (along
with player colour and the full move sequence) is stored in a versioned,
checksummed NVS blob after each accepted human move, engine move, undo and
new-game initialization. When a saved game is restored, its original strength
is restored independently of the next-game menu selection.

The serial monitor logs each received command as [SERIAL] RX and each accepted
move in UCI notation as [MOVE], distinguishing the player from Stockfish and
showing the moving colour. A single UCI move such as e2e4 or a promotion such
as e7e8q can be entered directly. Additional commands are UNDO (takeback),
HINT (Stockfish suggestion), STOP (cancel the current search), LEVEL [1..20]
(set the next game's strength), PGN (export the current game in SAN/PGN form),
and NEWGAME [Level] [WHITE|BLACK]. PGN export includes the appropriate move
numbering, check/checkmate suffixes, result and a SetUp/FEN pair for custom
starting positions. Type moves (or uci) to print the current UCI position
command and FEN. Positions can also be loaded through the serial monitor:
```text
position startpos moves e2e4 e7e5
position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
position fen r3k2r/p1ppqpb1/bn2pnp1/2pP4/1p2P3/2N2N2/PPQBBPPP/R3K2R w KQkq - 0 1
fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
```

The UCI form accepts an optional `moves` sequence after either starting
position. The original FEN and move list are persisted, so a custom test
position also survives a reset. FEN piece-letter case is preserved. Invalid
FENs or move sequences are rejected without replacing the current game.
PGN export is a separate planned feature.

Search callbacks only publish compact status/best-move data; drawing and
position updates remain in the Arduino loop task. The GUI deliberately avoids
a full-screen framebuffer.

This is an initial implementation milestone. Touch mapping, rendering, search
responsiveness and move handling still require a hardware test on the FNK0104B.

## Opening-book data

The 16 MiB flash layout reserves a dedicated 11.875 MiB raw `book` partition
at offset `0x410000`. The initial book is
`lichess_2100_rapid_2026-05.bin` (Polyglot, 1,601,616 bytes, CC0 release).
The image has been flashed separately from the firmware and esptool verified
the written bytes. The application-side Polyglot reader is still to be
implemented, so the book is not yet used for move selection.

See [Flashing an opening book](docs/FLASHING-OPENING-BOOK.md) for the
repeatable PowerShell procedure, partition offset and verification output.

## Opening book

The selected initial Polyglot book is `lichess_2100_rapid_2026-05.bin` (CC0
release, 1,601,616 bytes). The raw file is not committed to this repository.
Run `python scripts/prepare_polyglot_book.py <book.bin>` to validate it and
create an `.sfbook` image with entry count and CRC32 metadata. Flash that image
at offset `0x410000`; see [the flashing guide](docs/FLASHING-OPENING-BOOK.md).

The GUI maps only the actual Polyglot payload from the `book` partition. It
checks the image metadata, CRC and key order, then probes legal matching moves
using the Polyglot key. Book moves are weighted by the stored entry weights;
if no legal book move is present, the normal Stockfish search is used. Physical
validation of the mmap and move-selection path remains pending.

## Planned milestones

1. Inspect upstream Stockfish build system, architecture-specific code, NNUE network loading, threading, and memory allocation.
2. Establish a reproducible PlatformIO environment for the exact Freenove board.
3. Validate Engine construction, embedded NNUE loading, and single-threaded search on hardware.
4. Measure search scaling, internal RAM/PSRAM use, and the combined engine-plus-display memory budget.
5. Build a minimal UCI-capable firmware and validate protocol behavior against a desktop build of the same upstream revision.
6. Add the touchscreen chess UI as a separate layer, avoiding a full-screen double framebuffer unless measurements justify it.
7. Add PGN export (SAN notation, tags and game result) from the established UCI move history.

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

The ordinary `freenove_stockfish_compile` environment does not define `STOCKFISH_NNUE_BENCHMARK`; the instrumentation is compiled out of the normal diagnostic firmware. It does include `STOCKFISH_ESP32_S3_PIE`, so the profiler and regular firmware measure/use the same accumulator implementation.

The accumulator total is further divided into mutually exclusive update paths: lookup of the last usable accumulator, forward incremental update for one perspective, backward incremental update after a cache refresh, combined forward update for both perspectives, and Finny-cache refresh. These path timings help distinguish the cost of incremental updates from full cache refreshes. Small dispatcher and branch overhead remains included only in the enclosing accumulator total.
The combined incremental update is additionally decomposed into:
- `accum-build-indices`: building the added/removed feature lists for both colors
- `accum-main-loop`: updating the 1024-element accumulator vectors (counted once per perspective)
- `accum-psqt-loop`: updating the PSQT buckets (counted once per perspective)
- `accum-update-both`: complete per-dirty-state update, including index construction and both perspectives

The main-loop and PSQT-loop counters are nested within `accum-update-both`; do not add their percentages to the enclosing counter. Their sum and the index-building time should approximately explain the helper cost.

## ESP32-S3 PIE accumulator (default engine path)

The regular `freenove_stockfish_compile` environment now enables the Xtensa PIE kernel for the 1024-element int16 accumulator update. This is the normal ESP32-S3 engine firmware; the kernel is no longer an opt-in experiment.

```sh
# Regular, uninstrumented firmware (PIE enabled)
pio run -e freenove_stockfish_compile -t upload

# Diagnostic build using the same PIE kernel
pio run -e freenove_stockfish_nnubench -t upload
```

The profiler build adds cycle counters and prints an `accum-pie-loop` counter. Use `freenove_stockfish_compile` for normal firmware and `freenove_stockfish_nnubench` for instrumented measurements.

The PIE kernel processes eight int16 lanes per iteration with 128-bit loads, saturating subtraction/addition, and a 128-bit store. Because these PIE arithmetic operations saturate while the scalar Stockfish update has wrapping int16 semantics, the kernel is selected only after a network-load-time range proof. For each output lane, the proof checks whether the bias plus any possible subset of up to `FeatureSet::MaxActiveDimensions` feature weights stays in the int16 range. If the proof succeeds, the PIE path is used; if it fails, `apply_features()` uses the unchanged scalar path. The check is performed for the loaded network, so a different or future network can fall back automatically without changing firmware configuration. The regular compile, GUI, and profiler environments inherit `STOCKFISH_ESP32_S3_PIE` from the standard engine build.

The `accum-pie-loop` counter is nested inside `accum-main-loop`; do not add their percentages. Do not add their percentages together.

The PIE path has passed an on-device scalar parity check using the current Smallnet P_hm network. Across the start position, Kiwipete, and a 5-second timed search, the validation build compared 82,976 incremental updates (84,967,424 individual accumulator lanes): zero mismatching updates, zero mismatching lanes, and zero out-of-range intermediate results. The validation build also completed NNUE verification and all three searches. This establishes exact parity for the exercised updates and network; the load-time range proof provides the safety gate for other loaded networks.

### Exact scalar-vs-PIE parity validation

The separate `freenove_stockfish_pievalidate` environment runs the PIE kernel and then independently recomputes every one of the 1024 output lanes with the former scalar update sequence for **every** incremental accumulator update:

```sh
pio run -e freenove_stockfish_pievalidate -t upload
```

The reference calculation mirrors the scalar path's per-operation int16 wrapping. It also records whether any intermediate scalar result would leave the signed int16 range, because such a value would be saturated by the PIE instructions. After each search, the probe prints a `[PIE_VALIDATE]` line. A valid result requires `status=PASS`, `updates` greater than zero, and zero mismatched updates/elements and zero out-of-range updates/elements. `NOT_RUN` means the PIE path was not exercised; it is not a pass. A failure reports the first mismatching lane and its expected/actual values.

This mode deliberately doubles the accumulator-update work and includes the profiler, so its NPS is not a performance result. The reference comparison runs outside the `accum-pie-loop` timing counter. It is a diagnostic build only; normal firmware and the profiler do not compile the reference calculation. CI compiles this environment but cannot execute the hardware parity test.

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
