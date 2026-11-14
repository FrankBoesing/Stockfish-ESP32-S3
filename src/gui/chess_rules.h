#pragma once

#include <cstddef>

#include "stockfish/movegen.h"
#include "stockfish/position.h"
#include "stockfish/position_status.h"

namespace ChessGuiRules {

// Mirrors the board's 180-degree rotation when the human plays Black.
inline int screenToSquare(int x, int y, int boardX, int boardY,
                          int boardSize, int squareSize, char playerColor) {
  if (squareSize <= 0 || x < boardX || x >= boardX + boardSize
      || y < boardY || y >= boardY + boardSize)
    return -1;

  const int col = (x - boardX) / squareSize;
  const int row = (y - boardY) / squareSize;
  const int file = playerColor == 'w' ? col : 7 - col;
  const int rank = playerColor == 'w' ? 7 - row : row;
  return rank * 8 + file;
}

// Return all legal GUI destination squares for one selected piece. Stockfish
// internally encodes standard castling as king-to-rook; the GUI must highlight
// the king's final square (c- or g-file), not the rook's start square.
inline bool legalDestinations(const Stockfish::Position& position, int fromSquare,
                              bool destinations[64]) {
  for (int square = 0; square < 64; ++square)
    destinations[square] = false;

  if (fromSquare < 0 || fromSquare >= 64)
    return false;

  bool found = false;
  for (const Stockfish::Move move : Stockfish::MoveList<Stockfish::LEGAL>(position)) {
    if (static_cast<int>(move.from_sq()) != fromSquare)
      continue;

    Stockfish::Square to = move.to_sq();
    if (move.type_of() == Stockfish::CASTLING) {
      const bool kingSide = Stockfish::file_of(to) > Stockfish::file_of(move.from_sq());
      to = Stockfish::make_square(kingSide ? Stockfish::FILE_G : Stockfish::FILE_C,
                                  Stockfish::rank_of(move.from_sq()));
    }

    destinations[static_cast<int>(to)] = true;
    found = true;
  }
  return found;
}

inline bool isPawnPromotion(char movingPiece, int toSquare, char playerColor) {
  if (toSquare < 0 || toSquare >= 64
      || (movingPiece != 'p' && movingPiece != 'P'))
    return false;

  return (playerColor == 'w' && toSquare / 8 == 7)
      || (playerColor == 'b' && toSquare / 8 == 0);
}

// The GUI's established takeback policy removes a human/engine pair, or the
// only available ply when the game history contains just one move.
inline std::size_t undoPliesToRemove(std::size_t historySize) {
  return historySize >= 2 ? 2 : historySize;
}

enum class GameOutcome {
  Active,
  Check,
  CheckmatePlayerLoss,
  CheckmatePlayerWin,
  Stalemate,
  DrawFiftyMove,
  DrawRepetition,
  DrawMaterial
};

// Keep precedence identical to the live GUI: mate/stalemate before all draw
// counters, then fifty-move, repetition and insufficient material.
inline GameOutcome classifyGame(const Stockfish::PositionStatus& status,
                                char sideToMove, char playerColor) {
  if (!status.hasLegalMoves) {
    if (!status.inCheck)
      return GameOutcome::Stalemate;
    return sideToMove == playerColor ? GameOutcome::CheckmatePlayerLoss
                                     : GameOutcome::CheckmatePlayerWin;
  }
  if (status.drawByFiftyMove)
    return GameOutcome::DrawFiftyMove;
  if (status.drawByRepetition)
    return GameOutcome::DrawRepetition;
  if (status.insufficientMaterial)
    return GameOutcome::DrawMaterial;
  return status.inCheck ? GameOutcome::Check : GameOutcome::Active;
}

inline bool isTerminal(GameOutcome outcome) {
  return outcome != GameOutcome::Active && outcome != GameOutcome::Check;
}

inline const char* statusText(GameOutcome outcome) {
  switch (outcome) {
    case GameOutcome::Active: return "Your move";
    case GameOutcome::Check: return "Check!";
    case GameOutcome::CheckmatePlayerLoss: return "You lose";
    case GameOutcome::CheckmatePlayerWin: return "You win";
    case GameOutcome::Stalemate: return "Stalemate";
    case GameOutcome::DrawFiftyMove: return "Draw: 50 moves";
    case GameOutcome::DrawRepetition: return "Draw: repetition";
    case GameOutcome::DrawMaterial: return "Draw: material";
  }
  return "Position error";
}

}  // namespace ChessGuiRules
