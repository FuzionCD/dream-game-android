#pragma once

#include "color_tint.h"
#include "label.h"
#include "quad.h"
#include "text_item.h"
#include "tile_content.h"
#include "title_menu.h"   // TileIcon (for the per-quad helper accessor)
#include <cstddef>
#include <cstdint>
#include <vector>

class DetailPanel;
class Item;
class PlayerSystem;

// reconstructed from Ghidra:
//   ctor:    FUN_100009a00
//   draw:    FUN_10000a3dc
//   update:  FUN_10000a594
//   open:    FUN_10000a9f4
//   helper:  FUN_10000a1d4, adds 9 glyphs (3x3 atlas slice) to a Label
//
// UserStatsPanel lives at GameBoard+0xA3F8 (0xEB0 bytes total). it's the
// in-game read-only "your stats" overlay opened when the player taps the
// top-left HUD button (buttonFrame1 / publishedState == 1).
//
// despite the visible/posX/posY prefix matching DialogPanel exactly, this
// is not a Menu subclass and does not share a base class with the other
// panels, verified by walking FUN_100009a00 (ctor) and confirming it
// never calls FUN_100057424 (Menu::init), never wires a vtable, and the
// fade state at +0x01C is a single float (vs. Menu's secondaryVisible +
// animTimer0 pair at +0x014/+0x018). the matching prefix is a project-
// wide idiom, not inheritance.
//
// chrome composition:
//   +0x020  bgQuad           full-screen black backdrop (alpha-faded)
//   +0x0F8  headerLabel      9-slice frame across the top
//   +0x190  entries[0..2]    stat row (hit-quad + icon + 2 chrome frames
//                            + digit tint + descPtr), stride 0x3C0
//   +0xCD0  statsChromeLabel 9-slice frame for the bottom stats line
//   +0xD68  statsText        sprintf "World: %d Level: %d Items: %d"
//   +0xDF0  perksLabel       9-slice frame for the perks strip
//   +0xE88..+0xEA7           paired pointer + Vec2 vector for the perks
//                            strip (binary stores &playerSystem.perks at
//                            +0xE88 so the strip walks the live perk
//                            vector and renders each one via Perk::drawAt)
//
// the 3 entry rows surface the player's three primary stat slots from
// PlayerSystem at +0x14c/+0x150/+0x154 (the "displayed" values), with
// per-stat ranges read from +0x158..+0x16C and description text pointers
// at +0x170/+0x178/+0x180. each row is a hit-test Quad over a TileContent
// icon (typed 2 = ATK, 6 = HP, 3 = DEF, verified by FUN_1000147d4 calls)
// with a 9-slice description frame to the right.
//
// touch hit-tests inside the rows fan out to populate the panel-owned
// DetailPanel (pointer stored at +0x10 by the ctor) with the per-slot
// description; tapping the bottom-stats area fills it with the fixed
// "This shows the world you're in / how many levels you've gained / how
// many items you've found" copy.

// per-entry sub-layout, repeated 3 times at +0x190 with stride 0x3C0.
// math: Quad 0xD8 + TileContent 0x178 + Label 0x98 + ColorTint 0x38
//     + Label 0x98 + void* 8 = 0x3C0 exactly. no trailing pad.
struct UserStatsEntry {
    Quad        hitTestQuad;   // +0x000  hit-test target. all 3 rows share UV.
    TileContent statIcon;      // +0x0D8  the stat icon (type 2 / 6 / 3).
    Label       chromeFrame;   // +0x250  9-slice frame around the stat number.
    ColorTint   statTint;      // +0x2E8  digit display for the stat value.
    Label       descFrame;     // +0x320  9-slice frame for the description.
    void*       descPtr;       // +0x3B8  raw pointer to the underlying Item
                               //         (= playerSystem.baseItems[i]).
                               //         touch hits pass it to DetailPanel's
                               //         populators via FUN_100040124 /
                               //         FUN_10003f2c0 / FUN_10003f6f8.
};
static_assert(sizeof(UserStatsEntry) == 0x3C0,
              "UserStatsEntry must be exactly 0x3C0 bytes per row stride");

class UserStatsPanel {
public:
    // FUN_100009a00. zero state + lay out backdrop / header / 3 entries /
    // bottom-stats / items section. detailPanel pointer is captured here so
    // touch hits can populate it without re-plumbing a Game* arg every frame.
    void init(DetailPanel* detailPanelPtr);

    // FUN_10000a9f4. populate the 3 entry rows from playerSystem, format the
    // bottom-stats text from `worldStatsBlock` (= GameBoard+0x20, the
    // World/Level/Items counter triple), prime the fade-in timer, play
    // sound 8 (menuAppear). called from GameBoard::dispatchHexAndRackTouch
    // when HUD publishedState becomes 1.
    void open(PlayerSystem& playerSystem, const int* worldStatsBlock);

    // FUN_10000a594. per-frame tick. while the fade-in is mid-progress
    // (fadeTimer < 1.0) it advances the timer + alpha. once stable, it
    // hit-tests touches against the 3 entry rows, the bottom-stats zone
    // and the per-item icons; matching hits dispatch DetailPanel
    // populators (`FUN_100040124` / `FUN_10003f2c0` / `FUN_10003f6f8`).
    // touches outside any zone, or any tap on a release frame, dismiss
    // the panel via fadeTimer reset + visible = false.
    void update(float dt);

    // programmatic dismiss (used by the Android back button). mirrors the
    // touch-outside close inside update(): fade out, drop any inspect card,
    // play the close beep. no binary equivalent (iOS has no back button).
    void requestClose();

    // FUN_10000a3dc. early-out on !visible; otherwise push matrix,
    // translate to (posX, posY), draw backdrop + header + 3 rows +
    // bottom stats + items list under tex 9. matches the binary's exact
    // draw order so overlay compositing lines up.
    void draw();

    // ---- byte-exact struct fields (total 0xEB0 bytes) ----

    bool        visible;            // +0x000
    uint8_t     pad001[3];          // +0x001..+0x003
    float       posX;               // +0x004  pixel-snapped header X
    float       posY;               // +0x008  pixel-snapped header Y
    uint8_t     pad00C[4];          // +0x00C..+0x00F  align to 8 for ptr
    DetailPanel* detailPanel;       // +0x010  detail-panel pointee for hit dispatch
    bool        active;             // +0x018  gate that goes high on open()
                                    //         and stays until the fade-out
                                    //         completes inside update().
    uint8_t     pad019[3];          // +0x019..+0x01B
    float       fadeTimer;          // +0x01C  fade-in ramp 0..1; once >= 1.0
                                    //         update switches to interactive
                                    //         hit-test mode.
    Quad        bgQuad;             // +0x020  full-screen black backdrop with
                                    //         alpha modulated by fadeTimer.
                                    //         ctor sets size to (1.0, virtual
                                    //         height) and color 0xFF000000.
    Label       headerLabel;        // +0x0F8  9-slice frame at top of the
                                    //         panel content area.
    UserStatsEntry entries[3];      // +0x190..+0xCCF  stride 0x3C0
    Label       statsChromeLabel;   // +0xCD0  9-slice frame for the bottom-
                                    //         stats text.
    TextItem    statsText;          // +0xD68  "World: %d  Level: %d  Items: %d"
    Label       perksLabel;         // +0xDF0  9-slice frame for the additional-
                                    //         perks strip at the bottom of
                                    //         the panel.
    PlayerSystem* playerSystemPtr;  // +0xE88  set to &playerSystem so update +
                                    //         draw walk playerSystem.perks live
                                    //         (via Perk::drawAt) without copying.
    std::vector<float> perkCoords;  // +0xE90..+0xEA7  (libc++ vector head 24B)
                                    //         per-perk display positions; perk
                                    //         `i` occupies coords[2*i .. 2*i+1].
                                    //         grown by reserve/push_back inside
                                    //         open() so vec size matches
                                    //         playerSystem.perks.size().
    uint8_t     drawAlpha;          // +0xEA8  current drawing alpha byte:
                                    //         output of the cosine-eased fade
                                    //         curve, consumed by draw() and
                                    //         the per-item Perk::drawAt calls.
    uint8_t     padEA9[7];          // +0xEA9..+0xEAF  pad to 0xEB0
};

static_assert(sizeof(UserStatsPanel) == 0xEB0,
              "UserStatsPanel must be exactly 0xEB0 bytes (0xA3F8..0xB2A8 slab)");