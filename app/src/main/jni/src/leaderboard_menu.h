#pragma once

#include "label.h"
#include "quad.h"
#include "text_item.h"
#include <cstddef>
#include <cstdint>
#include <vector>

// reconstructed from Ghidra:
//   ctor:       FUN_100036c54
//   open:       FUN_100037118
//   update:     FUN_100037b34
//   draw:       FUN_100037c28
//   dirtyXfer:  FUN_100037ebc
//
// LeaderboardMenu is the per-difficulty score-history viewer at Game.leaderboardMenu_.
// 3 difficulty rows (Easy / Normal / Hard) of 5 score entries each, each
// entry shows a character portrait + rank + 4 stat counters with their
// singular/plural unit labels. data comes from the embedded ScoreHistory's
// 3 sorted lists (= Game.scoreHistory_).

class LeaderboardMenu {
public:
    // FUN_100036c54 visual setup tail: installs the 3 header chrome glyphs,
    // sets the close-X chromeQuad UV+size, and primes the Label's measured
    // width via setSize. called once from Game::create after placement-new.
    void init();

    void open();                 // FUN_100037118
    void update(float touchX, float touchY);  // FUN_100037b34
    void draw();                 // FUN_100037c28

    // FUN_100037ebc. flat 28-byte score record. each ScoreHistory entry
    // gets copied into a std::vector<XferEntry> on Game when the leaderboard
    // mutates (insertEntry sets the dirty flag). iOS pipes this vector to
    // GameKit's updateLeaderboardScores; on Android nothing consumes it
    // but we port the staging step for binary fidelity.
    struct XferEntry {
        int32_t worldIndex;        // = source list's difficulty (loop index)
        int32_t characterIndex;    // from Entry.characterIndex
        int32_t levels;            // from Entry.currentX
        int32_t items;             // from Entry.currentY
        int32_t worlds;            // from Entry.targetX
        int32_t turns;             // from Entry.targetY
        int32_t score;             // from Entry.progress
    };

    // FUN_100037ebc. clear dirty, clear destination vector, walk all 3
    // ScoreHistory lists pushing each entry as an XferEntry.
    void dirtyXfer(std::vector<XferEntry>& dest);

    // FUN_100038004. inverse of dirtyXfer. called from Game::init after
    // SaveSystem::load populates the save buffer from disk: re-inserts
    // each saved XferEntry back into ScoreHistory. dirty propagation is
    // handled inside ScoreHistory::insertEntry, matching binary.
    void restoreFromSave(const std::vector<XferEntry>& saved);

    // returns true if either close path fired (back-tap on header OR
    // tap-outside-header). Game::update consumes this on every frame.
    bool isCloseRequested() const { return closeRequested != 0; }

    // ---- per-score-row sub-structure (0x680 bytes) ----
    // semantic TextItem mapping derived from FUN_100037118's field-read
    // pattern (sprintf "%d" of the entry's stat fields + plural-check on
    // the same).
    struct ScoreRow {
        Quad     emptyMarker;          // drawn when populated==0
        uint8_t  populated;
        Quad     portrait;             // setPortraitVisual from entry.characterIndex
        TextItem levelsCount;          // sprintf("%d", entry.levels)
        TextItem itemsCount;           // sprintf("%d", entry.items)
        TextItem worldsCount;          // sprintf("%d", entry.worlds)
        TextItem turnsCount;           // sprintf("%d", entry.turns)
        TextItem scoreCount;           // sprintf("%d", entry.score)
        TextItem levelsLabel;          // "level"/"levels"
        TextItem itemsLabel;           // "item"/"items"
        TextItem worldsLabel;          // "world"/"worlds"
        TextItem turnsLabel;           // "turn"/"turns"
    };

    // ---- per-difficulty section (0x22B8 bytes) ----
    struct Section {
        Quad     divider1;
        Quad     divider2;
        TextItem diffLabel;            // "Easy" / "Normal" / "Hard"
        ScoreRow rows[5];              // 5 * 0x680 = 0x2080
    };

    // ---- byte-exact field layout (Game.leaderboardMenu_, 0x69A8 bytes) ----

    uint8_t  visible;                  // gate for update + close-request reads
    uint8_t  closeRequested;           // press-miss OR back-tap confirmed
    uint8_t  dirty;                    // set by ScoreHistory::insertEntry, consumed by FUN_100037ebc

    Label    headerLabel;              // "Leaderboard" title + back-button hit-test
    Quad     chromeQuad;               // decorative chrome

    uint8_t  pressed;                  // press-hold state (set on press-hit, cleared on release)
    uint8_t  backTapConfirmed;         // set on confirmed back-button release; binary writes but no read site located in C++ core (vestigial)

    Section  sections[3];              // 3 * 0x22B8 = 0x6828
};
