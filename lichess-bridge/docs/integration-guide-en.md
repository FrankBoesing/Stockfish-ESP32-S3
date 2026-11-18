# MCU Integration Guide — LICHESS-BRIDGE/1

This guide describes how to connect a different chess-computer firmware—such as TeensyChess—to the existing Python Lichess bridge. It focuses on the MCU-side work. It does not require the MCU project to use the same GUI, board driver or Stockfish port as the ESP32-S3 reference firmware.

[Serial protocol specification](serial-protocol-en.md) | [Bridge overview](../README.md)

## 1. Architecture and responsibilities

The system has three independent parts:

1. **Lichess:** hosts the game and distributes the current move list through the Bot API.
2. **Python bridge (PC):** authenticates the BOT account, accepts or sends challenges, reads the game stream, maintains the authoritative chess position, sends FEN positions to the MCU and submits the MCU's moves to Lichess.
3. **Chess MCU:** accepts a position, calculates a move, optionally using its own opening book, updates its local display/game state and returns one UCI move.

For two remote chess computers, each location runs its own copy of the Python bridge, its own Lichess BOT account and one MCU connected over USB serial. The PCs do not connect directly to each other.

The Python bridge is intended to be reusable. A new MCU target should implement the serial protocol; it should not need to implement Lichess authentication or the Lichess Bot API.

## 2. Minimum MCU interface

Implement the commands and responses in the [serial protocol specification](serial-protocol-en.md). At minimum, the MCU needs these operations:

- Enter and leave bridge mode.
- Reset the game state at the start of a new game.
- Load and validate a complete six-field FEN.
- Start a move calculation with a time budget in milliseconds.
- Cancel an active calculation.
- Return a legal UCI move.
- Keep the local GUI in sync with the current game.

The protocol is standard chess only. Chess960 and other variants are not part of version 1.

## 3. Host-to-MCU state machine

A useful implementation model is:

```text
NORMAL_MODE
    |
    | BOTMODE
    v
BOTMODE_ACTIVE -- PING --> PONG
    |
    | NEWGAME
    v
WAIT_FOR_POSITION
    |
    | POSITION <FEN>
    v
POSITION_READY
    |
    | GO <movetime_ms>
    v
SEARCHING
    |                     |
    | BESTMOVE <uci>      | STOP
    v                     v
MOVE_COMMITTED         SEARCH_CANCELLED
    |                     |
    +------ wait for next POSITION / GO cycle
    |
    | EXITBOT
    v
NORMAL_MODE
```

This is a conceptual state machine. A production implementation should also define recovery from invalid commands, invalid FEN, search failure, serial disconnect and MCU reset.

## 4. Exact command/response sequence

### Startup

```text
PC -> MCU: BOTMODE
MCU -> PC: READY LICHESS-BRIDGE/1 <device_name>
PC -> MCU: PING
MCU -> PC: PONG
```

The bridge waits for the complete `READY` response before sending `PING`. If the engine is not initialized yet, the MCU may return `ERROR ENGINE_NOT_READY <text>`; the PC treats an `ERROR` response as an operation failure. A practical MCU integration should therefore be ready to enter bridge mode only after its engine can accept positions and searches.

### New game

```text
PC -> MCU: NEWGAME
MCU -> PC: OK NEWGAME
```

Reset search state, pending moves, move history, terminal-position flags and any per-game state. Do not leave a search from a previous game running.

### Load a position

```text
PC -> MCU: POSITION <six-field FEN>
MCU -> PC: OK POSITION
```

The FEN describes the complete current position, including side to move, castling rights, en-passant target, halfmove clock and fullmove number. The Python bridge reconstructs the board from the Lichess move stream and sends the full FEN; it does not send a UCI move list.

Validate the FEN before acknowledging it. On failure return:

```text
MCU -> PC: ERROR BAD_FEN <reason>
```

Do not start a search until the current `POSITION` has been accepted.

### Calculate and return a move

```text
PC -> MCU: GO 20000
             ... asynchronous calculation ...
MCU -> PC: BESTMOVE e7e5
```

There is deliberately no immediate `OK GO` response. The response to `GO` is the eventual `BESTMOVE`. The search must be asynchronous with respect to the MCU's main loop, so serial input continues to be serviced.

The time value is a positive number of milliseconds. It is chosen by the Python bridge from the Lichess clock when one exists. For games without a conventional clock, the bridge uses the configured `max_move_time_ms` (20,000 ms in the current default configuration).

Return a move in lowercase UCI notation. A promotion includes its suffix, for example `e7e8q`. The Python bridge independently validates the returned move against its current chess position before submitting it to Lichess.

If the position is checkmate or stalemate and has no legal moves, return `BESTMOVE 0000`. Do not use `0000` for a timeout, engine error or cancelled search.

### Stop a search

```text
PC -> MCU: STOP
MCU -> PC: INFO STOP acknowledged
```

The PC sends `STOP` after a move-search timeout. Cancel the search and suppress its eventual best-move callback. Do not send a stale `BESTMOVE` after acknowledging the stop. The Python bridge does not submit a move from a timed-out search.

### Exit bridge mode

```text
PC -> MCU: EXITBOT
```

Leave bridge mode and return the MCU application to its regular operating mode. Version 1 does not require a response to `EXITBOT`.

## 5. Integrating with an existing chess GUI

A serial protocol adapter should be separate from the GUI and engine internals. Map the protocol commands to the corresponding operations in the target project.

Recommended behaviour:

1. **Entering BOTMODE:** save or snapshot the current local game state if the application must resume it after the remote game. Disable normal touch/game actions that could modify the remote position.
2. **NEWGAME:** clear the remote game's history and reset per-game engine state.
3. **POSITION:** replace the engine's root position with the supplied FEN, update the GUI board, side to move, move highlights and terminal-state information. The FEN is authoritative.
4. **GO:** run the opening-book lookup (if supported and enabled) or start the engine search with the requested time limit.
5. **Best move:** validate/apply the selected move to the local game history, synchronize the engine position and redraw the board before sending `BESTMOVE` to the PC. This keeps the screen responsive and makes the move visible immediately.
6. **Opponent's reply:** the Python bridge receives the opponent's move from Lichess. When it is the bot's turn again, it sends a new full FEN. Do not infer the opponent's move from the MCU's previous best move.
7. **EXITBOT:** cancel pending work and restore the pre-existing local game if the application supports session restoration.

For the reference ESP32-S3 implementation, the pre-existing local GUI game is snapshotted in RAM and restored when bridge mode exits. Remote positions are not written over the local game's NVS save. A TeensyChess implementation can use its own persistence model, but should avoid unintentionally replacing a user's saved local game with a remote Lichess game.

## 6. Opening book behaviour

Opening-book selection is performed on the MCU, not by the Python bridge. In the reference firmware, the Polyglot book is queried when it is enabled and available:

- On a book hit, the move is briefly shown, applied to the local game and returned as `BESTMOVE`.
- On a book miss, Stockfish performs the search using the `GO` time budget.
- If the book is disabled or unavailable, Stockfish is used directly.

A different MCU implementation may use another opening book or no book at all. The host protocol does not distinguish a book move from a searched move; both are returned as `BESTMOVE <uci_move>`.

## 7. Serial and parser implementation notes

- Use 115200 baud, 8N1 by default. The baud rate can be changed in the PC configuration, but both sides must match.
- Use newline-delimited messages. Accept LF and CRLF from the host.
- Buffer a complete input line before parsing it. Do not parse an incomplete FEN.
- The FEN text limit in the reference firmware is 192 bytes. Supporting at least this length is recommended.
- Keep protocol output to one response per line. Do not interleave debug output with a protocol response.
- The Python parser recognises `READY`, `PONG`, `OK NEWGAME`, `OK POSITION`, `BESTMOVE`, `INFO`, `#LOG` and `ERROR`. Other lines are ignored as unknown output and currently generate a warning.
- Prefix diagnostic-only output with `#LOG ` where possible. Never use protocol prefixes for unrelated debug messages.
- Do not emit a move after `STOP` or after a newer `POSITION` has superseded the search.
- Ensure that only one search is active at a time. Reject overlapping `GO` or a `POSITION` received during an active search with an `ERROR` response.

## 8. Error handling and reconnects

The current Python bridge treats a protocol `ERROR` as an operation failure. It also has a serial reconnect path that reopens the port, repeats `BOTMODE` and `PING`, and sends `NEWGAME` for an active game. The next game-state update supplies a fresh FEN.

The MCU should therefore:

- Start each accepted `NEWGAME` from a clean search/game state.
- Handle `BOTMODE` when already in bridge mode without corrupting its saved local-game snapshot.
- Clear any old best-move callback when a search is cancelled or a new game starts.
- Never return a best move from a previous position after a reconnect.
- Keep the main application alive if the serial connection is lost; do not assume the host will always send `EXITBOT`.

A reconnect or MCU reset during an active game may require the host to reinitialise the MCU and resend the current position. Test this path on the actual target hardware.

## 9. Validation checklist for a new MCU target

Before connecting a real Lichess BOT account, test the target MCU with a serial terminal or a small host script:

- [ ] `BOTMODE` returns the protocol version and device name.
- [ ] `PING` returns `PONG`.
- [ ] `NEWGAME` resets all per-game and search state and returns `OK NEWGAME`.
- [ ] A valid six-field FEN returns `OK POSITION`.
- [ ] An invalid FEN returns `ERROR BAD_FEN ...` without corrupting the previous valid state.
- [ ] `GO` starts asynchronously and eventually returns one legal `BESTMOVE`.
- [ ] The MCU main loop continues to process serial commands while searching.
- [ ] `STOP` cancels the result and does not allow a stale move to escape later.
- [ ] `BESTMOVE 0000` is returned only for a position with no legal moves.
- [ ] The GUI displays the position and the MCU-applied move correctly.
- [ ] `EXITBOT` returns the application to normal mode and restores local state if supported.
- [ ] An unplug/reconnect or MCU reset does not cause a stale move to be submitted.

The reference Stockfish-ESP32-S3 target has passed a basic real-hardware end-to-end test with a Polyglot book hit, a Stockfish search fallback, display updates and Lichess move confirmations. A new MCU target must be validated independently.

## 10. What does not need to be ported

A TeensyChess or other MCU project does not need to reimplement:

- Lichess authentication or Personal API Token management.
- Challenge creation, acceptance or filtering.
- Lichess game-stream parsing.
- Clock and move-time-budget calculation.
- Submission of moves to Lichess.
- Host-side legal-move validation.

Those responsibilities remain in the Python bridge. The MCU integration consists of the serial protocol adapter plus the engine, game-history and GUI hooks described above.
