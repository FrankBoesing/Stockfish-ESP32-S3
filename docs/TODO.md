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
- [ ] Audit the ESP32-S3 Xtensa LX7 compiler output for Stockfish's 64-bit bitboard and indexing primitives (`__builtin_popcountll`, `__builtin_ctzll`, `__builtin_clzll`, `mul_hi64()` and `Magic::index()`).
  - Inspect the actual PlatformIO Xtensa GCC assembly/disassembly for use of native `NSA/NSAU`, `MULUH/MULSH` and `MAC16` instructions before writing inline assembly or replacing compiler builtins.
  - Benchmark any candidate replacement in isolation and in complete search runs; retain the generic implementation as a reference/fallback.
- [ ] Use the existing NNUE layer-cycle profiler to rank `fc_0`, `fc_1`, `fc_2` and activation costs on the ESP32-S3. The accumulator PIE kernel is already implemented and parity-validated; investigate a separate PIE/DSP implementation for the remaining affine layers only if profiling justifies it.
  - Check `fc_1` and `fc_2` first: their post-activation inputs are the most promising candidates for signed 8-bit dot products, subject to a measured range proof.
  - Treat `fc_0` separately: the feature-transformer output can reach 255 while weights are signed, so neither signed×signed nor unsigned×unsigned multiply-accumulate is a drop-in replacement. Any mixed-sign correction/repacking must be proven exact.
  - Verify accumulator width/overflow behaviour, instruction semantics and scalar parity before enabling a new kernel.
