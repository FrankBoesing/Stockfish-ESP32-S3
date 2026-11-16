// Temporary, noinline probes for inspecting the ESP32-S3 Xtensa GCC code generation.
// This translation unit is compiled with the normal Stockfish PlatformIO flags.
// It is never called by the firmware; inspect its object file in the CI artifact.

#include <cstdint>

#include "stockfish/attacks.h"
#include "stockfish/bitboard.h"
#include "stockfish/misc.h"

extern "C" {

__attribute__((noinline, used))
uint32_t s3_probe_mul_hi64(uint64_t a, uint32_t b) {
    return (uint32_t) Stockfish::mul_hi64((Stockfish::u64) a, (Stockfish::usize) b);
}

__attribute__((noinline, used))
uint32_t s3_probe_popcountll(uint64_t value) {
    return (uint32_t) Stockfish::popcount((Stockfish::Bitboard) value);
}

__attribute__((noinline, used))
uint32_t s3_probe_lsb64(uint64_t value) {
    return (uint32_t) Stockfish::lsb((Stockfish::Bitboard) value);
}

__attribute__((noinline, used))
uint32_t s3_probe_msb64(uint64_t value) {
    return (uint32_t) Stockfish::msb((Stockfish::Bitboard) value);
}

__attribute__((noinline, used))
uint32_t s3_probe_magic_index(const Stockfish::Attacks::Magic* magic, uint64_t occupied) {
    return magic->index((Stockfish::Bitboard) occupied);
}

__attribute__((noinline, used))
void s3_probe_prefetch(const void* address) {
    Stockfish::prefetch(address);
}

__attribute__((noinline, used))
int64_t s3_probe_dot4_i16(const int16_t* a, const int16_t* b) {
    int64_t sum = 0;
    for (int i = 0; i < 4; ++i)
        sum += (int32_t) a[i] * (int32_t) b[i];
    return sum;
}

}  // extern "C"
