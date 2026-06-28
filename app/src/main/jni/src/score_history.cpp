#include "score_history.h"

#include "game.h"

// FUN_100037d3c.
//
// inserts a new entry into the per-difficulty list, sorted by `score` in
// descending order. evicts the worst-ranked entry if the list would exceed
// kMaxEntriesPerList. returns the 1-based rank (1 = highest score).
//
// the binary's two-phase evict (pre-emptive at count > 4, post-insert at
// count > 5) is preserved here; equivalent to a single pop_back when the
// pre-insert size hits the cap, but matched verbatim so the rank counting
// matches the binary exactly when the new score lands at the very end.
//
// binary signature is `(LeaderboardMenu*, worldIndex, ...)` because the
// lists live inside the LeaderboardMenu struct. our port splits them into
// a separate ScoreHistory class at the same Game offset, so the
// `leaderboard.dirty = 1` side effect at the binary's first line lands
// here via getGame()->leaderboardMenu() instead of direct pointer math.
int ScoreHistory::insertEntry(uint32_t worldIndex,
                              uint32_t characterIndex,
                              uint32_t levels, uint32_t items,
                              uint32_t worlds, uint32_t turns,
                              int32_t  score) {

    if (Game* g = getGame()) {
        g->leaderboardMenu().dirty = 1;
    }

    if (worldIndex >= kListsPerDifficulty) {
        worldIndex = 0;
    }

    auto& list = lists_[static_cast<size_t>(worldIndex)];

    // step 1: pre-emptive evict if already at 5 entries. matches binary's
    // `if (4 < uVar5) { ... pop tail ... }` before the rank-walk.
    if (list.size() > static_cast<size_t>(kMaxEntriesPerList - 1)) {
        list.pop_back();
    }

    // step 2: walk to find insertion position (sorted descending by score).
    auto it   = list.begin();
    int  rank = 1;

    while (it != list.end() && score < it->score) {
        rank += 1;
        ++it;
    }

    // step 3: insert.
    Entry entry{};
    entry.worldIndex     = worldIndex;
    entry.characterIndex = characterIndex;
    entry.levels         = levels;
    entry.items          = items;
    entry.worlds         = worlds;
    entry.turns          = turns;
    entry.score          = score;
    list.insert(it, entry);

    // step 4: post-insert evict if still over cap (defensive; the step-1
    // pre-evict guarantees we won't exceed, but the binary keeps both
    // checks).
    if (list.size() > static_cast<size_t>(kMaxEntriesPerList)) {
        list.pop_back();
    }

    return rank;
}
