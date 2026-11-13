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

The upstream source has not yet been vendored or added as a submodule.
The next step is to inspect a pinned upstream revision and its build assumptions.
