#include "color_tint.h"
#include "font_glyphs.h"
#include "renderer.h"    // bindTexture
#include "title_menu.h"  // for TileIcon
#include <GLES/gl.h>
#include <cmath>

// internal: delete each owned TileIcon and clear the vector.
// the std::vector<TileIcon*> doesn't own its pointees, so we delete them
// manually here before clearing the vector. matches the binary's manual
// free loop in FUN_10003c174 / FUN_10003c5ac.
static void freeDigits(std::vector<TileIcon*>& digits) {

    for (TileIcon* icon : digits) {

        if (icon) {
            delete icon;
        }
    }

    digits.clear();
}

// reconstructed from Ghidra FUN_10003c070
void ColorTint::init() {
    // value-init leaves digits empty; init() is the explicit "reset to
    // defaults" call. if the vector still owns digit allocations from a
    // previous setNumber, tear them down too (init is only ever called
    // pre-population).
    freeDigits(digits);
    posX = 0.0f;
    posY = 0.0f;
    // default tint is opaque white.
    colorR = 0xff;
    colorG = 0xff;
    colorB = 0xff;
    colorA = 0xff;
    anchorX = 0.0f;
    anchorY = 0.0f;
    scaleX = 1.0f;
    scaleY = 1.0f;
}

// reconstructed from Ghidra FUN_10003c0ec
void ColorTint::draw() {
    bindTexture(9);
    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    if (scaleX != 1.0f || scaleY != 1.0f) {
        glScalef(scaleX, scaleY, 1.0f);
    }

    for (TileIcon* icon : digits) {

        if (icon != nullptr) {
            icon->quad.draw();
        }
    }

    glPopMatrix();
}

// reconstructed from Ghidra FUN_10003c8d8.
// in the binary this also walks the digit vector and applies setColor to each
// (FUN_10000826c). we replicate that.
void ColorTint::setColor(uint8_t r, uint8_t g, uint8_t b) {
    colorR = r;
    colorG = g;
    colorB = b;

    for (TileIcon* icon : digits) {

        if (icon != nullptr) {
            icon->quad.setColor(colorR, colorG, colorB, colorA);
        }
    }
}

// reconstructed from Ghidra FUN_10003c948.
// only the alpha byte changes; same vector walk applies.
void ColorTint::setAlpha(uint8_t a) {
    colorA = a;

    for (TileIcon* icon : digits) {

        if (icon != nullptr) {
            icon->quad.setColor(colorR, colorG, colorB, colorA);
        }
    }
}

// FUN_100057374: pixel-snap to 1/640 grid.
// DAT_10005a72c = 640.0 (the game's reference horizontal pixel count)
static float pixelSnap(float v) {
    constexpr float ref = 640.0f;
    float scaled = v * ref + (v < 0.0f ? -0.5f : 0.5f);
    return (float)(int)scaled / ref;
}

// reconstructed from Ghidra FUN_10003c870
void ColorTint::setPosition(float x, float y, int mode) {

    if (mode != 0) {
        // mode 1: pixel-snap the position relative to the anchor.
        // posX = (x - anchorX) + snap(anchorX)  =>  x adjusted by anchor's rounding delta
        x += pixelSnap(anchorX) - anchorX;
        y += pixelSnap(anchorY) - anchorY;
    }

    posX = x;
    posY = y;
}

// internal helper: decompose an integer into its decimal digits, low-order first.
// matches FUN_10003c174's loop structure exactly: continue condition is checked
// before the divide, so value 0 produces a single 0 digit (not two).
static int decomposeDigits(int value, int out[16]) {
    int v = value < 0 ? -value : value;
    int n = 0;
    bool cond;

    do {
        out[n++] = v % 10;
        cond = (9 < v);
        v /= 10;
    } while (cond && n < 16);

    return n;
}

// internal: shared body of setNumber / setSignedNumber. takes a flat array
// of glyph indices (low-order first; sign char last when included) and lays
// them out horizontally as TileIcons. mirrors FUN_10003c25c's math exactly.
//
// FUN_10003c174 builds a linked list of digits then hands it to FUN_10003c25c
// which dequeues each digit (head-to-tail = high-to-low + sign first),
// configures a TileIcon for it, and lays them out. our flat array iterates
// in reverse so srcIdx = digitCount-1 (= sign char, if any, else MSB) draws
// first and srcIdx = 0 (= LSB) draws last.
void ColorTint::layoutDigits(const int* digitArr, int digitCount, int textStyle,
                             int positionMode) {
    const FontGlyph* style = FONT_GLYPHS[textStyle];

    // (re)size the digit vector to digitCount slots. previous contents were
    // already freed by the caller via freeDigits().
    digits.resize(digitCount, nullptr);

    float totalWidth = 0.0f;   // fVar19 in the binary (sum of width - kernL - kernR)
    float cursorX = 0.0f;      // fVar16 carry between iterations

    for (int outIdx = 0; outIdx < digitCount; outIdx++) {
        int srcIdx = digitCount - 1 - outIdx;  // reverse: high-order first
        const FontGlyph& g = style[digitArr[srcIdx]];

        // create a fresh TileIcon (operator_new(0xD8) + thunk_FUN_100007d78)
        TileIcon* icon = new TileIcon();
        icon->quad = Quad();
        digits[outIdx] = icon;

        // UV: pixel coords / 1024
        float u0 = g.uvMinX * FONT_UV_SCALE;
        float v0 = g.uvMinY * FONT_UV_SCALE;
        float u1 = (g.uvMinX + g.width)  * FONT_UV_SCALE;
        float v1 = (g.uvMinY + g.height) * FONT_UV_SCALE;
        icon->quad.setTexCoords(u0, v0, u1, v1);

        // display size: pixel width/height / 640
        icon->quad.setSize(g.width / FONT_SIZE_SCALE, g.height / FONT_SIZE_SCALE);

        // accumulate total advance: width - kernLeft - kernRight
        totalWidth += g.width - (float)g.kernLeft - (float)g.kernRight;

        // this digit's center X: cursorX + halfWidth + xOffset - kernLeft
        float halfW = g.width * 0.5f;
        float thisCenter = cursorX + (halfW + (float)g.xOffset - (float)g.kernLeft);

        icon->quad.posX = thisCenter / FONT_SIZE_SCALE;
        icon->quad.posY = FONT_DIGIT_BASELINE_Y;

        // advance cursor for next digit: thisCenter + halfWidth - xOffset - kernRight
        cursorX = thisCenter + (halfW - (float)g.xOffset - (float)g.kernRight);
    }

    // center the whole string: shift each digit left by totalWidth/2/640.
    // FUN_10003c25c also reapplies color to each digit on this pass.
    float centerShift = (totalWidth * -0.5f) / FONT_SIZE_SCALE;

    for (TileIcon* icon : digits) {
        icon->quad.posX += centerShift;
        icon->quad.setColor(colorR, colorG, colorB, colorA);
    }

    // store anchor: (totalWidth/2/640, height0/2/640). FUN_10003c25c.
    anchorX = (totalWidth * 0.5f) / FONT_SIZE_SCALE;
    anchorY = (style[0].height * 0.5f) / FONT_SIZE_SCALE;

    // finally, apply pixel-snapped position via setPosition on the existing posX/posY
    setPosition(posX, posY, positionMode);
}

// reconstructed from Ghidra FUN_10003c174.
// no sign char prepended; used by static stat displays (HUD ATK/DEF/HP,
// snag stat cards, tile decorations, tile content magnitude).
void ColorTint::setNumber(int value, int textStyle, int positionMode) {
    freeDigits(digits);

    if (textStyle < 0 || textStyle >= 3) {
        textStyle = 0;
    }

    int digitArr[16];
    int digitCount = decomposeDigits(value, digitArr);

    layoutDigits(digitArr, digitCount, textStyle, positionMode);
}

// reconstructed from Ghidra FUN_10003c5ac.
// prepends a sign-char "digit" (glyph index 10 for non-negative, 11 for
// negative) at the high-order end before laying out; used by the floating
// "+N" / "-N" stat-change tween. textStyle 2 (fire-style digits) skips the
// sign char (those slots are zeroed in FONT_GLYPHS[2]).
void ColorTint::setSignedNumber(int value, int textStyle, int positionMode) {
    freeDigits(digits);

    if (textStyle < 0 || textStyle >= 3) {
        textStyle = 0;
    }

    int digitArr[16];
    int digitCount = decomposeDigits(value, digitArr);

    if (textStyle != 2) {
        // matches binary's `param_2 >> 0x1f | 10`: 10 (plus) for
        // non-negative, 11 (minus) for negative.
        digitArr[digitCount] = (value < 0) ? 11 : 10;
        digitCount++;
    }

    layoutDigits(digitArr, digitCount, textStyle, positionMode);
}

// reconstructed from Ghidra FUN_10003c6e0.
// builds a "lo-hi" range display, with glyph 11 ('-') as the separator.
// matches the binary's linked-list-build pattern: |hi| digits LSB-first,
// then separator, then |lo| digits LSB-first, which when iterated head-
// to-tail and centered by layoutDigits renders left-to-right as
// "<lo>-<hi>". negative inputs are coerced to their absolute value.
void ColorTint::setRangeDisplay(int lo, int hi, int textStyle, int positionMode) {
    freeDigits(digits);

    if (textStyle < 0 || textStyle >= 3) {
        textStyle = 0;
    }

    // decompose each side independently. 16-deep arrays leave plenty of
    // headroom; binary clamps to 16 digits inside decomposeDigits too.
    int loDigits[16];
    int hiDigits[16];
    const int loCount = decomposeDigits(lo, loDigits);
    const int hiCount = decomposeDigits(hi, hiDigits);

    // layout order matches setSignedNumber's convention: digitArr[0] is the
    // last-drawn (rightmost) glyph, digitArr[N-1] is the first-drawn
    // (leftmost). so we want:
    //   digitArr[0..hiCount-1]      = hi digits (LSB first)
    //   digitArr[hiCount]           = 11 (separator '-')
    //   digitArr[hiCount+1..]       = lo digits (LSB first)
    int digitArr[34];
    int digitCount = 0;

    for (int i = 0; i < hiCount; ++i) {
        digitArr[digitCount++] = hiDigits[i];
    }

    digitArr[digitCount++] = 11;   // separator glyph (minus sign)

    for (int i = 0; i < loCount; ++i) {
        digitArr[digitCount++] = loDigits[i];
    }

    layoutDigits(digitArr, digitCount, textStyle, positionMode);
}
