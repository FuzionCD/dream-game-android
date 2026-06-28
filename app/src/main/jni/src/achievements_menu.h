#pragma once

#include "label.h"
#include "quad.h"
#include "text_item.h"
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>

// reconstructed from Ghidra:
//   open:    FUN_100058410  (init+open hybrid; first half placement-new of
//                            50 tiles guarded by *param_1==0, second half
//                            sorted-map population + per-tile state copy)
//   update:  FUN_10005888c  (touch + scroll inertia + per-tile fade-in)
//   draw:    FUN_100058eb4  (translate + walk sorted map + per-tile chrome)
//
// AchievementsMenu lives at Game.achievementsMenu_ (0xAC68 bytes). vertical scrolling
// list of 50 per-achievement tiles. structurally similar to Shop and
// LeaderboardMenu (close-on-back-tap header, per-tile TextItems, shared
// chrome Quads) but adds vertical scrolling and a sort-by-status display
// order via std::map.

class AchievementsMenu {
public:
    // FUN_100057d14 + FUN_100058410 first half: builds the one-time chrome.
    // populates the 4 header chrome Quads, the 9-slice tileBgLabel, the
    // 3 shared decoration Quads, all 50 per-tile icon Quads, and all 50
    // per-tile TextItem (title / description / progressText) chrome.
    // called once from Game::create; the binary's coldStartFlag is
    // preserved as a vestigial layout field but not exercised here.
    void init();

    // FUN_100058410 second half: per-show open. resets flags, rebuilds
    // the sortedDisplay std::map by walking all 50 achievements through
    // AchievementTracker queries, and primes progressText + progressBar
    // per tile based on counter / target / locked state. fires the
    // trailing update(0) prime tick at the end.
    void open();

    // FUN_10005888c, per-frame update. handles 3 concerns:
    //   1. touch input: press/release of backButton, and drag-to-scroll
    //   2. scroll inertia + rubber-band bounce-back at the list extremes
    //   3. per-tile fadeIn animation (0.01 -> 1.0 over ~0.3s when revealed)
    // touchX/touchY are read from getGame() per the binary; dt is passed in.
    void update(float dt);

    // FUN_100058eb4, draw. translates by 84/640 vertical, walks the
    // sortedDisplay map in key order, and renders each tile inside its
    // own push/pop matrix with row-parity coloring + locked/unlocked
    // chrome + pulse animation on unlocked-deco during fadeIn. closes
    // with the 4 header chrome Quads.
    void draw();

    // ---- per-tile sub-layout (0x350 bytes) ----
    //
    // 50 of these, visited in sortedDisplay order during
    // draw, not in array order; the std::map walk is what gives the menu
    // its "unlocked first, locked last" arrangement.
    struct Tile {
        TextItem title;            // setString from ACHIEVEMENT_TABLE[idx].title
        TextItem description;      // setString from ACHIEVEMENT_TABLE[idx].description
        Quad     icon;             // UV from ACHIEVEMENT_TABLE[idx].iconX/iconY,
                                   //         size = ACHIEVEMENT_ICON_SIZE_PX (84x84)
        Quad     progressBar;      // background bar (drawn first under tex 9)
        TextItem progressText;     // "N", current count for progress achievements

        uint8_t  locked;           // cached AchievementTracker::isLocked at open()
        float    fadeIn;           // 0.0 = invisible (locked+unshown), 1.0 = visible;
                                   //         update() ticks toward 1.0 when tile scrolls into view
    };

    // ---- byte-exact field layout ----

    uint8_t        coldStartFlag;          // set to 1 after first open() builds tile chrome;
                                           //          re-opens skip the chrome-construction half
    uint8_t        visible;                // gate for update + close-request reads
    uint8_t        closeRequested;         // set when back-tap confirmed -> T=9
    uint8_t        backTapConfirmed;       // cleared every update tick, no read site
                                           //          (vestigial; matches LeaderboardMenu pattern)

    // 4 header chrome Quads drawn last under tex 9. the backButton /
    // backButtonOverlay pair is the "back button": backButton is the
    // hit-test target, and both get
    // tinted grey on press (same dual-tint pattern as LeaderboardMenu's
    // headerLabel + chromeQuad).
    Quad           chromeA;                // static header decoration (left?)
    Quad           chromeB;                // static header decoration (right?)
    Quad           backButton;             // hit-test target (tap-to-close)
    Quad           backButtonOverlay;      // icon/label sitting on top of backButton

    uint8_t        pressed;                // press-hold state

    // shared per-tile background Label. positioned inside the per-tile
    // translate by draw(); recolored each row (alternating palette by parity).
    Label          tileBgLabel;

    // shared per-tile decoration Quads. each is recolored + drawn at the
    // current tile's translated position inside draw()'s per-tile loop:
    //   lockedDeco        drawn when tile.locked != 0 (tex 9, no icon)
    //   unlockedHighlight drawn when tile.locked == 0 (tex 9, before icon)
    //   unlockedDeco      drawn when tile.locked == 0 (tex 8, after icon)
    Quad           lockedDeco;
    Quad           unlockedHighlight;
    Quad           unlockedDeco;

    // ---- scroll state ----
    uint8_t        scrollDragging;         // set on press-inside-list, cleared on release
    float          dragStartTouchY;        // touchY when drag began
    float          dragStartScrollY;       // scrollY when drag began
    float          lastTouchY;             // per-frame touchY for velocity calc
    float          scrollVelocity;         // accumulated drag velocity
    float          scrollY;                // current vertical scroll offset
    float          scrollResidual;         // bounce-back / inertia residual

    // ---- 50-tile array ----
    Tile           tiles[50];              // (50 x 0x350 = 0xA5A0)

    // ---- sorted-display std::map (24 bytes) ----
    //
    // populated in open() by inserting (signedSortKey -> idx-list) entries
    // for each of the 50 achievements. signedSortKey is:
    //   +sortKey   when tile.locked != 0 (still locked, fadeIn = 1)
    //               or (unlocked and hasBeenShown)
    //   -sortKey   when unlocked-banner-pending (fadeIn = 0)
    // negative keys sort first (= smallest), placing freshly-unlocked
    // banner-pending tiles at the top of the scroll list. they then
    // animate in when the user taps them.
    //
    // value type is std::list<int> rather than int: the binary's tree
    // node is 0x40 bytes with an embedded list head (see
    // FUN_100059208's `operator_new(0x40)` + self-aliasing list init),
    // so each map entry holds a list of one-or-more idx values for the
    // same sortKey. in practice every list is exactly one element since
    // sortKeys are unique per achievement, but we match the binary's
    // container choice for byte-exact iteration.
    std::map<int, std::list<int>> sortedDisplay;
};
