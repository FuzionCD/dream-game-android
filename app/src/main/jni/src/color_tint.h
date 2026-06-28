#pragma once

#include <cstdint>
#include <vector>

// reconstructed from Ghidra:
//   init:           FUN_10003c070
//   draw:           FUN_10003c0ec
//   setNumber:      FUN_10003c174 (+ FUN_10003c25c digit layout)
//   setColor:       FUN_10003c8d8
//   setAlpha:       FUN_10003c948
//   setPosition:    FUN_10003c870
//
// despite the "color tint" name (used in our struct map), this is really a
// digit-display widget. the internal vector holds pointers to TileIcon objects,
// each rendering a glyph from the bitmap font texture, and they all share the
// color stored in this object. FUN_10003c25c (called from setNumber) tears down
// the existing digit list, allocates a new TileIcon per digit, and lays them
// out horizontally. drawing pushes a translation/scale matrix and calls draw on
// each digit via vtable.
//
// struct layout:
// vector<TileIcon*>::begin    (8 bytes, raw pointer)
// vector<TileIcon*>::end      (8 bytes, raw pointer)
// vector<TileIcon*>::cap      (8 bytes, raw pointer)
// posX                        (4 bytes)
// posY                        (4 bytes)
// rgba color (R, G, B, A)     (4 bytes)
// anchorX                     (4 bytes, used for pixel-snap layout)
// anchorY                     (4 bytes)
// scaleX                      (4 bytes, init 1.0)
// scaleY                      (4 bytes, init 1.0)

struct TileIcon;

class ColorTint {
public:
    // FUN_10003c070. resets fields to defaults: empty digit vector, posXY=0,
    // color=white opaque, anchor=0, scale=1. the std::vector-backed digits
    // field is already empty from the implicit member-init, but callers still
    // need to call init() explicitly when resetting a live tint: the PoD scalar
    // fields (color/anchor/scale) only zero out under value-initialization.
    void init();

    // FUN_10003c0ec: bind tex 9, push matrix, translate, optional scale,
    // iterate digit-pointer vector calling vtable[2] (draw), pop matrix
    void draw();

    // FUN_10003c8d8: set RGB (alpha preserved). param is 3-byte array (R, G, B)
    void setColor(uint8_t r, uint8_t g, uint8_t b);

    // FUN_10003c948: set just the alpha byte
    void setAlpha(uint8_t a);

    // FUN_10003c870: set position. mode 0 = direct, mode 1 = pixel-snap delta
    void setPosition(float x, float y, int mode);

    // FUN_10003c174 + FUN_10003c25c: rebuild digit list to display the given int.
    // ports the binary's two-step path (decompose into digits, allocate-or-reuse
    // a TileIcon per digit, set UV/size/pos from FONT_GLYPHS[textStyle], center
    // the whole string by anchor, apply current color/alpha). digit glyph table
    // already extracted in font_glyphs.h (3 styles x 12 entries).
    void setNumber(int value, int textStyle, int positionMode);

    // FUN_10003c5ac: like setNumber, but prepends a sign-char glyph (index 10
    // for non-negative, 11 for negative) so the result reads "+N" / "-N".
    // textStyle 2 (fire-style) skips the sign, matching the binary's gate.
    // used by the floating stat-change tween (push only).
    void setSignedNumber(int value, int textStyle, int positionMode);

    // FUN_10003c6e0: rebuild digit list as "lo-hi", both numbers laid out
    // with a glyph-11 ('-') separator between. binary takes |lo| and |hi|
    // (negative values display as positive). used by UserStatsPanel to
    // show a stat range like "10-25" in the per-row ColorTint.
    void setRangeDisplay(int lo, int hi, int textStyle, int positionMode);

private:
    // shared body of setNumber / setSignedNumber: the per-glyph layout pass
    // over a flat int array of glyph indices.
    void layoutDigits(const int* digits, int digitCount, int textStyle,
                      int positionMode);

public:
    std::vector<TileIcon*> digits;
    float    posX, posY;
    uint8_t  colorR, colorG;
    uint8_t  colorB, colorA;
    float    anchorX, anchorY;
    float    scaleX, scaleY;
};
