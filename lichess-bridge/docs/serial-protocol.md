# Serielles Protokoll LICHESS-BRIDGE/1

## Transport

- Baudrate: standardmäßig 115200, in `config.toml` änderbar.
- 8 Datenbits, keine Parität, 1 Stoppbit (8N1).
- Textcodierung: UTF-8; alle definierten Befehle und Antworten sind ASCII-kompatibel.
- Jede Nachricht endet mit LF (`\n`). Der PC akzeptiert LF und CRLF.
- Eine Nachricht muss vollständig in einer Zeile stehen.
- Der PC sendet `POSITION` gefolgt von genau einem Leerzeichen und der vollständigen FEN (sechs Felder).
- Der Microcontroller muss eine empfangene Zeile vollständig puffern, bevor sie verarbeitet wird.

## PC → Microcontroller

| Befehl | Bedeutung |
|---|---|
| `BOTMODE` | Bridge-Modus aktivieren. |
| `PING` | Verbindung prüfen. |
| `NEWGAME` | Interne Partie-/Enginezustände zurücksetzen. |
| `POSITION <FEN>` | Vollständige Standard-Schachstellung übernehmen. |
| `GO <movetime_ms>` | Einen Zug mit dem angegebenen Suchzeitbudget berechnen. |
| `STOP` | Laufende Berechnung abbrechen. |
| `EXITBOT` | Bridge-Modus verlassen. |

## Microcontroller → PC

| Antwort | Bedeutung |
|---|---|
| `READY LICHESS-BRIDGE/1 <device_name>` | Bridge-Modus aktiv und Gerät identifiziert. |
| `PONG` | Antwort auf `PING`. |
| `OK NEWGAME` | `NEWGAME` verarbeitet. |
| `OK POSITION` | FEN übernommen. |
| `BESTMOVE <uci_move>` | Ein legaler Zug im UCI-Format, z. B. `e2e4` oder `e7e8q`. |
| `BESTMOVE 0000` | Kein legaler Zug vorhanden. |
| `INFO <text>` | Optionale Protokollinformation. |
| `ERROR <code> <text>` | Protokollfehler oder abgelehnter Befehl. |
| `#LOG <text>` | Optionale Diagnoseausgabe, keine Protokollantwort. |

## Verbindlicher Ablauf

1. PC öffnet den konfigurierten Port und sendet `BOTMODE`.
2. Microcontroller antwortet mit `READY LICHESS-BRIDGE/1 <device_name>`.
3. PC sendet `PING`; Microcontroller antwortet `PONG`.
4. Bei Spielbeginn sendet der PC `NEWGAME` und wartet auf `OK NEWGAME`.
5. Für jeden eigenen Zug sendet der PC `POSITION <FEN>` und wartet auf `OK POSITION`.
6. Danach sendet er `GO <movetime_ms>` und wartet auf `BESTMOVE`.
7. Der PC validiert den Zug anhand des aktuellen Lichess-Spielstands. Nur legale Züge werden übertragen.
8. Bei Timeout sendet der PC `STOP`; es wird kein Zug übertragen.
9. Beim Beenden sendet der PC nach Möglichkeit `EXITBOT`.

## Fehler- und Diagnoseverhalten

- Unbekannte Befehle sind mit `ERROR UNKNOWN_COMMAND <text>` zu beantworten.
- Ungültige FEN: `ERROR BAD_FEN <text>`.
- Ein neuer `POSITION`-Befehl ersetzt immer die zuletzt übermittelte Position.
- `GO` darf nur mit einer gültigen zuvor übernommenen Position starten.
- `BESTMOVE` muss zur zuletzt übernommenen Position legal sein.
- `0000` ist nur zulässig, wenn tatsächlich keine legalen Züge existieren.
- Diagnoseausgaben dürfen nicht wie Protokollantworten aussehen. Empfohlen wird `#LOG ...`.
- Keine mehrzeiligen Debug-Dumps über dieselbe serielle Schnittstelle ausgeben.

## Implementierungsannahmen für die Firmware

Die MCU-Firmware ist nicht Teil dieses Python-Projekts. Folgende Punkte sind daher Anforderungen an die spätere Firmware-Anpassung und noch nicht auf echter Hardware verifiziert:

- `GO` muss asynchron oder zumindest so verarbeitet werden, dass `STOP` während der Suche noch empfangen werden kann.
- `movetime_ms` ist ein maximales Suchzeitbudget, kein Versprechen, dass die Suche exakt so lange läuft.
- Nach `STOP` soll die MCU die Suche abbrechen und keine verspätete `BESTMOVE`-Antwort für den abgebrochenen Auftrag senden.
- Für jeden Auftrag sollte genau eine abschließende Antwort gesendet werden.
