#include "label.h"
#include <GLES/gl.h>
#include <cmath>
#include <cstring>

// FUN_10004c014, Label widget ctor.
//
// the binary memsets the cached-state block then sets scale = 1024.0f.
// our embedded members default-init by the C++ aggregate ctor (vectors
// -> empty, ints/floats -> garbage), so init() zeros the cached state
// to keep the binary's clean-slate semantics.
void Label::init() {
    glyphs.clear();
    sizeModes.clear();
    offsets.clear();
    overrideHeights.clear();
    lineIndices.clear();

    pendingLineIndex  = 0;
    scale             = 1024.0f;
    cachedSize0       = 0.0f;
    cachedSize1       = 0.0f;
    widthValid        = false;
    pad89[0] = pad89[1] = pad89[2] = 0;
    leftX             = 0.0f;
    topY              = 0.0f;
    mirrored          = false;
    pad95[0] = pad95[1] = pad95[2] = 0;
}

// FUN_10004c058, Label widget dtor.
//
// clears the 5 vectors in reverse declaration order. C++ default
// destruction already does this when Label (or its embedding parent)
// goes out of scope.
void Label::destroy() {
    lineIndices.clear();
    overrideHeights.clear();
    offsets.clear();
    sizeModes.clear();
    glyphs.clear();
}

// FUN_10004c658, measure the natural extent of a glyph run.
//
// walk every glyph; returns (advanceWidth, heightTotal). heightTotal (s1) is
// not a simple sum but a per-branch register that overwrites or accumulates
// depending on filter args and the glyph's mode. Ghidra's `return fVar6`
// drops the s1 return; the exit's empty-list branch (`movi v0,#0; movi
// v1,#0`) proves a paired return.
//
// branch summary (verified vs asm at 0x10004c6f8 .. 0x10004c790):
//
//   filterOpen (filterLine == -1 and filterRun == -1):
//     s1 = current glyph's vNat  (overwrite, no accumulation)
//     s0 += per-glyph U-span when mode is 0 or 2
//   else if filter passes (lineMatch || runMatch):
//     mode 1 or 2: s1 += (overrideHeights[i] when >= 0, else vNat)
//     mode 0 or 3: s1 unchanged (carry forward s3)
//     s0 += per-glyph U-span when mode is 0 or 2
//   else (no filter match):
//     s1 unchanged
//
//   per-glyph vNat = (v3.v - v0.v) * scale / 640.
//   per-glyph U-span = (v3.u - v0.u) * scale / 640.
//
// runIdx resets to 0 on a line-index transition, otherwise increments.
// passing (-1, -1) reads the last glyph's vNat into heightTotal (the per-Label
// single-glyph height, since all glyphs on a 9-slice row share a V-span).
// passing (lineIdx, runIdx) sums per-line heights from stretchable (mode 1/2)
// glyphs.
static GlyphRunMetrics measureGlyphRunImpl(const Label& lbl,
                                           int filterLine, int filterRun) {
    GlyphRunMetrics out = {0.0f, 0.0f};

    if (lbl.glyphs.empty()) {
        return out;
    }

    constexpr float DENOM = 640.0f;   // DAT_10005a54c (= "pixels per unit")

    // matches binary's `cmn w10, #1; b.eq` gate where w10 = filterLine &
    // filterRun. for our typed-int args, (-1, -1) is the only call site that
    // exercises this branch.
    bool filterOpen = (filterLine == -1) && (filterRun == -1);

    float s1 = 0.0f;                  // running s3 carry-forward
    int   runIdx = -1;
    size_t count = lbl.glyphs.size();

    for (size_t i = 0; i < count; ++i) {
        int lineIdx = lbl.lineIndices[i];

        if (i == 0 || lineIdx == lbl.lineIndices[i - 1]) {
            runIdx += 1;
        } else {
            runIdx = 0;
        }

        const Quad& q = lbl.glyphs[i].quad;

        // always compute vNat (used in both branches)
        float vNat = (q.vertices[3].v - q.vertices[0].v) * lbl.scale / DENOM;
        uint32_t mode = lbl.sizeModes[i];

        bool lineMatch   = (lineIdx == filterLine);
        bool runMatch    = (runIdx == filterRun);
        bool filterMatch = filterOpen || lineMatch || runMatch;

        // s1 (heightTotal), branch-specific:
        if (filterOpen) {
            // overwrite: s1 keeps just this glyph's vNat
            s1 = vNat;
        } else if (filterMatch && (mode == 1u || mode == 2u)) {
            // accumulate, with overrideHeights override path
            float oh = lbl.overrideHeights[i];
            s1 = s1 + ((oh >= 0.0f) ? oh : vNat);
        }
        // else: pass-through (s1 carries forward unchanged)

        // s0 (advanceWidth), gated by filter and mode is 0 or 2
        if (filterMatch && ((mode | 2u) == 2u)) {
            float u = q.vertices[3].u - q.vertices[0].u;
            out.advanceWidth += (u * lbl.scale) / DENOM;
        }
    }

    out.heightTotal = s1;
    return out;
}

GlyphRunMetrics Label::measureGlyphRun(int filterLine, int filterRun) const {
    return measureGlyphRunImpl(*this, filterLine, filterRun);
}

// FUN_10004c61c, getWidth.
//
// cachedSize0 holds width when not mirrored, cachedSize1 when mirrored.
// when widthValid is false, recompute via the measureGlyphRun walk's
// advanceWidth.
float Label::getWidth() {

    if (!widthValid) {
        return measureGlyphRunImpl(*this, -1, -1).advanceWidth;
    }

    return mirrored ? cachedSize1 : cachedSize0;
}

// getHeight mirrors getWidth: the two cached fields swap roles based on
// mirrored. no glyph-walk fallback, since height isn't accumulated like
// width is.
float Label::getHeight() {
    return mirrored ? cachedSize0 : cachedSize1;
}

// FUN_10004c93c, getLeftX.
//
// natural orientation: cached leftX.
// mirrored orientation: -(leftX) - getWidth() (reflects across origin).
float Label::getLeftX() {

    if (!mirrored) {
        return leftX;
    }

    return -leftX - getWidth();
}

// FUN_10004c8cc, contains (bbox hit-test).
//
// standard rectangle test against (leftX, topY) + (width, height).
// touchX/touchY are in the label's local coordinate space; caller must
// subtract the panel's anchor first.
bool Label::contains(float touchX, float touchY) {
    float left   = getLeftX();
    float width  = getWidth();
    float top    = topY;
    float height = getHeight();

    // binary's FUN_10004c8cc uses b.hi (strict greater) on the right/bottom
    // edges, so the inclusive-on-both-sides bbox matches: leftX <= x <=
    // leftX + width AND topY <= y <= topY + height.
    if (touchX < left) {
        return false;
    }

    if (touchX > left + width) {
        return false;
    }

    if (touchY < top) {
        return false;
    }

    return touchY <= top + height;
}

// FUN_10004c794, setColor.
//
// write RGBA bytes into the .color field of every vertex on every glyph
// quad. matches the binary's per-glyph FUN_10000826c loop.
void Label::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {

    for (TileIcon& tile : glyphs) {
        tile.quad.setColor(r, g, b, a);
    }
}

// FUN_10004c84c, setAlpha.
//
// write only the alpha byte into every vertex on every glyph quad.
void Label::setAlpha(uint8_t a) {

    for (TileIcon& tile : glyphs) {
        tile.quad.setAlpha(a);
    }
}

// FUN_10004c09c, draw.
//
// when mirrored, wraps the loop in a glPushMatrix + glRotatef(90, Z).
// then iterates the glyphs vector calling Quad::draw on each.
void Label::draw() {

    if (mirrored) {
        glPushMatrix();
        // DAT_10005a544 = 90.0f rotation angle around the Z axis.
        glRotatef(90.0f, 0.0f, 0.0f, 1.0f);
    }

    for (TileIcon& tile : glyphs) {
        tile.quad.draw();
    }

    if (mirrored) {
        glPopMatrix();
    }
}

// FUN_10004c310, addGlyph.
//
// push a fresh glyph onto all 5 per-glyph vectors in lockstep, set the
// new TileIcon's UV from (uvOriginPx, uvSizePx) divided by label.scale,
// then run layoutGlyphs to re-position the whole run. lineIndex for the
// new glyph comes from pendingLineIndex.
//
// the trailing layoutGlyphs() call's float arg is unused. Ghidra shows
// `param_2 = u1` getting passed but the value is never read inside
// layoutGlyphs (it confuses s1 from measureGlyphRun for that arg).
void Label::addGlyph(float overrideHeight,
                            const float uvOriginPx[2],
                            const float uvSizePx[2],
                            uint32_t sizeMode,
                            const GlyphOffset& offset) {

    // push to all 5 parallel vectors. std::vector::push_back handles
    // the cap-vs-end check + grow that the binary inlines per-vector.
    glyphs.push_back(TileIcon());
    sizeModes.push_back(sizeMode);
    offsets.push_back(offset);
    overrideHeights.push_back(overrideHeight);
    lineIndices.push_back(pendingLineIndex);

    // convert pixel-space UV into normalized UV by dividing by label.scale,
    // then apply to the new (last) glyph's Quad.
    float u0 = uvOriginPx[0] / scale;
    float v0 = uvOriginPx[1] / scale;
    float u1 = uvSizePx[0]   / scale + u0;
    float v1 = uvSizePx[1]   / scale + v0;
    glyphs.back().quad.setTexCoords(u0, v0, u1, v1);

    // run layout. binary passes the right-edge UV as param_2, mirrored here
    // so the rolling-natural-width chain inside layoutGlyphs matches.
    layoutGlyphs(/*widthInput=*/u1, /*heightInput=*/u1);
}

// FUN_10004c990, setSize.
//
// sets widthValid (so getWidth uses the cached value rather than walking
// glyphs), then stashes (width, height) into cachedSize0 / cachedSize1
// with the mirror-flag-driven swap. tail-calls layoutGlyphs to resize +
// re-position each glyph quad against the new bbox.
void Label::setSize(float width, float height) {
    widthValid = true;

    if (mirrored) {
        cachedSize0 = height;
        cachedSize1 = width;
    } else {
        cachedSize0 = width;
        cachedSize1 = height;
    }

    layoutGlyphs(width, height);
}

// FUN_10004c150, layoutGlyphs.
//
// per-glyph resize + offset application. for each glyph:
//   - figure out its (lineIdx, runIdx); runIdx increments inside a line
//     and resets to 0 on a line-index transition.
//   - call measureGlyphRun(lineIdx, runIdx) which returns (advanceWidth,
//     heightTotal), the two register-returned floats (s0, s1) that
//     Ghidra's decomp drops s1 from. heightTotal is the sum-of-natural-
//     heights from glyphs matching the (line or run) filter at modes
//     1/2; for a 9-slice this equals top-row + bottom-row natural
//     height, which is what makes the middle row fill the remaining
//     space.
//   - naturalHeight = (v3.v - v0.v) * scale / 640. overridden by
//     overrideHeights[i] when >= 0.
//   - if sizeModes[i] < 4, compute final (w, h) by mode:
//       mode 0: (naturalWidth,                 cachedSize1 - heightTotal)
//       mode 1: (cachedSize0 - advanceWidth,    naturalHeight)
//       mode 2: (naturalWidth,                  naturalHeight)
//       mode 3: (cachedSize0 - advanceWidth,    cachedSize1 - heightTotal)
//     then call Quad::setSize on the glyph.
//   - call Quad::addVertexOffset(offsets[i].dx, offsets[i].dy).
//
// Ghidra's decomp shows a `param_2` carry-forward (param_2 = previous
// glyph's naturalWidth). that's a decomp artifact: the s1 register is the
// heightTotal return from measureGlyphRun, not a phantom carry. asm at
// 0x10004c1f8 (bl 0x10004c658) returns (s0, s1); 0x10004c240 (fsub s0, s10,
// s1) uses s1 directly as heightTotal. param_2's "reassignment" inside the
// loop is just the C variable being reused as naturalWidth, never read as a
// height carry.
//
// after the per-glyph loop, calls setPosition(leftX, topY) to write every
// glyph's posX/posY from the now-correct sizes.
void Label::layoutGlyphs(float /*widthInput*/, float /*heightInput*/) {
    constexpr float DENOM = 640.0f;   // DAT_10005a548

    if (glyphs.empty()) {
        setPosition(leftX, topY);
        return;
    }

    int rollingRunIdx = -1;

    for (size_t i = 0; i < glyphs.size(); ++i) {
        int lineIdx;

        if (i == 0) {
            lineIdx       = lineIndices[0];
            rollingRunIdx = 0;
        } else {

            if (lineIndices[i] == lineIndices[i - 1]) {
                rollingRunIdx += 1;
            } else {
                rollingRunIdx = 0;
            }

            lineIdx = lineIndices[i];
        }

        GlyphRunMetrics runMetrics = measureGlyphRunImpl(*this, lineIdx, rollingRunIdx);
        Quad& q = glyphs[i].quad;

        // natural width/height come from the glyph's UV-span (vertices[3].u -
        // vertices[0].u and vertices[3].v - vertices[0].v). the binary reads
        // offsets +0x50 / +0x14 / +0x54 / +0x18, which are the v3.u/v0.u/v3.v/v0.v
        // fields, not the second vertex block.
        float vT = q.vertices[3].v - q.vertices[0].v;
        float naturalHeight = (vT * scale) / DENOM;
        float oh = overrideHeights[i];

        if (oh >= 0.0f) {
            naturalHeight = oh;
        }

        uint32_t mode = sizeModes[i];

        if (mode < 4u) {
            float final0 = cachedSize0 - runMetrics.advanceWidth;
            float final1 = cachedSize1 - runMetrics.heightTotal;

            float uT = q.vertices[3].u - q.vertices[0].u;
            float naturalWidth = (uT * scale) / DENOM;

            switch (mode) {
                case 0: final0 = naturalWidth; break;
                case 1: final1 = naturalHeight; break;
                case 2: final0 = naturalWidth; final1 = naturalHeight; break;
                case 3: break;
            }

            q.setSize(final0, final1);
        }

        q.addVertexOffset(offsets[i].dx, offsets[i].dy);
    }

    setPosition(leftX, topY);
}

// FUN_10004c4f8, setPosition.
//
// orient + position every glyph against the new (x, y) origin.
//
// natural orientation (mirrored == false):
//   leftX = x, topY = y; the cursor walks rightward by each glyph's
//   width.height and wraps to (leftX, currentY + lastGlyphHeight) on a
//   line-index transition.
//
// mirrored orientation (mirrored == true):
//   leftX = y (yes, the binary swaps); topY = -x - getWidth(). then the
//   same glyph walk runs against this re-anchored origin.
//
// each glyph's quad.posX/.posY are set to the current cursor offset by
// half the glyph's natural width/height (centered placement).
void Label::setPosition(float x, float y) {
    float cursorX;
    float cursorY;

    if (!mirrored) {
        leftX   = x;
        topY    = y;
        cursorX = x;
        cursorY = y;
    } else {
        leftX   = y;
        topY    = -x - getWidth();
        cursorX = leftX;
        cursorY = topY;
    }

    if (glyphs.empty()) {
        return;
    }

    size_t count = glyphs.size();

    for (size_t i = 0; i < count; ++i) {
        Quad& q = glyphs[i].quad;

        // centered glyph placement: anchor is the cursor, glyph extends by
        // half-width/half-height on each side.
        q.posX = cursorX + q.width  * 0.5f;
        q.posY = cursorY + q.height * 0.5f;

        // advance for the next glyph. when the next glyph is on a different
        // line, snap cursor X back to leftX and bump Y by this glyph's
        // height (the line-height contribution of the line we just ended).
        bool atLastGlyph    = (i + 1 >= count);
        bool nextLineBreaks = !atLastGlyph &&
                              (lineIndices[i] != lineIndices[i + 1]);

        if (nextLineBreaks) {
            cursorX  = leftX;
            cursorY += q.height;
        } else {
            cursorX += q.width;
        }
    }
}
