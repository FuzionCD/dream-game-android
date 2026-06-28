#include "stat_bars.h"
#include "renderer.h"      // bindTexture
#include "random.h"        // rngFloat, per-slot animation rate jitter
#include "tile_content.h"  // lookupContentIconUVPx for spawnIconBurst
#include <GLES/gl.h>
#include <cmath>
#include <cstring>
#include <new>

// FUN_10003bc20, placement-style ctor.
// clears the visible byte then default-constructs all 20 Quads in place.
void StatBars::init() {
    visible = false;
    posX = 0.0f;
    posY = 0.0f;

    for (int i = 0; i < 20; i++) {
        // matches the binary's loop: thunk_FUN_100007d78 over each slot's Quad (stride 0xE8)
        new (&slots[i].quad) Quad();
        slots[i].targetX     = 0.0f;
        slots[i].targetY     = 0.0f;
        slots[i].alphaTarget = 0.0f;
        slots[i].animT       = 0.0f;
    }
}

// FUN_10003be88, draws all 20 Quads under a translate by (posX, posY).
// texture binding is the caller's responsibility (binary calls bindTexture(9)
// inside this function; we mirror that).
void StatBars::draw() {

    if (!visible) {
        return;
    }

    bindTexture(9);

    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    for (int i = 0; i < 20; i++) {
        slots[i].quad.draw();
    }

    glPopMatrix();
}

// FUN_10003beec, full port.
//
// per slot:
//   1. roll a per-slot rate jitter in [0.8, 1.2] via stream 0; advance animT
//      by (dt / ANIM_DURATION) * roll. clamp to 1.0; track if any slot still
//      animating across the 20-slot pass.
//   2. quad.scaleX = slot.targetX * animT^2 (quadratic ease-in scale).
//      quad.scaleY = slot.targetY * animT^2.
//   3. tint pair at scratch00[0..7] = slot.alphaTarget * cos-eased(animT).
//      stored as two identical floats (binary uses CONCAT44 to write 8 bytes).
//   4. quad alpha = (1 - cos-eased((animT - 0.7) / 0.3 * PI)) * 255. this
//      keeps quad invisible during the first 70% of animT, then ramps it up
//      over the last 30%. the PI / 0.3 / -0.7 / 255 constants come from the
//      __TEXT,__const block at 0x10005a2c0..0x10005a2cc.
//   5. once all slots animT == 1.0 (no slot moved), clear visible.
//
// visible=false makes the body early-out; nothing runs until a level
// transition triggers setVisible(true).
void StatBars::update(float dt) {

    if (!visible) {
        return;
    }

    constexpr float ANIM_DURATION       = 0.8f;       // DAT_10005a2b8
    constexpr float ANIM_RATE_JITTER_HI = 1.2f;       // DAT_10005a2bc
    constexpr float ANIM_PI             = 3.1415927f; // DAT_10005a2c0
    constexpr float ALPHA_PROG_OFFSET   = -0.7f;      // DAT_10005a2c4
    constexpr float ALPHA_PROG_DIVISOR  =  0.3f;      // DAT_10005a2c8
    constexpr float ALPHA_END           = 255.0f;     // DAT_10005a2cc

    bool anyAnimating = false;
    float dtRate = dt / ANIM_DURATION;

    for (int i = 0; i < 20; i++) {
        StatBarSlot& slot = slots[i];

        // 1. per-slot rate jitter + advance animT
        float jitter = rngFloat(ANIM_DURATION, ANIM_RATE_JITTER_HI, /*stream*/ 0);
        float t      = slot.animT + dtRate * jitter;

        if (t < 1.0f) {
            anyAnimating = true;
        } else {
            t = 1.0f;
        }

        slot.animT = t;

        // 2. quadratic-ease translation: targetX * t^2 -> posX, targetY * t^2 -> posY.
        //    each particle slides outward from the burst origin to (targetX,
        //    targetY), in screen-space units (targetX/Y are pre-rolled by
        //    spawnIconBurst as magnitude * (cos(angle), sin(angle))).
        float t2          = t * t;
        slot.quad.posX    = slot.targetX * t2;
        slot.quad.posY    = slot.targetY * t2;

        // 3. cos-eased uniform scale: alphaTarget * (0.5 - cos(t*PI)/2). scale
        //    ramps from 0 up to alphaTarget around t=0.5, then back to 0 at
        //    t=1 (so the icon "pops" out and shrinks away in size on top of
        //    the position lerp + alpha fade).
        float ease1 = 0.5f - std::cos(t * ANIM_PI) * 0.5f;
        float val   = slot.alphaTarget * ease1;
        slot.quad.scaleX = val;
        slot.quad.scaleY = val;

        // 4. quad alpha: gated to the last 30% of animT via the (t - 0.7)/0.3
        // progress curve. clamp to 0 when t <= 0.7 (= matches binary's
        // `fcsel s0,s1,s2,gt` where s2 == 0).
        float prog2 = t + ALPHA_PROG_OFFSET;
        float arg   = (prog2 > 0.0f) ? (prog2 / ALPHA_PROG_DIVISOR) * ANIM_PI : 0.0f;
        float ease2 = 0.5f - std::cos(arg) * 0.5f;
        float alpha = (1.0f - ease2) * ALPHA_END;
        slot.quad.setAlpha(static_cast<uint8_t>(static_cast<int>(alpha)));
    }

    if (!anyAnimating) {
        visible = false;
    }
}

// reconstructed from Ghidra FUN_10003bc94. seeds 20 icon-particle slots that
// burst outward from `tilePos` when the player consumes a tile. each slot's
// quad gets the per-content-type icon UV (FUN_100014980 lookup) and a polar
// target offset randomised per slot; the per-frame StatBars::update tick
// then animates each slot's quad scale + position toward its target.
void StatBars::spawnIconBurst(const float* tilePos, int contentType) {
    constexpr float NEG_PI         = -3.1415927f;     // DAT_10005a28c
    constexpr float POS_PI         =  3.1415927f;     // DAT_10005a290
    constexpr float JITTER_LO      = -0.02f;          // DAT_10005a294
    constexpr float JITTER_HI      =  0.02f;          // DAT_10005a298
    constexpr float TEX_PIXEL_INV  =  1.0f / 1024.0f; // DAT_10005a29c
    constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f; // DAT_10005a2a0
    constexpr float ROT_DEG_FACTOR = 180.0f;          // DAT_10005a2a4
    constexpr float ROT_DEG_OFFSET = 90.0f;           // DAT_10005a2a8
    constexpr float ALPHA_LO       = 0.7f;            // DAT_10005a2ac
    constexpr float MAGNITUDE_LO   = 0.05f;           // DAT_10005a2b0
    constexpr float MAGNITUDE_HI   = 0.10f;           // DAT_10005a2b4
    constexpr int   STREAM         = 0;               // cosmetic LCG stream
    constexpr int   SLOT_COUNT     = 20;

    visible = true;
    posX = tilePos[0];
    posY = tilePos[1];

    float uvOriginPx[2] = { 0.0f, 0.0f };
    float uvSizePx[2]   = { 0.0f, 0.0f };
    lookupContentIconUVPx(contentType, uvOriginPx, uvSizePx);

    // initial angle offset, rolled once before the loop. binary stashes
    // the result and re-uses it as the per-slot angle floor.
    float angleBase = rngFloat(NEG_PI, POS_PI, STREAM);

    for (int i = 0; i < SLOT_COUNT; i++) {
        StatBarSlot& slot = slots[i];

        // distribute slots evenly around the circle, with two layers of
        // jitter (the loop-invariant angleBase + a per-slot rngFloat).
        float u     = (float)i / (float)SLOT_COUNT;
        float spoke = u * POS_PI + (1.0f - u) * NEG_PI;
        float angle = angleBase + spoke + rngFloat(JITTER_LO, JITTER_HI, STREAM);

        // quad UV / size from the per-content-type pixel-coord table,
        // converted to atlas-fraction UV (/1024) and screen-fraction
        // size (/640).
        float u0 = uvOriginPx[0]                 * TEX_PIXEL_INV;
        float v0 = uvOriginPx[1]                 * TEX_PIXEL_INV;
        float u1 = (uvOriginPx[0] + uvSizePx[0]) * TEX_PIXEL_INV;
        float v1 = (uvOriginPx[1] + uvSizePx[1]) * TEX_PIXEL_INV;
        slot.quad.setTexCoords(u0, v0, u1, v1);
        slot.quad.setSize(uvSizePx[0] * SCREEN_PIXEL_INV,
                          uvSizePx[1] * SCREEN_PIXEL_INV);

        // pos / scale start at 0 (the burst grows out from tile center).
        slot.quad.posX     = 0.0f;
        slot.quad.posY     = 0.0f;
        slot.quad.scaleX   = 0.0f;
        slot.quad.scaleY   = 0.0f;

        // rotation in degrees: angle (rad -> deg) + 90deg face-outward offset.
        slot.quad.rotation = (angle * ROT_DEG_FACTOR) / POS_PI + ROT_DEG_OFFSET;

        slot.animT       = 0.0f;
        slot.alphaTarget = rngFloat(ALPHA_LO, 1.0f, STREAM);

        // polar burst direction (FUN_100057250 dual-return). magnitude
        // = distance from center where the slot animates to.
        float magnitude = rngFloat(MAGNITUDE_LO, MAGNITUDE_HI, STREAM);
        slot.targetX = magnitude * std::cos(angle);
        slot.targetY = magnitude * std::sin(angle);
    }
}
