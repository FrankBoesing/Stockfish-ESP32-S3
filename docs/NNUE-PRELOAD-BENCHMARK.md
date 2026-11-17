# NNUE manual DCache-preload benchmark

## Purpose

This document records the no-preload hardware reference for later ESP32-S3
DCache-preload experiments. The reference must be captured before any manual
preload code is added. Keep the raw serial logs with the experiment notes.

**Status: awaiting the first hardware capture.** No new benchmark numbers are
claimed in this revision.

## Frozen software configuration

- Repository: `FrankBoesing/Stockfish-ESP32-S3`
- Reference source revision: `3bef1021887a0044e5ad50a276e8630452d1c63e`
- Branch at reference setup: `main`
- Board: Freenove ESP32-S3 Display FNK0104B
- SoC: ESP32-S3, 240 MHz
- External RAM: 8 MiB OPI PSRAM
- Flash mode: QIO
- Framework/toolchain recorded for the project: Arduino-ESP32 3.3.12,
  ESP-IDF 5.5.5, PlatformIO Espressif32
- NNUE network: Stockfish 19 Smallnet P_hm,
  `nn-61e7af4bb97d.nnue`, architecture `(768, 1024, 32, 32, 1)`
- Search configuration: one worker, 1 MiB hash
- Build environment: `freenove_stockfish_nnubench`
- Optimization: `-O2` (verified in the current `platformio.ini`)
- Relevant defines: `NO_PREFETCH`, `STOCKFISH_ESP32_S3_PIE=1`,
  `STOCKFISH_NNUE_BENCHMARK=1`
- Manual ESP32-S3 DCache preload: **disabled / not present**
- Stockfish compiler prefetch: disabled via `-DNO_PREFETCH`

The profiler is deliberately used for both the baseline and future preload
variants. Its cycle instrumentation adds overhead, so its NPS must not be
compared directly with the uninstrumented production build. For A/B
comparisons, keep the profiler build, optimization level, test sequence and
hardware configuration identical; only the preload experiment should differ.

## Reproducible test procedure

Build and flash the profiler firmware:

```sh
pio run -e freenove_stockfish_nnubench -t upload
```

Open the serial monitor at 115200 baud and capture the complete output from
reset until `Probe complete`. The probe runs these tests automatically:

1. Start position: maximum depth 16, capped at 30,000 nodes.
2. Kiwipete: maximum depth 16, capped at 30,000 nodes.
3. Start position: 5-second timed search.

Perform **three complete runs**, resetting the board before each run. Do not
change the firmware, CPU frequency, display state, power setup or serial
configuration between runs. Use the median of the three results, not the
fastest result. Keep each full raw serial log.

## Values to retain

For each of the three searches, retain the `[SEARCH]` line and all
`[NNUEBENCH]` lines. The most relevant counters for the preload experiment
are:

- `accum-update-both`: complete combined incremental update for both
  perspectives.
- `accum-pie-loop`: measured ESP32-S3 PIE accumulator kernel invocation.
- `accum-main-loop`: accumulator update loop, including the PIE path.
- `accum-build-indices`: construction of the added/removed feature lists.
- `accumulator`, `feature-transform` and `evaluation-total`: surrounding
  costs for context.

Also retain the FC0/FC1/FC2 and activation counters. They help show whether
the experiment unexpectedly changes unrelated NNUE work.

Counters are nested. In particular, `accum-pie-loop` is inside
`accum-main-loop`, and `accum-main-loop` / `accum-build-indices` are
components of the larger accumulator-update accounting. Do not add their
percentages together.

## Hardware results

The following tables are intentionally unfilled until real FNK0104B serial
logs have been captured. Values must be copied from the board output; do not
estimate or substitute historical NPS values from a different build.

### Search results

| Run | Test | Nodes | Depth | NPS | Best move |
|---:|---|---:|---:|---:|---|
| 1 | Start position | pending | pending | pending | pending |
| 1 | Kiwipete | pending | pending | pending | pending |
| 1 | 5-second search | pending | pending | pending | pending |
| 2 | Start position | pending | pending | pending | pending |
| 2 | Kiwipete | pending | pending | pending | pending |
| 2 | 5-second search | pending | pending | pending | pending |
| 3 | Start position | pending | pending | pending | pending |
| 3 | Kiwipete | pending | pending | pending | pending |
| 3 | 5-second search | pending | pending | pending | pending |
| Median | Start position | pending | pending | pending | — |
| Median | Kiwipete | pending | pending | pending | — |
| Median | 5-second search | pending | pending | pending | — |

### NNUE cycle results

Populate these from the matching `[NNUEBENCH]` output for each test. Record
both average cycles per call and call counts; averages alone can hide a changed
number of accumulator updates.

| Run | Test | eval cycles/eval | accum-update-both avg/calls | accum-main-loop avg/calls | accum-pie-loop avg/calls |
|---:|---|---:|---:|---:|---:|
| 1 | Start position | pending | pending | pending | pending |
| 1 | Kiwipete | pending | pending | pending | pending |
| 1 | 5-second search | pending | pending | pending | pending |
| 2 | Start position | pending | pending | pending | pending |
| 2 | Kiwipete | pending | pending | pending | pending |
| 2 | 5-second search | pending | pending | pending | pending |
| 3 | Start position | pending | pending | pending | pending |
| 3 | Kiwipete | pending | pending | pending | pending |
| 3 | 5-second search | pending | pending | pending | pending |
| Median | Start position | pending | pending | pending | pending |
| Median | Kiwipete | pending | pending | pending | pending |
| Median | 5-second search | pending | pending | pending | pending |

## Interpretation rules for later experiments

- The primary comparison is the median cycle cost of
  `accum-update-both` and `accum-pie-loop`, together with their call counts.
- Search NPS is a secondary whole-engine sanity check, not the sole decision
  metric.
- A preload variant must use the same search positions, node/time limits,
  build optimization, profiler instrumentation and hardware conditions.
- The preload variant must also retain the existing scalar-vs-PIE correctness
  gate. No preload optimization may weaken or bypass that validation.
- A claimed improvement must exceed run-to-run variation and must not cause
  regressions in total NNUE evaluation cycles, NPS, stability or correctness.

## Raw logs and follow-up

Attach or paste the three complete serial logs in the development conversation.
After they are checked, replace the pending cells above with the measured
values and add the raw-log filenames / checksums if the logs are stored outside
the repository. This document then becomes the immutable no-preload comparison
point for subsequent A/B experiments.
