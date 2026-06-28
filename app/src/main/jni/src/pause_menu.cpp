#include "pause_menu.h"

#include "game.h"
#include "label.h"
#include "menu.h"
#include "quad.h"
#include "renderer.h"      // bindTexture
#include "sound_queue.h"
#include "text_item.h"

#include <GLES/gl.h>
#include <algorithm>

// ----------------------------------------------------------------------
// DAT constants for PauseMenu::init (FUN_10002e838).
// ----------------------------------------------------------------------

static constexpr float kCtorPanelHeight       = 0.640625f;    // DAT_10005a0f0
static constexpr float kCtorLabelWidth        = 0.3734375f;   // DAT_10005a0f4
static constexpr float kCtorTab0Anchor        = 0.046875f;    // DAT_10005a0f8
static constexpr float kCtorTab2Anchor        = 0.4859375f;   // DAT_10005a0fc
static constexpr float kCtorTab1Anchor        = 0.1515625f;   // DAT_10005a100
static constexpr float kCtorIconYOffset       = -0.0015625f;  // DAT_10005a104
static constexpr float kCtorTab1IconXBump     = 0.015625f;    // DAT_10005a108
static constexpr float kCtorStandaloneXOffset = 0.0546875f;   // DAT_10005a10c
static constexpr float kCtorStandaloneYAnchor = 0.0515625f;   // DAT_10005a110

static constexpr float kCtorPanelWidth        = 0.46875f;
// the binary's hardcoded anchorY = 0.3125 is iOS-tuned (vh ~= 1.5). our port
// uses Menu::setSizeAndCenterY to derive the anchor from runtime virtualHeight
// (matches LevelUpPanel's pattern for the same reason).

// helper-FUN_10002f5dc constants (slider position update on row drag).
static constexpr float kSliderDragHalfRange   = 0.13124999f;  // DAT_10005a114
static constexpr float kSliderQuadBOffsetX    = -0.13124999f; // DAT_10005a118
static constexpr float kSliderQuadBRangeX     = 0.26249999f;  // DAT_10005a11c
static constexpr float kSliderQuadBOffsetY    = 0.0015625f;   // DAT_10005a120
static constexpr float kSliderQuadCOffset     = -0.0015625f;  // DAT_10005a124

// dimmed touch-feedback color (Label::setColor RGBA 200,200,200,255).
static constexpr uint8_t kPressDimR = 200, kPressDimG = 200, kPressDimB = 200, kPressDimA = 0xFF;

// ----------------------------------------------------------------------
// DAT constants for ForfeitConfirmPanel::init (FUN_100042cf4).
// ----------------------------------------------------------------------

static constexpr float kForfeitPanelWidth     = 0.6328125f;   // DAT_10005a470
static constexpr float kForfeitPanelHeight    = 0.3968750f;   // DAT_10005a474
// kForfeitAnchorYIos = DAT_10005a478 = 0.7828, iOS-tuned. our port uses
// Menu::setSizeAndCenterY so the popup centers correctly on Android too.
static constexpr float kForfeitLabelWidth     = 0.5734375f;   // DAT_10005a47c
static constexpr float kForfeitLabelX         = 0.0296875f;   // DAT_10005a480
static constexpr float kForfeitLabelY         = 0.0281250f;   // DAT_10005a484
static constexpr float kForfeitButtonWidth    = 0.2718750f;   // DAT_10005a488
static constexpr float kForfeitBtn0AnchorX    = 0.0468750f;   // DAT_10005a48c
static constexpr float kForfeitBtnIconYOffset = -0.0015625f;  // DAT_10005a490
static constexpr float kForfeitBtn1AnchorX    = 0.3125f;      // immediate in iter 1 branch
static constexpr float kForfeitButtonAnchorY  = 0.2421875f;   // immediate in both iter branches

// posX/posY for the 3 decorative icons of the popup.
static constexpr float kForfeitIcon0PosX = 0.32499998f;
static constexpr float kForfeitIcon0PosY = 0.0671875f;   // IEEE 0x3d89999a (forfeitTitle posY)
static constexpr float kForfeitIcon1PosX = 0.315625f;    // IEEE 0x3ea1999a (warnLine1 posX)
static constexpr float kForfeitIcon1PosY = 0.15625000f;
static constexpr float kForfeitIcon2PosX = 0.315625f;    // IEEE 0x3ea1999a (warnLine2 posX)
static constexpr float kForfeitIcon2PosY = 0.20312500f;

// ----------------------------------------------------------------------
// PauseMenu, FUN_10002e838.
// ----------------------------------------------------------------------

void PauseMenu::init() {
    // FUN_100057424: Menu chrome with empty title bar.
    Menu::init(0, 0, 0, 0);

    // C++ virtual dispatch installs PauseMenu's vtable (= the binary's
    // `*param_1 = &PTR_FUN_1000745a0`) automatically via the override list.

    // tabs: Label + zero-init icon Quad each.
    for (int i = 0; i < 3; i++) {
        tabs[i].label.init();
        tabs[i].iconQuad = Quad();
    }

    // info rows: 3 Quads each, zero-init.
    for (int i = 0; i < 2; i++) {
        volumeSliders[i].quadA = Quad();
        volumeSliders[i].quadB = Quad();
        volumeSliders[i].quadC = Quad();
    }

    tutorialFlag = 0;

    tutorialUnchecked = Quad();
    tutorialChecked   = Quad();

    forfeitConfirmPanel.init();

    // trailing std::vector, already value-initialized by C++; nothing to
    // do here. open() drops + rebuilds entries per show.

    // Menu chrome sizing + anchor. binary hardcodes anchorY = 0.3125,
    // calibrated for iOS's virtualHeight ~= 1.5 (vertically-centered there).
    // on Android (vh ~= 2.2) that same constant pushes the panel against the
    // top; Menu::setSizeAndCenterY derives the anchor from the runtime vh so
    // the panel stays vertically centered on any aspect.
    setSizeAndCenterY(kCtorPanelWidth, kCtorPanelHeight);

    // per-tab icon parameters. atlas UVs decode to:
    //   tabs[0] icon = small right-pointing arrow (Main Menu)
    //   tabs[1] icon = speaker / sound glyph     (Tutorial label; the
    //                                              tutorialChecked /
    //                                              tutorialUnchecked Quads
    //                                              carry the on/off state)
    //   tabs[2] icon = book / details            (Forfeit)
    struct TabIconParams {
        float u0, v0, u1, v1, w, h;
    };

    static constexpr TabIconParams kTabIcons[3] = {
        // tabs[0], Main Menu (small right-pointing arrow)
        { 0.81640625f, 0.24023438f, 1.0f,         0.27050781f,
          0.29375f,    0.04843750f                                  },
        // tabs[1], Tutorial (speaker glyph; standaloneQuads carry state)
        { 0.87207031f, 0.27148438f, 1.0f,         0.30273438f,
          0.20468750f, 0.05000000f                                  },
        // tabs[2], Forfeit (book / details glyph)
        { 0.89160156f, 0.30371094f, 1.0f,         0.33496094f,
          0.17343750f, 0.05000000f                                  },
    };

    static constexpr float kTabAnchors[3] = {
        kCtorTab0Anchor, kCtorTab1Anchor, kCtorTab2Anchor,
    };

    // 3-glyph 9-slice frame per tab. binary's stack-local uv tuples:
    static constexpr GlyphOffset kZeroOffset = { 0.0f, 0.0f };

    for (int i = 0; i < 3; i++) {
        Label& label = tabs[i].label;
        Quad&  icon  = tabs[i].iconQuad;

        {
            const float uvOrigin[2] = { 718.0f, 246.0f };
            const float uvSize[2]   = { 26.0f,  68.0f };
            label.addGlyph(-1.0f, uvOrigin, uvSize, 2, kZeroOffset);
        }
        {
            const float uvOrigin[2] = { 744.0f, 246.0f };
            const float uvSize[2]   = { 1.0f,   68.0f };
            label.addGlyph(-1.0f, uvOrigin, uvSize, 1, kZeroOffset);
        }
        {
            const float uvOrigin[2] = { 745.0f, 246.0f };
            const float uvSize[2]   = { 26.0f,  68.0f };
            label.addGlyph(-1.0f, uvOrigin, uvSize, 2, kZeroOffset);
        }

        // measureGlyphRun(-1, -1) returns (advanceWidth, heightTotal) as a
        // paired (s0, s1) float. Ghidra's decomp drops s1, which made the
        // setSize-height arg look like a `uVar15` running propagation; it
        // isn't. for filterOpen mode s1 = the last glyph's natural height
        // (= 68/640 = 0.10625 for the 9-slice corner glyph), so every tab's
        // cachedSize1 lands at the natural glyph height.
        const GlyphRunMetrics runMetrics = label.measureGlyphRun(-1, -1);
        label.setSize(kCtorLabelWidth, runMetrics.heightTotal);
        label.setPosition(kCtorTab0Anchor, kTabAnchors[i]);

        const TabIconParams& ic = kTabIcons[i];
        icon.setTexCoords(ic.u0, ic.v0, ic.u1, ic.v1);
        icon.setSize(ic.w, ic.h);

        // FUN_10000aefc, getCenter() returns (centerX, centerY) as a paired
        // float in (s0, s1). Ghidra's decomp drops s1 again, so it looks like
        // the iconY is computed from the per-tab anchor; the asm actually uses
        // s1 (= centerY = topY + height/2) for the Y. result: each icon sits
        // vertically centered inside its label.
        const float centerX = label.getLeftX() + label.getWidth() * 0.5f;
        const float centerY = label.topY       + label.getHeight() * 0.5f;
        icon.posX = centerX;
        icon.posY = centerY + kCtorIconYOffset;
        icon.snapToPixelGrid();
    }

    // binary's post-loop tweak: nudge tabs[1].iconQuad.posX by +0.015625.
    // matters because the Tutorial-toggle icon needs to shift slightly to
    // make room for the standaloneQuads (the on/off badge).
    tabs[1].iconQuad.posX += kCtorTab1IconXBump;

    // standalone Tutorial-state badges. UV / size are fixed; only one of
    // the two draws per frame, selected by tutorialFlag.
    tutorialUnchecked.setTexCoords(0.48242188f, 0.20117188f,
                                   0.51757813f, 0.23632813f);
    tutorialUnchecked.setSize(0.05625f, 0.05625f);
    tutorialChecked.setTexCoords(0.88964844f, 0.83691406f,
                                 0.92480469f, 0.87207031f);
    tutorialChecked.setSize(0.05625f, 0.05625f);

    // both standalones anchor on tabs[1].label. binary loads both s0 (leftX)
    // and s1 (topY) from getLeftX's paired return, then adds the X/Y offset
    // constants. Ghidra drops the s1 read, making the Y look like a bare
    // constant; the asm at 0x10002ed00 does `fadd s1,s1,s2`, i.e.
    // standaloneY = tabs[1].label.topY + kCtorStandaloneYAnchor.
    const float standaloneX = tabs[1].label.getLeftX() + kCtorStandaloneXOffset;
    const float standaloneY = tabs[1].label.topY       + kCtorStandaloneYAnchor;
    tutorialUnchecked.posX = standaloneX;
    tutorialUnchecked.posY = standaloneY;
    tutorialChecked.posX = standaloneX;
    tutorialChecked.posY = standaloneY;

    // per-row quadC + posY parameters.
    //   row 0 = SE volume, row 1 = BGM volume. binary inlines distinct UV/
    //   size tuples per row for quadC (the slider thumb foreground).
    struct InfoRowParams {
        float qcU0, qcV0, qcU1, qcV1, qcW, qcH, posY;
    };

    static constexpr InfoRowParams kInfoRows[2] = {
        // row 0 (SE volume slider). posY = 0.30625.
        { 0.30078125f, 0.46777344f, 0.328125f,   0.49707031f,
          0.04375f,    0.046875f,                              0.30625f },
        // row 1 (BGM volume slider). posY = 0.40312.
        { 0.60058594f, 0.72363281f, 0.625f,      0.75292969f,
          0.0390625f,  0.046875f,                              0.40312f },
    };

    // quadA / quadB UV+size are shared across both rows.
    const float rowQuadAUV[4]   = { 0.62597656f, 0.72949219f,
                                     0.84960938f, 0.78808594f };
    const float rowQuadASize[2] = { 0.35781250f, 0.09375f };

    const float rowQuadBUV[4]   = { 0.11816406f, 0.66894531f,
                                     0.16015625f, 0.71093750f };
    const float rowQuadBSize[2] = { 0.06718750f, 0.06718750f };

    // posX shared across both rows, anchored to tabs[0]'s label center.
    const float rowPosX = tabs[0].label.getLeftX()
                          + tabs[0].label.getWidth() * 0.5f;

    for (int i = 0; i < 2; i++) {
        PauseMenuInfoRow&     row = volumeSliders[i];
        const InfoRowParams& rp  = kInfoRows[i];

        row.quadA.setTexCoords(rowQuadAUV[0], rowQuadAUV[1],
                                 rowQuadAUV[2], rowQuadAUV[3]);
        row.quadA.setSize(rowQuadASize[0], rowQuadASize[1]);

        row.quadB.setTexCoords(rowQuadBUV[0], rowQuadBUV[1],
                                 rowQuadBUV[2], rowQuadBUV[3]);
        row.quadB.setSize(rowQuadBSize[0], rowQuadBSize[1]);

        // initial slider position (0.5 = center). open() overwrites with
        // the current seVolume / bgmVolume seed.
        row.linearValue = 0.5f;

        row.quadA.posX = rowPosX;
        row.quadA.posY = rp.posY;

        row.quadC.setTexCoords(rp.qcU0, rp.qcV0, rp.qcU1, rp.qcV1);
        row.quadC.setSize(rp.qcW, rp.qcH);
    }
}

// FUN_10002f5dc, drag-position update for one info row.
// maps a normalized 0..1 input into quadB / quadC positions: quadB slides
// along the row's X axis, quadC tracks quadB with a small offset.
static void positionSlider(PauseMenu& panel, int rowIdx, float normalized) {
    const float clamped = std::clamp(normalized, 0.0f, 1.0f);
    PauseMenuInfoRow& row = panel.volumeSliders[rowIdx];

    row.linearValue = clamped;

    // quadB.posX = lerp between (quadA.posX + offsetX, quadA.posX + offsetX + range).
    const float qbOffX = row.quadA.posX + kSliderQuadBOffsetX;
    const float qbX    = (1.0f - clamped) * qbOffX
                         + clamped * (qbOffX + kSliderQuadBRangeX);
    const float qbY    = row.quadA.posY + kSliderQuadBOffsetY;

    row.quadB.posX = qbX;
    row.quadB.posY = qbY;

    // quadC tracks quadB with a small (-0.0015625, -0.0015625) offset.
    row.quadC.posX = qbX + kSliderQuadCOffset;
    row.quadC.posY = qbY + kSliderQuadCOffset;
}

// FUN_10002f688, open the pause menu.
void PauseMenu::open(float seVolumeSeed, float bgmVolumeSeed, char tutorialModeSeed) {
    panelShow();                          // FUN_100057ac8 (Menu chrome show)
    selectedTabIndex   = -1;
    secondaryTabIndex  = -1;
    scoreRequest       = 0;
    exitRequest        = 0;
    tutorialFlag       = tutorialModeSeed;

    positionSlider(*this, 0, seVolumeSeed);
    positionSlider(*this, 1, bgmVolumeSeed);

    forfeitConfirmPanel.panelHide(true);  // FUN_10004368c (= Menu::panelHide(1))

    // FUN_10002f688's tail pop-loop is libc++'s vector::clear in disguise.
    trailingItems.clear();

    // prime one update tick at dt = 0; touchInput goes through as 0 too.
    update(0.0f, 0.0f);
}

// FUN_10002f0cc, draw.
void PauseMenu::draw() {

    if (!visible) {
        return;
    }

    Menu::draw();                         // chrome (bgDim + frame9slice + title)

    glPushMatrix();
    glTranslatef(anchorX, anchorY, 0.0f);
    bindTexture(9);

    for (int i = 0; i < 3; i++) {
        tabs[i].label.draw();
        tabs[i].iconQuad.draw();
    }

    // Tutorial on/off badge: pick which standalone draws based on
    // tutorialFlag.
    if (tutorialFlag == 0) {
        tutorialUnchecked.draw();
    } else {
        tutorialChecked.draw();
    }

    for (int i = 0; i < 2; i++) {
        volumeSliders[i].quadA.draw();
        volumeSliders[i].quadB.draw();
        volumeSliders[i].quadC.draw();
    }

    glPopMatrix();

    forfeitConfirmPanel.draw();           // popup chrome + buttons + question

    for (TextItem& item : trailingItems) {
        item.draw();
    }
}

// FUN_10002f734, fade chrome + every tab/standalone/row Quad to alpha `a`.
void PauseMenu::setAlpha(uint8_t a) {
    Menu::setAlpha(a);                    // FUN_100057b58, chrome fade

    for (int i = 0; i < 3; i++) {
        tabs[i].label.setAlpha(a);
        tabs[i].iconQuad.setAlpha(a);
    }

    tutorialUnchecked.setAlpha(a);
    tutorialChecked.setAlpha(a);

    for (int i = 0; i < 2; i++) {
        volumeSliders[i].quadA.setAlpha(a);
        volumeSliders[i].quadB.setAlpha(a);
        volumeSliders[i].quadC.setAlpha(a);
    }
}

// FUN_10002f1d0, touch state machine + fade tick.
// gameState codes mirror the binary's `getGame()->inputState()`:
//   0 = released (handle confirm of any pressed tab / row),
//   1 = press    (hit-test bounds, then 3 tabs, then 2 rows),
//   2 = drag     (if a row is held, slide it).
void PauseMenu::update(float dt, float touchInput) {
    (void)touchInput;
    baseUpdate(dt);                       // FUN_1000578f8, fade + confirm-button state

    // when the Forfeit popup is up, route input there and consume its
    // confirmResult afterward.
    if (forfeitConfirmPanel.visible) {
        forfeitConfirmPanel.update(dt, touchInput);

        // binary's chain (FUN_10002f1d0 tail):
        //   if (visible && !secondaryVisible)         return;   // parent closing
        //   if (!forfeitConfirmPanel.visible)         return;   // popup already closed
        //   if (forfeitConfirmPanel.secondaryVisible) return;   // popup opening
        //   if (!forfeitConfirmPanel.confirmResult)   return;   // No (cancel)
        //   scoreRequest = 1;
        //   panelHide(false);
        if (visible && !secondaryVisible) {
            return;
        }

        if (!forfeitConfirmPanel.visible) {
            return;
        }

        if (forfeitConfirmPanel.secondaryVisible) {
            return;
        }

        if (!forfeitConfirmPanel.confirmResult) {
            return;
        }

        // popup closed and user confirmed Forfeit: flag score-requested
        // + close PauseMenu. GameBoard's consumer sees scoreRequest = 1
        // and routes through the ScorePanel exit path.
        scoreRequest = 1;
        panelHide(false);
        return;
    }

    if (!visible) {
        return;
    }

    if (animTimer0 < 1.0f) {
        return;
    }

    Game* game = getGame();

    if (game == nullptr) {
        return;
    }

    const int  state  = game->inputState();
    const float touchX = game->touchX();
    const float touchY = game->touchY();
    const float localX = touchX - anchorX;
    const float localY = touchY - anchorY;

    if (state == 1) {                     // PRESS
        // hit-test panel chrome bounds first. binary's FUN_10002f1d0
        // falls through to the bottom panelHide(false) when this misses,
        // i.e. tapping anywhere off the panel resumes the game (the only
        // "close without committing" path; there's no explicit Resume
        // button in the chrome). state==1+inside-chrome paths return via
        // the tab / row handlers below.
        if (!frame9slice.contains(localX, localY)) {
            panelHide(false);
            return;
        }

        // hit-test the 3 tabs in order.
        for (int i = 0; i < 3; i++) {

            if (tabs[i].label.contains(localX, localY)) {
                tabs[i].label.setColor(kPressDimR, kPressDimG, kPressDimB, kPressDimA);
                tabs[i].iconQuad.setColor(kPressDimR, kPressDimG, kPressDimB, kPressDimA);
                selectedTabIndex = i;

                // tabs[1] is the Tutorial toggle; also dim both standalone
                // badges so the un-shown one matches the in-tap feedback.
                if (i == 1) {
                    tutorialUnchecked.setColor(kPressDimR, kPressDimG, kPressDimB, kPressDimA);
                    tutorialChecked.setColor(kPressDimR, kPressDimG, kPressDimB, kPressDimA);
                }

                game->soundQueue.trigger(5);
                return;
            }
        }

        // no tab hit; try the 2 slider rows' quadA bounds.
        for (int i = 0; i < 2; i++) {

            if (volumeSliders[i].quadA.contains(localX, localY)) {
                volumeSliders[i].quadB.setColor(kPressDimR, kPressDimG, kPressDimB, kPressDimA);
                volumeSliders[i].quadC.setColor(kPressDimR, kPressDimG, kPressDimB, kPressDimA);
                secondaryTabIndex = i;
                game->soundQueue.trigger(5);
                return;
            }
        }

        return;
    }

    if (state == 2) {                     // DRAG, slide active info row.

        if (secondaryTabIndex == -1) {
            return;
        }

        // binary formula: (((touchX - panelAnchorX) - row.quadA.posX) /
        //                  0.13125 + 1.0) * 0.5
        const PauseMenuInfoRow& row = volumeSliders[secondaryTabIndex];
        const float normalized = ((localX - row.quadA.posX)
                                   / kSliderDragHalfRange + 1.0f) * 0.5f;
        positionSlider(*this, secondaryTabIndex, normalized);
        return;
    }

    // state == 0, RELEASE.
    if (selectedTabIndex != -1) {
        // re-hit-test the same tab; if still under finger, fire its action.
        const int tabIdx = selectedTabIndex;
        const bool stillOnTab = tabs[tabIdx].label.contains(localX, localY);

        if (stillOnTab) {

            if (tabIdx == 0) {
                // Main Menu: request the case-5 transition back to title
                // (no score panel).
                exitRequest = 1;
            } else if (tabIdx == 1) {
                // Tutorial toggle: XOR the mode byte. GameBoard's consumer
                // wipes the hint-state region so hints re-fire for the new
                // mode the next time the player resumes.
                tutorialFlag ^= 1;

                // un-dim both standalone badges (press-time dim at lines
                // 456-457 restored to white). FUN_10002f1d0 0x10002f404-f424.
                tutorialUnchecked.setColor(0xFF, 0xFF, 0xFF, 0xFF);
                tutorialChecked.setColor(0xFF, 0xFF, 0xFF, 0xFF);
            } else if (tabIdx == 2) {
                // Forfeit: pop up the "Forfeit Run?" confirmation.
                forfeitConfirmPanel.open();
            }

            game->soundQueue.trigger(6);  // tap-confirm sound
        } else {
            game->soundQueue.trigger(7);  // tap-cancel sound
        }

        // reset tab visuals + clear selection.
        tabs[tabIdx].label.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        tabs[tabIdx].iconQuad.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        selectedTabIndex = -1;
        return;
    }

    if (secondaryTabIndex != -1) {
        const int rowIdx = secondaryTabIndex;

        // binary plays sound 0x15 for row 0 (SE-volume release chime) and
        // sound 7 for row 1 (BGM-volume release chime).
        game->soundQueue.trigger((rowIdx == 0) ? 0x15 : 0x07);

        // reset slider visuals.
        volumeSliders[rowIdx].quadB.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        volumeSliders[rowIdx].quadC.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        secondaryTabIndex = -1;
    }
}

// ----------------------------------------------------------------------
// ForfeitConfirmPanel, FUN_100042cf4.
// ----------------------------------------------------------------------

void ForfeitConfirmPanel::init() {
    // Menu chrome with empty title bar.
    Menu::init(0, 0, 0, 0);

    // C++ vtable auto-installs PTR_FUN_100074760 via overrides of
    // update / setAlpha / onConfirmTapped.

    // 2 buttons (Yes / No), Label + iconQuad each.
    for (int i = 0; i < 2; i++) {
        buttons[i].label.init();
        buttons[i].iconQuad = Quad();
    }

    questionLabel.init();
    forfeitTitle = Quad();
    warnLine1 = Quad();
    warnLine2 = Quad();

    // Menu chrome sizing. binary's anchorY = 0.7828 is iOS-tuned (vh ~= 1.5);
    // same Android centering problem as PauseMenu, same fix.
    setSizeAndCenterY(kForfeitPanelWidth, kForfeitPanelHeight);

    // question label: 9-slice frame at top of popup.
    static constexpr GlyphOffset kZeroOffset = { 0.0f, 0.0f };
    {
        const float uvOrigin[2] = { 182.0f, 321.0f };
        const float uvSize[2]   = { 32.0f,  51.0f };
        questionLabel.addGlyph(-1.0f, uvOrigin, uvSize, 2, kZeroOffset);
    }
    {
        const float uvOrigin[2] = { 214.0f, 321.0f };
        const float uvSize[2]   = { 1.0f,   51.0f };
        questionLabel.addGlyph(-1.0f, uvOrigin, uvSize, 1, kZeroOffset);
    }
    {
        const float uvOrigin[2] = { 215.0f, 321.0f };
        const float uvSize[2]   = { 32.0f,  51.0f };
        questionLabel.addGlyph(-1.0f, uvOrigin, uvSize, 2, kZeroOffset);
    }

    // measureGlyphRun(-1, -1): s1 = last glyph's natural height. used as
    // cachedSize1 by setSize (same Ghidra dropped-return pattern as the
    // PauseMenu tab loop above).
    const GlyphRunMetrics qRun = questionLabel.measureGlyphRun(-1, -1);
    questionLabel.setSize(kForfeitLabelWidth, qRun.heightTotal);
    questionLabel.setPosition(kForfeitLabelX, kForfeitLabelY);

    // 3 decorative icons (UV / size + posX/posY).
    forfeitTitle.setTexCoords(0.87890625f, 0.43066406f, 1.0f, 0.46191406f);
    forfeitTitle.setSize(0.19375f, 0.05f);
    forfeitTitle.posX = kForfeitIcon0PosX;
    forfeitTitle.posY = kForfeitIcon0PosY;

    warnLine1.setTexCoords(0.66796875f, 0.70214844f, 1.0f, 0.72851560f);
    warnLine1.setSize(0.53125f, 0.0421875f);
    warnLine1.posX = kForfeitIcon1PosX;
    warnLine1.posY = kForfeitIcon1PosY;
    warnLine1.snapToPixelGrid();

    warnLine2.setTexCoords(0.48242188f, 0.31542970f, 0.63476560f, 0.34277344f);
    warnLine2.setSize(0.24375f, 0.04375f);
    warnLine2.posX = kForfeitIcon2PosX;
    warnLine2.posY = kForfeitIcon2PosY;
    warnLine2.snapToPixelGrid();

    // buttons: 2 iterations, same 9-slice shape as PauseMenu tabs.
    struct ButtonIconParams {
        float u0, v0, u1, v1, w, h;
    };

    static constexpr ButtonIconParams kButtonIcons[2] = {
        // buttons[0], "Yes" / confirm Forfeit (book / Forfeit glyph,
        // same UV as PauseMenu tabs[2]).
        { 0.89160156f, 0.30371094f, 1.0f,        0.33496094f,
          0.17343750f, 0.05000000f                                  },
        // buttons[1], "No" / cancel (X / cancel glyph).
        { 0.89355470f, 0.39941406f, 1.0f,        0.42968750f,
          0.17031250f, 0.04843750f                                  },
    };

    static constexpr float kButtonAnchorXs[2] = {
        kForfeitBtn0AnchorX, kForfeitBtn1AnchorX,
    };

    for (int i = 0; i < 2; i++) {
        Label& label = buttons[i].label;
        Quad&  icon  = buttons[i].iconQuad;

        {
            const float uvOrigin[2] = { 718.0f, 246.0f };
            const float uvSize[2]   = { 26.0f,  68.0f };
            label.addGlyph(-1.0f, uvOrigin, uvSize, 2, kZeroOffset);
        }
        {
            const float uvOrigin[2] = { 744.0f, 246.0f };
            const float uvSize[2]   = { 1.0f,   68.0f };
            label.addGlyph(-1.0f, uvOrigin, uvSize, 1, kZeroOffset);
        }
        {
            const float uvOrigin[2] = { 745.0f, 246.0f };
            const float uvSize[2]   = { 26.0f,  68.0f };
            label.addGlyph(-1.0f, uvOrigin, uvSize, 2, kZeroOffset);
        }

        // same paired-return pattern as PauseMenu tab loop: s1 from
        // measureGlyphRun is the heightTotal.
        const GlyphRunMetrics runMetrics = label.measureGlyphRun(-1, -1);
        label.setSize(kForfeitButtonWidth, runMetrics.heightTotal);
        label.setPosition(kButtonAnchorXs[i], kForfeitButtonAnchorY);

        const ButtonIconParams& ic = kButtonIcons[i];
        icon.setTexCoords(ic.u0, ic.v0, ic.u1, ic.v1);
        icon.setSize(ic.w, ic.h);

        // getCenter() returns (centerX, centerY) paired. centerY is what
        // gets used for the icon Y (not the explicit anchorY arg; see
        // PauseMenu tab loop for the same Ghidra drop).
        const float centerX = label.getLeftX() + label.getWidth() * 0.5f;
        const float centerY = label.topY       + label.getHeight() * 0.5f;
        icon.posX = centerX;
        icon.posY = centerY + kForfeitBtnIconYOffset;
        icon.snapToPixelGrid();
    }
}

// FUN_1000435d0, ForfeitConfirmPanel::open.
void ForfeitConfirmPanel::open() {
    panelShow();                          // FUN_100057ac8 (Menu chrome show)
    selectedButton = -1;
    confirmResult  = 0;

    // prime one update tick at dt=0, touchInput=0.
    update(0.0f, 0.0f);
}

// FUN_100043328, ForfeitConfirmPanel::draw.
void ForfeitConfirmPanel::draw() {

    if (!visible) {
        return;
    }

    Menu::draw();                         // chrome (bgDim + frame9slice + title)

    glPushMatrix();
    glTranslatef(anchorX, anchorY, 0.0f);
    bindTexture(9);

    for (int i = 0; i < 2; i++) {
        buttons[i].label.draw();
        buttons[i].iconQuad.draw();
    }

    questionLabel.draw();
    forfeitTitle.draw();
    warnLine1.draw();
    warnLine2.draw();

    glPopMatrix();
}

// FUN_100057b58 (Menu::setAlpha) + button/label/icon fades.
void ForfeitConfirmPanel::setAlpha(uint8_t a) {
    Menu::setAlpha(a);

    for (int i = 0; i < 2; i++) {
        buttons[i].label.setAlpha(a);
        buttons[i].iconQuad.setAlpha(a);
    }

    questionLabel.setAlpha(a);
    forfeitTitle.setAlpha(a);
    warnLine1.setAlpha(a);
    warnLine2.setAlpha(a);
}

// FUN_1000433c4, ForfeitConfirmPanel::update.
// press: hit-test the 2 buttons + dim. release: fire confirmResult
// (button 0 = 1 / button 1 = 0) and panelHide.
void ForfeitConfirmPanel::update(float dt, float touchInput) {
    (void)touchInput;
    baseUpdate(dt);

    if (!visible) {
        return;
    }

    if (animTimer0 < 1.0f) {
        return;
    }

    Game* game = getGame();

    if (game == nullptr) {
        return;
    }

    const int  state  = game->inputState();
    const float touchX = game->touchX();
    const float touchY = game->touchY();
    const float localX = touchX - anchorX;
    const float localY = touchY - anchorY;

    if (state == 1) {                     // PRESS
        // hit-test panel chrome first.
        if (!frame9slice.contains(localX, localY)) {
            // miss: reset selection + close popup with cancel result.
            confirmResult  = 0;
            panelHide(false);
            return;
        }

        // hit-test the 2 buttons in order.
        for (int i = 0; i < 2; i++) {

            if (buttons[i].label.contains(localX, localY)) {
                buttons[i].label.setColor(kPressDimR, kPressDimG, kPressDimB, kPressDimA);
                buttons[i].iconQuad.setColor(kPressDimR, kPressDimG, kPressDimB, kPressDimA);
                selectedButton = i;
                game->soundQueue.trigger(5);
                return;
            }
        }

        return;
    }

    if (state == 0) {                     // RELEASE
        if (selectedButton == -1) {
            return;
        }

        const int btnIdx = selectedButton;
        const bool stillOnButton =
            buttons[btnIdx].label.contains(localX, localY);

        if (stillOnButton) {
            // button 0 = Yes (Forfeit confirmed). button 1 = No (cancel).
            confirmResult = (btnIdx == 0) ? 1 : 0;
            panelHide(false);
            game->soundQueue.trigger(6);
        } else {
            game->soundQueue.trigger(7);
        }

        // reset button visuals + clear selection.
        buttons[btnIdx].label.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        buttons[btnIdx].iconQuad.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        selectedButton = -1;
    }
}
