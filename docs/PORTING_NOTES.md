# Porting notes

## Principles

- Upstream-first: preserve official Stockfish engine logic.
- No Teensy source, patches, build flags, or assumptions are used.
- Do not disable NNUE, Syzygy, threading, or search features speculatively.
- Every ESP32-specific change must be minimal, isolated, justified, and measured.
- Pin the upstream Stockfish commit used for each successful milestone.

## Initial investigation checklist

- [ ] Record the exact Stockfish upstream commit and network filenames.
- [ ] Inspect Stockfish's current C++ language requirements and compiler assumptions.
- [ ] Inspect SIMD use in NNUE and determine whether a scalar fallback is available.
- [ ] Inspect thread implementation and available ESP-IDF/FreeRTOS primitives.
- [ ] Inspect memory allocation and alignment requirements.
- [ ] Determine whether the embedded NNUE network fits in flash and how it is accessed.
- [ ] Confirm the actual Freenove board ID, flash size, PSRAM size/type, and PlatformIO platform package.
- [ ] Build a minimal serial UCI target before display integration.

## PlatformIO caution

The initial board identifier is provisional. Do not assume that the named Freenove board profile exactly matches every Freenove 2.8-inch capacitive-touch board revision. In particular, flash and PSRAM mode settings must come from the actual module configuration, not guesswork.

## Baseline validation

The initial firmware should support at least:

- `uci`
- `isready`
- `ucinewgame`
- `position startpos`
- `go depth 1` (subject to memory feasibility)
- `stop`
- `quit`

Compare legal best moves and protocol behavior with a desktop build from the same upstream revision.
