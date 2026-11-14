#include "position_status.h"

#include "bitboard.h"
#include "movegen.h"
#include "position.h"

namespace Stockfish {

PositionStatus get_position_status(const Position& pos) {
    PositionStatus status;
    status.inCheck = pos.checkers() != 0;
    status.hasLegalMoves = MoveList<LEGAL>(pos).size() != 0;
    status.halfmoveClock = pos.rule50_count();
    status.drawByFiftyMove = status.halfmoveClock >= 100
                          && (!status.inCheck || status.hasLegalMoves);
    // At the root, Stockfish marks the third occurrence with a negative distance.
    status.drawByRepetition = pos.is_repetition(0);

    if (!(pos.pieces(PAWN) | pos.pieces(ROOK) | pos.pieces(QUEEN))) {
        const Bitboard minors = pos.pieces(KNIGHT) | pos.pieces(BISHOP);
        if (popcount(minors) <= 1)
            status.insufficientMaterial = true;
        else if (!pos.pieces(KNIGHT)) {
            constexpr Bitboard DarkSquares = 0xAA55AA55AA55AA55ULL;
            const Bitboard bishops = pos.pieces(BISHOP);
            status.insufficientMaterial = !(bishops & DarkSquares)
                                       || !(bishops & ~DarkSquares);
        }
    }
    return status;
}

}  // namespace Stockfish
