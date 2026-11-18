# Microcontroller Chess – Lichess Bridge

[English](README.md) | [Deutsch](README-de.md)

A standalone Python bridge for two geographically separated microcontroller-based chess computers. Each location runs its own bridge instance; the two PCs communicate exclusively through Lichess.

## Architecture

```text
PC A → Python Bridge A → Lichess Bot A
PC B → Python Bridge B → Lichess Bot B
  │                         │
  USB/Serial                USB/Serial
  │                         │
Chess-MCU A               Chess-MCU B
```

There is no direct connection between the PCs. Each location holds only the token for its own bot account.

**Important:** Engine games must use the official Lichess Bot API, not the Board API. BOT accounts may play challenge games, but not pools or tournaments. Upgrading a regular account to a BOT account is irreversible. Use dedicated accounts that have not played any games before the upgrade. UltraBullet is not supported. Follow the [Lichess Fair Play rules](https://lichess.org/page/fair-play).

## Requirements

- Python 3.11 or newer
- Internet access to https://lichess.org
- One dedicated Lichess BOT account per chess computer
- MCU firmware implementing the `BOTMODE` serial protocol (see [serial protocol specification](docs/serial-protocol-en.md) and the [MCU integration guide](docs/integration-guide-en.md))

The reference firmware implementation is currently provided for the Stockfish-ESP32-S3 PlatformIO environment `freenove_stockfish_gui`. Other firmware targets must implement the serial protocol themselves.

## Installation

Run these commands from the extracted project directory.

**Windows PowerShell**

```powershell
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -e ".[test]"
Copy-Item config.example.toml config.toml
```

**macOS / Linux**

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -e '.[test]'
cp config.example.toml config.toml
```

## Lichess BOT accounts and API tokens

For the full account setup procedure—including registration, the irreversible BOT upgrade, token creation and verification—see [Setting Up Lichess Bot Accounts](docs/ACCOUNT-SETUP-en.md).

**Multiple accounts:** Lichess does not generally permit multiple accounts without restrictions. Its Terms of Service list specific exceptions and leave the decision to Lichess; more than three accounts are generally not accepted. This is not blanket permission to operate three accounts. Clarify the intended use with Lichess before registering another BOT account.

Each approved BOT account needs its own Personal API Token. This bridge requires:

- `bot:play` to play bot games, use the game stream and submit moves
- `challenge:read` to receive incoming challenges
- `challenge:write` to create, accept and decline challenges

Do not put the token in the TOML file. Set it as an environment variable at runtime.

**Windows PowerShell**

```powershell
$env:LICHESS_TOKEN = "TOKEN_FOR_THIS_LOCATION'S_BOT"
```

**macOS / Linux**

```bash
export LICHESS_TOKEN="TOKEN_FOR_THIS_LOCATION'S_BOT"
```

Never commit tokens or publish them in logs. The local `config.toml` file is excluded by `.gitignore`.

## Configuration

Create a local `config.toml` at each location. The main settings are:

- `lichess.username`: exact username of the local BOT account
- `lichess.token_env`: name of the environment variable holding that bot's token
- `opponent.allowed_username`: exact username of the remote opponent account
- `opponent.role`: `CHALLENGE` on the initiating PC, `ACCEPT` on the other PC
- `serial.port`: for example `COM5`, `/dev/cu.usbmodem...` or `/dev/ttyACM0`
- `serial.baudrate`: defaults to 115200
- `game.initial_time_seconds` and `game.increment_seconds`: clock settings for challenge creation
- `game.safety_margin_ms`, `move_time_factor`, `min_move_time_ms`, `max_move_time_ms`: conservative search-time budget settings

The exact available settings and defaults are documented in [config.example.toml](config.example.toml).

## Starting and operating the bridge

List serial ports:

```bash
python bridge.py --list-ports
```

Validate the configuration:

```bash
python bridge.py --config config.toml --validate-config
```

Run a local protocol simulation without Lichess, an MCU or a token:

```bash
python bridge.py --offline-simulation
```

Run the bridge with a simulated microcontroller but a real Lichess connection:

```bash
python bridge.py --config config.toml --simulate
```

Normal operation with a real MCU:

```bash
python bridge.py --config config.toml
```

Start location B first with `role = "ACCEPT"`, then start location A with `role = "CHALLENGE"`. The challenge is sent to the explicitly configured opponent. Once the game starts, the Lichess game URL is printed in the console.

Stop the bridge with Ctrl+C. During shutdown it sends `STOP` and `EXITBOT` when possible. Make sure no other application, such as a serial monitor, has the configured serial port open; serial ports are normally exclusive on Windows.

## Tests

Run the Python test suite:

```bash
python -m pytest
```

HTTP tests use a mock transport; no real tokens are required.

## Current functionality and limitations

- Version 1 supports standard chess only.
- Each bridge instance processes one game at a time.
- The Stockfish-ESP32-S3 firmware implementation is in the parent repository under `src/chess_gui.cpp` and `src/gui/serial_console.cpp`.
- The integrated `BOTMODE` implementation currently targets the `freenove_stockfish_gui` PlatformIO environment. Other boards and firmware targets have not been verified.
- `GO` is asynchronous and can be cancelled with `STOP`.
- When enabled and available, the MCU's Polyglot opening book is queried in BOTMODE. A book hit is applied to the local GUI and returned as `BESTMOVE`; on a miss, the MCU runs a normal Stockfish search. The Python bridge does not implement or select opening-book moves.
- If the result of an HTTP move submission is unclear, the bridge does not blindly submit the same move again. It waits for the game stream; after a process crash, a manual check against Lichess may be necessary.
- Basic end-to-end operation has been tested with the real Freenove ESP32-S3 hardware and real BOT accounts, including a Polyglot hit, a Stockfish fallback, local display updates and Lichess move confirmation. Reconnect and less common fault cases still need further testing.
- The move-time budget is a configurable heuristic, not a guarantee against flagging.
- Games without a conventional chess clock (for example correspondence games) are detected using `gameFull` metadata (`clock`, `speed`, `daysPerTurn`). The `gameState` placeholder value `2147483647` is not treated as a real remaining time; the bridge uses `max_move_time_ms` instead.

See the [serial protocol specification](docs/serial-protocol-en.md), the [MCU integration guide](docs/integration-guide-en.md), and the official [Lichess API](https://lichess.org/api), [Fair Play rules](https://lichess.org/page/fair-play) and [API tips and rate limits](https://lichess.org/page/api-tips).
