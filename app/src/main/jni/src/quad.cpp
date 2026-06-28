#include "quad.h"

// FUN_100007d78
Quad::Quad()
    : posX(0.0f), posY(0.0f)
    , width(0.0f), height(0.0f)
    , scaleX(1.0f), scaleY(1.0f)
    , rotation(0.0f)
    , animating(false)
    , hidden(false) {

    // unit quad centered at origin
    // vertex layout matches binary: (-0.5,-0.5), (0.5,-0.5), (-0.5,0.5), (0.5,0.5)
    vertices[0] = { -0.5f, -0.5f, 0xFFFFFFFF, 0.0f, 0.0f };
    vertices[1] = {  0.5f, -0.5f, 0xFFFFFFFF, 1.0f, 0.0f };
    vertices[2] = { -0.5f,  0.5f, 0xFFFFFFFF, 0.0f, 1.0f };
    vertices[3] = {  0.5f,  0.5f, 0xFFFFFFFF, 1.0f, 1.0f };

    for (int i = 0; i < 4; i++) {
        targetVertices[i] = vertices[i];
    }
}

// FUN_1000081bc
void Quad::setTexCoords(float u0, float v0, float u1, float v1) {
    vertices[0].u = u0;  vertices[0].v = v0;
    vertices[1].u = u1;  vertices[1].v = v0;
    vertices[2].u = u0;  vertices[2].v = v1;
    vertices[3].u = u1;  vertices[3].v = v1;
}

// FUN_1000081f0
void Quad::setSize(float w, float h) {
    width = w;
    height = h;

    float hw = w * 0.5f;
    float hh = h * 0.5f;

    vertices[0].x = -hw;  vertices[0].y = -hh;
    vertices[1].x =  hw;  vertices[1].y = -hh;
    vertices[2].x = -hw;  vertices[2].y =  hh;
    vertices[3].x =  hw;  vertices[3].y =  hh;
}

// FUN_100008238
void Quad::addVertexOffset(float dx, float dy) {

    for (int i = 0; i < 4; i++) {
        vertices[i].x += dx;
        vertices[i].y += dy;
    }
}

void Quad::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t c = (a << 24) | (b << 16) | (g << 8) | r;

    for (int i = 0; i < 4; i++) {
        vertices[i].color = c;
    }
}

// FUN_1000573a8. pixel-snap posX/posY to a 1/640 grid, anchored at the
// first vertex. the v0 offset makes the quad's bounding box land on whole
// pixels regardless of half-extents from setSize.
// SNAP_REF = DAT_10005a730 = 640.0f.
void Quad::snapToPixelGrid() {
    constexpr float SNAP_REF = 640.0f;

    auto snap = [](float v) {
        float scaled = v * SNAP_REF + (v < 0.0f ? -0.5f : 0.5f);
        return (float)(int)scaled / SNAP_REF;
    };

    float v0x = vertices[0].x;
    float v0y = vertices[0].y;
    posX = snap(posX + v0x) - v0x;
    posY = snap(posY + v0y) - v0y;
}

// FUN_100008388.
//
// writes only the alpha byte (byte 3 of each vertex's packed color), leaving
// per-vertex RGB untouched. broadcasting RGB from vertex 0 would destroy any
// gradient set per-vertex (e.g. ScorePanel's bgQuad white-to-purple).
void Quad::setAlpha(uint8_t a) {

    for (int i = 0; i < 4; i++) {
        vertices[i].color = (vertices[i].color & 0x00FFFFFFu)
                            | (static_cast<uint32_t>(a) << 24);
    }
}

// FUN_100008308. copies the 4-element vertex array, then the pos / size /
// scale / rotation scalars. Vertex is POD so per-element assignment matches
// the binary's block copy.
void Quad::copyVisualState(const Quad& src) {

    for (int i = 0; i < 4; i++) {
        vertices[i] = src.vertices[i];
    }

    posX     = src.posX;
    posY     = src.posY;
    width    = src.width;
    height   = src.height;
    scaleX   = src.scaleX;
    scaleY   = src.scaleY;
    rotation = src.rotation;
}

// FUN_100007ef0, per-frame animation step. runs at the start of draw() when
// animating=true. clips the current vertex bbox to the target rect
// [animMin..animMax] (specified in world space; we subtract posX/posY to bring
// it into local vertex space). UV coords on moved edges are lerped
// proportionally so the displayed UV sub-region shrinks/shifts to match the
// new bbox.
//
// the snapshot/restore pattern: animationStep snapshots vertices into
// targetVertices before tweening; draw renders with the tweened vertices;
// the after-draw block (in Quad::draw) restores vertices from targetVertices.
// net effect: the tween's visual extent is per-frame, the underlying
// vertices are preserved across frames so external posX/posY animation
// works correctly without the bbox drifting.
//
// returns true if the quad is visible this frame, false if the bbox
// doesn't overlap the target rect (caller should skip rendering).
bool Quad::animationStep() {
    const float targetMinX = animMinX - posX;
    const float targetMinY = animMinY - posY;
    const float targetMaxX = animMaxX - posX;
    const float targetMaxY = animMaxY - posY;

    // bbox/target overlap test. if no overlap, hide and bail.
    // NaN inputs naturally fall through to the "no overlap" path.
    if (vertices[0].x > targetMaxX ||
        vertices[3].x < targetMinX ||
        vertices[0].y > targetMaxY ||
        vertices[3].y < targetMinY) {
        hidden = true;
        return false;
    }

    hidden = false;

    // snapshot current vertices into targetVertices.
    for (int i = 0; i < 4; i++) {
        targetVertices[i] = vertices[i];
    }

    // tween formula (all 4 edges, factor = where the target edge sits along
    // the bbox span as fraction 0..1, measured from the opposite edge):
    //   factor = (targetEdge - oppositeEdge) / (thisEdge - oppositeEdge)
    //   inv    = 1 - factor
    //   thisVertex.uv = inv * oppositeVertex.uv + factor * thisVertex.uv
    //   thisVertex.{x,y} = targetEdge
    // each edge reads the current (post-prior-tween) opposite-edge vertex.

    // -- left edge: if current left extends past target's left, pull it in.
    if (vertices[0].x < targetMinX) {
        const float factor = (targetMinX - vertices[0].x)
                             / (vertices[1].x - vertices[0].x);
        const float inv    = 1.0f - factor;

        vertices[0].u = inv * vertices[0].u + factor * vertices[1].u;
        vertices[0].v = inv * vertices[0].v + factor * vertices[1].v;
        vertices[2].u = inv * vertices[2].u + factor * vertices[3].u;
        vertices[2].v = inv * vertices[2].v + factor * vertices[3].v;
        vertices[0].x = targetMinX;
        vertices[2].x = targetMinX;
    }

    // -- right edge. uses the post-left vertices[0].x as the opposite-edge
    //    anchor (the left edge stays the anchor across the left-then-right
    //    tween sequence).
    if (vertices[3].x > targetMaxX) {
        const float factor = (targetMaxX - vertices[0].x)
                             / (vertices[1].x - vertices[0].x);
        const float inv    = 1.0f - factor;

        vertices[1].u = inv * vertices[0].u + factor * vertices[1].u;
        vertices[1].v = inv * vertices[0].v + factor * vertices[1].v;
        vertices[3].u = inv * vertices[2].u + factor * vertices[3].u;
        vertices[3].v = inv * vertices[2].v + factor * vertices[3].v;
        vertices[1].x = targetMaxX;
        vertices[3].x = targetMaxX;
    }

    // -- top edge.
    if (vertices[0].y < targetMinY) {
        const float factor = (targetMinY - vertices[0].y)
                             / (vertices[3].y - vertices[0].y);
        const float inv    = 1.0f - factor;

        vertices[0].u = inv * vertices[0].u + factor * vertices[2].u;
        vertices[0].v = inv * vertices[0].v + factor * vertices[2].v;
        vertices[1].u = inv * vertices[1].u + factor * vertices[3].u;
        vertices[1].v = inv * vertices[1].v + factor * vertices[3].v;
        vertices[0].y = targetMinY;
        vertices[1].y = targetMinY;
    }

    // -- bottom edge. uses the post-top vertices[0].y as the opposite-edge anchor.
    if (vertices[3].y > targetMaxY) {
        const float factor = (targetMaxY - vertices[0].y)
                             / (vertices[3].y - vertices[0].y);
        const float inv    = 1.0f - factor;

        vertices[2].u = inv * vertices[0].u + factor * vertices[2].u;
        vertices[2].v = inv * vertices[0].v + factor * vertices[2].v;
        vertices[3].u = inv * vertices[1].u + factor * vertices[3].u;
        vertices[3].v = inv * vertices[1].v + factor * vertices[3].v;
        vertices[2].y = targetMaxY;
        vertices[3].y = targetMaxY;
    }

    return true;
}

// FUN_100007df8.
// texture must be bound externally via bindTexture() before calling draw().
void Quad::draw() {

    if (animating) {

        if (!animationStep()) {
            return;   // bbox doesn't overlap target rect
        }
    }

    // non-animating quads draw unconditionally: the binary consults the hidden
    // byte (0xC5) only inside the animating branch, never on this path. honoring
    // a stale hidden here would wrongly drop a quad after its animation ended.
    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    if (scaleX != 1.0f || scaleY != 1.0f) {
        glScalef(scaleX, scaleY, 1.0f);
    }

    if (rotation != 0.0f) {
        glRotatef(rotation, 0.0f, 0.0f, 1.0f);
    }

    // stride 0x14 = 20 bytes, matching the binary exactly
    const int stride = 20;
    const char* base = (const char*)vertices;

    // offset 0: position (2 floats)
    glVertexPointer(2, GL_FLOAT, stride, base);
    // offset 8: color (4 unsigned bytes)
    glColorPointer(4, GL_UNSIGNED_BYTE, stride, base + 8);
    // offset 12: texcoord (2 floats)
    glTexCoordPointer(2, GL_FLOAT, stride, base + 12);

    // GL_TRIANGLE_STRIP = 5
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glPopMatrix();

    // after drawing, if animating, restore vertices from the pre-tween
    // snapshot so the next frame's tween starts from a consistent base.
    if (animating) {

        for (int i = 0; i < 4; i++) {
            vertices[i] = targetVertices[i];
        }
    }
}

// FUN_1000083bc, bbox hit-test for input dispatch. returns true when (x, y)
// lies within the Quad's display bounding box, computed as the two diagonal
// corners (vertex[0], vertex[3]) translated by posX/posY and scaled by
// scaleX/scaleY. inclusive on all four edges.
//
// expressed as a single and-chain so NaN inputs naturally fail every check
// (NaN >= X and NaN <= X both return false in IEEE-754), matching the
// binary's NaN-unordered fcmp behavior which short-circuits to false.
bool Quad::contains(float x, float y) const {
    float leftX   = posX + scaleX * vertices[0].x;
    float rightX  = posX + scaleX * vertices[3].x;
    float topY    = posY + scaleY * vertices[0].y;
    float bottomY = posY + scaleY * vertices[3].y;

    return (x >= leftX) && (x <= rightX) && (y >= topY) && (y <= bottomY);
}
