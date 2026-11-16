# Project TODO

This file contains future development work that is intentionally not part of the completed GUI and opening-book implementation.

## Stockfish engine and NNUE optimization

- [ ] Resume Stockfish search and NNUE performance optimization from the validated ESP32-S3 PIE baseline.
  - Re-establish repeatable search/NPS and NNUE-cycle baselines before changing the implementation.
  - Profile the search hot paths, memory placement and PSRAM/internal-RAM trade-offs.
  - Revisit the current single-worker limit only if per-worker memory and stack requirements can be reduced and measured safely.
  - Review Stockfish's compiler-level `__builtin_prefetch()` hints separately from ESP32-S3 SoC cache preloading. Keep `-DNO_PREFETCH` until the Xtensa compiler output has been inspected; do not assume that removing the define will produce a useful instruction.

## ESP32-S3 cache and Xtensa-native optimization

- [ ] Evaluate the ESP32-S3 DCache manual-preload and auto-preload mechanisms for selected PSRAM-backed Stockfish data (for example, bounded TT or history regions).
  - Check the exact ESP-IDF 5.5.5 API and its support status first. The low-level ROM/HAL preload interfaces may be internal or explicitly unsuitable for direct application use.
  - Confirm valid address ranges, cache-line/block alignment, maximum preload size, completion/wait semantics, and effects of the shared DCache and the other core.
  - Prototype only a narrowly bounded region with predictable near-future use; compare against ordinary cached access and the existing cache behaviour. Do not add unconditional or broad preloading without repeatable gains.
- [x] Inspect the ESP32-S3 Xtensa GCC code generation for Stockfish's 64-bit bitboard and indexing primitives (PlatformIO Xtensa GCC 14.2.0, `-O2`).
  - `mul_hi64()` uses native `MULUH/MULL`; `Magic::index()` uses two `MULL` operations, XOR and shift.
  - `lsb()` and `msb()` use native `NSAU` instructions.
  - `popcountll()` resolves to the ROM helper `__popcountdi2` from the Magic-table initialization path; `__ctzdi2` calls in `position.cpp` are in `Position::set()` (position/FEN setup), not the normal move-generation path.
  - The current `-DNO_PREFETCH` build emits no compiler-prefetch instruction. A simple scalar int16 dot-product did not auto-lower to `MAC16`.
  - See [the measured code-generation notes](PORTING_ANALYSIS.md#11-tatsächliche-xtensa-codegenerierung-auf-dem-esp32-s3--2026-11-16).
- [ ] Benchmark a custom bit-count or scan implementation only if a profiling result identifies a runtime hotspot; retain the generic/compiler implementation as the reference and verify exact parity.
- [ ] Use the existing NNUE layer-cycle profiler to rank `fc_0`, `fc_1`, `fc_2` and activation costs on the ESP32-S3. The accumulator PIE kernel is already implemented and parity-validated; investigate a separate PIE/DSP implementation for the remaining affine layers only if profiling justifies it.
  - Check `fc_1` and `fc_2` first: their post-activation inputs are the most promising candidates for signed 8-bit dot products, subject to a measured range proof.
  - Treat `fc_0` separately: the feature-transformer output can reach 255 while weights are signed, so neither signed×signed nor unsigned×unsigned multiply-accumulate is a drop-in replacement. Any mixed-sign correction/repacking must be proven exact.
  - Verify accumulator width/overflow behaviour, instruction semantics and scalar parity before enabling a new kernel.

## GUI and user interaction

- [ ] Remove the serial-console `STOP` command from the user-facing command parser and help text. Keep the internal `stopSearch()` functionality where it is required to manage searches safely.
- [ ] Add a serial-console `CALIBRATION` command to start touch calibration again on demand.
- [ ] Implement PGN import so a game can be loaded from PGN notation, including move parsing and validation before replacing the current game.
- [ ] Review the right-hand information panel for text that is too long and currently overlaps or extends into the chessboard. Shorten, wrap, clip or otherwise constrain those labels to the panel width.
- [ ] Make opening-book use dependent on the selected Stockfish skill level: use the book only at or above a configurable minimum level. Decide the threshold before implementation.
