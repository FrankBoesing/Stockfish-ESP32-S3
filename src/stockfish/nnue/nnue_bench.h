/*
  Diagnostic-only cycle counters for the ESP32-S3 NNUE benchmark.
  This header has no effect on normal firmware unless
  STOCKFISH_NNUE_BENCHMARK is defined.
*/

#ifndef NNUE_BENCH_H_INCLUDED
#define NNUE_BENCH_H_INCLUDED

#if defined(STOCKFISH_NNUE_BENCHMARK)

#include <cstdint>
#include "esp_cpu.h"

namespace Stockfish::Eval::NNUE::Bench {

enum Stage : uint8_t {
    AccumulatorUpdate,
    FeatureTransform,
    FC0,
    SqrClippedReLU0,
    ClippedReLU0,
    FC1,
    SqrClippedReLU1,
    ClippedReLU1,
    FC2,
    EvaluationTotal,
    AccumulatorFindLast,
    AccumulatorForwardIncremental,
    AccumulatorBackwardIncremental,
    AccumulatorForwardBoth,
    AccumulatorRefreshCache,
    AccumulatorBuildIndices,
    AccumulatorApplyMainLoop,
    AccumulatorApplyPsqtLoop,
    AccumulatorUpdateBoth,
    AccumulatorApplyPieLoop,
    StageCount
};

struct Counter {
    uint64_t cycles;
    uint32_t calls;
};

extern Counter counters[StageCount];

inline uint32_t cycle_count() {
    return static_cast<uint32_t>(esp_cpu_get_cycle_count());
}

inline void record(Stage stage, uint32_t start) {
    counters[stage].cycles += static_cast<uint32_t>(cycle_count() - start);
    ++counters[stage].calls;
}

}  // namespace Stockfish::Eval::NNUE::Bench

#endif  // STOCKFISH_NNUE_BENCHMARK
#endif  // NNUE_BENCH_H_INCLUDED
