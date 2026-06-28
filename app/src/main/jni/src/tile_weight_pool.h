#pragma once

#include "random.h"
#include <cstddef>
#include <cstdint>
#include <vector>

// 12-byte entry: (typeId, baseWeight, currentWeight).
//
// the in-game roll is an "anti-streak" weighted lottery. each entry has a
// baseWeight (its per-loss accumulation rate) and a currentWeight (its
// probability mass at the current roll). every roll picks one entry weighted
// by currentWeight, drains the winner back to baseWeight, and bumps every
// other entry's currentWeight by baseWeight. types that haven't been rolled
// in a while accumulate weight until they're picked.
//
// the GameBoard ctor (FUN_100014ed8 at 100015460..1000154ac) pushes 5 entries
// via FUN_10004cfec. typeIds map to content kinds returned by rollRackTile:
//   (2, 30) DEF   (3, 30) ATK   (6, 10) HP   (5, 15) CTRL   (1, 15) snag
struct TileWeightEntry {
    int32_t typeId;         // - value returned by weightedRollTileType (content type)
    int32_t baseWeight;     // - per-loss weight increment, and floor on win
    int32_t currentWeight;  // - probability mass at the current roll
};

// std::vector<TileWeightEntry> at GameBoard.tileWeightPool. the binary uses libc++
// aarch64's std::vector here; its three-pointer (begin/end/cap) layout is
// exactly 0x18 bytes and lines up 1:1 with FUN_10004cfe0 / FUN_10004cfec /
// FUN_10004d388. we use the real type so push_back / assign / capacity growth
// are handled by libstdc++ instead of duplicated by hand.
//
// reconstructed Ghidra anchors (the helpers below cover the domain logic that
// isn't a stdlib primitive; the vector primitives themselves are inlined):
//   ctor (zero-init) : FUN_10004cfe0  (vector default ctor; called from
//                                      GameBoard ctor FUN_100014ed8)
//   push entry       : FUN_10004cfec  (5x from GameBoard ctor; we use
//                                      push_back({type, base, base}))
//   reset to base    : FUN_10004d198  (called from FUN_1000165e8 each level
//                                      start; resetTileWeightsToBase below)
//   weighted roll    : FUN_10004d078  (called from rollRackTile;
//                                      weightedRollTileType below)
//   assign from src  : FUN_10004d26c  (called from the saved-game load
//                                      path FUN_100016b18 with src =
//                                      GameSnapshot.tileWeightPool; std::vector::assign)
//   grow + push      : FUN_10004d388  (vector reserve+move helper; handled by
//                                      libstdc++)
using TileWeightPool = std::vector<TileWeightEntry>;

// FUN_10004d198 - reset every entry's currentWeight back to baseWeight.
inline void resetTileWeightsToBase(TileWeightPool& pool) {
    for (TileWeightEntry& e : pool) {
        e.currentWeight = e.baseWeight;
    }
}

// FUN_10004d078 - weighted-by-currentWeight roll. winner drains to base,
// non-winners gain baseWeight. returns winner's typeId; 0 on empty pool
// (matches binary's empty-vector fall-through).
inline int32_t weightedRollTileType(TileWeightPool& pool, uint32_t streamIdx) {
    if (pool.empty()) {
        return 0;
    }

    // total currentWeight across all entries.
    int32_t total = 0;

    for (const TileWeightEntry& e : pool) {
        total += e.currentWeight;
    }

    // RNG int [0, total - 1] inclusive. rollRackTile uses gameplay stream 4.
    int32_t roll = rngInt(0, total - 1, streamIdx);

    // walk again, decrementing roll by each currentWeight; first entry where
    // the running counter goes negative is the winner.
    size_t winner = 0;
    bool found = false;

    for (size_t i = 0; i < pool.size(); ++i) {
        roll -= pool[i].currentWeight;

        if (roll < 0) {
            winner = i;
            found = true;
            break;
        }
    }

    // matches binary's fall-through (uVar7 stays 0 if loop completes without
    // hitting LAB_10004d13c). only reachable when total currentWeight is 0,
    // which is also the edge case the binary leaves unguarded.
    if (!found) {
        winner = 0;
    }

    // update weights: winner = base; non-winners += base.
    for (size_t i = 0; i < pool.size(); ++i) {
        int32_t v = pool[i].baseWeight;

        if (i != winner) {
            v = pool[i].currentWeight + v;
        }

        pool[i].currentWeight = v;
    }

    return pool[winner].typeId;
}
