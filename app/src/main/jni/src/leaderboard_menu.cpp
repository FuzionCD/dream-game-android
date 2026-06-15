#include "leaderboard_menu.h"

#include "game.h"
#include "portrait_table.h"
#include "renderer.h"
#include "score_history.h"

#include <SDL2/SDL_log.h>
#include <cstdio>

// FUN_100036c54, LeaderboardMenu ctor (visual setup tail).
//
// Quad sub-object default ctors run automatically via the C++ placement-
// new chain in Game::create. Label's POD fields (scale, cachedSize) are
// zero from memset, so headerLabel.init() must be called explicitly to
// set scale=1024; addGlyph below divides UV pixel coords by scale, so
// without this the 9-slice chrome renders with INF UVs (= invisible).
//
// TextItem sub-objects in sections are not explicitly init'd here:
// open() writes scaleX/Y/rgba directly per row, and no displayed string
// contains a space (spaceMultiplier stays at 0 but is never read).
//
// per-difficulty section sub-ctors and ScoreRow init come from the
// default member ctor chain; every Quad/Label/TextItem fired its own
// in-place default-init when the surrounding placement-new ran.
void LeaderboardMenu::init() {
    visible = 0;
    pressed = 0;

    headerLabel.init();

    // ---- 3 chrome glyphs on the header label ----
    //
    // binary @ 0x100036da4..0x100036e34: 3 calls to Label::addGlyph
    // (FUN_10004c310) with depth=-1.0, no offset, sizeModes 2/1/2.
    constexpr GlyphOffset kNoOffset{0.0f, 0.0f};

    {
        const float uv0[2]   = {718.0f, 246.0f};
        const float size0[2] = {26.0f, 68.0f};
        headerLabel.addGlyph(-1.0f, uv0, size0, 2, kNoOffset);

        const float uv1[2]   = {744.0f, 246.0f};
        const float size1[2] = {1.0f, 68.0f};
        headerLabel.addGlyph(-1.0f, uv1, size1, 1, kNoOffset);

        const float uv2[2]   = {745.0f, 246.0f};
        const float size2[2] = {26.0f, 68.0f};
        headerLabel.addGlyph(-1.0f, uv2, size2, 2, kNoOffset);
    }

    // ---- close-X chrome Quad UV + size ----
    //
    // binary @ 0x100036e3c..0x100036e60: setTexCoords + setSize.
    chromeQuad.setTexCoords(0.6855469f, 0.5966797f,
                            0.90234375f, 0.6279297f);
    chromeQuad.setSize(0.346875f, 0.05f);

    // ---- header label finalization ----
    //
    // binary @ 0x100036e60..0x100036e6c:
    //   bl  measureGlyphRun(label, -1, -1)   ; returns (s0=advWidth, s1=heightTotal)
    //   fmov s0, 0.46875                     ; overwrites s0 only (width)
    //   bl  setSize(label, width=s0, height=s1)
    //
    // s1 (heightTotal) is a dropped paired return: Ghidra's decomp drops it
    // entirely, so the call looks like setSize(0.46875, <garbage>), but it's
    // actually the measured height of the 3 chrome glyphs. without it the
    // 9-slice's middle-row vertical stretch collapses and the title box
    // drifts off the chromeQuad anchor.
    const GlyphRunMetrics m = headerLabel.measureGlyphRun(-1, -1);
    headerLabel.setSize(0.46875f, m.heightTotal);
}

// FUN_100037118, LeaderboardMenu::open.
//
// per-difficulty rank rebuild + visual layout. constants extracted verbatim
// from DAT_10005a1e4..DAT_10005a214 (the function's DAT block, 13 floats).
//
// layout per difficulty:
//   - diffLabel: name string + per-diff color, centered at (0.5, Y_diff)
//     where Y_diff = headerLabel.topY shifted by section index * 270/640.
//   - divider1 / divider2: thin horizontal bars flanking the diff label,
//     fading toward the label center via per-vertex alpha (vertex 1+3 get
//     alpha 0x50). divider1 is horizontally flipped after coloring so the
//     gradient falls away from the label on both sides.
//   - emptyMarker (per row): a thin bar spanning the section row, used
//     as the visible row chrome when populated == 0. width = divider2.right
//     - divider1.left.
//   - populated rows additionally draw a character portrait (tinted per
//     difficulty) + 5 stat numbers (levels/items/worlds/turns/score) and
//     their 4 singular/plural unit labels (no label for score).
//
// after the layout, calls update(0, 0) to prime the touch-state machine
// (FUN_100037b34's trailing tick at the end of FUN_100037118).
void LeaderboardMenu::open() {
    // section 1: state flags. visible/closeRequested packed at +0,
    // pressed/backTapConfirmed packed at +0x178.
    visible          = 1;
    closeRequested   = 0;
    pressed          = 0;
    backTapConfirmed = 0;

    // section 2: header label position. centered horizontally, anchored
    // virtualHeight - 0.125 from the bottom of the screen.
    const float headerWidth = headerLabel.getWidth();
    const float headerY     = Renderer::getVirtualHeight() - 0.125f;
    headerLabel.setPosition(0.5f - headerWidth * 0.5f, headerY);

    // section 3: chrome quad position. centered on the header label's
    // visual midpoint (FUN_10000aefc returns paired (centerX, centerY)).
    // posY = centerY + (-1/640), not topY + (-1/640): Ghidra dropped the
    // s1 paired-return and the decompile fell back to fVar28 (topY), which
    // would put the chrome decoration a full half-height above the strip.
    // asm 0x100037268: `fadd s0, s1, s2` uses s1 (centerY).
    const float headerCenterX = headerLabel.getLeftX()
                              + headerLabel.getWidth()  * 0.5f;
    const float headerCenterY = headerLabel.topY
                              + headerLabel.getHeight() * 0.5f;
    chromeQuad.posX = headerCenterX;
    chromeQuad.posY = headerCenterY + (-1.0f / 640.0f);

    // section 4: per-difficulty loop. binary @ 0x100037278..0x100037ae0.
    Game* game = getGame();

    // DAT constants used inside the loop (kept as named consts so reads
    // stay verifiable against the binary's stack-spill layout):
    constexpr float kDiffYCalibration = -1.265625f;            // DAT_10005a1e8
    constexpr float kDiffYBaseline    =  30.0f / 640.0f;       // DAT_10005a1ec
    constexpr float kDiffYStridePx    =  270.0f;
    constexpr float kDividerWidthCut  = -60.0f / 640.0f;       // DAT_10005a1f4
    constexpr float kDividerXOffset   = -20.0f / 640.0f;       // DAT_10005a1f8
    constexpr float kDividerYOffset   = -10.0f / 640.0f;       // DAT_10005a1fc
    constexpr float kDividerHeight    =   5.0f / 640.0f;
    constexpr float kEmptyMarkerYStep =  40.0f / 640.0f;       // DAT_10005a200
    constexpr float kEmptyMarkerHeight=  20.0f / 640.0f;
    constexpr float kPortraitXOffset  =  20.0f / 640.0f;       // DAT_10005a204
    constexpr float kLevelsRightX     = 225.0f / 640.0f;       // DAT_10005a208
    constexpr float kItemsRightX      = 325.0f / 640.0f;       // DAT_10005a20c
    constexpr float kScoreXGap        =  -5.0f / 640.0f;       // DAT_10005a210
    constexpr float kUnitLabelXGap    =   5.0f / 640.0f;       // DAT_10005a214
    constexpr float kWorldsLabelRightX=  0.1796875f;
    constexpr float kTurnsLabelRightX =  0.6875f;
    constexpr float kRowYStridePx     =  42.0f;                // row spacing
    constexpr float kDiffLabelScale   =  0.06f;
    constexpr float kNumberScale      =  0.07f;
    constexpr float kPortraitScale    =  0.5f;
    constexpr uint8_t kEmptyMarkerAlpha = 0x28;                // 40
    constexpr uint8_t kDividerFadeAlpha = 0x50;                // 80
    constexpr uint8_t kUnitLabelAlpha   = 0x82;                // 130

    // per-difficulty colors. all packed RGBA (R | G<<8 | B<<16 | A<<24).
    struct DiffPalette {
        uint32_t labelRgba;      // diff label + divider tint + count number tint base
        uint32_t portraitRgba;   // per-diff portrait quad tint
        const char* name;
    };
    static constexpr DiffPalette kPalette[3] = {
        // Easy:   R=255 G=255 B=150 A=255  / portrait: R=255 G=255 B=200 A=255
        { 0xff96ffffu, 0xffc8ffffu, "Easy"   },
        // Normal: R=0   G=210 B=0   A=255  / portrait: R=150 G=255 B=100 A=255
        { 0xff00d200u, 0xff64ff96u, "Normal" },
        // Hard:   R=255 G=50  B=0   A=255  / portrait: R=255 G=130 B=50  A=255
        { 0xff0032ffu, 0xff3282ffu, "Hard"   },
    };

    for (int diff = 0; diff < 3; diff++) {
        Section& sec = sections[diff];
        const DiffPalette& pal = kPalette[diff];

        // ---- 4a: diff label name + color + scale + font ----
        sec.diffLabel.glyphTablePtr = (game != nullptr)
                                      ? game->bmfontTablePtr(2)
                                      : nullptr;
        sec.diffLabel.scaleX = kDiffLabelScale;
        sec.diffLabel.scaleY = kDiffLabelScale;
        sec.diffLabel.setString(pal.name, -1);
        sec.diffLabel.rgba = pal.labelRgba;
        sec.diffLabel.applyColor();

        // ---- 4b: diff label position (horizontally centered, Y stacked) ----
        const float labelCenteredX =
            0.5f - sec.diffLabel.renderedWidth * sec.diffLabel.scaleX * 0.5f;

        // binary @ 0x100037368: bl FUN_10000aefc, paired (centerX, centerY).
        // asm 0x100037370 `fadd s0, s1, s0` uses s1 (centerY) for the Y math.
        // Ghidra dropped s1 and the decomp fell back to fVar28 (topY); using
        // topY here puts the diff sections half-a-header-height too high.
        const float headerCenterY = headerLabel.topY
                                  + headerLabel.getHeight() * 0.5f;
        const float labelY =
            (static_cast<float>(diff) * kDiffYStridePx) / 640.0f
            + (headerCenterY + kDiffYCalibration) * 0.5f
            + kDiffYBaseline;
        sec.diffLabel.posX = labelCenteredX;
        sec.diffLabel.posY = labelY;

        // ---- 4c: divider1 (left of label) ----
        //
        // width = labelCenteredX - 0.09375 (a horizontal bar from screen
        // left to just left of the label). then color = diffLabel.rgba on
        // all 4 verts, then vertex 1+3 get alpha 0x50 (right side fades).
        // finally horizontally-flip the Quad so the gradient runs
        // right-to-left (fade falls away from the label center).
        sec.divider1.setSize(labelCenteredX + kDividerWidthCut,
                             kDividerHeight);
        sec.divider1.posX = (sec.diffLabel.posX + kDividerXOffset)
                            - sec.divider1.width * 0.5f;
        sec.divider1.posY = sec.diffLabel.posY + kDividerYOffset;

        // setColor(divider1, diffLabel.rgba) -> all 4 verts solid
        sec.divider1.vertices[0].color = pal.labelRgba;
        sec.divider1.vertices[1].color = pal.labelRgba;
        sec.divider1.vertices[2].color = pal.labelRgba;
        sec.divider1.vertices[3].color = pal.labelRgba;

        // setColor right column (vertices 1 + 3) -> fade-toward-label end.
        // FUN_1000082a4 with mode=0 selectively touches verts 1 and 3.
        // alpha replaced; RGB inherited from diffLabel.
        {
            const uint32_t dimRgba =
                (pal.labelRgba & 0x00FFFFFFu)
                | (static_cast<uint32_t>(kDividerFadeAlpha) << 24);
            sec.divider1.vertices[1].color = dimRgba;
            sec.divider1.vertices[3].color = dimRgba;
        }

        // FUN_100008474: horizontal-flip, negate all vertex X.
        sec.divider1.vertices[0].x = -sec.divider1.vertices[0].x;
        sec.divider1.vertices[1].x = -sec.divider1.vertices[1].x;
        sec.divider1.vertices[2].x = -sec.divider1.vertices[2].x;
        sec.divider1.vertices[3].x = -sec.divider1.vertices[3].x;

        // ---- 4d: divider2 (right of label, mirror of divider1) ----
        sec.divider2.setSize(sec.divider1.width, sec.divider1.height);
        sec.divider2.posX = 1.0f - sec.divider1.posX;
        sec.divider2.posY = sec.divider1.posY;

        // setColor with diffLabel rgba + 0xff alpha (= solid).
        {
            const uint32_t solidRgba =
                (pal.labelRgba & 0x00FFFFFFu) | 0xFF000000u;
            sec.divider2.vertices[0].color = solidRgba;
            sec.divider2.vertices[1].color = solidRgba;
            sec.divider2.vertices[2].color = solidRgba;
            sec.divider2.vertices[3].color = solidRgba;
        }
        // fade vertex 1 + 3 (right side, falls away from label).
        {
            const uint32_t dimRgba =
                (pal.labelRgba & 0x00FFFFFFu)
                | (static_cast<uint32_t>(kDividerFadeAlpha) << 24);
            sec.divider2.vertices[1].color = dimRgba;
            sec.divider2.vertices[3].color = dimRgba;
        }

        // ---- 4e: per-row inner loop (5 score rows) ----
        static const std::list<ScoreHistory::Entry> kEmptyList;
        const std::list<ScoreHistory::Entry>& list =
            (game != nullptr) ? game->scoreHistory().list(diff)
                              : kEmptyList;
        auto entryIt = list.begin();

        for (int row = 0; row < 5; row++) {
            ScoreRow& r = sec.rows[row];

            // 4e.i: emptyMarker size + position + color (alpha 0x28).
            const float emptyMarkerWidth =
                (sec.divider2.posX + sec.divider2.width * 0.5f)
              - (sec.divider1.posX - sec.divider1.width * 0.5f);
            r.emptyMarker.setSize(emptyMarkerWidth, kEmptyMarkerHeight);
            r.emptyMarker.posX = 0.5f;
            r.emptyMarker.posY = sec.diffLabel.posY
                               + kEmptyMarkerYStep
                               + (static_cast<float>(row) * kRowYStridePx)
                                 / 640.0f;
            r.emptyMarker.setColor((pal.labelRgba >>  0) & 0xFF,
                                   (pal.labelRgba >>  8) & 0xFF,
                                   (pal.labelRgba >> 16) & 0xFF,
                                   0xFF);
            r.emptyMarker.setAlpha(kEmptyMarkerAlpha);

            if (entryIt == list.end()) {
                // 4e.ii: no entry for this row, mark unpopulated, done.
                r.populated = 0;
                continue;
            }

            // 4e.iii: populated row. setPortraitVisual + portrait tint.
            const ScoreHistory::Entry& entry = *entryIt;
            setPortraitVisual(static_cast<int>(entry.characterIndex),
                              r.portrait);
            r.portrait.scaleX = kPortraitScale;
            r.portrait.scaleY = kPortraitScale;
            r.portrait.posX = (r.emptyMarker.posX
                              - r.emptyMarker.width * 0.5f)
                            + kPortraitXOffset;
            r.portrait.posY = r.emptyMarker.posY;
            r.portrait.setColor((pal.portraitRgba >>  0) & 0xFF,
                                (pal.portraitRgba >>  8) & 0xFF,
                                (pal.portraitRgba >> 16) & 0xFF,
                                (pal.portraitRgba >> 24) & 0xFF);

            // 4e.iv: 5 stat-number TextItems. each gets bmfontTable(1),
            // scale 0.07, the portrait's vertex 0 RGBA (= portrait tint),
            // applyColor, then setString of "%d" of its field.
            //
            // worlds is the first per-row TextItem set up by the binary
            // (at x28+0x680 = worldsCount). its posY = portrait.posY +
            // maxCharHeight * scaleY * 0.5 (vertical center on portrait).
            char buf[16];

            auto setupNumberTextItem =
                [&](TextItem& ti, uint32_t value, float rightAlignX) {
                    ti.glyphTablePtr = (game != nullptr)
                                       ? game->bmfontTablePtr(1)
                                       : nullptr;
                    ti.scaleX = kNumberScale;
                    ti.scaleY = kNumberScale;
                    ti.rgba   = pal.portraitRgba;
                    ti.applyColor();
                    std::snprintf(buf, sizeof(buf), "%d",
                                  static_cast<int>(value));
                    ti.setString(buf, -1);
                    ti.posX = rightAlignX - ti.renderedWidth * ti.scaleX;
                };

            // worldsCount (first one set up by binary, at +0x680).
            setupNumberTextItem(r.worldsCount, entry.worlds,
                                kWorldsLabelRightX);
            r.worldsCount.posY = r.portrait.posY
                               + r.worldsCount.maxCharHeight
                                 * r.worldsCount.scaleY * 0.5f;

            // levelsCount (binary's second, at +0x570). shares Y with
            // worldsCount, as do the rest.
            setupNumberTextItem(r.levelsCount, entry.levels,
                                kLevelsRightX);
            r.levelsCount.posY = r.worldsCount.posY;

            // itemsCount (binary's third, at +0x5f8).
            setupNumberTextItem(r.itemsCount, entry.items, kItemsRightX);
            r.itemsCount.posY = r.worldsCount.posY;

            // turnsCount (binary's fourth, at +0x708).
            setupNumberTextItem(r.turnsCount, entry.turns,
                                kTurnsLabelRightX);
            r.turnsCount.posY = r.worldsCount.posY;

            // scoreCount (binary's fifth, at +0x790). right-aligned to
            // emptyMarker.right + kScoreXGap.
            const float emptyMarkerRight = r.emptyMarker.posX
                                         + r.emptyMarker.width * 0.5f
                                         + kScoreXGap;
            setupNumberTextItem(r.scoreCount,
                                static_cast<uint32_t>(entry.score),
                                emptyMarkerRight);
            r.scoreCount.posY = r.worldsCount.posY;

            // 4e.v: 4 unit-label TextItems. font + scale + color same as
            // numbers, but alpha overridden to 0x82 via setAlpha after
            // applyColor. plural form chosen by count != 1.
            auto setupUnitLabel =
                [&](TextItem& ti, const char* singular,
                    const char* plural, uint32_t count,
                    const TextItem& anchorNumber, float xGap) {
                    ti.glyphTablePtr = (game != nullptr)
                                       ? game->bmfontTablePtr(1)
                                       : nullptr;
                    ti.scaleX = kNumberScale;
                    ti.scaleY = kNumberScale;
                    ti.rgba   = pal.portraitRgba;
                    ti.applyColor();
                    ti.alpha = kUnitLabelAlpha;
                    ti.setString(count == 1 ? singular : plural, -1);
                    ti.posX = anchorNumber.posX
                            + anchorNumber.renderedWidth
                              * anchorNumber.scaleX
                            + xGap;
                    ti.posY = anchorNumber.posY;
                };

            // worldsLabel (binary's first label, at +0x928), anchored to
            // worldsCount.
            setupUnitLabel(r.worldsLabel, "world", "worlds",
                           entry.worlds, r.worldsCount, kUnitLabelXGap);

            // levelsLabel (binary's second label, at +0x818).
            setupUnitLabel(r.levelsLabel, "level", "levels",
                           entry.levels, r.levelsCount, kUnitLabelXGap);

            // itemsLabel (binary's third label, at +0x8a0).
            setupUnitLabel(r.itemsLabel, "item", "items",
                           entry.items, r.itemsCount, kUnitLabelXGap);

            // turnsLabel (binary's fourth label, at +0x9b0).
            setupUnitLabel(r.turnsLabel, "turn", "turns",
                           entry.turns, r.turnsCount, kUnitLabelXGap);

            r.populated = 1;
            ++entryIt;
        }
    }

    // section 5: trailing prime tick. binary calls FUN_100037b34 with
    // touchInput at (0, 0). matches Shop's open() prime tick pattern.
    update(0.0f, 0.0f);
}

// FUN_100037b34, LeaderboardMenu::update.
//
// per-frame touch input handler. on every entry: clears backTapConfirmed.
// on press (game.inputState == 1): hit-tests the header Label. hit ->
// pressed=1 + tint header+chrome grey; miss -> closeRequested=1 (the
// tap-anywhere-to-close UX: the back-button icon is a visual hint but any
// non-back-button tap is what actually closes the menu).
// on release while pressed: clears pressed, restores white tint, re-
// hit-tests, sets backTapConfirmed=1 if still on the button.
//
// note: backTapConfirmed is written but no C++ core code reads it; the
// close is driven entirely by closeRequested. we mirror the binary.
void LeaderboardMenu::update(float touchX, float touchY) {
    backTapConfirmed = 0;

    Game* game = getGame();

    if (game == nullptr) {
        return;
    }

    const int phase = game->inputState();

    if (phase == 1) {
        // press
        const bool hit = headerLabel.contains(touchX, touchY);

        if (hit) {
            pressed = 1;
            headerLabel.setColor(0xb4, 0xb4, 0xb4, 0xff);
            chromeQuad.setColor(0xb4, 0xb4, 0xb4, 0xff);
        }
        else {
            closeRequested = 1;
        }
    }

    if (phase == 0 && pressed != 0) {
        // released while was-pressed
        pressed = 0;
        headerLabel.setColor(0xff, 0xff, 0xff, 0xff);
        chromeQuad.setColor(0xff, 0xff, 0xff, 0xff);

        if (headerLabel.contains(touchX, touchY)) {
            backTapConfirmed = 1;
        }
    }
}

// FUN_100037c28, LeaderboardMenu::draw.
//
// 3-pass texture-bind sweep:
//   tex 0  -> per-diff divider Quads + diffLabel Label + per-row
//             emptyMarker Quads (only when populated == 0)
//   tex 8  -> per-row portrait Quad + 4 unit-label TextItems + 5 stat
//             TextItems (only when populated == 1)
//   tex 9  -> header Label + close-X chrome Quad
void LeaderboardMenu::draw() {

    if (!visible) {
        return;
    }

    for (int diff = 0; diff < 3; diff++) {
        Section& sec = sections[diff];

        bindTexture(0);
        sec.divider1.draw();
        sec.divider2.draw();
        sec.diffLabel.draw();

        for (int row = 0; row < 5; row++) {
            ScoreRow& r = sec.rows[row];

            if (r.populated == 0) {
                bindTexture(0);
                r.emptyMarker.draw();
            }
            else {
                bindTexture(8);
                r.portrait.draw();
                r.worldsLabel.draw();
                r.levelsLabel.draw();
                r.itemsLabel.draw();
                r.turnsLabel.draw();
                r.worldsCount.draw();
                r.levelsCount.draw();
                r.itemsCount.draw();
                r.turnsCount.draw();
                r.scoreCount.draw();
            }
        }
    }

    bindTexture(9);
    headerLabel.draw();
    chromeQuad.draw();
}

// FUN_100037ebc, LeaderboardMenu::dirtyXfer.
//
// clears `dirty`, clears the destination vector, walks all 3 ScoreHistory
// lists pushing each entry as an XferEntry. iOS pipes this vector to
// GameKit's updateLeaderboardScores; on Android nothing consumes the
// staged data but we port the staging step for binary fidelity.
//
// note: XferEntry.worldIndex is the loop index (diff 0/1/2) the entry was
// found under, not entry.worldIndex. these should be identical (each entry
// lives in its difficulty's list) but the binary uses the loop var
// explicitly so we mirror that.
void LeaderboardMenu::dirtyXfer(std::vector<XferEntry>& dest) {
    dirty = 0;
    dest.clear();

    Game* game = getGame();

    if (game == nullptr) {
        return;
    }

    for (int diff = 0; diff < 3; diff++) {
        const std::list<ScoreHistory::Entry>& list =
            game->scoreHistory().list(diff);

        for (const ScoreHistory::Entry& entry : list) {
            XferEntry xfer;
            xfer.worldIndex     = diff;
            xfer.characterIndex = static_cast<int32_t>(entry.characterIndex);
            xfer.levels         = static_cast<int32_t>(entry.levels);
            xfer.items          = static_cast<int32_t>(entry.items);
            xfer.worlds         = static_cast<int32_t>(entry.worlds);
            xfer.turns          = static_cast<int32_t>(entry.turns);
            xfer.score          = entry.score;
            dest.push_back(xfer);
        }
    }
}

// FUN_100038004, LeaderboardMenu::restoreFromSave.
//
// walks the saved XferEntry vector, calling ScoreHistory::insertEntry for
// each. dirty propagation is handled inside insertEntry (matches the
// binary's FUN_100037d3c first-line side effect).
void LeaderboardMenu::restoreFromSave(const std::vector<XferEntry>& saved) {
    Game* game = getGame();

    if (game == nullptr) {
        return;
    }

    for (const XferEntry& e : saved) {
        game->scoreHistory().insertEntry(
            static_cast<uint32_t>(e.worldIndex),
            static_cast<uint32_t>(e.characterIndex),
            static_cast<uint32_t>(e.levels),
            static_cast<uint32_t>(e.items),
            static_cast<uint32_t>(e.worlds),
            static_cast<uint32_t>(e.turns),
            e.score);
    }
}
