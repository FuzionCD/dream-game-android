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
// per-char glyph entry: 9 floats (36 bytes). the parser writes entries[c] and
// TextItem reads it back via entries[(signed char)c]. fields are pre-divided by
// lineHeight / scaleW / scaleH so the reader does no extra math.
struct BMFontEntry {
    float uvU, uvV;          // +0x00, +0x04  atlas top-left UV   (x/scaleW, y/scaleH)
    float uvU2, uvV2;        // +0x08, +0x0C  atlas bottom-right UV
    float sizeW, sizeH;      // +0x10, +0x14  display size        (w/lineHeight, h/lineHeight)
    float xoffsetHalved;     // +0x18         (xoffset / lineHeight) * 0.5
    float yoffsetCenter;     // +0x1C         yoffset/lineHeight + sizeH*0.5 - base/lineHeight
    float xadvanceHalved;    // +0x20         (xadvance / lineHeight) * 0.5
};

static_assert(sizeof(BMFontEntry) == 36, "BMFontEntry must be 9 floats (36 bytes).");

// the binary sign-extends the char before indexing (entries[(signed char)c]),
// so bytes >= 0x80 index BEFORE the array. that's a benign edge case (DEL and
// high bytes are rarely rendered); we keep the signed index so the access stays
// byte-for-byte identical to the binary's `tableBase + (signed char)c*0x24`.
struct BMFontTable {
    int         textureIndex;   // +0x0000
    float       perTableMul;    // +0x0004
    BMFontEntry entries[128];   // +0x0008..+0x1208 (128 * 36 = 4608 bytes)
    int         lineHeight;     // +0x1208
    int         base;           // +0x120C
};

static_assert(sizeof(BMFontTable) == 0x1210,
              "BMFontTable must be exactly 0x1210 bytes "
              "(matches FUN_1000461b4's stride: param_1 + idx*0x1210 + 0x10)");
static_assert(offsetof(BMFontTable, entries) == 0x0008,
              "entries must start at the binary's +0x0008 offset");
static_assert(offsetof(BMFontTable, lineHeight) == 0x1208,
              "lineHeight must land at the binary's +0x1208 footer offset");
static_assert(offsetof(BMFontTable, base) == 0x120C,
              "base must land at the binary's +0x120C footer offset");
