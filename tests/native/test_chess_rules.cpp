#include "gui/chess_rules.h"
#include "gui/game_history.h"
#include "gui/polyglot_book.h"
#include "gui/flash_polyglot_book.h"

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
std::vector<uint8_t> fakeFlashPartitionBytes;
const esp_partition_t fakeBookPartition{0xBE0000};
}

extern "C" {
const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label) {
  if (type != ESP_PARTITION_TYPE_DATA || subtype != 0x06
      || !label || std::strcmp(label, "book") != 0)
    return nullptr;
  return &fakeBookPartition;
}

esp_err_t esp_partition_read(const esp_partition_t *, std::size_t offset,
                             void *dst, std::size_t size) {
  if (!dst || offset > fakeFlashPartitionBytes.size()
      || size > fakeFlashPartitionBytes.size() - offset)
    return -1;
  std::memcpy(dst, fakeFlashPartitionBytes.data() + offset, size);
  return ESP_OK;
}

esp_err_t esp_partition_mmap(const esp_partition_t *, std::size_t offset,
                             std::size_t size, esp_partition_mmap_flag_t,
                             const void **outPtr,
                             esp_partition_mmap_handle_t *outHandle) {
  if (!outPtr || !outHandle || offset > fakeFlashPartitionBytes.size()
      || size > fakeFlashPartitionBytes.size() - offset)
    return -1;
  *outPtr = fakeFlashPartitionBytes.data() + offset;
  *outHandle = 1;
  return ESP_OK;
}

void esp_partition_munmap(esp_partition_mmap_handle_t) {}
}

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
    moves.clear();
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
    moves.push_back(move);
    return true;
  }

  bool undoLast() {
    if (moves.empty())
      return false;
    position.undo_move(moves.back());
    moves.pop_back();
    states.pop_back();
    return true;
  }

  std::size_t historySize() const { return moves.size(); }

  Stockfish::Position& get() { return position; }
  const Stockfish::Position& get() const { return position; }

private:
  // deque keeps StateInfo addresses stable for Position's repetition chain.
  std::deque<Stockfish::StateInfo> states;
  std::vector<Stockfish::Move> moves;
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

  // Mirror the castling target mapping for Black, on rank 8.
  CHECK(p.setFen("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1"));
  checkDestinations(p, "e8", {"c8", "g8", "d7", "e7", "f7", "d8", "f8"});
  bool blackCastles[64] = {};
  CHECK(ChessGuiRules::legalDestinations(p.get(), squareIndex("e8"), blackCastles));
  CHECK(!blackCastles[squareIndex("a8")]);
  CHECK(!blackCastles[squareIndex("h8")]);

  // En-passant target d6 is a legal destination from e5.
  CHECK(p.setFen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"));
  checkDestinations(p, "e5", {"e6", "d6"});

  // Black en passant uses the mirrored rank and side-to-move.
  CHECK(p.setFen("4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1"));
  checkDestinations(p, "e4", {"e3", "d3"});

  // En passant that would uncover a rook check against the moving king is
  // illegal and must not be shown as a destination.
  CHECK(p.setFen("k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"));
  checkDestinations(p, "e5", {"e6"});

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

void testPromotionRequestCancel() {
  TestPosition position;
  const std::string fen = "7k/P7/8/8/8/8/8/7K w - - 0 1";
  CHECK(position.setFen(fen));

  GameHistory history;
  CHECK(history.appendUci("a2a3"));
  const auto originalMoves = history.toUciMoves();
  const std::string originalFen = position.get().fen();

  ChessGuiRules::PromotionRequest request;
  CHECK(!request.pending());
  CHECK(!request.begin(-1, squareIndex("a8")));
  CHECK(request.begin(squareIndex("a7"), squareIndex("a8")));
  CHECK(request.pending());

  int selectedSquare = -1;
  CHECK(request.cancel(selectedSquare));
  CHECK(selectedSquare == squareIndex("a7"));
  CHECK(!request.pending());
  CHECK(position.get().fen() == originalFen);
  CHECK(history.toUciMoves() == originalMoves);
  CHECK(!request.cancel(selectedSquare));

  // A chosen option consumes the pending request exactly once.
  CHECK(request.begin(squareIndex("a7"), squareIndex("a8")));
  int from = -1, to = -1;
  CHECK(request.take(from, to));
  CHECK(from == squareIndex("a7"));
  CHECK(to == squareIndex("a8"));
  CHECK(!request.pending());
  CHECK(!request.take(from, to));
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

void testUndoAndNewAfterCheckmate() {
  TestPosition game;
  CHECK(game.setFen(StartFen));

  // Fool's Mate gives Black checkmate after four plies.
  CHECK(game.play("f2f3"));
  CHECK(game.play("e7e5"));
  CHECK(game.play("g2g4"));
  CHECK(game.play("d8h4"));
  CHECK(game.historySize() == 4);

  const auto terminal = ChessGuiRules::classifyGame(
    Stockfish::get_position_status(game.get()), 'w', 'w');
  CHECK(terminal == ChessGuiRules::GameOutcome::CheckmatePlayerLoss);
  CHECK(!ChessGuiRules::acceptsBoardMoves(ChessGuiRules::isTerminal(terminal)));
  CHECK(!ChessGuiRules::hintEnabled(false, true, 'w', 'w'));
  CHECK(ChessGuiRules::undoEnabled(game.historySize()));

  // GUI takeback removes the final player/engine pair from a completed game.
  const std::size_t removeCount = ChessGuiRules::undoPliesToRemove(game.historySize());
  CHECK(removeCount == 2);
  for (std::size_t i = 0; i < removeCount; ++i)
    CHECK(game.undoLast());
  CHECK(game.historySize() == 2);
  auto restored = Stockfish::get_position_status(game.get());
  CHECK(restored.hasLegalMoves);
  CHECK(!ChessGuiRules::isTerminal(
    ChessGuiRules::classifyGame(restored, 'w', 'w')));

  // New Game restores the standard start position and clears move history.
  CHECK(game.setFen(StartFen));
  CHECK(game.historySize() == 0);
  CHECK(game.get().fen() == StartFen);
  const auto fresh = Stockfish::get_position_status(game.get());
  const auto freshOutcome = ChessGuiRules::classifyGame(fresh, 'w', 'w');
  CHECK(ChessGuiRules::acceptsBoardMoves(
    ChessGuiRules::isTerminal(freshOutcome)));
  CHECK(!ChessGuiRules::undoEnabled(game.historySize()));
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


uint16_t polyglotMove(int from, int to, unsigned promotion = 0) {
  return uint16_t((from & 7) << 6)
       | uint16_t((from >> 3) << 9)
       | uint16_t(to & 7)
       | uint16_t((to >> 3) << 3)
       | uint16_t(promotion << 12);
}

void storeBe16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}

void storeBe64(uint8_t* p, uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    p[i] = uint8_t(value);
    value >>= 8;
  }
}

void testPolyglotHashing() {
  TestPosition p;
  CHECK(p.setFen(StartFen));
  CHECK(PolyglotBook::hashPosition(p.get()) == 0x463B96181691FC9Cull);

  CHECK(p.play("e2e4"));
  CHECK(PolyglotBook::hashPosition(p.get()) == 0x823C9B50FD114196ull);

  // Castling rights and the side to move are part of the Polyglot key.
  CHECK(p.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1"));
  const uint64_t noRights = PolyglotBook::hashPosition(p.get());
  CHECK(p.setFen(StartFen));
  CHECK(PolyglotBook::hashPosition(p.get()) != noRights);

  CHECK(p.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1"));
  CHECK(PolyglotBook::hashPosition(p.get()) != 0x463B96181691FC9Cull);

  // Polyglot hashes a capturable en-passant file, even if the capture would
  // expose the king. It ignores an EP target when no side-to-move pawn can
  // pseudo-legally capture it.
  CHECK(p.setFen("4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 1"));
  const uint64_t pinnedEp = PolyglotBook::hashPosition(p.get());
  CHECK(p.setFen("4r1k1/8/8/3pP3/8/8/8/4K3 w - - 0 1"));
  CHECK(pinnedEp != PolyglotBook::hashPosition(p.get()));

  CHECK(p.setFen("4k3/8/8/3p4/8/8/8/4K3 w - d6 0 1"));
  const uint64_t uncapturableEp = PolyglotBook::hashPosition(p.get());
  CHECK(p.setFen("4k3/8/8/3p4/8/8/8/4K3 w - - 0 1"));
  CHECK(uncapturableEp == PolyglotBook::hashPosition(p.get()));
}

void testPolyglotPseudoLegalEpAfterDoublePush() {
  TestPosition pinned;
  TestPosition withoutEp;

  // After g2-g4, f4xg3 e.p. is pseudo-legal, but illegal because it removes
  // both rank-4 blockers and exposes the black king h4 to the white rook a4.
  // Polyglot still hashes the g-file; Stockfish's ordinary ep_square is none.
  CHECK(pinned.setFen("8/8/8/8/R4p1k/8/6P1/4K3 w - - 0 1"));
  CHECK(pinned.play("g2g4"));
  CHECK(pinned.get().polyglot_ep_square() == square("g3"));
  CHECK(pinned.get().ep_square() == Stockfish::SQ_NONE);

  CHECK(withoutEp.setFen("8/8/8/8/R4pPk/8/8/4K3 b - - 0 1"));
  CHECK(PolyglotBook::hashPosition(pinned.get())
        != PolyglotBook::hashPosition(withoutEp.get()));
}

void testPolyglotMoveDecoding() {
  TestPosition p;
  Stockfish::Move decoded;

  CHECK(p.setFen(StartFen));
  CHECK(PolyglotBook::decodeLegalMove(
    p.get(), polyglotMove(squareIndex("e2"), squareIndex("e4")), decoded));
  CHECK(Stockfish::UCIEngine::move(decoded, false) == "e2e4");
  CHECK(!PolyglotBook::decodeLegalMove(
    p.get(), polyglotMove(squareIndex("e2"), squareIndex("e5")), decoded));

  // Standard Polyglot castling uses king-to-c/g; Stockfish internally stores
  // the rook square as the destination for orthodox castling.
  CHECK(p.setFen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"));
  CHECK(PolyglotBook::decodeLegalMove(
    p.get(), polyglotMove(squareIndex("e1"), squareIndex("g1")), decoded));
  CHECK(Stockfish::UCIEngine::move(decoded, false) == "e1g1");
  CHECK(PolyglotBook::decodeLegalMove(
    p.get(), polyglotMove(squareIndex("e1"), squareIndex("c1")), decoded));
  CHECK(Stockfish::UCIEngine::move(decoded, false) == "e1c1");

  // En-passant is matched against Stockfish's fully legal move list.
  CHECK(p.setFen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"));
  CHECK(PolyglotBook::decodeLegalMove(
    p.get(), polyglotMove(squareIndex("e5"), squareIndex("d6")), decoded));
  CHECK(Stockfish::UCIEngine::move(decoded, false) == "e5d6");

  CHECK(p.setFen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1"));
  CHECK(PolyglotBook::decodeLegalMove(
    p.get(), polyglotMove(squareIndex("a7"), squareIndex("a8"), 4), decoded));
  CHECK(Stockfish::UCIEngine::move(decoded, false) == "a7a8q");
  CHECK(PolyglotBook::decodeLegalMove(
    p.get(), polyglotMove(squareIndex("a7"), squareIndex("a8"), 1), decoded));
  CHECK(Stockfish::UCIEngine::move(decoded, false) == "a7a8n");
  CHECK(!PolyglotBook::decodeLegalMove(
    p.get(), polyglotMove(squareIndex("a7"), squareIndex("a8"), 5), decoded));
}

void testPolyglotBinarySearchAndWeightedSelection() {
  TestPosition p;
  CHECK(p.setFen(StartFen));
  const uint64_t key = PolyglotBook::hashPosition(p.get());

  // Three records for the start position: an illegal move with a large weight,
  // e2e4 with weight 1, and d2d4 with weight 3. A later unrelated key checks
  // that lookup stops at the end of the matching contiguous range.
  std::vector<uint8_t> data(4 * PolyglotBook::EntrySize, 0);
  for (int i = 0; i < 3; ++i)
    storeBe64(data.data() + i * 16, key);
  storeBe16(data.data() + 8, polyglotMove(squareIndex("e2"), squareIndex("e5")));
  storeBe16(data.data() + 10, 100);
  storeBe16(data.data() + 16 + 8, polyglotMove(squareIndex("e2"), squareIndex("e4")));
  storeBe16(data.data() + 16 + 10, 1);
  storeBe16(data.data() + 32 + 8, polyglotMove(squareIndex("d2"), squareIndex("d4")));
  storeBe16(data.data() + 32 + 10, 3);
  storeBe64(data.data() + 48, key + 1);

  Stockfish::Move selected;
  PolyglotBook::SelectionStats stats;
  CHECK(PolyglotBook::chooseMove(p.get(), data.data(), 4, 0, selected, &stats));
  CHECK(Stockfish::UCIEngine::move(selected, false) == "e2e4");
  CHECK(stats.legalEntries == 2);
  CHECK(stats.totalWeight == 4);

  CHECK(PolyglotBook::chooseMove(p.get(), data.data(), 4, 1, selected, &stats));
  CHECK(Stockfish::UCIEngine::move(selected, false) == "d2d4");

  CHECK(PolyglotBook::chooseMove(p.get(), data.data(), 4, 3, selected, &stats));
  CHECK(Stockfish::UCIEngine::move(selected, false) == "d2d4");

  // A key outside the book range must produce a clean miss.
  CHECK(p.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1"));
  CHECK(!PolyglotBook::chooseMove(p.get(), data.data(), 4, 0, selected, &stats));
  CHECK(stats.legalEntries == 0);
  CHECK(stats.totalWeight == 0);

  // Zero-weight entries are intentionally ignored.
  CHECK(p.setFen(StartFen));
  storeBe16(data.data() + 16 + 10, 0);
  storeBe16(data.data() + 32 + 10, 0);
  CHECK(!PolyglotBook::chooseMove(p.get(), data.data(), 4, 0, selected, &stats));
}

void storeLe16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value);
  p[1] = uint8_t(value >> 8);
}

void storeLe32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value);
  p[1] = uint8_t(value >> 8);
  p[2] = uint8_t(value >> 16);
  p[3] = uint8_t(value >> 24);
}

std::vector<uint8_t> makeSfbookImage(const std::vector<uint8_t>& entries) {
  const std::size_t headerSize = PolyglotBook::ImageHeaderSize;
  std::vector<uint8_t> image(headerSize + entries.size(), 0);
  std::memcpy(image.data(), "SFBK", 4);
  storeLe16(image.data() + 4, PolyglotBook::ImageVersion);
  storeLe16(image.data() + 6, uint16_t(headerSize));
  storeLe32(image.data() + 8, uint32_t(entries.size() / PolyglotBook::EntrySize));
  storeLe32(image.data() + 12, uint32_t(entries.size()));
  storeLe32(image.data() + 16,
            PolyglotBook::crc32(entries.data(), entries.size()));
  storeLe32(image.data() + 20, 0);
  if (!entries.empty())
    std::memcpy(image.data() + headerSize, entries.data(), entries.size());
  return image;
}

void testFlashPolyglotBookImage() {
  TestPosition position;
  CHECK(position.setFen(StartFen));
  const uint64_t key = PolyglotBook::hashPosition(position.get());

  std::vector<uint8_t> entries(2 * PolyglotBook::EntrySize, 0);
  storeBe64(entries.data(), key);
  storeBe16(entries.data() + 8,
            polyglotMove(squareIndex("e2"), squareIndex("e4")));
  storeBe16(entries.data() + 10, 5);
  storeBe64(entries.data() + 16, key);
  storeBe16(entries.data() + 24,
            polyglotMove(squareIndex("d2"), squareIndex("d4")));
  storeBe16(entries.data() + 26, 3);

  fakeFlashPartitionBytes = makeSfbookImage(entries);
  {
    PolyglotBook::FlashPolyglotBook book;
    CHECK(book.begin());
    CHECK(book.ready());
    CHECK(book.entryCount() == 2);
    CHECK(book.dataBytes() == entries.size());

    Stockfish::Move selected;
    PolyglotBook::SelectionStats stats;
    CHECK(book.chooseMove(position.get(), 0, selected, &stats));
    CHECK(Stockfish::UCIEngine::move(selected, false) == "e2e4");
    CHECK(stats.legalEntries == 2);
    CHECK(stats.totalWeight == 8);
    CHECK(book.chooseMove(position.get(), 5, selected, &stats));
    CHECK(Stockfish::UCIEngine::move(selected, false) == "d2d4");
  }

  // Corrupt payload must be rejected by the reader's own CRC implementation.
  fakeFlashPartitionBytes = makeSfbookImage(entries);
  fakeFlashPartitionBytes[PolyglotBook::ImageHeaderSize + 10] ^= 0x01;
  {
    PolyglotBook::FlashPolyglotBook book;
    CHECK(!book.begin());
    CHECK(std::strcmp(book.error(), "book CRC32 mismatch") == 0);
  }

  // Sorted-key validation is performed on the actual mapped payload too.
  std::vector<uint8_t> unsorted(2 * PolyglotBook::EntrySize, 0);
  storeBe64(unsorted.data(), key + 1);
  storeBe64(unsorted.data() + PolyglotBook::EntrySize, key);
  fakeFlashPartitionBytes = makeSfbookImage(unsorted);
  {
    PolyglotBook::FlashPolyglotBook book;
    CHECK(!book.begin());
    CHECK(std::strcmp(book.error(), "Polyglot entries are not sorted by key") == 0);
  }

  // A raw Polyglot .bin has no SFBK metadata and must not be treated as ready.
  fakeFlashPartitionBytes.assign(PolyglotBook::ImageHeaderSize, 0xFF);
  {
    PolyglotBook::FlashPolyglotBook book;
    CHECK(!book.begin());
    CHECK(std::strstr(book.error(), "book image header missing") != nullptr);
  }

  CHECK(PolyglotBook::crc32(
          reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);
}

}  // namespace

int main() {
  Stockfish::Attacks::init();
  Stockfish::Position::init();

  testLegalDestinationHighlights();
  testPromotionAndMoveValidation();
  testPromotionRequestCancel();
  testGameStatusAndAdjudication();
  testUndoAndNewAfterCheckmate();
  testRepetitionHistory();
  testBoardOrientationAndUndoPolicy();
  testCompactGameHistory();
  testPolyglotHashing();
  testPolyglotPseudoLegalEpAfterDoublePush();
  testPolyglotMoveDecoding();
  testPolyglotBinarySearchAndWeightedSelection();
  testFlashPolyglotBookImage();

  if (failures) {
    std::cerr << failures << " of " << checks << " checks failed.\n";
    return 1;
  }
  std::cout << "PASS: " << checks << " native Stockfish GUI rule checks.\n";
  return 0;
}
