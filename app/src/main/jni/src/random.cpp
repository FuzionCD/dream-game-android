#include "random.h"
#include <cmath>     // std::floor
#include <cstring>   // std::memcpy

namespace {

constexpr uint32_t LCG_MUL = 0x41A7;   // 16807, the Park-Miller MINSTD multiplier

// 5 LCG streams (indexed 0..4 by callers). seed defaults to 999, matching
// FUN_10005708c's fallback for `seed == 0`, so calls work even before
// rngSeed is invoked. when the saved-game load path (FUN_100016b18) lands
// it will overwrite all 5 with the per-level seeds from the GameSnapshot's
// seed table.
int g_rngState[5] = { 999, 999, 999, 999, 999 };

// step the stream once and return a fraction in [0, 1].
// reproduces FUN_1000570ec's bit-packing trick exactly:
//   state = state * 0x41A7              (signed 32-bit, overflow allowed)
//   bits  = 0x40000000 | (state >> 10)  (sign=0, exponent=128, mantissa from state[10..31])
//   frac  = bit_cast<float>(bits) - 2.0
//   clamp frac to [0, 1]
float rngStep(uint32_t streamIdx) {
    int& state = g_rngState[streamIdx];
    // 32-bit signed multiply with overflow truncation
    state = (int)((uint32_t)state * LCG_MUL);

    // pack high 22 bits of state into mantissa of a float biased at exponent 1.
    // resulting float is in [2.0, ~3.0). bit_cast via memcpy avoids strict-
    // aliasing UB; the compiler optimizes it to a free register move.
    uint32_t bits = 0x40000000u | ((uint32_t)state >> 10);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    f -= 2.0f;

    // defensive clamp matching the binary's two-branch test. with the float-
    // bit-packing above, f naturally falls in [0, ~1), but values of f at
    // exactly 1.0 (the boundary) get capped, and any sign-bit accident pegs to 0.
    if (f >= 1.0f) {
        f = 1.0f;
    } else if (f <= 0.0f) {
        f = 0.0f;
    }
    return f;
}

}  // anonymous namespace

void rngSeed(int seed, uint32_t streamIdx) {
    g_rngState[streamIdx] = (seed != 0) ? seed : 999;
}

// FUN_1000570a8, bare LCG step, no output. signed 32-bit overflow allowed.
void rngAdvance(uint32_t streamIdx) {
    int& state = g_rngState[streamIdx];
    state = (int)((uint32_t)state * LCG_MUL);
}

int rngInt(int lo, int hi, uint32_t streamIdx) {
    // FUN_1000570ec swaps if hi < lo, otherwise uses the natural [lo, hi] range.
    int base, rangeCount;

    if (hi < lo) {
        rangeCount = lo - hi + 1;
        base       = hi;
    } else {
        rangeCount = hi - lo + 1;
        base       = lo;
    }

    float frac   = rngStep(streamIdx);
    float result = (float)base + (float)rangeCount * frac;

    // FUN_1000570ec's tail is `frintm` (floor) -> `fcvtzs` (signed truncate).
    // for non-negative results these are equivalent; we use std::floor to be
    // explicit since result can in principle be negative (e.g. lo == -5).
    return (int)std::floor(result);
}

float rngFloat(float lo, float hi, uint32_t streamIdx) {
    // FUN_1000571d0, straight lerp, no +1 since floats are continuous.
    float frac = rngStep(streamIdx);
    return lo + (hi - lo) * frac;
}

int32_t rngGetSeed(uint32_t streamIdx) {
    return static_cast<int32_t>(g_rngState[streamIdx]);
}
