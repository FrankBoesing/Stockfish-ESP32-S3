# Project TODO

This file contains future development work that is intentionally not part of the completed GUI and opening-book implementation.

## Stockfish engine and NNUE optimization

- [ ] Resume Stockfish search and NNUE performance optimization from the validated ESP32-S3 PIE baseline.
  - Re-establish repeatable search/NPS and NNUE-cycle baselines before changing the implementation.
  - Profile the search hot paths, memory placement and PSRAM/internal-RAM trade-offs.
  - Revisit the current single-worker limit only if per-worker memory and stack requirements can be reduced and measured safely.
  - Review Stockfish memory-prefetch hints. They are currently disabled with `-DNO_PREFETCH`; inspect Xtensa code generation/disassembly before considering enabling them, then compare enabled/disabled performance on the FNK0104B.
