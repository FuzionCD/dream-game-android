#pragma once

// font glyph table extracted from binary __DATA at file offset 0xec6b0
// (PTR_DAT_1000746b0). 3 styles, 12 glyphs per style, 0x1C bytes per glyph.
//
// each glyph entry in the binary is laid out as:
//   +0x00: float uvMinX   (pixels in the 1024x1024 source texture)
//   +0x04: float uvMinY
//   +0x08: float width    (display pixels)
//   +0x0C: float height
//   +0x10: int   kernLeft
//   +0x14: int   kernRight
//   +0x18: int   xOffset
//
// the rendering code (FUN_10003c25c) reads kern/offset fields with a float-to-int
// cast; on arm64 FCVTZS of denormals/NaN yields 0, so the actual stored bit
// patterns for the int-typed fields are effectively zeroed when the binary
// reads them as floats. we just store them as ints directly to skip the dance.
//
// glyph index 10 and 11 in styles 0 and 1 are extra characters (likely "/" and
// ":" or similar) referenced by non-digit text paths. style 2 ends digits at
// index 9; its slots 10/11 are unused/garbage.
//
// styles correspond to the int param passed to ColorTint::setNumber:
//   style 0: small digits (~19px tall) at sheet9 row y=0
//   style 1: large digits (~33px tall) at sheet9 row y=991
//   style 2: medium digits (~35px tall) at sheet9 row y=955

#include <cstdint>

struct FontGlyph {
    float uvMinX;
    float uvMinY;
    float width;
    float height;
    int32_t kernLeft;
    int32_t kernRight;
    int32_t xOffset;
};

// 3 styles, 12 entries each
inline constexpr FontGlyph FONT_GLYPHS[3][12] = {
    // style 0: small digits at y=0
    {
        { 332.0f,   0.0f, 14.0f, 19.0f, 0, 0, 0 },
        { 347.0f,   0.0f, 11.0f, 19.0f, 0, 0, 0 },
        { 359.0f,   0.0f, 14.0f, 19.0f, 0, 0, 0 },
        { 374.0f,   0.0f, 13.0f, 19.0f, 0, 0, 0 },
        { 388.0f,   0.0f, 15.0f, 19.0f, 0, 0, 0 },
        { 404.0f,   0.0f, 14.0f, 19.0f, 0, 0, 0 },
        { 419.0f,   0.0f, 15.0f, 19.0f, 0, 0, 0 },
        { 435.0f,   0.0f, 15.0f, 19.0f, 0, 0, 0 },
        { 451.0f,   0.0f, 14.0f, 19.0f, 0, 0, 0 },
        { 466.0f,   0.0f, 14.0f, 19.0f, 0, 0, 0 },
        { 481.0f,   0.0f, 14.0f, 19.0f, 0, 0, 0 },
        { 496.0f,   0.0f, 10.0f, 19.0f, 0, 0, 0 },
    },
    // style 1: large digits at y=991
    {
        { 641.0f, 991.0f, 24.0f, 33.0f, 0, 0, 0 },
        { 666.0f, 991.0f, 15.0f, 33.0f, 0, 0, 0 },
        { 682.0f, 991.0f, 23.0f, 33.0f, 0, 0, 0 },
        { 706.0f, 991.0f, 23.0f, 33.0f, 0, 0, 0 },
        { 730.0f, 991.0f, 26.0f, 33.0f, 0, 0, 0 },
        { 757.0f, 991.0f, 24.0f, 33.0f, 0, 0, 0 },
        { 782.0f, 991.0f, 23.0f, 33.0f, 0, 0, 0 },
        { 806.0f, 991.0f, 23.0f, 33.0f, 0, 0, 0 },
        { 830.0f, 991.0f, 23.0f, 33.0f, 0, 0, 0 },
        { 854.0f, 991.0f, 23.0f, 33.0f, 0, 0, 0 },
        { 878.0f, 991.0f, 19.0f, 33.0f, 0, 0, 0 },
        { 898.0f, 991.0f, 19.0f, 33.0f, 0, 0, 0 },
    },
    // style 2: fire-styled digits at y=955
    {
        { 641.0f, 955.0f, 21.0f, 35.0f, 0, 0, 0 },
        { 663.0f, 955.0f, 11.0f, 35.0f, 0, 0, 0 },
        { 675.0f, 955.0f, 22.0f, 35.0f, 0, 0, 0 },
        { 698.0f, 955.0f, 22.0f, 35.0f, 0, 0, 0 },
        { 721.0f, 955.0f, 23.0f, 35.0f, 0, 0, 0 },
        { 745.0f, 955.0f, 21.0f, 35.0f, 0, 0, 0 },
        { 767.0f, 955.0f, 23.0f, 35.0f, 0, 0, 0 },
        { 791.0f, 955.0f, 24.0f, 35.0f, 0, 0, 0 },
        { 816.0f, 955.0f, 21.0f, 35.0f, 0, 0, 0 },
        { 838.0f, 955.0f, 22.0f, 35.0f, 0, 0, 0 },
        // slots 10/11 unused for style 2
        { 0.0f,     0.0f,  0.0f,  0.0f, 0, 0, 0 },
        { 0.0f,     0.0f,  0.0f,  0.0f, 0, 0, 0 },
    },
};

// constants from FUN_10003c25c
inline constexpr float FONT_UV_SCALE  = 1.0f / 1024.0f;  // DAT_10005a2d0
inline constexpr float FONT_SIZE_SCALE = 640.0f;          // DAT_10005a2d4
inline constexpr float FONT_DIGIT_BASELINE_Y = -0.003125f;  // hardcoded in FUN_10003c25c
