#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-g++}"
OUT="${TMPDIR:-/tmp}/stockfish-gui-rules-tests.$$"
trap 'rm -f "$OUT"' EXIT

# Build the actual Stockfish position/move-generation/game-status code. The
# upstream CLI main() is omitted and the NNUE blob is disabled: these tests
# exercise chess rules and GUI state, not evaluation or search strength.
STOCKFISH_SOURCES=(
  src/stockfish/attacks.cpp
  src/stockfish/benchmark.cpp
  src/stockfish/bitboard.cpp
  src/stockfish/evaluate.cpp
  src/stockfish/misc.cpp
  src/stockfish/movegen.cpp
  src/stockfish/movepick.cpp
  src/stockfish/position.cpp
  src/stockfish/search.cpp
  src/stockfish/thread.cpp
  src/stockfish/timeman.cpp
  src/stockfish/tt.cpp
  src/stockfish/uci.cpp
  src/stockfish/ucioption.cpp
  src/stockfish/tune.cpp
  src/stockfish/syzygy/tbprobe.cpp
  src/stockfish/nnue/nnue_accumulator.cpp
  src/stockfish/nnue/nnue_misc.cpp
  src/stockfish/nnue/network.cpp
  src/stockfish/nnue/features/p_hm.cpp
  src/stockfish/engine.cpp
  src/stockfish/score.cpp
  src/stockfish/memory.cpp
  src/stockfish/esp_fatal.cpp
  src/stockfish/position_status.cpp
)

echo "Building native Stockfish GUI rule regression tests..."
"$CXX" -std=c++17 -O2 -pthread \
  -DNNUE_EMBEDDING_OFF -DNO_TABLEBASES -DNO_PREFETCH \
  -Isrc/stockfish -Isrc/gui -Itests/native/stubs \
  tests/native/test_chess_rules.cpp "${STOCKFISH_SOURCES[@]}" \
  -o "$OUT"

echo "Running native Stockfish GUI rule regression tests..."
"$OUT"
