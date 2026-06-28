#pragma once

#include "quad.h"
#include "title_menu.h"          // TileIcon (the per-glyph quad type)
#include <cstdint>
#include <vector>

// reconstructed from Ghidra. the binary uses this widget class as the
// text+hit-test primitive inside every modal panel slot: level-up panel
// (2 per stat slot, 2 per perk slot), encounter reward, score panel,
// events bar, etc. each instance owns a glyph vector + 4 parallel
// per-glyph metadata vectors, plus cached bbox state.
//
//   ctor:                   FUN_10004c014
//   dtor:                   FUN_10004c058
//   draw:                   FUN_10004c09c
//   addGlyph(uv, off, h, l): FUN_10004c310
//   setSize:                FUN_10004c990
//   setPosition:            FUN_10004c4f8
//   layout (internal):      FUN_10004c150
//   getWidth:               FUN_10004c61c
//   widthAccum (glyph walk): FUN_10004c658
//   setColor:               FUN_10004c794
//   setAlpha:               FUN_10004c84c
//   contains (bbox test):   FUN_10004c8cc
//   getLeftX:               FUN_10004c93c
//
// the 4 parallel vectors (sizeMode, glyphOffset, glyphScale, lineIndex)
// have one entry per glyph, written together by addGlyph(). they drive
// the layout() pass that re-positions every glyph whenever setPosition
// or setSize is called.

struct GlyphOffset {
    float dx;
    float dy;
};

// FUN_10004c658 returns two floats (s0 and s1), even though Ghidra's decompile
// flattens it to `return fVar6`. the function-exit disassembly zeroes both v0
// and v1 on the empty-list branch, proving a (advance, height) pair return. the
// caller pattern in FUN_10002c230 saves v1 across two calls and feeds them into
// Label::setSize.
struct GlyphRunMetrics {
    float advanceWidth;   // sum of (v3.u - v0.u) * scale / 640 over filter-matched
                          // glyphs with mode 0 or 2 (the layout-advancing
                          // glyphs: letters in text, corners/edges in 9-slice).
                          // returned in s0 of the binary.
    float heightTotal;    // sum of natural heights (or override when >=0) over
                          // filter-matched glyphs with mode 1 or 2. for a
                          // 9-slice frame called with filter (line_i, run_i),
                          // this sums the top-row + bottom-row natural heights
                          // (the filter passes "line OR run match" and a row's
                          // corner/edge glyphs are mode 1 or 2). that sum,
                          // subtracted from cachedSize1 in layoutGlyphs, is what
                          // makes the middle row fill the panel's remaining
                          // vertical space. returned in s1 of the binary;
                          // Ghidra's decomp drops this return entirely.
};

class Label {
public:
    // FUN_10004c014. zero all 5 vectors + cached state. scale = 1024.0
    // (= the pixel-per-unit divisor used inside layout() / widthAccum).
    void init();

    // FUN_10004c058. destroy the 5 vectors. C++ default dtor handles
    // this for us, but we expose a method to match the binary's name.
    void destroy();

    // FUN_10004c09c. iterate glyphs calling Quad::draw (vtable[2]) on
    // each. when mirrored, wraps the loop in glPushMatrix + glRotatef.
    void draw();

    // FUN_10004c310. push a new glyph onto all 5 parallel per-glyph vectors,
    // set its UV from the caller-provided (uvOriginPx, uvSizePx), and run
    // layoutGlyphs to re-position the entire run.
    //
    //   overrideHeight: pushed onto overrideHeights[]; -1 (or any negative
    //                   value) leaves the height as the glyph's natural;
    //                   >=0 overrides naturalHeight inside layoutGlyphs.
    //   uvOriginPx:     top-left UV in pixel units (gets divided by scale)
    //   uvSizePx:       UV width/height span (also pixel units)
    //   sizeMode:       0..3, picks one of the 4 width/height computations
    //                   in layoutGlyphs.
    //   offset:         (dx, dy) added via Quad::addVertexOffset after sizing.
    //
    // the new glyph's line index is read from pendingLineIndex (caller is
    // expected to set that field before each addGlyph call when lines change).
    void addGlyph(float overrideHeight,
                  const float uvOriginPx[2], const float uvSizePx[2],
                  uint32_t sizeMode,
                  const GlyphOffset& offset);

    // FUN_10004c990. set the cached size + mark widthValid, then trigger
    // layoutGlyphs so each glyph's quad gets resized + repositioned.
    void setSize(float width, float height);

    // FUN_10004c4f8. set the cached top-left + walk every glyph updating
    // its Quad.posX / .posY to (leftX + glyphOffset.dx, topY + glyphOffset.dy).
    void setPosition(float leftX, float topY);

    // FUN_10004c150, internal layout pass. for each glyph, recomputes
    // per-glyph Quad.size based on sizeModes[i] (4 distinct modes), then
    // applies offsets[i] via Quad::addVertexOffset. tail-calls setPosition
    // so glyph positions are re-anchored.
    void layoutGlyphs(float widthInput, float heightInput);

    // FUN_10004c61c. cached width or, when invalid, recomputed via
    // widthAccum over the glyph vector.
    float getWidth();

    // FUN_10004c658, measure the natural extent of a (sub)run of glyphs.
    // returns (advanceWidth, heightTotal). pass (-1, -1) to disable the filter
    // (match every glyph). see GlyphRunMetrics above for the exact axis-level
    // filter semantics.
    GlyphRunMetrics measureGlyphRun(int filterLine, int filterRun) const;

    // FUN_10004c93c. when mirrored: returns -(topY + width). otherwise
    // returns the cached leftX.
    float getLeftX();

    // FUN_10004c794. write a 4-byte color to each glyph's Quad vertices
    // via Quad::setColor.
    void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // FUN_10004c84c. write the alpha byte across all glyph vertices.
    void setAlpha(uint8_t a);

    // FUN_10004c8cc. bbox test against the cached (leftX, topY, width,
    // height). touchX / touchY must already be in the label's local
    // coordinate space (caller subtracts the panel's anchor).
    bool contains(float touchX, float touchY);

    // height counterpart to getWidth(). cachedWidth and cachedWidthMirror
    // hold (width, height) but the binary's mirror flag swaps which
    // field holds which; getWidth + getHeight handle the swap.
    float getHeight();

    // ---- byte-exact field layout ----

    std::vector<TileIcon>          glyphs;          // one TileIcon per visible character
    std::vector<uint32_t>          sizeModes;       // one mode (0..3) per glyph
    std::vector<GlyphOffset>       offsets;         // per-glyph (dx, dy) vertex offset
    std::vector<float>             overrideHeights; // per-glyph height override (>=0 wins over natural)
    std::vector<int32_t>           lineIndices;     // per-glyph line / paragraph index

    int32_t   pendingLineIndex; // caller sets before addGlyph; pushed into
                                //               lineIndices[] for the new glyph. line breaks
                                //               between calls = bump this between addGlyph calls.
    float     scale;            // ctor inits 1024.0  (pixels-per-unit divisor)
    float     cachedSize0;      // (width when !mirrored, height when mirrored)
    float     cachedSize1;      // (height when !mirrored, width when mirrored)
    bool      widthValid;       // "cache is current" gate; getWidth recomputes if false
    float     leftX;            // cached top-left X (setPosition writes)
    float     topY;             // cached top-left Y (setPosition writes)
    bool      mirrored;         // flag; rotates layout + flips X coords
};
