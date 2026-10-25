#pragma once

// The C runtime rand() FGP.exe links (0x0048d323): Microsoft's linear
// congruential generator, one state per thread, seeded 1 until srand.

#include <cstdint>

namespace fgp {

struct CrtRandom {
    std::uint32_t state{1};
    // rand(): the multiply at 0x0048d32b, then bits 16..30.
    int next() {
        state = state * 214013U + 2531011U;
        return static_cast<int>((state >> 16) & 0x7fffU);
    }
    // 0x0040c530: rand() % n, without calling rand() for n == 0.
    int below(int bound) { return bound != 0 ? next() % bound : 0; }
};

} // namespace fgp
