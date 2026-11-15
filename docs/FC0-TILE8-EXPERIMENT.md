# ESP32-S3 FC0 TILE8 experiment

This experiment evaluates the 1024-input / 32-output FC0 sparse affine layer
using the existing ESP32-S3 input-major weight layout.

## Kernel

The output dimension is split into four tiles of eight channels. For each
tile, eight explicit 32-bit accumulators are initialized from the corresponding
bias values. The input vector is scanned in ascending order; when an input is
nonzero, its eight contiguous weights for the current tile are multiplied by
the input and added to the eight accumulators. The eight results are written
once after the scan.

The per-output input accumulation order remains identical to the original
scalar implementation. The experiment does not alter the normal ESP32-S3 path
or the separate FC0 ACCX experiment.

The intended performance trade-off is fewer repeated output loads/stores and
register-resident accumulators versus four scans of the input vector and four
passes over active weight rows. Compiler output must be inspected to determine
whether the accumulators remain in registers or spill.

## Build targets

- `pio run -e freenove_stockfish_fc0_tile8_validate -t upload`
  Adds an exact scalar parity check after every accelerated FC0 call. Do not
  use its NPS as a performance result.
- `pio run -e freenove_stockfish_fc0_tile8_bench -t upload`
  Measures the TILE8 kernel separately, without the duplicate scalar check.
- `pio run -e freenove_stockfish_fc0_tile8 -t upload`
  Uninstrumented search performance build.

The validation output starts with `[FC0_TILE8_VALIDATE]`. A PASS requires
at least one call and zero mismatched calls/elements.

## Evaluation

Compare the instrumented kernel timing against the previous scalar and ACCX
measurements, then compare NPS using uninstrumented builds under identical
conditions. Retain TILE8 only if the hardware measurements show a repeatable
benefit. The standard firmware continues to use the existing scalar kernel.
