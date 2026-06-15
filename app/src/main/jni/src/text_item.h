#pragma once

#include "title_menu.h"   // for TileIcon
#include <cstdint>
#include <string>
#include <vector>

// reconstructed from Ghidra:
//   init():                FUN_10002fa08
//   init(int* glyphTable): FUN_10002fa50
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
//   - the displayed text as std::string at +0x00
//   - a parallel std::vector<char> at +0x18 mirroring the rendered chars
//     (used for diff detection on subsequent setString calls; only chars
//     that change rebuild their glyph)
//   - a std::vector<TileIcon> at +0x30 holding one TileIcon per rendered
//     character, plus a trailing tail used as outline pass for the
//     {C}/{X} tags
//   - assorted layout state (cursor advance, max char height, color, etc.)
//
// all glyph data comes from the per-instance table at +0x58 (set by
// init(ptr) or by panel-side field writes). format of that table:
//   +0x00: int textureIndex   (used by draw to bind tex *ptr + 1)
//   +0x04: float perTableMul  (multiplier applied to char-advance)
//   +0x08..: per-char entries indexed by ASCII code, stride 0x24 bytes:
//     +0x08..+0x0F : UV min (2 floats)
//     +0x10..+0x17 : UV max (2 floats)
//     +0x18..+0x1F : display size (2 floats)
//     +0x20..+0x27 : vertex offset (2 floats)
//     +0x28        : char advance (1 float)
//
// fields below mirror the binary layout byte-for-byte. std::string and
// std::vector on libc++ aarch64 are exactly 24 bytes each, matching the
// binary's per-field offsets.

struct TextItem {
    // ---- public methods (the 7 binary entry points) ----

    // FUN_10002fa08. zero core fields, set scaleX/Y=1.0, color=white,
    // spaceMultiplier=2.0. C++ ctors of std::string / std::vector<char> /
    // std::vector<TileIcon> handle the +0x00..+0x48 region; this method
    // only touches the +0x48..+0x88 fields.
    void init();

    // FUN_10002fa50. same as init(), but stores `glyphTable` at +0x58
    // for use by setString / draw. used by panels that own a per-panel
    // glyph table (e.g. DetailPanel passes game+0x10).
    void init(int* glyphTable);

    // C++ default destructor handles all three RAII members
    // (storedText / charBuffer / glyphVec) in reverse declaration order,
    // matching FUN_10002faa0's manual sequence.

    // FUN_10002fae8. populate glyph vector from the input string.
    //   - early-out if `s` matches storedText already
    //   - if `len` < 0, computes via strlen(s)
    //   - emits one TileIcon per char, looking up UV/size/offset from
    //     the +0x58 glyph table indexed by char code
    //   - inline-tag pairs `{X}` are emitted as inline icons via
    //     parseInlineTag (FUN_10002fe60) instead of letter glyphs
    //   - applies current color/alpha to each glyph as it lands
    //   - stores final cursor X into renderedWidth (+0x78), max ascender
    //     into maxCharHeight (+0x7C)
    void setString(const char* s, int len = -1);

    // FUN_100030014. push matrix, translate(posX,posY), scale, rotate;
    // bind tex `*glyphTablePtr + 1`; iterate glyph vector calling
    // Quad::draw() on each via vtable[2]; if outlineGlyphCount > 0,
    // bind tex 9 and iterate the outline tail in reverse; pop matrix.
    void draw();

    // FUN_100030144. walk primary glyphs (count at +0x48) applying the
    // current rgba (+0x74) via Quad::setColor, then walk outline tail
    // (count at +0x50) in reverse applying the current alpha byte
    // (+0x77) via Quad::setAlpha. used after rgba/alpha mutations.
    void applyColor();

    // FUN_1000301fc. write +0x77 (alpha byte of rgba), call applyColor.
    void setAlpha(uint8_t a);

    // ---- byte-exact field layout ----

    std::string         storedText;        // +0x00..+0x17  current displayed string
    std::vector<char>   charBuffer;        // +0x18..+0x2F  rendered-char history
    std::vector<TileIcon> glyphVec;        // +0x30..+0x47  per-char TileIcon storage
    int64_t             glyphCount;        // +0x48         primary glyph count
    int64_t             outlineGlyphCount; // +0x50         trailing outline glyph count
    int*                glyphTablePtr;     // +0x58         per-instance glyph table
    float               posX;              // +0x60
    float               posY;              // +0x64
    float               scaleX;            // +0x68         init = 1.0
    float               scaleY;            // +0x6C         init = 1.0
    float               rotation;          // +0x70
    union {
        uint32_t        rgba;              // +0x74         init = 0xFFFFFFFF
        struct {
            uint8_t     colorR;            // +0x74
            uint8_t     colorG;            // +0x75
            uint8_t     colorB;            // +0x76
            uint8_t     alpha;             // +0x77         setAlpha target
        };
    };
    float               renderedWidth;     // +0x78         setString output
    float               maxCharHeight;     // +0x7C         setString output
    float               spaceMultiplier;   // +0x80         init = 2.0
    uint8_t             pad84[4];          // +0x84..+0x87
};

static_assert(sizeof(TextItem) == 0x88,
              "TextItem must be exactly 0x88 bytes, matches the binary's "
              "operator_new(0x88) site and the embed offsets in DialogPanel "
              "(textLines[3] at +0xCB8 stride 0x88) and DetailPanel "
              "(textCenter at +0xD00 + textLines[3] at +0xD88 stride 0x88).");
