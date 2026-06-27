#include "nemesis_renderable.h"
#include "game.h"
#include "renderer.h"   // bindTexture
#include "random.h"
#include <GLES/gl.h>
#include <cmath>
#include <cstring>
#include <new>

using namespace NemesisRenderableConstants;

// reconstructed from Ghidra FUN_1000088f0
void NemesisRenderable::init() {
    // visible flag + its trailing pad
    visible = false;
    std::memset(pad001, 0, sizeof(pad001));

    // 25 hex segments: bare Quad init, no UV/size yet (placeOnHexGrid sets those)
    for (int i = 0; i < 25; i++) {
        segments[i].quad = Quad();
        std::memset(segments[i].scratch, 0, sizeof(segments[i].scratch));
    }

    // center quad and bg circle are TileIcons (Quad + 0x10 extra)
    centerQuad = TileIcon();
    bgQuad = TileIcon();

    // zero level config / position / rotation block.
    nemesisGridCol = 0;
    nemesisGridRow = 0;
    posX = 0.0f;
    posY = 0.0f;
    rotation = 0.0f;
    // facingDir and the reformT/posTransitionT/bgFadeT/start/target/transitionSpeed
    // fields are not touched by the binary's constructor; placeOnHexGrid writes
    // them. they're already zero from memset(GameBoard).

    // both ColorTints
    tintLevelNumber.init();
    tintLevelUpFlash.init();

    // 20 outline dots: Quad init each
    for (int i = 0; i < 20; i++) {
        outlineDots[i] = TileIcon();
    }

    // 20 fill dots: Quad init each
    for (int i = 0; i < 20; i++) {
        fillDots[i] = TileIcon();
    }

    // post-array fields
    cascadeLevelMirror = 0;
    cascadeActiveIndex = 0;
    pendingXP = 0;
    fillTimer = 1.0f;  // DAT_100059c34
    fillFlag = false;
    std::memset(pad3A19, 0, sizeof(pad3A19));

    // nemesis eat-cycle state (lives in the trailing pad past pendingXP).
    // zeroed at init; setHP's death path and the state-machine drive these.
    eatTarget = 0;
    eatActive = false;
    std::memset(pad3A25, 0, sizeof(pad3A25));
    eatStep   = 0;
    eatFired  = false;
    std::memset(pad3A2D, 0, sizeof(pad3A2D));

    // center quad UV: (0, 513) -> (184, 667) on 1024-wide texture, size 0.288 x 0.25
    centerQuad.quad.setTexCoords(0.0f, 0.50097656f, 0.1796875f, 0.6513672f);
    centerQuad.quad.setSize(0.2875f, 0.25f);

    // bg circle UV: (186, 47) -> (330, 191), size 0.225 x 0.225
    bgQuad.quad.setTexCoords(0.18164063f, 0.045898438f, 0.32226563f, 0.18652344f);
    bgQuad.quad.setSize(0.225f, 0.225f);

    // 20-dot circular layout. for each i:
    //   angle_deg = (i + 0.5) * (360 / 20) - 90      ; first dot at -81 deg (top, slightly right)
    //   angle_rad = angle_deg * PI / 180
    //   posX = cos(angle_rad) * radius + center_x
    //   posY = sin(angle_rad) * radius + center_y
    //   rotation = (i + 0.5) * (360/20)              ; in degrees, not offset by -90
    //
    // both outline dot[i] and fill dot[i] get the same position. each pair's
    // UV/size/offset matches what the binary writes.
    //
    // center.x and center.y come from *(float*)(this + 0x176c) and (this + 0x1768).
    // those addresses fall inside bgQuad (TileIcon at +0x16c0 spans +0x16c0..+0x1797),
    // specifically at TileIcon's quad.posX / quad.posY (+0xa8 and +0xac inside the
    // TileIcon = +0x1768 and +0x176c absolute). so center is just (bgQuad.posX, bgQuad.posY).
    // bgQuad has not been positioned yet (posX/posY are 0 from the constructor of Quad),
    // so center is (0, 0) here. placeOnHexGrid may move it.
    float centerX = bgQuad.quad.posX;
    float centerY = bgQuad.quad.posY;

    for (int i = 0; i < 20; i++) {
        float angleDeg = ((float)i + 0.5f) * (DEG_360 / 20.0f);
        float angleRad = (angleDeg + DEG_NEG90) * (PI / DEG_180);
        float c = std::cos(angleRad);
        float s = std::sin(angleRad);
        float r = DOT_CIRCLE_RADIUS;

        // outline dot UV: (160, 271) -> (172, 285), size (0.01875, 0.021875)
        outlineDots[i].quad.setTexCoords(0.15625f, 0.26464844f, 0.16796875f, 0.27832031f);
        outlineDots[i].quad.setSize(0.01875f, 0.021875f);
        outlineDots[i].quad.addVertexOffset(0.0f, -0.0109375f);
        outlineDots[i].quad.posX = c * r + centerX;
        outlineDots[i].quad.posY = s * r + centerY;
        outlineDots[i].quad.rotation = angleDeg;

        // fill dot UV: (381, 20) -> (393, 34), size (0.01875, 0.021875)
        fillDots[i].quad.setTexCoords(0.37207031f, 0.01953125f, 0.38378906f, 0.033203125f);
        fillDots[i].quad.setSize(0.01875f, 0.021875f);
        fillDots[i].quad.addVertexOffset(0.0f, -0.0109375f);
        fillDots[i].quad.posX = c * r + centerX;
        fillDots[i].quad.posY = s * r + centerY;
        fillDots[i].quad.rotation = angleDeg;
    }
}

// reconstructed from Ghidra FUN_10000996c (one-liner: clear visibility)
void NemesisRenderable::reset() {
    visible = false;
}

// reconstructed from Ghidra FUN_100008f64
void NemesisRenderable::setNemesisLevel(int count) {
    nemesisLevel = count;
    cascadeLevelMirror = count;

    // ColorTint::setNumber(value, textStyle=2, positionMode=1)
    tintLevelNumber.setNumber(count, 2, 1);
    // FUN_10003c870(tint, &posX_pair, mode=1): setPosition anchored to
    // &bgQuad.quad.posX (8 bytes covering posX,posY).
    tintLevelNumber.setPosition(bgQuad.quad.posX, bgQuad.quad.posY, 1);

    tintLevelUpFlash.setNumber(count, 2, 1);
    tintLevelUpFlash.setPosition(bgQuad.quad.posX, bgQuad.quad.posY, 1);
}

// reconstructed from Ghidra FUN_1000098d0
void NemesisRenderable::creditXP(int amount) {

    if (amount <= 0) {
        return;
    }

    pendingXP += amount;

    // if the level-up cascade animation isn't running, restart it. binary
    // gates the reset on `fillTimer >= 1.0` (Ghidra's NaN idiom resolves
    // to that comparison). active cascade keeps its current state.
    if (fillTimer >= 1.0f) {
        fillTimer = 0.0f;
        fillFlag  = false;
    }

    // walk the XP track. each XP point bumps nemesisXP by one; on overflow
    // past 19, carry into nemesisLevel and wrap. matches the binary's div+mod
    // pair (`level += xp / 20; xp = xp % 20`).
    int next = nemesisXP + amount;
    nemesisXP = next;

    if (next > 19) {
        nemesisLevel += next / 20;
    }

    nemesisXP = next % 20;
}

// FUN_100012f04(col, row, mode=0): linear un-snapped hex grid -> screen.
// returns both x and y in s0/s1 (Ghidra's decompile drops s1).
static float hexGridX(int col, int row) {
    constexpr float HEX_X_COL_STEP   =  0.196875f;
    constexpr float HEX_X_ROW_OFFSET = -0.098437503f;
    return (float)col * HEX_X_COL_STEP + (float)row * HEX_X_ROW_OFFSET;
}
static float hexGridY(int /*col*/, int row) {
    constexpr float HEX_Y_ROW_STEP   =  0.170498759f;
    return (float)row * HEX_Y_ROW_STEP;
}

// reconstructed from Ghidra FUN_100009874.
void NemesisRenderable::beginMoveTo(float durationScale,
                                    int32_t col, int32_t row, int facing) {
    nemesisGridCol  = col;
    nemesisGridRow  = row;
    facingDir       = (float)facing;
    posTransitionT  = 0.0f;
    transitionSpeed = durationScale;
    posStartX       = posX;
    posStartY       = posY;
    posTargetX      = hexGridX(col, row);
    posTargetY      = hexGridY(col, row);
}

// reconstructed from Ghidra FUN_100008fec
void NemesisRenderable::setNemesisXP(int count) {
    // FUN_10005722c(count, 0, 0x14) clamps to [0, 20]
    int clamped = count;
    if (clamped < 0) {
        clamped = 0;
    }
    if (clamped > 20) {
        clamped = 20;
    }

    nemesisXP = clamped;
    cascadeActiveIndex = clamped;

    // first `clamped` outline dots get full alpha (0xff), remainder dim (0x50).
    // all fill dots get alpha 0. each dot's quad scale also resets to (1,1).
    // setAlpha preserves existing R/G/B (per FUN_100008388).
    for (int i = 0; i < 20; i++) {
        uint8_t outlineAlpha = (i < nemesisXP) ? 0xff : 0x50;

        outlineDots[i].quad.setAlpha(outlineAlpha);
        outlineDots[i].quad.scaleX = 1.0f;
        outlineDots[i].quad.scaleY = 1.0f;

        fillDots[i].quad.setAlpha(0x00);
        fillDots[i].quad.scaleX = 1.0f;
        fillDots[i].quad.scaleY = 1.0f;
    }
}

// FUN_1000571d0(lo, hi, stream): linear-congruential RNG returning a float in
// [lo, hi). FUN_100008dc0 (Nemesis init) uses stream 0 (the cosmetic stream).
static float randomRange(float lo, float hi) {
    return rngFloat(lo, hi, 0);
}

// reconstructed from Ghidra FUN_100008dc0
void NemesisRenderable::placeOnHexGrid(int64_t levelConfig, float facingDir_) {
    visible = true;

    int32_t gridCol = (int32_t)(levelConfig & 0xFFFFFFFF);
    int32_t gridRow = (int32_t)((uint64_t)levelConfig >> 32);
    nemesisGridCol = gridCol;
    nemesisGridRow = gridRow;
    facingDir = facingDir_;

    posX = hexGridX(gridCol, gridRow);
    posY = hexGridY(gridCol, gridRow);
    rotation = (float)((int32_t)facingDir_ - 4) * ROT_DEG_PER_STEP;

    reformT = 0.0f;
    transitionSpeed = 1.0f;
    bgFadeT = 0.0f;
    posTransitionT = 1.0f;

    cascadeLevelMirror = 0;
    cascadeActiveIndex = 0;
    pendingXP = 0;
    fillTimer = 1.0f;
    fillFlag = false;

    setNemesisLevel(nemesisLevel);
    setNemesisXP(nemesisXP);

    // 25 hex segments: same generic hex tile UV for all, plus per-segment
    // animation seeds in scratch[0..3] and scratch[4..7].
    for (int i = 0; i < 25; i++) {
        // generic hex tile UV: (85, 101) -> (185, 195) on 1024 texture
        segments[i].quad.setTexCoords(0.08300781f, 0.09863281f, 0.18066406f, 0.19042969f);
        segments[i].quad.setSize(0.15625f, 0.146875f);
        segments[i].quad.addVertexOffset(0.0f, -0.0734375f);

        // scratch[0..3] = i * SEG_SPACING (0.04). this is the per-segment phase.
        // scratch[4..7] = random in [SEG_PHASE_HALF, 0.5] (= [0.3, 0.5]).
        // used as one endpoint of the scale lerp in update().
        float* scratch0 = (float*)&segments[i].scratch[0];
        float* scratch4 = (float*)&segments[i].scratch[4];
        *scratch0 = (float)i * SEG_SPACING;
        *scratch4 = randomRange(SEG_PHASE_HALF, 0.5f);
    }

    // run one update tick at dt=0 to seed the per-segment positions
    update(0.0f);
}

// reconstructed from Ghidra FUN_100009094.
// drives the position transition, swirl-in, segment animation, bg fade-in,
// outline-dot fade-in, and (when active) the fill cascade.
void NemesisRenderable::update(float dt) {

    if (!visible) {
        return;
    }

    // -- position transition (start -> target lerp) --
    if (posTransitionT < 1.0f) {
        float advance = (dt + dt) * transitionSpeed;
        posTransitionT += advance;

        if (posTransitionT >= 1.0f) {
            // transition complete: lock to 1.0 and rebake target from current
            // level config.
            posTransitionT = 1.0f;

            posX = hexGridX(nemesisGridCol, nemesisGridRow);
            // binary stashes `advance` into posY here; almost certainly a
            // Ghidra-decoded scratch reuse, not a real gameplay side effect,
            // but we replicate to stay byte-identical.
            posY = advance;
            rotation = (float)((int32_t)facingDir - 4) * ROT_DEG_PER_STEP;
            reformT = 0.0f;
        }

        // lerp posX/posY from start to target by posTransitionT
        float t = posTransitionT;
        posX = posTargetX * t + posStartX * (1.0f - t);
        posY = posTargetY * t + posStartY * (1.0f - t);
    }

    // -- swirl-in animation (segments lerp into final positions) --
    if (reformT < 1.0f) {
        reformT += dt * 5.0f * transitionSpeed;
        if (reformT >= 1.0f) {
            reformT = 1.0f;
        }
    }

    // -- per-segment animation --
    // base_x for segment cluster center, computed from reformT + posTransitionT
    float reformTerm = reformT * SEG_AMPL_X + (1.0f - reformT) * -0.1484375f;
    float transitionAmpl = std::sin(posTransitionT * PI);
    float baseX = reformTerm + transitionAmpl * SEG_AMPL_X_OFFSET;

    centerQuad.quad.posX = baseX + 0.1640625f;
    centerQuad.quad.posY = 0.0f;

    float phaseAdvance = dt * SEG_PHASE_RATE;

    for (int i = 0; i < 25; i++) {
        float* phasePtr = (float*)&segments[i].scratch[0];
        float* randPtr  = (float*)&segments[i].scratch[4];

        // advance segment phase, wrap to [0, 1)
        float phase = std::fmod(phaseAdvance + *phasePtr, 1.0f);
        *phasePtr = phase;

        // smooth phase via cosine
        float smoothPhase = 0.5f - std::cos((phase + phase) * PI) * 0.5f;

        // segment angle: lerp(-90, 90) by smoothPhase (degrees)
        float angleDeg = smoothPhase * DEG_POS90 + (1.0f - smoothPhase) * DEG_NEG90;
        float angleRad = angleDeg * (PI / DEG_180);

        // binary calls FUN_100057250(SEG_RADIUS_X, angleRad) which is a
        // sincos helper: returns scale*cos in s0 and scale*sin in s1. the
        // posX gets the cos result (offset from baseX) and posY gets sin.
        // earlier port had these swapped, which placed all segments on one
        // side of the body instead of arcing around it.
        segments[i].quad.posX = baseX + std::cos(angleRad) * SEG_RADIUS_X;
        segments[i].quad.posY = std::sin(angleRad) * SEG_RADIUS_X;
        segments[i].quad.rotation = angleDeg + DEG_NEG90;

        // size animation: lerp(SEG_RADIUS_Y, randomFromConfig, sin(PI*smoothPhase))
        float sizeBlend = std::sin(smoothPhase * PI);
        float scale = (*randPtr) * sizeBlend + (1.0f - sizeBlend) * SEG_RADIUS_Y;
        segments[i].quad.scaleX = scale;
        segments[i].quad.scaleY = scale;
    }

    // -- bg fade-in --
    if (bgFadeT < 1.0f) {
        bgFadeT += dt + dt;
        if (bgFadeT >= 1.0f) {
            bgFadeT = 1.0f;
        }

        uint8_t bgAlpha = (uint8_t)(int)(bgFadeT * ALPHA_FULL);
        bgQuad.quad.setAlpha(bgAlpha);
        tintLevelNumber.setAlpha(bgAlpha);

        // outline dots: lit ones use bgAlpha, unlit use bgFadeT * ALPHA_DIM
        for (int i = 0; i < 20; i++) {
            uint8_t a = (i < nemesisXP) ? bgAlpha
                                       : (uint8_t)(int)(bgFadeT * ALPHA_DIM);
            outlineDots[i].quad.setAlpha(a);
        }
    }

    // -- XP cascade animation (port of FUN_100009094's tail). drives the
    // per-dot fade-in (fillFlag == 0) and the post-level-up overflow flash
    // (fillFlag == 1). creditXP populates pendingXP and resets fillTimer = 0
    // to kick the cascade; each step animates one outline dot's alpha. once
    // cascadeActiveIndex passes 19, the cascade flips to flash mode and
    // setNemesisLevel rebuilds the tints.
    if (fillTimer < 1.0f) {

        if (!fillFlag) {
            // ---- fade-IN: animate one outline dot at a time ----
            if (fillTimer == 0.0f) {
                // start a new cascade step
                int oldIdx = cascadeActiveIndex;
                cascadeActiveIndex = oldIdx + 1;
                pendingXP -= 1;

                if (oldIdx > 18) {
                    // 20 dots filled -> level-up. flip to flash mode and
                    // rebuild the tints to reflect the new level.
                    cascadeActiveIndex = 0;
                    cascadeLevelMirror += 1;
                    fillFlag = true;
                    setNemesisLevel(cascadeLevelMirror);

                    // reset all 20 fill dots to full alpha + scale 1.0 to
                    // seed the flash fade-out.
                    for (int i = 0; i < 20; i++) {
                        fillDots[i].quad.setAlpha(0xFF);
                        fillDots[i].quad.scaleX = 1.0f;
                        fillDots[i].quad.scaleY = 1.0f;
                    }

                    tintLevelUpFlash.setAlpha(0xFF);
                    tintLevelUpFlash.scaleX = 1.0f;
                    tintLevelUpFlash.scaleY = 1.0f;

                    if (Game* g = getGame()) {
                        g->soundQueue.trigger(0x2b);   // "level up complete"
                    }
                    return;
                }

                // not a level-up step. per-dot XP-gain sound, gated on dt > 0
                // so a dt == 0 tick on a freshly-kicked cascade stays silent.
                if (dt > 0.0f) {

                    if (Game* g = getGame()) {
                        g->soundQueue.trigger(0x2c);   // "xp dot gained"
                    }
                }
            }

            // advance fillTimer at a rate scaled by pendingXP (more pending
            // = faster cascade so the player isn't kept waiting).
            float rate = (float)pendingXP;

            if (rate < 1.0f) {
                rate = 1.0f;
            }

            float t = fillTimer + (dt + dt) * rate;

            if (t >= 1.0f) {
                t = 1.0f;
            }

            fillTimer = t;

            // animate this step's outline dot. cascadeActiveIndex was
            // incremented above; the dot being animated is at idx-1.
            int dotIdx = cascadeActiveIndex - 1;

            if (dotIdx >= 0 && dotIdx < 20) {
                uint8_t a = (uint8_t)(int)(t * ALPHA_FULL + (1.0f - t) * ALPHA_DIM);
                outlineDots[dotIdx].quad.setAlpha(a);

                // scale: cos-eased pop from 1.0 to 1.8 over fillTimer.
                float u = 0.5f - std::cos((t + t) * PI) * 0.5f;
                float scale = u * 1.8f + (1.0f - u);   // 1.8f = DAT_100059c3c
                outlineDots[dotIdx].quad.scaleX = scale;
                outlineDots[dotIdx].quad.scaleY = scale;
            }

            if (fillTimer < 1.0f) {
                return;
            }
        }
        else {
            // ---- fade-OUT: level-up flash. all 20 fill dots dim while
            //                outline dots scale back to 1.0. ----
            float t = fillTimer + (dt + dt);

            if (t >= 1.0f) {
                t = 1.0f;
            }

            fillTimer = t;

            for (int i = 0; i < 20; i++) {
                uint8_t outlineA = (uint8_t)(int)(t * ALPHA_DIM + (1.0f - t) * ALPHA_FULL);
                outlineDots[i].quad.setAlpha(outlineA);

                float u = 0.5f - std::cos((t + t) * PI) * 0.5f;
                float outlineScale = u + u + (1.0f - u);   // u*2 + (1-u) = 1 + u
                outlineDots[i].quad.scaleX = outlineScale;
                outlineDots[i].quad.scaleY = outlineScale;

                uint8_t fillA = (uint8_t)(int)((1.0f - t) * ALPHA_FULL);
                fillDots[i].quad.setAlpha(fillA);

                float fillScale = t * 4.0f + (1.0f - t);
                fillDots[i].quad.scaleX = fillScale;
                fillDots[i].quad.scaleY = fillScale;
            }

            uint8_t tintA = (uint8_t)(int)((1.0f - t) * ALPHA_FULL);
            tintLevelUpFlash.setAlpha(tintA);
            float tintScale = t * 4.0f + (1.0f - t);
            tintLevelUpFlash.scaleX = tintScale;
            tintLevelUpFlash.scaleY = tintScale;

            if (fillTimer < 1.0f) {
                return;
            }

            // flash complete: clear flag, ready for next fade-in cycle.
            fillFlag = false;
        }

        // post-step: if more XP pending, kick the next cascade step by
        // resetting fillTimer to 0 (caught by the `fillTimer == 0` branch
        // above next frame).
        if (pendingXP > 0) {
            fillTimer = 0.0f;
        }
    }
}

// reconstructed from Ghidra FUN_1000096fc.
// draws the 25 hex segments + center quad, with the whole transform rotated
// around (posX, posY) by `rotation` degrees.
void NemesisRenderable::drawSegments() {

    if (!visible) {
        return;
    }

    bindTexture(9);
    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);
    glRotatef(rotation, 0.0f, 0.0f, 1.0f);

    for (int i = 0; i < 25; i++) {
        segments[i].quad.draw();
    }

    centerQuad.quad.draw();

    glPopMatrix();
}

// reconstructed from Ghidra FUN_100009790.
// draws the bg circle, fill dots (when fillFlag is set), outline dots, and
// the two ColorTints. translated by (posX, posY) but not rotated.
void NemesisRenderable::drawFrame() {

    if (!visible) {
        return;
    }

    bindTexture(9);
    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    bgQuad.quad.draw();

    if (fillFlag) {

        for (int i = 0; i < 20; i++) {
            fillDots[i].quad.draw();
        }
    }

    for (int i = 0; i < 20; i++) {
        outlineDots[i].quad.draw();
    }

    if (fillFlag) {
        tintLevelUpFlash.draw();
    }

    tintLevelNumber.draw();

    glPopMatrix();
}
