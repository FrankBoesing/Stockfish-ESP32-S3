# Upstream Stockfish

Upstream repository: https://github.com/official-stockfish/Stockfish

## Policy

- The official repository is the sole engine source.
- Do not import code from Teensy ports or unrelated ESP32 chess engines.
- Pin a specific upstream commit before integrating source.
- Keep platform adaptations outside upstream engine files where possible.
- Any required patch must be documented with rationale and upstream impact.
- NNUE network files and their exact revisions must be tracked and verified.

## Current state

The upstream source is vendored as a pinned source snapshot.
The next step is to inspect a pinned upstream revision and its build assumptions.


## Pinned source snapshot

The ESP32-S3 compile target vendors Stockfish 19 under `src/stockfish/`.

- Upstream: https://github.com/official-stockfish/Stockfish
- Release: `sf_19`
- Release commit: `edb0d9db6731067ec50ce619ff372b463bc4dd5d`
- NNUE feature patch: `sf_19_smallnet` patch 1, forward-ported from base `3f6f417b87c0e80ee30914b6b539b4ab7d3b2a5b`
- Patch 2 (WebAssembly-specific) is not applied.
- Upstream `Copying.txt` and `AUTHORS` are included.

The diagnostic target excludes Stockfish. The separate `freenove_stockfish_compile` environment is a compile/link probe only; runtime UCI search is not implemented yet.
