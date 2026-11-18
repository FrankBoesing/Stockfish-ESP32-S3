# Serial Protocol LICHESS-BRIDGE/1

[English](serial-protocol-en.md) | [Deutsch](serial-protocol-de.md)

## Transport

- Default baud rate: 115200; configurable in `config.toml`.
- 8 data bits, no parity, 1 stop bit (8N1).
- Text encoding: UTF-8. All defined commands and responses are ASCII-compatible.
- Each message ends with LF (`\n`). The PC accepts LF and CRLF.
- Exactly one complete command or response is sent per line.
- The PC sends `POSITION`, one space, and the complete six-field FEN on the same line.
- The MCU must buffer a complete input line before processing it.

## PC → Microcontroller

| Command | Meaning |
|---|---|
| `BOTMODE` | Enter bridge mode. |
| `PING` | Check the serial link. |
| `NEWGAME` | Reset game and engine state for a new Lichess game. |
| `POSITION <FEN>` | Load a complete standard-chess position. |
| `GO <movetime_ms>` | Calculate one move using the supplied search-time budget in milliseconds. |
| `STOP` | Cancel the current search or pending book move. |
| `EXITBOT` | Leave bridge mode and restore the previous local GUI game when supported. |

## Microcontroller → PC

| Response | Meaning |
|---|---|
| `READY LICHESS-BRIDGE/1 <device_name>` | Bridge mode is active and the device has identified itself. |
| `PONG` | Reply to `PING`. |
| `OK NEWGAME` | `NEWGAME` has been processed. |
| `OK POSITION` | The FEN has been accepted and loaded. |
| `BESTMOVE <uci_move>` | A legal move in UCI notation, for example `e2e4` or `e7e8q`. |
| `BESTMOVE 0000` | No legal move exists in the current position. |
| `INFO <text>` | Optional protocol information. |
| `#LOG <text>` | Optional diagnostic message; not a command response. |
| `ERROR <code> <text>` | Invalid command, invalid position or another protocol error. |

The current MCU firmware also emits plain diagnostic lines such as `[BOOK]`, `[SEARCH]` and `[MOVE]`. The Python bridge ignores these as non-protocol output. New MCU implementations should preferably prefix diagnostic-only lines with `#LOG ` so that they can be handled as informational messages.

## Required command sequence

1. The PC opens the configured serial port and sends `BOTMODE`.
2. The MCU responds with `READY LICHESS-BRIDGE/1 <device_name>`.
3. The PC sends `PING`; the MCU responds with `PONG`.
4. At the beginning of a game, the PC sends `NEWGAME` and waits for `OK NEWGAME`.
5. Whenever it is the bot's turn, the PC sends `POSITION <FEN>` and waits for `OK POSITION`.
6. The PC then sends `GO <movetime_ms>` and waits for `BESTMOVE`.
7. The PC validates the move against the current Lichess position. Only a legal move is submitted to Lichess.
8. On a search timeout, the PC sends `STOP` and does not submit a move from the cancelled search.
9. On shutdown, the PC sends `EXITBOT` when possible.

A normal move cycle is therefore:

```text
PC -> MCU: POSITION rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1
MCU -> PC: OK POSITION
PC -> MCU: GO 20000
MCU -> PC: BESTMOVE e7e5
```

The FEN in this example is illustrative. It must describe the actual current Lichess game position.

## Parsing and state requirements

- Commands and responses are line-oriented. Do not merge multiple messages into one line.
- A `POSITION` command contains exactly six standard FEN fields. The current PC bridge sends a full FEN, not a move list.
- The position must be validated before returning `OK POSITION`. If it is invalid, return `ERROR BAD_FEN <text>`.
- `GO` requires a previously accepted position and a positive decimal `movetime_ms` value. The reference firmware accepts values in the unsigned 32-bit range.
- Do not send an immediate acknowledgement for `GO`. It starts an asynchronous operation; the eventual result is `BESTMOVE`.
- Do not block the main MCU loop while searching. The firmware must remain able to read and process `STOP`.
- A `BESTMOVE` response must be legal in the exact position most recently supplied with `POSITION`. Promotion moves use the lowercase UCI suffix `q`, `r`, `b` or `n`.
- Return `BESTMOVE 0000` only when the position has no legal moves. Do not use it for a failed or interrupted search.
- A successful `STOP` cancels the pending result. The reference firmware emits `INFO STOP acknowledged` and does not return a `BESTMOVE` for that cancelled search.
- Return errors as `ERROR <code> <text>`. The Python bridge treats protocol errors as failures for the current operation.
- A new `POSITION` replaces the previously accepted position.
- The MCU and PC must not process overlapping searches or interleave replies from different commands.

The current reference parser limits the FEN text to 192 bytes. Other MCU implementations should support at least that length.

## MCU firmware behaviour

The Stockfish-ESP32-S3 reference firmware implements the protocol in a dedicated serial `BOTMODE`. The normal GUI serial console remains available when the bridge mode is inactive.

- `GO` starts an asynchronous Stockfish search using the requested `movetime_ms`. The main loop stays responsive and can process `STOP`.
- If the Polyglot opening book is enabled and available, BOTMODE checks it before starting Stockfish search. A book hit is displayed briefly, committed to the local game history and returned as `BESTMOVE`. A book miss falls back to Stockfish.
- After a search result, the firmware applies the selected move to its local game history, synchronizes the engine position and updates the display before publishing `BESTMOVE` to the PC.
- The next time the bot is to move, the PC sends the latest full FEN. The MCU must not assume that the previous local position is still authoritative after the opponent has moved.
- The local GUI game that existed before entering BOTMODE is kept in RAM and restored on `EXITBOT`. Remote game positions are not written over the saved local game in NVS.
- Touch input for normal game interaction is disabled while in BOTMODE. Touch calibration remains available if required.
- `EXITBOT` leaves bridge mode; it does not require an acknowledgement in protocol version 1.

The reference firmware targets the `freenove_stockfish_gui` PlatformIO environment and has been tested with the Python bridge on real Freenove ESP32-S3 Display hardware. Other MCU projects must implement these requirements themselves.
