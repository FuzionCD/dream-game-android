#pragma once

#include <GLES/gl.h>
#include <cstdint>

// reconstructed from Ghidra decompilation:
//   constructor:    FUN_100007d78
//   draw:           FUN_100007df8
//   animationStep:  FUN_100007ef0 (called from draw when animating)
//   startAnimation: FUN_1000084e0 (sets animating + bbox)
//   setTexCoords:   FUN_1000081bc
//   setSize:        FUN_1000081f0
//
// animation semantics (verified in FUN_100007ef0 disassembly):
//   each frame, the draw path calls animationStep, which checks whether the
//   current vertex bbox already encloses [animMin, animMax] (in local space,
//   i.e. relative to posX/posY). if not, it extends one or more of the four
//   bbox edges of `vertices` toward the corresponding animMin/animMax edge,
//   blending vertex colors via the interp factor (vertex[i].color is lerped
//   between adjacent vertices on the moving edge). vtable copy-step in draw
//   then snapshots vertices into targetVertices.
//   when the bbox fully encloses the target rect, hidden is cleared and the
//   animation is logically done (animating stays true until external clear).
//   if any animMin/animMax bound is outside the current bbox extent, hidden
//   is set to 1 (the Quad is invisible while animating in toward that bound).
//
// the original has virtual methods (draw is called via vtable in board/tile code).
// we make draw() virtual to match, which gives us the vtable pointer at offset 0x00
// and keeps sizeof(Quad) == 0xD8.
//
// member order matches the binary exactly so that structs embedding Quads
// have correct field offsets. note: the iOS struct stride was previously
// believed to be 0xC8, but Label::measureGlyphRun iterates with `lVar2 += 0xd8`
// and Label::addGlyph (FUN_10004c310) does `_memcpy(puVar1 + 1, auStack_140, 0xd0)`
// = 0xD0 + 8-byte vtable = 0xD8 total. the trailing 0x10 bytes are the
// animation target rect, read every frame by the animation tween.

struct Vertex {
    float x, y;
    uint32_t color;
    float u, v;
};

class Quad {
public:
    Quad();
    virtual ~Quad() = default;

    // FUN_1000081bc: set texture region (u_min, v_min) to (u_max, v_max)
    void setTexCoords(float u0, float v0, float u1, float v1);

    // FUN_1000081f0: set display size, rebuilds vertex positions as half-extents
    void setSize(float w, float h);

    // FUN_100008238: shift all 4 vertex (x, y) by (dx, dy). different from
    // posX/posY in that it changes the vertex bounding box itself, leaving the
    // render-time translate point (posX, posY) at the original anchor.
    void addVertexOffset(float dx, float dy);

    // set tint color (rgba, each 0-255)
    void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // FUN_100008388: set just the alpha byte on all 4 vertices, leaving R/G/B
    // as-is (read from vertex 0, applied to the rest).
    void setAlpha(uint8_t a);

    // FUN_1000573a8: snap posX/posY to the nearest 1/640 pixel grid, using
    // (posX + vertex[0].x, posY + vertex[0].y) as the anchor point. the result
    // is `snappedAnchor - vertex[0]`, i.e. posX is adjusted so the bounding
    // box's first vertex lands on a pixel boundary at 640px reference width.
    void snapToPixelGrid();

    // FUN_100008308: copy the displayable visual state from `src`: vertices
    // (= 4 * 20 bytes), posX/Y, width/height, scaleX/Y, rotation. does not
    // copy targetVertices, animating / hidden flags. used by HexMap's kind-3
    // deactivation to clone the icon's display state onto the outline.
    void copyVisualState(const Quad& src);

    // FUN_1000083bc: returns true when (x, y) lies within the Quad's display
    // bounding box. the bbox is computed as the two diagonal corners
    //   left   = posX + scaleX * vertex[0].x
    //   top    = posY + scaleY * vertex[0].y
    //   right  = posX + scaleX * vertex[3].x
    //   bottom = posY + scaleY * vertex[3].y
    // and checked inclusively (left <= x <= right) and (top <= y <= bottom).
    // used by the input dispatch code for tap / drag hit-tests against
    // back / nav / perk-slot / rack-tile quads.
    bool contains(float x, float y) const;

    // FUN_100007ef0: per-frame animation step. snapshots vertices into
    // targetVertices, then clips the vertex bbox to the world-space target
    // rect [animMin..animMax] with proportional UV lerping on moved edges.
    // returns false when no overlap (caller hides the quad).
    bool animationStep();

    // FUN_100007df8: draw at current position/rotation/scale.
    // texture must be bound externally via bindTexture() before calling.
    // virtual because the original calls draw through vtable for tile subclasses.
    virtual void draw();

    // --- fields ordered to match binary layout after the vtable pointer ---
    // (vtable pointer is implicit at offset 0x00, added by the compiler)

    // vertex data (private but must come first to match layout)
    Vertex vertices[4];        // 0x08, 80 bytes
    Vertex targetVertices[4];  // 0x58, 80 bytes

    // public fields
    float posX, posY;          // 0xA8, 0xAC
    float width, height;       // 0xB0, 0xB4
    float scaleX, scaleY;      // 0xB8, 0xBC
    float rotation;
    bool animating;
    bool hidden;

    // animation target rect (local space, relative to posX/posY).
    // written by startAnimation (FUN_1000084e0), read every frame by
    // animationStep (FUN_100007ef0) to extend the vertex bbox toward it.
    // ctor leaves these uninitialized; gameplay must call startAnimation
    // before setting `animating = true`.
    float animMinX;
    float animMinY;
    float animMaxX;
    float animMaxY;
};