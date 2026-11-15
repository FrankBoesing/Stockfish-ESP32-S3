#pragma once

#include <cstddef>
#include <cstdint>

#include "stockfish/movegen.h"
#include "stockfish/position.h"
#include "polyglot_random.h"

namespace PolyglotBook {

constexpr std::size_t EntrySize = 16;

inline uint16_t readBigEndian16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

inline uint64_t readBigEndian64(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value = (value << 8) | p[i];
    return value;
}

// Polyglot piece order is P-black, P-white, N-black, N-white, ... K-black,
// K-white. Squares use the conventional a1=0 ... h8=63 mapping.
inline uint64_t hashPosition(const Stockfish::Position& position) {
    using namespace Stockfish;

    uint64_t key = 0;
    const auto& board = position.piece_array();

    for (int square = 0; square < 64; ++square) {
        const Piece piece = board[square];
        if (piece == NO_PIECE)
            continue;

        const int colorPivot = color_of(piece) == WHITE ? 1 : 0;
        const int pieceIndex = (int(type_of(piece)) - 1) * 2 + colorPivot;
        key ^= Random[pieceIndex * 64 + square];
    }

    if (position.can_castle(WHITE_OO))
        key ^= Random[768];
    if (position.can_castle(WHITE_OOO))
        key ^= Random[769];
    if (position.can_castle(BLACK_OO))
        key ^= Random[770];
    if (position.can_castle(BLACK_OOO))
        key ^= Random[771];

    // Polyglot includes an en-passant file only if a side-to-move pawn can
    // pseudo-legally capture onto that square. King-safety legality is ignored.
    const Square ep = position.polyglot_ep_square();
    if (is_ok(ep)) {
        const int epFile = int(file_of(ep));
        const int sourceRank = int(rank_of(ep))
                            + (position.side_to_move() == WHITE ? -1 : 1);
        if (sourceRank >= 0 && sourceRank < 8) {
            for (int sourceFile = epFile - 1; sourceFile <= epFile + 1; sourceFile += 2) {
                if (sourceFile < 0 || sourceFile >= 8)
                    continue;
                const Square source = make_square(File(sourceFile), Rank(sourceRank));
                if (position.piece_on(source)
                    == make_piece(position.side_to_move(), PAWN)) {
                    key ^= Random[772 + epFile];
                    break;
                }
            }
        }
    }

    if (position.side_to_move() == WHITE)
        key ^= Random[780];

    return key;
}

// Decode and validate a Polyglot move against Stockfish's legal move list.
// This also handles en-passant and castling using Stockfish's own move
// representation (Stockfish internally encodes standard castling king-to-rook).
inline bool decodeLegalMove(const Stockfish::Position& position,
                            uint16_t polyglotMove,
                            Stockfish::Move& result) {
    using namespace Stockfish;

    // Bit 15 is reserved. Promotion codes are 0=None, 1=N, 2=B, 3=R, 4=Q.
    if (polyglotMove & 0x8000u)
        return false;

    const int toFile = polyglotMove & 0x7;
    const int toRank = (polyglotMove >> 3) & 0x7;
    const int fromFile = (polyglotMove >> 6) & 0x7;
    const int fromRank = (polyglotMove >> 9) & 0x7;
    const int promotionCode = (polyglotMove >> 12) & 0x7;

    if (promotionCode > 4)
        return false;

    const Square from = make_square(File(fromFile), Rank(fromRank));
    const Square to = make_square(File(toFile), Rank(toRank));

    PieceType promotionType = NO_PIECE_TYPE;
    switch (promotionCode) {
        case 0: break;
        case 1: promotionType = KNIGHT; break;
        case 2: promotionType = BISHOP; break;
        case 3: promotionType = ROOK; break;
        case 4: promotionType = QUEEN; break;
    }

    for (const Move candidate : MoveList<LEGAL>(position)) {
        if (candidate.from_sq() != from)
            continue;

        Square candidateTo = candidate.to_sq();
        if (candidate.type_of() == CASTLING) {
            // Polyglot stores the king's destination square (c/g-file).
            // Stockfish's orthodox castling move points at the rook's start.
            const bool kingSide = file_of(candidate.to_sq()) > file_of(from);
            candidateTo = make_square(kingSide ? FILE_G : FILE_C, rank_of(from));
        }

        if (candidateTo != to)
            continue;

        if (promotionCode == 0) {
            if (candidate.type_of() == PROMOTION)
                continue;
        } else if (candidate.type_of() != PROMOTION
                   || candidate.promotion_type() != promotionType) {
            continue;
        }

        result = candidate;
        return true;
    }

    return false;
}

struct SelectionStats {
    uint32_t legalEntries = 0;
    uint64_t totalWeight = 0;
};

// Entries must be sorted by their unsigned 64-bit big-endian key, as required
// by Polyglot. The routine does a lower_bound search, then scans only the
// contiguous entries for this position. It performs no allocation.
inline bool chooseMove(const Stockfish::Position& position,
                       const uint8_t* entries,
                       std::size_t entryCount,
                       uint64_t randomValue,
                       Stockfish::Move& selected,
                       SelectionStats* stats = nullptr) {
    if (stats)
        *stats = SelectionStats{};

    if (!entries || !entryCount || position.is_chess960())
        return false;

    const uint64_t key = hashPosition(position);

    std::size_t first = 0;
    std::size_t last = entryCount;
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        const uint64_t entryKey = readBigEndian64(entries + middle * EntrySize);
        if (entryKey < key)
            first = middle + 1;
        else
            last = middle;
    }

    const std::size_t rangeBegin = first;
    uint64_t totalWeight = 0;
    uint32_t legalEntries = 0;

    for (std::size_t i = rangeBegin; i < entryCount; ++i) {
        const uint8_t* entry = entries + i * EntrySize;
        if (readBigEndian64(entry) != key)
            break;

        const uint16_t weight = readBigEndian16(entry + 10);
        if (!weight)
            continue;

        Stockfish::Move candidate;
        if (!decodeLegalMove(position, readBigEndian16(entry + 8), candidate))
            continue;

        totalWeight += weight;
        ++legalEntries;
    }

    if (stats) {
        stats->legalEntries = legalEntries;
        stats->totalWeight = totalWeight;
    }
    if (!totalWeight)
        return false;

    uint64_t choice = randomValue % totalWeight;
    for (std::size_t i = rangeBegin; i < entryCount; ++i) {
        const uint8_t* entry = entries + i * EntrySize;
        if (readBigEndian64(entry) != key)
            break;

        const uint16_t weight = readBigEndian16(entry + 10);
        if (!weight)
            continue;

        Stockfish::Move candidate;
        if (!decodeLegalMove(position, readBigEndian16(entry + 8), candidate))
            continue;

        if (choice < weight) {
            selected = candidate;
            return true;
        }
        choice -= weight;
    }

    // The second pass must select one of the entries counted in the first.
    return false;
}

}  // namespace PolyglotBook
