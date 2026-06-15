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
// LeaderboardMenu is the per-difficulty score-history viewer at Game+0x1CB18.
// 3 difficulty rows (Easy / Normal / Hard) of 5 score entries each, each
// entry shows a character portrait + rank + 4 stat counters with their
// singular/plural unit labels. data comes from the embedded ScoreHistory's
// 3 sorted lists at LeaderboardMenu+0x69A8 (= Game+0x234C0).

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
        int32_t characterIndex;    // from Entry+0x04
        int32_t levels;            // from Entry+0x08
        int32_t items;             // from Entry+0x0c
        int32_t worlds;            // from Entry+0x10
        int32_t turns;             // from Entry+0x14
        int32_t score;             // from Entry+0x18
    };
    static_assert(sizeof(XferEntry) == 0x1C,
                  "XferEntry must be 7 ints (28 bytes) with no trailing "
                  "padding to match the binary's vector stride in "
                  "FUN_100037ebc.");

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
    // pattern (sprintf "%d" of node+0x18..0x28 + plural-check on the same).
    struct ScoreRow {
        Quad     emptyMarker;          // +0x000  drawn when populated==0
        uint8_t  populated;            // +0x0D8
        uint8_t  pad0D9[7];            // +0x0D9..+0x0DF
        Quad     portrait;             // +0x0E0  setPortraitVisual from entry.characterIndex
        TextItem levelsCount;          // +0x1B8  sprintf("%d", entry.levels)
        TextItem itemsCount;           // +0x240  sprintf("%d", entry.items)
        TextItem worldsCount;          // +0x2C8  sprintf("%d", entry.worlds)
        TextItem turnsCount;           // +0x350  sprintf("%d", entry.turns)
        TextItem scoreCount;           // +0x3D8  sprintf("%d", entry.score)
        TextItem levelsLabel;          // +0x460  "level"/"levels"
        TextItem itemsLabel;           // +0x4E8  "item"/"items"
        TextItem worldsLabel;          // +0x570  "world"/"worlds"
        TextItem turnsLabel;           // +0x5F8  "turn"/"turns"
    };
    static_assert(sizeof(ScoreRow) == 0x680, "ScoreRow stride must match the binary's 0x680 per-row footprint");

    // ---- per-difficulty section (0x22B8 bytes) ----
    struct Section {
        Quad     divider1;             // +0x000
        Quad     divider2;             // +0x0D8
        TextItem diffLabel;            // +0x1B0  "Easy" / "Normal" / "Hard"
        ScoreRow rows[5];              // +0x238  5 * 0x680 = 0x2080
    };
    static_assert(sizeof(Section) == 0x22B8, "Section stride must match the binary's 0x22B8 per-difficulty footprint");

    // ---- byte-exact field layout (Game+0x1CB18..+0x234C0, 0x69A8 bytes) ----

    uint8_t  visible;                  // +0x000  gate for update + close-request reads
    uint8_t  closeRequested;           // +0x001  press-miss OR back-tap confirmed
    uint8_t  dirty;                    // +0x002  set by ScoreHistory::insertEntry, consumed by FUN_100037ebc
    uint8_t  pad003[5];                // +0x003..+0x007  alignment for Label vtable

    Label    headerLabel;              // +0x008  "Leaderboard" title + back-button hit-test
    Quad     chromeQuad;               // +0x0A0  decorative chrome

    uint8_t  pressed;                  // +0x178  press-hold state (set on press-hit, cleared on release)
    uint8_t  backTapConfirmed;         // +0x179  set on confirmed back-button release; binary writes but no read site located in C++ core (vestigial)
    uint8_t  pad17A[6];                // +0x17A..+0x17F  alignment

    Section  sections[3];              // +0x180  3 * 0x22B8 = 0x6828 (ends at +0x69A8)
};

static_assert(sizeof(LeaderboardMenu) == 0x69A8,
              "LeaderboardMenu must be exactly 0x69A8 bytes, header region "
              "ending exactly where ScoreHistory begins at Game+0x234C0.");

static_assert(offsetof(LeaderboardMenu, visible)          == 0x0000);
static_assert(offsetof(LeaderboardMenu, closeRequested)   == 0x0001);
static_assert(offsetof(LeaderboardMenu, dirty)            == 0x0002);
static_assert(offsetof(LeaderboardMenu, headerLabel)      == 0x0008);
static_assert(offsetof(LeaderboardMenu, chromeQuad)       == 0x00A0);
static_assert(offsetof(LeaderboardMenu, pressed)          == 0x0178);
static_assert(offsetof(LeaderboardMenu, backTapConfirmed) == 0x0179);
static_assert(offsetof(LeaderboardMenu, sections)         == 0x0180);
