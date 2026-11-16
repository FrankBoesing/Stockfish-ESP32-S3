# Porting notes

## Principles

- Upstream-first: preserve official Stockfish engine logic.
- No Teensy source, patches, build flags, or assumptions are used.
- Do not disable NNUE, Syzygy, threading, or search features speculatively.
- Every ESP32-specific change must be minimal, isolated, justified, and measured.
- Pin the upstream Stockfish commit used for each successful milestone.

## Confirmed board identity

The target is the Freenove ESP32-S3 Display 2.8-inch capacitive-touch variant, model **FNK0104B**.

Freenove's documentation identifies:
- Display controller: ILI9341
- Native panel resolution: 240 x 320 (landscape operation can rotate the panel)
- Capacitive touch: touch variant FNK0104B

## Confirmed hardware configuration

The physical FNK0104B unit has 16 MiB flash and 8 MiB OPI PSRAM. The GUI uses
the ILI9341 at 320 × 240 in landscape orientation with the FT6336U touch
controller. The active PlatformIO configuration targets the Freenove board
and uses QIO flash mode plus the project-specific 16 MiB partition table.
The old generic DevKit/temporary-profile warning was superseded by the
validated board configuration and no longer applies.

## Scope of these notes

This file records initial porting principles and target-specific constraints
established during bring-up. It is not an active task checklist. Completed
hardware, engine and GUI validation milestones are recorded in
[README-extended.md](../README-extended.md) and the dated
[development log](STATUS-2026-11-14.md).

## PlatformIO configuration

The checked-in target configuration is specific to the confirmed FNK0104B
hardware. Changes to QIO flash mode, OPI PSRAM settings or the 16 MiB partition
layout must be validated against the physical board and the existing NVS/book
partition requirements.

## Baseline validation

The initial firmware should support at least:

- `uci`
- `isready`
- `ucinewgame`
- `position startpos`
- `go depth 1` (subject to memory feasibility)
- `stop`
- `quit`

Compare legal best moves and protocol behavior with a desktop build from the same
upstream revision.
