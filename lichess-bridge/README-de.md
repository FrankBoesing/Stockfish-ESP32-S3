# Microcontroller Chess – Lichess Bridge

[Deutsch](README-de.md) | [English](README.md)

Eigenständige Python-Bridge für zwei räumlich getrennte Microcontroller-Schachcomputer. Jeder Standort betreibt eine eigene Instanz; beide PCs kommunizieren ausschließlich über Lichess.

## Architektur

```text
PC A → Python Bridge A → Lichess Bot A
PC B → Python Bridge B → Lichess Bot B
  │                         │
  USB/Serial                USB/Serial
  │                         │
Chess-MCU A               Chess-MCU B
```

Es gibt keine direkte Verbindung zwischen den PCs. Jeder Standort besitzt nur das Token seines eigenen Bot-Kontos.

**Wichtig:** Für Engine-Partien ist ausschließlich die offizielle Lichess Bot API vorgesehen, nicht die Board API. Bot-Konten dürfen nur Challenge-Partien spielen, keine Pools oder Turniere. Die Hochstufung eines normalen Kontos zu einem Bot-Konto ist irreversibel. Dafür sollten zwei eigens angelegte Konten verwendet werden, die noch keine Partie gespielt haben. UltraBullet wird nicht unterstützt. Beachte die [Lichess Fair-Play-Regeln](https://lichess.org/page/fair-play).

## Voraussetzungen

- Python 3.11 oder neuer
- Internetzugriff auf https://lichess.org
- Ein dediziertes Bot-Konto pro Schachcomputer
- Stockfish-ESP32-S3-Firmware mit dem integrierten Protokollmodus `BOTMODE` (siehe [docs/serial-protocol-de.md](docs/serial-protocol-de.md))

## Installation

Im entpackten Projektverzeichnis:

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

## Lichess-Bot-Konten und Tokens

Für den vollständigen Ablauf – einschließlich Registrierung, Gmail-Plus-Alias als nicht garantierter Versuch, Token-Erzeugung, unwiderruflicher BOT-Hochstufung und Kontrolle – siehe [docs/ACCOUNT-SETUP-DE.md](docs/ACCOUNT-SETUP-DE.md).

**Mehrfachkonten beachten:** Lichess erlaubt mehr als ein Konto nur in bestimmten Ausnahmefällen. Die Nutzungsbedingungen stellen dies in das Ermessen von Lichess; mehr als drei Konten werden üblicherweise nicht akzeptiert. Das ist keine pauschale Erlaubnis für drei Konten. Vor der Registrierung eines weiteren BOT-Kontos sollte die konkrete Nutzung mit Lichess geklärt werden.

Jedes genehmigte BOT-Konto benötigt ein eigenes Personal API Token. Für diese Bridge werden verwendet:

- `bot:play` für Bot-Spiel, Game-Stream und Zugübertragung
- `challenge:read` für eingehende Challenges
- `challenge:write` für Challenges erstellen, annehmen und ablehnen

Das Token kommt nicht in die TOML-Datei. Es wird zur Laufzeit als Umgebungsvariable gesetzt.

**Windows PowerShell**
```powershell
$env:LICHESS_TOKEN = "TOKEN_DES_LOKALEN_BOTS"
```

**macOS / Linux**
```bash
export LICHESS_TOKEN="TOKEN_DES_LOKALEN_BOTS"
```

Tokens niemals committen oder in Logs veröffentlichen. `config.toml` ist in `.gitignore` eingetragen.

## Konfiguration

Bearbeite auf jedem Standort die lokale Datei `config.toml`:

- `lichess.username`: exakter Name des lokalen Bot-Kontos
- `lichess.token_env`: Umgebungsvariable, die dessen Token enthält
- `opponent.allowed_username`: exakter Benutzername des anderen Bots
- `opponent.role`: `CHALLENGE` auf dem initiierenden PC, `ACCEPT` auf dem zweiten PC
- `serial.port`: z. B. `COM5`, `/dev/cu.usbmodem...` oder `/dev/ttyACM0`
- `serial.baudrate`: Standard 115200
- `game.initial_time_seconds` und `game.increment_seconds`: Bedenkzeit
- `game.safety_margin_ms`, `move_time_factor`, `min_move_time_ms`, `max_move_time_ms`: konservatives Suchzeitbudget

## Start und Bedienung

Ports auflisten:
```bash
python bridge.py --list-ports
```

Konfiguration prüfen:
```bash
python bridge.py --config config.toml --validate-config
```

Lokale Protokollsimulation ohne Lichess, MCU und Token:
```bash
python bridge.py --offline-simulation
```

Bridge mit simuliertem Microcontroller, aber echter Lichess-Verbindung:
```bash
python bridge.py --config config.toml --simulate
```

Normalbetrieb:
```bash
python bridge.py --config config.toml
```

Zuerst Standort B mit `role = "ACCEPT"` starten, danach Standort A mit `role = "CHALLENGE"`. Die Challenge wird an den ausdrücklich konfigurierten Gegner gesendet. Nach Spielstart wird die Lichess-Partie-URL in der Konsole angezeigt.

Mit Strg+C beenden. Beim Herunterfahren sendet die Bridge nach Möglichkeit `STOP` und `EXITBOT`.

## Tests

```bash
python -m pytest
```

Die HTTP-Tests verwenden einen Mock-Transport; es werden keine echten Tokens benötigt.

## Hinweise und Einschränkungen

- Version 1 unterstützt nur Standard-Schach.
- Pro Bridge wird eine Partie gleichzeitig verarbeitet.
- Die Firmware-Implementierung für Stockfish-ESP32-S3 liegt im übergeordneten Repository unter `src/chess_gui.cpp` und `src/gui/serial_console.cpp`.
- Der integrierte `BOTMODE` ist für das PlatformIO-Environment `freenove_stockfish_gui` vorgesehen; andere Boards/Firmware-Targets sind damit noch nicht verifiziert.
- `GO` läuft asynchron und bleibt über `STOP` abbrechbar.
- Nach einem unklaren Ergebnis beim HTTP-Move-POST wird derselbe Zug nicht blind erneut gesendet. Die Bridge wartet auf den Game-Stream; nach einem Prozessabbruch kann ein manueller Abgleich mit Lichess erforderlich sein.
- Der grundlegende Ende-zu-Ende-Ablauf wurde mit echter Freenove-ESP32-S3-Hardware und realen Bot-Konten erfolgreich getestet. Wiederverbindungs- und seltene Fehlerfälle benötigen weitere Tests.
- Das Zeitbudget ist eine konfigurierbare Heuristik, keine Garantie gegen Zeitüberschreitung.
- Lichess-Partien ohne klassische Schachuhr (z. B. Korrespondenzpartien) werden anhand der `gameFull`-Metadaten (`clock`, `speed`, `daysPerTurn`) erkannt. Der `gameState`-Platzhalter `2147483647` wird ebenfalls nicht als echte Restbedenkzeit interpretiert; stattdessen verwendet die Bridge `max_move_time_ms`.

Siehe [serielles Protokoll LICHESS-BRIDGE/1](docs/serial-protocol-de.md) und die offizielle [Lichess API](https://lichess.org/api), [Fair Play](https://lichess.org/page/fair-play) sowie [API-Tipps/Rate-Limits](https://lichess.org/page/api-tips).
