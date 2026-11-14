#ifndef POSITION_STATUS_H_INCLUDED
#define POSITION_STATUS_H_INCLUDED

namespace Stockfish {

class Position;

// Shared, position-only game status. Keeping this independent of Engine lets
// the exact GUI adjudication inputs be regression-tested without constructing
// an NNUE network or starting a search worker.
struct PositionStatus {
    bool inCheck = false;
    bool hasLegalMoves = false;
    bool drawByFiftyMove = false;
    bool drawByRepetition = false;
    bool insufficientMaterial = false;
    int halfmoveClock = 0;
};

PositionStatus get_position_status(const Position& pos);

}  // namespace Stockfish

#endif  // POSITION_STATUS_H_INCLUDED
