/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "attacks.h"
#include "esp_fatal.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
    #include <esp_heap_caps.h>
#endif

#include "misc.h"

namespace Stockfish::Attacks {

Bitboard LineBB[SQUARE_NB][SQUARE_NB];
Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
Bitboard RayPassBB[SQUARE_NB][SQUARE_NB];

namespace {

#ifndef USE_DUAL_HYPERBOLA_QUINT
alignas(64) Magic Magics[SQUARE_NB][2];
#endif

}

[[maybe_unused]] static constexpr Bitboard line_mask(Square sq, Direction d1, Direction d2) {
    Bitboard mask = 0;
    for (Direction d : {d1, d2})
    {
        Square s = sq;
        while (Bitboard dest = safe_destination(s, d))
        {
            mask |= dest;
            s += d;
        }
    }
    return mask;
}

#ifdef USE_HYPERBOLA_QUINT
static void init_magics(Magic magics[][2]) {
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        Magic& rook = magics[s][ROOK - BISHOP];
        rook.mask1  = line_mask(s, NORTH, SOUTH);
        rook.mask2  = line_mask(s, EAST, WEST);

        Magic& bishop = magics[s][BISHOP - BISHOP];
        bishop.mask1  = line_mask(s, NORTH_EAST, SOUTH_WEST);
        bishop.mask2  = line_mask(s, NORTH_WEST, SOUTH_EAST);
    }
}

#elif defined(USE_DUAL_HYPERBOLA_QUINT)

// Sliding attacks within a rank, indexed by the slider's file and the
// 6 inner bits of the rank occupancy (edge squares never affect the
// attack set), yielding the 8-bit attack set on that rank
alignas(64) constexpr auto RankAttacks = []() {
    std::array<std::array<u8, 64>, FILE_NB> table{};
    for (int file = 0; file < 8; ++file)
        for (int occ6 = 0; occ6 < 64; ++occ6)
            table[file][occ6] = u8(sliding_attack(ROOK, Square(file), occ6 << 1));
    return table;
}();

static constexpr auto make_dual_magics() {
    std::array<DualMagic, SQUARE_NB> magics{};
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        DualMagic& m        = magics[s];
        m.maskFile          = line_mask(s, NORTH, SOUTH);
        m.maskDiag          = line_mask(s, NORTH_EAST, SOUTH_WEST);
        m.maskNone          = 0;
        m.maskAntidiag      = line_mask(s, NORTH_WEST, SOUTH_EAST);
        m.r                 = square_bb(s) * 2;
        m.rr                = square_bb(Square(63 - int(s))) * 2;
        m.rankAttacksLookup = RankAttacks[int(file_of(s))].data();
        m.shift             = 8 * int(rank_of(s));
    }
    return magics;
}

alignas(64) extern constexpr std::array<DualMagic, SQUARE_NB> DualMagics = make_dual_magics();

#else

namespace {
void init_magics(PieceType pt, Bitboard table[], Magic magics[][2]) {

    int seeds[][RANK_NB] = {{8977, 44560, 54343, 38998, 5731, 95205, 104912, 17020},
                            {728, 10316, 55013, 32803, 12281, 15100, 16645, 255}};

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
    // These three scratch arrays total 80 KiB. Keeping them as automatic
    // variables overflows the small Arduino loopTask stack on ESP32-S3.
    // Allocate one temporary block in PSRAM and release it after table setup.
    constexpr size_t EntryCount = 4096;
    constexpr size_t ScratchSize =
      2 * EntryCount * sizeof(Bitboard) + EntryCount * sizeof(int);
    constexpr uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    void* scratchAllocation = heap_caps_malloc(ScratchSize + alignof(Bitboard) - 1, psramCaps);
    if (!scratchAllocation)
        SF_FATAL_ABORT("Failed to allocate PSRAM scratch memory for magic attack initialization");

    const uintptr_t alignedAddress =
      (reinterpret_cast<uintptr_t>(scratchAllocation) + alignof(Bitboard) - 1)
      & ~(uintptr_t(alignof(Bitboard) - 1));
    Bitboard* occupancy = reinterpret_cast<Bitboard*>(alignedAddress);
    int*      epoch     = reinterpret_cast<int*>(occupancy + EntryCount);
    Bitboard* reference = reinterpret_cast<Bitboard*>(epoch + EntryCount);
    std::memset(epoch, 0, EntryCount * sizeof(int));
    std::memset(reference, 0, EntryCount * sizeof(Bitboard));
#else
    Bitboard occupancyStorage[4096];
    int      epochStorage[4096] = {};
    Bitboard referenceStorage[4096] = {};
    Bitboard* occupancy = occupancyStorage;
    int*      epoch = epochStorage;
    Bitboard* reference = referenceStorage;
#endif
    int cnt = 0;
    int size = 0;

    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        Bitboard edges = ((Rank1BB | Rank8BB) & ~rank_bb(s)) | ((FileABB | FileHBB) & ~file_bb(s));

        Magic&   m       = magics[s][pt - BISHOP];
        Bitboard attacks = sliding_attack(pt, s, 0);
        m.mask           = attacks & ~edges;
        m.shift          = (Is64Bit ? 64 : 32) - popcount(m.mask);
        m.attacks        = s == SQ_A1 ? table : magics[s - 1][pt - BISHOP].attacks + size;
        size             = 0;

        Bitboard b = 0;
        do
        {
            occupancy[size] = b;
            reference[size] = sliding_attack(pt, s, b);

            size++;
            b = (b - m.mask) & m.mask;
        } while (b);

        PRNG rng(seeds[Is64Bit][rank_of(s)]);

        for (int i = 0; i < size;)
        {
            for (m.magic = 0; popcount((m.magic * m.mask) >> 56) < 6;)
                m.magic = rng.sparse_rand<Bitboard>();

            for (++cnt, i = 0; i < size; ++i)
            {
                unsigned idx = m.index(occupancy[i]);

                if (epoch[idx] < cnt)
                {
                    epoch[idx]     = cnt;
                    m.attacks[idx] = reference[i];
                }
                else if (m.attacks[idx] != reference[i])
                    break;
            }
        }
    }

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
    heap_caps_free(scratchAllocation);
#endif
}

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
// The sliding-attack lookup tables are read frequently during move generation.
// Keep their storage in external RAM so the ESP32-S3 internal DRAM remains
// available for stacks, task data, and latency-sensitive state.
static Bitboard* RookTable   = nullptr;
static Bitboard* BishopTable = nullptr;

static void allocate_attack_tables() {
    if (RookTable && BishopTable)
        return;

    constexpr uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    Bitboard* rook = static_cast<Bitboard*>(heap_caps_calloc(0x19000, sizeof(Bitboard), psramCaps));
    Bitboard* bishop = static_cast<Bitboard*>(heap_caps_calloc(0x1480, sizeof(Bitboard), psramCaps));

    if (!rook || !bishop)
    {
        if (rook)
            heap_caps_free(rook);
        if (bishop)
            heap_caps_free(bishop);
        SF_FATAL_ABORT("Failed to allocate PSRAM for rook/bishop attack tables");
    }

    RookTable   = rook;
    BishopTable = bishop;
}
#else
// Preserve the upstream allocation model on non-ESP32 targets.
static std::array<Bitboard, 0x19000> RookTable;
static std::array<Bitboard, 0x1480>  BishopTable;
#endif

}

#endif

void init() {

#ifdef USE_HYPERBOLA_QUINT
    init_magics(Magics);
#elif !defined(USE_DUAL_HYPERBOLA_QUINT)
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
    allocate_attack_tables();
    init_magics(ROOK, RookTable, Magics);
    init_magics(BISHOP, BishopTable, Magics);
#else
    init_magics(ROOK, RookTable.data(), Magics);
    init_magics(BISHOP, BishopTable.data(), Magics);
#endif
#endif

    for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1)
    {
        for (PieceType pt : {BISHOP, ROOK})
            for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2)
            {
                if (PseudoAttacks[pt][s1] & s2)
                {
                    LineBB[s1][s2] = (attacks_bb(pt, s1, 0) & attacks_bb(pt, s2, 0)) | s1 | s2;
                    BetweenBB[s1][s2] =
                      (attacks_bb(pt, s1, square_bb(s2)) & attacks_bb(pt, s2, square_bb(s1)));
                    RayPassBB[s1][s2] =
                      attacks_bb(pt, s1, 0) & (attacks_bb(pt, s2, square_bb(s1)) | s2);
                }
                BetweenBB[s1][s2] |= s2;
            }
    }
}

#ifndef USE_DUAL_HYPERBOLA_QUINT
const Magic& magic(Square s, PieceType pt) {
    assert((pt == BISHOP || pt == ROOK) && is_ok(s));
    return Magics[s][pt - BISHOP];
}
#endif

}  // namespace Stockfish::Attacks
