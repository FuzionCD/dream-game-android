#include "level_up_panel.h"
#include "game.h"             // getGame() + bindTexture
#include "perk_table.h"       // PERK_TYPE_TABLE for pickPerkSet's filter pass
#include "player_system.h"
#include "random.h"
#include "renderer.h"         // Renderer::getVirtualHeight
#include "sound_queue.h"
#include <SDL.h>

// the picks list and drain list are real std::list<int> instances (see
// level_up_panel.h's tail-region comment). the binary's manual node
// management (operator_new(0x18), prev/next splicing, count++/--) is
// exactly what std::list does internally on libc++ aarch64; we delegate
// to the stdlib rather than re-implementing the same primitives.

// panelShow / panelHide / snapToPixelGrid / baseUpdate moved into menu.cpp
// when we extracted the Menu base class. invocations below use
// `this->panelShow()` / `Menu::panelHide(bool)` etc.

// FUN_10002c230, LevelUpPanel ctor.
//
// runs once at GameBoard construction. binary's body has 4 phases:
//
//   phase 1: panel base ctor FUN_100057424(this, 641, 906, 251, 48) +
//            vtable override to PTR_thunk_FUN_10002cbbc_100074540. the base
//            ctor sets up bgDim, frame9slice, titleQuad, closeBg, and the
//            confirmButton Quad.
//
//   phase 2: per-stat-slot sub-object init loop (3 x 0x2E8 stride).
//            init the slot's TileContent icon (lightweight, no setType yet),
//            label0, label1, numberTint.
//
//   phase 3: per-perk-slot sub-object init loop (3 x 0x360 stride). init
//            label0, label1, nameText, descText[0..2]. open() populates
//            setString + position per perk pick.
//
//   phase 4: panel-wide layout pass. binary calls FUN_100057b98 (sets
//            panel size + writes anchorX) and FUN_100057c54 (writes anchorY).
//
//   phase 5: per-stat-slot visual layout. for each slot:
//              - setType(2/6/3) on the TileContent icon
//              - 3 addGlyph calls on label0 (9-slice frame, uv y=124)
//              - 3 addGlyph calls on label1 (9-slice frame, uv y=123)
//              - setSize + setPosition for both labels
//              - place icon at label0's centroid via TileContent::setPosition
//
//   phase 6: per-perk-slot visual layout. same chrome pattern as phase 5
//            plus the TextItem nameText / descText[0..2] glyph table + scale
//            wires.
// FUN_10002cbbc, dtor. frees the preview Perks the slots own (delete is
// null-safe, matching the binary's cbz guard). ~Perk tears down each one's
// TileIcon / ColorTint sub-objects.
LevelUpPanel::~LevelUpPanel() {
    for (LevelUpPerkSlot& slot : perkSlots) {
        delete slot.perk;
    }
}

void LevelUpPanel::init() {
    // ---- phase 1: Menu base ctor (FUN_100057424) ----
    //
    // binary calls Menu::ctor(this, 0x281, 0x38a, 0xfb, 0x30) =
    // (titleAtlasX=641, titleAtlasY=906, titleAtlasW=251, titleAtlasH=48).
    // sets up bgDim, frame9slice (9-slice chrome), titleQuad UV/size,
    // closeBg + confirmButton.
    Menu::init(0x281, 0x38a, 0xfb, 0x30);

    // ---- phase 2: per-stat-slot sub-object inits ----
    for (int i = 0; i < STAT_SLOT_COUNT; ++i) {
        LevelUpStatSlot& s = statSlots[i];

        s.icon.initDefault();   // FUN_10001467c
        s.label0.init();        // FUN_10004c014
        s.label1.init();        // FUN_10004c014
        s.numberTint.init();    // FUN_10003c070
    }

    // ---- phase 3: per-perk-slot sub-object inits ----
    for (int i = 0; i < PERK_SLOT_COUNT; ++i) {
        LevelUpPerkSlot& s = perkSlots[i];

        s.perk = nullptr;       // binary writes *(perk*) = 0 explicitly
        s.posX = 0.0f;
        s.posY = 0.0f;
        s.label0.init();
        s.label1.init();
        s.nameText.init();      // FUN_10002fa08

        for (TextItem& d : s.descText) {
            d.init();
        }
    }

    // step "5" of the binary (std::list inits) is handled by libc++'s
    // default ctor when LevelUpPanel is value-initialized inside GameBoard.

    // ---- phase 4: Menu base setSize + setAnchorY ----
    //
    // Menu::setSize sizes frame9slice + positions titleQuad/closeBg/
    // confirmButton (with the shared DAT_10005a740/744/748 offsets) +
    // writes anchorX. Menu::setAnchorY just snap-writes anchorY.
    //
    // binary hardcodes DAT_10005a0a4 = 0.294 for anchorY, calibrated for
    // iOS (virtualHeight ~= 1.5). Menu::setSizeAndCenterY derives the
    // anchor from runtime virtualHeight so the panel stays vertically
    // centered on any aspect ratio.
    constexpr float PANEL_W = 0.763f;   // DAT_10005a09c
    constexpr float PANEL_H = 0.895f;   // DAT_10005a0a0
    Menu::setSizeAndCenterY(PANEL_W, PANEL_H);

    // ---- phase 5: per-stat-slot visual layout ----
    //
    // sizeMode legend (see Label::layoutGlyphs):
    //   2 = corner (left/right end caps, natural width)
    //   1 = center stretch (fills between caps)
    //
    // the 6-glyph 9-slice frame structure per label:
    //   glyph 0 (mode 2)  left cap
    //   glyph 1 (mode 1)  center stretch
    //   glyph 2 (mode 2)  right cap
    //
    // label0 uses uv y=124 (one frame row on the atlas).
    // label1 uses uv y=123 (the adjacent row, a slightly different chrome).
    constexpr float PIXEL_TO_PANEL    = 640.0f;       // DAT_10005a0a8 (panel ref width)
    constexpr float SLOT_X_BASE       = 0.05625f;     // DAT_10005a0ac (verified asm-level)
    constexpr float LABEL_Y           = 0.0546875f;   // DAT_10005a0b0 (verified)
    constexpr float ICON_Y_NUDGE      = -0.0015625f;  // DAT_10005a0b4 (verified)
    constexpr int   STAT_SLOT_X_STRIDE_PX = 0x92;     // 146 atlas pixels between stat slots

    // slot 0 = ATK (TileContent type 2), slot 1 = HP (type 6), slot 2 = DEF (type 3).
    static constexpr uint32_t STAT_ICON_TYPE[STAT_SLOT_COUNT] = { 2u, 6u, 3u };

    // uvOrigin is the larger atlas-x pair (FUN_10004c310's 3rd arg,
    // uvOriginPx), uvSize the smaller dimension pair (4th arg, uvSizePx).
    // the resulting frame is a horizontal stretchable button: 20-wide
    // left cap (mode 2) + 1-wide center stretch (mode 1) + 20-wide right
    // cap (mode 2), each 124 tall for label0 / 123 tall for label1 (the
    // two labels pull their chrome from adjacent atlas rows).
    static constexpr float L0_UV_ORIGIN[3][2] = {
        {488.0f, 81.0f},
        {508.0f, 81.0f},
        {509.0f, 81.0f},
    };

    static constexpr float L0_UV_SIZE[3][2] = {
        {20.0f, 124.0f},
        { 1.0f, 124.0f},
        {20.0f, 124.0f},
    };

    static constexpr float L1_UV_ORIGIN[3][2] = {
        {530.0f, 81.0f},
        {550.0f, 81.0f},
        {551.0f, 81.0f},
    };

    static constexpr float L1_UV_SIZE[3][2] = {
        {20.0f, 123.0f},
        { 1.0f, 123.0f},
        {20.0f, 123.0f},
    };

    static constexpr uint32_t FRAME_MODES[3] = { 2u, 1u, 2u };

    for (int i = 0; i < STAT_SLOT_COUNT; ++i) {
        LevelUpStatSlot& s = statSlots[i];

        // phase 5a: icon content type.
        s.icon.setType(STAT_ICON_TYPE[i]);

        // phase 5b: 3-glyph 9-slice frame on label0.
        for (int g = 0; g < 3; ++g) {
            GlyphOffset off = {0.0f, 0.0f};
            s.label0.addGlyph(-1.0f, L0_UV_ORIGIN[g], L0_UV_SIZE[g],
                              FRAME_MODES[g], off);
        }

        // phase 5c: 3-glyph 9-slice frame on label1.
        for (int g = 0; g < 3; ++g) {
            GlyphOffset off = {0.0f, 0.0f};
            s.label1.addGlyph(-1.0f, L1_UV_ORIGIN[g], L1_UV_SIZE[g],
                              FRAME_MODES[g], off);
        }

        // phase 5d: label0 setSize + setPosition. binary calls measureGlyphRun
        // (FUN_10004c658) twice and passes the s1 (heightTotal) component of
        // both returns into setSize. for a single-line 9-slice button this
        // works out to (3 glyphs x 124 atlas-px) / 640 = 0.581, the
        // artist-encoded button width. the second call is an ABI artifact
        // (same filter args, same return).
        float l0Width = s.label0.measureGlyphRun(-1, -1).heightTotal;
        s.label0.setSize(l0Width, l0Width);

        float slotX = ((float)(i * STAT_SLOT_X_STRIDE_PX) / PIXEL_TO_PANEL)
                    + SLOT_X_BASE;
        s.label0.setPosition(slotX, LABEL_Y);

        // phase 5e: label1 setSize + setPosition, anchored to label0's left
        // edge. binary calls getWidth(label0) twice (returns Vec2 = (width,
        // height) per asm verification) and copies both dimensions onto
        // label1. setPosition uses (label0.leftX, label0.topY) from
        // getLeftX's Vec2 return.
        l0Width  = s.label0.getWidth();
        float l0Height = s.label0.getHeight();
        s.label1.setSize(l0Width, l0Height);
        s.label1.setPosition(s.label0.getLeftX(), s.label0.topY);

        // phase 5f: place the icon at label0's centroid.
        //   centerX = label0.leftX + label0.getWidth() * 0.5
        //   centerY = label0.topY  + label0.getHeight() * 0.5 + ICON_Y_NUDGE
        //
        // binary's asm: getLeftX returns Vec2 (leftX, topY); getWidth returns
        // Vec2 (width, height). centroid is computed as the label's actual
        // mid-rect with a small Y nudge (DAT_10005a0b4 = -0.0015625).
        float centerX = s.label0.getLeftX() + s.label0.getWidth()  * 0.5f;
        float centerY = s.label0.topY      + s.label0.getHeight() * 0.5f + ICON_Y_NUDGE;
        s.icon.setPosition(centerX, centerY);
    }

    // ---- phase 6: per-perk-slot visual layout ----
    //
    // each perk slot has the same 3-glyph 9-slice frame structure as the stat
    // slots, just stacked vertically (Y stride 126 atlas-px = 0.197 normalized)
    // instead of horizontally. label0 / label1 reuse the same atlas UVs as
    // the stat slots; both atlases share the chrome frame texture at uv
    // y=124 (label0, unselected) and y=123 (label1, selected).
    //
    // per-slot setSize uses (0.65, heightTotal): DAT_10005a0b8 = 0.65 is
    // the panel-wide perk-button width; the height comes from the preceding
    // measureGlyphRun(-1, -1).heightTotal (= last glyph's vNat = atlas-row-
    // height / 640). label1 inherits label0's dimensions + left edge.
    //
    // Perk* allocation + TextItem::setString happen in open() per the binary;
    // the up-front glyph-table-ptr + scale wires for nameText / descText[0..2]
    // live in init() so they're ready before the first open() runs.
    constexpr float PERK_LABEL_WIDTH      = 0.65f;     // DAT_10005a0b8
    constexpr int   PERK_SLOT_Y_STRIDE_PX = 0x7E;      // 126 atlas px
    constexpr float NAME_TEXT_SCALE       = 0.085f;    // scaleX + scaleY
    constexpr float DESC_TEXT_SCALE       = 0.07f;     // scaleX + scaleY

    static constexpr float PERK_L0_UV_ORIGIN[3][2] = {
        {488.0f, 81.0f},
        {508.0f, 81.0f},
        {509.0f, 81.0f},
    };

    static constexpr float PERK_L0_UV_SIZE[3][2] = {
        {20.0f, 124.0f},
        { 1.0f, 124.0f},
        {20.0f, 124.0f},
    };

    static constexpr float PERK_L1_UV_ORIGIN[3][2] = {
        {530.0f, 81.0f},
        {550.0f, 81.0f},
        {551.0f, 81.0f},
    };

    static constexpr float PERK_L1_UV_SIZE[3][2] = {
        {20.0f, 123.0f},
        { 1.0f, 123.0f},
        {20.0f, 123.0f},
    };

    // glyph table pointer: bmfontTable(0) is the embedded BMFontTable for the
    // panel font (3 BMFontTables live in the Game struct per game.h, index 0
    // is the panel font used by all panel-resident TextItems).
    Game* perkGame = getGame();
    const BMFontTable* panelFontTable = nullptr;

    if (perkGame) {
        panelFontTable = &perkGame->bmfontTable(0);
    }

    for (int i = 0; i < PERK_SLOT_COUNT; ++i) {
        LevelUpPerkSlot& s = perkSlots[i];

        // binary writes perk* = 0 explicitly here (already nullptr from phase 3).
        s.perk = nullptr;

        // 3-glyph 9-slice frame on label0 (uv y=124 row).
        for (int g = 0; g < 3; ++g) {
            GlyphOffset off = {0.0f, 0.0f};
            s.label0.addGlyph(-1.0f, PERK_L0_UV_ORIGIN[g], PERK_L0_UV_SIZE[g],
                              FRAME_MODES[g], off);
        }

        // 3-glyph 9-slice frame on label1 (uv y=123 row).
        for (int g = 0; g < 3; ++g) {
            GlyphOffset off = {0.0f, 0.0f};
            s.label1.addGlyph(-1.0f, PERK_L1_UV_ORIGIN[g], PERK_L1_UV_SIZE[g],
                              FRAME_MODES[g], off);
        }

        // TextItem glyph table + scale. setString hasn't been called yet;
        // these writes just prep the per-TextItem font reference so open()
        // can populate the strings without re-binding.
        if (panelFontTable != nullptr) {
            s.nameText.glyphTablePtr = panelFontTable;
            s.nameText.scaleX        = NAME_TEXT_SCALE;
            s.nameText.scaleY        = NAME_TEXT_SCALE;

            for (TextItem& d : s.descText) {
                d.glyphTablePtr = panelFontTable;
                d.scaleX        = DESC_TEXT_SCALE;
                d.scaleY        = DESC_TEXT_SCALE;
            }
        }

        // label0 setSize + setPosition. binary loads width = 0.65
        // (DAT_10005a0b8) but leaves s1 = heightTotal from the preceding
        // measureGlyphRun's paired (s0, s1) return, so setSize gets
        // (0.65, heightTotal). same dropped-s1 pattern as layoutGlyphs
        // earlier; without the height the bbox is zero-tall and every
        // label.contains() hit-test fails.
        //
        // for filterOpen (-1, -1), heightTotal = last glyph's vNat = the
        // chrome's atlas row height / 640 (= 124/640 for label0's y=124
        // chrome).
        GlyphRunMetrics l0m = s.label0.measureGlyphRun(-1, -1);
        s.label0.setSize(PERK_LABEL_WIDTH, l0m.heightTotal);

        float perkY = (float)((i + 1) * PERK_SLOT_Y_STRIDE_PX) / PIXEL_TO_PANEL + LABEL_Y;
        s.label0.setPosition(SLOT_X_BASE, perkY);

        // label1 inherits label0's (width, height): binary uses getWidth's
        // Vec2 return (the (s0=width, s1=height) pair after setSize). using
        // the now-real l0Height keeps both labels in sync.
        float l0Width  = s.label0.getWidth();
        float l0Height = s.label0.getHeight();
        s.label1.setSize(l0Width, l0Height);
        s.label1.setPosition(s.label0.getLeftX(), perkY);
    }
}

// FUN_10002d278, open the level-up panel. sets up the 3 stat slots (magnitude +
// tint + position), allocates the 3 perk-slot Perks matching pickPerkSet's
// output, writes each perk's name + 3 description TextItems with their
// positions, runs setSlotSelected, and primes with update(0). the per-glyph
// tables are wired once in init().
void LevelUpPanel::open(PlayerSystem* playerSystem) {
    // step 1: base show (visible byte, animTimer0, audio cue 0x08).
    Menu::panelShow();

    // step 2: second audio cue, the level-up "panel ready" sound that
    // plays on top of the open whoosh. binary's second FUN_100035ccc call.
    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x0B);
    }

    // step 3: clear the cached alpha. the panel's first fade frame will
    // overwrite this immediately, but the binary explicitly zeros it here.
    cachedAlpha = 0;

    // step 4: cache the perkLevel(0xE) gate. perk 0x0E grants
    // "Can choose 2 stats or 2 perks on level up"; when active (> 0),
    // tap handler allows two picks from the same category, otherwise the
    // player must pick one stat + one perk.
    int perk0E = playerSystem->perkLevel(0xE);
    allowTwoFromCategory = (perk0E > 0) ? 1 : 0;

    // step 5: clear any stale picks left over from a previous open.
    picksList.clear();
    drainList.clear();
    readyToCommit = 0;

    // step 6: stat slots. write each slot's magnitude (slot.value).
    // magnitude formula: perkLevel(0xC) + 1, plus perkLevel(0xD) when
    // this slot is the player's min stat. matches perkType 0x0C ("Stats
    // increase by 2/3 on level up") and 0x0D ("Lowest stat increases by
    // an extra +1/+2"). slot order is ATK / HP / DEF.
    int baseStats[3] = {
        playerSystem->baseATK,    // slot 0 = ATK
        playerSystem->baseHP,     // slot 1 = HP
        playerSystem->baseDEF,    // slot 2 = DEF
    };
    int minStat = baseStats[0];

    if (baseStats[1] < minStat) {
        minStat = baseStats[1];
    }

    if (baseStats[2] < minStat) {
        minStat = baseStats[2];
    }

    int baseGain = playerSystem->perkLevel(0xC) + 1;

    for (int i = 0; i < STAT_SLOT_COUNT; ++i) {
        LevelUpStatSlot& slot = statSlots[i];
        int mag = baseGain;

        if (baseStats[i] == minStat) {
            mag += playerSystem->perkLevel(0xD);
        }

        // slot.value = the +N amount the player gains. consumed
        // by getNextStatPick during commit drain.
        slot.value = mag;

        // FUN_100014870 = TileContent::setRawAndDisplayMagnitude. show the
        // future stat value (current + gain) on the icon so the player sees
        // "ATK 7" preview when their current ATK is 4 and gain is +3.
        slot.icon.setRawAndDisplayMagnitude(mag + baseStats[i]);

        // FUN_10003c5ac = ColorTint::setSignedNumber. renders "+N" as the
        // sign-prefixed number tint next to the icon. textStyle 0, mode 1.
        slot.numberTint.setSignedNumber(mag, 0, 1);

        // position the "+N" tint at label0's bottom-right corner with small
        // negative insets on both axes. getLeftX (FUN_10004c93c) returns the
        // (X, Y) origin and getWidth (FUN_10004c61c) the (W, H); both pairs
        // add componentwise, plus the (-0.0375, -0.03125) insets.
        // Ghidra's `param_2` was an artifact of the result local, not a
        // caller arg and not a cross-slot accumulator.
        constexpr float NUMBER_TINT_X_INSET = -0.0375f;    // DAT_10005a0bc
        constexpr float NUMBER_TINT_Y_INSET = -0.03125f;   // DAT_10005a0c0
        float tintX = slot.label0.getLeftX()
                    + slot.label0.getWidth()
                    + NUMBER_TINT_X_INSET;
        float tintY = slot.label0.topY
                    + slot.label0.getHeight()
                    + NUMBER_TINT_Y_INSET;
        slot.numberTint.setPosition(tintX, tintY, 1);
    }

    // step 7: perk slots. generate 3 random perkTypes via pickPerkSet
    // (FUN_10002d620), then allocate a fresh Perk(perkType, currentLevel+1)
    // for each slot. the Perk is a preview: the player sees what level
    // they'd get if they picked it, before committing.
    //
    // every per-slot position derives from label0's (leftX, topY) Vec2.
    // Ghidra's decomp shows `param_2` accumulating across iterations, but
    // the asm calls getLeftX twice per slot and uses both components
    // (s0=leftX, s1=topY); param_2 was a phantom local.
    int perkTypes[3] = { 0, 0, 0 };
    pickPerkSet(playerSystem, perkTypes);

    constexpr float PERK_SLOT_X_OFFSET  = 0.089062f;   // DAT_10005a0c4
    constexpr float PERK_SLOT_Y_OFFSET  = 0.095312f;   // DAT_10005a0c8
    constexpr float NAME_TEXT_X_OFFSET  = 0.171875f;
    constexpr float NAME_TEXT_Y_OFFSET  = 0.0625f;     // DAT_10005a0cc
    constexpr float DESC_LINE_Y_BASE_PX = 72.0f;       // DAT_10005a0d0
    constexpr float DESC_LINE_Y_DIVISOR = 640.0f;      // DAT_10005a0d4 (= screen ref width)
    constexpr float DESC_LINE_Y_STRIDE_PX = 25.0f;

    for (int i = 0; i < PERK_SLOT_COUNT; ++i) {
        LevelUpPerkSlot& slot = perkSlots[i];

        // free any prior preview Perk in this slot. binary's loop walks
        // the Perk's TileIcon + ColorTint sub-objects + std::string before
        // operator_delete; our types' default dtors handle the equivalents.
        Perk* old = slot.perk;

        if (old != nullptr) {
            delete old;
        }

        int curLevel = playerSystem->perkLevel(perkTypes[i]);
        Perk* fresh = new Perk();
        fresh->init(perkTypes[i], curLevel + 1);
        slot.perk = fresh;

        // per-slot positions all anchor to label0's (leftX, topY); getLeftX
        // returns Vec2 (leftX, topY) per asm verification.
        float labelLeftX = slot.label0.getLeftX();
        float labelTopY  = slot.label0.topY;

        // perk-visual origin (warnLine1 + warnLine2 + tint draw here).
        slot.posX = labelLeftX + PERK_SLOT_X_OFFSET;
        slot.posY = labelTopY  + PERK_SLOT_Y_OFFSET;

        // name text, right of the icon, 0.171875 across from label's left.
        slot.nameText.setString(fresh->getName(), -1);
        slot.nameText.posX = labelLeftX + NAME_TEXT_X_OFFSET;
        slot.nameText.posY = labelTopY  + NAME_TEXT_Y_OFFSET;

        // three description lines stacked below name. y stride per line is
        // (j * 25 + 72) / 640.
        for (int j = 0; j < 3; ++j) {
            slot.descText[j].setString(fresh->getDescriptionLine(j), -1);
            slot.descText[j].posX = labelLeftX + NAME_TEXT_X_OFFSET;
            slot.descText[j].posY = labelTopY
                                  + (j * DESC_LINE_Y_STRIDE_PX + DESC_LINE_Y_BASE_PX)
                                  / DESC_LINE_Y_DIVISOR;
        }

        // tint slot i (stat) and slot i+3 (perk) to "unselected"; binary
        // calls FUN_10002d0f8 with each from inside this loop body.
        setSlotSelected(i,                   false);
        setSlotSelected(i + STAT_SLOT_COUNT, false);
    }

    // binary tail-calls vtable[3] = MenuLevel::update (= LevelUpPanel::update)
    // with dt = 0. that initial tick runs baseUpdate(0) which calls setAlpha(0)
    // via the eased-alpha math (t = 0 -> alpha = 0), priming bgDim /
    // frame9slice / titleQuad to start invisible for the fade-in. without it,
    // sub-objects keep their default 0xFF vertex alphas and the panel pops in
    // fully opaque for one frame before the fade applies.
    update(0.0f, 0.0f);

    SDL_Log("LevelUpPanel::open: stat picks (ATK+%d/HP+%d/DEF+%d), perk picks (%d/%d/%d), allowTwo=%d",
            statSlots[0].value, statSlots[1].value, statSlots[2].value,
            perkTypes[0], perkTypes[1], perkTypes[2],
            (int)allowTwoFromCategory);
}

// FUN_10002d620, pickPerkSet.
//
// generates the 3-perk choice set for this level-up. two-pass algorithm
// matching the binary:
//
//   pass 1, category-diverse pick from available perks:
//     - FUN_1000422b8 builds map<category, vector<perkType>> of perks that
//       passed (a) not-maxed: playerSystem.perkLevel < levelCount, and
//       (b) unlocked: levels[curLvl].unlockLevel <= playerSystem.currentLevel.
//       stat-bump perkTypes (0, 4, 8) have empty levels vectors so they
//       automatically fail the not-maxed check and never enter the map.
//     - while picks < 3 and the available-categories set is non-empty:
//         random category from the set (with replacement: each pick removes
//         the category) -> random perkType from its vector -> push to outPerkTypes.
//       since each category is removed after one pick, this guarantees up
//       to 3 perks from up to 3 distinct categories.
//
//   pass 2, stat-bump fallback when a specific category is missing:
//     - if category 5 (ATK) was not picked: add perkType 0 (Strong) to
//       fallback set.
//     - if category 4 (DEF) was not picked: add perkType 4 (Tough).
//     - if category 3 (HP) was not picked: add perkType 8 (Resilient).
//       (exact set membership via std::set::lower_bound, not "any >= N".)
//     - while picks < 3: pop random from fallback set, push to outPerkTypes.
//       binary's FUN_10002dd7c returns 0 when the set is empty so degenerate
//       cases push Strong repeatedly; we match that.
//
// all RNG via stream 1 to match the binary's FUN_1000570ec(0, N, 1) calls.
void LevelUpPanel::pickPerkSet(PlayerSystem* playerSystem,
                               int outPerkTypes[3]) {
    constexpr int CATEGORY_COUNT = 7;

    // ---- pass 1 build phase: matches FUN_1000422b8 ----
    //
    // gather available perks by category, applying not-maxed + unlocked
    // filters. stat-bump perkTypes have levelCount == 0, so the curLvl >=
    // levelCount maxed check is always true and drops them naturally.
    int availableByCategory[CATEGORY_COUNT][PERK_TYPE_COUNT];
    int availableCounts[CATEGORY_COUNT] = {0};

    const int currentLevel = playerSystem->currentLevel;

    for (int t = 0; t < PERK_TYPE_COUNT; ++t) {
        const PerkTypeEntry& entry = PERK_TYPE_TABLE[t];
        int curLvl = playerSystem->perkLevel(t);

        if (curLvl >= entry.levelCount) {
            continue;   // maxed (stat-bumps with levelCount == 0 land here too)
        }

        if (entry.levels[curLvl].unlockLevel > currentLevel) {
            continue;   // locked at current XP level
        }

        // prerequisite gate (FUN_1000422b8): level entry +0 is either 0x19
        // (none) or a perkType the player must already own. Perceptive (0x13)
        // and Sudden (0x14) require Eventful (0x12). read at the current level
        // index, matching the binary's begin + perkLevel * 0x18 + 0.
        int prereq = entry.levels[curLvl].prerequisite;

        if (prereq != 0x19 && playerSystem->perkLevel(prereq) == 0) {
            continue;   // prerequisite perk not yet owned
        }

        int cat = entry.category;
        availableByCategory[cat][availableCounts[cat]++] = t;
    }

    // build the set of non-empty categories.
    int nonEmptyCats[CATEGORY_COUNT];
    int nonEmptyCount = 0;

    for (int c = 0; c < CATEGORY_COUNT; ++c) {

        if (availableCounts[c] > 0) {
            nonEmptyCats[nonEmptyCount++] = c;
        }
    }

    // ---- pass 1 pick phase ----
    int picked = 0;
    bool pickedCatBitset[CATEGORY_COUNT] = {false};

    while (picked < 3 && nonEmptyCount > 0) {
        int catIdx = rngInt(0, nonEmptyCount - 1, 1);
        int cat    = nonEmptyCats[catIdx];

        // remove this category from the available set, preserving sort order
        // so the array mirrors the binary's std::set after each erase (the
        // i-th node it draws is the i-th smallest; FUN_10002dc60).
        for (int k = catIdx; k < nonEmptyCount - 1; ++k) {
            nonEmptyCats[k] = nonEmptyCats[k + 1];
        }

        nonEmptyCount -= 1;

        int perkIdx  = rngInt(0, availableCounts[cat] - 1, 1);
        outPerkTypes[picked++] = availableByCategory[cat][perkIdx];
        pickedCatBitset[cat] = true;
    }

    // ---- pass 2 stat-bump fallback ----
    //
    // the binary checks EXACT category membership via std::set::lower_bound:
    // it adds a stat-bump perk only when that specific category is absent from
    // the picked set, not when no higher category was picked. category 5 maps
    // to Strong (0), 4 to Tough (4), 3 to Resilient (8).
    if (picked < 3) {
        int fallback[3];
        int fallbackCount = 0;

        if (!pickedCatBitset[5]) {
            fallback[fallbackCount++] = 0;   // Strong (ATK stat-bump), category 5
        }

        if (!pickedCatBitset[4]) {
            fallback[fallbackCount++] = 4;   // Tough (DEF stat-bump), category 4
        }

        if (!pickedCatBitset[3]) {
            fallback[fallbackCount++] = 8;   // Resilient (HP stat-bump), category 3
        }

        while (picked < 3) {

            if (fallbackCount > 0) {
                int idx = rngInt(0, fallbackCount - 1, 1);
                outPerkTypes[picked++] = fallback[idx];

                // order-preserving erase, mirroring the binary's std::set
                // (FUN_10002dd7c draws the idx-th smallest, then erases it).
                for (int k = idx; k < fallbackCount - 1; ++k) {
                    fallback[k] = fallback[k + 1];
                }

                fallbackCount -= 1;
            } else {
                // binary's FUN_10002dd7c returns 0 from empty set; match.
                outPerkTypes[picked++] = 0;
            }
        }
    }
}

// Menu::baseUpdate (FUN_1000578f8) is inherited from menu.cpp; handles
// fade animation + confirm-button tap. virtual setAlpha dispatch lets our
// override (below) fade slot content alongside the chrome.

// vtable[4], FUN_10002da3c. fades all of the panel's faded sub-objects.
// the binary's order:
//   1. base setAlpha (FUN_100057b58): bgDim at half alpha, frame9slice
//      and titleQuad at full alpha.
//   2. cache the alpha byte (cachedAlpha).
//   3. each stat slot: TileContent icon (propagates to baseQuad + colorTint)
//      + 2 Label widgets + numberTint.
//   4. each perk slot: 2 Label widgets + nameText + 3 desc TextItems.
//
// notable: the binary does not setAlpha on the confirm button (independently
// colored by show/hide) or on any of the Perk*'s sub-objects (warnLine1/warnLine2/
// tint); those keep whatever opacity setSlotSelected last wrote.
void LevelUpPanel::setAlpha(uint8_t a) {
    // Menu::setAlpha fades bgDim (half) + frame9slice + titleQuad.
    Menu::setAlpha(a);

    cachedAlpha = a;

    for (LevelUpStatSlot& slot : statSlots) {
        // TileContent::setAlpha propagates onto the icon's baseQuad and its
        // inner colorTint, matching the binary's per-slot setAlpha pair.
        slot.icon.setAlpha(a);
        slot.label0.setAlpha(a);
        slot.label1.setAlpha(a);
        slot.numberTint.setAlpha(a);
    }

    for (LevelUpPerkSlot& slot : perkSlots) {
        slot.label0.setAlpha(a);
        slot.label1.setAlpha(a);
        slot.nameText.setAlpha(a);

        for (TextItem& desc : slot.descText) {
            desc.setAlpha(a);
        }
    }
}

// vtable[5], confirm button released over its bbox. for LevelUpPanel,
// this latches readyToCommit = 1. next
// frame, gameBoardUpdate's level-up-panel branch sees the latch, runs
// the commit drain, and calls close().
void LevelUpPanel::onConfirmTapped() {
    readyToCommit = 1;
}

// FUN_10002ce9c, per-frame update + touch.
//
// the binary's body in order:
//   1. baseUpdate(dt): fade animation + confirm-button tap dispatch.
//   2. early-out if !visible (panel finished closing).
//   3. early-out if animTimer0 < 1.0 (still fading).
//   4. early-out if Game.inputState != 1 (no active touch).
//   5. compute local-space touch coords (touch - anchor).
//   6. hit-test slots 0..5 against each slot's label0 widget. on miss,
//      return.
//   7. dup-check: scan picksList for matching slotIdx. if found, play
//      sound 7 (reject) and return.
//   8. play sound 5 (accept).
//   9. category gating:
//        - allowTwoFromCategory == 0: walk picksList, remove any existing
//          pick in the same category (stat vs perk) as the new pick.
//        - allowTwoFromCategory == 1 and picksList.size() >= 2: remove
//          the oldest pick (the front of the picks list).
//   10. push_back the new pick on picksList.
//   11. setSlotSelected(newPick, true).
//   12. if picksList.size() > 1: readyByte = 1 (confirm enabled).
//   13. drainList.assign(picksList): mirror.
//
// stat and perk slot labels are both populated with their 9-slice chrome
// glyphs in init() and laid out via setSize/setPosition there, so
// Label::contains can hit-test against the real bbox by the time update
// runs.
void LevelUpPanel::update(float dt, float touchInput) {
    Menu::baseUpdate(dt);

    if (!visible) {
        return;
    }

    if (animTimer0 < 1.0f) {
        return;
    }

    Game* g = getGame();

    if (!g || g->inputState() != 1) {
        return;
    }

    float touchLocalX = g->touchX() - anchorX;
    float touchLocalY = g->touchY() - anchorY;

    // step 6: slot hit-test. matches the binary's dual-stride iteration
    // in FUN_10002ce9c: for each i in 0..2, test the stat slot's label0
    // first (slotIdx = i on hit), then the perk slot's label0 (slotIdx
    // = i + 3 on hit). first hit wins.
    int hitSlot = -1;

    for (int i = 0; i < STAT_SLOT_COUNT; ++i) {

        if (statSlots[i].label0.contains(touchLocalX, touchLocalY)) {
            hitSlot = i;
            break;
        }

        if (perkSlots[i].label0.contains(touchLocalX, touchLocalY)) {
            hitSlot = i + STAT_SLOT_COUNT;
            break;
        }
    }

    if (hitSlot < 0) {
        return;
    }

    // step 7: dup check.
    for (int existing : picksList) {

        if (existing == hitSlot) {
            g->soundQueue.trigger(7);   // "reject, already picked"
            return;
        }
    }

    // step 8: accept the new pick.
    g->soundQueue.trigger(5);

    // step 9: category gating.
    if (!allowTwoFromCategory) {
        // remove any existing pick in the same category as the new one.
        for (auto it = picksList.begin(); it != picksList.end(); ++it) {
            int existing = *it;
            bool sameCategory = (hitSlot < STAT_SLOT_COUNT && existing < STAT_SLOT_COUNT) ||
                                (hitSlot >= STAT_SLOT_COUNT && existing >= STAT_SLOT_COUNT);

            if (sameCategory) {
                setSlotSelected(existing, false);
                picksList.erase(it);
                break;
            }
        }
    } else if (picksList.size() >= 2) {
        // allowTwoFromCategory + already 2 picks: bump the oldest pick to
        // make room for the new one.
        setSlotSelected(picksList.front(), false);
        picksList.pop_front();
    }

    // steps 10-11: push the new pick + tint its slot.
    picksList.push_back(hitSlot);
    setSlotSelected(hitSlot, true);

    // step 12: enable the confirm button once we have at least 2 picks.
    if (picksList.size() > 1) {
        readyByte = true;
    }

    // step 13: mirror picksList into drainList for the commit drain.
    drainList.assign(picksList.begin(), picksList.end());
}

// FUN_10002cd20, LevelUpPanel::draw (MenuLevel::draw).
//
// the binary's body has two phases:
//
//   phase A: Menu::draw (FUN_100057878, the base).
//      - early-out if !visible.
//      - bindTexture(0) + bgDim.draw(): fullscreen darkening backdrop.
//      - glPushMatrix + glTranslatef(anchorX, anchorY, 0).
//      - bindTexture(9), frame9slice.draw() (= Label::draw on the 9-slice),
//        titleQuad.draw().
//      - if (readyByte): closeBg.draw() + confirmButton.draw().
//      - glPopMatrix.
//
//   phase B: per-slot rendering. binary opens a NEW glPushMatrix +
//      glTranslatef(anchorX, anchorY) and binds texture 9 again, then for
//      each i in 0..2:
//        - pick stat slot's label0 (default) or label1 (when picksList
//          contains slotIdx == i). this is the "selected" visual swap.
//        - draw that label, then the slot's TileContent icon, then the
//          numberTint ColorTint.
//        - pick perk slot's label0 (default) or label1 (when picksList
//          contains slotIdx == i + 3).
//        - draw that label.
//      then a second inner loop: draw each perk slot's Perk* +
//      nameText TextItem + 3 descText TextItems.
void LevelUpPanel::draw() {

    if (!visible) {
        return;
    }

    // ---- phase A: Menu::draw (chrome) ----
    Menu::draw();

    // ---- phase B: slot rendering, anchored to the panel ----
    glPushMatrix();
    glTranslatef(anchorX, anchorY, 0.0f);
    bindTexture(9);

    for (int i = 0; i < STAT_SLOT_COUNT; ++i) {
        LevelUpStatSlot& slot = statSlots[i];

        // stat slot: pick label0 (unselected) or label1 (selected); the
        // binary scans picksList for the matching slotIdx and swaps which
        // Label gets drawn.
        bool selectedStat = false;

        for (int idx : picksList) {

            if (idx == i) {
                selectedStat = true;
                break;
            }
        }

        if (selectedStat) {
            slot.label1.draw();
        } else {
            slot.label0.draw();
        }

        slot.icon.draw();
        slot.numberTint.draw();

        // perk slot: same pattern but picksList encodes perk picks as
        // slotIdx i + STAT_SLOT_COUNT.
        LevelUpPerkSlot& perk = perkSlots[i];
        bool selectedPerk = false;

        for (int idx : picksList) {

            if (idx == i + STAT_SLOT_COUNT) {
                selectedPerk = true;
                break;
            }
        }

        if (selectedPerk) {
            perk.label1.draw();
        } else {
            perk.label0.draw();
        }
    }

    // per-perk-slot inner loop: draw perk icons + nameText + 3 descText
    // entries. matches the binary's tail loop at FUN_10002cd20 (the
    // `if (lVar3 == 3) { ... }` block).
    for (int i = 0; i < PERK_SLOT_COUNT; ++i) {
        LevelUpPerkSlot& slot = perkSlots[i];

        if (slot.perk != nullptr) {
            slot.perk->drawAt(slot.posX, slot.posY, cachedAlpha);
        }

        slot.nameText.draw();

        for (TextItem& d : slot.descText) {
            d.draw();
        }
    }

    glPopMatrix();
}

// FUN_10002d0f8, setSlotSelected.
//
// tint the slot's visible sub-objects in either "fresh" (white / bright)
// or "selected" (grey / dim) state. stat slots and perk slots have
// different sub-object layouts and so are handled separately.
//
//   stat slot (slotIdx 0..2):
//     - the icon's baseQuad + inner colorTint (both via
//       FUN_100014b54's bundled write)
//     - main number ColorTint (numberTint, via FUN_10003c8d8)
//     - both written with the same 0xFFRRGGBB color: 0xFFFFFFFF for
//       unselected (param_3 == 0), 0xFFC8C8C8 for selected (grey)
//
//   perk slot (slotIdx 3..5):
//     - the slot's Perk* (3 sub-visuals: warnLine1, warnLine2, tint), tinted
//       via FUN_100042150 with 0xFFFFFFFF (unselected) or 0xFFC8C8C8
//       (selected)
//     - the nameText TextItem: its rgba field written with
//       a derived grey (0xFFFFFFFF or 0xFFC8C8C8), then applyColor()
//     - 3 desc TextItems (descText[0..2]): each rgba
//       set to a greenish grey (0xFFC8BEC8 bright, 0xFF968C96 dim, G
//       channel raised above the R/B grey), then applyColor()
void LevelUpPanel::setSlotSelected(int slotIdx, bool selected) {

    if (slotIdx < STAT_SLOT_COUNT) {
        // ---- stat slot path ----
        // binary's FUN_100014b54(slot.icon, &rgba) propagates the same color
        // onto the icon's baseQuad AND its embedded colorTint, then
        // FUN_10003c8d8(numberTint, &rgba) writes the same color into the
        // "+N" digit tint.
        LevelUpStatSlot& slot = statSlots[slotIdx];
        uint8_t v = selected ? 0xC8 : 0xFF;   // white or 0xFFC8C8C8 grey

        slot.icon.baseQuad.setColor(v, v, v, 0xFF);
        slot.icon.colorTint.setColor(v, v, v);
        slot.numberTint.setColor(v, v, v);
        return;
    }

    // ---- perk slot path ----
    LevelUpPerkSlot& slot = perkSlots[slotIdx - STAT_SLOT_COUNT];

    uint8_t pv = selected ? 0xC8 : 0xFF;   // perk visuals + name share grey

    // FUN_100042150: tints the Perk's warnLine1, warnLine2, and tint sub-objects.
    if (slot.perk) {
        slot.perk->icon1.quad.setColor(pv, pv, pv, 0xFF);
        slot.perk->icon2.quad.setColor(pv, pv, pv, 0xFF);
        slot.perk->tint.setColor(pv, pv, pv);
    }

    // name TextItem rgba is a pure grey at the same intensity.
    slot.nameText.rgba = 0xFF000000u
                       | (uint32_t(pv) << 16)
                       | (uint32_t(pv) << 8)
                       | uint32_t(pv);
    slot.nameText.applyColor();

    // 3 desc TextItems are tinted green. binary uses a brighter green
    // when unselected (G channel 0xBE on grey 0xC8) and a dimmer green
    // when selected (G channel 0x8C on grey 0x96).
    uint8_t  descGrey  = selected ? 0x96 : 0xC8;
    uint8_t  descGreen = selected ? 0x8C : 0xBE;
    uint32_t descColor = 0xFF000000u
                       | (uint32_t(descGrey)  << 16)
                       | (uint32_t(descGreen) <<  8)
                       |  uint32_t(descGrey);

    for (int i = 0; i < 3; ++i) {
        slot.descText[i].rgba = descColor;
        slot.descText[i].applyColor();
    }
}

// FUN_10002db34, drain next stat pick.
//
// returns 0 if no more stat picks remain in the selected-picks list.
// otherwise: removes the entry from the list, writes the stat magnitude
// (= statSlots[slotIdx].value) into *outVal, returns the stat-type code:
//   slot 0 -> 2 (ATK)
//   slot 1 -> 6 (HP)
//   slot 2 -> 3 (DEF)
// these codes match the switch in gameBoardUpdate (the inner switch loop
// at "switch (FUN_10002db34(panel, &local_64))"). see DAT_10005a0e4 for
// the per-slot stat-type mapping.
int LevelUpPanel::getNextStatPick(int* outVal) {

    // matches DAT_10005a0e4: 3 ints, one per stat slot.
    static constexpr int STAT_TYPE_CODE[3] = { 2, 6, 3 };

    for (auto it = drainList.begin(); it != drainList.end(); ++it) {
        int slotIdx = *it;

        if (slotIdx < STAT_SLOT_COUNT) {
            drainList.erase(it);
            *outVal = statSlots[slotIdx].value;
            return STAT_TYPE_CODE[slotIdx];
        }
    }

    return 0;
}

// FUN_10002dbe0, drain next perk pick.
//
// returns nullptr if no perk picks remain. otherwise removes the entry
// and returns the Perk* stored in perkSlots[slotIdx-3].perk.
Perk* LevelUpPanel::getNextPerkPick() {

    for (auto it = drainList.begin(); it != drainList.end(); ++it) {
        int slotIdx = *it;

        if (slotIdx >= STAT_SLOT_COUNT) {
            Perk* p = perkSlots[slotIdx - STAT_SLOT_COUNT].perk;
            drainList.erase(it);
            return p;
        }
    }

    return nullptr;
}

// FUN_10002da0c, panel close.
//
// gameBoardUpdate's commit-loop falls into close() when all picks have
// been drained. clears readyToCommit and hides via the 0-arg path of
// panelHide (clears secondaryVisible + audio cue).
void LevelUpPanel::close() {
    Menu::panelHide(/*justClearVisible=*/false);
    readyToCommit = 0;
}
