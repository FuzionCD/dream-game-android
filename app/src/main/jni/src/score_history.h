#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>

// reconstructed from Ghidra:
//   insertEntry: FUN_100037d3c
//   bulkLoad:    FUN_100038004  (save-data loader, out of scope for F3)
//
// per-difficulty high-score history. the binary owns three intrusive
// doubly-linked lists (one per difficulty), each capped at 5 entries. a
// new entry's rank (1-based position in the sorted-descending list) gets
// returned to the caller and feeds ScorePanel::setResultRankVisual.
//
// libc++ std::list<T> on aarch64 is exactly 24 bytes (begin_, end_,
// compressed_pair<size_,allocator>), matching the binary's ListHead at
// stride per difficulty. each node is 8 prev + 8 next + sizeof(T);
// for sizeof(Entry)=0x20 that's a 0x30-byte node, matching the binary's
// `operator_new(0x30)` allocation in FUN_100037d3c.
//
// the binary embeds the tracker inside Game; the list heads it owns live
// with stride 0x18 per difficulty. ScoreHistory owns only those 3 list
// heads; the binary's tracker has additional state (a dirty flag, UI
// fields, etc.) that lives outside this class. Game splits its opaque tail
// gap to host ScoreHistory at the binary's exact tracker offset.

class ScoreHistory {
public:
    // entry payload (0x20 bytes). std::list adds prev/next pointers (16
    // bytes) around it, giving the 0x30 total the binary allocates via
    // operator_new(0x30) in FUN_100037d3c.
    //
    // field ordering matches the binary's FUN_100037d3c param->offset writes
    // and the pluralization checks in FUN_100037118 ("world(s)" reads worlds,
    // "level(s)" reads levels, "item(s)" reads items, "turn(s)" reads turns).
    // "worlds" here means levels-cleared (worldLevelIndex - 1), distinct
    // from `worldIndex` (= difficulty tier 0..2). snags-defeated lives only
    // on the per-run GameBoard counter; it's not persisted to score entries.
    struct Entry {
        uint32_t worldIndex;       // difficulty tier (0=easy/1=normal/2=hard)
        uint32_t characterIndex;   // selects portrait UV in leaderboard
        uint32_t levels;           // levelsGained
        uint32_t items;            // itemsFound
        uint32_t worlds;           // worldLevelIndex - 1
        uint32_t turns;            // totalTurnCount
        int32_t  score;            // totalScore
    };

    static constexpr int kListsPerDifficulty = 3;  // easy / normal / hard
    static constexpr int kMaxEntriesPerList  = 5;

    // FUN_100037d3c. insert a new entry into the per-difficulty list for
    // `worldIndex`. returns the 1-based rank (1 = best score on this
    // difficulty). evicts the lowest-scoring entry if the list would
    // exceed kMaxEntriesPerList. param order matches the binary's call
    // shape at FUN_100045410 (worldIndex, characterIndex, levels, items,
    // worlds, turns, score).
    int insertEntry(uint32_t worldIndex,
                    uint32_t characterIndex,
                    uint32_t levels, uint32_t items,
                    uint32_t worlds, uint32_t turns,
                    int32_t  score);

    // accessor for inspection / serialization (= Phase H save-data write side).
    const std::list<Entry>& list(int difficulty) const {
        return lists_[static_cast<size_t>(difficulty)];
    }

    // FUN_100037e88. returns true if any of the 3 difficulty lists has at
    // least one entry. drives the titleMenu's "history exists" hint passed
    // to FUN_10004ad80 (param_3) on every return-to-title transition.
    bool hasAnyEntries() const {

        for (const auto& l : lists_) {

            if (!l.empty()) {
                return true;
            }
        }

        return false;
    }

private:
    std::array<std::list<Entry>, kListsPerDifficulty> lists_;
};

#ifdef __aarch64__
#endif
