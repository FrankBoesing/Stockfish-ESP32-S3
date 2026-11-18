# Setting Up Lichess BOT Accounts (Windows / PowerShell)

[English](ACCOUNT-SETUP-en.md) | [Deutsch](ACCOUNT-SETUP-DE.md)

This guide documents the setup used for the test account `espsf19testbot`, its irreversible upgrade to a Lichess BOT account, and the subsequent Python bridge tests.

> **Important — multiple accounts:** Lichess does not generally permit multiple accounts without restrictions. Its Terms of Service list specific exceptions and leave the decision to Lichess. More than three accounts are generally not accepted. This is not blanket permission to operate three accounts. Before creating another BOT account, clarify the intended use with Lichess. See the [Lichess Terms of Service](https://lichess.org/terms-of-service).

## 1. Register a bot user account

1. Open https://lichess.org/signup.
2. Register a dedicated username for the chess computer. Our test account was `espsf19testbot`.
3. Use an email address you can access and complete email verification if requested.
4. Sign in with the new account.

### Using one email address for multiple registrations

We considered Gmail plus-addressing as a possible approach. For example, Gmail may deliver addresses such as these to the same inbox:

- Existing address: `name@gmail.com`
- Possible alias for Bot A: `name+lichess-a@gmail.com`
- Possible alias for Bot B: `name+lichess-b@gmail.com`

**Assumption / not verified with Lichess:** Whether Lichess accepts such an alias for another registration must be checked in the registration form. This does not guarantee that multiple Lichess accounts are permitted. Do not use someone else's email address or an invented address.

## 2. Do not play a game before upgrading the account

The new account must not have played any games before it is upgraded. The upgrade is irreversible; afterwards, the account can only play as a bot through the Bot API.

The successful account query for our test account showed:

- `username`: `espsf19testbot`
- `title`: `BOT`
- `count.all`: `0`

The `count.all = 0` field confirmed that the account had not played a game before the upgrade.

Official documentation: [Upgrade to Bot account](https://lichess.org/api#operation/botAccountUpgrade).

## 3. Create a Personal API Token

1. While signed in as `espsf19testbot`, open https://lichess.org/account/oauth/token.
2. Create a new Personal API Token, for example with the description `Stockfish ESP32 S3 Bridge`.
3. Enable the permissions required by the bridge:
   - `bot:play`: play bot games, use the game stream and submit moves. This permission is also required for upgrading the account to BOT.
   - `challenge:read`: read incoming challenge events.
   - `challenge:write`: create, accept and decline challenges.
4. Generate the token and copy it to a secure location immediately. Treat it like a password.

Never put the token in Git, in `config.toml`, in screenshots or in chat messages.

Official pages: [Lichess API documentation](https://lichess.org/api) and [Personal API Tokens](https://lichess.org/account/oauth/token).

## 4. Irreversibly upgrade the account to BOT

Use the official endpoint:

`POST https://lichess.org/api/bot/account/upgrade`

In Windows PowerShell, first set the token for the current session only:

```powershell
$env:LICHESS_TOKEN = "PASTE_THE_REAL_TOKEN_HERE"
```

Then perform the upgrade:

```powershell
curl.exe -d "" "https://lichess.org/api/bot/account/upgrade" `
  -H "Authorization: Bearer $env:LICHESS_TOKEN"
```

The request must succeed with HTTP 200. For our account, we then queried the profile to verify its status.

**Check once more before sending the POST request:** the correct user is signed in, the token belongs to exactly that user, and the account has not played any games. This action cannot be undone.

## 5. Verify the BOT status

Use the same token:

```powershell
curl.exe "https://lichess.org/api/account" `
  -H "Authorization: Bearer $env:LICHESS_TOKEN"
```

For our test account, the JSON response included these fields:

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

The important fields are `username` and `title = BOT`. Other profile and statistics fields may differ.

## 6. Configure the Python bridge

In the `lichess-bridge` directory, copy the example configuration:

```powershell
Copy-Item config.example.toml config.toml
```

For an initial test against your regular account, for example, set:

```toml
[lichess]
username = "espsf19testbot"
token_env = "LICHESS_TOKEN"
base_url = "https://lichess.org"

[opponent]
allowed_username = "frank1123b"
role = "ACCEPT"
```

Initially, keep the remaining values from `config.example.toml`. The `config.toml` file is excluded by `.gitignore` and must not be committed.

Set the token in PowerShell:

```powershell
$env:LICHESS_TOKEN = "PASTE_THE_REAL_TOKEN_HERE"
```

This environment variable applies only to the current PowerShell window. It must be set again after closing that window.

## 7. Test the bridge and Lichess connection

From the `lichess-bridge` directory:

```powershell
python.exe -m venv .venv
.\\.venv\\Scripts\\python.exe -m pip install --upgrade pip
.\\.venv\\Scripts\\python.exe -m pip install -e ".[test]"
```

Then start the bridge with a simulated microcontroller:

```powershell
.\\.venv\\Scripts\\python.exe bridge.py --config config.toml --simulate
```

The successful initial connection test produced output similar to this:

```text
Microcontroller ready: LICHESS-BRIDGE/1 SIMULATED-CHESS-MCU
Serial connection verified.
GET https://lichess.org/api/account "HTTP/1.1 200 OK"
Lichess connected: BOT @espsf19testbot
Role ACCEPT: waiting for a challenge from @frank1123b
GET https://lichess.org/api/stream/event "HTTP/1.1 200 OK"
```

This confirmed the token, BOT status, account query and event stream.

## 8. First game against the regular account

1. Leave the bridge running with `role = "ACCEPT"`.
2. Sign in to Lichess with the regular account `frank1123b`.
3. Open the bot profile: https://lichess.org/@/espsf19testbot.
4. Send a challenge for standard chess (unrated for the first test).
5. The challenge must come from the explicitly allowed account; challenges from other users are declined.
6. After acceptance, the bridge opens the bot game stream and transfers the moves.

Our first test game was publicly available at https://lichess.org/EtpnOLuW. The simulated microcontroller played random legal moves. The logs confirmed HTTP 200 responses for challenge acceptance and move submissions.

The subsequent real-hardware test used the Freenove ESP32-S3 Display and the Stockfish firmware. The MCU returned a Polyglot book move (`b8c6`) and, after a book miss, a Stockfish search move (`a7a6`). Lichess accepted both moves, and the display showed the updated position. This confirms the basic end-to-end path on the reference hardware; reconnect and less common fault cases still need further testing.

## 9. A second BOT for the second location

For the planned setup with two geographically separated chess computers, one BOT account and token is intended for each location. Before creating a second BOT account, check the multiple-account policy carefully: the regular account `frank1123b` and `espsf19testbot` already make two accounts. A second BOT account would therefore be the third account. Lichess does not grant blanket permission for this; when in doubt, ask Lichess Support or the moderators first and briefly explain the technical project.

Only after clarifying this:

1. Register the second account, using a dedicated username.
2. Do not play any games with this account yet.
3. Create its own Personal API Token with the required permissions.
4. Upgrade it using `POST /api/bot/account/upgrade`.
5. Use `GET /api/account` to confirm that `title = BOT` is returned.
6. Configure a separate `config.toml` on the second PC with the second bot name, its own token and the opponent account.
7. Assign the roles: one PC uses `CHALLENGE`, the other uses `ACCEPT`.

The bridge software is identical at both locations; only the configuration, token and serial port differ.

## 10. Security and troubleshooting

- Never put tokens in files that are committed to Git.
- If a token is accidentally exposed, revoke it immediately in the Lichess account settings and create a new one.
- `HTTP 401`: the token is missing, invalid or belongs to a different account.
- `title` is missing or is not `BOT`: check the account and whether the BOT upgrade succeeded.
- `HTTP 403`: check the permissions and account status.
- `HTTP 429`: Lichess advises waiting at least one full minute; do not keep sending requests.
- Recheck the BOT status and the irreversible consequences of the upgrade before performing it.

Further references: [Lichess API](https://lichess.org/api), [API tips and rate limits](https://lichess.org/page/api-tips), [Terms of Service](https://lichess.org/terms-of-service) and [Fair Play](https://lichess.org/page/fair-play).
