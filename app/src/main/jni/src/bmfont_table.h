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
// Game struct three times (panel font, dialog font, and world font).
// populated at startup by Game::loadFonts via parseFntFile, read at every
// TextItem render.
//
// distinct from font_glyphs.h's FontGlyph / FONT_GLYPHS, which is a small
// static ROM table extracted from the binary's __DATA segment for
// ColorTint::setNumber's tinted-digit rendering on the UI atlas. this is
// the runtime BMFont parser's output for general text rendering on
// dedicated font atlases (font.png / fontClean.png / fontWorld.png).
//
// total size 0x1210 bytes (4624). layout:
// textureIndex  (set by parser to font idx 0/1/2; TextItem::draw
//                           binds tex (textureIndex + 1))
// perTableMul   (per-table cursor-advance multiplier, read by
//                           TextItem::setString and multiplied into every
//                           char's advance. a separate initializer
//                           (FUN_100036958, run by Game::Game on each of the 3
//                           tables before parseFntFile populates them) sets it
//                           to 1.0. load-bearing: Game::create's memset leaves
//                           it at 0, and without the explicit 1.0 write every
//                           char's advance is 0 and they collapse onto each
//                           other in render.)
// 128 per-char entries x 36 bytes (9 floats each)
// lineHeight    (size divisor, sscanf'd from "common lineHeight=...")
// base          (baseline subtractor, from same "common ..." line)
//
// per-char glyph entry: 9 floats (36 bytes). the parser writes entries[c] and
// TextItem reads it back via entries[(signed char)c]. fields are pre-divided by
// lineHeight / scaleW / scaleH so the reader does no extra math.
struct BMFontEntry {
    float uvU, uvV;          // atlas top-left UV     (x/scaleW, y/scaleH)
    float uvU2, uvV2;        // atlas bottom-right UV
    float sizeW, sizeH;      // display size          (w/lineHeight, h/lineHeight)
    float xoffsetHalved;     // (xoffset / lineHeight) * 0.5
    float yoffsetCenter;     // yoffset/lineHeight + sizeH*0.5 - base/lineHeight
    float xadvanceHalved;    // (xadvance / lineHeight) * 0.5
};

// the binary sign-extends the char before indexing (entries[(signed char)c]),
// so bytes >= 0x80 index BEFORE the array. that's a benign edge case (DEL and
// high bytes are rarely rendered); we keep the signed index so the access stays
// byte-for-byte identical to the binary's `tableBase + (signed char)c*0x24`.
struct BMFontTable {
    int         textureIndex;
    float       perTableMul;
    BMFontEntry entries[128];   // (128 * 36 = 4608 bytes)
    int         lineHeight;
    int         base;
};
