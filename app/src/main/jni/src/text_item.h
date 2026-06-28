#pragma once

#include "title_menu.h"   // for TileIcon
#include <cstdint>
#include <string>
#include <vector>

struct BMFontTable;

// reconstructed from Ghidra:
//   init():                FUN_10002fa08
//   init(glyphTable):      FUN_10002fa50
//   ~TextItem():           FUN_10002faa0
//   setString:             FUN_10002fae8
//   draw():                FUN_100030014
//   applyColor():          FUN_100030144
//   setAlpha(a):           FUN_1000301fc
// internal helpers (file-static in our port):
//   parseInlineTag:   FUN_10002fe60   (handles {A}/{C}/{D}/{H}/{X} tags)
//   emitInlineIcon:   FUN_100030204   (single inline-icon glyph emit)
//
// 0x88-byte text-rendering widget. each instance owns:
//   - the displayed text as std::string (storedText)
//   - a parallel std::vector<char> (charBuffer) mirroring the rendered chars
//     (used for diff detection on subsequent setString calls; only chars
//     that change rebuild their glyph)
//   - a std::vector<TileIcon> (glyphVec) holding one TileIcon per rendered
//     character, plus a trailing tail used as outline pass for the
//     {C}/{X} tags
//   - assorted layout state (cursor advance, max char height, color, etc.)
//
// all glyph data comes from the per-instance glyph table glyphTablePtr (set
// by init(ptr) or by panel-side field writes). format of that table:
//   int textureIndex   (used by draw to bind tex *ptr + 1)
//   float perTableMul  (multiplier applied to char-advance)
//   per-char entries indexed by ASCII code, stride 0x24 bytes:
//     UV min (2 floats)
//     UV max (2 floats)
//     display size (2 floats)
//     vertex offset (2 floats)
//     char advance (1 float)
//
// fields below mirror the binary layout byte-for-byte. std::string and
// std::vector on libc++ aarch64 are exactly 24 bytes each, matching the
// binary's per-field offsets.

struct TextItem {
    // ---- public methods (the 7 binary entry points) ----

    // FUN_10002fa08. zero core fields, set scaleX/Y=1.0, color=white,
    // spaceMultiplier=2.0. C++ ctors of std::string / std::vector<char> /
    // std::vector<TileIcon> handle the RAII members; this method
    // only touches the trailing scalar fields.
    void init();

    // FUN_10002fa50. same as init(), but stores `glyphTable` in glyphTablePtr
    // for use by setString / draw. used by panels that own a per-panel
    // glyph table (e.g. DetailPanel passes the panel-font glyph table).
    void init(const BMFontTable* glyphTable);

    // C++ default destructor handles all three RAII members
    // (storedText / charBuffer / glyphVec) in reverse declaration order,
    // matching FUN_10002faa0's manual sequence.

    // FUN_10002fae8. populate glyph vector from the input string.
    //   - early-out if `s` matches storedText already
    //   - if `len` < 0, computes via strlen(s)
    //   - emits one TileIcon per char, looking up UV/size/offset from
    //     the glyphTablePtr glyph table indexed by char code
    //   - inline-tag pairs `{X}` are emitted as inline icons via
    //     parseInlineTag (FUN_10002fe60) instead of letter glyphs
    //   - applies current color/alpha to each glyph as it lands
    //   - stores final cursor X into renderedWidth, max ascender
    //     into maxCharHeight
    void setString(const char* s, int len = -1);

    // FUN_100030014. push matrix, translate(posX,posY), scale, rotate;
    // bind tex `glyphTablePtr->textureIndex + 1`; iterate glyph vector calling
    // Quad::draw() on each via vtable[2]; if outlineGlyphCount > 0,
    // bind tex 9 and iterate the outline tail in reverse; pop matrix.
    void draw();

    // FUN_100030144. walk primary glyphs (glyphCount) applying the
    // current rgba via Quad::setColor, then walk outline tail
    // (outlineGlyphCount) in reverse applying the current alpha byte
    // (alpha) via Quad::setAlpha. used after rgba/alpha mutations.
    void applyColor();

    // FUN_1000301fc. write the alpha byte of rgba, call applyColor.
    void setAlpha(uint8_t a);

    // ---- byte-exact field layout ----

    std::string         storedText;        // current displayed string
    std::vector<char>   charBuffer;        // rendered-char history
    std::vector<TileIcon> glyphVec;        // per-char TileIcon storage
    int64_t             glyphCount;        // primary glyph count
    int64_t             outlineGlyphCount; // trailing outline glyph count
    const BMFontTable*  glyphTablePtr;     // per-instance glyph table
    float               posX;
    float               posY;
    float               scaleX;            // init = 1.0
    float               scaleY;            // init = 1.0
    float               rotation;
    union {
        uint32_t        rgba;              // init = 0xFFFFFFFF
        struct {
            uint8_t     colorR;
            uint8_t     colorG;
            uint8_t     colorB;
            uint8_t     alpha;             // setAlpha target
        };
    };
    float               renderedWidth;     // setString output
    float               maxCharHeight;     // setString output
    float               spaceMultiplier;   // init = 2.0
};
