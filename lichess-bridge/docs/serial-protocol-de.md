# Serielles Protokoll LICHESS-BRIDGE/1

[Deutsch](serial-protocol-de.md) | [English](serial-protocol-en.md) | [MCU-Integrationsleitfaden (Englisch)](integration-guide-en.md)

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
- Ungültiges Suchzeitbudget: `ERROR BAD_MOVETIME <text>`.
- `GO` ohne übernommene Stellung: `ERROR NO_POSITION <text>`.
- Bereits laufende Suche: `ERROR BUSY <text>` bzw. `ERROR SEARCH_BUSY <text>`.
- Ein neuer `POSITION`-Befehl ersetzt immer die zuletzt übermittelte Position.
- `GO` darf nur mit einer gültigen zuvor übernommenen Position starten.
- `BESTMOVE` muss zur zuletzt übernommenen Position legal sein.
- `0000` ist nur zulässig, wenn tatsächlich keine legalen Züge existieren.
- Diagnoseausgaben dürfen nicht wie Protokollantworten aussehen. Empfohlen wird `#LOG ...`.
- Keine mehrzeiligen Debug-Dumps über dieselbe serielle Schnittstelle ausgeben.

## Firmware-Implementierung

Die Stockfish-ESP32-S3-GUI-Firmware implementiert das Protokoll in einem eigenen seriellen `BOTMODE`. Außerhalb dieses Modus bleibt die normale serielle GUI-Konsole aktiv.

- `GO` startet eine asynchrone Stockfish-Suche mit dem übergebenen `movetime_ms`. Die Hauptschleife bleibt aktiv und kann weiterhin `STOP` verarbeiten.
- Die Bridge-FEN ersetzt die aktuelle Engine-Stellung und aktualisiert das lokale GUI-Brett. Nach der Suche übernimmt die Firmware den ausgewählten Zug auch in die lokale Spielhistorie und aktualisiert das Display, bevor `BESTMOVE` an den PC gesendet wird. Bei der nächsten eigenen Zuggelegenheit übermittelt der PC erneut die vollständige FEN.
- Der vor BOTMODE aktive GUI-Spielstand wird für die Bridge-Sitzung im RAM zwischengespeichert und bei `EXITBOT` wiederhergestellt. Remote-Stellungen werden nicht in NVS gespeichert.
- Wenn die Polyglot-Eröffnungsbibliothek aktiviert und verfügbar ist, wird sie auch im BOTMODE abgefragt. Bei einem Treffer wird der Buchzug nach einer kurzen Anzeigepause lokal übernommen und als `BESTMOVE` gemeldet; andernfalls startet Stockfish.
- `STOP` bricht eine laufende Suche ab und verwirft deren ausstehende Antwort.
- Ein regulärer Suchabschluss sendet genau eine `BESTMOVE`-Antwort. Bei einer Stellung ohne legalen Zug wird `BESTMOVE 0000` gesendet.
- `EXITBOT` beendet den Bridge-Modus. Während `BOTMODE` werden Touch-Eingaben für das Spiel ignoriert; eine gegebenenfalls notwendige Touch-Kalibrierung bleibt möglich.

Die Referenzimplementierung ist in der Stockfish-ESP32-S3-GUI-Firmware integriert und wurde mit der Python-Bridge auf echter Freenove-ESP32-S3-Hardware getestet. Andere MCU-Projekte müssen die Anforderungen dieser Spezifikation eigenständig implementieren.
