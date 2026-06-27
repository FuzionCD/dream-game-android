#include "shop_menu.h"

#include "event_slot.h"   // EventSlot dtor for heapPreview cleanup
#include "game.h"         // getGame(), bmfontTablePtr
#include "portrait_table.h"  // setPortraitVisual (= FUN_100056478)
#include "snag_content.h"    // applySpriteUV (= FUN_100014d84)
#include "snag_table.h"      // snagInfo lookup
#include "random.h"       // rngInt
#include "renderer.h"     // bindTexture

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {

// FUN_10000f800 / FUN_1000544a4 / FUN_100054544, std::set<int> rng-pop.
// the binary uses 3 distinct helpers (one per comparator template instance);
// libc++ std::set<int> + the existing rngInt give us a single C++ port.
int rngPopSet(std::set<int>& s, uint32_t stream) {
    int idx = rngInt(0, static_cast<int>(s.size()) - 1, stream);
    auto it = std::next(s.begin(), idx);
    int v = *it;
    s.erase(it);
    return v;
}

}   // namespace

// FUN_1000550b0, ShopUnlockRow::init.
//
// the binary inits rowFrame (Label::init FUN_10004c014), the 3 chrome Quads
// (FUN_100007d78), the 3 TextItems (FUN_10002fa08), and the 12 preview-slot
// Quads. Quads are default-constructed by Shop::init's placement-new, so
// this method only needs the Label and TextItem inits.
void ShopUnlockRow::init() {
    rowFrame.init();
    title.init();
    description.init();
    unlockedCountText.init();
}

// FUN_100051a14, Shop::seedPools.
//
// clears the 3 pool sets + 3 unlocks vectors, then re-inserts the
// canonical face / snag / event ID lists into the sets. the binary uses
// 3 different insert helpers (FUN_1000101ac / FUN_10004eebc /
// FUN_1000103d8) because each set has a distinct comparator template
// instantiation; std::set::insert is the libc++-equivalent call.
void Shop::seedPools() {
    facePool.clear();
    snagPool.clear();
    eventPool.clear();
    faceUnlocks.clear();
    snagUnlocks.clear();
    eventUnlocks.clear();

    // face pool: IDs 0..29 except 4 and 0x11. 28 entries total.
    for (int i = 0; i < 30; i++) {

        if (i != 4 && i != 0x11) {
            facePool.insert(i);
        }
    }

    // snag pool: 20 specific snag IDs (binary lists them inline via 20
    // sequential FUN_10004eebc calls in FUN_100051a14).
    static constexpr int kSnagPool[20] = {
        5, 10, 0x0E, 0x17, 0x1A, 0x1E, 0x24, 0x29, 0x2F, 0x31,
        0x33, 0x34, 0x3A, 0x3C, 0x41, 0x44, 0x52, 0x5D, 0x62, 0x6A,
    };

    for (int id : kSnagPool) {
        snagPool.insert(id);
    }

    // event pool: 15 specific event IDs.
    static constexpr int kEventPool[15] = {
        0, 0x11, 0x35, 0x36, 0x27, 0x0A, 0x10, 0x26, 0x3E, 0x0B,
        0x2E, 0x3B, 0x0E, 0x31, 0x4D,
    };

    for (int id : kEventPool) {
        eventPool.insert(id);
    }
}

// FUN_1000502d4, Shop::init.
//
// big chrome-construction function. called once from Game::create at
// `bl 0x100051e1c` (FUN_1000437a4 +0x880). orchestrates:
//   1. zero the 3 visibility flags
//   2. init the header Label + the 3 rowFrame Labels (each owns 3 TextItems)
//   3. seed the pool sets
//   4. install 6-glyph 9-slice frame on the header Label + size + position
//   5. for each of the 3 rows: install 4-glyph rowFrame + iconQuad UV/size,
//      position iconQuad, then branch on row index for priceQuad/unlockedQuad
//      UV/size, position priceQuad, copy pos to unlockedQuad, then inner
//      loop for previewSlot Quads (slot count = rowIndex + 1)
//   6. tail block: install the 7 chrome Quads' UV/size + positions, plus
//      the 5 animQuad UV/size + colors
//
// Quad / std::list / std::set / std::vector default state comes from C++
// placement-new in Game::create; this method only does the post-default
// chrome layout work.
//
// the binary's `do { ... } while (true)` with an early-return on the row
// counter reaching 3 is modeled as a plain for(i=0..2) loop, the chrome
// tail running unconditionally after.
void Shop::init() {

    // ---- 1. zero flags ----
    visible        = false;
    closeRequested = false;
    dirty          = false;

    // ---- 2. Label inits ----
    headerLabel.init();

    for (int i = 0; i < 3; i++) {
        rows[i].init();
    }

    // ---- 3. seed pools ----
    seedPools();

    // ---- 4. header Label: 6 glyphs forming a 3x2 9-slice frame ----
    //
    // each addGlyph call: (overrideHeight=-1, uvOriginPx, uvSizePx,
    // sizeMode, vertexOffset). pendingLineIndex advances after every
    // 2 glyphs to break into the next row of the 9-slice.
    //
    // sizeMode meanings (from layoutGlyphs):
    //   0 = horizontal-edge cell  (stretches X to fill, fixed natural Y)
    //   1 = vertical-edge cell    (stretches Y to fill, fixed natural X)
    //   2 = corner cell           (natural width and height)
    //   3 = interior              (stretches both axes)
    constexpr GlyphOffset kNoOffset{0.0f, 0.0f};

    {
        // top row: corner (44, 36) + edge (1, 36) at UV (211, 195)
        const float uv0[2]   = {211.0f, 195.0f};
        const float size0[2] = {44.0f, 36.0f};
        headerLabel.addGlyph(-1.0f, uv0, size0, 2, kNoOffset);

        const float uv1[2]   = {255.0f, 195.0f};
        const float size1[2] = {1.0f, 36.0f};
        headerLabel.addGlyph(-1.0f, uv1, size1, 1, kNoOffset);
    }

    headerLabel.pendingLineIndex++;

    {
        // middle row: edge (44, 1) + interior (1, 1) at UV (211, 231)
        const float uv0[2]   = {211.0f, 231.0f};
        const float size0[2] = {44.0f, 1.0f};
        headerLabel.addGlyph(-1.0f, uv0, size0, 0, kNoOffset);

        const float uv1[2]   = {255.0f, 231.0f};
        const float size1[2] = {1.0f, 1.0f};
        headerLabel.addGlyph(-1.0f, uv1, size1, 3, kNoOffset);
    }

    headerLabel.pendingLineIndex++;

    {
        // bottom row: corner (44, 49) + edge (1, 49) at UV (211, 232)
        const float uv0[2]   = {211.0f, 232.0f};
        const float size0[2] = {44.0f, 49.0f};
        headerLabel.addGlyph(-1.0f, uv0, size0, 2, kNoOffset);

        const float uv1[2]   = {255.0f, 232.0f};
        const float size1[2] = {1.0f, 49.0f};
        headerLabel.addGlyph(-1.0f, uv1, size1, 1, kNoOffset);
    }

    headerLabel.setSize(0.65625f, 0.640625f);   // DAT_10005a5d8 = 0.640625
    headerLabel.setPosition(0.15625f, 0.2875f); // DAT_10005a5dc = 0.2875

    // ---- DAT_ constants shared across the row + tail blocks ----
    constexpr float kRowFrameWidth     = 0.884375f;   // DAT_10005a5e0
    constexpr float kPixelScale        = 640.0f;      // DAT_10005a5e4
    constexpr float kRowFramePosX      = 0.0625f;     // DAT_10005a5e8
    constexpr float kIconOffsetXY      = 0.096875f;   // DAT_10005a5ec
    constexpr float kPriceOffsetX      = 0.003125f;   // DAT_10005a5f0
    constexpr float kPriceOffsetY      = 0.0046875f;  // DAT_10005a5f4
    constexpr float kPrevR1Iter1X      = -0.0828125f; // DAT_10005a5f8
    constexpr float kPrevR1Iter1Y      = -0.015625f;  // DAT_10005a5fc
    constexpr float kPrevR2Iter0X      = -0.1203125f; // DAT_10005a600
    constexpr float kPrevR0OrR1Iter0X  = -0.1015625f; // DAT_10005a604
    constexpr float kPrevR2Iter2Y      = 0.0171875f;  // DAT_10005a608
    constexpr float kPrevR1Iter30X     = -0.121875f;  // DAT_10005a60c
    constexpr float kPrevDecBXOffset   = -0.2554688f; // DAT_10005a610
    constexpr float kPrevDecCXOffset   = -0.0015625f; // DAT_10005a614

    // ---- 5. per-row chrome ----
    for (int rowIndex = 0; rowIndex < 3; rowIndex++) {
        ShopUnlockRow& row = rows[rowIndex];

        // -- rowFrame 9-slice: 4 glyphs (top corner / top edge / bottom
        //    corner / bottom edge - all at large pixel coords). last glyph
        //    has a tiny Y vertex offset (0.04375 = bits 0x3d333333).
        {
            const float uv0[2]   = {488.0f, 81.0f};
            const float size0[2] = {20.0f, 124.0f};
            row.rowFrame.addGlyph(-1.0f, uv0, size0, 2, kNoOffset);

            const float uv1[2]   = {508.0f, 81.0f};
            const float size1[2] = {1.0f, 124.0f};
            row.rowFrame.addGlyph(-1.0f, uv1, size1, 1, kNoOffset);

            const float uv2[2]   = {616.0f, 616.0f};
            const float size2[2] = {36.0f, 124.0f};
            row.rowFrame.addGlyph(-1.0f, uv2, size2, 2, kNoOffset);

            const float uv3[2]   = {652.0f, 644.0f};
            const float size3[2] = {116.0f, 70.0f};
            constexpr GlyphOffset kLastOffset{0.0f, 0.04375f};
            row.rowFrame.addGlyph(-1.0f, uv3, size3, 2, kLastOffset);
        }

        // -- size/position the rowFrame. width = constant 0.884375;
        //    height = rowFrame.glyphs[0].quad.height (set by addGlyph from
        //    the natural pixel-size 124/1024). row Y stacks down from
        //    headerLabel.topY by (rowIndex * 128 - 50) / 640 = -0.078,
        //    +0.122, +0.322 (per disasm at 0x10005086c: paired-return s1
        //    of getLeftX(headerLabel) feeds into the setPosition Y arg).
        //    using the cached headerLabel.topY field is equivalent.
        const float frameHeight = row.rowFrame.glyphs[0].quad.height;
        row.rowFrame.setSize(kRowFrameWidth, frameHeight);
        const float rowY = headerLabel.topY
                           + (float)(rowIndex * 128 - 50) / kPixelScale;
        row.rowFrame.setPosition(kRowFramePosX, rowY);

        // -- iconQuad UV + size + position (UV span (0.084..0.184,
        //    0.827..0.927), size 0.159 square; positioned at rowFrame's
        //    bbox + kIconOffsetXY in both axes).
        row.iconQuad.setTexCoords(0.083984375f, 0.82714844f,
                                  0.18359375f,  0.9267578f);
        row.iconQuad.setSize(0.159375f, 0.159375f);

        const float iconX = row.rowFrame.getLeftX() + kIconOffsetXY;
        const float iconY = rowY + kIconOffsetXY;
        row.iconQuad.posX = iconX;
        row.iconQuad.posY = iconY;

        // -- priceQuad + unlockedQuad UVs vary by row index (each row
        //    shows a different icon set in the binary's UV atlas).
        if (rowIndex == 0) {
            row.priceQuad.setTexCoords(0.27246094f, 0.8408203f,
                                       0.359375f,  0.9267578f);
            row.priceQuad.setSize(0.1390625f, 0.1375f);
            row.unlockedQuad.setTexCoords(0.5361328f, 0.8251953f,
                                          0.6230469f, 0.9111328f);
            row.unlockedQuad.setSize(0.1390625f, 0.1375f);
        }
        else if (rowIndex == 1) {
            row.priceQuad.setTexCoords(0.36035156f, 0.8408203f,
                                       0.44726563f, 0.9267578f);
            row.priceQuad.setSize(0.1390625f, 0.1375f);
            row.unlockedQuad.setTexCoords(0.62402344f, 0.8251953f,
                                          0.7109375f,  0.9111328f);
            row.unlockedQuad.setSize(0.1390625f, 0.1375f);
        }
        else /* rowIndex == 2 */ {
            row.priceQuad.setTexCoords(0.18457031f, 0.8408203f,
                                       0.27148438f, 0.9267578f);
            row.priceQuad.setSize(0.1390625f, 0.1375f);
            row.unlockedQuad.setTexCoords(0.4482422f,  0.8408203f,
                                          0.53515625f, 0.9267578f);
            row.unlockedQuad.setSize(0.1390625f, 0.1375f);
        }

        // -- priceQuad position = iconQuad pos + (priceOffsetX, priceOffsetY).
        //    snap to pixel grid first, then unlockedQuad gets the snapped
        //    position (the two overlap, so they must share one snapped pos).
        const float priceX = iconX + kPriceOffsetX;
        const float priceY = iconY + kPriceOffsetY;
        row.priceQuad.posX = priceX;
        row.priceQuad.posY = priceY;
        row.priceQuad.snapToPixelGrid();
        row.unlockedQuad.posX = row.priceQuad.posX;
        row.unlockedQuad.posY = row.priceQuad.posY;

        // -- inner loop: install up to (rowIndex + 1) preview slots. only
        //    populates as many slots as the row's index allows; later
        //    slots stay default-constructed.
        //
        // each iteration computes positions fresh from the rowFrame's
        // cached leftX/topY/width/height; there is no anchorY
        // propagation. ghidra's `fVar35 = fVar35 + fVar38 * 0.5` is
        // register-name reuse; the disasm at 0x100050c80-0x100050c8c
        // shows `s1 = topY + height * 0.5` recomputed per iter from the
        // paired returns of getLeftX/getWidth on rowFrame.
        int innerYStep      = 30;   // = iVar34 in binary, starts at 0x1E
        const int innerCount = rowIndex + 1;

        for (int slotIter = 0; slotIter < innerCount; slotIter++) {
            ShopPreviewSlot& slot = row.previews[slotIter];

            // refresh leftX/topY/width/height cache per iter (binary
            // calls getLeftX + getWidth twice per slot, first for the
            // s0 captures, second for the paired s1).
            const float frameLeftX = row.rowFrame.getLeftX();
            const float frameWidth = row.rowFrame.getWidth();
            const float frameTopY  = row.rowFrame.topY;
            const float frameHt    = row.rowFrame.cachedSize1;
            const float rightEdge  = frameLeftX + frameWidth;
            const float rowCenterY = frameTopY  + frameHt * 0.5f;

            // -- per-(row, slotIter) X/Y deltas applied on top of
            //    rightEdge / rowCenterY. lookup table mirrors the
            //    disasm at LAB_100050db8 / LAB_100050d6c / LAB_100050e0c
            //    + the branch entry points for each (row, iter) combo.
            //    Y delta is 0 except on row 2 (matches the binary's
            //    skipped `fadd s1, s1, s2` step on rows 0 and 1).
            float decXDelta = 0.0f;
            float decYDelta = 0.0f;

            if (rowIndex == 2) {

                if (slotIter == 1) {
                    decXDelta = kPrevR1Iter1X;       // DAT_10005a5f8
                    decYDelta = kPrevR1Iter1Y;       // DAT_10005a5fc
                }
                else if (slotIter == 0) {
                    decXDelta = kPrevR2Iter0X;       // DAT_10005a600
                    decYDelta = kPrevR1Iter1Y;       // DAT_10005a5fc
                }
                else /* slotIter == 2 */ {
                    decXDelta = kPrevR0OrR1Iter0X;   // DAT_10005a604
                    decYDelta = kPrevR2Iter2Y;       // DAT_10005a608
                }
            }
            else if (rowIndex == 1) {
                decXDelta = (innerYStep == 30) ? kPrevR1Iter30X    // 5a60c
                                                : kPrevR1Iter1X;    // 5a5f8
            }
            else /* rowIndex == 0 */ {
                decXDelta = kPrevR0OrR1Iter0X;       // DAT_10005a604
            }

            const float decAX = rightEdge  + decXDelta;
            const float decAY = rowCenterY + decYDelta;

            // -- decorationA: small icon at UV (0.842..0.871, 0.271..0.301),
            //    0.047 square. positioned at (rightEdge + dx, rowCenterY + dy).
            slot.decorationA.setTexCoords(0.8417969f,  0.27148438f,
                                          0.87109375f, 0.30078125f);
            slot.decorationA.setSize(0.046875f, 0.046875f);
            slot.decorationA.posX = decAX;
            slot.decorationA.posY = decAY;

            // -- decOrPad: another small UV block, same pos as decorationA.
            slot.decorationOrPad.setTexCoords(0.8173828f, 0.27148438f,
                                              0.8408203f, 0.29492188f);
            slot.decorationOrPad.setSize(0.0375f, 0.0375f);
            slot.decorationOrPad.posX = decAX;
            slot.decorationOrPad.posY = decAY;

            // -- decorationB: bigger swatch (0.077 x 0.073) at UV
            //    (0.129..0.177, 0.296..0.342). disasm 0x100050ed0-0x100050f08
            //    shows posY = rowFrame.topY + iVar34/640 (not rowCenterY).
            slot.decorationB.setTexCoords(0.12890625f, 0.29589844f,
                                          0.17675781f, 0.34179688f);
            slot.decorationB.setSize(0.0765625f, 0.0734375f);
            slot.decorationB.posX = rightEdge + kPrevDecBXOffset;
            slot.decorationB.posY = frameTopY + (float)innerYStep
                                                / kPixelScale;
            slot.decorationB.snapToPixelGrid();

            // -- decorationC: final small swatch at UV
            //    (0.881..0.920, 0.931..0.967), 0.0625 x 0.0578. position
            //    = decorationB shifted by (-0.0015625, 0).
            slot.decorationC.setTexCoords(0.8808594f,  0.93066406f,
                                          0.9199219f,  0.9667969f);
            slot.decorationC.setSize(0.0625f, 0.0578125f);
            slot.decorationC.posX = slot.decorationB.posX + kPrevDecCXOffset;
            slot.decorationC.posY = slot.decorationB.posY;
            slot.decorationC.snapToPixelGrid();

            innerYStep += 30;
        }
    }

    // ---- 6. chrome tail: 7 chrome Quads + 5 animQuads ----

    // chromeQuad0: top "Shop" badge. its posX (0.84375) is the reference
    // for the key-icon row in Shop::open. binary disasm at 0x100051070:
    //   chromeQuad0.posY = rowFrame[0].topY + DAT_10005a618
    // (paired-return s1 of getLeftX(rowFrame[0]) feeds the fadd).
    chromeQuad0.setTexCoords(0.45214844f, 0.26660156f,
                             0.5732422f,  0.3330078f);
    chromeQuad0.setSize(0.19375f, 0.10625f);
    rows[0].rowFrame.getLeftX();   // refresh leftX/topY cache
    chromeQuad0.posX = 0.84375f;   // bits 0x3f580000
    chromeQuad0.posY = rows[0].rowFrame.topY + (-0.0234375f);  // DAT_10005a618

    // chromeQuad1: thin horizontal divider line. UV is a 1-pixel slice
    // (vMin = 0.825, vMax = 0.826). height gets overridden below.
    chromeQuad1.setTexCoords(0.8857421875f,  0.8251953125f,
                             0.9990234375f,  0.826171875f);
    chromeQuad1.setSize(0.18125f, 0.0015625f);

    // re-derive chromeQuad1.height. binary disasm at 0x1000510dc-0x10005110c:
    //   v8 = paired-s1 of getLeftX(row[2])  = row[2].topY
    //   s1 = getWidth(row[2])                = row[2].height (s1 of getWidth =
    //                                          cachedSize1 when not mirrored)
    //   newH = (row[2].topY + row[2].height + DAT_10005a614)
    //          - (chromeQuad0.posY + DAT_10005a61c)
    rows[2].rowFrame.getLeftX();   // refresh topY cache
    const float row2TopY    = rows[2].rowFrame.topY;
    const float row2Height  = rows[2].rowFrame.getHeight();
    const float chrome1Height =
        (row2TopY + row2Height + kPrevDecCXOffset + 0.5f) // DEBUG: 0.5 is arbitrary, there was a graphical glitch in the past and I liked how it looked
        - (chromeQuad0.posY + 0.0296875f);  // DAT_10005a61c
    chromeQuad1.setSize(chromeQuad1.width, chrome1Height);

    chromeQuad1.posX = chromeQuad0.posX + (-0.0109375f);  // DAT_10005a620
    chromeQuad1.posY = chromeQuad0.posY + 0.0296875f
                       + chromeQuad1.height * 0.5f;

    // chromeQuad2: top-right icon button, 0.181 square. positioned above
    // chromeQuad1 (X-aligned with it).
    chromeQuad2.setTexCoords(0.88671875f, 0.0009765625f,
                             1.0f,        0.11425781f);
    chromeQuad2.setSize(0.18125f, 0.18125f);
    chromeQuad2.posX = chromeQuad1.posX;
    chromeQuad2.posY = chromeQuad1.posY + chromeQuad1.height * 0.5f
                       + chromeQuad2.height * 0.5f;

    // chromeQuad4: lower-right icon button, 0.2 square. binary processes
    // chromeQuad4 before chromeQuad3 so chromeQuad3's width can derive
    // from the gap between chromeQuad4 and chromeQuad2.
    chromeQuad4.setTexCoords(0.87402344f, 0.115234375f,
                             0.99902344f, 0.24023438f);
    chromeQuad4.setSize(0.2f, 0.2f);
    chromeQuad4.posX = 0.5015625f;   // bits 0x3f006666
    chromeQuad4.posY = chromeQuad2.posY + 0.0109375f;  // DAT_10005a624

    // chromeQuad3: vertical "connector" line. ctor UV+size is a thin
    // vertical (0.0016 x 0.181), then the width gets re-set to the
    // horizontal gap between chromeQuad2's left edge and chromeQuad4's
    // right edge, turning it into a horizontal connector at chromeQuad2's
    // Y. weird shape but that's what the binary builds.
    chromeQuad3.setTexCoords(0.4150390625f, 0.4658203125f,
                             0.416015625f,  0.5791015625f);
    chromeQuad3.setSize(0.0015625f, 0.18125f);
    {
        const float gapWidth = (chromeQuad2.posX - chromeQuad2.width * 0.5f)
                              - (chromeQuad4.posX + chromeQuad4.width * 0.5f);
        chromeQuad3.setSize(gapWidth, chromeQuad3.height);
    }
    chromeQuad3.posX = chromeQuad4.posX + chromeQuad4.width * 0.5f
                       + chromeQuad3.width * 0.5f;
    chromeQuad3.posY = chromeQuad2.posY;

    // chromeQuad5: thin horizontal divider, same UV pattern as chromeQuad1.
    // height bumped from 0.0015625 to 0.03125 via a second setSize call.
    chromeQuad5.setTexCoords(0.8857421875f, 0.8251953125f,
                             0.9990234375f, 0.826171875f);
    chromeQuad5.setSize(0.18125f, 0.0015625f);
    chromeQuad5.setSize(chromeQuad5.width, 0.03125f);  // bits 0x3d000000
    chromeQuad5.posX = chromeQuad4.posX + (-0.0125f);  // DAT_10005a628
    chromeQuad5.posY = chromeQuad4.posY + chromeQuad4.height * 0.5f
                       + chromeQuad5.height * 0.5f;

    // chromeQuad6: lower "Shop" badge (mirror of chromeQuad0). uses
    // DAT_10005a624 (= 0.0109375) for its X nudge, the same constant
    // chromeQuad4.posY used, since the binary's fVar17 register stays
    // loaded with that DAT through the chromeQuad6 block.
    chromeQuad6.setTexCoords(0.45214844f, 0.26660156f,
                             0.5732422f,  0.3330078f);
    chromeQuad6.setSize(0.19375f, 0.10625f);
    chromeQuad6.posX = chromeQuad5.posX + 0.0109375f;  // DAT_10005a624
    chromeQuad6.posY = chromeQuad5.posY + chromeQuad5.height * 0.5f
                       + 0.03125f;   // DAT_10005a62c

    // ---- animQuad UV / size / colors ----
    //
    // 5 animation overlay Quads. binary calls FUN_10000826c (= setColor)
    // on quads 0 and 1 with the packed RGBA value 0x96412814 (= R 20 /
    // G 40 / B 65 / A 150, a dim translucent blue). quads 2..4 keep the
    // Quad ctor's default white.
    animQuads[0].setTexCoords(0.3740234375f, 0.34765625f,
                              0.4267578125f, 0.41796875f);
    animQuads[0].setSize(0.084375f, 0.1125f);
    animQuads[0].setColor(20, 40, 65, 150);

    animQuads[1].setTexCoords(0.3486328125f, 0.5810546875f,
                              0.4169921875f, 0.6591796875f);
    animQuads[1].setSize(0.109375f, 0.125f);
    animQuads[1].setColor(20, 40, 65, 150);

    animQuads[2].setTexCoords(0.37402344f, 0.28027344f,
                              0.4169922f,  0.3466797f);
    animQuads[2].setSize(0.06875f, 0.10625f);

    animQuads[3].setTexCoords(0.3486328f,  0.46679688f,
                              0.4091797f,  0.5332031f);
    animQuads[3].setSize(0.096875f, 0.10625f);

    animQuads[4].setTexCoords(0.3486328f,  0.5341797f,
                              0.4111328f,  0.57910156f);
    animQuads[4].setSize(0.1f, 0.071875f);
}

// FUN_100054254, gated key add.
//
//   if (delta <= 0) return;
//   keys = clamp(keys + delta, 0, 20);   // via FUN_10005722c
//   dirty = true;
//
// dirty is later consumed by Shop::dirtyXfer (FUN_100054298), which on the
// next frame pushes keys + the 3 unlock pools into the persistent save buffer.
void Shop::addKeys(int delta) {

    if (delta <= 0) {
        return;
    }

    keys  = std::clamp(keys + delta, 0, 20);
    dirty = true;
}

// FUN_100052058, Shop::open.
//
// initial state flags + free leftover event-preview + per-row TextItem
// setup + key-icon list rebuild + primed update tick.
//
// open does not touch the row 9-slice Label chrome (the rowFrame); those
// glyph stacks + positions are installed by Shop::init (FUN_1000502d4).
// open's row TextItem positions use rowFrame.getLeftX() / topY, which were
// cached during Shop::init.
void Shop::open() {
    // ---- initial flags ----
    visible          = true;
    closeRequested   = false;
    unlockAnimActive = 0;
    shakeActive      = 0;
    shakeStep        = 0;
    hoveredRow       = -1;

    // ---- free leftover event-preview EventSlot ----
    // binary inlines the per-Quad dtor cascade (~18 FUN_100007df0 calls)
    // before operator_delete. our typed EventSlot has a virtual dtor
    // that handles the same cascade via C++ implicit destruction.
    delete heapPreview;
    heapPreview = nullptr;

    // ---- 3 unlock rows: TextItem setup ----
    //
    // each row gets:
    //   - 3 TextItems (title / description / count) wired to game's
    //     bmfontTable[0] glyph table, scale 0.07 baseline.
    //   - title gets a 0.085 scale override (= "section heading" size).
    //   - title.setString() picks the row-appropriate string.
    //   - description.setString() picks the row's flavor text.
    //   - positions: X = rowFrame.leftX + 0.190625 (offset 0 until chrome
    //     lands); Y is row-stacked via the propagated baseY (= previous
    //     row's description.posY).
    //
    // DAT constants from FUN_100052058's locals:
    //   DAT_10005a630 = 0.190625  title X offset from rowFrame.leftX
    //   DAT_10005a634 = 0.059375  title Y offset from baseY
    //   DAT_10005a638 = 0.053125  description Y offset from title.Y
    //   DAT_10005a63c = 0.100000  count Y offset from title.Y
    constexpr float kTitleXOffset = 0.190625f;
    constexpr float kTitleYOffset = 0.059375f;
    constexpr float kDescYOffset  = 0.053125f;
    constexpr float kCountYOffset = 0.100000f;
    constexpr float kBodyScale    = 0.07f;
    constexpr float kTitleScale   = 0.085f;

    static constexpr const char* kTitles[3] = {
        "Unlock Face",
        "Unlock Snag",
        "Unlock Event",
    };
    static constexpr const char* kDescriptions[3] = {
        "A new face to hide behind",
        "A new challenge to defeat",
        "A new opportunity to try",
    };

    Game* g = getGame();
    const BMFontTable* glyphTable = (g != nullptr) ? g->bmfontTablePtr(0) : nullptr;

    for (int i = 0; i < 3; i++) {
        ShopUnlockRow& row = rows[i];

        // inner loop: 3 TextItems get their glyph-table pointer + base
        // scale set. order matters, title overwrites scale below.
        row.title.glyphTablePtr       = glyphTable;
        row.title.scaleX              = kBodyScale;
        row.title.scaleY              = kBodyScale;
        row.description.glyphTablePtr = glyphTable;
        row.description.scaleX        = kBodyScale;
        row.description.scaleY        = kBodyScale;
        row.unlockedCountText.glyphTablePtr = glyphTable;
        row.unlockedCountText.scaleX        = kBodyScale;
        row.unlockedCountText.scaleY        = kBodyScale;

        // title scale override (= "Unlock X" heading is bigger than body).
        row.title.scaleX = kTitleScale;
        row.title.scaleY = kTitleScale;

        row.title.setString(kTitles[i], -1);
        row.description.setString(kDescriptions[i], -1);

        // FUN_10004c93c is Label::getLeftX, returns paired (s0=leftX,
        // s1=topY) on ARM64. disasm at 0x100052220-0x100052224 consumes
        // both returns: title.pos = (leftX + 0.190625, topY + 0.059375).
        // (Ghidra dropped the s1 source; the ZEXT416(0x3f800000) carry it
        // substituted is noise, not the real Y.)
        const float leftX = row.rowFrame.getLeftX();
        const float topY  = row.rowFrame.topY;

        row.title.posX = leftX + kTitleXOffset;
        row.title.posY = topY  + kTitleYOffset;

        row.description.posX = row.title.posX;
        row.description.posY = row.title.posY + kDescYOffset;

        row.unlockedCountText.posX = row.title.posX;
        row.unlockedCountText.posY = row.title.posY + kCountYOffset;
    }

    // ---- keys snapshot + key-icon list rebuild ----
    keysBackup = keys;
    keyIcons.clear();   // = FUN_100055040 (libc++ list::clear).

    // key icon visual: UV (0.3544922, 0.66015625) -> (0.4170, 0.7383),
    // size 0.1 x 0.125. all icons share the same glyph; only positions
    // differ.
    //
    // position layout (per FUN_100052058 tail):
    //   posX = base - i * spacing
    //   posY = 0.078125
    //
    //   base    = chromeQuad0.posX + 0.0015625 (chrome reference Quad's
    //             posX, set to 0.84375 by Shop::init's tail block, + nudge)
    //
    //   spacing = clamp((keys - 5) / 10.0, 0.0, 1.0) * 0.053125
    //             + (1 - clamp) * 0.09375
    //     keys <=  5 -> spacing = 0.09375  (wider)
    //     keys >= 15 -> spacing = 0.053125 (tighter)
    constexpr float kKeyIconBaseXNudge   = 0.0015625f;  // DAT_10005a644
    constexpr float kKeyIconSpacingMin   = 0.053125f;   // DAT_10005a638
    constexpr float kKeyIconSpacingMax   = 0.09375f;    // DAT_10005a640
    constexpr float kKeyIconPosY         = 0.078125f;

    const float baseX = chromeQuad0.posX + kKeyIconBaseXNudge;
    const float clampT =
        std::clamp((static_cast<float>(keys) - 5.0f) / 10.0f, 0.0f, 1.0f);
    const float spacing = clampT * kKeyIconSpacingMin
                          + (1.0f - clampT) * kKeyIconSpacingMax;

    for (int i = 0; i < keys; i++) {
        ShopKeyIcon icon;   // default-constructed (Quad ctor zeroes verts +
                            //   sets UV/size to unit-quad defaults)
        icon.quad.setTexCoords(0.3544922f, 0.66015625f, 0.4169922f, 0.7382812f);
        icon.quad.setSize(0.1f, 0.125f);
        icon.quad.posX = baseX - static_cast<float>(i) * spacing;
        icon.quad.posY = kKeyIconPosY;
        icon.decayPhase = 1.0f;

        // binary inserts each new node at the tail (sentinel.__prev_
        // splice in the FUN_100052058 decomp). pushed chronologically,
        // this leaves head = i=0 (rightmost screen X) and tail = i=keys-1
        // (leftmost). Shop::update's decay loop pops from the head, so the
        // rightmost icon is consumed first.
        keyIcons.push_back(icon);
    }

    // tail: prime initial visuals + count text + one update tick.
    recomputeRowAvailability();    // FUN_100052478
    formatUnlockedCounts();         // FUN_10005250c
    update(0.0f, 1.0f);             // FUN_1000525f4 (prime tick with dt=0)
}

// FUN_100053a70, Shop::draw.
//
// renders in 4 texture-bind groups:
//   1. tex 8 (sheet1):  key icons + headerLabel + 7 chrome Quads (+ unlock
//                       anim Quads if active)
//   2. tex 9 (ui1):     3 rowFrame Labels + per-row preview-slot Quads
//                       (decorationA / decorationB always; decorationC
//                       gated by `iVar8 < keysBackup`; decorationOrPad
//                       gated by anim-active state)
//   3. tex 12 (icons2): 3 row iconQuads + per-row priceQuad-or-unlockedQuad
//                       + per-row 3 TextItems
//
// the shakeActive / unlockAnimActive branches inside group 1 light up
// during a successful unlock animation. for the basic browse view none of
// those flags are set, so the animated-reveal Quads stay invisible.
void Shop::draw() {
    bindTexture(8);

    // ---- key icons: walk std::list, draw each Quad ----
    //
    // binary walks via sentinel.__prev (reverse order: newest first), but
    // the visual result is identical for non-overlapping icons. forward
    // iteration is cleaner in C++.
    for (ShopKeyIcon& icon : keyIcons) {
        icon.quad.draw();
    }

    // ---- big "Shop" header Label ----
    headerLabel.draw();

    // ---- shakeActive: draw the unlock avatar (or heapPreview EventSlot
    //      with its own scale transform) + nameOverlay text ----
    if (shakeActive != 0) {

        if (heapPreview == nullptr) {
            bindTexture((GLuint)avatarTextureIndex);
            unlockAvatarQuad.draw();
        }
        else {
            // binary glPushMatrix, translate to heapPreview's mainQuad
            // pos, scale by heapPreviewAnimPhase, translate back, draw,
            // pop. effectively scales the EventSlot around its anchor.
            const float ax = heapPreview->mainQuad.quad.posX;
            const float ay = heapPreview->mainQuad.quad.posY;
            const float scale = heapPreviewAnimPhase;
            glPushMatrix();
            glTranslatef(ax, ay, 0.0f);
            glScalef(scale, scale, 1.0f);
            glTranslatef(-ax, -ay, 0.0f);
            heapPreview->draw();
            glPopMatrix();
        }
        unlockNameOverlay.draw();
    }

    bindTexture(8);

    // ---- unlockAnimActive + unlockAnimStep2: 3 mid-reveal anim quads
    //      (4 first, then 2, then 3 per binary order) ----
    if (unlockAnimActive != 0 && unlockAnimStep2 != 0) {
        animQuads[4].draw();
        animQuads[2].draw();
        animQuads[3].draw();
    }

    // ---- 7 chrome Quads (chrome1..6 before chrome0 per binary order) ----
    chromeQuad1.draw();
    chromeQuad2.draw();
    chromeQuad3.draw();
    chromeQuad4.draw();
    chromeQuad5.draw();
    chromeQuad6.draw();
    chromeQuad0.draw();

    // ---- unlockAnimActive: always draw animQuad[0]; if also Step2,
    //      draw animQuad[1] (binary nests the [1] draw inside the same
    //      conditional, separated by a comma operator at line ~46-49). --
    if (unlockAnimActive != 0) {
        animQuads[0].draw();

        if (unlockAnimStep2 != 0) {
            animQuads[1].draw();
        }
    }

    bindTexture(9);

    // ---- 3 rowFrames + per-row preview slots ----
    //
    // each row draws (rowIndex + 1) preview slots' decorationA + decorationB.
    // decorationOrPad: only when anim is active and this row is the
    //   committed row and the slot is within the consumed range.
    // decorationC: drawn for slots where slotIter < keysBackup.
    for (int rowIndex = 0; rowIndex < 3; rowIndex++) {
        ShopUnlockRow& row = rows[rowIndex];
        row.rowFrame.draw();

        const int slotCount = rowIndex + 1;

        for (int slotIter = 0; slotIter < slotCount; slotIter++) {
            ShopPreviewSlot& slot = row.previews[slotIter];
            slot.decorationA.draw();
            slot.decorationB.draw();

            // anim-only: tinted decoration (only the committed row's
            // consumed slots get this).
            if (unlockAnimActive != 0
                && rowIndex == committedRow
                && slotIter <= (committedRow - keysToConsume)) {
                slot.decorationOrPad.draw();
            }

            if (slotIter < keysBackup) {
                slot.decorationC.draw();
            }
        }
    }

    bindTexture(12);

    // ---- 3 row iconQuads (the face/snag/event silhouette) ----
    for (int rowIndex = 0; rowIndex < 3; rowIndex++) {
        rows[rowIndex].iconQuad.draw();
    }

    // ---- per-row priceQuad / unlockedQuad selection ----
    //
    // affordability check: keysBackup >= (rowIndex + 1) and pool is
    // non-empty (binary reads pool.end_node.__left at +0x3968/+0x3980/
    // +0x3998 to test emptiness, equivalent to !pool.empty()).
    {
        const bool faceAvailable  = keysBackup >= 1 && !facePool.empty();
        const bool snagAvailable  = keysBackup >= 2 && !snagPool.empty();
        const bool eventAvailable = keysBackup >= 3 && !eventPool.empty();

        (faceAvailable  ? rows[0].priceQuad : rows[0].unlockedQuad).draw();
        (snagAvailable  ? rows[1].priceQuad : rows[1].unlockedQuad).draw();
        (eventAvailable ? rows[2].priceQuad : rows[2].unlockedQuad).draw();
    }

    // ---- per-row 3 TextItems (title, description, unlockedCountText) ----
    for (int rowIndex = 0; rowIndex < 3; rowIndex++) {
        ShopUnlockRow& row = rows[rowIndex];
        row.title.draw();
        row.description.draw();
        row.unlockedCountText.draw();
    }
}

// FUN_100053f1c, Shop::setRowState.
//
// applies one of 3 tint presets to a single row's chrome elements:
//   state 0: pure white (default)
//   state 1: dark gray + warm-grey unlockedCountText (unaffordable / sold)
//   state 2: light gray hover-highlight
//
// touches: rowFrame Label color, iconQuad/priceQuad/unlockedQuad color,
// 3 TextItem rgba+alpha, 3 preview slot decorationA/B/C colors. uses
// Quad::setColor (vertex-color writes) for the Quads and the row Label's
// setColor (FUN_10004c794).
void Shop::setRowState(int rowIndex, int state) {

    if (rowIndex < 0 || rowIndex >= 3) {
        return;
    }
    ShopUnlockRow& row = rows[rowIndex];

    if (state == 2) {
        // light gray (hover)
        row.rowFrame.setColor(200, 200, 200, 0xFF);
        row.iconQuad.setColor(0xC8, 0xC8, 0xC8, 0xFF);
        row.priceQuad.setColor(0xC8, 0xC8, 0xC8, 0xFF);
        row.title.rgba             = 0xFFC8C8C8u;
        row.title.applyColor();
        row.description.colorR     = 0x96; row.description.colorG = 0x8C;
        row.description.colorB     = 0x96; row.description.alpha  = 0xFF;
        row.description.applyColor();
        row.unlockedCountText.colorR = 0x96; row.unlockedCountText.colorG = 0x8C;
        row.unlockedCountText.colorB = 0x96; row.unlockedCountText.alpha  = 0xFF;
        row.unlockedCountText.applyColor();

        for (int s = 0; s < 3; s++) {
            ShopPreviewSlot& slot = row.previews[s];
            slot.decorationA.setColor(0xC8, 0xC8, 0xC8, 0xFF);
            slot.decorationB.setColor(0xC8, 0xC8, 0xC8, 0xFF);
            slot.decorationC.setColor(0xC8, 0xC8, 0xC8, 0xFF);
        }
    }
    else if (state == 1) {
        // dark gray (unaffordable / sold out)
        row.rowFrame.setColor(100, 100, 100, 0xFF);
        row.iconQuad.setColor(0x64, 0x64, 0x64, 0xFF);
        row.unlockedQuad.setColor(0x96, 0x96, 0x96, 0xFF);
        row.title.colorR = 100; row.title.colorG = 0x5A;
        row.title.colorB = 0x64; row.title.alpha = 0xFF;
        row.title.applyColor();
        row.description.colorR = 100; row.description.colorG = 0x5A;
        row.description.colorB = 0x64; row.description.alpha  = 0xFF;
        row.description.applyColor();
        row.unlockedCountText.colorR = 100; row.unlockedCountText.colorG = 0x5A;
        row.unlockedCountText.colorB = 0x64; row.unlockedCountText.alpha  = 0xFF;
        row.unlockedCountText.applyColor();

        for (int s = 0; s < 3; s++) {
            ShopPreviewSlot& slot = row.previews[s];
            slot.decorationA.setColor(0x64, 0x64, 0x64, 0xFF);
            slot.decorationB.setColor(0x64, 0x64, 0x64, 0xFF);
            slot.decorationC.setColor(0xB4, 0xB4, 0xB4, 0xFF);
        }
    }
    else if (state == 0) {
        // pure white (default / affordable)
        row.rowFrame.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        row.iconQuad.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        row.priceQuad.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        row.title.rgba = 0xFFFFFFFFu;
        row.title.applyColor();
        row.description.colorR = 200; row.description.colorG = 0xBE;
        row.description.colorB = 0xC8; row.description.alpha  = 0xFF;
        row.description.applyColor();
        row.unlockedCountText.colorR = 200; row.unlockedCountText.colorG = 0xBE;
        row.unlockedCountText.colorB = 0xC8; row.unlockedCountText.alpha  = 0xFF;
        row.unlockedCountText.applyColor();

        for (int s = 0; s < 3; s++) {
            ShopPreviewSlot& slot = row.previews[s];
            slot.decorationA.setColor(0xFF, 0xFF, 0xFF, 0xFF);
            slot.decorationB.setColor(0xFF, 0xFF, 0xFF, 0xFF);
            slot.decorationC.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        }
    }
}

// FUN_100052478, Shop::recomputeRowAvailability.
//
// per-row state pick. state 1 = unavailable (pool empty or keys
// insufficient), state 0 = available.
void Shop::recomputeRowAvailability() {

    for (uint32_t row = 0; row < 3; row++) {
        bool unavailable = true;

        if (row == 2 && !eventPool.empty()) {
            unavailable = (keys < 3);
        }
        else if (row == 1 && !snagPool.empty()) {
            unavailable = (keys < 2);
        }
        else if (row == 0 && !facePool.empty()) {
            unavailable = (keys < 1);
        }
        setRowState(static_cast<int>(row), unavailable ? 1 : 0);
    }
}

// FUN_10005250c, Shop::formatUnlockedCounts.
//
// builds "X/Y Unlocked" text for each row's unlockedCountText. the binary
// (recovered from disasm) uses fixed totals with unlocked = total - remaining
// pool: faces 30, snags 20, events 15. faces keep the literal 30 (two face IDs
// are never seeded); snags/events derive the total from pool + unlocks, which is
// invariant under commit (pop from pool, push to unlocks) and adapts if the
// pools grow.
void Shop::formatUnlockedCounts() {
    char buf[64];

    // faces have a fixed total of 30 with two IDs that are never seeded, so the
    // pool starts at 28 and the count is 30 - remaining pool (reads 2/30 at the
    // start). snags and events have no unseeded entries, so pool + unlocks gives
    // their true total and adapts if the pools grow.
    std::snprintf(buf, sizeof(buf), "%d/%d Unlocked",
                  30 - (int)facePool.size(), 30);
    rows[0].unlockedCountText.setString(buf, -1);

    const int snagTotal  = (int)snagPool.size()  + (int)snagUnlocks.size();
    const int eventTotal = (int)eventPool.size() + (int)eventUnlocks.size();

    std::snprintf(buf, sizeof(buf), "%d/%d Unlocked",
                  (int)snagUnlocks.size(), snagTotal);
    rows[1].unlockedCountText.setString(buf, -1);

    std::snprintf(buf, sizeof(buf), "%d/%d Unlocked",
                  (int)eventUnlocks.size(), eventTotal);
    rows[2].unlockedCountText.setString(buf, -1);
}

// FUN_100053d8c, Shop::beginUnlockSequence.
//
// walks the keyIcons list (forward) and seeds per-icon anim state:
//   decayFromXY = current quad pos (anim start)
//   decayPhase  = 0
//   decayDelay  = clamp(iter / 10) * 0.5 (stagger so newer icons start
//                                          their decay first)
//
// for the first icon (iter == -1, the soon-to-be-consumed one):
//   decayToXY.x = rowFrame[committedRow].leftX + width/2 + iconHeight/2
//   decayToXY.y = current Y (unchanged)
//   triggers sound 0x37 (keyDrop).
//
// for subsequent icons (iter >= 0):
//   decayToXY.x = chromeQuad0.posX + 0.0015625 - iter * spacing
//   decayToXY.y = 0.078125  (= bits 0x3DA00000)
//   spacing     = clamp((keys-6)/10) * 0.053125 + (1-clamp) * 0.09375
//                 (same shape as Shop::open's spacing formula but with a
//                  -6 offset that accounts for the icon being consumed).
void Shop::beginUnlockSequence() {

    if (keyIcons.empty()) {
        return;
    }

    int iter = -1;

    for (ShopKeyIcon& icon : keyIcons) {
        const float clampT = std::clamp((float)iter / 10.0f, 0.0f, 1.0f);
        icon.decayDelay    = clampT * 0.5f;
        icon.decayPhase    = 0.0f;

        union FloatPair { uint64_t u; struct { float x, y; }; };
        FloatPair prev;
        prev.x = icon.quad.posX;
        prev.y = icon.quad.posY;
        icon.decayFromXY = prev.u;

        if (iter == -1) {
            // newest icon flies to the current row's price marker. binary
            // (FUN_100053d8c @ 0x100053210..0x100053280) keeps the
            // icon's current posX (uVar10 low 32 bits) and overwrites
            // only posY:
            //   decayToY = icon.quad.height * 0.5
            //              + rowFrame.topY (from getLeftX s1)
            //              + rowFrame.height * 0.5 (from getWidth s1)
            // = icon-height-offset row center Y (the low-32 keep is the
            // unchanged X, not X-axis math).
            Label& rowFrame = rows[committedRow].rowFrame;
            rowFrame.getLeftX();   // refresh leftX/topY cache (s1=topY)
            rowFrame.getWidth();   // refresh width/height (s1=height)
            FloatPair dst;
            dst.u = prev.u;        // start: (curX, curY)
            dst.y = icon.quad.height * 0.5f
                    + rowFrame.topY
                    + rowFrame.cachedSize1 * 0.5f;
            icon.decayToXY = dst.u;

            Game* g = getGame();

            if (g) {
                g->soundQueue.trigger(0x37);   // keyDrop
            }
        }
        else {
            // remaining icons re-space into the chrome row, leaving a
            // gap where the consumed icon was. spacing math mirrors
            // Shop::open with a -6 keys offset.
            const float spacingT = std::clamp(
                ((float)((int)keyIcons.size() - 6)) / 10.0f, 0.0f, 1.0f);
            const float spacing = spacingT * 0.053125f
                                  + (1.0f - spacingT) * 0.09375f;
            FloatPair dst;
            dst.x = chromeQuad0.posX + 0.0015625f
                    - (float)iter * spacing;
            dst.y = 0.078125f;
            icon.decayToXY = dst.u;
        }
        iter++;
    }
}

// FUN_1000525f4, Shop::update.
//
// per-frame tick. one of the longest functions in the binary (~1300
// ARM64 instructions). orchestrates:
//   1. key-icon decay list walk (advance per-icon delay, then phase,
//      lerp position; pop consumed head; trigger next-stage anim when
//      all keysToConsume are spent)
//   2. unlock-reveal multi-stage anim state machine (path follow ->
//      scale grow -> shake -> reset): controlled by animTimers[0/2/4] for
//      stage progress + animTimers[1/3] for between-stage delays
//   3. touch hit-test on rowFrames (press -> setRowState(2),
//      release -> commit or cancel)
//   4. shake interp (after commit, lerp avatar between shakeFromXY and
//      shakeToXY)
//   5. animQuad[0] keyspawn trigger
//   6. AnimationController::update on unlockNameOverlay
void Shop::update(float dt, float /*param2*/) {

    Game* game = getGame();

    union FloatPair { uint64_t u; struct { float x, y; }; };

    // ============================================================
    // 1. KEY ICON DECAY LIST WALK
    // ============================================================
    //
    // walks keyIcons forward. for each icon:
    //   if decayDelay > 0: tick down by dt, skip
    //   else: advance decayPhase by 2*dt (clamped 0..1), lerp pos
    //         between decayFromXY and decayToXY
    //   if decayPhase reached 1.0 and this is the head and unlock-anim
    //     is in the consumption stage (unlockAnimActive but not Step2):
    //     pop the head node, free it, decrement keysToConsume; if
    //     keysToConsume now <= 0, fire the next stage (path build).
    //
    // we walk by iterator instead of the binary's intrusive __prev/__next
    // hops since std::list iterators give us the same forward traversal
    // and erase() handles all the relink + delete logic.
    if (!keyIcons.empty()) {
        auto it = keyIcons.begin();

        while (it != keyIcons.end()) {
            ShopKeyIcon& icon = *it;

            if (icon.decayPhase < 1.0f) {

                if (icon.decayDelay > 0.0f) {
                    icon.decayDelay -= dt;
                    ++it;
                    continue;
                }

                // advance phase by 2 dt, clamp 0..1
                icon.decayPhase = std::clamp(
                    2.0f * dt + icon.decayPhase, 0.0f, 1.0f);

                // lerp pos between decayFromXY and decayToXY
                FloatPair from; from.u = icon.decayFromXY;
                FloatPair to;   to.u   = icon.decayToXY;
                const float t  = icon.decayPhase;
                icon.quad.posX = (1.0f - t) * from.x + t * to.x;
                icon.quad.posY = (1.0f - t) * from.y + t * to.y;

                // head-only: when phase reaches 1.0 and we're in the
                // consumption stage (unlockAnimActive && !unlockAnimStep2),
                // pop this icon from the list, decrement keysToConsume,
                // fire stage-advance when all consumed.
                if (unlockAnimActive != 0
                    && unlockAnimStep2 == 0
                    && &icon == &keyIcons.front()
                    && t >= 1.0f) {

                    it = keyIcons.erase(it);
                    keysToConsume--;

                    if (game) {
                        game->soundQueue.trigger(0x38);   // unlockLight
                    }

                    if (keysToConsume < 1) {
                        // last key consumed; advance to reveal stages:
                        //   animTimers[0] (path follow phase)  = 0.0
                        //   animTimers[1] (path -> scale delay) = 0.5
                        //   animTimers[2] (scale phase)         = 0.0
                        //   animTimers[3] (scale -> shake delay) = 0.5
                        //   animTimers[4] (shake phase)         = 0.0
                        unlockAnimStep2 = 1;
                        animTimers[0]   = 0.0f;
                        animTimers[1]   = 0.5f;
                        animTimers[2]   = 0.0f;
                        animTimers[3]   = 0.5f;
                        animTimers[4]   = 0.0f;

                        // zero rotations on animQuads[2..4] only;
                        // animQuads[1] is left untouched.
                        animQuads[2].rotation = 0.0f;
                        animQuads[3].rotation = 0.0f;
                        animQuads[4].rotation = 0.0f;
                        animQuads[3].setAlpha(0xFF);
                        animQuads[2].setAlpha(0xFF);
                        animQuads[4].setAlpha(0xFF);

                        // start animQuad[1] anim bbox. binary loads:
                        //   animMinX = 0
                        //   animMinY = rowFrame[committedRow].topY +
                        //              rowFrame.height * 0.5
                        //              (= row center Y, via the
                        //              dropped-paired-return of
                        //              getLeftX/getWidth: s1=topY then
                        //              s1=height)
                        //   animMaxX = 1.0
                        //   animMaxY = chromeQuad6.posY +
                        //              chromeQuad6.height * 0.5
                        //              - 0.021875
                        Label& rowFrame =
                            rows[committedRow].rowFrame;
                        rowFrame.getLeftX();   // refresh leftX/topY cache
                        rowFrame.getWidth();   // refresh width/height cache
                        const float rowCenterY = rowFrame.topY
                            + rowFrame.cachedSize1 * 0.5f;
                        const float maxY = chromeQuad6.posY
                            + chromeQuad6.height * 0.5f
                            - 0.021875f;
                        animQuads[1].animating = true;
                        animQuads[1].animMinX  = 0.0f;
                        animQuads[1].animMinY  = rowCenterY;
                        animQuads[1].animMaxX  = 1.0f;
                        animQuads[1].animMaxY  = maxY;

                        // build pathPoints: 6 anchor points pushed in
                        // order from the binary's @ 0x100052794..0x1000529b0.
                        // verified by disassembly. each push is a u64 in
                        // the binary (8-byte vector stride); we store
                        // them flat as float pairs for the same observable
                        // memory layout. paired-return drops on getLeftX
                        // captured via s10 / s1 stack saves.
                        pathPoints.clear();
                        const float committedRowTopY =
                            rows[committedRow].rowFrame.topY;

                        // p0: (chromeQuad0.posX, rowFrame[committedRow].topY)
                        pathPoints.push_back(chromeQuad0.posX);
                        pathPoints.push_back(committedRowTopY);

                        // p1: (chromeQuad0.posX, chromeQuad1.posY + chromeQuad1.height * 0.5)
                        pathPoints.push_back(chromeQuad0.posX);
                        pathPoints.push_back(
                            chromeQuad1.posY + chromeQuad1.height * 0.5f);

                        // p2: (chromeQuad3.posX + width*0.5, chromeQuad3.posY + 0.009375)
                        //     = chromeQuad3 right-edge X, just below its Y center.
                        pathPoints.push_back(
                            chromeQuad3.posX + chromeQuad3.width * 0.5f);
                        pathPoints.push_back(chromeQuad3.posY + 0.009375f);

                        // p3: (chromeQuad3.posX - width*0.5, prev.Y)
                        //     = chromeQuad3 left-edge X, same Y as p2.
                        pathPoints.push_back(
                            chromeQuad3.posX - chromeQuad3.width * 0.5f);
                        pathPoints.push_back(chromeQuad3.posY + 0.009375f);

                        // p4: (chromeQuad6.posX, chromeQuad5.posY - chromeQuad5.height*0.5)
                        //     = chromeQuad6's X, chromeQuad5 top-edge Y.
                        pathPoints.push_back(chromeQuad6.posX);
                        pathPoints.push_back(
                            chromeQuad5.posY - chromeQuad5.height * 0.5f);

                        // p5: (pixelSnap(chromeQuad6.posX),
                        //      pixelSnap((virtualHeight + chromeQuad6.posY) * 0.5))
                        const float vh = Renderer::getVirtualHeight();
                        const float snapX = std::floor(
                            chromeQuad6.posX * 640.0f + 0.5f) / 640.0f;
                        const float snapY = std::floor(
                            (vh + chromeQuad6.posY) * 0.5f * 640.0f + 0.5f)
                            / 640.0f;
                        pathPoints.push_back(snapX);
                        pathPoints.push_back(snapY);

                        // compute pathLengths: cumulative arc length
                        // along the path. binary uses (per
                        // disassembly @ 0x100052a4c / 0x100052afc):
                        //   segments 2 + 4: segLen = |dx| * PI * 0.5
                        //                            (quarter-arc along X)
                        //   other segments: segLen = sqrt(dx*dx + dy*dy)
                        //                            (euclidean)
                        // first pass = total. second pass = cumulative
                        // fraction (cum / total) pushed per segment.
                        pathLengths.clear();
                        float totalLen = 0.0f;

                        for (size_t i = 1; 2 * i < pathPoints.size(); i++) {
                            float dx = pathPoints[2 * i]
                                     - pathPoints[2 * i - 2];
                            float dy = pathPoints[2 * i + 1]
                                     - pathPoints[2 * i - 1];
                            float segLen;

                            if (i == 2 || i == 4) {
                                segLen = std::abs(dx) * 3.1415927f * 0.5f;
                            }
                            else {
                                segLen = std::sqrt(dx * dx + dy * dy);
                            }
                            totalLen += segLen;
                        }

                        // 2nd pass: build cumulative-fraction table.
                        float cum = 0.0f;
                        pathLengths.push_back(0.0f);

                        for (size_t i = 1; 2 * i < pathPoints.size(); i++) {
                            float dx = pathPoints[2 * i]
                                     - pathPoints[2 * i - 2];
                            float dy = pathPoints[2 * i + 1]
                                     - pathPoints[2 * i - 1];
                            float segLen;

                            if (i == 2 || i == 4) {
                                segLen = std::abs(dx) * 3.1415927f * 0.5f;
                            }
                            else {
                                segLen = std::sqrt(dx * dx + dy * dy);
                            }
                            cum += segLen;
                            pathLengths.push_back(totalLen > 0.0f
                                ? cum / totalLen
                                : 0.0f);
                        }
                    }
                    else {
                        // binary @ 0x100052c9c-0x100052ca0: still more keys
                        // to consume, re-run beginUnlockSequence so the
                        // new head icon gets its decayToXY retargeted to
                        // the row center (and the trailing icons re-stagger
                        // their delays). without this, the next icon keeps
                        // its pre-pop chrome-pipe destination, snaps there
                        // instantly, and the consumption animation only
                        // fires once regardless of keysToConsume.
                        beginUnlockSequence();
                    }
                    continue;
                }
            }
            ++it;
        }
    }

    // ============================================================
    // 2. UNLOCK ANIM STATE MACHINE
    // ============================================================
    //
    // active only when both unlockAnimActive and unlockAnimStep2 are set
    // (= consumption phase finished, now play the reveal). drives 3
    // sequential timers separated by 2 delays.
    if (unlockAnimActive != 0 && unlockAnimStep2 != 0) {

        if (animTimers[0] < 1.0f) {
            // STAGE 1: path-follow. timer 0 grows from 0.0..1.0 at
            // dt / 3.0 per frame, then smoothstep clamp 0..1.
            //
            // the old value is captured before the smoothstep: find-segment
            // uses the new phase, the sound trigger uses the old, so
            // segment-boundary detection works.
            const float oldPhase = animTimers[0];
            animTimers[0] = std::clamp(
                animTimers[0] + dt / 3.0f, 0.0f, 1.0f);

            // walk pathLengths to find which segment phase falls inside.
            // binary: find smallest seg (1..N-1) with phase <= pathLengths[seg].
            const float phase = animTimers[0];
            const size_t segCount = pathLengths.size();

            for (size_t seg = 1; seg < segCount; seg++) {

                if (phase <= pathLengths[seg]) {
                    const float seg0  = pathLengths[seg - 1];
                    const float seg1  = pathLengths[seg];
                    const float local = (phase - seg0) / (seg1 - seg0);

                    // path indices: prev = seg-1, curr = seg. each
                    // point is 2 floats in pathPoints flat layout.
                    const float prevX = pathPoints[2 * (seg - 1)];
                    const float prevY = pathPoints[2 * (seg - 1) + 1];
                    const float currX = pathPoints[2 * seg];
                    const float currY = pathPoints[2 * seg + 1];

                    float px, py;

                    if (seg == 2) {
                        // arc p1 -> p2 (rounded corner). binary @
                        // 0x100053198: polar2rect(p1.X - p2.X, local*PI/2)
                        //   point.X = p2.X + dx*cos(local*PI/2)
                        //   point.Y = p1.Y + dx*sin(local*PI/2)
                        // where dx = p1.X - p2.X (= prevX - currX).
                        const float dx    = prevX - currX;
                        const float angle = local * 1.5707964f;
                        px = currX + dx * std::cos(angle);
                        py = prevY + dx * std::sin(angle);
                    }
                    else if (seg == 4) {
                        // arc p3 -> p4 (rounded corner). binary @
                        // 0x10005320c: polar2rect(p4.X - p3.X, (1-local)*PI/2)
                        //   point.X = p3.X + dx*cos((1-local)*PI/2)
                        //   point.Y = p4.Y + dx*sin((1-local)*PI/2)
                        // where dx = p4.X - p3.X (= currX - prevX).
                        const float dx    = currX - prevX;
                        const float angle = (1.0f - local) * 1.5707964f;
                        px = prevX + dx * std::cos(angle);
                        py = currY + dx * std::sin(angle);
                    }
                    else {
                        // segments 1, 3, 5 (and 0 which never fires
                        // since the loop starts at seg=1): general
                        // linear interpolation. binary @ 0x1000532bc.
                        px = (1.0f - local) * prevX + local * currX;
                        py = (1.0f - local) * prevY + local * currY;
                    }

                    animQuads[1].posX = px;
                    animQuads[1].posY = py;
                    // small chrome quads anchor off animQuads[1] with
                    // fixed offsets (binary @ 0x100053318..0x100053358).
                    animQuads[2].posX = px - 0.0265625f;
                    animQuads[2].posY = py + 0.0203125f;
                    animQuads[3].posX = px + 0.015625f;
                    animQuads[3].posY = py + 0.0203125f;
                    animQuads[4].posX = px + 0.0015625f;
                    animQuads[4].posY = py - 0.0328125f;

                    // per-segment "enter" sound. binary @ 0x10005335c:
                    // trigger 0x39 (unlockAppear) when the old timer
                    // value <= pathLengths[seg-1] (= the segment start
                    // fraction). old-vs-new phase means this fires once
                    // per segment boundary crossing, on the frame the
                    // timer first passes the boundary.
                    if (game && oldPhase <= pathLengths[seg - 1]) {
                        game->soundQueue.trigger(0x39);
                    }
                    break;
                }
            }
        }
        else if (animTimers[2] < 1.0f) {
            // STAGE 2: animQuad[4] vertical drift. binary @
            // 0x100052d4c..0x100052e27. animTimers[1] is the delay
            // before stage 2 starts; once expired, animTimers[2]
            // smoothsteps 0..1 at dt/0.1 per frame. the only thing animated
            // is animQuads[4].posY (small upward drift via sigmoid).
            // alpha + scale + the other anim quads do not change here;
            // those belong to stage 3 (shake).
            const float oldTimer2 = animTimers[2];
            animTimers[1] -= dt;

            if (animTimers[1] < 0.0f) {
                // sound 0x3A (unlockLight) on stage 2 entry, when
                // old animTimers[2] == 0 and dt > 0.
                if (oldTimer2 == 0.0f && dt > 0.0f && game) {
                    game->soundQueue.trigger(0x3A);
                }
                animTimers[2] = std::clamp(
                    animTimers[2] + dt / 0.1f, 0.0f, 1.0f);
                const float t = animTimers[2];

                // sigmoid = (1 - cos(t * PI)) / 2 = 0..1 monotonic.
                const float sigmoid =
                    0.5f - std::cos(t * 3.1415927f) * 0.5f;

                // animQuads[4] drifts upward as sigmoid grows.
                animQuads[4].posX = animQuads[1].posX + 0.0015625f;
                animQuads[4].posY = animQuads[1].posY + (-0.0328125f)
                                    + sigmoid * (-0.03125f);
            }

            if (animTimers[2] >= 1.0f) {
                // stage 2 done: commit unlock (pop ID from pool, push
                // to unlocks vector, install avatar + nameOverlay text)
                // and kick off the shake stage.
                shakeActive    = 1;
                shakeStep      = 0;
                keys           = (int)keyIcons.size();
                dirty          = true;

                // free any pre-existing heapPreview EventSlot.
                if (heapPreview) {
                    delete heapPreview;
                    heapPreview = nullptr;
                }

                if (committedRow == 2) {
                    // event unlock. binary @ 0x100053458..0x100053454.
                    int evId = rngPopSet(eventPool, 0);
                    eventUnlocks.push_back(evId);
                    heapPreview = new EventSlot();
                    heapPreview->init(evId, 0);
                    // anchor the EventSlot at animQuads[1]'s position.
                    float anchorXY[2] = {
                        animQuads[1].posX, animQuads[1].posY };
                    heapPreview->setPosition(anchorXY);
                    heapPreviewAnimPhase = 0.0f;

                    // name overlay: 5-case color jump table indexed by
                    // EventKey (= EVENT_TABLE[eventType].key). binary
                    // jump table at 0x100053a5c, color blocks at
                    // 0x1000534c8/48/60/74/8c. anchor = (animQuads[1].pos
                    // + (0, DAT_10005a694=0.1015625)), fontScale =
                    // DAT_10005a698 = 0.05.
                    static constexpr uint32_t kEventColors[5][2] = {
                        // {primary, outline} per EventKey
                        { 0xff00b4fau, 0xff32f0ffu },  // 0 Experience
                        { 0xffaa64aau, 0xffe6aaebu },  // 1 Health
                        { 0xff00be50u, 0xff14f064u },  // 2 Attack
                        { 0xffa06e00u, 0xffdca028u },  // 3 Defence
                        { 0xff003ce6u, 0xff008cfau },  // 4 Control
                    };
                    const uint32_t keyIdx = heapPreview->getEventTypeKey();
                    const uint32_t primary = (keyIdx < 5)
                        ? kEventColors[keyIdx][0] : 0xffffffffu;
                    const uint32_t outline = (keyIdx < 5)
                        ? kEventColors[keyIdx][1] : 0xffffffffu;
                    unlockNameOverlay.startText(
                        0.05f,
                        heapPreview->getName(),
                        animQuads[1].posX,
                        animQuads[1].posY + 0.1015625f,
                        primary, outline);

                    // events anchor the avatar on heapPreview, not
                    // unlockAvatarQuad. binary @ 0x1000535f4..610.
                    overlayOffsetX = unlockNameOverlay.posX
                                     - heapPreview->mainQuad.quad.posX;
                    overlayOffsetY = unlockNameOverlay.posY
                                     - heapPreview->mainQuad.quad.posY;
                }
                else if (committedRow == 1) {
                    // snag unlock. binary @ 0x100053384..0x100053454.
                    int snagId = rngPopSet(snagPool, 0);
                    snagUnlocks.push_back(snagId);
                    avatarTextureIndex = 10;
                    // FUN_10003e0a8, snag portrait UV setter. binary
                    // reads SnagInfo[snagId].sprite{U,V,W,H} and calls
                    // FUN_100014d84 (= applySpriteUV) to install them.
                    const SnagInfo& info = snagInfo((uint32_t)snagId);
                    applySpriteUV(unlockAvatarQuad,
                                  info.spriteU, info.spriteV,
                                  info.spriteW, info.spriteH);
                    unlockAvatarQuad.posX = animQuads[1].posX;
                    unlockAvatarQuad.posY = animQuads[1].posY;
                    unlockAvatarQuad.snapToPixelGrid();
                    unlockAvatarQuad.scaleX = 0.0f;
                    unlockAvatarQuad.scaleY = 0.0f;

                    // name overlay: snag-themed reddish palette. anchor
                    // = (animQuads[1].pos + (0, DAT_10005a660=0.09375)),
                    // fontScale = DAT_10005a698 = 0.05. binary builds
                    // primary byte-by-byte at sp+0x98 (= 0xff0014b4);
                    // outline at sp+0x90 (= 0xff003ce6).
                    unlockNameOverlay.startText(
                        0.05f,
                        info.name,
                        animQuads[1].posX,
                        animQuads[1].posY + 0.09375f,
                        0xff0014b4u, 0xff003ce6u);

                    // snag avatar is unlockAvatarQuad (just positioned
                    // + snapped above). binary @ 0x10005343c..454.
                    overlayOffsetX = unlockNameOverlay.posX
                                     - unlockAvatarQuad.posX;
                    overlayOffsetY = unlockNameOverlay.posY
                                     - unlockAvatarQuad.posY;
                }
                else if (committedRow == 0) {
                    // face unlock. binary @ 0x1000534d8..0x100053544.
                    int faceId = rngPopSet(facePool, 0);
                    faceUnlocks.push_back(faceId);
                    avatarTextureIndex = 8;
                    // FUN_100056478 = setPortraitVisual on a Quad.
                    setPortraitVisual(faceId, unlockAvatarQuad);
                    unlockAvatarQuad.posX = animQuads[1].posX;
                    unlockAvatarQuad.posY = animQuads[1].posY;
                    unlockAvatarQuad.snapToPixelGrid();
                    unlockAvatarQuad.scaleX = 0.0f;
                    unlockAvatarQuad.scaleY = 0.0f;

                    // name overlay: empty string + scale 1.0 + anchor
                    // (0, 0). binary @ 0x100053524..0x100053540 leaves the
                    // stack colors uninitialized, but initFromLines
                    // early-exits before reading them (nonSpaceLineCount
                    // == 0 for empty input). binary also skips the
                    // overlayOffset compute for face (branch jumps past
                    // 10005360c), so the field keeps its prior value; the
                    // empty overlay never renders.
                    unlockNameOverlay.startText(
                        1.0f, "", 0.0f, 0.0f, 0u, 0u);
                }
            }
        }
        else if (animTimers[4] < 1.0f) {
            // STAGE 3: shake (avatar shimmer). timer 4 grows 0..1 once
            // the inter-stage delay timer 3 has counted down.
            animTimers[3] -= dt;

            if (animTimers[3] < 0.0f) {

                if (animTimers[4] == 0.0f && dt > 0.0f && game) {
                    game->soundQueue.trigger(0x3B);   // shimmer
                }
                animTimers[4] = std::clamp(
                    animTimers[4] + 2.0f * dt, 0.0f, 1.0f);
                const float t = animTimers[4];
                const float easeIn = std::clamp(
                    t - 0.5f, 0.0f, 0.5f) * 2.0f;
                const float cosTerm = std::cos(easeIn * 3.1415927f) * 0.5f;
                const float blend = 0.5f - cosTerm;
                const uint8_t alpha = (uint8_t)(int)(
                    blend * 0.0f + (1.0f - blend) * 255.0f);

                // animQuad[3] / [2] / [4] motion + rotation, alpha fade.
                animQuads[3].posX = animQuads[1].posX + 0.015625f
                                    + t * 0.15625f;
                animQuads[3].posY = animQuads[1].posY + 0.0203125f
                                    + t * t * 0.09375f;
                animQuads[3].rotation = t * 45.0f;
                animQuads[3].setAlpha(alpha);

                animQuads[2].posX = animQuads[1].posX - 0.0265625f
                                    + t * -0.15625f;
                animQuads[2].posY = animQuads[1].posY + 0.0203125f
                                    + t * t * 0.09375f;
                animQuads[2].rotation = t * -45.0f;
                animQuads[2].setAlpha(alpha);

                animQuads[4].posX = animQuads[1].posX + 0.0015625f
                                    + t * -0.2234375f;
                animQuads[4].posY = animQuads[1].posY + -0.0640625f
                                    + t * t * 0.0890625f;
                animQuads[4].rotation = t * -45.0f;
                animQuads[4].setAlpha(alpha);

                // avatar scale: sin(t * 120deg) / sin(60deg) ramp again.
                const float scale =
                    std::sin(t * 2.0943952f) / 0.8660254f;
                heapPreviewAnimPhase = scale;
                unlockAvatarQuad.scaleX = scale;
                unlockAvatarQuad.scaleY = scale;
            }
        }
        else {
            // STAGE 4: all stages complete. reset overlay + clear
            // unlockAnimActive; refresh row tints + count text.
            unlockNameOverlay.reset();
            unlockAnimActive = 0;
            keys             = (int)keyIcons.size();
            keysBackup       = (int)keyIcons.size();
            recomputeRowAvailability();
            formatUnlockedCounts();
        }
    }

    // ============================================================
    // 3. TOUCH INPUT HIT-TEST
    // ============================================================
    if (game) {
        const int   state  = game->inputState();
        const float tx     = game->touchX();
        const float ty     = game->touchY();

        // binary's chrome bounds (lines 706-755 of FUN_1000525f4 manual
        // decomp + disassembly @ 0x100053634..0x100053668):
        //   lower bound: touchY >= chromeQuad0.posY (chromeQuad0 center)
        //   upper bound: touchY <= chromeQuad6.posY - chromeQuad6.height * 0.5f
        //                (= chromeQuad6 top edge)
        // taps outside either bound trip closeRequested.
        const float chromeLowerY = chromeQuad0.posY;
        const float chromeUpperY = chromeQuad6.posY
                                   - chromeQuad6.height * 0.5f;

        if (state == 1) {
            // pressed
            if (unlockAnimActive == 0
                && ty >= chromeLowerY
                && ty <= chromeUpperY) {

                for (uint32_t row = 0; row < 3; row++) {

                    if (rows[row].rowFrame.contains(tx, ty)) {
                        bool affordable = false;

                        if (row == 2 && !eventPool.empty() && keys > 2) {
                            affordable = true;
                        }
                        else if (row == 1 && !snagPool.empty() && keys > 1) {
                            affordable = true;
                        }
                        else if (row == 0 && !facePool.empty() && keys > 0) {
                            affordable = true;
                        }

                        if (affordable) {
                            setRowState((int)row, 2);
                            hoveredRow = (int)row;
                            game->soundQueue.trigger(5);
                        }
                        else {
                            game->soundQueue.trigger(0x12);  // disabled
                        }
                        break;
                    }
                }
            }
            else if (unlockAnimActive == 0) {
                // tap outside chrome bounds -> close request.
                closeRequested = true;
            }
        }
        else if (state == 0 && hoveredRow != -1) {
            // released; re-check the hover row.
            const int row = hoveredRow;

            if (rows[row].rowFrame.contains(tx, ty)) {
                // commit
                unlockAnimActive = 1;
                unlockAnimStep2  = 0;
                committedRow     = row;
                keysToConsume    = row + 1;

                if (shakeActive != 0) {
                    shakeStep     = 1;
                    shakeProgress = 0.0f;
                    // capture current avatar position. binary reads from
                    // heapPreview->mainQuad.quad.posX/Y when an event
                    // unlock has spawned one, else from unlockAvatarQuad.
                    // EventSlot+0xB8 = mainQuad.quad start; the binary
                    // dereferences as a packed (posX, posY) u64.
                    float sx, sy;

                    if (heapPreview) {
                        sx = heapPreview->mainQuad.quad.posX;
                        sy = heapPreview->mainQuad.quad.posY;
                    }
                    else {
                        sx = unlockAvatarQuad.posX;
                        sy = unlockAvatarQuad.posY;
                    }
                    shakeFromX = sx;
                    shakeFromY = sy;
                    shakeToX   = sx;
                    shakeToY   = Renderer::getVirtualHeight() + 0.15625f;
                }
                beginUnlockSequence();
                game->soundQueue.trigger(6);
            }
            else {
                game->soundQueue.trigger(7);
            }
            setRowState(row, 0);
            hoveredRow = -1;
        }
    }

    // ============================================================
    // 4. SHAKE INTERP
    // ============================================================
    if (shakeStep != 0) {
        shakeProgress = std::clamp(
            shakeProgress + dt / 0.3f, 0.0f, 1.0f);
        // sigmoid blend = 0.5 - cos(progress * PI) * 0.5  (DAT_10005a650
        // = PI, disasm @ 0x1000537cc-1000537e0).
        const float cosT = std::cos(shakeProgress * 3.1415927f) * 0.5f;
        const float t    = 0.5f - cosT;
        const float lx   = shakeToX * t + shakeFromX * (1.0f - t);
        const float ly   = shakeToY * t + shakeFromY * (1.0f - t);

        if (heapPreview) {
            // EventSlot::setPosition (FUN_100029f20) snaps the mainQuad.
            // binary then reads the snapped heapPreview->mainQuad pos
            // back for the overlay anchor, not the raw lerp values.
            float pos[2] = { lx, ly };
            heapPreview->setPosition(pos);
            unlockNameOverlay.posX = heapPreview->mainQuad.quad.posX
                                     + overlayOffsetX;
            unlockNameOverlay.posY = heapPreview->mainQuad.quad.posY
                                     + overlayOffsetY;
        }
        else {
            unlockAvatarQuad.posX = lx;
            unlockAvatarQuad.posY = ly;
            unlockNameOverlay.posX = lx + overlayOffsetX;
            unlockNameOverlay.posY = ly + overlayOffsetY;
        }

        if (shakeProgress >= 1.0f) {
            shakeActive = 0;
            shakeStep   = 0;
        }
    }

    // ============================================================
    // 5. animQuad[0] keyspawn trigger (per-frame while consuming keys)
    // ============================================================
    //
    // binary @ 0x10005388c..0x100053938. each frame the unlock anim is
    // running with keys still left, animQuads[0] is dragged to the head
    // key icon's pos (minus a 1.5625e-3 nudge in both axes) and its
    // startAnimation bbox is rebuilt to span from chromeQuad0's inner top
    // edge (= top + 0.021875) down to the row's center Y.
    if (unlockAnimActive != 0 && !keyIcons.empty()) {
        const ShopKeyIcon& head = keyIcons.front();
        animQuads[0].posX = head.quad.posX + (-0.0015625f);
        animQuads[0].posY = head.quad.posY + (-0.0015625f);

        // row center Y from rowFrame.getLeftX + getWidth paired returns.
        Label& rowFrame = rows[committedRow].rowFrame;
        rowFrame.getLeftX();      // refresh leftX / topY cache (s1=topY)
        rowFrame.getWidth();      // refresh width / height cache (s1=height)
        const float rowCenterY = rowFrame.topY
                                 + rowFrame.cachedSize1 * 0.5f;
        const float chromeTopInner = chromeQuad0.posY
                                     - chromeQuad0.height * 0.5f
                                     + 0.021875f;

        // animMin = (0, chromeTopInner). animMax = (1.0, rowCenterY).
        animQuads[0].animMinX  = 0.0f;
        animQuads[0].animMinY  = chromeTopInner;
        animQuads[0].animMaxX  = 1.0f;
        animQuads[0].animMaxY  = rowCenterY;
        animQuads[0].animating = true;
    }

    // ============================================================
    // 6. AnimationController::update on unlockNameOverlay
    // ============================================================
    unlockNameOverlay.update(dt);
}

// FUN_100054298, Shop::dirtyXfer.
//
// pushes live shop state into the persistent save buffer:
//   snap.keys         = clamp(shop.keys, 0, 20)
//   snap.faceUnlocks  = shop.faceUnlocks
//   snap.snagUnlocks  = shop.snagUnlocks
//   snap.eventUnlocks = shop.eventUnlocks
// then clears shop.dirty so the next frame doesn't re-fire. binary uses
// 3 distinct vector::assign helpers (one per template instance); libc++
// vector::operator= handles all three.
void Shop::dirtyXfer(PersistentUnlocks& snap) {
    dirty = false;
    snap.keys         = std::clamp(keys, 0, 20);
    snap.faceUnlocks  = faceUnlocks;
    snap.snagUnlocks  = snagUnlocks;
    snap.eventUnlocks = eventUnlocks;
}

// FUN_100054338, Shop::restoreFromSave.
//
// inverse of dirtyXfer. runs once at Game::init after SaveSystem::load
// has populated the save buffer from disk:
//   1. re-seed the 3 pool sets with the canonical defaults (FUN_100051a14)
//   2. clamp snap.keys to [0, 20] and store in shop.keys (FUN_10005722c)
//   3. copy snap.{face,snag,event}Unlocks -> shop.{...}Unlocks
//      (binary uses 3 distinct vector::assign helpers; libc++ operator=
//      handles all three the same way, including the self-assign guard
//      the binary checks before each call)
//   4. erase every already-unlocked ID from the matching pool set so
//      future random draws can't re-offer them
void Shop::restoreFromSave(const PersistentUnlocks& snap) {
    seedPools();

    keys = std::clamp(snap.keys, 0, 20);

    faceUnlocks  = snap.faceUnlocks;
    snagUnlocks  = snap.snagUnlocks;
    eventUnlocks = snap.eventUnlocks;

    for (int id : faceUnlocks) {
        facePool.erase(id);
    }

    for (int id : snagUnlocks) {
        snagPool.erase(id);
    }

    for (int id : eventUnlocks) {
        eventPool.erase(id);
    }
}
