#pragma once

#include "animation_controller.h"
#include "quad.h"
#include "text_item.h"
#include <cstddef>
#include <cstdint>
#include <list>

// reconstructed from Ghidra:
//   ctor:        FUN_100010764  (allocated via operator_new(0x1018))
//   open:        FUN_100010990  (the post-run "And then I wake up" panel open)
//   update:      FUN_100011344  (per-frame animation cascade)
//   draw:        FUN_100011b78
//   setResultRankVisual(rank): FUN_100011730
//   clearKeysRow:              FUN_100011d54
//
// the post-run score / results panel. heap-allocated via operator_new(0x1018)
// at Game::create time and stored at Game+0x19128. opens when the player dies
// or completes a run: FUN_100045410's exitRequested branch passes the score
// data (turns / worlds / snags / items / etc.) plus the key-count earned this
// run, and the panel cascades 8 stat rows in then animates a row of key
// icons popping in at the bottom.
//
// not the in-game pause menu; that's the embedded PauseMenu panel at
// GameBoard+0xF488 (separate subsystem).
//
// the 8 stat rows are:
//   row 0: "Turns Taken"
//   row 1: "Worlds Completed"
//   row 2: "Snags Defeated"
//   row 3: "Special Snags Defeated"
//   row 4: "Levels Gained"
//   row 5: "Items Found"
//   row 6: "Events Activated"
//   row 7: "Score" (final aggregate; no per-value column shown)
//
// each row has 3 TextItem columns:
//   col 0 (+0x000): row title ("Turns Taken")
//   col 1 (+0x088): raw count (e.g. "84")
//   col 2 (+0x110): scored value (count * multiplier)
//
// totals roll up into totalScore at +0xE48 and are also the final row's
// scored-value column.
//
// the "keys row" linked list at +0xE50/+0xE58/+0xE60 holds heap-allocated
// 0xF0-byte icon nodes, one per key earned this run, all rendering the
// same key sprite from the panel atlas. these are the currency the player
// can spend in the between-run shop.
//
// a separate post-run "rank visual" (resultRankQuad + resultPanelQuad)
// switches UV/size between 4 cases driven by the int returned from
// FUN_100037d3c (the per-difficulty stat-history insertion). exact in-game
// meaning isn't fully pinned down; port byte-for-byte and keep the naming
// generic.

struct ScorePanelRow {
    TextItem titleLabel;   // +0x000  the per-row title (e.g. "Turns Taken")
    TextItem valueLabel;   // +0x088  the raw-count column (e.g. "84")
    TextItem scoreLabel;   // +0x110  the scored-value column (count * mult)
};

static_assert(sizeof(ScorePanelRow) == 0x198,
              "ScorePanelRow must be 0x198 bytes, 3 TextItems of 0x88 each.");

// the per-key icon value, stored in the keysRow std::list. all icons share the
// same atlas UV and tile horizontally at the bottom of the panel. this is the
// std::list node body: the binary's 0xF0 node is its 0x10 prev/next header plus
// this 0xE0 value.
struct KeyIconValue {
    Quad  tileQuad;    // +0x00..+0xD7
    float progress;    // +0xD8  scale-in animation 0..1
    float popInTimer;  // +0xDC  per-icon stagger delay
};

static_assert(sizeof(KeyIconValue) == 0xE0,
              "KeyIconValue must be 0xE0 (the std::list node value).");

class ScorePanel {
public:
    // FUN_100010764. zeros the visible flags, inits the background Quad +
    // title AnimationController, inits the 8 rows' 24 TextItems via
    // TextItem::init. centers the bg Quad on (0.5, virtualHeight*0.5).
    // called once at Game::create allocation time.
    void init();

    // FUN_100010990. opens the panel: sets visible=1, primes the row-cascade
    // animation, computes scored totals from the per-stat raw counts, and
    // builds the key-row display list with `keysEarned` entries.
    //
    // scoreCounts is the per-stat raw count array sourced from GameBoard
    // per-run counters when FUN_100045410's exitRequested branch fires:
    //   [0] turns_taken
    //   [1] worlds_completed
    //   [2] snags_defeated
    //   [3] special_snags_defeated
    //   [4] levels_gained
    //   [5] items_found
    //   [6] events_activated
    //
    // keysEarned is the number of currency keys awarded for the run. drives
    // both the bottom-row icon cascade and the per-entry pop-in delay.
    void open(const int* scoreCounts, int keysEarned);

    // FUN_100011344. per-frame anim:
    //   1. chrome alpha fade 0->1
    //   2. AnimationController::update on title
    //   3. scoreDelay countdown (initial 2.5s) blocks row cascade
    //   4. row-by-row cascade: rowProgress (+0x180) tracks current row,
    //      subProgress (+0x178) animates within (cosine ease)
    //   5. when rowProgress == 8 (final row reached), animate the key icons'
    //      alpha + scale via sin curves (per-icon popInTimer staggers them)
    //   6. when fully open and game touch state == 2 (drag), set
    //      closeRequested (+2) = 1; game.update reads this to dismiss
    void update(float dt);

    // FUN_100011b78. draws bg, then animated-in rows (gated on rowProgress /
    // subProgress), then result quads + the key icons (tail to head).
    // early-outs gracefully when nothing is animated in yet.
    void draw();

    // FUN_100011730. picks the result-rank visual (UV + size + position) for
    // resultRankQuad + resultPanelQuad based on `rank` (1..4). called from
    // FUN_100045410's exitRequested branch right after open() with the int
    // returned by FUN_100037d3c (the per-difficulty stat-history insert).
    // exact in-game semantics aren't fully pinned down; port byte-faithfully.
    // rank outside 1..4 clears the result quads.
    void setResultRankVisual(int rank);

    // FUN_100011d54. clears the key-icon linked list (drops + deletes each
    // row object). called by open() before rebuilding the list.
    void clearKeysRow();

    // ---- byte-exact field layout (0x1018 bytes total) ----
    //
    // tracks the operator_new(0x1018) size + FUN_100010764 ctor + all field
    // touch sites in FUN_100010990 / FUN_100011344 / FUN_100011b78.

    bool                  visible;            // +0x000
    bool                  secondaryVisible;   // +0x001  binary sets in open;
                                              //         purpose not yet pinned.
    bool                  closeRequested;     // +0x002  tap-to-dismiss signal
                                              //         set by update when game
                                              //         input state == drag
    uint8_t               pad003[5];          // +0x003..+0x007

    Quad                  bgQuad;             // +0x008..+0x0DF
                                              // setSize(1.0, virtualHeight)

    AnimationController   titleAnim;          // +0x0E0..+0x167
                                              // "And then I wake up" title via
                                              // FUN_100039ea4

    float                 titleAnchorX;       // +0x168
    float                 titleAnchorY;       // +0x16C

    float                 chromeAlpha;        // +0x170  0..1, fade-in
    float                 scoreDelay;         // +0x174  initial 2.5s, blocks rows
    float                 subProgress;        // +0x178  current row anim 0..1
    uint8_t               pad17C[4];          // +0x17C..+0x17F

    int64_t               rowProgress;        // +0x180  current row index 0..8

    ScorePanelRow         rows[8];            // +0x188..+0xE47  8 * 0x198
                                              //                 = 0xCC0

    int32_t               totalScore;         // +0xE48  sum of all row scoreLabel
                                              //         values
    uint8_t               padE4C[4];          // +0xE4C..+0xE4F

    std::list<KeyIconValue> keysRow;          // +0xE50..+0xE67 (libc++ head:
                                              // prev/next/size = 0x18 bytes)

    Quad                  resultRankQuad;     // +0xE68..+0xF3F  set by
                                              //                 setResultRankVisual
    Quad                  resultPanelQuad;    // +0xF40..+0x1017
};

static_assert(sizeof(ScorePanel) == 0x1018,
              "ScorePanel must be exactly 0x1018 bytes, matches the binary's "
              "`operator_new(0x1018)` in FUN_1000437a4.");

static_assert(offsetof(ScorePanel, bgQuad)          == 0x008);
static_assert(offsetof(ScorePanel, titleAnim)       == 0x0E0);
static_assert(offsetof(ScorePanel, chromeAlpha)     == 0x170);
static_assert(offsetof(ScorePanel, scoreDelay)      == 0x174);
static_assert(offsetof(ScorePanel, subProgress)     == 0x178);
static_assert(offsetof(ScorePanel, rowProgress)     == 0x180);
static_assert(offsetof(ScorePanel, rows)            == 0x188);
static_assert(offsetof(ScorePanel, totalScore)      == 0xE48);
static_assert(offsetof(ScorePanel, keysRow)         == 0xE50);
static_assert(offsetof(ScorePanel, resultRankQuad)  == 0xE68);
static_assert(offsetof(ScorePanel, resultPanelQuad) == 0xF40);
