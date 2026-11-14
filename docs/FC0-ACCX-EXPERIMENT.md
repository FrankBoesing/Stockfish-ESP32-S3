# ESP32-S3 FC0 ACCX experiment

Status: experimental implementation committed to `stockfish-port-analysis`.
The normal `freenove_stockfish_compile` and GUI builds remain on the existing
input-major scalar FC0 path unless the dedicated ACCX macro is enabled.

## Kernel dimensions and layout

The current Stockfish network architecture defines:

- Feature Transformer output / FC0 input: 1024 unsigned bytes
- FC0 output: 32 signed 32-bit values
- FC0 weights: 32 × 1024 signed bytes

The FC0 layer is therefore 1024 inputs by 32 outputs. The 768 value in the
network description is the feature-set input dimension before the 1024-value
Feature Transformer output; it is not the FC0 input dimension.

The experimental build keeps FC0 weights in their native output-major order.
For each output channel, the kernel processes the 1024 input/weight pairs in
64 groups of 16. It uses the ESP32-S3 PIE instruction
`EE.VMULAS.S8.ACCX` to accumulate 16 signed 8-bit products into the 40-bit
ACCX scalar accumulator. ACCX is cleared for each output channel, the dot
product is extracted as a 32-bit value, and the bias is added afterward.

The input values are produced by the Feature Transformer as
`(clamp16(a, 0, 255) * clamp16(b, 0, 255)) / 512`; their maximum is 127.
Consequently the unsigned input bytes are also valid signed 8-bit operands.

## Arithmetic safety

At network load time, a bound is calculated for every output channel:

- upper bound = bias + 127 × sum(positive weights)
- lower bound = bias + 127 × sum(negative weights)

These bounds include every possible intermediate subset sum because all
inputs are non-negative. ACCX is selected only when both bounds fit in signed
32-bit. This also keeps the 40-bit ACCX accumulator well inside its range.
If the proof fails, the experimental build uses a scalar output-major fallback.

The validation build also checks every runtime FC0 input byte is at most 127.
For every accelerated FC0 call, it computes all 32 outputs again with an
independent scalar reference and counts exact mismatches.

## Build and test

Validation build (flash this first):

```powershell
pio run -e freenove_stockfish_fc0_accx_validate -t upload
```

Expected serial marker:

```text
[FC0_ACCX_VALIDATE] ... status=PASS ... scalar_fallback_calls=0 mismatched_calls=0 mismatched_elements=0 input_out_of_range_calls=0
```

The search/profile run is diagnostic only; validation performs a second scalar
FC0 computation and its NPS must not be used as a performance result.

After a complete validation PASS, build the instrumented profiler:

```powershell
pio run -e freenove_stockfish_fc0_accx_bench -t upload
```

Compare its FC0 and `fc0-accx-kernel` cycle counters with the existing
`freenove_stockfish_nnubench` baseline. The profiler adds cycle-counter
instrumentation but does not compute a second scalar FC0 result.

For the final NPS comparison, use the uninstrumented ACCX build:

```powershell
pio run -e freenove_stockfish_fc0_accx -t upload
```

Compare its start-position, Kiwipete and timed-search NPS against
`freenove_stockfish_compile` with identical hash, clock, node/time limits and
search settings. Do not use the profiler or validation NPS as the final
performance figure.

## Important

- This kernel uses ACCX, not the 20-bit-per-lane QACC accumulator. The FC0
  reduction has 1024 terms per output, so QACC.S8's 20-bit lane saturation
  would require an additional chunk/flush scheme.
- The default firmware, GUI, PIE accumulator, NNUE weights, search settings,
  and optimizer flags are not changed by this experiment.
- Hardware validation and cycle measurements are still required. A successful
  CI compile proves assembler/compiler acceptance, not runtime parity or speed.
