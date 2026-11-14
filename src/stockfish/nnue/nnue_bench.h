/*
  Diagnostic-only cycle counters and PIE parity statistics for the ESP32-S3.
  These declarations have no effect on normal firmware unless their respective
  diagnostic build macros are defined.
*/

#ifndef NNUE_BENCH_H_INCLUDED
#define NNUE_BENCH_H_INCLUDED

#include <cstdint>

#if defined(STOCKFISH_NNUE_BENCHMARK)
#include "esp_cpu.h"
#endif

namespace Stockfish::Eval::NNUE::Bench {

#if defined(STOCKFISH_NNUE_BENCHMARK)

enum Stage : uint8_t {
    AccumulatorUpdate,
    FeatureTransform,
    FC0,
    FC0AccxKernel,
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

#endif  // STOCKFISH_NNUE_BENCHMARK

#if defined(STOCKFISH_ESP32_S3_PIE_VALIDATE)

// Updated only by the single Stockfish search worker in the validation build.
struct PieValidationStats {
    uint64_t calls;
    uint64_t mismatchedCalls;
    uint64_t mismatchedElements;
    uint64_t outOfRangeCalls;
    uint64_t outOfRangeElements;
    uint32_t firstMismatchIndex;
    int32_t firstExpected;
    int32_t firstActual;
    bool hasMismatch;
};

extern PieValidationStats pieValidationStats;

#endif  // STOCKFISH_ESP32_S3_PIE_VALIDATE

#if defined(STOCKFISH_ESP32_S3_FC0_ACCX_VALIDATE)
struct FC0AccxValidationStats {
    uint64_t acceleratedCalls;
    uint64_t scalarFallbackCalls;
    uint64_t inputOutOfRangeCalls;
    uint64_t inputOutOfRangeElements;
    uint64_t mismatchedCalls;
    uint64_t mismatchedElements;
    uint32_t firstMismatchIndex;
    int32_t firstExpected;
    int32_t firstActual;
    bool hasMismatch;
};

extern FC0AccxValidationStats fc0AccxValidationStats;
#endif

}  // namespace Stockfish::Eval::NNUE::Bench

#endif  // NNUE_BENCH_H_INCLUDED
