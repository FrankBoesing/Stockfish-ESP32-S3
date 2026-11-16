# Stockfish ESP32-S3

An experimental, native port of the official Stockfish chess engine to the Espressif ESP32-S3.

## Development checkpoint

The Stockfish 19 Smallnet engine, ESP32-S3 PIE accumulator path, touchscreen application and Polyglot opening-book reader are implemented on the Freenove FNK0104B target. QIO is the configured flash mode. The user has confirmed that the packaged opening book operates on the physical board and that its persistent enable/disable controls work from the GUI and serial console. This file contains detailed implementation and benchmark notes; [docs/STATUS-2026-11-14.md](docs/STATUS-2026-11-14.md) is a chronological development log, with the latest state recorded at its end. See [the book flashing guide](docs/FLASHING-OPENING-BOOK.md).

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

Stockfish 19 Smallnet P_hm is running on the Freenove ESP32-S3 display board. The regular `freenove_stockfish_compile` build enables the ESP32-S3 PIE accumulator kernel, guarded by a network-load-time int16 range proof and automatic scalar fallback. The current network passed an on-device scalar-vs-PIE comparison across 82,976 updates (84,967,424 accumulator lanes) with no mismatches or out-of-range intermediate values. The latest documented QIO hardware run measured 1,552 NPS in the start position, 1,600 NPS in Kiwipete and 2,075 NPS in the 5-second search; these are results from one run, not a statistically controlled benchmark. The GUI, game persistence, serial controls and native rule/Polyglot regression tests are implemented. The user has confirmed opening-book operation on the physical FNK0104B, including the new persistent ON/OFF setting through both input paths. See [the development log](docs/STATUS-2026-11-14.md) for measurements and constraints.

Stockfish's explicit memory-prefetch hints are currently disabled in the
ESP32-S3 PlatformIO builds with `-DNO_PREFETCH`. This makes Stockfish's
`prefetch()` helper a no-op, so the compiler does not emit its
`__builtin_prefetch()` path. These hints target search data such as
transposition-table and history entries; this does **not** disable the ESP32
hardware caches or ESP-IDF cache maintenance. See
[the porting analysis](docs/PORTING_ANALYSIS.md) before changing this setting.

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

The serial command `PRINT` displays the current board using Stockfish's ASCII representation. The board is also printed automatically after loading a position with `POSITION`, `FEN` or `PGN IMPORT`. The serial monitor logs each received command as [SERIAL] RX and each accepted
move in UCI notation as [MOVE], distinguishing the player from Stockfish and
showing the moving colour. A single UCI move such as e2e4 or a promotion such
as e7e8q can be entered directly. Short castling is also accepted as O-O, o-o
or 0-0 and is converted to the corresponding UCI king move. Additional commands
are UNDO (takeback),
HINT (Stockfish suggestion), CALIBRATION (restart touchscreen calibration),
LEVEL [1..20] (set the next game's strength), BOOK [ON|OFF] (show or change
opening-book use), PGN (export) and PGN IMPORT (load a PGN game), and
NEWGAME [Level] [WHITE|BLACK]. During PGN import, paste the complete PGN;
the importer automatically starts when it reaches a result marker (1-0, 0-1,
1/2-1/2 or *). ENDPGN remains available as a manual finish for PGNs without a
result marker; CANCELPGN aborts the import.
The opening-book preference is saved separately in NVS and restored at boot;
installations without a saved value default to BOOK ON. PGN export includes the appropriate move
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
PGN export and import are implemented for serial use. Import resolves SAN against Stockfish's legal moves, supports custom starting positions through FEN/SetUp tags, and ignores comments, NAGs and parenthesized variations. The current game is replaced only after the complete move sequence validates.

Search callbacks only publish compact status/best-move data; drawing and
position updates remain in the Arduino loop task. The GUI deliberately avoids
a full-screen framebuffer.

The touchscreen application is now the project's interactive target and has been exercised on the FNK0104B. The board's coordinate labels have been confirmed readable, and the opening-book control has been checked on hardware.

## Opening book

The 16 MiB flash layout reserves a dedicated 11.875 MiB raw `book` partition
at offset `0x410000`. The selected book is
`lichess_2100_rapid_2026-05.bin` (Polyglot, 1,601,616 bytes, CC0 release).
Keep the raw `.bin` and generated `.sfbook` image local; neither belongs in
the firmware repository.

The GUI reader validates a 24-byte `SFBK` image header, CRC32 and sorted
Polyglot keys, maps only the actual payload with `esp_partition_mmap()`, and
selects a weighted legal book move before the normal Stockfish search. A miss
falls back to Stockfish; HINT continues to use Stockfish. The book can be
enabled or disabled in the New Game menu or with `BOOK ON` / `BOOK OFF`;
`BOOK` reports the current setting. The preference is stored separately in
NVS, defaults to enabled on first use, and survives reboot.

The raw `.bin` format is not accepted by the current reader. To prepare a
replacement image, run:

```powershell
python scripts/prepare_polyglot_book.py .\lichess_2100_rapid_2026-05.bin
```

Flash the resulting `.sfbook` at `0x410000`. The selected image has already
been flashed and the user has confirmed working book moves on the FNK0104B.
See [Flashing an opening book](docs/FLASHING-OPENING-BOOK.md) for the repeatable
procedure.

## Further development

The current touchscreen and opening-book feature set has been exercised on the
FNK0104B. Future work can return to engine/NNUE performance optimization from
the validated PIE baseline or add further chess features. These are development
directions rather than outstanding hardware-validation requirements.

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

The Stockfish source snapshot is included in `src/stockfish/` and is based on the official upstream project:

https://github.com/official-stockfish/Stockfish

The upstream author list and source-file copyright/license headers are retained. ESP32-specific changes and build integration are maintained in this repository. The exact upstream Git revision is not separately recorded as a dedicated submodule or pinned source reference; consult the repository history when tracing individual changes.

## Licensing

The project and integrated Stockfish-derived firmware are distributed under GNU GPL version 3 (GPLv3). The complete license is provided in the repository-root `LICENSE` file; Stockfish's original `src/stockfish/Copying.txt`, `AUTHORS` list and source notices are retained. Third-party assets preserve their own license and attribution notices. Firmware distribution must comply with GPLv3 requirements for the corresponding source and modifications.

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
