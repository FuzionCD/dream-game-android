#include "achievements_menu.h"

#include "achievement_table.h"
#include "game.h"
#include "renderer.h"

#include <GLES/gl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// ----------------------------------------------------------------------
// AchievementsMenu::init, one-time chrome + per-tile static fill.
// ----------------------------------------------------------------------
//
// the binary splits the one-time setup across two functions:
//   FUN_100057d14 (called from Game::create), header chrome + decoration
//                   Quads + tileBgLabel 3-glyph 9-slice + per-tile icon UVs
//   FUN_100058410 first half (lazy on first open), per-tile TextItem fill
//                   (title / description / progressText strings, scales,
//                    colors)
//
// our port collapses both into init(), called once from Game::create after
// placement-new restores RAII state. the coldStartFlag at +0x0 stays
// vestigial; we run init unconditionally instead of guarding on it.
void AchievementsMenu::init() {
    Game* game = getGame();

    // FUN_100057d14 zeroes coldStartFlag as its first write, so a second
    // init() would re-trigger the first-open branch. C++ value-init covers
    // the empty case; this just matches the binary for any re-init.
    coldStartFlag = 0;

    // =================================================================
    // PART 1, FUN_100057d14 (chrome + decoration + per-tile icon UVs)
    // =================================================================

    // ---- header chrome Quads ----
    //
    // 4 fixed-position decorative quads drawn last (tex 9) every frame.
    // chromeA + chromeB are static decoration; backButton + backButtonOverlay
    // are tinted together as the tap-to-close target.
    //
    // UV coords are pixel positions on a 1024-px texture. display sizes
    // and positions are 640-px reference (Renderer's virtual-width unit).

    chromeA.setTexCoords(1005.0f / 1024.0f, 747.0f / 1024.0f,
                         1023.0f / 1024.0f, 837.0f / 1024.0f);
    chromeA.setSize(1.0f, 90.0f / 640.0f);
    chromeA.posX = 0.5f;
    chromeA.posY = 45.0f / 640.0f;

    chromeB.setTexCoords(426.0f / 1024.0f, 357.0f / 1024.0f,
                         688.0f / 1024.0f, 393.0f / 1024.0f);
    chromeB.setSize(262.0f / 640.0f, 36.0f / 640.0f);
    chromeB.posX = 0.5f;
    chromeB.posY = 35.0f / 640.0f;

    backButton.setTexCoords(  0.0f / 1024.0f, 199.0f / 1024.0f,
                             75.0f / 1024.0f, 266.0f / 1024.0f);
    backButton.setSize(75.0f / 640.0f, 67.0f / 640.0f);
    backButton.posX = 603.0f / 640.0f;
    backButton.posY = 33.0f / 640.0f;
    backButton.snapToPixelGrid();

    backButtonOverlay.setTexCoords(628.0f / 1024.0f, 487.0f / 1024.0f,
                                   671.0f / 1024.0f, 530.0f / 1024.0f);
    backButtonOverlay.setSize(43.0f / 640.0f, 43.0f / 640.0f);
    // posX/posY copied from backButton (overlay sits on top, same anchor)
    backButtonOverlay.posX = backButton.posX;
    backButtonOverlay.posY = backButton.posY;
    backButtonOverlay.snapToPixelGrid();

    // ---- tileBgLabel: 3-glyph 9-slice for per-row backgrounds ----
    //
    // initialized via the standard Label setup sequence
    //   init() -> 3x addGlyph() -> measureGlyphRun(-1,-1) ->
    //   setSize(width, paired_return_heightTotal) -> setPosition -> setColor
    // matching the LeaderboardMenu headerLabel pattern exactly.
    //
    // the 3 glyphs (left cap / 1-px middle stretch / right cap) form a
    // horizontal-only banner. mode 2 / 1 / 2 means corners use natural
    // size, middle stretches horizontally to fill (cachedSize0 - measured
    // cap widths).
    tileBgLabel.init();

    constexpr GlyphOffset kNoOffset{0.0f, 0.0f};

    {
        const float uv0[2]   = {824.0f, 540.0f};
        const float size0[2] = { 70.0f,  70.0f};
        tileBgLabel.addGlyph(-1.0f, uv0, size0, 2, kNoOffset);

        const float uv1[2]   = {894.0f, 540.0f};
        const float size1[2] = {  1.0f,  70.0f};
        tileBgLabel.addGlyph(-1.0f, uv1, size1, 1, kNoOffset);

        const float uv2[2]   = {895.0f, 540.0f};
        const float size2[2] = { 26.0f,  70.0f};
        tileBgLabel.addGlyph(-1.0f, uv2, size2, 2, kNoOffset);
    }

    // measureGlyphRun returns paired (advWidth, heightTotal). setSize takes
    // width 632/640 and heightTotal as height; Ghidra drops the paired s1
    // in the decomp, so we capture it via measureGlyphRun.heightTotal.
    const GlyphRunMetrics m = tileBgLabel.measureGlyphRun(-1, -1);
    tileBgLabel.setSize(632.0f / 640.0f, m.heightTotal);
    tileBgLabel.setPosition(4.0f / 640.0f, 0.0f);
    tileBgLabel.setColor(0x64, 0x50, 0x6E, 0xFF);   // dark grayish-purple

    // ---- shared per-tile decoration Quads ----
    //
    // each is recolored + repositioned per row inside draw(); the setup
    // here just pins their UV + size + base position.
    //
    // lockedDeco and unlockedHighlight share the same base position; only
    // one is drawn per row (depending on tile.locked). unlockedDeco offsets
    // by +1/640 in both axes to slightly nest inside the highlight.

    lockedDeco.setTexCoords(433.0f / 1024.0f, 394.0f / 1024.0f,
                            480.0f / 1024.0f, 453.0f / 1024.0f);
    lockedDeco.setSize(47.0f / 640.0f, 59.0f / 640.0f);
    lockedDeco.posX = 606.0f / 640.0f;
    lockedDeco.posY =  36.0f / 640.0f;
    lockedDeco.snapToPixelGrid();

    unlockedHighlight.setTexCoords(481.0f / 1024.0f, 394.0f / 1024.0f,
                                   528.0f / 1024.0f, 453.0f / 1024.0f);
    unlockedHighlight.setSize(47.0f / 640.0f, 59.0f / 640.0f);
    // shared position with lockedDeco
    unlockedHighlight.posX = lockedDeco.posX;
    unlockedHighlight.posY = lockedDeco.posY;

    unlockedDeco.setTexCoords(363.0f / 1024.0f, 676.0f / 1024.0f,
                              427.0f / 1024.0f, 757.0f / 1024.0f);
    // 0.07 = 44.8/640 and 0.08859375 = 56.7/640, non-integer artist sizes;
    // preserve the binary's float literals.
    unlockedDeco.setSize(0.07f, 0.08859375f);
    // offset by +1/640 from unlockedHighlight (DAT_10005a75c = 1/640)
    unlockedDeco.posX = unlockedHighlight.posX + 1.0f / 640.0f;
    unlockedDeco.posY = unlockedHighlight.posY + 1.0f / 640.0f;

    // ---- per-tile TextItem POD-field init + icon UV / size / position ----
    //
    // matches the binary's per-tile loop in FUN_100057d14: TextItem::init
    // on title / description / progressText, Quad ctors on icon /
    // progressBar (already covered by C++ placement-new), then per-tile
    // icon UV/size/position via FUN_10004d5c4 + FUN_100014d84.
    //
    // TextItem::init sets the POD fields the C++ default ctor leaves at zero,
    // most importantly spaceMultiplier=2.0; without it every space collapses
    // to zero advance and the strings render as one mashed-together word.
    for (int idx = 0; idx < 50; idx++) {
        Tile& tile = tiles[idx];
        const AchievementInfo& info = ACHIEVEMENT_TABLE[idx];

        tile.title.init();
        tile.description.init();
        tile.progressText.init();

        const float ix = static_cast<float>(info.iconX);
        const float iy = static_cast<float>(info.iconY);
        tile.icon.setTexCoords( ix          / 1024.0f,  iy          / 1024.0f,
                               (ix + 84.0f) / 1024.0f, (iy + 84.0f) / 1024.0f);
        tile.icon.setSize(ACHIEVEMENT_ICON_SIZE_PX / 640.0f,
                          ACHIEVEMENT_ICON_SIZE_PX / 640.0f);
        tile.icon.posX = 40.0f / 640.0f;
        tile.icon.posY = 35.0f / 640.0f;
    }

    // chrome-only. per-tile TextItem fill (title / description /
    // progressText) lives in open() under the coldStartFlag gate, matching
    // the binary's split (FUN_100057d14 vs FUN_100058410's first half). it
    // also avoids reading an empty BMFont table here, since Game::create
    // runs init before loadFonts.
}

// ----------------------------------------------------------------------
// FUN_100058410, per-show open.
// ----------------------------------------------------------------------
//
// runs on every entry to the achievements page. two phases:
//   - first-open gate (guarded by *param_1 == 0): per-tile TextItem fill
//     (title / description / progressText font + color + scale + string)
//   - every-show: reset transient flags, rebuild sortedDisplay, prime
//     per-tile progress text + bar, then call update(0) as the prime tick.
//
// the first-open TextItem work can't move into init() (FUN_100057d14)
// because the BMFont tables aren't populated at game-create; setString
// would emit zero glyphs.
void AchievementsMenu::open() {
    Game* game = getGame();

    // ---- 0. first-open per-tile TextItem fill ----
    //
    // matches FUN_100058410 first half. all 3 TextItems use bmfontTable(0)
    // (panel font, game+0x10). title scale 0.08 / desc scale 0.065 /
    // progressText scale 0.06. description shrinks-to-fit when renderedWidth
    // would exceed 490/640 at base scale.
    if (coldStartFlag == 0) {
        coldStartFlag = 1;

        constexpr float kDescBaseScale  = 0.065f;          // DAT_10005a760
        constexpr float kDescMaxVisualW = 490.0f / 640.0f; // DAT_10005a764

        for (int idx = 0; idx < 50; idx++) {
            Tile& tile = tiles[idx];
            const AchievementInfo& info = ACHIEVEMENT_TABLE[idx];

            // title TextItem
            tile.title.glyphTablePtr = game->bmfontTablePtr(0);
            tile.title.setString(info.title, -1);
            tile.title.rgba = 0xFFF0F0F0;
            tile.title.applyColor();
            tile.title.scaleX = 0.08f;
            tile.title.scaleY = 0.08f;
            tile.title.posX   = 84.0f / 640.0f;
            tile.title.posY   = 30.0f / 640.0f;

            // description TextItem
            tile.description.glyphTablePtr = game->bmfontTablePtr(0);
            tile.description.setString(info.description, -1);
            tile.description.rgba = 0xFFA0A0A0;
            tile.description.applyColor();
            tile.description.scaleX = kDescBaseScale;
            tile.description.scaleY = kDescBaseScale;
            tile.description.posX   = 84.0f / 640.0f;
            tile.description.posY   = 56.0f / 640.0f;

            const float baseVisualWidth =
                tile.description.renderedWidth * kDescBaseScale;

            if (baseVisualWidth > kDescMaxVisualW) {
                const float fittedScale =
                    (kDescMaxVisualW / baseVisualWidth) * kDescBaseScale;
                tile.description.scaleX = fittedScale;
                tile.description.scaleY = fittedScale;
            }

            // progressText TextItem (setString + posX/posY happen below)
            tile.progressText.glyphTablePtr = game->bmfontTablePtr(0);
            tile.progressText.rgba = 0xFF828282;
            tile.progressText.applyColor();
            tile.progressText.scaleX = 0.06f;
            tile.progressText.scaleY = 0.06f;
        }
    }

    // ---- 1. transient state reset ----
    //
    // mirrors the flag block at the top of FUN_100058410. visible goes to 1
    // (the page is showing), everything else clears. dragStartTouchY /
    // dragStartScrollY / lastTouchY / scrollVelocity are not cleared by the
    // binary, only scrollY and scrollResidual; they're live during drag only
    // and not relied on across opens.
    visible          = 1;
    closeRequested   = 0;
    backTapConfirmed = 0;
    pressed          = 0;
    scrollDragging   = 0;
    scrollY          = 0.0f;
    scrollResidual   = 0.0f;

    // ---- 2. sortedDisplay clear ----
    //
    // std::map::clear destroys every node and resets the sentinel/size to
    // empty. matches the binary's FUN_10004a3b4 + manual sentinel re-init.
    sortedDisplay.clear();

    // ---- 3. per-tile loop ----
    //
    // DAT constants used inside the loop:
    constexpr float kProgTextOffsetX  = -0.5f  / 640.0f; // DAT_10005a768
    constexpr float kProgTextOffsetY  =  7.0f  / 640.0f; // DAT_10005a76c
    constexpr float kProgBarUvOx      =  509.0f;          // DAT_10005a778, uvOriginPx[0]
    constexpr float kProgBarUvOyBase  =  463.0f;          // DAT_10005a774, uvOriginPx[1] base
    constexpr float kProgBarUvSx      =  55.0f;           // DAT_10005a77c, uvSizePx[0]
    constexpr float kProgBarUvHeight  =  54.0f;           // DAT_10005a770, uvSizePx[1] full
    constexpr float kProgBarPosY62    =  62.0f / 640.0f; // DAT_10005a784/640
    // posX = 0.06171875 (79/1280); preserve as a decimal literal, it rounds
    // back to the binary's bits exactly.
    constexpr float kProgBarPosX      =  0.06171875f;

    AchievementTracker& tracker = game->achievementTracker();

    for (uint32_t idx = 0; idx < 50; idx++) {
        Tile& tile = tiles[idx];

        // 3a. cache locked state, default fadeIn to visible.
        tile.locked = tracker.isLocked(idx) ? 1 : 0;
        tile.fadeIn = 1.0f;

        // 3b. compute signed sort key. positive means show normally
        // (already revealed or unlocked-and-shown). negative means a
        // locked-and-never-shown row that hasn't faded in yet, which sorts
        // to the bottom of the std::map.
        int32_t signedSortKey = static_cast<int32_t>(
            ACHIEVEMENT_TABLE[idx].sortKey);

        if (tile.locked == 0 && !tracker.hasBeenShown(idx)) {
            // unlocked but the banner hasn't been shown yet: start
            // invisible, wait for the scroll-into-view trigger in update()
            // to bump fadeIn from 0 and call markShown().
            tile.fadeIn   = 0.0f;
            signedSortKey = -signedSortKey;
        }

        // 3c. push idx onto sortedDisplay[signedSortKey]. operator[] inserts
        // a default-constructed std::list<int> if the key doesn't exist;
        // matches the binary's FUN_100059208 (find-or-create node) + manual
        // list push_back fixup.
        sortedDisplay[signedSortKey].push_back(static_cast<int>(idx));

        // 3d. progress text + bar.
        int current = 0;
        int target  = 0;
        const int hasProgress =
            tracker.getProgress(idx, &current, &target);

        if (hasProgress != 0) {
            // progress-based achievement: format current count + visual
            // progress bar.
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", current);
            tile.progressText.setString(buf, -1);

            // center progressText horizontally on the icon (with a small
            // left nudge from kProgTextOffsetX), and offset Y by 7/640
            // from icon top.
            tile.progressText.posX =
                tile.icon.posX
              + tile.progressText.renderedWidth * tile.progressText.scaleX * -0.5f
              + kProgTextOffsetX;
            tile.progressText.posY = tile.icon.posY + kProgTextOffsetY;

            // progressBar: animated fill. r = clamp(current/target, 0, 1).
            // UV reveals more of the texture region as progress fills;
            // display size grows vertically with r. ends with FUN_100014d84.
            const float r = std::min(1.0f,
                std::max(0.0f, static_cast<float>(current)
                              / static_cast<float>(target)));

            // p1 = uvOx (fixed), p2 = uvOy base + (1-r)*height (top moves)
            // p3 = uvSx (fixed), p4 = r * uvHeight (size grows with r)
            // p5 = 1.0 (display scale)
            // -> setTexCoords(uvOx/1024, (uvOyBase + (1-r)*uvHeight)/1024,
            //                 (uvOx+uvSx)/1024, (uvOyBase + uvHeight)/1024)
            // -> setSize(uvSx/640, r*uvHeight/640)
            const float p2 = kProgBarUvOyBase + (1.0f - r) * kProgBarUvHeight;
            const float p4 = r * kProgBarUvHeight;
            constexpr float kUvDenom    = 1024.0f;
            constexpr float kSizeDenom  =  640.0f;
            tile.progressBar.setTexCoords(
                kProgBarUvOx           / kUvDenom,  p2                  / kUvDenom,
               (kProgBarUvOx + kProgBarUvSx) / kUvDenom,
               (p2 + p4)               / kUvDenom);
            tile.progressBar.setSize(kProgBarUvSx / kSizeDenom,
                                     p4           / kSizeDenom);

            // anchor the bar's BOTTOM edge at 62/640 (= kProgBarPosY62).
            // posY is the center, so posY = 62/640 - height/2.
            tile.progressBar.posX = kProgBarPosX;
            tile.progressBar.posY = kProgBarPosY62 - tile.progressBar.height * 0.5f;
        } else {
            // one-shot achievement: no progress text or bar.
            tile.progressText.setString("", -1);
            tile.progressBar.setTexCoords(0.0f, 0.0f, 0.0f, 0.0f);
            tile.progressBar.setSize(0.0f, 0.0f);
        }
    }

    // ---- 4. trailing prime tick ----
    //
    // FUN_10005888c with dt 0. matches Shop / LeaderboardMenu's prime-tick
    // pattern.
    update(0.0f);
}

// ----------------------------------------------------------------------
// FUN_10005888c, per-frame update.
// ----------------------------------------------------------------------
//
// 3 concerns folded into one function:
//   1. touch input (press/release of backButton, drag-to-scroll start)
//   2. scroll inertia + rubber-band bounce-back math
//   3. per-tile fadeIn animation
//
// touchX/touchY/inputState come from the Game struct via getGame()
// (FUN_100043798). dt is passed in.
void AchievementsMenu::update(float dt) {
    // clear the vestigial backTapConfirmed every frame (write but never
    // read; preserved for binary parity).
    backTapConfirmed = 0;

    Game* game = getGame();

    // ---- 1. touch input ----
    const int phase = game->inputState();

    if (phase == 1) {
        // press
        const float touchX = game->touchX();
        const float touchY = game->touchY();

        if (backButton.contains(touchX, touchY)) {
            // press inside the back button: tint grey, queue press sound
            pressed = 1;
            backButton.setColor       (0xB4, 0xB4, 0xB4, 0xFF);
            backButtonOverlay.setColor(0xB4, 0xB4, 0xB4, 0xFF);
            game->soundQueue.trigger(5);
        } else {
            // press outside the back button. if touch is below the chromeB
            // header band (touchY > chromeB.posY + chromeB.height/2), it's
            // a list interaction, start a drag.
            const float chromeBBottom = chromeB.posY + chromeB.height * 0.5f;

            if (touchY > chromeBBottom) {
                scrollDragging   = 1;
                dragStartTouchY  = touchY;
                lastTouchY       = touchY;
                dragStartScrollY = scrollY;
                scrollResidual   = 0.0f;
                scrollVelocity   = 0.0f;

                // check if the touch landed on a banner-pending tile and
                // trigger its reveal. walks sortedDisplay in iteration
                // order, advancing a global tileSlot counter (= the
                // vertical position in the scrolling list).
                constexpr float kTileStrideDenom = 640.0f;       // DAT_10005a788
                constexpr float kTileBodyHeight  = 84.0f  / 640.0f; // DAT_10005a78c
                constexpr float kRevealZoneHeight= 73.0f  / 640.0f; // DAT_10005a790
                constexpr int   kTileStridePx    = 0x49;          // 73 px

                int tileSlot = -1;
                const float vh = Renderer::getVirtualHeight();
                bool revealed = false;

                for (auto& kv : sortedDisplay) {
                    if (revealed) {
                        break;
                    }

                    for (int idx : kv.second) {
                        tileSlot += 1;
                        // per-iter stride 0x49, so slot 0 -> listY 0,
                        // slot 1 -> 73/640, and so on.
                        const float tileYInList =
                            static_cast<float>(tileSlot * kTileStridePx)
                          / kTileStrideDenom;
                        const float tileY = tileYInList - scrollY;

                        if (tileY < -0.15625f) {
                            // tile above viewport, keep walking
                            continue;
                        }

                        if (tileY > vh) {
                            // tile below viewport, stop the inner loop; map
                            // order is increasing tileY so the rest are also
                            // below.
                            revealed = true;
                            break;
                        }

                        Tile& tile = tiles[idx];

                        if (tile.fadeIn != 0.0f) {
                            // not a banner-pending tile, skip
                            continue;
                        }

                        // reveal-zone hit-test: touchY within
                        // [tileY + tileBodyHeight, tileY + tileBodyHeight + revealZoneHeight]
                        const float zoneTop    = tileY + kTileBodyHeight;
                        const float zoneBottom = zoneTop + kRevealZoneHeight;

                        if (touchY > zoneTop && touchY < zoneBottom) {
                            // trigger reveal
                            tile.fadeIn = 0.01f;
                            game->achievementTracker().markShown(
                                static_cast<uint32_t>(idx));
                            // reward 1 key for the achievement reveal
                            game->shop().addKeys(1);
                            game->soundQueue.trigger(0x33);
                            revealed = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    // ---- 2. drag-time scroll update + rubber-band ----
    if (scrollDragging != 0) {
        const float touchY = game->touchY();

        // clamped delta from last-frame touchY, capped at +/-5*dt to limit
        // tracking velocity from teleporting jumps.
        const float deltaCap = dt * 5.0f;
        const float rawDelta = touchY - lastTouchY;
        const float clampedDelta =
            std::min(deltaCap, std::max(-deltaCap, rawDelta));
        const float effectiveTouchY = lastTouchY + clampedDelta;

        // linear-tracking scrollY (will be rubber-banded below if out of range).
        float linearScrollY =
            (dragStartScrollY + dragStartTouchY) - effectiveTouchY;
        scrollY = linearScrollY;

        // scrollMax = bottom-of-list position (5.703125 + 84/640 - vh).
        const float vh = Renderer::getVirtualHeight();
        constexpr float kScrollMaxBase = 5.703125f;       // DAT_10005a794
        constexpr float kTileBodyHeight= 84.0f / 640.0f;  // DAT_10005a78c
        const float scrollMax = (kScrollMaxBase - vh) + kTileBodyHeight;

        // rubber-band: damp scroll positions outside [0, scrollMax]. the
        // damping factor goes from 1 (in-range) to 11 (far out of range)
        // via a cosine smoothstep on the overrun * 64.
        float overrun;
        if (linearScrollY >= 0.0f) {
            overrun = (linearScrollY <= scrollMax)
                    ? 0.0f
                    : (linearScrollY - scrollMax);
        } else {
            overrun = -linearScrollY;
        }

        constexpr float kOverrunScale = 64.0f;      // DAT_10005a798
        constexpr float kPi           = 3.1415927f;  // DAT_10005a79c
        const float smoothT = std::min(1.0f, std::max(0.0f,
                                       overrun * kOverrunScale));
        const float cosFactor = 0.5f - std::cos(smoothT * kPi) * 0.5f;
        const float damping   = cosFactor * 10.0f + (1.0f - cosFactor);
        const float dampedOverrun = overrun / damping;

        if (linearScrollY > scrollMax) {
            scrollY = dampedOverrun + scrollMax;
        } else if (linearScrollY < 0.0f) {
            scrollY = -dampedOverrun;
        }
        // else: in range, keep the linear value already written above.

        // exponential-smoothed scroll velocity:
        //   v = 0.9 * v_prev + 0.1 * ((lastY - newY) / max(dt, 0.001))
        constexpr float kMinDt     = 0.001f;  // DAT_10005a7a0
        constexpr float kVelRetain = 0.9f;    // DAT_10005a7a4
        constexpr float kVelNew    = 0.1f;    // DAT_10005a7a8
        const float clampedDt = std::max(dt, kMinDt);
        scrollVelocity =
            scrollVelocity * kVelRetain
          + ((lastTouchY - effectiveTouchY) / clampedDt) * kVelNew;
        lastTouchY = effectiveTouchY;
    }

    // ---- 3. release handling ----
    if (game->inputState() == 0) {
        if (pressed != 0) {
            pressed = 0;
            backButton.setColor       (0xFF, 0xFF, 0xFF, 0xFF);
            backButtonOverlay.setColor(0xFF, 0xFF, 0xFF, 0xFF);

            const float touchX = game->touchX();
            const float touchY = game->touchY();

            if (backButton.contains(touchX, touchY)) {
                closeRequested = 1;
                game->soundQueue.trigger(6);
            } else {
                game->soundQueue.trigger(7);
            }
        }

        if (scrollDragging != 0) {
            scrollDragging = 0;
            // residual = clamp(velocity, -4, 4), preserves momentum.
            scrollResidual = std::min(4.0f, std::max(-4.0f, scrollVelocity));
        }
    }

    // ---- 4. scrollResidual friction-decay ----
    //
    // shrink |scrollResidual| by 2*dt per frame, preserving sign.
    {
        float mag = std::fabs(scrollResidual) - dt * 2.0f;

        if (mag < 0.0f) {
            mag = 0.0f;
        }

        // sign multiplier: +1 when residual >= 0, -1 when < 0 (binary indexes
        // a 2-float table at DAT_10005a7c8 by the sign bit).
        const float sign = (scrollResidual < 0.0f) ? -1.0f : 1.0f;
        scrollResidual = sign * mag;
    }

    // apply residual to scrollY
    scrollY += scrollResidual * dt;

    // ---- 5. post-residual bounce-back if not dragging ----
    if (scrollDragging == 0) {
        const float vh = Renderer::getVirtualHeight();
        constexpr float kScrollMaxBase = 5.703125f;
        constexpr float kTileBodyHeight= 84.0f / 640.0f;
        const float scrollMax = (kScrollMaxBase - vh) + kTileBodyHeight;

        bool needFreezeResidual = false;

        if (scrollY >= 0.0f) {
            if (scrollY > scrollMax) {
                // overshot the bottom, pull back toward scrollMax
                float pulled = scrollY - dt;

                if (pulled <= scrollMax) {
                    pulled = scrollMax;
                }

                scrollY = pulled;

                if (scrollResidual > 0.0f) {
                    needFreezeResidual = true;
                }
            }
        } else {
            // overshot the top, pull back toward 0
            float pulled = scrollY + dt;

            if (pulled >= 0.0f) {
                pulled = 0.0f;
            }

            scrollY = pulled;

            if (scrollResidual < 0.0f) {
                needFreezeResidual = true;
            }
        }

        if (needFreezeResidual) {
            // smoothly zero out the residual when crossing the edge.
            const float t = std::min(1.0f, std::max(0.0f, dt * 10.0f));
            scrollResidual = scrollResidual * (1.0f - t);
        }
    }

    // ---- 6. final scrollY clamp (allows slight overscroll for elasticity) ----
    {
        const float vh = Renderer::getVirtualHeight();
        constexpr float kScrollMaxBase   = 5.703125f;
        constexpr float kTileBodyHeight  = 84.0f / 640.0f;
        constexpr float kOverscrollAbove = -50.0f / 640.0f;  // DAT_10005a7b0
        constexpr float kOverscrollBelow =  50.0f / 640.0f;  // DAT_10005a7ac
        const float scrollMax = (kScrollMaxBase - vh) + kTileBodyHeight;
        const float lo = kOverscrollAbove;
        const float hi = scrollMax + kOverscrollBelow;
        scrollY = std::min(hi, std::max(lo, scrollY));
    }

    // ---- 7. per-tile fadeIn animation ----
    //
    // every frame, bump tile.fadeIn toward 1.0 by (dt / 0.3). only the
    // tiles with fadeIn currently in (0, 1) are animated; fadeIn==0 stays
    // at 0 (locked tile, waiting to be revealed), and fadeIn==1 stays at 1.
    const float fadeStep = dt / 0.3f;   // DAT_10005a7b4

    for (int i = 0; i < 50; i++) {
        float f = tiles[i].fadeIn;

        if (f > 0.0f && f < 1.0f) {
            f += fadeStep;
            f = std::min(1.0f, std::max(0.0f, f));
            tiles[i].fadeIn = f;
        }
    }
}

// ----------------------------------------------------------------------
// FUN_100058eb4, draw.
// ----------------------------------------------------------------------
//
// translate by 84/640 vertically (top margin), then walk sortedDisplay in
// map-iteration order. each tile occupies one slot in the list, position =
// slot * 73/640 - scrollY. tiles outside (-0.156, virtualHeight) are
// clipped. visible tiles are drawn inside their own push/pop matrix.
//
// closes with the 4 header chrome Quads under tex 9.
// caller (Game::draw) gates on visible.
void AchievementsMenu::draw() {
    constexpr float kTopMargin       = 84.0f / 640.0f;   // DAT_10005a7b8
    constexpr float kTileStrideDenom = 640.0f;           // DAT_10005a7bc
    constexpr float kPulseCosScale   = 3.1415927f;        // DAT_10005a7c0
    constexpr float kAlphaScale      = 255.0f;           // DAT_10005a7c4
    constexpr int   kTileStridePx    = 0x49;             // 73 px per tile
    const float vh = Renderer::getVirtualHeight();

    glPushMatrix();
    glTranslatef(0.0f, kTopMargin, 0.0f);

    int tileSlot = -1;
    bool stopAll = false;

    for (auto& kv : sortedDisplay) {
        if (stopAll) {
            break;
        }

        for (int idx : kv.second) {
            tileSlot += 1;
            // stride 0x49 per slot: slot 0 -> listY 0, slot 1 -> 73/640,
            // up to slot 49 -> 49*73/640.
            const float tileY =
                static_cast<float>(tileSlot * kTileStridePx)
              / kTileStrideDenom - scrollY;

            if (tileY < -0.15625f) {
                continue;
            }

            if (tileY > vh) {
                // strict greater (binary b.gt).
                stopAll = true;
                break;
            }

            Tile& tile = tiles[idx];

            glPushMatrix();
            glTranslatef(0.0f, tileY, 0.0f);

            bindTexture(9);

            // row-parity palette. fadeIn drives a cos-smoothed lerp from
            // primary (banner-pending color) to secondary (settled color).
            uint32_t primaryRgba;
            uint32_t secondaryRgba;

            if ((tileSlot & 1) == 0) {
                primaryRgba   = 0xFF3CFFFFu;   // even: bright yellow-white
                secondaryRgba = 0xFF6E5064u;   // even: dark grayish-purple
            } else {
                primaryRgba   = 0xFF3CE6E6u;   // odd: slightly muted yellow
                secondaryRgba = 0xFF644B55u;   // odd: slightly cooler dark
            }

            // mix via FUN_10003a808 with param_4=1, param_5=0: cos-smoothed,
            // not doubled. t = 0.5 - cos(fadeIn * pi) * 0.5.
            const float t = 0.5f - std::cos(tile.fadeIn * kPulseCosScale) * 0.5f;
            const float invT = 1.0f - t;

            auto chMix = [&](uint32_t a, uint32_t b, int shift) -> uint32_t {
                const int ca = static_cast<int>(((a >> shift) & 0xFFu));
                const int cb = static_cast<int>(((b >> shift) & 0xFFu));
                const int m  = static_cast<int>(static_cast<float>(ca) * invT
                                              + static_cast<float>(cb) * t);
                return static_cast<uint32_t>(m & 0xFF) << shift;
            };

            const uint32_t mixed =
                chMix(primaryRgba, secondaryRgba,  0) |   // R
                chMix(primaryRgba, secondaryRgba,  8) |   // G
                chMix(primaryRgba, secondaryRgba, 16) |   // B
                chMix(primaryRgba, secondaryRgba, 24);    // A

            tileBgLabel.setColor(
                static_cast<uint8_t>( mixed        & 0xFF),
                static_cast<uint8_t>((mixed >>  8) & 0xFF),
                static_cast<uint8_t>((mixed >> 16) & 0xFF),
                static_cast<uint8_t>((mixed >> 24) & 0xFF));
            tileBgLabel.draw();

            tile.progressBar.draw();
            tile.progressText.draw();

            bindTexture(9);

            // read the freshly-set row color from the first glyph's vertex
            // 0; both decoration quads pick up that tint.
            const uint32_t rowRgba =
                tileBgLabel.glyphs[0].quad.vertices[0].color;

            if (tile.locked == 0) {
                // unlocked tile: highlight + icon + animated unlockedDeco
                unlockedHighlight.setColor(
                    static_cast<uint8_t>( rowRgba        & 0xFF),
                    static_cast<uint8_t>((rowRgba >>  8) & 0xFF),
                    static_cast<uint8_t>((rowRgba >> 16) & 0xFF),
                    static_cast<uint8_t>((rowRgba >> 24) & 0xFF));
                unlockedHighlight.draw();

                bindTexture(0xD);
                tile.icon.draw();

                bindTexture(8);

                // pulse animation on unlockedDeco:
                //   scaleX = scaleY = (0.5 - cos(fadeIn*pi)*0.5) + 1
                //   alpha  = (1 - (0.5 - cos*0.5)) * 255
                // at fadeIn=0: scale=1.0, alpha=255 (full on)
                // at fadeIn=1: scale=2.0, alpha=0   (faded out via scale-up)
                const float cosArg = tile.fadeIn * kPulseCosScale;
                const float u      = 0.5f - std::cos(cosArg) * 0.5f;
                const float scale  = u + 1.0f;
                unlockedDeco.scaleX = scale;
                unlockedDeco.scaleY = scale;
                unlockedDeco.setAlpha(
                    static_cast<uint8_t>(static_cast<int>(
                        (1.0f - u) * kAlphaScale)));
                unlockedDeco.draw();
            } else {
                // locked tile: just lockedDeco tinted with row color
                lockedDeco.setColor(
                    static_cast<uint8_t>( rowRgba        & 0xFF),
                    static_cast<uint8_t>((rowRgba >>  8) & 0xFF),
                    static_cast<uint8_t>((rowRgba >> 16) & 0xFF),
                    static_cast<uint8_t>((rowRgba >> 24) & 0xFF));
                lockedDeco.draw();
            }

            tile.title.draw();
            tile.description.draw();

            glPopMatrix();
        }
    }

    glPopMatrix();

    // ---- header chrome ----
    bindTexture(9);
    chromeA.draw();
    chromeB.draw();
    backButton.draw();
    backButtonOverlay.draw();
}
