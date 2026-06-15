#pragma once

#include <cstddef>
#include <cstdint>

// reconstructed from Ghidra:
//   parser:   FUN_100036990  (BMFont .fnt -> table)
//   loader:   FUN_1000461b4  (Game::loadFonts orchestrator)
//   readers:  FUN_10002fae8  (TextItem::setString, per-char glyph reads)
//             FUN_100030014  (TextItem::draw, texture bind via textureIndex+1)
//
// AngelCode BMFont glyph table. one of these is embedded inline in the
// Game struct at +0x10 (panel font), +0x1220 (dialog font), and +0x2430
// (world font). populated at startup by Game::loadFonts via parseFntFile,
// read at every TextItem render.
//
// distinct from font_glyphs.h's FontGlyph / FONT_GLYPHS, which is a small
// static ROM table extracted from the binary's __DATA segment for
// ColorTint::setNumber's tinted-digit rendering on the UI atlas. this is
// the runtime BMFont parser's output for general text rendering on
// dedicated font atlases (font.png / fontClean.png / fontWorld.png).
//
// total size 0x1210 bytes (4624). layout:
//   +0x0000  textureIndex  (set by parser to font idx 0/1/2; TextItem::draw
//                           binds tex (textureIndex + 1))
//   +0x0004  perTableMul   (per-table cursor-advance multiplier, read by
//                           TextItem::setString and multiplied into every
//                           char's advance. a separate initializer
//                           (FUN_100036958, run by Game::Game on each of the 3
//                           tables before parseFntFile populates them) sets it
//                           to 1.0. load-bearing: Game::create's memset leaves
//                           it at 0, and without the explicit 1.0 write every
//                           char's advance is 0 and they collapse onto each
//                           other in render.)
//   +0x0008..+0x1208       128 per-char entries x 36 bytes (9 floats each)
//   +0x1208  lineHeight    (size divisor, sscanf'd from "common lineHeight=...")
//   +0x120C  base          (baseline subtractor, from same "common ..." line)
//
// per-char entry layout. char `c`'s 9 floats are written at byte offsets
// (8 + 36c)..(43 + 36c). the binary parser uses the index formula
// `param_1[c*9 + 2 .. c*9 + 10]` (where param_1 is `int*`/`float*`), which
// translates to a 36-byte stride starting at byte 8.
//
// fields within char c's slot, byte offsets relative to (entryBytes + 36c):
//   +0x00  uvU              = x / scaleW
//   +0x04  uvV              = y / scaleH
//   +0x08  uvU2             = uvU + width / scaleW
//   +0x0C  uvV2             = uvV + height / scaleH
//   +0x10  sizeW            = width / lineHeight
//   +0x14  sizeH            = height / lineHeight
//   +0x18  xoffsetHalved    = (xoffset / lineHeight) * 0.5
//   +0x1C  yoffsetCenter    = (yoffset / lineHeight) + (sizeH * 0.5)
//                                                    - (base / lineHeight)
//   +0x20  xadvanceHalved   = (xadvance / lineHeight) * 0.5
//
// important: TextItem reads with an 8-byte shift. instead of indexing the
// per-char slot directly (entryBytes + 36c), TextItem accesses a glyph at
// `entryPtr = (uint8_t*)bmfontTable + c*0x24` and reads at offsets
// +0x08, +0x0C, +0x10, +0x14, +0x18, +0x1C, +0x20, +0x24, +0x28. that's
// 8 bytes ahead of the 36-byte stride, so the last two reads (+0x24, +0x28)
// land in entry[c+1]'s first 8 bytes. this is the binary's exact read
// pattern and we preserve it byte-for-byte rather than re-shifting on our
// side. for c=127, the +0x24 / +0x28 reads land in `lineHeight` / `base`;
// DEL is rarely rendered, so the side effect is benign.
struct BMFontTable {
    int     textureIndex;          // +0x0000
    float   perTableMul;           // +0x0004
    uint8_t entryBytes[128 * 36];  // +0x0008..+0x1208 (4608 bytes)
    int     lineHeight;            // +0x1208
    int     base;                  // +0x120C
};

static_assert(sizeof(BMFontTable) == 0x1210,
              "BMFontTable must be exactly 0x1210 bytes "
              "(matches FUN_1000461b4's stride: param_1 + idx*0x1210 + 0x10)");
static_assert(offsetof(BMFontTable, lineHeight) == 0x1208,
              "lineHeight must land at the binary's +0x1208 footer offset");
static_assert(offsetof(BMFontTable, base) == 0x120C,
              "base must land at the binary's +0x120C footer offset");
