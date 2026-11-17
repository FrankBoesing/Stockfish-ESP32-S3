# NNUE manual DCache-preload benchmark

## Purpose

This document records the no-preload hardware reference for later ESP32-S3
DCache-preload experiments. The reference must be captured before any manual
preload code is added. Keep the raw serial logs with the experiment notes.

**Status: baseline runs 1 and 2 of 3 captured; one run is pending.**
Two complete hardware captures are recorded. The measurements are highly
repeatable so far, but the final three-run median is still pending.

## Frozen software configuration

- Repository: `FrankBoesing/Stockfish-ESP32-S3`
- Reference source revision: `3bef1021887a0044e5ad50a276e8630452d1c63e`
  (intended benchmark revision; the supplied serial output does not print a
  Git SHA, so the flashed binary's exact source revision is not independently
  verifiable from this capture)
- Branch at reference setup: `main`
- Board: Freenove ESP32-S3 Display FNK0104B
- SoC: ESP32-S3, 240 MHz
- External RAM: 8 MiB OPI PSRAM
- Flash mode: DIO (reported by the ESP-ROM boot logs for runs 1 and 2)
- Framework/toolchain recorded for the project: Arduino-ESP32 3.3.12,
  ESP-IDF 5.5.5, PlatformIO Espressif32
- NNUE network: Stockfish 19 Smallnet P_hm,
  `nn-61e7af4bb97d.nnue`, architecture `(768, 1024, 32, 32, 1)`
- Search configuration: one worker, 1 MiB hash
- Build environment: `freenove_stockfish_nnubench`
- Optimization: `-O2` (verified in the reference `platformio.ini`)
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

### Search results

Runs 1 and 2 are two complete probe sequences from the supplied serial
capture. Both 30,000-node searches ended at 30,040 and 30,007 nodes
respectively; the reported values are retained exactly as emitted by the
firmware. The NPS values are identical between the two runs.

| Run | Test | Nodes | Depth | NPS | Best move | Ponder |
|---:|---|---:|---:|---:|---|---|
| 1 | Start position | 30,040 | 13 | 1,526 | e2e4 | e7e6 |
| 1 | Kiwipete | 30,007 | 11 | 1,568 | d5e6 | a6e2 |
| 1 | 5-second search | 10,703 | 14 | 2,034 | e2e4 | e7e6 |
| 2 | Start position | 30,040 | 13 | 1,526 | e2e4 | e7e6 |
| 2 | Kiwipete | 30,007 | 11 | 1,568 | d5e6 | a6e2 |
| 2 | 5-second search | 10,703 | 14 | 2,034 | e2e4 | e7e6 |
| 3 | Start position | pending | pending | pending | pending | pending |
| 3 | Kiwipete | pending | pending | pending | pending | pending |
| 3 | 5-second search | pending | pending | pending | pending | pending |
| Median | Start position | pending | pending | pending | — | — |
| Median | Kiwipete | pending | pending | pending | — | — |
| Median | 5-second search | pending | pending | pending | — | — |

### NNUE cycle results

Values below are average cycles per evaluation. The selected accumulator
counters also include calls so changes in call count remain visible.

| Run | Test | Evaluations | Evaluation cycles/eval | accum-update-both avg/calls | accum-main-loop avg/calls | accum-pie-loop avg/calls |
|---:|---|---:|---:|---:|---:|---:|
| 1 | Start position | 15,875 | 205,389 | 68,385 / 20,772 | 32,043 / 41,845 | 31,719 / 41,845 |
| 1 | Kiwipete | 12,976 | 254,617 | 77,230 / 16,777 | 36,304 / 35,108 | 35,955 / 35,108 |
| 1 | 5-second search | 3,491 | 226,424 | 70,048 / 5,168 | 32,846 / 10,406 | 32,509 / 10,406 |
| 2 | Start position | 15,875 | 205,386 | 68,384 / 20,772 | 32,043 / 41,845 | 31,719 / 41,845 |
| 2 | Kiwipete | 12,976 | 254,620 | 77,235 / 16,777 | 36,305 / 35,108 | 35,956 / 35,108 |
| 2 | 5-second search | 3,491 | 226,388 | 70,037 / 5,168 | 32,840 / 10,406 | 32,503 / 10,406 |
| 3 | Start position | pending | pending | pending | pending | pending |
| 3 | Kiwipete | pending | pending | pending | pending | pending |
| 3 | 5-second search | pending | pending | pending | pending | pending |
| Median | Start position | pending | pending | pending | pending | pending |
| Median | Kiwipete | pending | pending | pending | pending | pending |
| Median | 5-second search | pending | pending | pending | pending | pending |

### Complete NNUE counters — run 1

Each cell contains `average cycles/call; calls; reported share`. Shares are
copied from the firmware output. Since counters are nested, shares across
rows must not be summed.

| Counter | Start position | Kiwipete | 5-second search |
|---|---:|---:|---:|
| accumulator | 102,520; 15,875; 49.9% | 142,113; 12,976; 55.8% | 121,609; 3,491; 53.7% |
| feature-transform | 15,759; 15,875; 7.6% | 15,989; 12,976; 6.2% | 15,892; 3,491; 7.0% |
| fc0 | 64,181; 15,875; 31.2% | 71,149; 12,976; 27.9% | 65,039; 3,491; 28.7% |
| sqr-relu0 | 813; 15,875; 0.3% | 827; 12,976; 0.3% | 847; 3,491; 0.3% |
| clip-relu0 | 333; 15,875; 0.1% | 344; 12,976; 0.1% | 346; 3,491; 0.1% |
| fc1 | 16,383; 15,875; 7.9% | 18,565; 12,976; 7.2% | 16,940; 3,491; 7.4% |
| sqr-relu1 | 748; 15,875; 0.3% | 776; 12,976; 0.3% | 783; 3,491; 0.3% |
| clip-relu1 | 349; 15,875; 0.1% | 363; 12,976; 0.1% | 369; 3,491; 0.1% |
| fc2 | 2,851; 15,875; 1.3% | 3,000; 12,976; 1.1% | 2,917; 3,491; 1.2% |
| evaluation-total | 205,389; 15,875; 100.0% | 254,617; 12,976; 100.0% | 226,424; 3,491; 100.0% |
| accum-find-last | 840; 15,875; 0.4% | 894; 12,976; 0.3% | 962; 3,491; 0.4% |
| accum-forward | 55,926; 186; 0.3% | 72,563; 681; 1.4% | 53,816; 38; 0.2% |
| accum-backward | 2,610; 190; 0.0% | 8,573; 683; 0.1% | 6,348; 40; 0.0% |
| accum-forward-both | 93,056; 15,687; 44.7% | 108,386; 12,294; 40.3% | 107,643; 3,452; 47.0% |
| accum-refresh-cache | 675,810; 190; 3.9% | 631,156; 683; 13.0% | 1,087,604; 40; 5.5% |
| accum-build-indices | 1,565; 20,772; 0.9% | 1,604; 16,777; 0.8% | 1,591; 5,168; 1.0% |
| accum-main-loop | 32,043; 41,845; 41.1% | 36,304; 35,108; 38.5% | 32,846; 10,406; 43.2% |
| accum-psqt-loop | 1,022; 41,845; 1.3% | 1,105; 35,108; 1.1% | 1,015; 10,406; 1.3% |
| accum-update-both | 68,385; 20,772; 43.5% | 77,230; 16,777; 39.2% | 70,048; 5,168; 45.7% |
| accum-pie-loop | 31,719; 41,845; 40.7% | 35,955; 35,108; 38.2% | 32,509; 10,406; 42.7% |

### Complete NNUE counters — run 2

Each cell contains `average cycles/call; calls; reported share`. Shares are
copied from the firmware output. Since counters are nested, shares across
rows must not be summed.

| Counter | Start position | Kiwipete | 5-second search |
|---|---:|---:|---:|
| accumulator | 102,516; 15,875; 49.9% | 142,117; 12,976; 55.8% | 121,565; 3,491; 53.6% |
| feature-transform | 15,763; 15,875; 7.6% | 15,988; 12,976; 6.2% | 15,902; 3,491; 7.0% |
| fc0 | 64,179; 15,875; 31.2% | 71,148; 12,976; 27.9% | 65,024; 3,491; 28.7% |
| sqr-relu0 | 814; 15,875; 0.3% | 827; 12,976; 0.3% | 846; 3,491; 0.3% |
| clip-relu0 | 333; 15,875; 0.1% | 344; 12,976; 0.1% | 349; 3,491; 0.1% |
| fc1 | 16,382; 15,875; 7.9% | 18,564; 12,976; 7.2% | 16,969; 3,491; 7.4% |
| sqr-relu1 | 747; 15,875; 0.3% | 776; 12,976; 0.3% | 777; 3,491; 0.3% |
| clip-relu1 | 349; 15,875; 0.1% | 364; 12,976; 0.1% | 369; 3,491; 0.1% |
| fc2 | 2,851; 15,875; 1.3% | 3,000; 12,976; 1.1% | 2,913; 3,491; 1.2% |
| evaluation-total | 205,386; 15,875; 100.0% | 254,620; 12,976; 100.0% | 226,388; 3,491; 100.0% |
| accum-find-last | 841; 15,875; 0.4% | 893; 12,976; 0.3% | 961; 3,491; 0.4% |
| accum-forward | 55,926; 186; 0.3% | 72,550; 681; 1.4% | 53,219; 38; 0.2% |
| accum-backward | 2,611; 190; 0.0% | 8,575; 683; 0.1% | 6,356; 40; 0.0% |
| accum-forward-both | 93,051; 15,687; 44.7% | 108,392; 12,294; 40.3% | 107,613; 3,452; 47.0% |
| accum-refresh-cache | 675,814; 190; 3.9% | 631,158; 683; 13.0% | 1,087,580; 40; 5.5% |
| accum-build-indices | 1,565; 20,772; 0.9% | 1,604; 16,777; 0.8% | 1,591; 5,168; 1.0% |
| accum-main-loop | 32,043; 41,845; 41.1% | 36,305; 35,108; 38.5% | 32,840; 10,406; 43.2% |
| accum-psqt-loop | 1,022; 41,845; 1.3% | 1,106; 35,108; 1.1% | 1,017; 10,406; 1.3% |
| accum-update-both | 68,384; 20,772; 43.5% | 77,235; 16,777; 39.2% | 70,037; 5,168; 45.7% |
| accum-pie-loop | 31,719; 41,845; 40.7% | 35,956; 35,108; 38.2% | 32,503; 10,406; 42.7% |

## Initial interpretation — runs 1 and 2

The accumulator path is a substantial part of the measured NNUE evaluation.
The reported `accum-pie-loop` average is approximately 31.7k–36.0k cycles
per call in both captures. This makes the accumulator path a reasonable target
for a carefully isolated DCache-preload overlap experiment.

The two runs are exceptionally close: average `evaluation-total` differs by
3 cycles for the start position (205,389 vs. 205,386), 3 cycles for Kiwipete
(254,617 vs. 254,620), and 36 cycles for the timed search (226,424 vs.
226,388). The integer NPS output is identical for all three searches in both
runs. This is strong evidence of repeatability for this specific profiler
setup, but two runs are not a substitute for the planned third capture.

This is **not evidence that manual preloading will improve performance**.
No preload variant has been measured. In particular, do not infer a speedup
from the counter shares alone.

The reported `accum-refresh-cache` average is based on relatively few calls
(190, 683 and 40 respectively); it is therefore especially sensitive to
individual cache-refresh events and should not be treated as a stable
per-evaluation cost.

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

Runs 1 and 2 were supplied together in the conversation attachment
`Eingefügter Text(3).txt`. No separate raw-log file or checksum has been added
to the repository. Keep the original capture outside the repository or attach
it as a file so the exact runs can be recovered.

After run 3 is checked, replace the pending cells above with measured values
and calculate medians independently for each test and metric. Add the raw-log
filename / checksum if the log is stored outside the repository.
This document then becomes the no-preload comparison point for subsequent
A/B experiments.
