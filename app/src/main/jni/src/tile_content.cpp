#include "tile_content.h"
#include "game.h"      // getGame -> achievementTracker
#include "renderer.h"  // bindTexture
#include <cmath>

namespace {

// per-content-type sub-icon UV / size / offset table.
// extracted from binary __DATA at DAT_1000787e8 (26 entries of 0x50 bytes).
// each row: { uPx, vPx, uExtPx, vExtPx, offXPx, offYPx, drawMagnitudeTint }
//   - uPx / vPx / uExtPx / vExtPx: pixel coords on the ui1.png atlas (not
//     tiles1..4; TileContent::draw binds texture 9 = ui1).
//   - offXPx / offYPx: per-type vertex nudge applied via addVertexOffset.
//   - drawMagnitudeTint: byte gate; when set, the embedded
//     ColorTint (= the magnitude digits) gets drawn after baseQuad.
//
// types we know about: 2=ATK pickup, 3=DEF pickup, 5=CONTROL pickup,
// 6=HEALTH pickup. types 0 (no content) and 1 (snag-tile marker) are
// all-zero entries; TileContent isn't allocated for those.
struct ContentTypeUV {
    int  uPx, vPx;
    int  uExtPx, vExtPx;
    int  offXPx, offYPx;
    bool drawMagnitudeTint;
    // colorTint offsets in pixels relative to baseQuad pos. the binary uses
    // separate offsets for 1-digit vs 2-digit magnitudes since the digit
    // grouping shifts width.
    int  magOff1X, magOff1Y;
    int  magOff2X, magOff2Y;
};
constexpr ContentTypeUV kContentTypeUVTable[26] = {
    /*  0 */ {  0,   0,    0,    0,   0,   0,  false,    0,  0,    0,  0},  // unused, blank
    /*  1 */ {  0,   0,    0,    0,   0,   0,  false,    0,  0,    0,  0},  // unused, snag
    /*  2 */ {652,   0,   78,   78,   0,   0,  true ,   -2,  0,   -3,  0},  // ATK pickup
    /*  3 */ {731,   0,   70,   78,   0,   0,  true ,   -2,  0,   -2,  0},  // DEF pickup
    /*  4 */ {885,   0,   62,   80,   0,   0,  true ,   -1,  6,   -1,  6},
    /*  5 */ {948,   0,   76,   76,   0,   0,  true ,   -2,  0,   -2,  0},  // CONTROL pickup
    /*  6 */ {802,   0,   82,   76,   0,   0,  true ,   -1, -1,   -1,  1},  // HEALTH pickup
    /*  7 */ {331, 102,   64,   80,   0,   0,  true ,   -1,  6,   -1,  6},
    /*  8 */ {332,  20,   86,   80,   1,  -1,  false,    0,  0,    0,  0},
    /*  9 */ {376, 601,   77,   76,   0,  -3,  true ,   -1,  3,   -1,  3},
    /* 10 */ {358, 678,  103,   93,   0,   0,  false,    0,  0,    0,  0},
    /* 11 */ {462, 696,   66,   74,   0,  -2,  true ,   -1,  1,   -1,  1},
    /* 12 */ {301, 596,   74,   80,   0,   0,  true ,   -2, 11,   -2, 11},
    /* 13 */ {322, 192,   74,   77,   0,   0,  false,    0,  0,    0,  0},
    /* 14 */ {530, 607,   74,   83,   0,   0,  false,    0,  0,    0,  0},
    /* 15 */ {397, 137,   90,   82,   0,   0,  false,    0,  0,    0,  0},
    /* 16 */ {530, 691,   84,   80,   0,   0,  false,    0,  0,    0,  0},
    /* 17 */ {462, 605,   67,   90,   0,   0,  true ,   -2, 12,   -2, 12},
    /* 18 */ {525, 518,   71,   86,  -1,   0,  true ,   -1,  1,   -1,  1},
    /* 19 */ {172, 373,   75,   81,   0,   0,  false,    0,  0,    0,  0},
    /* 20 */ {349, 421,   83,   90,   2,   0,  false,    0,  0,    0,  0},
    /* 21 */ {248, 321,   90,   72,   1,   0,  false,    0,  0,    0,  0},
    /* 22 */ {349, 336,   76,   84,   1,   0,  true ,   -1,  1,    0,  1},
    /* 23 */ {426, 270,   67,   86,   0,   0,  false,    0,  0,    0,  0},
    /* 24 */ {248, 398,  100,   80,   1,   2,  true ,   -1, -4,   -1, -5},
    /* 25 */ {564, 246,   79,   76,   0,  -5,  false,    0,  0,    0,  0},
};

// FUN_100014d84, apply pixel-coord UV + size to a Quad.
// inputs are pixel coords on a 1024x1024 atlas; converted via DAT_100059ebc
// (= 1/1024) for UV space and DAT_100059ec0 (= 640) for screen space.
void applyPixelUV(Quad& q, int uPx, int vPx, int uExtPx, int vExtPx, float scale = 1.0f) {
    constexpr float TEX_PIXEL_INV    = 1.0f / 1024.0f;  // DAT_100059ebc
    constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;   // DAT_100059ec0

    float u0 = (float)uPx                  * TEX_PIXEL_INV;
    float v0 = (float)vPx                  * TEX_PIXEL_INV;
    float u1 = (float)(uPx + uExtPx)       * TEX_PIXEL_INV;
    float v1 = (float)(vPx + vExtPx)       * TEX_PIXEL_INV;
    q.setTexCoords(u0, v0, u1, v1);

    float sw = ((float)uExtPx * scale) * SCREEN_PIXEL_INV;
    float sh = ((float)vExtPx * scale) * SCREEN_PIXEL_INV;
    q.setSize(sw, sh);
}

}  // anonymous namespace

// reconstructed from Ghidra FUN_100014980.
void lookupContentIconUVPx(int contentType, float* uvOriginPx, float* uvSizePx) {

    if (contentType < 0 || contentType >= 26) {
        return;
    }
    const ContentTypeUV& uv = kContentTypeUVTable[contentType];
    uvOriginPx[0] = (float)uv.uPx;
    uvOriginPx[1] = (float)uv.vPx;
    uvSizePx[0]   = (float)uv.uExtPx;
    uvSizePx[1]   = (float)uv.vExtPx;
}

// reconstructed from Ghidra FUN_100014708.
//
// composes the MovableActor base init (FUN_100038b18) with TileContent-
// specific setup: type/magnitude storage and the embedded colorTint.
//
// the binary registers the new TileContent with the global audio dispatcher
// at audioState (FUN_10004e36c); that's where its update calls get hooked
// into the per-frame tick.
void TileContent::init(uint32_t typeIn, int magnitude, void* parentPtr) {
    initBase(parentPtr);                    // FUN_100038b18

    // ---- TileContent-specific (FUN_100014708 from line 4 onward) ----
    type               = 0;
    rawMagnitude       = 0;
    displayedMagnitude = -1;                // binary writes 0xFFFFFFFF as a sentinel
    colorTint.init();                       // FUN_10003c070

    setType(typeIn);                        // FUN_1000147d4
    rawMagnitude = magnitude;
    setMagnitude(magnitude);                // FUN_100014c6c

    // achievement "Serendipity" (= binary's FUN_10004e36c). fires when an
    // XP-drop content (type 4) lands with magnitude 4+.
    if (type == 4 && displayedMagnitude > 3) {
        getGame()->achievementTracker().increment(AchievementId::Serendipity);
    }
}

// reconstructed from Ghidra FUN_10001467c.
// lightweight variant of init() used by LevelUpPanel::init for its 3 stat-slot
// icons. matches the binary byte-for-byte: no setType / setMagnitude / audio
// register call. caller must call setType (and optionally setMagnitude) before
// the icon is rendered.
void TileContent::initDefault() {
    initBase(nullptr);

    type               = 0;
    rawMagnitude       = 0;
    displayedMagnitude = 0;   // plain 0 here, not the -1 sentinel init() uses
    colorTint.init();
}

// reconstructed from Ghidra FUN_1000147d4.
//
// sets the on-tile sub-icon's UV/offset based on type. picks pixel-space
// (uPx, vPx, uExtPx, vExtPx) and a (offXPx, offYPx) vertex nudge from the
// per-type table extracted from DAT_1000787e8. the sub-icon lives on ui1.png;
// draw() binds texture 9 for it (not the hex-face tiles1..4 atlas).
void TileContent::setType(uint32_t typeIn) {

    if (typeIn >= 0x1A) {
        return;
    }

    type = (int)typeIn;
    const ContentTypeUV& uv = kContentTypeUVTable[typeIn];

    // FUN_100014980 + FUN_100014d84: convert pixel UV/size to atlas-relative
    // floats and apply to baseQuad (the sub-icon Quad).
    applyPixelUV(baseQuad, uv.uPx, uv.vPx, uv.uExtPx, uv.vExtPx, 1.0f);

    // FUN_100008238 (addVertexOffset): per-type pixel nudge to the vertex
    // bounding box. converted via DAT_100059eb8 (= 640) to screen space.
    constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;  // DAT_100059eb8
    float dx = (float)uv.offXPx * SCREEN_PIXEL_INV;
    float dy = (float)uv.offYPx * SCREEN_PIXEL_INV;

    if (dx != 0.0f || dy != 0.0f) {
        baseQuad.addVertexOffset(dx, dy);
    }
}

// FUN_100014a98, reposition the embedded ColorTint relative to baseQuad,
// picking the 1-digit or 2-digit offset slot from the per-type UV table based
// on the current displayedMagnitude. called from setPosition (after baseQuad
// moves) and setMagnitude (after the digit count may have changed).
void TileContent::repositionTintForMagnitude() {

    if (type < 0 || type >= 26) {
        return;
    }

    constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;  // DAT_100059ecc
    const ContentTypeUV& uv = kContentTypeUVTable[type];

    int magOffX = (displayedMagnitude > 9) ? uv.magOff2X : uv.magOff1X;
    int magOffY = (displayedMagnitude > 9) ? uv.magOff2Y : uv.magOff1Y;

    float tintX = (float)magOffX * SCREEN_PIXEL_INV + baseQuad.posX;
    float tintY = (float)magOffY * SCREEN_PIXEL_INV + baseQuad.posY;

    colorTint.setPosition(tintX, tintY, 1);
}

// reconstructed from Ghidra FUN_100014c6c.
//
// pushes magnitude into the embedded ColorTint, with a no-op early-out if the
// value hasn't changed (avoids redundant digit-Quad rebuilds). when the digit
// count crosses 9, the tint's anchor offset within the icon shifts (1-digit
// vs 2-digit slots in the per-type UV table), so repositionTintForMagnitude
// must run after every change to displayedMagnitude.
void TileContent::setMagnitude(int magnitude) {

    if (displayedMagnitude == magnitude) {
        return;
    }

    displayedMagnitude = magnitude;

    // FUN_10003c174 is ColorTint::setNumber. Args 1, 1 = (textStyle, positionMode);
    // textStyle 1 selects the small in-tile glyph set, positionMode 1 = center.
    colorTint.setNumber(magnitude, 1, 1);

    repositionTintForMagnitude();
}

// reconstructed from Ghidra FUN_100014870. setMagnitude variant that also
// writes rawMagnitude. used by FUN_100025dcc (snag death) to grow a tile's
// stat bonus, and by FUN_100025238 cases 0x46 / 0x56 / 0x5e.
void TileContent::setRawAndDisplayMagnitude(int magnitude) {
    rawMagnitude = magnitude;
    setMagnitude(magnitude);

    // achievement "Serendipity" (= binary's FUN_10004e36c). fires when an
    // XP-drop content (type 4) grows to magnitude 4+.
    if (type == 4 && displayedMagnitude > 3) {
        getGame()->achievementTracker().increment(AchievementId::Serendipity);
    }
}

// FUN_100014a40, TileContent's vtable[4] override (setPosition).
//
// applies a small per-class fixed offset to the input position (the binary
// uses DAT_100059ec4 = 0.0015625 / DAT_100059ec8 = 0.003125), writes the
// adjusted (x, y) to baseQuad, then repositions the embedded colorTint.
//
// FUN_100014a98, colorTint reposition. picks per-type pixel offsets from
// the table (1-digit form when displayedMagnitude <= 9, 2-digit form
// otherwise), divides by 640 (= DAT_100059ecc) for screen space, adds the
// adjusted baseQuad pos as the anchor.
void TileContent::setPosition(float x, float y, int /*skipLayout*/) {
    constexpr float CLASS_X_OFFSET = 0.0015625f;  // DAT_100059ec4
    constexpr float CLASS_Y_OFFSET = 0.003125f;   // DAT_100059ec8

    // base call snaps the adjusted position (binary passes flag 0). the
    // incoming flag is ignored: the tint is always repositioned afterward.
    MovableActor::setPosition(x + CLASS_X_OFFSET, y + CLASS_Y_OFFSET, 0);

    repositionTintForMagnitude();
}

// FUN_1000149d0, TileContent's vtable[2] override (not the inherited base).
// per the binary:
//   1. early-out if !visible and fadeT >= 1.0 (same gate as base draw)
//   2. bind texture 9 (ui1.png; the sub-icons live here, not tiles1..4)
//   3. call MovableActor's base draw (FUN_100038b88) which renders baseQuad
//   4. if the per-type drawMagnitudeTint gate is set, render the embedded
//      colorTint, the digit display showing the rolled magnitude
void TileContent::draw() {

    if (!visible && fadeT >= 1.0f) {
        return;
    }

    bindTexture(9);
    baseQuad.draw();

    if (type >= 0 && type < 26 && kContentTypeUVTable[type].drawMagnitudeTint) {
        colorTint.draw();
    }
}

// reconstructed from Ghidra FUN_100014b24 (also FUN_100014b84). propagate
// alpha onto baseQuad and the magnitude ColorTint so the digit display dims
// with the icon.
void TileContent::setAlpha(uint8_t alpha) {
    baseQuad.setAlpha(alpha);
    colorTint.setAlpha(alpha);
}

// the squish: cos-eases (1, 1) -> (0.7, 0) over fadeT, applied to both
// baseQuad and the embedded ColorTint so the digit display collapses
// with the tile icon.
//
//   ease   = (1 - cos(fadeT * pi)) / 2          // 0 at start, 1 at end
//   scaleX = ease * 0.7 + (1 - ease)            // 1.0 -> 0.7
//   scaleY = ease * 0.0 + (1 - ease) = 1 - ease // 1.0 -> 0.0  (vertical squish)
void TileContent::onFade(float fadeT) {
    constexpr float COS_PI       = 3.1415927f;  // DAT_100059ed0
    constexpr float SCALE_X_END  = 0.7f;        // DAT_100059ed4

    float ease   = 0.5f - std::cos(fadeT * COS_PI) * 0.5f;
    float scaleX = ease * SCALE_X_END + (1.0f - ease);
    float scaleY = 1.0f - ease;

    baseQuad.scaleX = scaleX;
    baseQuad.scaleY = scaleY;
    colorTint.scaleX = scaleX;
    colorTint.scaleY = scaleY;
}

// reconstructed from Ghidra FUN_100014bf8.
//
// inverse of onFade. used by revive's scale-out animation to "pop" the
// tile back into existence after a re-spawn:
//   scaleX = ease + (1 - ease) * 0.7  // 0.7 -> 1.0
//   scaleY = ease + (1 - ease) * 0.0  // 0.0 -> 1.0
void TileContent::onScaleOut(float scaleOutT) {
    constexpr float COS_PI       = 3.1415927f;  // DAT_100059ed8
    constexpr float SCALE_X_END  = 0.7f;        // DAT_100059edc

    float ease   = 0.5f - std::cos(scaleOutT * COS_PI) * 0.5f;
    float scaleX = ease + (1.0f - ease) * SCALE_X_END;
    float scaleY = ease;

    baseQuad.scaleX = scaleX;
    baseQuad.scaleY = scaleY;
    colorTint.scaleX = scaleX;
    colorTint.scaleY = scaleY;
}
