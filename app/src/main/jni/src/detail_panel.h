#pragma once

#include "color_tint.h"
#include "quad.h"
#include "text_item.h"
#include "title_menu.h"   // for TileIcon
#include <cstddef>
#include <cstdint>

class Perk;

// reconstructed from Ghidra:
//   constructor: FUN_10003e5e0
//   draw:        FUN_10003ef28
//   update:      FUN_10003f0e4
//
// DetailPanel lives at GameBoard+0x4408 (0x10B0 bytes). it's the "tile inspect"
// popup that appears when the player taps a tile or item to read its name,
// description, and effect. has 6 display modes (switch on +0x000):
//   mode 0: standard tile/item card (frames, optional bonus quad, 2 tints)
//   mode 1: simple icon overlay (tex 9, sheet9)
//   mode 2: simple icon overlay (tex 11, items1)
//   mode 3: Perk preview (forwards to Perk::drawAt via perkSource at +0x1078)
//   mode 4: outer frame only (tex 9)
//   mode 5: same as mode 1
//
// `subMode` (+0x010, the binary's `param_1[4]`) gates two pairs of frame quads:
//   subMode == 1 -> draw frames[2] and frames[3]  (first decoration variant)
//   subMode == 2 -> draw frames[10] and frames[11] (second decoration variant)
//
// the panel slides in/out via fadeT + start/target pos: when interpolatePosition
// is set, posX/posY lerp from start to target as fadeT advances 0..1.

// forward declarations for populator inputs
class TileContent;
class SnagContent;
class Item;
class EventSlot;

class DetailPanel {
public:
    // FUN_10003e5e0
    void init();
    // FUN_10003ef28
    void draw();
    // FUN_10003f0e4
    void update(float dt);

    // FUN_1000404b0, reset / dismiss. early-out if already invisible. when
    // visible: clear visible, clear interpolatePosition, set fadeT = 0 or 1
    // depending on hardReset, then run a single update tick at dt=0 to apply.
    // hardReset = 0 -> fadeT = 0 (let it animate out).
    // hardReset = 1 -> fadeT = 1 (snap closed instantly).
    void reset(int hardReset);

    // ----- populators -----
    // each populator sets `mode`, configures the icon/optional quad UV+size,
    // forwards the title + 3 desc lines to layoutAndFade(), and runs a single
    // update(0) tick. anchor[0]/anchor[1] is the tap point in virtual coords.

    // FUN_10003fb44, content tile (mode 1). looks up name/desc from the
    // content text table by content->type. callers resolve the active
    // TileContent ptr via TileObject::getTileContentIfAlive() first.
    void populateForContent(float headerY, const float* anchor,
                            TileContent* content);

    // FUN_10003fc3c, HexMap cell (mode 5). looks up name/desc from the
    // hex-map text table by cellType (1..7).
    void populateForHexMapCell(float headerY, const float* anchor, uint32_t cellType);

    // FUN_10003fd34, Nemesis (mode 6). title + 3 lines are sprintf'd inline.
    void populateForNemesis(float headerY, const float* anchor);

    // FUN_10003f2c0, Item card (mode 2). Item gear inspect. icon UV comes
    // from ICON_RECTS[type][subType] (same silhouette atlas as the in-game
    // Item warnLine1); title is Item::getName, desc lines are Item::getDescription
    // Line(0..2). called when the user taps a held/candidate slot in the
    // ItemChoicePanel.
    void populateForItem(float headerY, const float* anchor, Item* item);

    // FUN_100040124, mode 1 with caller-supplied strings + optional content
    // icon UV. used by UserStatsPanel's per-row hit-tests (each row's chrome
    // and hit-test Quad dispatch hard-coded "Attack Range" / "Heal Range" /
    // etc. strings); also used for the bottom-stats hit with contentType=0
    // and no icon. contentType 0 zeros the iconQuad; nonzero looks up the
    // tile UV via the same path populateForContent uses.
    void populateForStatRow(float headerY, const float* anchor,
                            uint32_t contentType,
                            const char* name,
                            const char* desc0,
                            const char* desc1,
                            const char* desc2);

    // FUN_10003f6f8, mode 3 (Perk preview). zeroes iconQuad (since draw()
    // forwards to perkSource->drawAt instead), stores the active Perk*,
    // and threads through the same layoutAndFade tail with the Perk's
    // name + 3 description lines.
    void populateForPerk(float headerY, const float* anchor, Perk* perk);

    // FUN_10003f7f8, mode 4 (Event card). title + 3 desc lines from the
    // Event, then an outerFrame sub-icon whose UV/size/position is chosen by
    // the Event kind (0..4) and anchored off frames[4] (the top-right corner
    // layoutAndFade just placed). driven by GameplayHUD's event-tray inspect.
    void populateForEvent(float headerY, const float* anchor, EventSlot* slot);

    // FUN_10003fe64, snag (mode 0). looks up name/desc from snag_table.h.
    // playerDeathTurns / snagDeathTurns are the combat-simulation results from
    // FUN_100023f84 (how many turns the player vs the snag wins in). render
    // as side-stat digits flanking combatSimPreview when extraFlag != 0.
    // callers resolve the active SnagContent ptr via getSnagIfAlive() first.
    void populateForSnag(float headerY, const float* anchor, SnagContent* snag,
                         int playerWinTurns, int snagWinTurns, uint8_t extraFlag);

    // shared core (FUN_10003f3b8), writes title + 3 lines, picks scale for
    // tall vs wide aspect, places posX/posY (with optional lerp from current).
    // mode5Bit = 1 toggles a wider title-bar metric (used by mode 1/5).
    // optionalQuadFlag = title != null (drives combatSimPreview enable).
    void layoutAndFade(float headerY, const float* anchor, uint8_t inhibitOpt,
                       uint32_t mode5Bit, const char* title,
                       const char* desc0, const char* desc1, const char* desc2);

    // FUN_100040234, second-pass frame placement called by layoutAndFade
    // once panel posX/posY are settled. positions every frame Quad to draw
    // the 9-slice border + title-bar half-strips around the title icon.
    void applyLayoutGeometry(float titleWidth, float panelHeight,
                             float titleXOffset, int orientation);

    // --- byte-exact struct fields ---

    int32_t mode;                  // +0x000  display mode (0..5)
    bool    visible;               // +0x004  drawn when set or fadeT < 1
    uint8_t pad005[3];             // +0x005

    float   posX;                  // +0x008  current panel position
    float   posY;                  // +0x00C
    int32_t subMode;               // +0x010  0/1/2; gates frame pairs
    uint8_t pad014[4];             // +0x014

    // 13 frame TileIcons (the panel's static decorative pieces).
    // pairs at indices 2/3 and 10/11 are gated by `subMode`.
    TileIcon frames[13];           // +0x018..+0xB0F  (13 * 0xD8 = 0xAF8)

    // mode 0 icon texture index (sheet9 / items1 / etc.)
    int32_t iconTexIndex;          // +0xB10
    uint8_t padB14[4];             // +0xB14

    TileIcon iconQuad;             // +0xB18  drawn in modes 1, 2, 5
    TileIcon outerFrame;           // +0xBF0  drawn in modes 0, 4

    ColorTint tintFrame;           // +0xCC8

    // 4 text items: textCenter is the "title" rendered in the middle of the
    // panel, textLines[0..2] are 3 description lines below.
    TextItem textCenter;           // +0xD00
    TextItem textLines[3];         // +0xD88..+0xF1F

    // hasTitle is consumed by layoutAndFade only; it tweaks the layout
    // metrics based on whether the populator passed a non-null title.
    // it does not gate any draw call (the binary's draw() ignores it).
    bool hasTitle;                 // +0xF20  init = 1
    uint8_t padF21[7];             // +0xF21

    // mode 0 (snag) extras. iOS draws the combatSimPreview + snagDeathTurns[Alt]
    // (the "combat preview" hex tile next to the snag icon) only when
    // snagPreviewEnabled is set; the snag populator passes its param_7
    // through to this byte. all other modes leave it clear.
    TileIcon combatSimPreview;     // +0xF28

    ColorTint snagDeathTurns;        // +0x1000
    ColorTint playerDeathTurns;      // +0x1038

    bool    snagPreviewEnabled;    // +0x1070  init = 0
    uint8_t pad1071[7];            // +0x1071
    // mode 3 (Perk preview) source pointer. populateForPerk
    // (FUN_10003f6f8, called from UserStatsPanel::update's perks-strip tap)
    // writes the active Perk* here; draw() forwards to Perk::drawAt to render
    // the perk icon + tint inside the preview window.
    Perk*   perkSource;            // +0x1078  init = nullptr

    uint8_t currentAlpha;          // +0x1080  per-frame alpha (set by update)
    uint8_t pad1081[3];            // +0x1081
    float   fadeT;                 // +0x1084  init = 1.0; 0..1 fade timer

    // when interpolatePosition is set, posX/posY lerp from start to target
    // by fadeT. otherwise posX/posY are author-controlled.
    float   startPosX;             // +0x1088
    float   startPosY;             // +0x108C
    float   targetPosX;            // +0x1090
    float   targetPosY;            // +0x1094

    bool    interpolatePosition;   // +0x1098  init = 0
    uint8_t pad1099[7];            // +0x1099..+0x109F
    int     touchHoldArea;         // +0x10A0  (0 none, 1 rack, 2 board); set when
                                   //          the inspect panel opens, read by the
                                   //          idle-tick to pick the auto-close rule.
    float   touchOrigX;            // +0x10A4  (hold origin; area 2 / board only)
    float   touchOrigY;            // +0x10A8
    uint8_t pad10AC[4];            // +0x10AC..+0x10AF
};
static_assert(sizeof(DetailPanel) == 0x10B0, "DetailPanel must be exactly 0x10B0 bytes");
