#pragma once

#include <cstdint>

// reconstructed from Ghidra:
//   FUN_10005708c  - seed a single RNG stream
//   FUN_1000570ec  - integer RNG returning [lo, hi] inclusive
//   FUN_1000571d0  - float RNG returning [lo, hi)
//
// the binary keeps 5 independent LCG streams (indexed 0..4) at DAT_10007e870.
// each step is `state = state * 16807 (mod 2^32, signed overflow)`. output
// extraction packs the upper 22 bits of state into the mantissa of an IEEE
// float biased to exponent +1, giving a float in [2.0, ~3.0) for free; then
// 2.0 is subtracted and the result is clamped to [0, 1].
//
// not std::rand-equivalent: glibc, bionic, and iOS libc++ all use different
// LCG multipliers (1103515245 vs the binary's 16807), so identical seeds
// would produce divergent sequences. this port preserves the binary's
// algorithm so seeds in save data and per-level seed tables produce the same
// world layouts they would on iOS.

// seed one of the 5 streams. the binary's seeder falls back to 999 for
// `seed == 0`, which we mirror.
void rngSeed(int seed, uint32_t streamIdx);

// FUN_1000570a8: advance the stream by one LCG step (`state *= 16807`,
// no output). called once per stream immediately after rngSeed during
// Game::init's seeding ritual; without it every stream starts at its
// raw seed and produces identical first-draws on identical seeds.
void rngAdvance(uint32_t streamIdx);

// integer RNG: returns a uniformly-distributed int in `[min(lo, hi), max(lo, hi)]`
// inclusive. matches FUN_1000570ec's behavior including the swap-on-reverse-range.
int rngInt(int lo, int hi, uint32_t streamIdx);

// float RNG: returns a float in `[lo, hi)`. matches FUN_1000571d0.
float rngFloat(float lo, float hi, uint32_t streamIdx);

// FUN_1000570c4, return the current LCG state of stream `streamIdx`
// as a signed int. used by the GameSnapshot builder to capture the
// 5 per-stream seeds (snap.rngSeeds[5]) so a restored save resumes
// each stream where it left off.
int32_t rngGetSeed(uint32_t streamIdx);
