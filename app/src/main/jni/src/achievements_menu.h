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
// AchievementsMenu lives at Game+0x23508 (0xAC68 bytes). vertical scrolling
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
    // called once from Game::create; the binary's coldStartFlag at +0x000
    // is preserved as a vestigial layout field but not exercised here.
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
    // 50 of these starting at +0x6B0. visited in sortedDisplay order during
    // draw, not in array order; the std::map walk is what gives the menu
    // its "unlocked first, locked last" arrangement.
    struct Tile {
        TextItem title;            // +0x000  setString from ACHIEVEMENT_TABLE[idx].title
        TextItem description;      // +0x088  setString from ACHIEVEMENT_TABLE[idx].description
        Quad     icon;             // +0x110  UV from ACHIEVEMENT_TABLE[idx].iconX/iconY,
                                   //         size = ACHIEVEMENT_ICON_SIZE_PX (84x84)
        Quad     progressBar;      // +0x1E8  background bar (drawn first under tex 9)
        TextItem progressText;     // +0x2C0  "N", current count for progress achievements

        uint8_t  locked;           // +0x348  cached AchievementTracker::isLocked at open()
        uint8_t  pad349[3];        // +0x349..+0x34B
        float    fadeIn;           // +0x34C  0.0 = invisible (locked+unshown), 1.0 = visible;
                                   //         update() ticks toward 1.0 when tile scrolls into view
    };
    static_assert(sizeof(Tile) == 0x350,
                  "Tile stride must match the binary's 0x350 per-tile footprint");

    // ---- byte-exact field layout ----

    uint8_t        coldStartFlag;          // +0x0000  set to 1 after first open() builds tile chrome;
                                           //          re-opens skip the chrome-construction half
    uint8_t        visible;                // +0x0001  gate for update + close-request reads
    uint8_t        closeRequested;         // +0x0002  set when back-tap confirmed -> T=9
    uint8_t        backTapConfirmed;       // +0x0003  cleared every update tick, no read site
                                           //          (vestigial; matches LeaderboardMenu pattern)
    uint8_t        pad0004[4];             // +0x0004..+0x0007

    // 4 header chrome Quads drawn last under tex 9. the +0x1B8 / +0x290 pair
    // is the "back button": backButton is the hit-test target, and both get
    // tinted grey on press (same dual-tint pattern as LeaderboardMenu's
    // headerLabel + chromeQuad).
    Quad           chromeA;                // +0x0008  static header decoration (left?)
    Quad           chromeB;                // +0x00E0  static header decoration (right?)
    Quad           backButton;             // +0x01B8  hit-test target (tap-to-close)
    Quad           backButtonOverlay;      // +0x0290  icon/label sitting on top of backButton

    uint8_t        pressed;                // +0x0368  press-hold state
    uint8_t        pad0369[7];             // +0x0369..+0x036F

    // shared per-tile background Label. positioned inside the per-tile
    // translate by draw(); recolored each row (alternating palette by parity).
    Label          tileBgLabel;            // +0x0370..+0x0407

    // shared per-tile decoration Quads. each is recolored + drawn at the
    // current tile's translated position inside draw()'s per-tile loop:
    //   lockedDeco        drawn when tile.locked != 0 (tex 9, no icon)
    //   unlockedHighlight drawn when tile.locked == 0 (tex 9, before icon)
    //   unlockedDeco      drawn when tile.locked == 0 (tex 8, after icon)
    Quad           lockedDeco;             // +0x0408
    Quad           unlockedHighlight;      // +0x04E0
    Quad           unlockedDeco;           // +0x05B8

    // ---- scroll state ----
    uint8_t        scrollDragging;         // +0x0690  set on press-inside-list, cleared on release
    uint8_t        pad0691[3];             // +0x0691..+0x0693
    float          dragStartTouchY;        // +0x0694  touchY when drag began
    float          dragStartScrollY;       // +0x0698  scrollY when drag began
    float          lastTouchY;             // +0x069C  per-frame touchY for velocity calc
    float          scrollVelocity;         // +0x06A0  accumulated drag velocity
    float          scrollY;                // +0x06A4  current vertical scroll offset
    float          scrollResidual;         // +0x06A8  bounce-back / inertia residual
    uint8_t        pad06AC[4];             // +0x06AC..+0x06AF  alignment for Tile[]

    // ---- 50-tile array ----
    Tile           tiles[50];              // +0x06B0..+0xAC4F  (50 x 0x350 = 0xA5A0)

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
    // node is 0x40 bytes with an embedded list head at +0x28 (see
    // FUN_100059208's `operator_new(0x40)` + self-aliasing list init),
    // so each map entry holds a list of one-or-more idx values for the
    // same sortKey. in practice every list is exactly one element since
    // sortKeys are unique per achievement, but we match the binary's
    // container choice for byte-exact iteration.
    std::map<int, std::list<int>> sortedDisplay;   // +0xAC50..+0xAC67
};

static_assert(sizeof(AchievementsMenu) == 0xAC68,
              "AchievementsMenu must be exactly 0xAC68 bytes, filling Game's "
              "gap from +0x23508 to the transitionTarget at +0x2E170.");

static_assert(offsetof(AchievementsMenu, coldStartFlag)     == 0x0000);
static_assert(offsetof(AchievementsMenu, visible)           == 0x0001);
static_assert(offsetof(AchievementsMenu, closeRequested)    == 0x0002);
static_assert(offsetof(AchievementsMenu, chromeA)           == 0x0008);
static_assert(offsetof(AchievementsMenu, backButton)        == 0x01B8);
static_assert(offsetof(AchievementsMenu, backButtonOverlay) == 0x0290);
static_assert(offsetof(AchievementsMenu, pressed)           == 0x0368);
static_assert(offsetof(AchievementsMenu, tileBgLabel)       == 0x0370);
static_assert(offsetof(AchievementsMenu, lockedDeco)        == 0x0408);
static_assert(offsetof(AchievementsMenu, unlockedHighlight) == 0x04E0);
static_assert(offsetof(AchievementsMenu, unlockedDeco)      == 0x05B8);
static_assert(offsetof(AchievementsMenu, scrollDragging)    == 0x0690);
static_assert(offsetof(AchievementsMenu, scrollY)           == 0x06A4);
static_assert(offsetof(AchievementsMenu, scrollResidual)    == 0x06A8);
static_assert(offsetof(AchievementsMenu, tiles)             == 0x06B0);
static_assert(offsetof(AchievementsMenu, sortedDisplay)     == 0xAC50);

static_assert(offsetof(AchievementsMenu::Tile, title)        == 0x000);
static_assert(offsetof(AchievementsMenu::Tile, description)  == 0x088);
static_assert(offsetof(AchievementsMenu::Tile, icon)         == 0x110);
static_assert(offsetof(AchievementsMenu::Tile, progressBar)  == 0x1E8);
static_assert(offsetof(AchievementsMenu::Tile, progressText) == 0x2C0);
static_assert(offsetof(AchievementsMenu::Tile, locked)       == 0x348);
static_assert(offsetof(AchievementsMenu::Tile, fadeIn)       == 0x34C);
