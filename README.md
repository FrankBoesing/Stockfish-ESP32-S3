# Stockfish ESP32-S3

A touchscreen chess computer powered by **Stockfish 19 Smallnet**, built for the Freenove ESP32-S3 Display board.

The project combines a complete chess game interface with the Stockfish engine on a compact ESP32-S3 device. It is an experimental hardware project and is currently tailored to one specific display board.

## Features

- Play against Stockfish on the integrated touchscreen.
- Choose to play as White or Black.
- Adjust the engine strength from Level 1 to Level 20.
- Legal destination squares are highlighted when selecting a piece.
- Pawn promotion is handled with an on-screen piece selector.
- Undo the previous move, request a hint, or start a new game.
- The board automatically rotates when you play as Black.
- Detects check, checkmate, stalemate, the 50-move rule, threefold repetition and supported insufficient-material draws.
- Touchscreen calibration is performed on first start and stored in flash.
- The current game, player colour and game strength are saved automatically and restored after a reboot.

## Hardware

The firmware is designed for this board configuration:

| Component | Configuration |
|---|---|
| Board | Freenove ESP32-S3 Display, FNK0104B |
| Display | 2.8-inch ILI9341, 320 × 240 pixels |
| Touch controller | FT6336U capacitive touch |
| Processor | ESP32-S3, dual-core Xtensa LX7 |
| PSRAM | 8 MiB OPI |
| Flash | 16 MiB |

**Compatibility note:** This is not a generic ESP32-S3 firmware. Display, touch, flash and PSRAM settings are specific to the Freenove board above. Other ESP32-S3 boards and display variants have not been validated.

## Playing

### First start

On the first boot, the display starts a three-point touch calibration. Tap each crosshair when prompted. The calibration is stored in flash and is reused after reboot.

### Start a game

1. Open **New Game**.
2. Choose **White** or **Black**.
3. Set the Stockfish level with the minus and plus controls.
4. Tap **Start**.

### Make moves

Tap one of your pieces, then tap a highlighted destination square. If a pawn reaches the last rank, choose the promotion piece in the dialog.

The side panel provides the **HINT**, **UNDO** and **NEW** controls. The HINT button asks Stockfish for a suggested move. UNDO takes back the previous move pair where possible. NEW returns to the new-game screen.

### Saved games

The current position and move history are saved automatically after accepted moves and game changes. After a restart, the previous game can be restored. The saved game retains its original player colour and Stockfish level; the menu selection for the next game is stored separately.

## USB serial console

Connect the board to a computer and open a serial monitor at **115200 baud**. The console accepts commands for testing and game control:

| Command | Description |
|---|---|
| `help` | Show available commands |
| `moves` or `uci` | Print the current move sequence / UCI position |
| `fen` | Print the current FEN |
| `undo` | Undo the previous move |
| `hint` | Ask Stockfish for a suggested move |
| `stop` | Stop the current search |
| `level 1..20` | Set the strength for the next game |
| `pgn` | Export the current game in PGN/SAN format |
| `newgame [level] [white\|black]` | Start a new game |
| `position startpos [moves ...]` | Load a start position and optional moves |
| `position fen <FEN> [moves ...]` | Load a FEN position and optional moves |

A single move can also be entered directly in UCI notation, for example `e2e4` or `e7e8q` for a promotion.

## Build and upload

The repository contains the firmware source and PlatformIO configuration. Install PlatformIO Core and run the following commands from the repository root:

```sh
# Compile the touchscreen application
pio run -e freenove_stockfish_gui

# Upload it to the connected Freenove board
pio run -e freenove_stockfish_gui -t upload

# Run the host-side chess/GUI regression tests (Linux or WSL)
bash tests/native/run_tests.sh
```

The serial console uses 115200 baud. PlatformIO installs the Espressif32 toolchain and libraries required by the selected environment. The Stockfish build script downloads the required Smallnet NNUE network during the build, so the first build needs Internet access.

GitHub Actions runs the native regression suite and the firmware build matrix for relevant changes on `main` and pull requests targeting `main`. A separate Gitleaks workflow checks the repository files and the full reachable Git history across branches on every branch push and weekly.

## Opening book

The firmware contains a reader for an optional Polyglot opening book stored in a separate flash partition. The book image must be prepared and flashed separately. This part of the project is still awaiting final hardware validation, so opening-book operation should be considered experimental.

See [Flashing an opening book](docs/FLASHING-OPENING-BOOK.md) for the image format and procedure.

## Project status

The touchscreen application and Stockfish engine are running on the target board. This remains an experimental project rather than a general-purpose, production-ready chess product. The exact board configuration and the opening-book flash procedure are the main hardware constraints.

For implementation details, benchmark results, build environments, NNUE/PIE validation and development history, see the [extended technical README](README-extended.md).

## Source and licence

This project, including the integrated Stockfish-derived firmware, is distributed under the **GNU General Public License version 3 (GPLv3)**. The complete license is in the repository-root [LICENSE](LICENSE) file; the original Stockfish license copy, author list and copyright notices remain under [src/stockfish](src/stockfish/).

The engine is based on [official Stockfish](https://github.com/official-stockfish/Stockfish). Third-party components and artwork retain their respective copyright and license notices. When distributing firmware, comply with GPLv3 requirements for the corresponding source code and build modifications.
