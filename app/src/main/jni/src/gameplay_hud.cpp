#include "gameplay_hud.h"
#include "game.h"
#include "detail_panel.h"   // populateForEvent / populateForStatRow (HUD inspect)
#include "renderer.h"   // bindTexture
#include <SDL.h>
#include <GLES/gl.h>
#include <cmath>
#include <cstring>

using namespace GameplayHUDConstants;

// FUN_10000bb60: lay out the 4 health-bar quads (+0xE0, +0x1B8, +0x290, +0x368)
// based on targetHealthRatio (+0x968) and currentHealthRatio (+0x96c).
//
// the bar is 80 source-pixels wide. roles:
//   healthBarFill     = the visible filled health (col A, height = filled_px)
//   healthBarGain     = heal extension above filled (col A, height = gain_px,
//                       nonzero only when currentRatio < targetRatio)
//   healthBarOverflow = damage overlay above filled (col B, height = overflow_px,
//                       nonzero only when currentRatio > targetRatio)
//   healthBarTip      = a tiny cap on top of filled (col A, height = min(filled, 2))
//
// when static (no animation), gain_px = overflow_px = 0 and the gain/overflow
// quads collapse to height 0. the tip is at most 2px and sits flush on top of
// the fill bar; only the fill is visibly distinct.
static void layoutStatBars(GameplayHUD* h) {
    constexpr float BAR_PX   = 80.0f;
    constexpr float UV_SCL   = 1.0f / 1024.0f;
    constexpr float SIZE_SCL = 640.0f;
    constexpr float BAR_W    = 0.096875f;

    float current = h->currentHealthRatio;
    float target  = h->targetHealthRatio;

    // resolve the case (matches the binary's branchy if/else):
    //   current <  target -> gain     = (target - current) * 80, clamp current to target
    //   current >  target -> overflow = (current - target) * 80, clamp current to target
    //   current == target -> neither, no clamp
    float gainPx     = 0.0f;
    float overflowPx = 0.0f;
    float clampedCurrent = current;

    if (current < target) {
        gainPx = (target - current) * BAR_PX;
    } else if (current > target) {
        overflowPx     = (current - target) * BAR_PX;
        clampedCurrent = target;
    }

    float filledPx = clampedCurrent * BAR_PX;
    float emptyPx  = BAR_PX - filledPx;
    float emptyUV  = emptyPx * UV_SCL;

    // --- healthBarFill (+0xE0): the actual filled portion ---
    // UV column A: 0.5751953..0.6357422. V range = empty/1024 .. 80/1024
    // (the lower portion of the column where the filled color lives).
    h->healthBarFill.quad.setTexCoords(0.5751953f, emptyUV,
                                       0.6357422f, (filledPx + emptyPx) * UV_SCL);
    h->healthBarFill.quad.setSize(BAR_W, filledPx / SIZE_SCL);
    h->healthBarFill.quad.posX = 0.5f;
    h->healthBarFill.quad.posY = emptyPx / SIZE_SCL + h->healthBarFill.quad.height * 0.5f;

    // --- healthBarGain (+0x1B8): heal segment, col A (same as fill) ---
    // V range carves out (80 - filled - gain) .. (80 - filled), the gap
    // between the current fill and the higher target. height = gain_px.
    // when current >= target this collapses to height 0 (invisible).
    float gainVBase = BAR_PX - filledPx - gainPx;
    h->healthBarGain.quad.setTexCoords(0.5751953f, gainVBase * UV_SCL,
                                       0.6357422f, (gainPx + gainVBase) * UV_SCL);
    h->healthBarGain.quad.setSize(BAR_W, gainPx / SIZE_SCL);
    h->healthBarGain.quad.posX = 0.5f;
    h->healthBarGain.quad.posY = (h->healthBarFill.quad.posY - h->healthBarFill.quad.height * 0.5f)
                                 - h->healthBarGain.quad.height * 0.5f;

    // --- healthBarOverflow (+0x290): damage segment, col B (different color) ---
    // UV column B: 0.5136719..0.5742188. V range carves out (80 - overflow - filled)
    // .. (80 - filled), the slice above the target that's about to be lost.
    // when current <= target this collapses to height 0.
    float overflowVBase = BAR_PX - filledPx - overflowPx;
    h->healthBarOverflow.quad.setTexCoords(0.5136719f, overflowVBase * UV_SCL,
                                           0.5742188f, (overflowPx + overflowVBase) * UV_SCL);
    h->healthBarOverflow.quad.setSize(BAR_W, overflowPx / SIZE_SCL);
    h->healthBarOverflow.quad.posX = 0.5f;
    h->healthBarOverflow.quad.posY = (h->healthBarFill.quad.posY - h->healthBarFill.quad.height * 0.5f)
                                     - h->healthBarOverflow.quad.height * 0.5f;

    // --- healthBarTip (+0x368): 2-pixel cap on top of filled ---
    // height = min(filled, 2). when filled > 2 the tip stays at 2px and just
    // tracks the top of the fill bar. when filled <= 2 the tip degenerates to
    // the same height as the fill (and overlaps it harmlessly).

    float tipPx = (filledPx < 2.0f) ? filledPx : 2.0f;

    h->healthBarTip.quad.setTexCoords(0.5751953f, emptyUV,
                                      0.6357422f, (emptyPx + tipPx) * UV_SCL);
    h->healthBarTip.quad.setSize(BAR_W, tipPx / SIZE_SCL);
    h->healthBarTip.quad.posX = 0.5f;
    // anchored at the center of the overflow bar (= fill bar's top when
    // overflow is 0) less half its own height, so the tip sits flush against
    // the health bar no matter which segment is on top.
    h->healthBarTip.quad.posY = (h->healthBarOverflow.quad.posY - h->healthBarOverflow.quad.height * 0.5f)
                                + h->healthBarTip.quad.height * 0.5f;
}

// FUN_10000bab8 / d300 / d250 are consolidated into setConditionalIcon
// below, parameterised by ConditionalIconState. all three are equivalent
// up to UV/size constants.

// reconstructed from Ghidra FUN_10000b160
void GameplayHUD::init(void* parentPtr) {
    parent = parentPtr;

    // 1 status bar + 4 health bars. layoutStatBars (FUN_10000bb60) sets the
    // health bars' UVs/sizes/positions once health values are known.
    statusBar         = TileIcon();
    healthBarFill     = TileIcon();
    healthBarGain     = TileIcon();
    healthBarOverflow = TileIcon();
    healthBarTip      = TileIcon();

    engagementState = 0;
    publishedState  = 0;

    // 6 button-frame and icon quads
    buttonFrame1     = TileIcon();
    buttonFrame2     = TileIcon();
    largeButtonFrame = TileIcon();
    playerIcon       = TileIcon();
    menuIcon         = TileIcon();
    conditionalIcon  = TileIcon();

    // post-icon scalar block
    conditionalFlag = false;
    std::memset(pad959, 0, sizeof(pad959));
    currentHealth = 1;
    maxHealth = 1;
    previousHealthRatio = 0.0f;
    targetHealthRatio = 1.0f;
    currentHealthRatio = 1.0f;

    // 3 ColorTints (ATK left, DEF right, HP center)
    tintAttack.init();
    tintDefence.init();
    tintHealth.init();

    fieldA18 = 1.0f;
    std::memset(padA1C, 0, sizeof(padA1C));

    // 10 XP markers (right side) + 10 control markers (left side).
    // bare Quad init each; UVs/sizes/positions set in the layout loop below.
    for (int i = 0; i < 10; i++) {
        xpMarkers[i].quad = Quad();
        xpMarkers[i].fadeT     = 0.0f;
        xpMarkers[i].fadeDelay = 0.0f;
    }
    xpCount             = 0;
    xpQueuedDelta       = 0;
    xpReceivedTotal     = 0;
    xpAdvanceBusy       = 0;
    xpDrainPhase        = 0;
    std::memset(pad12EE, 0, sizeof(pad12EE));

    for (int i = 0; i < 10; i++) {
        controlMarkers[i].quad = Quad();
        controlMarkers[i].fadeT     = 0.0f;
        controlMarkers[i].fadeDelay = 0.0f;
    }
    controlCount         = 0;
    controlQueuedDelta   = 0;
    controlReceivedTotal = 0;
    controlAdvanceBusy   = 0;
    controlDrainPhase    = 0;
    std::memset(pad1BBE, 0, sizeof(pad1BBE));
    levelUpReady    = 0;
    itemChoiceReady = 0;
    std::memset(pad1BC2, 0, sizeof(pad1BC2));

    // event tray: zero slotPtr + animation state on every entry;
    // progress = 1.0f marks the slot "empty / animation done"
    // (addEventSlot resets it to 0 on install, starting the slide-in).
    for (int i = 0; i < 4; ++i) {
        eventTray[i] = Entry{};
        eventTray[i].progress = 1.0f;
    }

    selectedItem = -1;
    std::memset(pad1C4C, 0, sizeof(pad1C4C));
    releasedEventSlot = nullptr;
    // removalAnims is default-constructed (libc++ self-points the sentinel
    // and zeroes size on aarch64), matching the binary's explicit
    // anchor.prev = anchor.next = &anchor + size = 0.

    overlayQuad1 = TileIcon();
    overlayQuad2 = TileIcon();

    overlayProgress = 0.0f;
    pulseTimer      = 0.0f;
    overlayState    = 0;
    touchReEntryGuard = 0;  // binary clears both bytes via strh at 0x10000b308
    playSoundNextFill = true;
    std::memset(pad1E2B, 0, sizeof(pad1E2B));

    // --- per-quad UV / size / position setup ---

    // status bar (+0x008): UV (0, 0.7539) -> (0.625, 0.8427), size 1.0 x 0.142.
    // posX = width/2 (= 0.5 here); posY = height/2 (= 0.0710938).
    // half-extent comes from width/height, not scaleX/scaleY.
    statusBar.quad.setTexCoords(0.0f, 0.75390625f, 0.625f, 0.84277344f);
    statusBar.quad.setSize(1.0f, 0.1421875f);
    statusBar.quad.posX = statusBar.quad.width * 0.5f;
    statusBar.quad.posY = statusBar.quad.height * 0.5f;

    // button frame 1 (+0x448): UV (0, 0.194) -> (0.073, 0.260), size 0.117x0.105.
    // posX/posY = width/2, height/2 = (0.0586, 0.0523), top-left corner.
    buttonFrame1.quad.setTexCoords(0.0f, 0.19433594f, 0.07324219f, 0.25976563f);
    buttonFrame1.quad.setSize(0.1171875f, 0.1046875f);
    buttonFrame1.quad.posX = buttonFrame1.quad.width  * 0.5f;
    buttonFrame1.quad.posY = buttonFrame1.quad.height * 0.5f;

    // button frame 2 (+0x520): same UV. posX = width/2 + HUD_BUTTON_X_RIGHT.
    // posY = height/2. final position (0.943, 0.0523), top-right corner.
    buttonFrame2.quad.setTexCoords(0.0f, 0.19433594f, 0.07324219f, 0.25976563f);
    buttonFrame2.quad.setSize(0.1171875f, 0.1046875f);
    buttonFrame2.quad.posX = buttonFrame2.quad.width  * 0.5f + HUD_BUTTON_X_RIGHT;
    buttonFrame2.quad.posY = buttonFrame2.quad.height * 0.5f;

    // large button frame (+0x5F8): UV (0, 0.265) -> (0.128, 0.342), size 0.205x0.123.
    // hardcoded posX/posY = 0.8961, 0.176.
    largeButtonFrame.quad.setTexCoords(0.0f, 0.26464844f, 0.12792969f, 0.34179688f);
    largeButtonFrame.quad.setSize(0.2046875f, 0.1234375f);
    largeButtonFrame.quad.posX = 0.89624023f;
    largeButtonFrame.quad.posY = 0.17578125f;

    // FUN_10000bab8: position conditional icon now that largeButtonFrame is in place
    setConditionalIcon(ConditionalIconState::Default);

    // player icon (+0x6D0): UV (0.068, 0.710) -> (0.117, 0.753), size 0.078x0.069.
    // posX = buttonFrame1.posX + HUD_ICON_NUDGE.
    // posY = buttonFrame1.height [read from buttonFrame1.quad.height] (binary
    //        reads byte offset +0x4F4 which is buttonFrame1.quad.posY).
    playerIcon.quad.setTexCoords(0.068359375f, 0.70996094f, 0.1171875f, 0.7529297f);
    playerIcon.quad.setSize(0.078125f, 0.06875f);
    playerIcon.quad.posX = buttonFrame1.quad.posX + HUD_ICON_NUDGE;
    playerIcon.quad.posY = buttonFrame1.quad.posY;

    // FUN_1000573a8: pixel-snap player icon
    playerIcon.quad.snapToPixelGrid();

    // menu icon (+0x7A8): UV (0.118, 0.712) -> (0.165, 0.753), size 0.075x0.066.
    // posX = buttonFrame2.posX + HUD_ICON_NUDGE; posY = buttonFrame2.posY + HUD_ICON_NUDGE.
    menuIcon.quad.setTexCoords(0.11816406f, 0.71191406f, 0.16503906f, 0.7529297f);
    menuIcon.quad.setSize(0.075f, 0.065625f);
    menuIcon.quad.posX = buttonFrame2.quad.posX + HUD_ICON_NUDGE;
    menuIcon.quad.posY = buttonFrame2.quad.posY + HUD_ICON_NUDGE;

    {
        constexpr float SNAP_REF = 640.0f;
        auto snap = [](float v) {
            float scaled = v * SNAP_REF + (v < 0.0f ? -0.5f : 0.5f);
            return (float)(int)scaled / SNAP_REF;
        };
        float vx = menuIcon.quad.vertices[0].x;
        float vy = menuIcon.quad.vertices[0].y;
        menuIcon.quad.posX = snap(menuIcon.quad.posX + vx) - vx;
        menuIcon.quad.posY = snap(menuIcon.quad.posY + vy) - vy;
    }

    // marker arrays: 10 XP markers (right side) + 10 control markers (left side),
    // each arranged in 2 rows x 5 cols.
    for (int i = 0; i < 10; i++) {
        // control marker UV (0.129, 0.265) -> (0.142, 0.277), size 0.020x0.020.
        // negative col offset, so markers grow leftward.
        controlMarkers[i].quad.setTexCoords(0.12890625f, 0.26464844f,
                                            0.14160156f, 0.27734375f);
        controlMarkers[i].quad.setSize(0.0203125f, 0.0203125f);

        float col = (float)(i % 5);
        float rowYBase = ((float)(i / 5) * 20.0f + HUD_MARKER_Y_BASE) / HUD_MARKER_DENOM;
        controlMarkers[i].quad.posX = (col * -19.0f + HUD_MARKER_X_IND) / HUD_MARKER_DENOM;
        controlMarkers[i].quad.posY = rowYBase;

        // XP marker UV (0.143, 0.265) -> (0.155, 0.277), same size.
        // positive col offset, so markers grow rightward.
        xpMarkers[i].quad.setTexCoords(0.14257813f, 0.26464844f,
                                       0.15527344f, 0.27734375f);
        xpMarkers[i].quad.setSize(0.0203125f, 0.0203125f);

        xpMarkers[i].quad.posX = (col * 19.0f + HUD_MARKER_X_MAIN) / HUD_MARKER_DENOM;
        xpMarkers[i].quad.posY = rowYBase;
    }

    // tintHealth starts with a yellow-ish color (R=ff, G=ff, B=64) at alpha 0x78
    tintHealth.setColor(0xff, 0xff, 0x64);
    tintHealth.setAlpha(0x78);

    // healthBarGain (+0x1B8) gets initial alpha 100 (slightly transparent so it
    // visually blends as a heal-in-progress overlay).
    healthBarGain.quad.setAlpha(100);

    // healthBarTip (+0x368) gets a light-gray tint (RGB 200,200,200 alpha 255).
    // the gray multiplier dims the texture sample to ~78% brightness so the tip
    // blends with the bar's edge instead of standing out as a separate slice.
    // (Ghidra shows the 0xFFC8C8C8 store as -NAN; that bit pattern is also a
    // valid NaN.)
    healthBarTip.quad.setColor(0xC8, 0xC8, 0xC8, 0xFF);

    // FUN_10000bb60: lay out the 4 health bars from current/max health
    layoutStatBars(this);

    // overlay quad 1 (+0x1C70): UV (0.206, 0.270) -> (0.372, 0.417), size 0.266x0.234.
    // posX = 0.5, posY = 0.1875.
    overlayQuad1.quad.setTexCoords(0.20605469f, 0.2705078f, 0.3720703f, 0.4169922f);
    overlayQuad1.quad.setSize(0.265625f, 0.234375f);
    overlayQuad1.quad.posX = 0.5f;
    overlayQuad1.quad.posY = 0.1875f;

    // overlay quad 2 (+0x1D48): same UV/size/pos as overlay 1
    overlayQuad2.quad.setTexCoords(0.20605469f, 0.2705078f, 0.3720703f, 0.4169922f);
    overlayQuad2.quad.setSize(0.265625f, 0.234375f);
    overlayQuad2.quad.posX = 0.5f;
    overlayQuad2.quad.posY = 0.1875f;
}

// reconstructed from Ghidra FUN_10000c004
void GameplayHUD::draw() {
    // -- pass 1: animated removal queue. each entry holds an EventSlot*
    //    that's mid-animation (sliding up and off the bar). binary draws
    //    the slot via EventSlot::draw (FUN_100029cdc). list starts empty;
    //    nodes get push_back'd by removeEventSlot.
    for (const RemovalAnim& anim : removalAnims) {

        if (anim.slot) {
            anim.slot->draw();
        }
    }

    // -- pass 2: 4 event tray slots. each non-null slotPtr draws into the
    //    bar at its anim-tweened tray position.
    for (int i = 0; i < 4; ++i) {

        if (eventTray[i].slotPtr) {
            eventTray[i].slotPtr->draw();
        }
    }

    // -- pass 3: 9 fixed quads on tex 9, in the binary's exact order --
    bindTexture(9);
    statusBar.quad.draw();              // +0x008
    healthBarGain.quad.draw();          // +0x1B8 (drawn behind fill)
    healthBarOverflow.quad.draw();      // +0x290 (drawn behind fill)
    healthBarFill.quad.draw();          // +0x0E0 (covers gain/overflow base)
    healthBarTip.quad.draw();           // +0x368 (2px cap on top)
    buttonFrame1.quad.draw();           // +0x448
    playerIcon.quad.draw();             // +0x6D0
    buttonFrame2.quad.draw();           // +0x520
    menuIcon.quad.draw();               // +0x7A8

    // conditional pair (gated by conditionalFlag at +0x958)
    if (conditionalFlag) {
        largeButtonFrame.quad.draw();   // +0x5F8
        conditionalIcon.quad.draw();    // +0x880
    }

    // -- pass 4: control markers (count from +0x1BB0) --
    for (int i = 0; i < controlCount; i++) {
        controlMarkers[i].quad.draw();
    }

    // -- pass 5: xp markers (count from +0x12E0) --
    for (int i = 0; i < xpCount; i++) {
        xpMarkers[i].quad.draw();
    }

    // -- pass 6: 3 ColorTints, drawn in the binary's specific order:
    //    HP first, then ATK, then DEF.
    tintHealth.draw();
    tintAttack.draw();
    tintDefence.draw();

    // -- pass 7: overlay quads (only when overlayProgress > 0; tex 8) --
    if (overlayProgress > 0.0f) {
        bindTexture(8);
        overlayQuad2.quad.draw();    // +0x1D48 drawn first
        overlayQuad1.quad.draw();    // +0x1C70 drawn second
    }
}

// forward decls; bodies live in the anonymous namespace below.
namespace {
void advanceMarkerBank(int delta, MarkerSlot* slots,
                       int32_t& count, int32_t& queuedDelta,
                       int32_t& receivedTotal, uint8_t& busy);
void tickMarkerBank(float dt, MarkerSlot* slots,
                    int32_t& count, int32_t& queuedDelta,
                    int32_t& receivedTotal, uint8_t& busy,
                    uint8_t& drainPhase, bool playSoundNextFill,
                    uint8_t* drainSignalOut, int chimeSound);
}  // namespace

// reconstructed from Ghidra FUN_10000c18c
void GameplayHUD::update(float dt) {
    // per-frame tick for both marker banks (xp + control). chime sounds
    // 0x1A and 0x1B match FUN_10000c568's dispatch, which selects the chime
    // by comparing the bank base against +0x12f0 (controlMarkers):
    // bank == controlMarkers -> 0x1A, else -> 0x1B.
    tickMarkerBank(dt, xpMarkers,
                   xpCount, xpQueuedDelta, xpReceivedTotal,
                   xpAdvanceBusy, xpDrainPhase,
                   playSoundNextFill,
                   &levelUpReady,   // HUD+0x1BC0, XP bank full -> level up
                   0x1B);
    tickMarkerBank(dt, controlMarkers,
                   controlCount, controlQueuedDelta, controlReceivedTotal,
                   controlAdvanceBusy, controlDrainPhase,
                   playSoundNextFill,
                   &itemChoiceReady,   // HUD+0x1BC1, CTRL bank full -> item choice
                   0x1A);

    // death-heart pulse: the broken beating heart overlay drawn while
    // Nemesis advances on a downed player. gated internally on overlayState
    // / overlayProgress; no-ops when neither is set.
    tickDeathHeart(dt);

    // re-enable the slot-fill chime when both marker banks are idle.
    if (!xpAdvanceBusy && !controlAdvanceBusy) {
        playSoundNextFill = true;
    }

    // -- health ratio current -> target lerp at constant rate --
    if (currentHealthRatio != targetHealthRatio) {
        constexpr float RATE_DOWN = -1.0f;  // DAT_100059d58 (taking damage)
        constexpr float RATE_UP   =  1.0f;  // DAT_100059d5c (healing)

        float original = currentHealthRatio;
        float rate = (original < targetHealthRatio) ? RATE_UP : RATE_DOWN;
        float advanced = original + dt * 0.5f * rate;
        currentHealthRatio = advanced;

        // clamp to target on overshoot. the outer branch keys on the ORIGINAL
        // pre-update ratio (fcmp at 0x10000c22c, reused by b.pl at 0x10000c258),
        // not the just-stored advanced value, so an overshoot in either
        // direction snaps to target.
        if (original < targetHealthRatio) {
            if (advanced >= targetHealthRatio) {
                currentHealthRatio = targetHealthRatio;
            }
        } else if (advanced <= targetHealthRatio) {
            currentHealthRatio = targetHealthRatio;
        }

        layoutStatBars(this);
    }

    // -- tint slide animation when fieldA18 < 1 (level-transition swap) --
    if (fieldA18 < 1.0f) {
        constexpr float PI = 3.1415927f;
        constexpr float Y_BOUNCE   = 0.03125f;     // DAT_100059ccc
        constexpr float TINT_X_R   = 0.6125f;      // DAT_100059cd0 (right resting)
        constexpr float TINT_Y     = 0.046875f;    // DAT_100059cd4 (resting Y)
        constexpr float TINT_X_L   = 0.38593751f;  // DAT_100059cd8 (left resting)

        float t = fieldA18 + dt + dt;  // advance by 2*dt
        if (t > 1.0f) {
            t = 1.0f;
        }
        fieldA18 = t;

        // tintAttack slide: starts on the right, ends on the left (resting ATK position)
        float bounce = (0.5f - std::cos((t + t) * PI) * 0.5f) * Y_BOUNCE;
        float xMix = 0.5f - std::cos(t * PI) * 0.5f;

        float atkX = xMix * TINT_X_L + (1.0f - xMix) * TINT_X_R;
        float atkY = bounce + xMix * TINT_Y + (1.0f - xMix) * TINT_Y;
        // mode 0 while still animating, mode 1 (pixel-snap) on the last frame.
        int mode = (t >= 1.0f) ? 1 : 0;
        tintAttack.setPosition(atkX, atkY, mode);

        // tintDefence slide: starts on the left, ends on the right (resting DEF position)
        float xMix2 = 0.5f - std::cos(fieldA18 * PI) * 0.5f;
        float defX = xMix2 * TINT_X_R + (1.0f - xMix2) * TINT_X_L;
        float defY = (xMix2 * TINT_Y + (1.0f - xMix2) * TINT_Y) - bounce;
        int mode2 = (fieldA18 >= 1.0f) ? 1 : 0;
        tintDefence.setPosition(defX, defY, mode2);
    }

    // -- per-slot tray animation pass (FUN_10000c18c's tray loop) --
    //
    // for each non-null tray entry:
    //   1. EventSlot::tickAnimation(dt) drives hoverState + per-marker
    //      fill animation. always runs (no progress gate).
    //   2. if entry.progress < 1.0, lerp the slot's position from
    //      (entry.currentX/Y) toward (entry.targetX/Y) using a cosine
    //      ease, then write the lerped XY back via EventSlot::setPosition.
    //      gated by progress so we stop touching positions once the
    //      animation settles.
    constexpr float SLOT_LERP_DURATION = 0.3f;          // DAT_100059d20
    constexpr float SLOT_COS_MUL       = 3.14159274f;   // DAT_100059d24 = pi

    for (int i = 0; i < 4; ++i) {
        Entry& entry = eventTray[i];
        EventSlot* slot = entry.slotPtr;

        if (slot == nullptr) {
            continue;
        }

        slot->tickAnimation(dt);

        if (entry.progress < 1.0f) {
            // advance progress (gated by shiftDelay countdown for the
            // staggered compaction case, see removeEventSlot)
            entry.shiftDelay -= dt;

            if (entry.shiftDelay <= 0.0f) {
                entry.progress += dt / SLOT_LERP_DURATION;

                if (entry.progress > 1.0f) {
                    entry.progress = 1.0f;
                }
            }

            // cosine ease + lerp
            const float t = 0.5f - std::cos(entry.progress * SLOT_COS_MUL) * 0.5f;
            const float lerpedX = entry.targetX * t + entry.currentX * (1.0f - t);
            const float lerpedY = entry.targetY * t + entry.currentY * (1.0f - t);

            const float xy[2] = { lerpedX, lerpedY };
            slot->setPosition(xy);
        }
    }

    // -- removalAnims cleanup pass (FUN_10000c18c's second loop) --
    //
    // for each node in removalAnims:
    //   1. lerp the slot's position toward (targetX, targetY) using the
    //      same cosine-eased primitive the tray entries use (binary
    //      FUN_10000c9d0). shiftDelay counts down first; once <= 0,
    //      progress advances by dt / 0.3.
    //   2. when progress hits 1.0, free the EventSlot and erase the
    //      node from the list.
    //
    // unlike the tray loop, every removalAnims node animates every frame:
    // there's no "skip if progress >= 1.0" gate, because a node with
    // progress >= 1.0 is about to be erased on this same iteration.
    for (auto it = removalAnims.begin(); it != removalAnims.end(); ) {
        RemovalAnim& anim = *it;

        // FUN_10000c9d0 inlined: progress advance + cosine-eased lerp.
        anim.shiftDelay -= dt;

        if (anim.shiftDelay <= 0.0f) {
            anim.progress += dt / SLOT_LERP_DURATION;

            if (anim.progress > 1.0f) {
                anim.progress = 1.0f;
            }
        }

        if (anim.slot) {
            const float t = 0.5f - std::cos(anim.progress * SLOT_COS_MUL) * 0.5f;
            const float lerpedX = anim.targetX * t + anim.currentX * (1.0f - t);
            const float lerpedY = anim.targetY * t + anim.currentY * (1.0f - t);
            const float xy[2] = { lerpedX, lerpedY };
            anim.slot->setPosition(xy);
        }

        if (anim.progress >= 1.0f) {
            // animation done: destroy the slot and unlink the node.
            // binary uses operator_delete after explicit Quad / TileIcon
            // dtors; our EventSlot's default destructor handles the
            // members directly.
            delete anim.slot;
            it = removalAnims.erase(it);
        } else {
            ++it;
        }
    }

    // ---- press-and-hold HUD inspect (FUN_10000c18c tail) ----
    // while a touch is held (inputState 1), show a DetailPanel card for the
    // HUD element under the finger: when the touch is low on screen, inspect
    // the bottom event tray; otherwise inspect the top-row stat icons.
    Game* g = getGame();

    if (g->inputState() != 1) {
        return;
    }

    constexpr float TRAY_TOUCH_Y = 75.0f / 640.0f;   // DAT_100059cdc

    if (g->touchY() >= TRAY_TOUCH_Y) {
        DetailPanel* panel = static_cast<DetailPanel*>(parent);

        for (int i = 0; i < 4; i++) {
            EventSlot* slot = eventTray[i].slotPtr;

            if (slot && slot->contains(g->touchX(), g->touchY())) {
                float anchor[2] = { slot->mainQuad.quad.posX,
                                    slot->mainQuad.quad.posY };
                panel->populateForEvent(0.109375f /*DAT_100059ce0*/, anchor,
                                        slot);
            }
        }
    } else {

        for (int i = 0; i < 5; i++) {

            if (tryInspectStatRegion(i)) {
                break;
            }
        }
    }
}

namespace {

// FUN_10000ca90's stat-region table (DAT_100076850, 5 x 0x30). bounds + anchor
// are screen pixels divided by 640 at use; uvType selects the content-tile
// icon populateForStatRow draws. {C}/{A}/{D}/{H}/{X} tokens are substituted
// downstream by TextItem::setString.
struct StatInspectRegion {
    int minXpx;
    int minYpx;
    int maxXpx;
    int maxYpx;
    int anchorXpx;
    uint32_t uvType;
    const char* title;
    const char* desc0;
    const char* desc1;
};

constexpr StatInspectRegion kStatRegions[5] = {
    {  75, 0, 211, 70, 191, 5, "Control",
       "Collect by placing {C} tiles on {C} spots.",
       "Every 10 {C} upgrades your items." },
    { 218, 0, 278, 70, 248, 2, "Current Attack",
       "Damage done to a snag per attack, less its {D}.",
       "A third of your {A} is lost when you deal damage." },
    { 288, 0, 350, 80, 321, 6, "Current Health",
       "When your {H} reaches 0, Nemesis advances",
       "quickly and your {H} is refilled to half." },
    { 362, 0, 422, 70, 392, 3, "Current Defence",
       "Snag damage is reduced by your {D}.",
       "Half of your {D} is lost after each attack." },
    { 429, 0, 565, 70, 449, 4, "Experience",
       "Collect when Nemesis consumes {X} tiles.",
       "Every 10 {X} upgrades your stats and perks." },
};

} // namespace

// reconstructed from Ghidra FUN_10000ca90.
bool GameplayHUD::tryInspectStatRegion(int index) {
    Game* g = getGame();
    const StatInspectRegion& r = kStatRegions[index];

    constexpr float SCALE = 640.0f;   // DAT_100059d28
    const float touchX = g->touchX();
    const float touchY = g->touchY();

    // bounds test in screen-normalized space (px / 640).
    if (touchX < r.minXpx / SCALE || r.maxXpx / SCALE < touchX ||
        touchY < r.minYpx / SCALE || r.maxYpx / SCALE < touchY) {
        return false;
    }

    float anchor[2] = { r.anchorXpx / SCALE, 0.046875f };
    static_cast<DetailPanel*>(parent)->populateForStatRow(
        0.0703125f /*DAT_100059d2c*/, anchor,
        r.uvType, r.title, r.desc0, r.desc1, "");
    return true;
}

// reconstructed from Ghidra FUN_10000d0dc
void GameplayHUD::setAttack(int value) {
    // tintAttack (+0x970): show ATK value as digits, then position at the
    // resting spot in the top-left.
    tintAttack.setNumber(value, 1, 1);  // textStyle 1 (large digits), positionMode 1
    tintAttack.setPosition(0.38593751f, 0.046875f, 1);
}

// reconstructed from Ghidra FUN_10000d134
void GameplayHUD::setDefence(int value) {
    tintDefence.setNumber(value, 1, 1);
    tintDefence.setPosition(0.6125f, 0.046875f, 1);
}

// reconstructed from Ghidra FUN_10000d18c
void GameplayHUD::setHealth(int value, int holdRatio) {
    currentHealth = value;
    tintHealth.setNumber(value, 0, 1);  // textStyle 0 (small digits)
    tintHealth.setPosition(0.5f, 0.071875f, 1);

    previousHealthRatio = currentHealthRatio;

    float vf = (float)currentHealth;
    float df = (float)maxHealth;
    // defensive divide-by-zero guard (the binary divides unconditionally).
    targetHealthRatio = (df != 0.0f) ? (vf / df) : 0.0f;

    // when holdRatio bit 0 is clear, snap currentHealthRatio to target so the
    // bar doesn't animate (used for hard-reset paths like initLevel). when set,
    // the bar lerps over time via update().
    if ((holdRatio & 1) == 0) {
        currentHealthRatio = targetHealthRatio;
    }

    layoutStatBars(this);
}

// reconstructed from Ghidra FUN_10000d228
void GameplayHUD::setMaxHealth(int value) {
    maxHealth = value;
    float vf = (float)currentHealth;
    float df = (float)value;
    // defensive divide-by-zero guard (the binary divides unconditionally).
    float ratio = (df != 0.0f) ? (vf / df) : 0.0f;
    targetHealthRatio  = ratio;
    currentHealthRatio = ratio;
    layoutStatBars(this);
}

// reconstructed from Ghidra FUN_10000d9ac
void GameplayHUD::resetOverlay(int hardClear) {
    // only the overlayState byte (+0x1E28) is cleared; the adjacent +0x1E29
    // byte (touchReEntryGuard) is a separate field, not touched here.
    overlayState = 0;

    if (hardClear != 0) {
        overlayProgress = 0.0f;
    }
}

// reconstructed from Ghidra FUN_10000c794, the death-heart pulse animator.
//
// when the player's HP hits 0 (setHP path), the binary writes overlayState=1
// (FUN_10000d998); this tick is what then drives the visible "broken beating
// heart" overlay while the camera holds and Nemesis advances onto the player.
//
// the two overlay quads share the heart sprite UV but layer differently:
//   - overlayQuad1: the slow swelling heart-image. alpha ramps with the
//     master overlayProgress (0..1, ~half-second wind-up); scale lerps
//     1.0 -> 1.3 on the per-beat curve `eased`.
//   - overlayQuad2: a brighter shockwave. alpha lerps 200 -> 50 across the
//     first 40% of each beat then snaps back; scale lerps 1.0 -> 1.8 over
//     the same window. multiplied by overlayProgress so the burst fades
//     in along with the rest.
//
// the cyclic pulseTimer drives a two-stage curve per ~1s cycle:
//   pulseTimer in [0.0, 0.4]  rest. eased = 0, no scale change.
//   pulseTimer in (0.4, 1.0]  beat. an inner timer v2 = fmod(((p-0.4)/0.6)*2, 1)
//     sweeps 0..1 twice (the visual "lub-dub"), shaped by a quadratic ramp
//     up to v2 = 0.3 and a logarithmic decay over v2 in [0.3, 1.0). sound
//     0x34 (heartbeat thump) fires once per cycle on entry to the beat.
void GameplayHUD::tickDeathHeart(float dt) {

    if (overlayState == 0 && overlayProgress <= 0.0f) {
        return;
    }

    // direction table from DAT_100059d60: fade-in rate when active,
    // fade-out rate when state has been cleared but progress isn't 0 yet.
    constexpr float kRateInactive = -1.0f;
    constexpr float kRateActive   =  1.0f;

    // constants extracted from DAT_100059cf4..d1c via the Mach-O reader.
    constexpr float kQuad1AlphaMax  = 255.0f;  // DAT_100059cf4
    constexpr float kPulsePhaseSplit = 0.4f;   // DAT_100059cf8
    constexpr float kPhase2Offset   = -0.4f;   // DAT_100059cfc (= -kPulsePhaseSplit)
    constexpr float kPhase2Span     =  0.6f;   // DAT_100059d00 (= 1.0 - kPulsePhaseSplit)
    constexpr float kSubStageSplit  =  0.3f;   // DAT_100059d04
    constexpr float kLogOffset      = -0.3f;   // DAT_100059d08 (= -kSubStageSplit)
    constexpr float kLogSpan        =  0.7f;   // DAT_100059d0c (= 1.0 - kSubStageSplit)
    constexpr float kQuad1ScaleMax  =  1.3f;   // DAT_100059d10
    constexpr float kQuad2ScaleMax  =  1.8f;   // DAT_100059d14
    constexpr float kQuad2AlphaMin  = 200.0f;  // DAT_100059d18 (lerp value when burst inactive)
    constexpr float kQuad2AlphaMax  =  50.0f;  // DAT_100059d1c (lerp value at burst peak)

    // 1. advance the master fade ramp. clamps to [0, 1].
    const float rate = (overlayState != 0) ? kRateActive : kRateInactive;
    overlayProgress = std::clamp(overlayProgress + 2.0f * dt * rate,
                                  0.0f, 1.0f);

    // 2. overlayQuad1 alpha. truncate float to int then mask to a byte.
    {
        const float alphaF = overlayProgress * kQuad1AlphaMax;
        const int   alphaI = static_cast<int>(alphaF);
        overlayQuad1.quad.setAlpha(static_cast<uint8_t>(alphaI & 0xFF));
    }

    // 3. advance the cyclic beat phase (period 1.0).
    const float prevPulse = pulseTimer;
    pulseTimer = std::fmod(pulseTimer + dt, 1.0f);

    // 4. select between the rest stage (eased = 0) and the beat stage. the
    // beat stage further splits its inner timer v2 into a quadratic ramp +
    // a log decay; the saved q2 = 0 fallback covers the rest path.
    float eased;        // 0..1 curve driving overlayQuad1 scale
    float scalePhase;   // the binary's `v3`; drives overlayQuad2 + alpha

    if (pulseTimer > kPulsePhaseSplit) {
        // remap pulseTimer in (0.4, 1.0] to v2 cycling 0..1 twice ("lub-dub").
        const float v2 = std::fmod(
            ((pulseTimer + kPhase2Offset) / kPhase2Span) * 2.0f,
            1.0f);

        // fire the heartbeat chime exactly once per cycle, on transition
        // from rest to beat. prev > 0.4 means we were already mid-beat.
        if (prevPulse <= kPulsePhaseSplit) {

            if (Game* g = getGame()) {
                g->soundQueue.trigger(0x34);
            }
        }

        if (v2 < kSubStageSplit) {
            // quadratic ramp-up: 0 -> 1 across v2 in [0, 0.3).
            const float t = v2 / kSubStageSplit;
            eased = t * t;
        }
        else {
            // logarithmic decay: 1 -> 0 across v2 in [0.3, 1). matches binary's
            //   logf((v2 - 0.3)/0.7 + 1.0) / logf(2.0)
            // (= log2 of the same expression).
            const float arg = (v2 + kLogOffset) / kLogSpan + 1.0f;
            eased = 1.0f - std::log(arg) / std::log(2.0f);
        }
        scalePhase = v2;
    }
    else {
        eased      = 0.0f;
        scalePhase = 0.0f;
    }

    // 5. overlayQuad1 scale = lerp(1.0, 1.3, eased).
    {
        const float scale = (1.0f - eased) + eased * kQuad1ScaleMax;
        overlayQuad1.quad.scaleX = scale;
        overlayQuad1.quad.scaleY = scale;
    }

    // 6. scalePhase normalized into the burst window. >= 0.4 clips to 0 so
    // the shockwave only animates during the first 40% of each beat.
    const float s0 = (scalePhase >= kPulsePhaseSplit)
                     ? 0.0f
                     : (scalePhase / kPulsePhaseSplit);
    const float oneMinusS0 = 1.0f - s0;

    // 7. overlayQuad2 scale = lerp(1.0, 1.8, s0).
    {
        const float scale = oneMinusS0 + s0 * kQuad2ScaleMax;
        overlayQuad2.quad.scaleX = scale;
        overlayQuad2.quad.scaleY = scale;
    }

    // 8. overlayQuad2 alpha = clamp_byte(progress * truncate(lerp(200, 50, s0))).
    // the lerp is truncated to int before the progress multiply, so a half-step
    // in s0 doesn't smooth the byte output. preserve that here.
    {
        const float lerpF       = oneMinusS0 * kQuad2AlphaMin + s0 * kQuad2AlphaMax;
        const int   lerpI       = static_cast<int>(lerpF);
        const float finalF      = overlayProgress * static_cast<float>(lerpI);
        const int   finalI      = static_cast<int>(finalF);
        overlayQuad2.quad.setAlpha(static_cast<uint8_t>(finalI & 0xFF));
    }
}

// reconstructed from Ghidra FUN_10000d778
namespace {

// reconstructed from Ghidra FUN_10000cf34. queues the delta when the
// bank is mid-animation; otherwise advances the lit count (clamped to
// 10), seeds the per-slot fade-in timers, and flips the bank busy
// flag. caller passes the bank's MarkerSlot[10] base + the 4 companion
// fields (count / queued / total / busy).
//
// the per-frame consumer of fadeT / fadeDelay is FUN_10000c568, ported
// below as tickMarkerBank (called each frame for both banks): it drives
// each slot's scale from fadeT, fires the fill chime when a slot first
// crosses fadeT == 0, and clears busy when every slot has finished. this
// function is the seeding half (count update + queue buffering).
void advanceMarkerBank(int delta,
                       MarkerSlot* slots,
                       int32_t&    count,
                       int32_t&    queuedDelta,
                       int32_t&    receivedTotal,
                       uint8_t&    busy) {
    constexpr float MARKER_RAMP_CEILING = 0.2f;   // DAT_100059d34

    // mirrors FUN_10000cf34's leading `if (param_2 != 0)` gate. drainXPSlot
    // can clamp delta to 0 (when xpReceivedTotal is a clean multiple of 10),
    // and the binary short-circuits before touching busy / receivedTotal.
    if (delta == 0) {
        return;
    }

    receivedTotal += delta;

    if (busy) {
        queuedDelta += delta;
        return;
    }

    busy = 1;

    int prevCount = count;
    int newCount  = prevCount + delta;

    if (newCount > 10) {
        queuedDelta = newCount - 10;
        newCount    = 10;
    }
    count = newCount;

    if (prevCount >= 10 || prevCount >= newCount) {
        return;
    }

    int slotsThisBatch = newCount - prevCount;

    for (int i = 0; i < slotsThisBatch; i++) {
        MarkerSlot& slot = slots[prevCount + i];

        slot.quad.scaleX = 0.0f;
        slot.quad.scaleY = 0.0f;
        slot.fadeT       = 0.0f;

        float ramp;

        if (slotsThisBatch == 1) {
            ramp = 0.0f;
        } else {
            float u = (float)i / (float)(slotsThisBatch - 1);
            ramp = u * MARKER_RAMP_CEILING;
        }
        slot.fadeDelay = ramp;
    }
}

// reconstructed from Ghidra FUN_10000c568. per-frame tick for one
// marker bank: drives each lit slot through its fade-in (or its
// fade-out, when drainPhase is set), fires the chime when a fresh
// slot crosses fadeT == 0, and on bank completion either drains
// queued deltas or transitions into drainPhase when the bank
// overflowed (count == 10 + queued > 0).
//
// `drainSignalOut` is a single-byte output stored at HUD+0x1BC0 / +0x1BC1
// (xp / control respectively); it gets set to 1 when the bank just entered
// drainPhase. the xp byte is levelUpReady, the control byte is
// itemChoiceReady; gameBoardUpdate consumes both to open the LevelUp /
// ItemChoice panels, then clears them.
void tickMarkerBank(float       dt,
                    MarkerSlot* slots,
                    int32_t&    count,
                    int32_t&    queuedDelta,
                    int32_t&    receivedTotal,
                    uint8_t&    busy,
                    uint8_t&    drainPhase,
                    bool        playSoundNextFill,
                    uint8_t*    drainSignalOut,
                    int         chimeSound) {
    constexpr float FADE_RATE_DENOM = 0.3f;       // DAT_100059ce4
    constexpr float COS_PI          = 3.1415927f; // DAT_100059ce8 (= pi)
    constexpr float COS_OVERSHOOT_K = 1.3f;       // DAT_100059cec
    constexpr float COS_NORMALIZER  = 1.5877855f; // DAT_100059cf0
                                                  //   = 1 - cos(pi * 1.3),
                                                  //     normalizes the ease

    if (!busy) {
        return;
    }

    int liveCount = count;
    bool anyAnimating = false;

    if (liveCount < 1) {
        busy = 0;
    } else {
        float fadeStep = dt / FADE_RATE_DENOM;

        for (int i = 0; i < liveCount; i++) {
            MarkerSlot& slot = slots[i];

            if (slot.fadeDelay > 0.0f) {
                slot.fadeDelay -= dt;
            }

            if (slot.fadeDelay <= 0.0f) {
                bool freshFrame = (playSoundNextFill && !drainPhase &&
                                   dt > 0.0f && slot.fadeT == 0.0f);

                if (freshFrame) {
                    Game* g = getGame();

                    if (g) {
                        g->soundQueue.trigger(chimeSound);
                    }
                }

                float t = slot.fadeT + fadeStep;

                if (t >= 1.0f) {
                    t = 1.0f;
                } else {
                    anyAnimating = true;
                }
                slot.fadeT = t;

                float effT = drainPhase ? (1.0f - t) : t;
                float c    = std::cos(effT * COS_PI * COS_OVERSHOOT_K);
                float ease = (1.0f - c) / COS_NORMALIZER;

                slot.quad.scaleX = ease;
                slot.quad.scaleY = ease;
            } else {
                anyAnimating = true;
            }
        }

        busy = anyAnimating ? 1 : 0;

        if (anyAnimating) {
            return;
        }
    }

    // bank just finished animating (or started already-empty). decide
    // whether to enter drainPhase (overflow case) or drain the queue.
    if (!drainPhase) {

        if (liveCount >= 10) {

            if (drainSignalOut) {
                *drainSignalOut = 1;
            }
            busy       = 1;
            drainPhase = 1;

            int receivedAfter = receivedTotal - 10;

            if (receivedAfter < 1) {
                receivedAfter = 0;
            }
            receivedTotal = receivedAfter;

            for (int i = 0; i < 10; i++) {
                slots[i].fadeT     = 0.0f;
                slots[i].fadeDelay = 0.0f;
            }
            return;
        }
    } else {
        count      = 0;
        drainPhase = 0;
    }

    int q = queuedDelta;
    queuedDelta = 0;

    int receivedAfter = receivedTotal - q;

    if (receivedAfter < 1) {
        receivedAfter = 0;
    }
    receivedTotal = receivedAfter;

    advanceMarkerBank(q, slots, count, queuedDelta, receivedTotal, busy);
}

}  // namespace

// reconstructed from Ghidra FUN_10000d010
void GameplayHUD::advanceXPSlot(int delta, bool silentFill) {

    if (delta <= 0) {
        return;
    }

    if (silentFill) {
        playSoundNextFill = false;
    }

    advanceMarkerBank(delta, xpMarkers,
                      xpCount, xpQueuedDelta, xpReceivedTotal, xpAdvanceBusy);
}

// reconstructed from Ghidra FUN_10000d034. same body as advanceXPSlot
// but operates on the control marker bank.
void GameplayHUD::advanceCTRLSlot(int delta, bool silentFill) {

    if (delta <= 0) {
        return;
    }

    if (silentFill) {
        playSoundNextFill = false;
    }

    advanceMarkerBank(delta, controlMarkers,
                      controlCount, controlQueuedDelta, controlReceivedTotal,
                      controlAdvanceBusy);
}

// reconstructed from Ghidra FUN_10000da70. rebuild HUD state from a saved
// run. caller (GameBoard::restoreFromSnapshot) passes the restored player
// stats + the snapshot's XP / CONTROL running totals + the event-tray list.
void GameplayHUD::restoreFromSnapshot(int playerAttack, int playerDefence,
                                      int playerMaxHealth, int playerCurrentHealth,
                                      int xpTotal, int controlTotal,
                                      const std::vector<int64_t>& eventTraySnapshot) {
    // ---- touch / engagement + marker-bank-full state reset ----
    engagementState   = 0;
    publishedState    = 0;
    fieldA18          = 1.0f;         // set by the binary; no reader in ported scope
    levelUpReady      = 0;            // +0x1BC0
    itemChoiceReady   = 0;            // +0x1BC1
    selectedItem      = -1;
    releasedEventSlot = nullptr;
    touchReEntryGuard = 0;

    // ---- ATK / DEF stat tints (setAttack/setDefence are the inline tint
    //      setNumber + setPosition that FUN_10000da70 performs) ----
    setAttack(playerAttack);
    setDefence(playerDefence);

    // ---- health bar ----
    // the ratio is computed from the HUD's current (pre-restore) currentHealth
    // over the restored maxHealth, a transient seed for layoutStatBars that the
    // setHealth() call below corrects.
    maxHealth          = playerMaxHealth;
    const float ratio  = (float)currentHealth / (float)maxHealth;
    targetHealthRatio  = ratio;
    currentHealthRatio = ratio;
    layoutStatBars(this);                            // FUN_10000bb60
    setHealth(playerCurrentHealth, 0);               // FUN_10000d18c

    // ---- death-heart overlay reset ----
    overlayState    = 0;
    overlayProgress = 0.0f;

    // ---- clear any existing event tray (compact pulls the next slot into
    //      [0] each pass until the tray is empty) ----
    while (eventTray[0].slotPtr != nullptr) {
        removeEventSlot(eventTray[0].slotPtr, true);
    }

    // ---- zero both marker banks (count / queued / total / busy). the
    //      binary's overlapping 8-byte writes cover +0x1BB0..+0x1BBC and
    //      +0x12E0..+0x12EC, so controlDrainPhase / xpDrainPhase are
    //      deliberately left untouched. ----
    controlCount         = 0;
    controlQueuedDelta   = 0;
    controlReceivedTotal = 0;
    controlAdvanceBusy   = 0;
    xpCount         = 0;
    xpQueuedDelta   = 0;
    xpReceivedTotal = 0;
    xpAdvanceBusy   = 0;

    // ---- re-seed the XP / CONTROL banks from the saved totals.
    //      advanceXPSlot/advanceCTRLSlot(total, silentFill=true) == the
    //      binary's `if (total > 0) { playSoundNextFill = 0;
    //      advanceMarkerBank(...) }`. ----
    advanceXPSlot(xpTotal, /*silentFill=*/true);
    advanceCTRLSlot(controlTotal, /*silentFill=*/true);

    // ---- rebuild the event tray. each saved entry is packed
    //      (eventType | currentCharges << 32); EventSlot::init clamps the
    //      charge count to the kind's chargesMax. ----
    for (int64_t packed : eventTraySnapshot) {
        const int eventType = (int)(packed & 0xFFFFFFFF);
        const int charges   = (int)(packed >> 32);
        EventSlot* slot = new EventSlot();
        slot->init(eventType, charges);
        addEventSlot(slot);
    }
}

// reconstructed from Ghidra FUN_10000d05c. drains the in-progress xp cycle's
// already-earned slots, then routes through the same advanceMarkerBank
// primitive the binary uses (FUN_10000cf34) with a NEGATIVE delta. clamp:
// only the slots earned within the current cycle (recv % 10) are eligible
// to drain, so a clean cycle (recv % 10 == 0) is a no-op.
void GameplayHUD::drainXPSlot(int delta, bool silentDrain) {

    if (delta <= 0) {
        return;
    }

    if (silentDrain) {
        playSoundNextFill = false;
    }

    int recvMod = xpReceivedTotal % 10;

    if (recvMod <= delta) {
        delta = recvMod;
    }

    advanceMarkerBank(-delta, xpMarkers,
                      xpCount, xpQueuedDelta, xpReceivedTotal, xpAdvanceBusy);
}

// reconstructed from Ghidra FUN_10000d3a0. visual ATK/DEF swap. pushes the
// new values into each tint at its resting target position, then swaps
// their CURRENT positions so the slide animation kicked by fieldA18 = 0
// runs back from the opposite side. textStyle 1 = panel font (HUD numbers),
// position mode 1 = pixel-snap relative to anchor.
void GameplayHUD::swapAtkDefDisplays(int newAtkValue, int newDefValue) {
    constexpr float TINT_X_LEFT  = 0.3859375f;   // DAT_100059cd8 (resting ATK X)
    constexpr float TINT_Y       = 0.046875f;    // DAT_100059cd4 (resting Y)
    constexpr float TINT_X_RIGHT = 0.6125f;      // DAT_100059cd0 (resting DEF X)

    // step 1: set each tint at its resting position. the binary crosses the
    // value bindings: tintAttack shows the DEF value (param_3) and tintDefence
    // shows the ATK value (param_2), so the post-swap slide reveals each stat
    // arriving from the other side.
    tintAttack.setNumber(newDefValue, 1, 1);
    tintAttack.setPosition(TINT_X_LEFT, TINT_Y, 1);

    tintDefence.setNumber(newAtkValue, 1, 1);
    tintDefence.setPosition(TINT_X_RIGHT, TINT_Y, 1);

    // step 2: swap the two tints' current pos so the slide starts on the
    // opposite side. binary saves tintAttack.pos into a stack pair, copies
    // tintDefence.pos onto tintAttack, then restores the saved pair onto
    // tintDefence. mode 1 means each setPosition re-applies pixel-snap delta.
    float savedAtkX = tintAttack.posX;
    float savedAtkY = tintAttack.posY;
    tintAttack.setPosition(tintDefence.posX, tintDefence.posY, 1);
    tintDefence.setPosition(savedAtkX, savedAtkY, 1);

    // step 3: kick the slide animation timer (consumed in update()).
    fieldA18 = 0.0f;
}

// FUN_10000d778, drop every event tray slot via repeated remove-slot-0
// calls with compact=true. each call queues a slide-off animation into
// removalAnims and shifts subsequent slots leftward into the gap, so
// the whole tray cascades off in one motion. Maelstrom (Event Kind 10)
// is the visible caller.
void GameplayHUD::clearEventSlots() {

    while (eventTray[0].slotPtr != nullptr) {
        removeEventSlot(eventTray[0].slotPtr, /*compact=*/true);
    }
}

// merges three near-identical binary functions (FUN_10000bab8 /
// FUN_10000d300 / FUN_10000d250); they only differ in UV/size, and all
// share the same Y nudge + snap.
void GameplayHUD::setConditionalIcon(ConditionalIconState state) {
    float u0, v0, u1, v1, w, h;

    switch (state) {
        case ConditionalIconState::Default:
            u0 = 0.16601563f; v0 = 0.69140625f;
            u1 = 0.22070313f; v1 = 0.75292969f;
            w  = 0.0875f;     h  = 0.09843750f;
            break;
        case ConditionalIconState::StagingActive:
            u0 = 0.22167969f; v0 = 0.69335938f;
            u1 = 0.28125000f; v1 = 0.75292969f;
            w  = 0.0953125f;  h  = 0.0953125f;
            break;
        case ConditionalIconState::ConfirmDiscard:
            u0 = 0.28222656f; v0 = 0.69531250f;
            u1 = 0.34863281f; v1 = 0.75292969f;   // u1 binary 0x3eb28000
            w  = 0.10625f;    h  = 0.09218750f;
            break;
    }

    conditionalIcon.quad.setTexCoords(u0, v0, u1, v1);
    conditionalIcon.quad.setSize(w, h);

    // anchor to largeButtonFrame's pos (largeButtonFrame is at HUD+0x05F8;
    // its Quad's posX/posY are at +0x6A0/+0x6A4 = the binary's read sites).
    conditionalIcon.quad.posX = largeButtonFrame.quad.posX;
    conditionalIcon.quad.posY = largeButtonFrame.quad.posY + HUD_COND_Y_NUDGE;

    conditionalIcon.quad.snapToPixelGrid();
}

// =================================================================
// HUD::queryReleaseTouch (FUN_10000cb84)
// =================================================================
//
// the EventSlot helpers that used to live here (contains, dimColor,
// resetColor, getEventTypeKey, addCharge) are now proper member
// functions on EventSlot; see event_slot.cpp.

namespace {

// Quad::setColor packed-bytes shim, FUN_10000826c. takes a 4-byte rgba
// in memory order R,G,B,A and writes it to all 4 of the quad's vertices.
// callers always pass one of two stack-local constants (0xFFB4B4B4 = grey,
// 0xFFFFFFFF = white) treated as raw bytes.
void quadSetColorBytes(Quad* q, uint32_t packedRGBA) {
    uint8_t r = (uint8_t)(packedRGBA >>  0);
    uint8_t g = (uint8_t)(packedRGBA >>  8);
    uint8_t b = (uint8_t)(packedRGBA >> 16);
    uint8_t a = (uint8_t)(packedRGBA >> 24);
    q->setColor(r, g, b, a);
}

}  // namespace

// reconstructed from Ghidra FUN_10000d7ac.
int GameplayHUD::countEventsHeld() const {
    int active = 0;

    for (int i = 0; i < 4; ++i) {

        if (eventTray[i].slotPtr != nullptr) {
            active++;
        }
    }

    return active;
}

// reconstructed from Ghidra FUN_10000dc84.
bool GameplayHUD::anyEventSlotSlidingIn() const {

    for (int i = 0; i < 4; ++i) {

        if (eventTray[i].slotPtr != nullptr && eventTray[i].progress < 1.0f) {
            return true;
        }
    }

    return false;
}

// reconstructed from Ghidra FUN_10000dcc8. paired-float return: s0 = first
// non-empty slot's mainQuad.posX (+0xb8), s1 = posY (+0xbc). Ghidra drops s1;
// recovered from disassembly (fmov s0,[+0xb8]; ldr s1,[+0xbc]).
void GameplayHUD::firstEventSlotPos(float& outX, float& outY) const {

    for (int i = 0; i < 4; ++i) {
        EventSlot* slot = eventTray[i].slotPtr;

        if (slot != nullptr) {
            outX = slot->mainQuad.quad.posX;
            outY = slot->mainQuad.quad.posY;
            return;
        }
    }

    outX = 0.0f;
    outY = 0.0f;
}

// reconstructed from Ghidra FUN_10000d7dc.
void GameplayHUD::pushEventCharge(int eventTypeKey) {

    for (int i = 0; i < 4; ++i) {
        EventSlot* slot = eventTray[i].slotPtr;

        if (!slot) {
            continue;
        }

        if ((int)slot->getEventTypeKey() != eventTypeKey) {
            continue;
        }

        if (slot->currentCharges >= slot->chargesMax) {
            continue;
        }

        slot->addCharge();
        return;
    }
}

// reconstructed from Ghidra FUN_10000d850.
bool GameplayHUD::canPushEventCharge(int eventTypeKey) {

    for (int i = 0; i < 4; ++i) {
        EventSlot* slot = eventTray[i].slotPtr;

        if (!slot) {
            continue;
        }

        if ((int)slot->getEventTypeKey() != eventTypeKey) {
            continue;
        }

        if (slot->currentCharges < slot->chargesMax) {
            return true;
        }
    }
    return false;
}

// FUN_10000d474, GameplayHUD::addEventSlot.
//
// finds the first empty eventTray slot and installs `slot` there with
// initial animation state. tray X positions are evenly spaced across the
// 640px reference screen: (i * 127 + 65.5) / 640 for i in [0..3]. each
// slot starts at its tray X (currentX = targetX) but at a fixed Y
// (currentY = 0.0375); the events-bar update path tweens currentY into
// the tray's resting Y from there.
//
// if all 4 tray slots are full, the binary tears down `slot`'s Quads and
// frees it via operator_delete. our port maps that to `delete slot`; the
// Quad members have trivial dtors so the binary's per-Quad teardown loop
// is implicit in C++ destruction.
void GameplayHUD::addEventSlot(EventSlot* slot) {
    constexpr float TRAY_X_BASE    = 65.5f;          // DAT_100059d40
    constexpr float TRAY_X_DIVISOR = 640.0f;         // DAT_100059d44
    constexpr float INSTALL_Y      = 0.0375f;        // = 24/640
    constexpr float RESTING_Y      = 124.0f / 640.0f;   // final tray Y (124/640, binary 0x3e466666)
    constexpr float TRAY_X_STRIDE  = 127.0f;

    for (int i = 0; i < 4; ++i) {

        if (eventTray[i].slotPtr == nullptr) {
            const float trayX = ((float)i * TRAY_X_STRIDE + TRAY_X_BASE)
                              / TRAY_X_DIVISOR;
            eventTray[i].targetX  = trayX;
            eventTray[i].targetY  = RESTING_Y;
            eventTray[i].currentX = trayX;
            eventTray[i].currentY = INSTALL_Y;
            eventTray[i].slotPtr  = slot;

            // call setPosition with the animation-start (currentXY) point.
            // each frame, GameplayHUD::update calls the per-entry position
            // lerp (FUN_10000c9d0 in the binary) which cos-eases from
            // (currentX, currentY) toward (targetX, targetY) and writes
            // the lerped XY back to the slot's quads via setPosition.
            const float startXY[2] = { eventTray[i].currentX, eventTray[i].currentY };
            slot->setPosition(startXY);
            slot->setAlpha(0xFF);
            // binary's str xzr at install clears both progress (+0x18) and
            // shiftDelay (+0x1C); a stale shiftDelay left by a prior compaction
            // would otherwise delay this reused slot's slide-in.
            eventTray[i].progress   = 0.0f;
            eventTray[i].shiftDelay = 0.0f;
            return;
        }
    }

    // tray full: free the passed-in slot. binary unwinds via 16
    // operator_delete calls (one per owned Quad) then deletes the slot;
    // our `delete slot` does both via the trivial Quad dtors.
    if (slot != nullptr) {
        delete slot;
    }
}

// FUN_10000d610, GameplayHUD::removeEventSlot.
//
// two-phase: (a) find the tray slot holding `slot`, queue an animated
// removal entry, and clear the tray slot; (b) when `compact` is true,
// shift every later tray slot leftward into the gap, animating each one
// from its old tray X to its new tray X.
//
// the animated entry slides the EventSlot from its current bar position
// up by 0.15625 (= 100/640 = "one button-frame height above the bar")
// before the consumer (bar update path) destroys it.
//
// `compact == false` is the HUD-reset path: queue the anim but skip the
// leftward shift (other slots keep their positions even though there's
// a gap, they're all being removed in turn anyway).
void GameplayHUD::removeEventSlot(EventSlot* slot, bool compact) {
    constexpr float TRAY_X_BASE      = 65.5f;          // DAT_100059d48 (same as add)
    constexpr float TRAY_X_DIVISOR   = 640.0f;         // DAT_100059d4c
    constexpr float SHIFT_STAGGER    = 0.1f;           // DAT_100059d50 (per-shifted-slot delay)
    constexpr float ANIM_RISE_Y      = -0.15625f;      // = -100/640 (rise above bar)
    constexpr float RESTING_Y        = 124.0f / 640.0f;   // final tray Y (124/640, binary 0x3e466666)
    constexpr float SHIFT_X_STRIDE   = 127.0f;         // per-slot X spacing for shift target

    // binary head: drop the just-released back-pointer if it still
    // referenced the slot we're about to remove. prevents the touch
    // dispatcher from re-firing the freed event on the next frame.
    if (releasedEventSlot == slot) {
        releasedEventSlot = nullptr;
    }

    int foundIdx     = -1;
    int compactSteps = 0;   // number of shifted slots so far (drives the
                            // stagger delay on each subsequent shift)

    for (int i = 0; i < 4; ++i) {
        EventSlot* curSlot = eventTray[i].slotPtr;

        if (curSlot == nullptr) {
            continue;
        }

        if (foundIdx < 0) {

            if (curSlot != slot) {
                continue;
            }

            // (a) queue the removal animation.
            const float slotPosX = slot->mainQuad.quad.posX;
            const float slotPosY = slot->mainQuad.quad.posY;

            RemovalAnim anim{};
            anim.slot     = slot;
            anim.currentX = slotPosX;
            anim.currentY = slotPosY;
            anim.targetX  = slotPosX;
            anim.targetY  = slotPosY + ANIM_RISE_Y;
            anim.progress   = 0.0f;
            anim.shiftDelay = 0.0f;
            removalAnims.push_back(anim);

            eventTray[i].slotPtr = nullptr;
            foundIdx = i;

            if (!compact) {
                return;
            }

        } else {
            // (b) compact: shift this slot left into the gap. animate
            // from current position to new tray X.
            const int   destIdx  = i - 1;
            const float newTrayX = ((float)destIdx * SHIFT_X_STRIDE + TRAY_X_BASE)
                                 / TRAY_X_DIVISOR;
            eventTray[destIdx].slotPtr    = curSlot;
            eventTray[i].slotPtr          = nullptr;
            eventTray[destIdx].currentX   = curSlot->mainQuad.quad.posX;
            eventTray[destIdx].currentY   = curSlot->mainQuad.quad.posY;
            eventTray[destIdx].targetX    = newTrayX;
            eventTray[destIdx].targetY    = RESTING_Y;
            eventTray[destIdx].progress   = 0.0f;
            eventTray[destIdx].shiftDelay = (float)compactSteps * SHIFT_STAGGER;
            compactSteps += 1;
        }
    }
}

// reconstructed from Ghidra FUN_10000d0c8. zero the control-marker bank:
// controlCount, controlQueuedDelta, controlReceivedTotal, controlAdvanceBusy.
void GameplayHUD::clearCTRLBank() {
    controlCount         = 0;
    controlQueuedDelta   = 0;
    controlReceivedTotal = 0;
    controlAdvanceBusy   = 0;
}

// reconstructed from Ghidra FUN_10000d0b4. zero the xp-marker bank.
// parallel to clearCTRLBank, same field shape at +0x12E0..+0x12EC.
void GameplayHUD::clearXPBank() {
    xpCount         = 0;
    xpQueuedDelta   = 0;
    xpReceivedTotal = 0;
    xpAdvanceBusy   = 0;
}

// FUN_10000cb84, touch-release / engagement state machine.
//
// a single function called every frame from the input dispatch chain.
// behavior depends on getGame()->inputState() (Game+0x4): 1 = first frame
// after touch-down, 2 = ongoing touch held, anything else = released.
// inputState 1-or-2 takes the "engagement" branch (capture which header
// the touch landed on); any other state takes the "release" branch
// (confirm or cancel).
//
// hit-test priority order: buttonFrame1 -> buttonFrame2 -> optional
// largeButtonFrame (gated by conditionalFlag) -> 4 EventSlots.
int GameplayHUD::queryReleaseTouch() {
    publishedState   = 0;
    releasedEventSlot = nullptr;

    Game* g = getGame();

    if (!g) {
        return 0;
    }

    // binary checks Game+0x4 (= inputState), not Game+0x0 (gameState).
    // inputState 1 = "first frame of a new touch"; inputState 2 = "ongoing
    // touch held"; both go to the engagement branch. any other value
    // (released, etc.) falls through to the release branch below.
    int is = g->inputState();

    if (is == 1 || is == 2) {
        // re-entry guard: only run the engagement body once per gesture.
        // first frame: touchReEntryGuard is 0, so we set to 1 and continue.
        // subsequent frames: it's already 1, so return 0 (don't re-engage).
        uint8_t prevGuard = touchReEntryGuard;
        touchReEntryGuard = 1;

        if (prevGuard != 0) {
            return 0;
        }

        // header-zone gate: header hit-tests only fire when touchY is in
        // the top of the screen. binary reads Game+0xC (= touchY in our
        // typed Game) and compares to DAT_100059d30 = 0x3df00000 (= 75/640).
        // taps below that virtual-Y fall through to the largeButtonFrame +
        // EventSlot tests.
        constexpr float HEADER_ZONE_BOTTOM = 75.0f / 640.0f;  // DAT_100059d30 = 0x3df00000
        float touchYNorm = g->touchY();
        const float* touchXY = &g->touchX();

        if (touchYNorm < HEADER_ZONE_BOTTOM) {
            // header hit-tests (priority order):
            //
            // 1. buttonFrame1 (+0x448) paired with playerIcon (+0x6D0).
            if (buttonFrame1.quad.contains(touchXY[0], touchXY[1])) {
                engagementState = 1;
                quadSetColorBytes(&buttonFrame1.quad, 0xFFB4B4B4u);
                quadSetColorBytes(&playerIcon.quad,   0xFFB4B4B4u);
                g->soundQueue.trigger(5);
                SDL_Log("DEBUG queryReleaseTouch: engagement state=1 "
                        "(buttonFrame1 @ +0x448, paired playerIcon)");
                return 1;
            }

            // 2. buttonFrame2 (+0x520) paired with menuIcon (+0x7A8).
            if (buttonFrame2.quad.contains(touchXY[0], touchXY[1])) {
                engagementState = 2;
                quadSetColorBytes(&buttonFrame2.quad, 0xFFB4B4B4u);
                quadSetColorBytes(&menuIcon.quad,     0xFFB4B4B4u);
                g->soundQueue.trigger(5);
                SDL_Log("DEBUG queryReleaseTouch: engagement state=2 "
                        "(buttonFrame2 @ +0x520, paired menuIcon)");
                return 1;
            }

            // matches binary: when the touch is in the header zone but on
            // neither header, return 1 (consume) without engaging.
            return 1;
        }

        // 3. largeButtonFrame (+0x5F8), gated by conditionalFlag.
        if (conditionalFlag) {
            if (largeButtonFrame.quad.contains(touchXY[0], touchXY[1])) {
                engagementState = 3;
                quadSetColorBytes(&largeButtonFrame.quad, 0xFFB4B4B4u);
                quadSetColorBytes(&conditionalIcon.quad,  0xFFB4B4B4u);
                g->soundQueue.trigger(5);
                SDL_Log("DEBUG queryReleaseTouch: engagement state=3 "
                        "(largeButtonFrame @ +0x5F8, paired conditionalIcon)");
                return 1;
            }
        }

        // 4. EventSlots: 4 tray entries. iterate, hit-test via
        // EventSlot::contains, dispatch dim or "locked" sound.
        for (int i = 0; i < 4; ++i) {
            EventSlot* slot = eventTray[i].slotPtr;

            if (!slot) {
                continue;
            }

            if (!slot->contains(touchXY[0], touchXY[1])) {
                continue;
            }

            // locked check: currentCharges < chargesMax -> play "locked"
            // sound 0x12, do not engage.
            if (slot->currentCharges < slot->chargesMax) {
                g->soundQueue.trigger(0x12);
                return 1;
            }

            // engage event slot i.
            selectedItem = i;
            slot->dimColor();
            g->soundQueue.trigger(5);
            return 1;
        }

        return 0;
    }

    // ----- RELEASE branch (gameState != 1 && != 2) -----
    touchReEntryGuard = 0;

    const float* touchXY = &g->touchX();

    switch (engagementState) {
        case 0: {
            // event-slot release path.
            if (selectedItem < 0) {
                return 0;
            }

            EventSlot* slot = eventTray[selectedItem].slotPtr;

            if (slot->contains(touchXY[0], touchXY[1])) {
                // hit: capture the slot pointer for the consumer.
                releasedEventSlot = slot;
                g->soundQueue.trigger(6);
            } else {
                g->soundQueue.trigger(7);
            }

            slot->resetColor();
            selectedItem = -1;
            return 1;
        }

        case 1: {
            bool hit = buttonFrame1.quad.contains(touchXY[0], touchXY[1]);
            quadSetColorBytes(&buttonFrame1.quad, 0xFFFFFFFFu);
            quadSetColorBytes(&playerIcon.quad,   0xFFFFFFFFu);

            if (hit) {
                publishedState = engagementState;
                g->soundQueue.trigger(6);
            } else {
                g->soundQueue.trigger(7);
            }

            engagementState = 0;
            return 1;
        }

        case 2: {
            bool hit = buttonFrame2.quad.contains(touchXY[0], touchXY[1]);
            quadSetColorBytes(&buttonFrame2.quad, 0xFFFFFFFFu);
            quadSetColorBytes(&menuIcon.quad,     0xFFFFFFFFu);

            if (hit) {
                publishedState = engagementState;
                g->soundQueue.trigger(6);
            } else {
                g->soundQueue.trigger(7);
            }

            engagementState = 0;
            return 1;
        }

        case 3: {
            bool hit = largeButtonFrame.quad.contains(touchXY[0], touchXY[1]);
            quadSetColorBytes(&largeButtonFrame.quad, 0xFFFFFFFFu);
            quadSetColorBytes(&conditionalIcon.quad,  0xFFFFFFFFu);

            if (hit) {
                publishedState = engagementState;
                g->soundQueue.trigger(6);
            } else {
                g->soundQueue.trigger(7);
            }

            engagementState = 0;
            return 1;
        }

        default:
            // unknown state: match binary's switch-default, sound 7 + reset.
            g->soundQueue.trigger(7);
            engagementState = 0;
            return 1;
    }
}
