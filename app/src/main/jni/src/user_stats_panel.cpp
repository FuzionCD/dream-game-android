#include "user_stats_panel.h"

#include "detail_panel.h"
#include "game.h"
#include "item.h"
#include "perk.h"
#include "player_system.h"
#include "renderer.h"   // bindTexture, Renderer::getVirtualHeight
#include "tile_object.h"

#include <GLES/gl.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// ---- ctor constants extracted from DAT_100059c40..c9c via Mach-O reader ----
//
// entries' per-row layout. each row's Quad center is at
//   (xBase + col * 125/640, yRow + 100/640)
// hit-test Quad and entry icon are (0.2, 0.2) atlas-derived UV.

constexpr float kEntryColXStride       = 125.0f;   // DAT_100059c40
constexpr float kEntryRowHeight        = 100.0f;   // DAT_100059c44
constexpr float kPixelToPanel          = 640.0f;   // DAT_100059c48
constexpr float kEntryFineXOffset      = 0.0015625f;  // DAT_100059c4c
constexpr float kEntryFineYOffset      = 0.003125f;   // DAT_100059c50
constexpr float kEntryFineLargerY      = 0.00625f;    // DAT_100059c54
constexpr float kEntryDescChromeWidth  = 0.2f;        // DAT_100059c58
constexpr float kEntryNudgeY           = 0.09375f;    // DAT_100059c5c
constexpr float kEntryDescBaselineY    = -0.0046875f; // DAT_100059c60
constexpr float kStatsChromeWidth      = 0.590625f;   // DAT_100059c64
constexpr float kRowSplitMin           = -0.0546875f; // DAT_100059c68
constexpr float kRowSplitMax           =  0.0546875f; // DAT_100059c6c
constexpr float kFadeRate              = 0.3f;        // DAT_100059c70  (open/close speed)
constexpr float kFadeCosPi             = 3.1415927f;  // DAT_100059c74
constexpr float kAlphaMax              = 255.0f;      // DAT_100059c78
constexpr float kBaseYRow1             = 0.0765625f;  // DAT_100059c7c
constexpr float kBaseYStride           = 70.0f;       // DAT_100059c80
constexpr float kBaseYDenom            = 640.0f;      // DAT_100059c84
constexpr float kStatsExtraY           = 0.04375f;    // DAT_100059c88
constexpr float kStatsExtraGap         = 0.0578125f;  // DAT_100059c8c
constexpr float kEmptyVecYBase         = 0.0625f;     // DAT_100059c90
constexpr float kHeaderChromeWidth     = 0.70625f;    // DAT_100059c94
constexpr float kHeaderTopOffset       = 0.0015625f;  // DAT_100059c98
constexpr float kStatsLabelDip         = -0.003125f;  // DAT_100059c9c

// per-entry icon types from the ctor's i==0/1/2 dispatch. confirmed
// against PlayerSystem field reads in open(): entry 0 = ATK (baseATK),
// entry 1 = HP (baseHP), entry 2 = DEF (baseDEF). TileContent type IDs:
//   2 = ATK, 6 = HP, 3 = DEF (per project_dream_tile_properties memory).
constexpr uint32_t kEntryIconType_ATK = 2;
constexpr uint32_t kEntryIconType_HP  = 6;
constexpr uint32_t kEntryIconType_DEF = 3;

// per-entry color tint applied to the ColorTint digits (entry hue).
//   entry 0 (ATK): 0xff0fe628  green
//   entry 1 (HP) : 0xff005af0  blue
//   entry 2 (DEF): 0xffe68214  orange
constexpr uint32_t kEntryTintRgba[3] = { 0xff0fe628u, 0xff005af0u, 0xffe68214u };

// per-row description string tables, extracted from the binary's
// PTR_s_Possible_range_of_values_of_draw_1000767e8 (chromeFrame hits) and
// PTR_s_Determines_the_value_of__A__tile_100076770 (hitTestQuad hits) via
// the Mach-O reader. each entry maps to one of the 3 stat rows.
struct StatRowStrings {
    uint32_t    contentType;
    const char* name;
    const char* desc0;
    const char* desc1;
    const char* desc2;
};

// chromeFrame strings: "range" descriptions for the digit displays.
constexpr StatRowStrings kChromeRowStrings[3] = {
    { 2, "Attack Range",
      "Possible range of values of drawn {A} tiles.",
      "This range is defined by your {A} stat and",
      "the sum of {A} values of your items." },
    { 6, "Heal Range",
      "Possible range of values of drawn {H} tiles.",
      "This range is defined by your {H} stat and",
      "the sum of {H} values of your items." },
    { 3, "Defence Range",
      "Possible range of values of drawn {D} tiles.",
      "This range is defined by your {D} stat and",
      "the sum of {D} values of your items." },
};

// hitTestQuad strings: "what this stat icon means" descriptions.
constexpr StatRowStrings kHitRowStrings[3] = {
    { 2, "Attack",
      "Determines the value of {A} tiles.",
      "", "" },
    { 6, "Health",
      "Determines the value of {H} tiles.",
      "Each point increases your maximum {H} by 5.",
      "" },
    { 3, "Defence",
      "Determines the value of {D} tiles.",
      "", "" },
};

// per-perk hit tolerance: touch must land within 0.0546875 of the stored
// perk coord on both axes. matches DAT_100059c68 / DAT_100059c6c (the
// kRowSplit constants used at the binary's hit-test).
constexpr float kPerkHitHalfWidth = 0.0546875f;   // DAT_100059c6c

// FUN_100057374, pixel-snap to a 1/640 grid. same algorithm used by
// detail_panel.cpp / color_tint.cpp / etc.; copied locally so we don't
// pull in DetailPanel's static helper.
inline float pixelSnap640(float v) {
    constexpr float ref = 640.0f;
    float scaled = v * ref + (v < 0.0f ? -0.5f : 0.5f);
    return (float)(int)scaled / ref;
}

// FUN_10000aefc, paired-return helper that gives the center point of a
// Label as (centerX, centerY) from getLeftX + getWidth (both paired-return
// pairs themselves: getLeftX -> (leftX, topY); getWidth -> (width, height)).
struct Vec2 { float x; float y; };
inline Vec2 labelCenter(Label& label) {
    return { label.getLeftX() + label.getWidth()  * 0.5f,
             label.topY       + label.getHeight() * 0.5f };
}

// FUN_10000a1d4. add a single 9-slice frame (3 rows of 3 glyphs) to the
// caller's Label. atlas UVs sit at y=677, y=701, y=702 in pixel space.
// each row bumps pendingLineIndex so layoutGlyphs treats them as
// independent rows.
void addNineSliceFrame(Label* label) {
    GlyphOffset zero = { 0.0f, 0.0f };

    // top row (y=677): corner, edge, corner
    float uv0_origin[2] = { 70.0f, 677.0f };  float uv0_size[2] = { 24.0f, 24.0f };
    label->addGlyph(-1.0f, uv0_origin, uv0_size, 2u, zero);

    float uv1_origin[2] = { 94.0f, 677.0f };  float uv1_size[2] = {  1.0f, 24.0f };
    label->addGlyph(-1.0f, uv1_origin, uv1_size, 1u, zero);

    float uv2_origin[2] = { 95.0f, 677.0f };  float uv2_size[2] = { 24.0f, 24.0f };
    label->addGlyph(-1.0f, uv2_origin, uv2_size, 2u, zero);
    label->pendingLineIndex += 1;

    // middle row (y=701): edge, center, edge; heights are 1px stretchable
    float uv3_origin[2] = { 70.0f, 701.0f };  float uv3_size[2] = { 24.0f, 1.0f };
    label->addGlyph(-1.0f, uv3_origin, uv3_size, 0u, zero);

    float uv4_origin[2] = { 94.0f, 701.0f };  float uv4_size[2] = {  1.0f, 1.0f };
    label->addGlyph(-1.0f, uv4_origin, uv4_size, 3u, zero);

    float uv5_origin[2] = { 95.0f, 701.0f };  float uv5_size[2] = { 24.0f, 1.0f };
    label->addGlyph(-1.0f, uv5_origin, uv5_size, 0u, zero);
    label->pendingLineIndex += 1;

    // bottom row (y=702): corner, edge, corner
    float uv6_origin[2] = { 70.0f, 702.0f };  float uv6_size[2] = { 24.0f, 24.0f };
    label->addGlyph(-1.0f, uv6_origin, uv6_size, 2u, zero);

    float uv7_origin[2] = { 94.0f, 702.0f };  float uv7_size[2] = {  1.0f, 24.0f };
    label->addGlyph(-1.0f, uv7_origin, uv7_size, 1u, zero);

    float uv8_origin[2] = { 95.0f, 702.0f };  float uv8_size[2] = { 24.0f, 24.0f };
    label->addGlyph(-1.0f, uv8_origin, uv8_size, 2u, zero);
}

} // anonymous namespace

// reconstructed from Ghidra FUN_100009a00.
void UserStatsPanel::init(DetailPanel* detailPanelPtr) {

    // zero core state. value-init has already cleared every field but the
    // binary explicitly assigns these too, so we match that.
    visible    = false;
    detailPanel = detailPanelPtr;

    // ---- backdrop quad init (bgQuad) ----
    bgQuad = Quad();
    headerLabel.init();

    // per-entry ctor walk (binary loops lVar19 from -0xB40 to 0 in 0x3C0
    // strides; the 3 entries are laid out backward but stored forward).
    for (int i = 0; i < 3; ++i) {
        UserStatsEntry& e = entries[i];
        e.hitTestQuad = Quad();
        e.statIcon    = TileContent();
        e.chromeFrame.init();
        e.statTint.init();
        e.descFrame.init();
    }

    statsChromeLabel.init();

    // FUN_10002fa50, TextItem init with the panel-font glyph table.
    Game* g = getGame();
    const BMFontTable* panelFont = g ? g->bmfontTablePtr(0) : nullptr;
    statsText.init(panelFont);

    perksLabel.init();

    playerSystemPtr = nullptr;
    // perkCoords default-constructed empty (begin = end = cap = nullptr).

    // backdrop size + color. width = 1.0 screen units, height = virtual
    // screen height (~1.5 on iOS, ~2.2 on Android), color black, full alpha.
    const float virtualHeight = Renderer::getVirtualHeight();
    bgQuad.posX = 0.5f;
    bgQuad.posY = virtualHeight * 0.5f;
    bgQuad.setSize(1.0f, virtualHeight);
    bgQuad.setColor(0x00, 0x00, 0x00, 0xFF);

    // ---- header label 9-slice (3 glyphs: top + middle + bottom strips) ----
    // unlike the per-entry chrome which uses addNineSliceFrame (full 3x3),
    // the header is laid out as 3 horizontal strips spanning the panel width.
    {
        GlyphOffset zero = { 0.0f, 0.0f };

        float uv0_origin[2] = { 572.0f, 81.0f };
        float uv0_size[2]   = { 452.0f, 100.0f };
        headerLabel.addGlyph(-1.0f, uv0_origin, uv0_size, 2u, zero);
        headerLabel.pendingLineIndex += 1;

        float uv1_origin[2] = { 572.0f, 184.0f };
        float uv1_size[2]   = { 452.0f,   1.0f };
        headerLabel.addGlyph(-1.0f, uv1_origin, uv1_size, 0u, zero);
        headerLabel.pendingLineIndex += 1;

        float uv2_origin[2] = { 572.0f, 185.0f };
        float uv2_size[2]   = { 452.0f,  60.0f };
        headerLabel.addGlyph(-1.0f, uv2_origin, uv2_size, 2u, zero);
    }

    // ---- per-entry chrome positioning + color + icon type ----
    // per-entry tables (extracted from FUN_100009a00 asm at
    // 0x100009cdc..0x100009ddc):
    //   entry 0 (ATK): TileContent type 2, tint 0xff0fe628,
    //                  icon xOff = 0,        yOff = 0.003125 (DAT_c50)
    //   entry 1 (HP) : TileContent type 6, tint 0xff005af0,
    //                  icon xOff = 0,        yOff = 0.00625  (DAT_c54)
    //   entry 2 (DEF): TileContent type 3, tint 0xffe68214,
    //                  icon xOff = 0.001562, yOff = 0.003125 (DAT_c4c, c50)
    struct EntrySetup {
        uint32_t iconType;
        uint32_t tintRgba;
        float    iconXOff;
        float    iconYOff;
    };
    constexpr EntrySetup kEntrySetup[3] = {
        { kEntryIconType_ATK, kEntryTintRgba[0], 0.0f,                kEntryFineYOffset    },
        { kEntryIconType_HP,  kEntryTintRgba[1], 0.0f,                kEntryFineLargerY    },
        { kEntryIconType_DEF, kEntryTintRgba[2], kEntryFineXOffset,   kEntryFineYOffset    },
    };

    for (int i = 0; i < 3; ++i) {
        UserStatsEntry& e = entries[i];

        // hit-test Quad: 0.2 x 0.2. UV (0.18847656, 0.1875) -> (0.31347656,
        // 0.3125), the panel-frame swatch in ui1.png. extracted from
        // FUN_100009a00.
        e.hitTestQuad.setTexCoords(0.18847656f, 0.1875f,
                                   0.31347656f, 0.3125f);
        e.hitTestQuad.setSize(0.2f, 0.2f);
        // posX = (i * 125 + 100) / 640. uses DAT_100059c44 = 100, not 70
        // (DAT_100059c80).
        e.hitTestQuad.posX = ((float)i * kEntryColXStride + kEntryRowHeight)
                             / kPixelToPanel;
        e.hitTestQuad.posY = 0.15625f;

        // stat icon: TileContent typed per entry slot (ATK / HP / DEF).
        e.statIcon.setType(kEntrySetup[i].iconType);

        // entry color tint. seed only; open() replaces it with the
        // setRangeDisplay call when stats are populated.
        const uint32_t rgba = kEntrySetup[i].tintRgba;
        e.statTint.setColor((uint8_t)(rgba >> 0),
                            (uint8_t)(rgba >> 8),
                            (uint8_t)(rgba >> 16));
        e.statTint.setAlpha((uint8_t)(rgba >> 24));

        // statIcon position: per-entry fine offset added to hitTestQuad pos.
        const float ix = e.hitTestQuad.posX + kEntrySetup[i].iconXOff;
        const float iy = e.hitTestQuad.posY + kEntrySetup[i].iconYOff;
        e.statIcon.setPosition(ix, iy);

        // chromeFrame: 9-slice around the digits. position relative to the
        // hit-test Quad: leftEdge for X, bottomEdge + DAT_c60 for Y.
        addNineSliceFrame(&e.chromeFrame);
        e.chromeFrame.setSize(kEntryDescChromeWidth, kEntryNudgeY);
        e.chromeFrame.setPosition(
            e.hitTestQuad.posX - e.hitTestQuad.width * 0.5f,
            e.hitTestQuad.posY + e.hitTestQuad.height * 0.5f + kEntryDescBaselineY);

        // descFrame: 9-slice around the description. (0.2, 0.2) square,
        // positioned just below chromeFrame.
        addNineSliceFrame(&e.descFrame);
        e.descFrame.setSize(kEntryDescChromeWidth, kEntryDescChromeWidth);
        const float descX = e.chromeFrame.getLeftX();
        const float descY = e.chromeFrame.topY
                          + e.chromeFrame.getHeight()
                          + kEntryDescBaselineY;
        e.descFrame.setPosition(descX, descY);

        e.descPtr = nullptr;
    }

    // ---- bottom-stats Label + TextItem ----
    // statsChromeLabel sits below entry-0's descFrame, not chromeFrame.
    // binary at FUN_100009a00 line 100009ee8 reads descFrame.
    addNineSliceFrame(&statsChromeLabel);
    statsChromeLabel.setSize(kStatsChromeWidth, kEntryNudgeY);
    {
        const float sx = entries[0].descFrame.getLeftX();
        const float sy = entries[0].descFrame.topY
                       + entries[0].descFrame.getHeight()
                       + kEntryDescBaselineY;
        statsChromeLabel.setPosition(sx, sy);
    }

    // statsText: font scale 0.07, color RGBA (0xB4, 0xAA, 0xB4, 0xFF). our
    // packed-word rgba + applyColor (FUN_100030144) lands bit-identical to
    // the binary's per-byte color writes.
    statsText.scaleX = 0.0699999928474426f;
    statsText.scaleY = 0.0699999928474426f;
    statsText.rgba   = 0xFFB4AAB4u;
    statsText.applyColor();

    // ---- perks strip Label ----
    // perksLabel.setSize uses kStatsChromeWidth (0.590625) for width, not
    // 0.15625. height starts at 0.15625 but open() overrides this once it
    // knows how many rows the player's perks will need. position anchors
    // to entry-0's descFrame leftX and statsChromeLabel's bottom edge.
    addNineSliceFrame(&perksLabel);
    perksLabel.setSize(kStatsChromeWidth, 0.15625f);
    {
        const float ix = entries[0].descFrame.getLeftX();
        const float iy = statsChromeLabel.topY
                       + statsChromeLabel.getHeight()
                       + kEntryDescBaselineY;
        perksLabel.setPosition(ix, iy);
    }

    drawAlpha = 0;
}

// reconstructed from Ghidra FUN_10000a9f4. open the panel and lay out
// every dynamic-sized element (headerLabel size, panel posX/posY pixel-
// snap, per-perk grid coords, perksLabel size update, statsText position).
void UserStatsPanel::open(PlayerSystem& playerSystem,
                          const int*    worldStatsBlock) {

    visible   = true;
    active    = true;
    fadeTimer = 0.0f;

    // hold the live PlayerSystem so the per-perk render walks playerSystem.perks
    // directly (via Perk::drawAt) without copying the vector.
    playerSystemPtr = &playerSystem;

    // entry-row populate. binary reads three stat indices from playerSystem:
    //   entry 0: baseATK + statRanges[0] + baseItems[0]
    //   entry 1: baseHP  + statRanges[2] + baseItems[1]
    //   entry 2: baseDEF + statRanges[1] + baseItems[2]
    // statRanges[] is laid out [ATK, DEF, HP] and the panel walks it
    // out-of-order to match the [ATK, HP, DEF] entry row order.
    struct EntryBinding {
        int32_t        magnitude;
        int32_t        rangeLo;
        int32_t        rangeHi;
        const Item*    baseItem;
    };

    const EntryBinding bindings[3] = {
        { playerSystem.baseATK, playerSystem.statRanges[0].lo,
          playerSystem.statRanges[0].hi, playerSystem.baseItems[0] },
        { playerSystem.baseHP,  playerSystem.statRanges[2].lo,
          playerSystem.statRanges[2].hi, playerSystem.baseItems[1] },
        { playerSystem.baseDEF, playerSystem.statRanges[1].lo,
          playerSystem.statRanges[1].hi, playerSystem.baseItems[2] },
    };

    // ---- perks grid layout ----
    // perkCoords is a list of (x, y) Vec2 entries, one per owned perk. each
    // row holds up to 5 perks centered horizontally; the grid origin anchors
    // to perksLabel's center + a vertical offset.
    //
    // dynamicHeight (panel content height contribution from the perks strip)
    // and posYInterp (panel posY interpolation factor) feed headerLabel
    // sizing and panel positioning below.
    float dynamicHeight = 0.0f;
    float posYInterp    = 0.0f;
    const size_t perkCount = playerSystem.perks.size();

    if (perkCount > 0) {
        const int numRows = (int)((perkCount + 4) / 5);  // ceil(perkCount / 5)

        // center anchor of the perks strip: (perksLabel.centerX,
        // perksLabel.topY + DAT_c7c).
        const Vec2 perksCenter = labelCenter(perksLabel);
        const float gridY0 = perksLabel.topY + kBaseYRow1;
        const float lastRowCount = (float)((perkCount - 1) % 5);
        const float lastRowHalfOffset = lastRowCount * 0.5f;
        constexpr float kSlotStridePx = 70.0f;   // DAT_100059c80
        constexpr float kSlotStrideDenom = 640.0f; // DAT_100059c84

        perkCoords.clear();
        perkCoords.reserve(perkCount * 2);

        for (size_t i = 0; i < perkCount; ++i) {
            const int rowIdx = (int)i / 5;
            const int rowFirstIdx = rowIdx * 5;
            const bool isLastIncompleteRow = ((int)perkCount < rowFirstIdx + 5);
            const float colCenterOff = isLastIncompleteRow
                                       ? lastRowHalfOffset
                                       : 2.0f;
            const float colInRow = (float)((int)i - rowFirstIdx) - colCenterOff;
            const float pxX = colInRow * kSlotStridePx / kSlotStrideDenom;
            const float pxY = (float)(rowIdx * 70) / kSlotStrideDenom;

            perkCoords.push_back(perksCenter.x + pxX);
            perkCoords.push_back(gridY0       + pxY);
        }

        // perksLabel.setSize keeps the current width (cachedSize0) and
        // updates height = numRows * 70/640 + DAT_c88 (0.04375).
        const float gridHeight =
            (float)numRows * kSlotStridePx / kSlotStrideDenom + kStatsExtraY;
        perksLabel.setSize(perksLabel.cachedSize0, gridHeight);

        // headerLabel content height (fVar16) anchors below the perks strip.
        // panel.posY interpolation factor scales with perks rows (clamped 1).
        const float perksBottom = perksLabel.topY + perksLabel.getHeight();
        dynamicHeight = perksBottom + kStatsExtraGap;
        posYInterp   = (float)numRows * 0.25f;
    }
    else {
        // no perks: header content extends down to perksLabel.topY +
        // DAT_c90 (0.0625). panel posY uses posYInterp = 0, fixed at 0.25.
        dynamicHeight = perksLabel.topY + kEmptyVecYBase;
        posYInterp    = 0.0f;
    }

    // ---- headerLabel size (drives panel posX/posY) ----
    // setSize(0.70625, dynamicHeight). headerLabel was glyph-loaded in
    // init() but never sized; this is the first setSize call on it.
    headerLabel.setSize(kHeaderChromeWidth, dynamicHeight);

    // ---- panel posX = pixelSnap((0.5 - hdr.width/2) + 0.0015625) ----
    {
        const float hdrW = headerLabel.getWidth();
        posX = pixelSnap640((0.5f - hdrW * 0.5f) + kHeaderTopOffset);
    }

    // ---- panel posY = pixelSnap(lerp(0.25, virtualMid - hdr.height/2,
    //                                  clamp(posYInterp, 0, 1))) ----
    {
        const float virtualHeight = Renderer::getVirtualHeight();
        const float hdrH    = headerLabel.getHeight();
        const float midDip  = virtualHeight * 0.5f - hdrH * 0.5f;
        const float t       = (posYInterp < 1.0f) ? posYInterp : 1.0f;
        const float blended = (1.0f - t) * 0.25f + t * midDip;
        posY = pixelSnap640(blended);
    }

    // ---- per-entry populate ----
    for (int i = 0; i < 3; ++i) {
        UserStatsEntry& e = entries[i];

        // setRawAndDisplayMagnitude (FUN_100014870) on the entry icon.
        e.statIcon.setRawAndDisplayMagnitude(bindings[i].magnitude);

        // setRangeDisplay (FUN_10003c6e0) on the digit tint. textStyle 0
        // (small digits) + positionMode 1 (pixel-snap) match the binary's
        // call shape.
        e.statTint.setRangeDisplay(bindings[i].rangeLo, bindings[i].rangeHi,
                                   0, 1);

        // descPtr captures the playerSystem-stored Item pointer for the
        // touch-hit DetailPanel populator.
        e.descPtr = const_cast<Item*>(bindings[i].baseItem);

        // statTint position: chromeFrame center + (0, DAT_c98 = 0.0015625).
        const Vec2 chromeCenter = labelCenter(e.chromeFrame);
        const float tintX = chromeCenter.x;
        const float tintY = chromeCenter.y + kHeaderTopOffset;
        e.statTint.setPosition(tintX, tintY, 1);
    }

    // ---- bottom-stats sprintf ----
    // binary reads worldStatsBlock at indices 1, 4, 5:
    //   block[1]  = worldLevelIndex     ("World" label, despite name)
    //   block[4]  = levelsGained         ("Level")
    //   block[5]  = itemsFound           ("Items")
    if (worldStatsBlock) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "World: %d      Level: %d      Items: %d",
                      worldStatsBlock[1],
                      worldStatsBlock[4],
                      worldStatsBlock[5]);
        statsText.setString(buf, -1);
    }

    // ---- statsText position centered within statsChromeLabel ----
    // binary: textCenterX = chromeCenterX; textCenterY = chromeCenterY +
    //         halfTextHeight + DAT_c9c. then store textX = chromeCenterX -
    //         halfTextWidth, textY = chromeBottom + dip.
    {
        const float halfTextW = statsText.renderedWidth * statsText.scaleX * 0.5f;
        const float halfTextH = statsText.maxCharHeight * statsText.scaleY * 0.5f;
        const Vec2  chromeCenter = labelCenter(statsChromeLabel);
        statsText.posX = chromeCenter.x - halfTextW;
        statsText.posY = chromeCenter.y + halfTextH + kStatsLabelDip;
    }

    // play the menu-appear chime.
    Game* g = getGame();
    if (g) {
        g->soundQueue.trigger(8);   // sound 8 = "menuAppear"
    }

    // prime the panel by running one update tick at dt = 0. mirrors the
    // binary's `FUN_10000a594(0.0, param_3)` tail call inside open.
    update(0.0f);
}

// reconstructed from Ghidra FUN_10000a594.
void UserStatsPanel::update(float dt) {

    if (!visible) {
        return;
    }

    // ---- fade-in / fade-out ramp ----
    if (fadeTimer < 1.0f) {
        // advance toward 1.0 over kFadeRate seconds.
        float newT = fadeTimer + dt / kFadeRate;
        if (newT > 1.0f) {
            newT = 1.0f;
        }
        fadeTimer = newT;

        // close-out gate: when the panel is in "wind-down" (active == 0)
        // and the timer reaches 1.0, hide the panel entirely.
        if (!active && newT >= 1.0f) {
            visible = false;
        }

        // cosine-eased alpha curve. un-inverted timer when opening
        // (active=true), inverted timer when closing:
        //   active  -> t = newT     (alpha ramps 0..255 over fade-in)
        //   !active -> t = 1 - newT (alpha ramps 255..0 over fade-out)
        //   eased   = 0.5 - cos(t * pi) * 0.5
        //   alpha   = uint8(eased * 255)
        const float t = active ? fadeTimer : (1.0f - fadeTimer);
        const float eased = 0.5f - std::cos(t * kFadeCosPi) * 0.5f;
        const int   alphaI = static_cast<int>(eased * kAlphaMax);
        drawAlpha = static_cast<uint8_t>(alphaI & 0xFF);

        // backdrop alpha is half of drawAlpha (binary writes
        // (uVar13 >> 1) & 0x7f).
        bgQuad.setAlpha(static_cast<uint8_t>((alphaI >> 1) & 0x7F));

        // propagate alpha to every child element so they fade together.
        headerLabel.setAlpha(drawAlpha);
        for (UserStatsEntry& e : entries) {
            e.hitTestQuad.setAlpha(drawAlpha);
            e.statIcon.setAlpha(drawAlpha);
            e.chromeFrame.setAlpha(drawAlpha);
            e.statTint.setAlpha(drawAlpha);
            e.descFrame.setAlpha(drawAlpha);
        }
        statsChromeLabel.setAlpha(drawAlpha);
        statsText.setAlpha(drawAlpha);    // FUN_1000301fc, writes the alpha
                                          // byte + re-applies color.
        perksLabel.setAlpha(drawAlpha);

        // per-row Item alpha propagation (= binary FUN_1000333a8 call
        // inside the fade loop). without this, the Item icons rendered
        // inside descFrames stay at full alpha while the panel fades.
        for (UserStatsEntry& e : entries) {

            if (e.descPtr) {
                static_cast<Item*>(e.descPtr)->setAlpha(drawAlpha);
            }
        }
        return;
    }

    // fadeTimer >= 1.0, fade complete. now hit-test touches.
    Game* g = getGame();
    if (!g) {
        return;
    }

    const int inputState = g->inputState();

    // release-frame or idle: nothing to dispatch.
    if (inputState != 1) {

        if (inputState != 0) {
            return;
        }
        // touch released this frame: dismiss DetailPanel but do not
        // close the stat panel itself (matches binary FUN_10000a594).
        if (detailPanel) {
            detailPanel->reset(0);
        }
        return;
    }

    // active touch: convert to panel-local coords.
    const float touchX = g->touchX() - posX;
    const float touchY = g->touchY() - posY;

    // outermost gate: when the touch lands outside headerLabel, trigger
    // the close-fade and play sound 8 (also used on open; the binary
    // reuses it for the close beep).
    if (!headerLabel.contains(touchX, touchY)) {
        requestClose();
        return;
    }

    if (!detailPanel) {
        return;
    }

    // ---- 3 entry rows: chromeFrame -> hitTestQuad -> descFrame ----
    for (int i = 0; i < 3; ++i) {
        UserStatsEntry& e = entries[i];

        // chromeFrame hit -> "range" description (chrome_strings[i]).
        if (e.chromeFrame.contains(touchX, touchY)) {
            const Vec2 c = labelCenter(e.chromeFrame);
            const float anchor[2] = { c.x + posX, c.y + posY };
            const float headerY   = e.chromeFrame.getHeight() * 0.5f;
            const StatRowStrings& s = kChromeRowStrings[i];
            detailPanel->populateForStatRow(headerY, anchor,
                                            s.contentType,
                                            s.name, s.desc0, s.desc1, s.desc2);
            return;
        }

        // hitTestQuad hit -> "what does this icon mean" description.
        if (e.hitTestQuad.contains(touchX, touchY)) {
            const float anchor[2] = { e.hitTestQuad.posX + posX,
                                      e.hitTestQuad.posY + posY };
            const float headerY   = e.hitTestQuad.height * 0.5f;
            const StatRowStrings& s = kHitRowStrings[i];
            detailPanel->populateForStatRow(headerY, anchor,
                                            s.contentType,
                                            s.name, s.desc0, s.desc1, s.desc2);
            return;
        }

        // descFrame hit -> Item card via baseItems[i] (= e.descPtr).
        if (e.descFrame.contains(touchX, touchY)) {
            const Vec2 c = labelCenter(e.descFrame);
            const float anchor[2] = { c.x + posX, c.y + posY };
            const float headerY   = e.descFrame.getHeight() * 0.5f;
            detailPanel->populateForItem(headerY, anchor,
                                         static_cast<Item*>(e.descPtr));
            return;
        }
    }

    // ---- bottom-stats hit ----
    if (statsChromeLabel.contains(touchX, touchY)) {
        const Vec2 c = labelCenter(statsChromeLabel);
        const float anchor[2] = { c.x + posX, c.y + posY };
        detailPanel->populateForStatRow(/*headerY=*/0.0f, anchor,
                                        /*contentType=*/0u,
                                        /*name=*/nullptr,
                                        "This shows the world you're in,",
                                        "how many levels you've gained,",
                                        "how many items you've found");
        return;
    }

    // ---- perks strip hit ----
    if (playerSystemPtr == nullptr) {
        return;
    }
    if (!perksLabel.contains(touchX, touchY)) {
        return;
    }

    const auto& ps = *playerSystemPtr;
    const auto& perks = ps.perks;

    if (perks.empty()) {
        return;
    }

    const size_t coordPairs = perkCoords.size() / 2;
    const size_t walkable   = (perks.size() < coordPairs)
                            ? perks.size() : coordPairs;

    for (size_t i = 0; i < walkable; ++i) {
        const float coordX = perkCoords[2 * i];
        const float coordY = perkCoords[2 * i + 1];

        const bool inX = (touchX >= coordX - kPerkHitHalfWidth) &&
                         (touchX <= coordX + kPerkHitHalfWidth);
        const bool inY = (touchY >= coordY - kPerkHitHalfWidth) &&
                         (touchY <= coordY + kPerkHitHalfWidth);

        if (inX && inY) {
            const float anchor[2] = { coordX + posX, coordY + posY };
            detailPanel->populateForPerk(kPerkHitHalfWidth, anchor,
                                         perks[i]);
            return;
        }
    }
}

// shared close path: fade the panel out (active=0 + fadeTimer rewind, which
// update() carries to visible=false once the fade completes), dismiss any
// open inspect card, and play the close beep (sound 8). called both by the
// touch-outside dismiss in update() and by the Android back button.
void UserStatsPanel::requestClose() {
    active    = false;
    fadeTimer = 0.0f;

    if (detailPanel) {
        detailPanel->reset(0);
    }

    if (Game* g = getGame()) {
        g->soundQueue.trigger(8);
    }
}

// reconstructed from Ghidra FUN_10000a3dc.
void UserStatsPanel::draw() {

    if (!visible) {
        return;
    }

    // backdrop drawn under tex 0 (bgQuad uses solid color, no atlas).
    bindTexture(0);
    bgQuad.draw();

    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    // panel chrome + entries on tex 9 (ui1).
    bindTexture(9);
    headerLabel.draw();

    for (UserStatsEntry& e : entries) {
        e.hitTestQuad.draw();
        e.statIcon.draw();        // TileContent::draw binds tex 9 internally.
        e.chromeFrame.draw();
        e.statTint.draw();
        e.descFrame.draw();
    }

    statsChromeLabel.draw();
    statsText.draw();

    // perks strip, gated on non-empty playerSystem.perks. walk the live
    // perks vector held in playerSystemPtr.
    bindTexture(9);

    if (playerSystemPtr != nullptr) {
        const auto& ps = *playerSystemPtr;
        const auto& perks = ps.perks;

        if (!perks.empty()) {
            perksLabel.draw();

            // per-perk render loop. each Perk owns its own setup (icon UV
            // / category color / level digit) seeded by Perk::init when it
            // was acquired; Perk::drawAt just positions + alphas the trio.
            // perkCoords was populated in open(): entry i occupies
            // perkCoords[2*i .. 2*i+1] as (centerX, centerY).
            const size_t count = perks.size();
            const size_t coordPairs = perkCoords.size() / 2;
            const size_t draws = (count < coordPairs) ? count : coordPairs;

            for (size_t i = 0; i < draws; ++i) {
                Perk* p = perks[i];

                if (!p) {
                    continue;
                }

                p->drawAt(perkCoords[2 * i],
                          perkCoords[2 * i + 1],
                          drawAlpha);
            }
        }
    }

    // per-entry Item-icon render loop (binary's FUN_1000332d4 dispatch).
    // each row's baseItem displays inside its descFrame at the frame's
    // centroid. anchor (0, 0) means abilities inherit the same translate
    // as the rest of the Item assemblage.
    for (UserStatsEntry& e : entries) {

        if (!e.descPtr) {
            continue;
        }

        glPushMatrix();
        const float cx = e.descFrame.getLeftX()
                         + e.descFrame.getWidth() * 0.5f;
        const float cy = e.descFrame.topY
                         + e.descFrame.getHeight() * 0.5f;
        glTranslatef(cx, cy, 0.0f);

        const float abilityAnchor[2] = { 0.0f, 0.0f };
        static_cast<Item*>(e.descPtr)->drawAt(/*showAbilities=*/1,
                                              abilityAnchor);
        glPopMatrix();
    }

    glPopMatrix();
}
