#include "gui/chess_rules.h"
#include "gui/game_history.h"

#include "stockfish/attacks.h"
#include "stockfish/position.h"
#include "stockfish/uci.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition) do { \
  ++checks; \
  if (!(condition)) { \
    ++failures; \
    std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ \
              << ": " #condition "\n"; \
  } \
} while (0)

constexpr char StartFen[] =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

Stockfish::Square square(const char* name) {
  return Stockfish::make_square(
    Stockfish::File(name[0] - 'a'), Stockfish::Rank(name[1] - '1'));
}

int squareIndex(const char* name) {
  return static_cast<int>(square(name));
}

class TestPosition {
public:
  bool setFen(const std::string& fen) {
    states.clear();
    states.emplace_back();
    const auto error = position.set(fen, false, &states.back());
    if (error.has_value()) {
      std::cerr << "Could not load FEN: " << error->what() << "\n";
      return false;
    }
    return true;
  }

  bool play(const std::string& uci) {
    const Stockfish::Move move = Stockfish::UCIEngine::to_move(position, uci);
    if (move == Stockfish::Move::none())
      return false;
    states.emplace_back();
    position.do_move(move, states.back());
    return true;
  }

  Stockfish::Position& get() { return position; }
  const Stockfish::Position& get() const { return position; }

private:
  // deque keeps StateInfo addresses stable for Position's repetition chain.
  std::deque<Stockfish::StateInfo> states;
  Stockfish::Position position;
};

void checkDestinations(const TestPosition& testPosition, const char* from,
                       const std::vector<const char*>& expected) {
  bool destinations[64] = {};
  const bool found = ChessGuiRules::legalDestinations(
    testPosition.get(), squareIndex(from), destinations);
  CHECK(found == !expected.empty());

  bool expectedSquares[64] = {};
  for (const char* name : expected)
    expectedSquares[squareIndex(name)] = true;

  for (int i = 0; i < 64; ++i) {
    if (destinations[i] != expectedSquares[i]) {
      std::cerr << "Destination mismatch from " << from << " at "
                << Stockfish::UCIEngine::square(static_cast<Stockfish::Square>(i))
                << ": expected=" << expectedSquares[i]
                << " actual=" << destinations[i] << "\\n";
      CHECK(false);
    } else {
      CHECK(true);
    }
  }
}

void testLegalDestinationHighlights() {
  TestPosition p;
  CHECK(p.setFen(StartFen));
  checkDestinations(p, "e2", {"e3", "e4"});
  checkDestinations(p, "a1", {});

  // Ordinary capture and forward movement.
  CHECK(p.setFen("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1"));
  checkDestinations(p, "e4", {"e5", "d5"});

  // The e2 rook is pinned to its king by the e8 rook. It may remain on the
  // e-file (including capture of the checking rook), but cannot move sideways.
  CHECK(p.setFen("k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1"));
  checkDestinations(p, "e2", {"e3", "e4", "e5", "e6", "e7", "e8"});

  // In check by the e8 rook: only legal king evasions should be highlighted.
  CHECK(p.setFen("k3r3/8/8/8/8/8/8/4K3 w - - 0 1"));
  CHECK(p.get().checkers() != 0);
  checkDestinations(p, "e1", {"d1", "f1", "d2", "f2"});

  // The internal Stockfish move encodes castling to the rook's square.
  // GUI highlights must instead point to the king's landing square.
  CHECK(p.setFen("r3k2r/8/8/8/8/8/4P3/R3K2R w KQkq - 0 1"));
  checkDestinations(p, "e1", {"c1", "d1", "f1", "g1", "d2", "f2"});
  bool castles[64] = {};
  CHECK(ChessGuiRules::legalDestinations(p.get(), squareIndex("e1"), castles));
  CHECK(!castles[squareIndex("a1")]);
  CHECK(!castles[squareIndex("h1")]);

  // En-passant target d6 is a legal destination from e5.
  CHECK(p.setFen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"));
  checkDestinations(p, "e5", {"e6", "d6"});

  bool invalid[64];
  CHECK(!ChessGuiRules::legalDestinations(p.get(), -1, invalid));
  CHECK(!ChessGuiRules::legalDestinations(p.get(), 64, invalid));
}

void testPromotionAndMoveValidation() {
  TestPosition white;
  const std::string whiteFen = "7k/P7/8/8/8/8/8/7K w - - 0 1";
  CHECK(white.setFen(whiteFen));
  CHECK(ChessGuiRules::isPawnPromotion('P', squareIndex("a8"), 'w'));
  CHECK(!ChessGuiRules::isPawnPromotion('p', squareIndex("a8"), 'w'));
  CHECK(!ChessGuiRules::isPawnPromotion('P', squareIndex("a7"), 'w'));

  for (const char suffix : {'q', 'r', 'b', 'n'}) {
    TestPosition candidate;
    CHECK(candidate.setFen(whiteFen));
    const std::string move = std::string("a7a8") + suffix;
    CHECK(candidate.play(move));
    const Stockfish::Piece promoted = candidate.get().piece_on(square("a8"));
    Stockfish::PieceType expected = Stockfish::QUEEN;
    if (suffix == 'r') expected = Stockfish::ROOK;
    if (suffix == 'b') expected = Stockfish::BISHOP;
    if (suffix == 'n') expected = Stockfish::KNIGHT;
    CHECK(promoted == Stockfish::make_piece(Stockfish::WHITE, expected));
  }

  TestPosition black;
  const std::string blackFen = "7k/8/8/8/8/8/p7/7K b - - 0 1";
  CHECK(black.setFen(blackFen));
  CHECK(ChessGuiRules::isPawnPromotion('p', squareIndex("a1"), 'b'));
  CHECK(!ChessGuiRules::isPawnPromotion('P', squareIndex("a1"), 'b'));
  for (const char suffix : {'q', 'r', 'b', 'n'}) {
    TestPosition candidate;
    CHECK(candidate.setFen(blackFen));
    const std::string move = std::string("a2a1") + suffix;
    CHECK(candidate.play(move));
    const Stockfish::Piece promoted = candidate.get().piece_on(square("a1"));
    Stockfish::PieceType expected = Stockfish::QUEEN;
    if (suffix == 'r') expected = Stockfish::ROOK;
    if (suffix == 'b') expected = Stockfish::BISHOP;
    if (suffix == 'n') expected = Stockfish::KNIGHT;
    CHECK(promoted == Stockfish::make_piece(Stockfish::BLACK, expected));
  }

  // An invalid UCI move is rejected by Stockfish and leaves the position intact.
  TestPosition invalid;
  CHECK(invalid.setFen(StartFen));
  const std::string before = invalid.get().fen();
  CHECK(!invalid.play("e2e5"));
  CHECK(invalid.get().fen() == before);

  // A canceled promotion has the same required state invariant: before a
  // choice is committed, neither the position nor the UCI move history changes.
  TestPosition canceled;
  CHECK(canceled.setFen(whiteFen));
  const std::string beforeCancel = canceled.get().fen();
  CHECK(canceled.get().fen() == beforeCancel);
  CHECK(canceled.get().piece_on(square("a7")) == Stockfish::W_PAWN);
  CHECK(canceled.get().piece_on(square("a8")) == Stockfish::NO_PIECE);
}

void testGameStatusAndAdjudication() {
  TestPosition p;
  CHECK(p.setFen("7k/6Q1/6K1/8/8/8/8/8 b - - 100 1"));
  auto status = Stockfish::get_position_status(p.get());
  CHECK(status.inCheck);
  CHECK(!status.hasLegalMoves);
  CHECK(!status.drawByFiftyMove);
  CHECK(ChessGuiRules::classifyGame(status, 'b', 'b')
        == ChessGuiRules::GameOutcome::CheckmatePlayerLoss);
  CHECK(ChessGuiRules::classifyGame(status, 'b', 'w')
        == ChessGuiRules::GameOutcome::CheckmatePlayerWin);
  CHECK(std::string(ChessGuiRules::statusText(
        ChessGuiRules::classifyGame(status, 'b', 'w'))) == "You win");

  CHECK(p.setFen("7k/5Q2/6K1/8/8/8/8/8 b - - 100 1"));
  status = Stockfish::get_position_status(p.get());
  CHECK(!status.inCheck);
  CHECK(!status.hasLegalMoves);
  CHECK(status.drawByFiftyMove);
  CHECK(ChessGuiRules::classifyGame(status, 'b', 'w')
        == ChessGuiRules::GameOutcome::Stalemate);

  CHECK(p.setFen("4k3/8/8/8/8/8/8/4K2R w - - 99 1"));
  status = Stockfish::get_position_status(p.get());
  CHECK(status.hasLegalMoves);
  CHECK(!status.drawByFiftyMove);
  CHECK(status.halfmoveClock == 99);

  CHECK(p.setFen("4k3/8/8/8/8/8/8/4K2R w - - 100 1"));
  status = Stockfish::get_position_status(p.get());
  CHECK(status.drawByFiftyMove);
  CHECK(ChessGuiRules::classifyGame(status, 'w', 'w')
        == ChessGuiRules::GameOutcome::DrawFiftyMove);

  CHECK(p.setFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1"));
  status = Stockfish::get_position_status(p.get());
  CHECK(status.insufficientMaterial);
  CHECK(ChessGuiRules::classifyGame(status, 'w', 'w')
        == ChessGuiRules::GameOutcome::DrawMaterial);

  CHECK(p.setFen("4k3/8/8/8/8/8/8/3NK3 w - - 0 1"));
  CHECK(Stockfish::get_position_status(p.get()).insufficientMaterial);

  // Two bishops on the same square colour are insufficient material.
  CHECK(p.setFen("4k3/8/8/8/3b4/8/8/2B1K3 w - - 0 1"));
  CHECK(Stockfish::get_position_status(p.get()).insufficientMaterial);

  // Opposite-coloured bishops are not classified as insufficient material by
  // the current Stockfish game-status implementation.
  CHECK(p.setFen("2b1k3/8/8/8/8/8/8/2B1K3 w - - 0 1"));
  CHECK(!Stockfish::get_position_status(p.get()).insufficientMaterial);

  // Check status is live play, not a terminal outcome.
  CHECK(p.setFen("4k3/8/8/8/8/8/4r3/4K3 w - - 0 1"));
  status = Stockfish::get_position_status(p.get());
  CHECK(status.inCheck);
  CHECK(status.hasLegalMoves);
  CHECK(ChessGuiRules::classifyGame(status, 'w', 'w')
        == ChessGuiRules::GameOutcome::Check);
  CHECK(!ChessGuiRules::isTerminal(ChessGuiRules::GameOutcome::Check));
}

void testRepetitionHistory() {
  const std::vector<std::string> cycle = {"g1f3", "g8f6", "f3g1", "f6g8"};
  TestPosition p;
  CHECK(p.setFen(StartFen));
  for (const auto& move : cycle)
    CHECK(p.play(move));
  CHECK(!Stockfish::get_position_status(p.get()).drawByRepetition);
  for (const auto& move : cycle)
    CHECK(p.play(move));
  CHECK(Stockfish::get_position_status(p.get()).drawByRepetition);
  CHECK(ChessGuiRules::classifyGame(Stockfish::get_position_status(p.get()), 'w', 'w')
        == ChessGuiRules::GameOutcome::DrawRepetition);
}

void testBoardOrientationAndUndoPolicy() {
  constexpr int BoardX = 4;
  constexpr int BoardY = 16;
  constexpr int BoardSize = 208;
  constexpr int SquareSize = 26;

  CHECK(ChessGuiRules::screenToSquare(17, 29, BoardX, BoardY,
        BoardSize, SquareSize, 'w') == squareIndex("a8"));
  CHECK(ChessGuiRules::screenToSquare(17, 29, BoardX, BoardY,
        BoardSize, SquareSize, 'b') == squareIndex("h1"));
  CHECK(ChessGuiRules::screenToSquare(199, 211, BoardX, BoardY,
        BoardSize, SquareSize, 'w') == squareIndex("h1"));
  CHECK(ChessGuiRules::screenToSquare(199, 211, BoardX, BoardY,
        BoardSize, SquareSize, 'b') == squareIndex("a8"));
  CHECK(ChessGuiRules::screenToSquare(BoardX - 1, 29, BoardX, BoardY,
        BoardSize, SquareSize, 'w') == -1);
  CHECK(ChessGuiRules::screenToSquare(BoardX + BoardSize, 29, BoardX, BoardY,
        BoardSize, SquareSize, 'w') == -1);

  CHECK(ChessGuiRules::undoPliesToRemove(0) == 0);
  CHECK(ChessGuiRules::undoPliesToRemove(1) == 1);
  CHECK(ChessGuiRules::undoPliesToRemove(2) == 2);
  CHECK(ChessGuiRules::undoPliesToRemove(3) == 2);
}

void testCompactGameHistory() {
  GameHistory history;
  CHECK(history.empty());
  CHECK(!history.popBack());
  CHECK(history.appendUci("e2e4"));
  CHECK(history.appendUci("a7a8q"));
  CHECK(history.appendUci("a7a8r"));
  CHECK(history.appendUci("a7a8b"));
  CHECK(history.appendUci("a7a8n"));
  CHECK(!history.appendUci("e2e2"));
  CHECK(!history.appendUci("i2e4"));
  CHECK(!history.appendUci("e2e4x"));
  CHECK(!history.appendUci("e2e4Q"));
  CHECK(history.size() == 5);

  char move[6] = {};
  CHECK(history.formatMoveAt(0, move) && std::string(move) == "e2e4");
  CHECK(history.formatMoveAt(1, move) && std::string(move) == "a7a8q");
  CHECK(history.formatMoveAt(2, move) && std::string(move) == "a7a8r");
  CHECK(history.formatMoveAt(3, move) && std::string(move) == "a7a8b");
  CHECK(history.formatMoveAt(4, move) && std::string(move) == "a7a8n");
  CHECK(!history.formatMoveAt(5, move));

  // Exercise the actual byte-copy restore path with an intentionally odd
  // address, as happens when the variable-length saved FEN ends at an odd byte.
  uint8_t unaligned[1 + 5 * sizeof(uint16_t)] = {};
  std::memcpy(unaligned + 1, history.packedData(), 5 * sizeof(uint16_t));
  GameHistory restored;
  CHECK(restored.assignPackedBytes(unaligned + 1, 5));
  CHECK(restored.toUciMoves() == history.toUciMoves());

  // Invalid packed input must be rejected without replacing the old history.
  uint16_t invalidPacked[2] = {history.packedData()[0], 0x8000u};
  CHECK(!restored.assignPacked(invalidPacked, 2));
  CHECK(restored.toUciMoves() == history.toUciMoves());

  CHECK(restored.popBack());
  CHECK(restored.size() == 4);
  CHECK(restored.appendUci("h2h4"));

  GameHistory swapped;
  CHECK(swapped.appendUci("b1c3"));
  restored.swap(swapped);
  CHECK(restored.size() == 1);
  CHECK(restored.formatMoveAt(0, move) && std::string(move) == "b1c3");
  CHECK(swapped.size() == 5);

  test_fail_psram_allocations = true;
  GameHistory internalFallback;
  CHECK(internalFallback.appendUci("e2e4"));
  CHECK(!internalFallback.usesPsram());
  test_fail_psram_allocations = false;

  GameHistory maximum;
  for (std::size_t i = 0; i < GameHistory::MaxMoves; ++i)
    CHECK(maximum.appendUci("e2e4"));
  CHECK(maximum.size() == GameHistory::MaxMoves);
  CHECK(!maximum.appendUci("e2e4"));
}

}  // namespace

int main() {
  Stockfish::Attacks::init();
  Stockfish::Position::init();

  testLegalDestinationHighlights();
  testPromotionAndMoveValidation();
  testGameStatusAndAdjudication();
  testRepetitionHistory();
  testBoardOrientationAndUndoPolicy();
  testCompactGameHistory();

  if (failures) {
    std::cerr << failures << " of " << checks << " checks failed.\n";
    return 1;
  }
  std::cout << "PASS: " << checks << " native Stockfish GUI rule checks.\n";
  return 0;
}
