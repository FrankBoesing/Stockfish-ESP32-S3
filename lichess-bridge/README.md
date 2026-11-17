# Microcontroller Chess – Lichess Bridge

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
- Microcontroller-Firmware mit dem Protokoll aus [docs/serial-protocol.md](docs/serial-protocol.md)

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

Lege zwei neue Lichess-Konten an. Spiele mit ihnen noch keine Partie. Folge danach für jedes Konto der offiziellen Anleitung zum [Upgrade to a BOT account](https://lichess.org/api#operation/botAccountUpgrade). Der Kontoname muss nach der Umstellung den Titel `BOT` tragen.

Erzeuge je Bot ein Personal API Token mit den nötigen Berechtigungen:

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

Wiederhole dies am zweiten PC mit dem Token von Bot B. Tokens niemals committen oder in Logs veröffentlichen. `config.toml` ist in `.gitignore` eingetragen.

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
- Die MCU-Firmware ist nicht Bestandteil dieses Projekts und muss separat angepasst werden.
- `GO` muss abbrechbar bleiben, damit `STOP` während der Suche verarbeitet werden kann.
- Nach einem unklaren Ergebnis beim HTTP-Move-POST wird derselbe Zug nicht blind erneut gesendet. Die Bridge wartet auf den Game-Stream; nach einem Prozessabbruch kann ein manueller Abgleich mit Lichess erforderlich sein.
- Die Wiederverbindung mit serieller Schnittstelle und Lichess-Streams ist implementiert, muss aber auf echter Hardware und mit realen Bot-Konten erprobt werden.
- Das Zeitbudget ist eine konfigurierbare Heuristik, keine Garantie gegen Zeitüberschreitung.

Siehe [serielles Protokoll LICHESS-BRIDGE/1](docs/serial-protocol.md) und die offizielle [Lichess API](https://lichess.org/api), [Fair Play](https://lichess.org/page/fair-play) sowie [API-Tipps/Rate-Limits](https://lichess.org/page/api-tips).
