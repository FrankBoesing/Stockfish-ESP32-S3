#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "game_history.h"

namespace SerialConsole {

constexpr size_t MaxFenBytes = 192;

struct GameState {
  bool hasGame;
  bool gameOver;
  bool searchInFlight;
  bool autoPlayEnabled;
  char sideToMove;
  char playerColor;
  int selectedSkillLevel;
  int skillLevel;
  size_t moveCount;
  bool openingBookEnabled;
  bool openingBookReady;
};

struct Callbacks {
  const char *startFen;
  void (*getState)(GameState &state);
  void (*printBoard)();
  void (*printUciPosition)();
  void (*undoMove)();
  void (*startHint)();
  void (*startCalibration)();
  bool (*saveSkillLevel)(int level);
  void (*setOpeningBookEnabled)(bool enabled);
  void (*startNewGame)(int selectedSkillLevel, char playerColor);
  bool (*setAutoPlay)(bool enabled);
  bool (*applyPosition)(const std::string &baseFen, GameHistory &history);
  bool (*applyMove)(const std::string &uciMove);
  void (*exportPgn)();
  bool (*importPgn)(const std::string &pgn);

  bool (*setBotMode)(bool enabled);
  bool (*botNewGame)();
  bool (*botSetPosition)(const std::string &fen);
  bool (*botStartSearch)(uint32_t movetimeMs);
  void (*botStopSearch)();
};

void begin(const Callbacks &callbacks);
void poll();
void completeBotSearch(const char *uciMove);

} // namespace SerialConsole
