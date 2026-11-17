#include "serial_console.h"

#include <Arduino.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr size_t SerialCommandCapacity = 2048;
constexpr size_t PgnImportMaxBytes = 32768;
constexpr size_t SerialPositionTokenCapacity = 96;

char serialCommand[SerialCommandCapacity] = {};
size_t serialCommandLength = 0;
bool serialCommandOverflow = false;

bool pgnImportActive = false;
std::string pgnImportBuffer;

enum class SerialPositionState : uint8_t {
  Idle,
  ExpectFormat,
  FenFields,
  ExpectMoves,
  Moves
};

bool serialPositionActive = false;
bool serialPositionFailed = false;
SerialPositionState serialPositionState = SerialPositionState::Idle;
char serialPositionToken[SerialPositionTokenCapacity] = {};
size_t serialPositionTokenLength = 0;
char serialPositionFen[SerialConsole::MaxFenBytes + 1] = {};
size_t serialPositionFenLength = 0;
uint8_t serialPositionFenFieldCount = 0;
GameHistory serialPositionHistory;

SerialConsole::Callbacks consoleCallbacks{};
bool consoleReady = false;

static SerialConsole::GameState getGameState() {
  SerialConsole::GameState state{};
  if (consoleCallbacks.getState)
    consoleCallbacks.getState(state);
  return state;
}

static bool serialPositionTokenEquals(const char *expected) {
  const size_t expectedLength = strlen(expected);
  if (serialPositionTokenLength != expectedLength)
    return false;
  for (size_t i = 0; i < expectedLength; ++i) {
    if (tolower((unsigned char)serialPositionToken[i])
        != tolower((unsigned char)expected[i]))
      return false;
  }
  return true;
}

static void failSerialPosition(const char *message, const char *token = nullptr) {
  if (!serialPositionFailed) {
    if (token)
      Serial.printf("[SERIAL] ERROR: %s: '%s'\n", message, token);
    else
      Serial.printf("[SERIAL] ERROR: %s\n", message);
  }
  serialPositionFailed = true;
}

static void consumeSerialPositionToken() {
  if (serialPositionFailed || !serialPositionTokenLength)
    return;

  serialPositionToken[serialPositionTokenLength] = '\0';

  switch (serialPositionState) {
    case SerialPositionState::ExpectFormat:
      if (serialPositionTokenEquals("startpos")) {
        if (!consoleCallbacks.startFen) {
          failSerialPosition("starting FEN is unavailable");
          break;
        }
        serialPositionFenLength = strlen(consoleCallbacks.startFen);
        if (serialPositionFenLength > SerialConsole::MaxFenBytes) {
          failSerialPosition("starting FEN exceeds the saved-game limit");
          break;
        }
        memcpy(serialPositionFen, consoleCallbacks.startFen,
               serialPositionFenLength + 1);
        serialPositionState = SerialPositionState::ExpectMoves;
      } else if (serialPositionTokenEquals("fen")) {
        serialPositionState = SerialPositionState::FenFields;
      } else {
        failSerialPosition("expected 'startpos' or 'fen'", serialPositionToken);
      }
      break;

    case SerialPositionState::FenFields: {
      const size_t separatorLength = serialPositionFenFieldCount ? 1 : 0;
      const size_t required = serialPositionFenLength + separatorLength
                            + serialPositionTokenLength;
      if (required > SerialConsole::MaxFenBytes) {
        failSerialPosition("FEN exceeds the saved-game limit");
        break;
      }
      if (separatorLength)
        serialPositionFen[serialPositionFenLength++] = ' ';
      memcpy(serialPositionFen + serialPositionFenLength,
             serialPositionToken, serialPositionTokenLength);
      serialPositionFenLength += serialPositionTokenLength;
      serialPositionFen[serialPositionFenLength] = '\0';
      if (++serialPositionFenFieldCount == 6)
        serialPositionState = SerialPositionState::ExpectMoves;
      break;
    }

    case SerialPositionState::ExpectMoves:
      if (serialPositionTokenEquals("moves")) {
        serialPositionState = SerialPositionState::Moves;
      } else {
        failSerialPosition("expected 'moves' after the position", serialPositionToken);
      }
      break;

    case SerialPositionState::Moves:
      if (!serialPositionHistory.appendUci(
            std::string_view(serialPositionToken, serialPositionTokenLength)))
        failSerialPosition("invalid UCI move or move-history limit reached",
                           serialPositionToken);
      break;

    default:
      failSerialPosition("invalid position parser state");
      break;
  }

  serialPositionTokenLength = 0;
}

static void feedSerialPositionByte(char value) {
  if (serialPositionFailed)
    return;

  if (isspace((unsigned char)value)) {
    consumeSerialPositionToken();
    return;
  }

  if (serialPositionTokenLength + 1 >= sizeof(serialPositionToken)) {
    failSerialPosition("position token is too long");
    return;
  }

  serialPositionToken[serialPositionTokenLength++] = value;
}

static void releaseSerialPositionHistory() {
  GameHistory empty;
  serialPositionHistory.swap(empty);
}

static void resetSerialPositionStream() {
  serialPositionActive = false;
  serialPositionFailed = false;
  serialPositionState = SerialPositionState::Idle;
  serialPositionTokenLength = 0;
  serialPositionFenLength = 0;
  serialPositionFenFieldCount = 0;
  serialPositionFen[0] = '\0';
  serialPositionHistory.clear();
  releaseSerialPositionHistory();
  serialCommandLength = 0;
  serialCommandOverflow = false;
}

static void finishSerialPositionStream() {
  if (!serialPositionFailed)
    consumeSerialPositionToken();

  if (!serialPositionFailed) {
    if (serialPositionState == SerialPositionState::ExpectFormat)
      failSerialPosition("usage: position startpos [moves ...] OR position fen <6 FEN fields> [moves ...]");
    else if (serialPositionState == SerialPositionState::FenFields)
      failSerialPosition("FEN requires all six fields");
    else if (serialPositionState != SerialPositionState::ExpectMoves
             && serialPositionState != SerialPositionState::Moves)
      failSerialPosition("incomplete position command");
  }

  if (!serialPositionFailed) {
    serialPositionFen[serialPositionFenLength] = '\0';
    Serial.printf("[SERIAL] POSITION received; %u plies; loading\n",
                  (unsigned)serialPositionHistory.size());
    const std::string baseFen(serialPositionFen, serialPositionFenLength);
    if (consoleCallbacks.applyPosition)
      consoleCallbacks.applyPosition(baseFen, serialPositionHistory);
  }

  resetSerialPositionStream();
}

static void beginSerialPositionStream(bool shorthandFen) {
  serialPositionActive = true;
  serialPositionFailed = false;
  serialPositionState = shorthandFen ? SerialPositionState::FenFields
                                     : SerialPositionState::ExpectFormat;
  serialPositionTokenLength = 0;
  serialPositionFenLength = 0;
  serialPositionFenFieldCount = 0;
  serialPositionFen[0] = '\0';
  serialPositionHistory.clear();
  serialCommandLength = 0;
  serialCommandOverflow = false;
  Serial.printf("[SERIAL] RX: %s (streaming)\n",
                shorthandFen ? "fen" : "position");
}

static bool tryBeginSerialPositionStream() {
  size_t first = 0;
  while (first < serialCommandLength
         && isspace((unsigned char)serialCommand[first]))
    ++first;
  if (first == serialCommandLength)
    return false;

  size_t end = first;
  while (end < serialCommandLength
         && !isspace((unsigned char)serialCommand[end]))
    ++end;
  if (end != serialCommandLength)
    return false;

  const size_t length = end - first;
  const auto tokenIs = [&](const char *expected) {
    const size_t expectedLength = strlen(expected);
    if (length != expectedLength)
      return false;
    for (size_t i = 0; i < length; ++i)
      if (tolower((unsigned char)serialCommand[first + i])
          != tolower((unsigned char)expected[i]))
        return false;
    return true;
  };

  if (tokenIs("position")) {
    beginSerialPositionStream(false);
    return true;
  }
  if (tokenIs("fen")) {
    beginSerialPositionStream(true);
    return true;
  }
  return false;
}

static bool processPositionCommand(const std::string &line, bool shortFenCommand) {
  std::istringstream input(line);
  std::string keyword;
  input >> keyword;
  std::string form;
  if (shortFenCommand) {
    form = "fen";
  } else if (!(input >> form)) {
    Serial.println("[SERIAL] Usage: position startpos [moves ...] OR position fen <6 FEN fields> [moves ...]");
    return false;
  }

  std::transform(form.begin(), form.end(), form.begin(),
                 [](unsigned char ch) { return char(tolower(ch)); });
  std::string baseFen;
  if (form == "startpos") {
    if (!consoleCallbacks.startFen) {
      Serial.println("[SERIAL] ERROR: starting FEN is unavailable");
      return false;
    }
    baseFen = consoleCallbacks.startFen;
  } else if (form == "fen") {
    std::string fields[6];
    for (unsigned i = 0; i < 6; ++i) {
      if (!(input >> fields[i])) {
        Serial.println("[SERIAL] ERROR: FEN requires all six fields");
        return false;
      }
    }
    for (unsigned i = 0; i < 6; ++i) {
      if (i)
        baseFen += ' ';
      baseFen += fields[i];
    }
  } else {
    Serial.printf("[SERIAL] ERROR: unknown position format '%s'\n", form.c_str());
    return false;
  }

  if (baseFen.empty() || baseFen.size() > SerialConsole::MaxFenBytes) {
    Serial.printf("[SERIAL] ERROR: FEN must be 1..%u bytes to fit the saved-game format\n",
                  (unsigned)SerialConsole::MaxFenBytes);
    return false;
  }

  GameHistory candidateHistory;
  std::string token;
  if (input >> token) {
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char ch) { return char(tolower(ch)); });
    if (token != "moves") {
      Serial.printf("[SERIAL] ERROR: expected 'moves', got '%s'\n", token.c_str());
      return false;
    }
    while (input >> token) {
      if (!candidateHistory.appendUci(token)) {
        Serial.printf("[SERIAL] ERROR: invalid UCI syntax or too many moves: '%s'\n",
                      token.c_str());
        return false;
      }
    }
  }

  if (consoleCallbacks.applyPosition)
    return consoleCallbacks.applyPosition(baseFen, candidateHistory);
  return false;
}

static bool pgnHasResultMarker(const std::string &pgn) {
  bool inTag = false;
  bool tagQuoted = false;
  bool tagEscaped = false;
  bool inBraceComment = false;
  bool inLineComment = false;
  bool movetextStarted = false;
  unsigned variationDepth = 0;
  std::string token;

  const auto isResultToken = [](const std::string &value) {
    return value == "1-0" || value == "0-1"
        || value == "1/2-1/2" || value == "*";
  };
  const auto finishToken = [&]() {
    const bool found = isResultToken(token);
    token.clear();
    return found;
  };

  for (const char ch : pgn) {
    if (inTag) {
      if (tagEscaped) {
        tagEscaped = false;
        continue;
      }
      if (tagQuoted && ch == '\\') {
        tagEscaped = true;
        continue;
      }
      if (ch == '"') {
        tagQuoted = !tagQuoted;
        continue;
      }
      if (!tagQuoted && ch == ']')
        inTag = false;
      continue;
    }
    if (inBraceComment) {
      if (ch == '}')
        inBraceComment = false;
      continue;
    }
    if (inLineComment) {
      if (ch == '\n' || ch == '\r')
        inLineComment = false;
      continue;
    }

    if (ch == '{') {
      if (finishToken()) return true;
      inBraceComment = true;
      continue;
    }
    if (ch == ';') {
      if (finishToken()) return true;
      inLineComment = true;
      continue;
    }
    if (ch == '[' && !movetextStarted && variationDepth == 0) {
      if (finishToken()) return true;
      inTag = true;
      tagQuoted = false;
      tagEscaped = false;
      continue;
    }
    if (ch == '(') {
      if (finishToken()) return true;
      ++variationDepth;
      continue;
    }
    if (ch == ')') {
      if (variationDepth > 0)
        --variationDepth;
      continue;
    }
    if (variationDepth > 0)
      continue;

    if (isspace((unsigned char)ch)) {
      if (finishToken()) return true;
      continue;
    }

    movetextStarted = true;
    token.push_back(ch);
  }

  return finishToken();
}

static bool isUciMoveToken(const std::string &command) {
  return (command.size() == 4 || command.size() == 5)
      && command[0] >= 'a' && command[0] <= 'h'
      && command[1] >= '1' && command[1] <= '8'
      && command[2] >= 'a' && command[2] <= 'h'
      && command[3] >= '1' && command[3] <= '8'
      && (command.size() == 4 || command[4] == 'q' || command[4] == 'r'
                              || command[4] == 'b' || command[4] == 'n');
}

static void executeSerialCommand() {
  if (serialCommandOverflow) {
    if (pgnImportActive) {
      pgnImportActive = false;
      pgnImportBuffer.clear();
      Serial.println("[PGN] ERROR: input line exceeded the serial line buffer; import cancelled");
    } else {
      Serial.printf("[SERIAL] ERROR: command exceeded %u bytes; discarded\n",
                    (unsigned)(SerialCommandCapacity - 1));
    }
    serialCommandLength = 0;
    serialCommandOverflow = false;
    return;
  }

  serialCommand[serialCommandLength] = '\0';
  const std::string line(serialCommand, serialCommandLength);
  if (pgnImportActive) {
    std::string control = line;
    control.erase(0, control.find_first_not_of(" \t\r\n"));
    const size_t last = control.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) control.clear();
    else control.erase(last + 1);
    std::transform(control.begin(), control.end(), control.begin(),
                   [](unsigned char ch) { return char(tolower(ch)); });
    if (control == "endpgn" || control == "pgn end") {
      pgnImportActive = false;
      Serial.printf("[PGN] Received %u bytes; validating...\n",
                    unsigned(pgnImportBuffer.size()));
      if (consoleCallbacks.importPgn)
        consoleCallbacks.importPgn(pgnImportBuffer);
      pgnImportBuffer.clear();
    } else if (control == "cancelpgn" || control == "pgn cancel") {
      pgnImportActive = false;
      pgnImportBuffer.clear();
      Serial.println("[PGN] Import cancelled");
    } else if (pgnImportBuffer.size() + line.size() + 1 > PgnImportMaxBytes) {
      pgnImportActive = false;
      pgnImportBuffer.clear();
      Serial.printf("[PGN] ERROR: import exceeds %u bytes; cancelled\n",
                    unsigned(PgnImportMaxBytes));
    } else {
      pgnImportBuffer += line;
      pgnImportBuffer.push_back('\n');
      if (pgnHasResultMarker(pgnImportBuffer)) {
        pgnImportActive = false;
        Serial.printf("[PGN] Result marker received; validating %u bytes...\n",
                      unsigned(pgnImportBuffer.size()));
        if (consoleCallbacks.importPgn)
          consoleCallbacks.importPgn(pgnImportBuffer);
        pgnImportBuffer.clear();
      }
    }
    serialCommandLength = 0;
    return;
  }

  if (!line.empty())
    Serial.printf("[SERIAL] RX (%u bytes): %s\n",
                  (unsigned)line.size(), line.c_str());

  std::istringstream input(line);
  std::string command;
  input >> command;
  std::transform(command.begin(), command.end(), command.begin(),
                 [](unsigned char ch) { return char(tolower(ch)); });

  if (command == "print") {
    if (consoleCallbacks.printBoard)
      consoleCallbacks.printBoard();
  } else if (command == "moves" || command == "uci") {
    if (consoleCallbacks.printUciPosition)
      consoleCallbacks.printUciPosition();
  } else if (command == "undo") {
    const SerialConsole::GameState before = getGameState();
    if (!before.hasGame) {
      Serial.println("[SERIAL] ERROR: no active game");
    } else {
      if (consoleCallbacks.undoMove)
        consoleCallbacks.undoMove();
      const SerialConsole::GameState after = getGameState();
      const size_t removedPlies = before.moveCount > after.moveCount
                                ? before.moveCount - after.moveCount : 0;
      if (removedPlies)
        Serial.printf("[UNDO] Removed %u plies; %u remain\n",
                      unsigned(removedPlies), unsigned(after.moveCount));
      else
        Serial.println("[UNDO] No move was removed");
    }
  } else if (command == "hint") {
    const SerialConsole::GameState state = getGameState();
    if (!state.hasGame)
      Serial.println("[SERIAL] ERROR: no active game");
    else if (state.gameOver)
      Serial.println("[SERIAL] ERROR: game is over");
    else if (state.searchInFlight)
      Serial.println("[SERIAL] ERROR: a search is already running");
    else if (state.sideToMove != state.playerColor)
      Serial.println("[SERIAL] ERROR: it is not the player's turn");
    else if (consoleCallbacks.startHint)
      consoleCallbacks.startHint();
  } else if (command == "calibration") {
    std::string extra;
    if (input >> extra) {
      Serial.println("[SERIAL] Usage: CALIBRATION");
    } else {
      if (consoleCallbacks.startCalibration)
        consoleCallbacks.startCalibration();
      Serial.println("[GUI] Touch calibration started");
    }
  } else if (command == "level") {
    std::string levelText;
    std::string extra;
    if (!(input >> levelText)) {
      const SerialConsole::GameState state = getGameState();
      Serial.printf("[SETTINGS] Next game: Level %d; current game: Level %d\n",
                    state.selectedSkillLevel, state.skillLevel);
    } else {
      int requestedLevel = 0;
      bool validLevel = !levelText.empty();
      for (unsigned char ch : levelText) {
        if (ch < '0' || ch > '9') {
          validLevel = false;
          break;
        }
        requestedLevel = requestedLevel * 10 + (ch - '0');
        if (requestedLevel > 20) {
          validLevel = false;
          break;
        }
      }
      if (!validLevel || requestedLevel < 1 || requestedLevel > 20 || (input >> extra)) {
        Serial.println("[SERIAL] Usage: LEVEL [1..20]");
      } else if (consoleCallbacks.saveSkillLevel
                 && consoleCallbacks.saveSkillLevel(requestedLevel)) {
        Serial.printf("[SETTINGS] Next game will use Level %d\n", requestedLevel);
      }
    }
  } else if (command == "book") {
    std::string requestedState;
    std::string extra;
    if (!(input >> requestedState)) {
      const SerialConsole::GameState state = getGameState();
      Serial.printf("[BOOK] Opening book is %s%s\n",
                    state.openingBookEnabled ? "ON" : "OFF",
                    state.openingBookEnabled && !state.openingBookReady
                      ? " (image unavailable; Stockfish fallback)" : "");
    } else {
      std::transform(requestedState.begin(), requestedState.end(), requestedState.begin(),
                     [](unsigned char ch) { return char(tolower(ch)); });
      if ((requestedState != "on" && requestedState != "off") || (input >> extra)) {
        Serial.println("[SERIAL] Usage: BOOK [ON|OFF]");
      } else if (consoleCallbacks.setOpeningBookEnabled) {
        consoleCallbacks.setOpeningBookEnabled(requestedState == "on");
      }
    }
  } else if (command == "pgn" || command == "exportpgn") {
    std::string action;
    std::string extra;
    if (!(input >> action)) {
      if (consoleCallbacks.exportPgn)
        consoleCallbacks.exportPgn();
    } else {
      std::transform(action.begin(), action.end(), action.begin(),
                     [](unsigned char ch) { return char(tolower(ch)); });
      if (action == "import" && !(input >> extra)) {
        pgnImportBuffer.clear();
        pgnImportActive = true;
        Serial.println("[PGN] Import mode active. Paste the PGN; import starts at its result marker.");
        Serial.println("[PGN] ENDPGN also finishes manually; CANCELPGN aborts.");
      } else if (action == "export" && !(input >> extra)) {
        if (consoleCallbacks.exportPgn)
          consoleCallbacks.exportPgn();
      } else {
        Serial.println("[SERIAL] Usage: PGN [EXPORT|IMPORT]");
      }
    }
  } else if (command == "position") {
    processPositionCommand(line, false);
  } else if (command == "fen") {
    processPositionCommand(line, true);
  } else if (command == "newgame") {
    const SerialConsole::GameState state = getGameState();
    std::string argument;
    int requestedSkill = state.selectedSkillLevel;
    char requestedPlayer = state.playerColor;
    bool skillSpecified = false;
    bool playerSpecified = false;
    bool valid = true;

    // Accept either ordering: NEWGAME 12 BLACK or NEWGAME BLACK 12.
    while (input >> argument) {
      std::transform(argument.begin(), argument.end(), argument.begin(),
                     [](unsigned char ch) { return char(tolower(ch)); });

      if (argument == "white" || argument == "black") {
        if (playerSpecified) {
          valid = false;
          break;
        }
        playerSpecified = true;
        requestedPlayer = argument == "white" ? 'w' : 'b';
        continue;
      }

      if (skillSpecified || argument.empty()) {
        valid = false;
        break;
      }

      int parsedSkill = 0;
      for (unsigned char ch : argument) {
        if (ch < '0' || ch > '9') {
          valid = false;
          break;
        }
        parsedSkill = parsedSkill * 10 + (ch - '0');
        if (parsedSkill > 20) {
          valid = false;
          break;
        }
      }
      if (!valid || parsedSkill < 1 || parsedSkill > 20) {
        valid = false;
        break;
      }
      skillSpecified = true;
      requestedSkill = parsedSkill;
    }

    if (!valid) {
      Serial.println("[SERIAL] Usage: NEWGAME [1..20] [WHITE|BLACK] (colour may also be given alone)");
    } else if (consoleCallbacks.startNewGame) {
      consoleCallbacks.startNewGame(requestedSkill, requestedPlayer);
    }
  } else if (command == "help") {
    Serial.println("Commands:");
    Serial.println("  position startpos [moves e2e4 ...] (streamed; up to 4096 plies)");
    Serial.println("  position fen <6 FEN fields> [moves ...] (streamed)");
    Serial.println("  fen <6 FEN fields>  (streamed shorthand)");
    Serial.println("  print  print the current board in Stockfish ASCII format");
    Serial.println("  moves (or uci)  print current position as a UCI command");
    Serial.println("  e2e4 [or e7e8q]  apply one legal UCI move; O-O / 0-0 = short castle");
    Serial.println("  UNDO  undo the last move pair (or the only move)");
    Serial.println("  HINT  ask Stockfish for a move suggestion");
    Serial.println("  CALIBRATION  start touch calibration");
    Serial.println("  LEVEL [1..20]  show/set skill for the next game");
    Serial.println("  BOOK [ON|OFF]  show or set opening-book usage");
    Serial.println("  PGN [EXPORT|IMPORT]  export or import a PGN game");
    Serial.println("      PGN IMPORT: auto-finishes at result; ENDPGN is optional");
    Serial.println("  NEWGAME [1..20] [WHITE|BLACK]  start a new game");
    Serial.println("  NEWGAME BLACK  start as Black (or use WHITE)");
    Serial.println("FEN piece-letter case is preserved. Invalid positions leave the current game unchanged.");
  } else if (!command.empty()) {
    // Accept short castling notation as an alias for the standard UCI king move.
    if (command == "o-o" || command == "0-0") {
      const SerialConsole::GameState state = getGameState();
      command = state.sideToMove == 'w' ? "e1g1" : "e8g8";
    }

    if (isUciMoveToken(command)) {
      if (consoleCallbacks.applyMove)
        consoleCallbacks.applyMove(command);
    } else {
      Serial.printf("[SERIAL] Unknown command: %s\n", command.c_str());
    }
  }

  serialCommandLength = 0;
}

} // namespace

namespace SerialConsole {

void begin(const Callbacks &callbacks) {
  consoleCallbacks = callbacks;

  const size_t serialRxBuffer = Serial.setRxBufferSize(SerialCommandCapacity);
  Serial.begin(115200);
  if (serialRxBuffer < SerialCommandCapacity)
    Serial.printf("[SERIAL] WARNING: USB-CDC RX buffer is %u bytes (requested %u)\n",
                  (unsigned)serialRxBuffer, (unsigned)SerialCommandCapacity);
  else
    Serial.printf("[SERIAL] USB-CDC RX buffer: %u bytes\n",
                  (unsigned)serialRxBuffer);

  consoleReady = true;
}

void poll() {
  if (!consoleReady)
    return;

  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value == '\r') continue;

    if (serialPositionActive) {
      if (value == '\n')
        finishSerialPositionStream();
      else
        feedSerialPositionByte(char(value));
      continue;
    }

    if (!pgnImportActive && !serialCommandOverflow
        && (value == ' ' || value == '\t' || value == '\n')
        && tryBeginSerialPositionStream()) {
      if (value == '\n')
        finishSerialPositionStream();
      continue;
    }

    if (value == '\n') {
      executeSerialCommand();
      continue;
    }
    if (serialCommandOverflow)
      continue; // Discard the rest of an overlong line up to its newline.
    if (serialCommandLength + 1 < sizeof(serialCommand))
      serialCommand[serialCommandLength++] = char(value);
    else {
      serialCommandLength = 0;
      serialCommandOverflow = true;
    }
  }
}

} // namespace SerialConsole
