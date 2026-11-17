# NNUE manual DCache-preload benchmark

## Purpose

This document records the no-preload hardware reference for later ESP32-S3
DCache-preload experiments. The reference must be captured before any manual
preload code is added. Keep the raw serial logs with the experiment notes.

**Status: three hardware runs captured.**
All three probe sequences completed. Run 3 was captured after the
LineBB/RayPassBB memory-layout change; its UART output does not include a Git
SHA. The three-run medians are recorded below, but the revision difference
means this is an indicative reference rather than a perfectly controlled
same-binary baseline.

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
comparisons, keep the profiler build, optimization level, test sequence,
source revision and hardware configuration identical; only the preload
experiment should differ.

Run 3 was captured after commit
`3fa924cd15c56c94a70f04b4ee484ef5ab7662f7` (LineBB moved to PSRAM and unused
RayPassBB disabled). The serial log does not print the flashed Git SHA, so
this revision association is based on the capture sequence and is not
independently verified. Runs 1 and 2 belong to the earlier reference setup.

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

Runs 1 and 2 are two complete probe sequences from the earlier supplied
serial capture. Run 3 is the complete serial output supplied separately.
The reported node counts, depths, NPS values and moves are retained exactly
as emitted by the firmware.

| Run | Test | Nodes | Depth | NPS | Best move | Ponder |
|---:|---|---:|---:|---:|---|---|
| 1 | Start position | 30,040 | 13 | 1,526 | e2e4 | e7e6 |
| 1 | Kiwipete | 30,007 | 11 | 1,568 | d5e6 | a6e2 |
| 1 | 5-second search | 10,703 | 14 | 2,034 | e2e4 | e7e6 |
| 2 | Start position | 30,040 | 13 | 1,526 | e2e4 | e7e6 |
| 2 | Kiwipete | 30,007 | 11 | 1,568 | d5e6 | a6e2 |
| 2 | 5-second search | 10,703 | 14 | 2,034 | e2e4 | e7e6 |
| 3 | Start position | 30,040 | 13 | 1,514 | e2e4 | e7e6 |
| 3 | Kiwipete | 30,007 | 11 | 1,557 | d5e6 | a6e2 |
| 3 | 5-second search | 10,703 | 14 | 2,016 | e2e4 | e7e6 |
| Median | Start position | 30,040 | 13 | 1,526 | — | — |
| Median | Kiwipete | 30,007 | 11 | 1,568 | — | — |
| Median | 5-second search | 10,703 | 14 | 2,034 | — | — |

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
| 3 | Start position | 15,875 | 205,649 | 68,629 / 20,772 | 32,075 / 41,845 | 31,760 / 41,845 |
| 3 | Kiwipete | 12,976 | 255,010 | 77,480 / 16,777 | 36,353 / 35,108 | 36,012 / 35,108 |
| 3 | 5-second search | 3,491 | 226,512 | 70,204 / 5,168 | 32,853 / 10,406 | 32,523 / 10,406 |
| Median | Start position | 15,875 | 205,389 | 68,385 / 20,772 | 32,043 / 41,845 | 31,719 / 41,845 |
| Median | Kiwipete | 12,976 | 254,620 | 77,235 / 16,777 | 36,305 / 35,108 | 35,956 / 35,108 |
| Median | 5-second search | 3,491 | 226,424 | 70,048 / 5,168 | 32,846 / 10,406 | 32,509 / 10,406 |

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


### Complete NNUE counters — run 3

Each cell contains `average cycles/call; calls; reported share`. Shares are
copied from the firmware output. Since counters are nested, shares across
rows must not be summed.

| Counter | Start position | Kiwipete | 5-second search |
|---|---:|---:|---:|
| accumulator | 102,900; 15,875; 50.0% | 142,605; 12,976; 55.9% | 121,894; 3,491; 53.8% |
| feature-transform | 15,730; 15,875; 7.6% | 15,978; 12,976; 6.2% | 15,869; 3,491; 7.0% |
| fc0 | 64,302; 15,875; 31.2% | 71,238; 12,976; 27.9% | 65,130; 3,491; 28.7% |
| sqr-relu0 | 815; 15,875; 0.3% | 824; 12,976; 0.3% | 852; 3,491; 0.3% |
| clip-relu0 | 307; 15,875; 0.1% | 314; 12,976; 0.1% | 318; 3,491; 0.1% |
| fc1 | 16,286; 15,875; 7.9% | 18,445; 12,976; 7.2% | 16,816; 3,491; 7.4% |
| sqr-relu1 | 850; 15,875; 0.4% | 877; 12,976; 0.3% | 887; 3,491; 0.3% |
| clip-relu1 | 344; 15,875; 0.1% | 363; 12,976; 0.1% | 365; 3,491; 0.1% |
| fc2 | 2,561; 15,875; 1.2% | 2,728; 12,976; 1.0% | 2,606; 3,491; 1.1% |
| evaluation-total | 205,649; 15,875; 100.0% | 255,010; 12,976; 100.0% | 226,512; 3,491; 100.0% |
| accum-find-last | 889; 15,875; 0.4% | 966; 12,976; 0.3% | 1,020; 3,491; 0.4% |
| accum-forward | 56,047; 186; 0.3% | 72,569; 681; 1.4% | 53,715; 38; 0.2% |
| accum-backward | 2,546; 190; 0.0% | 8,451; 683; 0.1% | 6,242; 40; 0.0% |
| accum-forward-both | 93,252; 15,687; 44.8% | 108,671; 12,294; 40.3% | 107,715; 3,452; 47.0% |
| accum-refresh-cache | 676,132; 190; 3.9% | 631,174; 683; 13.0% | 1,087,560; 40; 5.5% |
| accum-build-indices | 1,457; 20,772; 0.9% | 1,449; 16,777; 0.7% | 1,474; 5,168; 0.9% |
| accum-main-loop | 32,075; 41,845; 41.1% | 36,353; 35,108; 38.5% | 32,853; 10,406; 43.2% |
| accum-psqt-loop | 1,072; 41,845; 1.3% | 1,157; 35,108; 1.2% | 1,060; 10,406; 1.3% |
| accum-update-both | 68,629; 20,772; 43.6% | 77,480; 16,777; 39.2% | 70,204; 5,168; 45.8% |
| accum-pie-loop | 31,760; 41,845; 40.7% | 36,012; 35,108; 38.2% | 32,523; 10,406; 42.7% |

## Interpretation — runs 1 to 3

The accumulator path is a substantial part of the measured NNUE evaluation.
The median `accum-pie-loop` cost is approximately 31.7k–36.0k cycles per
call. The median `fc0` cost is approximately 64.2k–71.1k cycles per
evaluation, or about 28–31% of evaluation time. Both are plausible
optimization targets, but FC0 currently offers the larger measured cost
surface.

Across the three runs, the median `evaluation-total` values are 205,389
cycles for the start position, 254,620 for Kiwipete and 226,424 for the timed
search. Run 3 is only about 0.05–0.15% above the earlier run values, depending
on test. The median NPS values are 1,526, 1,568 and 2,034 respectively.

The close measurements are encouraging. However, runs 1 and 2 predate the
LineBB/RayPassBB memory-layout change, while run 3 was captured afterwards
(the exact flashed SHA is not present in the UART output). Therefore, use
these medians as an indicative no-preload reference. For a strict future A/B
comparison, first capture a fresh baseline and the experimental build from
the same source revision and under identical conditions.

This is **not evidence that manual preloading or an FC0 kernel change will
improve performance**. No optimization variant has been measured. In
particular, do not infer a speedup from the counter shares alone.

Run 3 memory snapshot: after Engine construction, PSRAM free was 91,252
bytes (largest free block 44,020 bytes), and internal free RAM was 151,624
bytes (largest block 110,580 bytes). After the searches, internal minimum
free RAM was 124,232 bytes and PSRAM minimum free was 86,512 bytes. These
values belong to the memory-probe run and should not be generalized to a
different firmware configuration.

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
`Eingefügter Text(3).txt`. Run 3 was supplied as a complete serial capture
in the conversation. No separate raw-log file or checksum has been added to
the repository. Preserve the original captures outside the repository or
attach them as files so the exact runs can be recovered.

The three-run medians are now filled in. Before a strict preload A/B
experiment, capture a same-revision baseline and experimental run so the
source revision and memory layout are held constant.
