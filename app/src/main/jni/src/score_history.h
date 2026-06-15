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
// doubly-linked lists at `tracker+0x69a8 + worldIndex*0x18` (one per
// difficulty), each capped at 5 entries. a new entry's rank (1-based
// position in the sorted-descending list) gets returned to the caller and
// feeds ScorePanel::setResultRankVisual.
//
// libc++ std::list<T> on aarch64 is exactly 24 bytes (begin_, end_,
// compressed_pair<size_,allocator>), matching the binary's ListHead at
// +0x18 stride per difficulty. each node is 8 prev + 8 next + sizeof(T);
// for sizeof(Entry)=0x20 that's a 0x30-byte node, matching the binary's
// `operator_new(0x30)` allocation in FUN_100037d3c.
//
// the binary embeds the tracker inside Game at +0x1cb18; the list heads
// it owns live at +0x234C0 (= tracker+0x69a8) with stride 0x18 per
// difficulty. ScoreHistory owns only those 3 list heads; the binary's
// tracker has additional state (dirty flag at +0x1cb1a, UI fields, etc.)
// that lives outside this class. Game splits its opaque tail gap to host
// ScoreHistory at the binary's exact +0x234C0 offset.

class ScoreHistory {
public:
    // entry payload (0x20 bytes). std::list adds prev/next pointers (16
    // bytes) around it, giving the 0x30 total the binary allocates via
    // operator_new(0x30) in FUN_100037d3c.
    //
    // field ordering matches the binary's FUN_100037d3c param->offset writes
    // and the pluralization checks in FUN_100037118 ("world(s)" reads +0x10,
    // "level(s)" reads +0x08, "item(s)" reads +0x0c, "turn(s)" reads +0x14).
    // "worlds" here means levels-cleared (worldLevelIndex - 1), distinct
    // from `worldIndex` (= difficulty tier 0..2). snags-defeated lives only
    // on the per-run GameBoard counter; it's not persisted to score entries.
    struct Entry {
        uint32_t worldIndex;       // +0x00  difficulty tier (0=easy/1=normal/2=hard)
        uint32_t characterIndex;   // +0x04  selects portrait UV in leaderboard
        uint32_t levels;           // +0x08  levelsGained (board+0x30)
        uint32_t items;            // +0x0c  itemsFound  (board+0x34)
        uint32_t worlds;           // +0x10  worldLevelIndex - 1
        uint32_t turns;            // +0x14  totalTurnCount (board+0x20)
        int32_t  score;            // +0x18  totalScore
        uint8_t  pad[4];           // +0x1c
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

static_assert(sizeof(ScoreHistory::Entry) == 0x20,
              "ScoreHistory::Entry payload must be 0x20 bytes; the binary's "
              "operator_new(0x30) node = 8 prev + 8 next + 0x20 payload.");

#ifdef __aarch64__
static_assert(sizeof(std::list<ScoreHistory::Entry>) == 0x18,
              "libc++ std::list on aarch64 must be 0x18 bytes, matching the "
              "binary's ListHead stride at tracker+0x69a8 + worldIndex*0x18.");
static_assert(sizeof(ScoreHistory) == 0x18 * ScoreHistory::kListsPerDifficulty,
              "ScoreHistory must be exactly 3 * 0x18 = 0x48 bytes so it can "
              "be embedded at Game+0x234C0 with no padding.");
#endif
