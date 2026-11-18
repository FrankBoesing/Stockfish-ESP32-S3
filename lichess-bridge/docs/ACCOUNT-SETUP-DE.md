# Lichess-Bot-Konten einrichten (Windows / PowerShell)

[Deutsch](ACCOUNT-SETUP-DE.md) | [English](ACCOUNT-SETUP-en.md)

Diese Anleitung dokumentiert den Ablauf, mit dem das Testkonto **espsf19testbot** eingerichtet, unwiderruflich zu einem BOT-Konto hochgestuft und anschließend mit der Python-Bridge getestet wurde.

> **Wichtig – Mehrfachkonten:** Lichess erlaubt mehrere Konten nicht grundsätzlich. Die Nutzungsbedingungen nennen nur bestimmte Ausnahmefälle und stellen die Entscheidung in das Ermessen von Lichess. Mehr als drei Konten werden üblicherweise nicht akzeptiert. Das ist keine pauschale Erlaubnis, drei Konten zu betreiben. Vor dem Anlegen eines weiteren BOT-Kontos sollte die konkrete Nutzung mit Lichess geklärt werden. Siehe [Lichess Terms of Service](https://lichess.org/terms-of-service).

## 1. Bot-Benutzerkonto registrieren

1. Öffne https://lichess.org/signup.
2. Registriere einen eigenen Benutzernamen für den Schachcomputer, in unserem Test: `espsf19testbot`.
3. Verwende eine E-Mail-Adresse, auf die du Zugriff hast, und schließe gegebenenfalls die E-Mail-Bestätigung ab.
4. Melde dich mit dem neuen Konto an.

### Eine E-Mail-Adresse für mehrere Registrierungen

Im Gespräch hatten wir Gmail-Plus-Adressen als mögliche Lösung betrachtet. Bei Gmail kann eine Adresse beispielsweise so ergänzt werden:

- bisherige Adresse: `name@gmail.com`
- möglicher Alias für Bot A: `name+lichess-a@gmail.com`
- möglicher Alias für Bot B: `name+lichess-b@gmail.com`

Gmail stellt Nachrichten an solche Plus-Adressen üblicherweise im selben Postfach zu. **Annahme / nicht für Lichess verifiziert:** Ob Lichess einen solchen Alias für eine weitere Registrierung akzeptiert, muss im Registrierungsdialog ausprobiert werden. Das ist keine Zusicherung, dass mehrere Lichess-Konten zulässig sind. Keine fremden oder erfundenen E-Mail-Adressen verwenden.

## 2. Vor der BOT-Umstellung keine Partie spielen

Das neue Konto darf vor der Hochstufung **keine Partie gespielt haben**. Die Hochstufung ist unwiderruflich; danach kann das Konto nur noch als Bot über die Bot API spielen.

Bei unserem Testkonto zeigte die erfolgreiche spätere Account-Abfrage:

- `username`: `espsf19testbot`
- `title`: `BOT`
- `count.all`: `0`

Das Feld `count.all = 0` bestätigte, dass vor der Umstellung keine Partie gespielt worden war.

Offizielle Dokumentation: [Upgrade to Bot account](https://lichess.org/api#operation/botAccountUpgrade).

## 3. Personal API Token erzeugen

1. Während du als `espsf19testbot` angemeldet bist, öffne https://lichess.org/account/oauth/token.
2. Erstelle ein neues Personal API Token, z. B. mit der Beschreibung `Stockfish ESP32 S3 Bridge`.
3. Aktiviere für die Bridge die benötigten Berechtigungen:
   - `bot:play): Bot-Partien spielen, Game-Stream verwenden und Züge übertragen; diese Berechtigung ist auch für die BOT-Hochstufung erforderlich.
   - `challenge:read`: Challenge-Ereignisse lesen.
   - `challenge:write`: Challenges erstellen, annehmen und ablehnen.
4. Erzeuge das Token und kopiere es sofort an einen sicheren Ort. Behandle es wie ein Passwort.

Das Token niemals in Git, in `config.toml`, in Screenshots oder in Chat-Nachrichten eintragen.

Offizielle Seiten: [API-Dokumentation](https://lichess.org/api) und [Personal API Tokens](https://lichess.org/account/oauth/token).

## 4. Konto unwiderruflich zu BOT hochstufen

Die Umstellung erfolgt mit dem offiziellen Endpunkt:

`POST https://lichess.org/api/bot/account/upgrade`

In Windows PowerShell zuerst das Token nur für die aktuelle Sitzung setzen:

```powershell
$env:LICHESS_TOKEN = "HIER_DAS_ECHTE_TOKEN_EINSETZEN"
```

Dann die Umstellung ausführen:

```powershell
curl.exe -d "" "https://lichess.org/api/bot/account/upgrade" `
  -H "Authorization: Bearer $env:LICHESS_TOKEN"
```

Die Anfrage muss mit HTTP 200 erfolgreich sein. Bei unserem Konto wurde anschließend die Profilabfrage verwendet, um den Status zu prüfen.

**Vor dem POST noch einmal kontrollieren:** richtiger Benutzer angemeldet, Token gehört zu genau diesem Benutzer, Konto hat noch keine Partie gespielt. Diese Aktion kann nicht rückgängig gemacht werden.

## 5. BOT-Status kontrollieren

Mit demselben Token:

```powershell
curl.exe "https://lichess.org/api/account" `
  -H "Authorization: Bearer $env:LICHESS_TOKEN"
```

Bei unserem Testkonto enthielt die JSON-Antwort unter anderem:

```json
{
  "id": "espsf19testbot",
  "username": "espsf19testbot",
  "title": "BOT",
  "count": {
    "all": 0
  }
}
```

Entscheidend sind `username` und `title = BOT`. Die weiteren Profil- und Statistikfelder können sich unterscheiden.

## 6. Python-Bridge konfigurieren

Im Verzeichnis `lichess-bridge` die Beispieldatei kopieren:

```powershell
Copy-Item config.example.toml config.toml
```

In `config.toml` für den ersten Test gegen dein normales Konto beispielsweise eintragen:

```toml
[lichess]
username = "espsf19testbot"
token_env = "LICHESS_TOKEN"
base_url = "https://lichess.org"

[opponent]
allowed_username = "frank1123b"
role = "ACCEPT"
```

Die übrigen Werte zunächst aus `config.example.toml` übernehmen. `config.toml` wird durch `.gitignore` ausgeschlossen und darf nicht committet werden.

Das Token in PowerShell setzen:

```powershell
$env:LICHESS_TOKEN = "HIER_DAS_ECHTE_TOKEN_EINSETZEN"
```

Diese Variable gilt nur für das aktuelle PowerShell-Fenster. Nach dem Schließen des Fensters muss sie erneut gesetzt werden.

## 7. Bridge und Lichess-Verbindung testen

Im Verzeichnis `lichess-bridge`:

```powershell
python.exe -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -e ".[test]"
```

Danach die Bridge mit simuliertem Microcontroller starten:

```powershell
.\.venv\Scripts\python.exe bridge.py --config config.toml --simulate
```

Der erfolgreiche erste Verbindungstest zeigte bei uns unter anderem:

```text
Microcontroller bereit: LICHESS-BRIDGE/1 SIMULATED-CHESS-MCU
Serielle Verbindung geprüft.
GET https://lichess.org/api/account "HTTP/1.1 200 OK"
Lichess verbunden: BOT @espsf19testbot
Rolle ACCEPT: warte auf eine Challenge von @frank1123b
GET https://lichess.org/api/stream/event "HTTP/1.1 200 OK"
```

Damit waren Token, BOT-Status, Account-Abfrage und Event-Stream bestätigt.

## 8. Erste Partie gegen das normale Konto

1. Bridge mit `role = "ACCEPT"` gestartet lassen.
2. Mit dem normalen Lichess-Konto `frank1123b` anmelden.
3. Das Bot-Profil öffnen: https://lichess.org/@/espsf19testbot.
4. Eine Challenge für Standard-Schach senden (im ersten Test unbewertet).
5. Die Challenge muss vom erlaubten Konto kommen; andere Herausforderer werden abgelehnt.
6. Nach Annahme öffnet die Bridge den Bot-Game-Stream und überträgt die Züge.

Unsere erste Testpartie war öffentlich unter https://lichess.org/EtpnOLuW erreichbar. Der simulierte Microcontroller spielte zufällige legale Züge. Die Logs bestätigten HTTP 200 für Challenge-Annahme und Zugübertragungen.

## 9. Zweiter BOT für den zweiten Standort

Für die geplante Verbindung zweier räumlich getrennter Schachcomputer ist je Standort ein eigenes BOT-Konto und Token vorgesehen. Vor dem Erstellen des zweiten BOT-Kontos unbedingt die Mehrfachkontenregel beachten: Das normale Konto `frank1123b` und `espsf19testbot` sind bereits zwei Konten. Ein zweites BOT-Konto wäre damit das dritte Konto. Die Lichess-Regeln erlauben das nicht pauschal; im Zweifel vorher den Lichess-Support bzw. die Moderation mit einer kurzen Beschreibung des technischen Projekts fragen.

Erst wenn das geklärt ist:

1. Zweites Konto registrieren, beispielsweise mit einem eigenen Benutzernamen.
2. Noch keine Partie mit diesem Konto spielen.
3. Eigenes Personal API Token mit den benötigten Berechtigungen erstellen.
4. Token mit `POST /api/bot/account/upgrade` hochstufen.
5. Per `GET /api/account` prüfen, dass `title = BOT` zurückgegeben wird.
6. Am zweiten PC eigene `config.toml` mit dem zweiten Bot-Namen, eigenem Token und dem Gegenkonto einrichten.
7. Die Rollen aufteilen: ein PC `CHALLENGE`, der andere `ACCEPT`.

Die Bridge-Software bleibt an beiden Standorten identisch; nur Konfiguration, Token und serieller Port unterscheiden sich.

## 10. Sicherheit und Fehlerbehebung

- Token nicht in Dateien eintragen, die in Git eingecheckt werden.
- Falls ein Token versehentlich veröffentlicht wurde: sofort in den Lichess-Kontoeinstellungen widerrufen und ein neues erzeugen.
- `HTTP 401`: Token fehlt, ist ungültig oder gehört nicht zum konfigurierten Konto.
- `title` fehlt oder ist nicht `BOT`: falsches Konto oder BOT-Umstellung nicht erfolgreich.
- `HTTP 403`: Berechtigung oder Kontostatus prüfen.
- `HTTP 429`: laut Lichess mindestens eine volle Minute warten und nicht weiter Requests senden.
- Der BOT-Status und die irreversiblen Folgen der Hochstufung sollten vor jeder Umstellung nochmals geprüft werden.

Weitere Quellen: [Lichess API](https://lichess.org/api), [API-Tipps und Rate Limits](https://lichess.org/page/api-tips), [Terms of Service](https://lichess.org/terms-of-service), [Fair Play](https://lichess.org/page/fair-play).
