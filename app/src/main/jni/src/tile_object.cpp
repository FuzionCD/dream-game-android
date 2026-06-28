#include "tile_object.h"
#include "color_tint.h"  // ColorTint::setAlpha used by the decoration list walk
#include "game.h"        // getGame
#include "renderer.h"    // bindTexture
#include "quad.h"        // ColorTint via QuadColor base
#include "random.h"      // rngInt for mirror + rotationStep rolls
#include "snag_content.h"
#include "tile_content.h"
#include "tile_grid_pool.h"  // kGridPoolTable for permitsDirection
#include <GLES/gl.h>
#include <algorithm>     // std::remove for attachSnag trackedContent dedup
#include <cmath>
#include <cstring>
#include <new>

// ----------------------------------------------------------------------------
// hex-grid UV math constants (read from binary __TEXT,__const at 0x100059e4c).
// FUN_100012930 picks a hex tile's UV based on its (tileVariant, gridIdx) by
// computing a wrapped index `(row + col*24 - (col/3)*72)` and using that to
// index into a 9-wide x 8-tall sub-grid in the texture sheet. the per-tile
// (directionality x variant) sprite picking is baked into the texture: the
// 288 hex sprites (24 directions x 12 variants) live across an atlas, and
// (tileVariant, gridIdx) selects which atlas cell.
// ----------------------------------------------------------------------------
namespace {
constexpr float HEX_U_STRIDE_PX  = 0.1765625f;    // DAT_100059e4c
constexpr float HEX_V_STRIDE_PX  = 0.1984375f;    // DAT_100059e50
constexpr float TEX_PIXEL_SCALE  = 640.0f;        // DAT_100059e54
constexpr float HEX_U_EXTENT_PX  = 112.0f;        // DAT_100059e58
constexpr float HEX_V_EXTENT_PX  = 126.0f;        // DAT_100059e5c
constexpr float TEX_PIXEL_INV    = 1.0f / 1024.0f;// DAT_100059e60
constexpr float HEX_ROT_PER_STEP = 60.0f;         // DAT_100059e64 (degrees per step)
constexpr float HEX_TILE_W       = 0.175f;        // size W (inline literal in FUN_100012930)
constexpr float HEX_TILE_H       = 0.196875f;     // size H (inline literal)
constexpr float ICON_POS_OFFSET  = 0.003125f;     // DAT_100059e6c (icon-Quad pos offset)
constexpr float PIXEL_SNAP_DENOM = 640.0f;        // DAT_10005a730 (snap-to-pixel grid)
}

// reconstructed from Ghidra FUN_1000120ec.
// the binary memsets the buffer first (via operator_new initializer in some
// callers, or relies on prior zeroing), then calls Quad::ctor twice and seeds
// a handful of fields. our caller (GameBoard::create) memsets to 0 before
// calling init(), matching that contract, including the std::vector slot
// (trackedContent), which we reconstruct via placement-new below.
void TileObject::init() {
    // ---- reset the two embedded Quads ----
    // each Quad's ctor (FUN_100007d78) sets vtable, unit-quad vertices, and
    // scale 1.0. our Quad::Quad() does the equivalent.
    mainQuad = Quad();
    iconQuad = Quad();

    // ---- post-Quad-ctor field setup, line-by-line from FUN_1000120ec ----
    // (offsets translated to named fields)
    gridLayout          = 0;        // (set by setHexUVs later)
    gridIdx             = 0;
    mirror              = false;
    rotationStep        = 0;
    gridCol             = 0;        // (set by setGridCoord at commit)
    gridRow             = 0;

    // 1.0 init sets all four anim timers to "idle" (no animation pending).
    iconFadeT           = 1.0f;
    slideAnimActive     = false;
    slideImmediate      = false;
    slideFlag           = false;
    slideStartPos[0]    = 0.0f;
    slideStartPos[1]    = 0.0f;
    slideTargetPos[0]   = 0.0f;
    slideTargetPos[1]   = 0.0f;
    slideTimer          = 1.0f;
    slideDelay          = 0.0f;
    rotationLerpStart   = 0.0f;
    rotationLerpTarget  = 0.0f;
    rotationLerpT       = 1.0f;

    content             = nullptr;
    snagContent         = nullptr;

    // GameBoard's allocator memset'd the whole region to 0, which trampled
    // the std::vector internals. placement-new gives us a fresh empty vector
    // with valid begin/end/cap = nullptr, matching the binary's state after
    // its own ctor (which leaves the vector's begin/end/cap = 0).
    new (&trackedContent) std::vector<SnagContent*>();

    // decoration list. same memset situation as trackedContent, so
    // placement-new a fresh empty std::list to reconstruct the libc++ head.
    new (&decorations) std::list<DecorationValue>();
    rotationAnimT      = 0.0f;
    rotationAnimActive = false;

    // FUN_1000120ec then configures the icon Quad's UV / size / alpha. these
    // are the defaults that get overwritten when setVisual / setSnagVisual
    // pick a real per-content-type icon (Phase 2 work). leaving them in matches
    // the binary's invariant that an uninitialized tile still has a valid icon
    // Quad ready for a draw call.
    iconQuad.setTexCoords(0.0f, 0.0f, 0.12695313f, 0.14160156f);
    iconQuad.setSize(0.203125f, 0.2265625f);
    iconQuad.setAlpha(0xB4);
}

// destructor: free the heap allocations the binary's per-sub-object cleanup
// loops would handle. trackedContent only ever holds aliases of snagContent
// (see transformToSnag / setSnagVisual which replace+track in lockstep), so we
// don't need to walk it separately.
TileObject::~TileObject() {

    if (content) {
        delete content;
        content = nullptr;
    }

    if (snagContent) {
        delete snagContent;
        snagContent = nullptr;
    }
}

// reconstructed from FUN_100012278 (the binary's per-tile draw helper that
// GameBoard::draw calls for every visible tile). order:
//   1. if rotationAnimT > 0: push matrix + translate to (posX, posY) +
//      rotate by rotationAnimT * 180 + translate back. (the binary's
//      DAT_100059e34 = 180.0f is the per-unit-T rotation in degrees.)
//   2. if slideAnimActive or iconFadeT < 1.0: bind tex 8, draw iconQuad,
//      the small icon overlay shown when the tile is mid-slide-in or has
//      shrunk for some reason.
//   3. bind tex (tileVariant/3) + 4 -> tiles{1..4}.png picker, draw mainQuad
//      (the hex face).
//   4. if `showContent`: call drawContent(drawTints) which renders the
//      tile's TileContent / SnagContent over the hex face, unless a
//      Darkness ("?") decoration is active, in which case content stays
//      hidden.
//   5. iterate the decoration list, draw each iconSubQuad
//      (bind tex 8 per node) plus the embedded ColorTint when value > 0
//      (kind=2 numeric overlay).
//   6. pop matrix if rotation push fired.
void TileObject::draw(bool showContent, bool drawTints) {
    constexpr float TILE_ROTATION_ANIM_DEGREES = 180.0f;  // DAT_100059e34

    bool rotated = (rotationAnimT > 0.0f);

    if (rotated) {
        glPushMatrix();
        glTranslatef(mainQuad.posX, mainQuad.posY, 0.0f);
        // the binary's blend `t * 180 + (1 - t) * 0` is just `t * 180`,
        // but written that way for symmetry with other lerp animations.
        glRotatef(rotationAnimT * TILE_ROTATION_ANIM_DEGREES, 0.0f, 0.0f, 1.0f);
        glTranslatef(-mainQuad.posX, -mainQuad.posY, 0.0f);
    }

    if (slideAnimActive || iconFadeT < 1.0f) {
        bindTexture(8);
        iconQuad.draw();
    }

    bindTexture(static_cast<GLuint>((gridLayout / 3) + 4));
    mainQuad.draw();

    if (showContent) {
        drawContent(drawTints);
    }

    // decoration walk: matches FUN_100012278's tail loop.
    for (DecorationValue& d : decorations) {
        bindTexture(8);
        d.iconSubQuad.draw();

        if (d.value > 0) {
            d.colorTint.draw();
        }
    }

    if (rotated) {
        glPopMatrix();
    }
}

// reconstructed from FUN_1000123c4 (the content-layer of FUN_100012278).
//
// walks the decoration list looking for an active "Darkness obscured" node
// (kind == 1, suppressed == 0). if found, return early without drawing
// content: the question-mark decoration alone covers the tile and the
// player can't see what's underneath. if no such decoration exists, draw
// the TileContent (vtable[2] = base MovableActor draw) plus every tracked
// SnagContent via its vtable[8] (full draw: sprite + 3 stat displays +
// 3 tints when drawTints is set).
void TileObject::drawContent(bool drawTints) {

    for (const DecorationValue& d : decorations) {

        if (!d.suppressed && d.kind == 1) {
            return;   // Darkness active, content stays hidden
        }
    }

    if (content) {
        content->draw();
    }

    for (SnagContent* sc : trackedContent) {

        if (sc) {
            sc->drawFull(drawTints);
        }
    }
}

// FUN_1000124ac, per-tile per-frame update. order: rotation timer, slide
// animation, rotation-lerp animation, iconQuad alpha-fade, content/snag
// vtable[3] dispatch, decoration alpha animation. all timers cos-eased and
// share rate constants from __DATA.
void TileObject::update(float dt) {
    constexpr float ANIM_DURATION    = 0.2f;   // DAT_100059e38, slide / rotation / lerp
    constexpr float ICON_FADE_DUR    = 0.1f;   // DAT_100059e40, iconFadeT rate
    constexpr float COS_PHASE        = 3.14159274f;  // DAT_100059e3c, pi
    constexpr float ICON_FADE_TARGET = 180.0f; // DAT_100059e44, alpha endpoint
    constexpr float ROT_FACTOR_INC   = 1.0f;   // DAT_100059eb4, active grow
    constexpr float ROT_FACTOR_DEC   = -1.0f;  // DAT_100059eb0, inactive decay

    auto clamp01 = [](float v) {
        if (v > 1.0f) return 1.0f;
        if (v < 0.0f) return 0.0f;
        return v;
    };
    auto cosEase = [](float t) {
        return (1.0f - std::cos(t * COS_PHASE)) * 0.5f;
    };

    // 1. rotation timer (active = grow toward 1, inactive but t > 0 = decay).
    if (rotationAnimActive || rotationAnimT > 0.0f) {
        float factor = rotationAnimActive ? ROT_FACTOR_INC : ROT_FACTOR_DEC;
        rotationAnimT = clamp01(rotationAnimT + (dt / ANIM_DURATION) * factor);
    }

    // 2. slide animation. delay phase first; then advance + cos-ease + propagate.
    if (slideTimer < 1.0f) {

        if (slideDelay > 0.0f) {
            slideDelay -= dt;
        } else {
            slideTimer = clamp01(slideTimer + dt / ANIM_DURATION);
            float t = cosEase(slideTimer);
            float easedX = slideStartPos[0] * (1.0f - t) + slideTargetPos[0] * t;
            float easedY = slideStartPos[1] * (1.0f - t) + slideTargetPos[1] * t;
            // setPosition propagates to iconQuad / decorations / content
            setPosition(easedX, easedY);

            if (slideTimer == 1.0f) {
                // slide just finished: maybe kick off icon fade-out, maybe play sound.
                if (rotationLerpT == 1.0f && slideImmediate && slideAnimActive) {
                    slideAnimActive = false;
                    iconFadeT       = 0.0f;
                }

                if (slideFlag) {
                    // tilePlace sound (slot 0xF). matches FUN_100043798()->FUN_100035ccc(0xF).
                    Game* g = getGame();

                    if (g) {
                        g->soundQueue.trigger(0xF);
                    }
                }
            }
        }
    }

    // 3. rotation-lerp animation (drives mainQuad+iconQuad rotation simultaneously).
    if (rotationLerpT < 1.0f) {
        rotationLerpT = clamp01(rotationLerpT + dt / ANIM_DURATION);
        float t = cosEase(rotationLerpT);
        float easedRot = rotationLerpStart * (1.0f - t) + rotationLerpTarget * t;
        mainQuad.rotation = easedRot;
        iconQuad.rotation = easedRot;

        // duplicate end-of-slide check (matches binary's structure exactly).
        if (slideTimer == 1.0f && rotationLerpT == 1.0f && slideImmediate && slideAnimActive) {
            slideAnimActive = false;
            iconFadeT       = 0.0f;
        }
    }

    // 4. iconFadeT alpha lerp. when slideAnimActive: 0 -> 180 (fade in).
    //    when !slideAnimActive: 180 -> 0 (fade out).
    if (iconFadeT < 1.0f) {
        iconFadeT = clamp01(iconFadeT + dt / ICON_FADE_DUR);
        bool fadingOut = !slideAnimActive;
        float startA = fadingOut ? ICON_FADE_TARGET : 0.0f;
        float endA   = fadingOut ? 0.0f : ICON_FADE_TARGET;
        uint8_t a = static_cast<uint8_t>(iconFadeT * endA + (1.0f - iconFadeT) * startA);
        iconQuad.setAlpha(a);
    }

    // 5. content / snagContent / trackedContent vtable[3] dispatch.
    //
    // matches FUN_1000124ac's three sub-blocks:
    //   `if (content) (*vtable[3])(content);`           TileContent (= content)
    //   for each entry in trackedContent:               trackedContent[*]
    //       `(*vtable[3])(entry);`                       SnagContent::update
    //
    // both TileContent and SnagContent inherit MovableActor::update, which
    // is the binary's FUN_100038bac (4-stage state machine). a freshly
    // allocated content/snag has spawnT/moveT/fadeT/scaleOutT all already at
    // 1.0 (initBase value), so each call is a no-op until something kicks off
    // an animation. the dispatch still has to happen for byte-faithful parity;
    // future combat / pickup paths reset these timers to <1.0 and need the
    // per-frame advance.
    if (content) {
        content->update(dt);
    }

    for (SnagContent* sc : trackedContent) {
        if (sc) {
            sc->update(dt);
        }
    }

    // 6. decoration list per-node alpha animation. faithful port of
    //    FUN_1000124ac's tail loop. for each node:
    //      - skip if alphaT >= 1.0 (settled).
    //      - else: alphaT += dt*2 (clamped to 1).
    //        `bVar2 = (suppressed == 0)`. compute end+start endpoints:
    //          !suppressed -> fade in  (start=0, end=255)
    //           suppressed -> fade out (start=255, end=0)
    //        write alpha onto iconSubQuad and the embedded ColorTint.
    //      - if `suppressed` and alphaT now == 1.0: unlink the node + free
    //        (FUN_100014064 -> returns next iter pointer).
    //      - otherwise advance to next via prev/next.
    //
    // an empty decoration list skips this loop body entirely.
    constexpr float DECOR_ALPHA_END = 255.0f;  // DAT_100059e48

    for (auto it = decorations.begin(); it != decorations.end(); ) {
        float t = it->alphaT;

        if (t >= 1.0f) {
            ++it;
            continue;
        }

        t += dt + dt;

        if (t > 1.0f) {
            t = 1.0f;
        }

        it->alphaT = t;
        bool fadingIn = !it->suppressed;
        float startA  = fadingIn ? 0.0f             : DECOR_ALPHA_END;
        float endA    = fadingIn ? DECOR_ALPHA_END  : 0.0f;
        uint8_t a     = static_cast<uint8_t>(t * endA + (1.0f - t) * startA);
        it->iconSubQuad.setAlpha(a);
        it->colorTint.setAlpha(a);

        if (!it->suppressed || t < 1.0f) {
            ++it;
            continue;
        }

        // suppressed + animation complete -> drop this node (erase returns the
        // next iterator). matches FUN_100014064's "remove + return next".
        it = decorations.erase(it);
    }
}

// reconstructed from Ghidra FUN_1000133b0
int TileObject::getContentType() {
    if (content && /* visible byte at MovableActor */
        *((uint8_t*)content + 8) != 0) {
        return content->type;
    }

    return 0;
}

// reconstructed from Ghidra FUN_1000134d8. dim the content / snag sub-
// elements (whichever is alive) via their vtable[5] hooks.
void TileObject::setTileAlpha(uint8_t alpha) {

    if (content && content->visible) {
        content->setAlpha(alpha);
    }

    if (snagContent && snagContent->visible) {
        snagContent->setAlpha(alpha);
    }
}

// reconstructed from Ghidra FUN_100014014
bool TileObject::isContentScaleAnimating() const {

    if (content && content->visible && content->scaleOutT < 1.0f) {
        return true;
    }

    if (snagContent && snagContent->visible && snagContent->scaleOutT < 1.0f) {
        return true;
    }

    return false;
}

// reconstructed from Ghidra FUN_1000133f0
int TileObject::getSnagType() {

    if (snagContent &&
        *((uint8_t*)snagContent + 8) != 0) {
        return snagContent->type;
    }

    return 0;
}

// reconstructed from Ghidra FUN_100012e0c.
int TileObject::getExitCount() const {
    return kGridPoolTable[gridLayout][gridIdx].exitCount;
}

// reconstructed from Ghidra FUN_100012470. walks the decoration list, matching
// when a node has the requested `kind` and its suppressed flag is 0.
bool TileObject::hasActiveDecorationOfKind(int kind) const {

    for (const DecorationValue& d : decorations) {

        if (!d.suppressed && d.kind == kind) {
            return true;
        }
    }

    return false;
}

// reconstructed from Ghidra FUN_100013c3c. same walk shape as
// hasActiveDecorationOfKind but returns the matched decoration's `value`
// instead of bool.
int TileObject::decorationValueOfKind(int kind) const {

    for (const DecorationValue& d : decorations) {

        if (!d.suppressed && d.kind == kind) {
            return d.value;
        }
    }

    return 0;
}

// reconstructed from Ghidra FUN_100012d70.
//
// asks "does this tile have an exit edge in direction `dir` (0..5) given
// base rotation `baseRot`?". the binary's mirror flag negates each exit via
// `5 - exit`, then `(exit + baseRot) % 6` is matched against `dir`.
// passing baseRot=-1 substitutes the tile's own rotationStep.
bool TileObject::permitsDirection(int dir, int baseRot) const {

    if (baseRot == -1) {
        baseRot = rotationStep;
    }

    const GridPoolEntry& entry = kGridPoolTable[gridLayout][gridIdx];

    for (int i = 0; i < entry.exitCount; i++) {
        int exit = entry.exits[i];

        if (mirror) {
            exit = 5 - exit;
        }

        if ((exit + baseRot) % 6 == dir) {
            return true;
        }
    }

    return false;
}

// reconstructed from Ghidra FUN_100013410
SnagContent* TileObject::getSnagIfAlive() {

    if (!snagContent) {
        return nullptr;
    }

    // visible byte at MovableActor
    if (*((uint8_t*)snagContent + 8) == 0) {
        return nullptr;
    }

    return snagContent;
}

// reconstructed from Ghidra FUN_100013430.
TileContent* TileObject::getTileContentIfAlive() {

    if (!content) {
        return nullptr;
    }

    if (!content->visible) {
        return nullptr;
    }

    return content;
}

// reconstructed from Ghidra FUN_100012844.
void TileObject::setRotationDirect(float degrees) {
    mainQuad.rotation = degrees;
    iconQuad.rotation = degrees;
}

// reconstructed from Ghidra FUN_100013bfc. suppress every decoration of the
// given kind that's currently active. for kind=1 (Darkness "?") this kicks
// the content-reveal animation. the decoration node stays in the list; only
// the suppressed flag flips and alphaT resets.
void TileObject::suppressDecorationsOfKind(int kind) {

    for (DecorationValue& d : decorations) {

        if (!d.suppressed && d.kind == kind) {
            d.suppressed = true;
            d.alphaT     = 0.0f;
        }
    }
}

// reconstructed from Ghidra FUN_1000133d0.
int TileObject::getContentMagnitude() {

    if (!content || !content->visible) {
        return 0;
    }

    return content->displayedMagnitude;
}

// reconstructed from Ghidra FUN_100012930.
//
// computes the main-hex texture coordinates from (tileVariant, gridIdx). the
// formula is the binary's verbatim:
//   raw_idx = gridIdx + tileVariant*24 - (tileVariant/3)*72        // wraps to [0..71]
//   sub_u   = raw_idx % 9                                  // column in 9-wide sheet
//   sub_v   = raw_idx / 9                                  // row in 8-tall sheet
//   u_pixels = sub_u * 0.1765625 * 640
//   v_pixels = sub_v * 0.1984375 * 640
//   u0 = u_pixels / 1024;  u1 = (u_pixels + 112) / 1024
//   v0 = v_pixels / 1024;  v1 = (v_pixels + 126) / 1024
// then size = (0.175, 0.196875), rotation = rotationStep * 60 deg.
//
// `mirror=1` triggers FUN_100008430 which swaps U coords between vert pairs
// (horizontal mirror of the hex sprite, used to break up visual repetition).
//
// also frees content / snagContent and clears the
// trackedContent vector; the binary does this so that re-rolling a tile
// drops all its sub-data.
void TileObject::setHexUVs(int _gridLayout, int _gridIdx, int mirror, int rotationStep) {
    gridLayout = _gridLayout;
    gridIdx = _gridIdx;

    // wrap (tileVariant, gridIdx) into a (sub_u, sub_v) pair for the 9x8 sheet.
    // mirrors the binary's `(param_3 + param_2 * 0x18 + (param_2/3) * -0x48)`
    // note signed division for negative cols.
    int rawIdx = gridIdx + gridLayout * 24 - (gridLayout / 3) * 72;
    int subU   = rawIdx % 9;
    int subV   = rawIdx / 9;

    float uPixels = (float)subU * HEX_U_STRIDE_PX * TEX_PIXEL_SCALE;
    float vPixels = (float)subV * HEX_V_STRIDE_PX * TEX_PIXEL_SCALE;
    float u0 = uPixels * TEX_PIXEL_INV;
    float v0 = vPixels * TEX_PIXEL_INV;
    float u1 = (uPixels + HEX_U_EXTENT_PX) * TEX_PIXEL_INV;
    float v1 = (vPixels + HEX_V_EXTENT_PX) * TEX_PIXEL_INV;

    mainQuad.setTexCoords(u0, v0, u1, v1);
    mainQuad.setSize(HEX_TILE_W, HEX_TILE_H);

    // 50% RNG-rolled horizontal mirror (mirror = (iVar1 < 50) in FUN_100012b5c).
    // FUN_100008430 swaps U of vert 0 <-> vert 1 and U of vert 2 <-> vert 3.
    this->mirror = (mirror != 0);

    if (this->mirror) {
        float u = mainQuad.vertices[0].u;
        mainQuad.vertices[0].u = mainQuad.vertices[1].u;
        mainQuad.vertices[1].u = u;
        u = mainQuad.vertices[2].u;
        mainQuad.vertices[2].u = mainQuad.vertices[3].u;
        mainQuad.vertices[3].u = u;
    }

    this->rotationStep = rotationStep;

    // rotationStep rotates the hex face by 60 deg increments (6 values: 0..300 deg).
    // the binary writes both mainQuad.rotation and iconQuad.rotation
    // so the icon overlay rotates with the face.
    float rotDeg = (float)rotationStep * HEX_ROT_PER_STEP;
    mainQuad.rotation = rotDeg;
    iconQuad.rotation = rotDeg;

    // free existing content/snagContent (they hold heap allocations). matches
    // the binary's "if (content) vtable[1](); content = 0;" pattern.

    if (content) {
        delete content;
        content = nullptr;
    }

    if (snagContent) {
        delete snagContent;
        snagContent = nullptr;
    }

    // empty the tracked-content vector (it was tracking now-freed allocations).
    // matches the binary's "end = begin" vector collapse.
    trackedContent.clear();
}

// reconstructed from Ghidra FUN_100012c34.
//
// sets the main hex's screen position, snaps to the 1/640 pixel grid (matches
// FUN_1000573a8), then propagates:
//   - the icon-quad's screen position (mirrors mainQuad pos + ICON_POS_OFFSET
//     into the iconQuad's own pos slot, iconQuad.posX/posY).
//   - decoration list nodes' attachment positions + their ColorTints
//   - content / snagContent's vtable[4] (their own setPosition).
void TileObject::setPosition(float x, float y) {
    mainQuad.posX = x;
    mainQuad.posY = y;

    // FUN_1000573a8: snap each axis to a 1/640 increment relative to the
    // tile's top-left vertex (vertices[0].x/y are the half-extents).
    float tx = (mainQuad.posX + mainQuad.vertices[0].x) * PIXEL_SNAP_DENOM;
    tx = (tx >= 0.0f) ? (tx + 0.5f) : (tx - 0.5f);
    mainQuad.posX = (float)(int)tx / PIXEL_SNAP_DENOM - mainQuad.vertices[0].x;

    float ty = (mainQuad.posY + mainQuad.vertices[0].y) * PIXEL_SNAP_DENOM;
    ty = (ty >= 0.0f) ? (ty + 0.5f) : (ty - 0.5f);
    mainQuad.posY = (float)(int)ty / PIXEL_SNAP_DENOM - mainQuad.vertices[0].y;

    // mirror the snapped pos into iconQuad with the icon offset added.
    iconQuad.posX = mainQuad.posX + ICON_POS_OFFSET;
    iconQuad.posY = mainQuad.posY + ICON_POS_OFFSET;

    // walk the decoration list. for each node, mirror our pos into
    // iconSubQuad.posX/posY (no subOffset; the icon anchors at the tile center;
    // addVertexOffset in pushDecoration carries any visual offset baked into the
    // vertex layout). subOffsetX/Y are applied only to the digit ColorTint so
    // the number sits relative to the icon.
    for (DecorationValue& d : decorations) {
        // iconSubQuad anchors at the tile center; its vertex offsets
        // (set in pushDecoration) carry whatever spatial offset the kind
        // needs. subOffsetX/Y only shift the digit ColorTint relative to
        // the icon anchor (kind=2 alone uses non-zero subOffset).
        d.iconSubQuad.posX = mainQuad.posX;
        d.iconSubQuad.posY = mainQuad.posY;
        // FUN_10003c870: propagate position into the kind=2 ColorTint. mode 1 =
        // pixel-snap delta (matches FUN_10003c870 param_3=1 in the binary walk).
        d.colorTint.setPosition(mainQuad.posX + d.subOffsetX,
                                mainQuad.posY + d.subOffsetY, 1);
    }

    // delegate to content / snagContent's setPosition (= their vtable[4]
    // overrides). TileContent has its own (FUN_100014a40) which applies a
    // small per-class offset and repositions the magnitude colorTint.
    if (content) {
        content->setPosition(mainQuad.posX, mainQuad.posY);
    }

    if (snagContent) {
        // SnagContent::setPosition lays out baseQuad + 3 stat displays + 3
        // ColorTints around the snag (port of FUN_10003dc38).
        snagContent->setPosition(mainQuad.posX, mainQuad.posY);
    }
}

// reconstructed from Ghidra FUN_10001349c.
//
// writes the placed-on-board coord to the tile's gridCol/gridRow, then propagates
// the same pair to TileContent's and SnagContent's grid coords if
// either is alive (visible byte set). the binary writes 8 bytes
// atomically; we split into two int writes with the same semantics.
void TileObject::setGridCoord(int col, int row) {
    gridCol = col;
    gridRow = row;

    // alive check uses MovableActor::visible. only sub-actors that
    // are currently visible take the propagated coord, matching the binary's
    // `*(char *)(lVar1 + 8) != '\0'` gate.
    if (content && content->visible) {
        content->gridCol = col;
        content->gridRow = row;
    }

    if (snagContent && snagContent->visible) {
        snagContent->gridCol = col;
        snagContent->gridRow = row;
    }
}

// FUN_100012d28. kick off rack slide-in. caller (populateRack) passes
// stagger=slot*0.05. setting iconFadeT=0 starts the icon fade-in.
void TileObject::setRackPosition(float slideDelayArg, const float* targetPos,
                                 bool immediate, bool flag) {
    // mirror current position into slideStartPos (binary copies mainQuad's
    // posX/posY pair into slideStartPos).
    slideStartPos[0]  = mainQuad.posX;
    slideStartPos[1]  = mainQuad.posY;
    slideTargetPos[0] = targetPos[0];
    slideTargetPos[1] = targetPos[1];
    slideTimer        = 0.0f;
    slideDelay        = slideDelayArg;
    slideImmediate    = immediate;
    slideFlag         = flag;

    // re-direct an in-flight slide without re-triggering the fade-in.
    if (!slideAnimActive) {
        slideAnimActive = true;
        iconFadeT       = 0.0f;
    }
}

// ----------------------------------------------------------------------------
// content-allocating methods (Phase 2.5 wiring).
// ----------------------------------------------------------------------------
namespace {
// stream index used by the FUN_100012b5c rolls (mirror + rotationStep).
// matches the literal `0` passed to FUN_1000570ec at the top of FUN_100012b5c.
constexpr uint32_t TILE_RNG_STREAM = 0;
}

// reconstructed from Ghidra FUN_100012b5c.
//
// world-tile init: roll a 50% mirror + a [0..5] rotationStep, hand them to
// setHexUVs, then if the content type is non-zero allocate a TileContent and
// init it with the rolled magnitude (the caller supplies it via
// FUN_100020450 / GameBoard's stat ranges; see snags.h's notes on what the
// magnitude means per content type).
void TileObject::setVisual(int gridLayout, int gridIdx,
                           uint32_t contentType, int magnitude) {
    int highlight    = (rngInt(0, 100, TILE_RNG_STREAM) < 50) ? 1 : 0;
    int rotationRoll = rngInt(0, 5, TILE_RNG_STREAM);

    setHexUVs(gridLayout, gridIdx, highlight, rotationRoll);

    if (contentType != 0) {
        // matches the binary's `operator new(0x178); thunk_FUN_100014708(...)`
        // the parent ptr arg becomes our `nullptr` (see TileContent::init's
        // comment about why every observed call site passes &zero).
        TileContent* tc = new TileContent();
        tc->init(contentType, magnitude, nullptr);
        content = tc;
    }
}

// reconstructed from Ghidra FUN_100012850.
//
// snag-tile init: same hex UV path as setVisual, then always allocate a
// SnagContent (regardless of kind) and feed it the player-context counters.
// the new SnagContent gets registered in trackedContent so transformToSnag can
// later free the old slot without leaking.
void TileObject::setSnagVisual(int gridLayout, int gridIdx, uint32_t kind,
                               PlayerSystem* player, int levelTurnCount,
                               int pickupsFound, int worldIndex) {
    int highlight    = (rngInt(0, 100, TILE_RNG_STREAM) < 50) ? 1 : 0;
    int rotationRoll = rngInt(0, 5, TILE_RNG_STREAM);

    setHexUVs(gridLayout, gridIdx, highlight, rotationRoll);

    // matches the binary's `operator new(0x498); thunk_FUN_10003ccc8(...)`:
    // the SnagContent ctor handles all the kind-specific stat scaling and
    // sprite UV picking via snags.h's bestiary table.
    SnagContent* sc = new SnagContent();
    sc->init(kind, this, player, levelTurnCount, pickupsFound, worldIndex);
    snagContent = sc;

    // FUN_100012ad8: push the new SnagContent into the tracking vector so
    // transformToSnag's "free the prior allocation" pass can find it. matches the
    // binary's exact-pointer dedup (skip if already in the vector).
    bool alreadyTracked = false;

    for (SnagContent* existing : trackedContent) {
        if (existing == sc) {
            alreadyTracked = true;
            break;
        }
    }

    if (!alreadyTracked) {
        trackedContent.push_back(sc);
    }
}

// reconstructed from Ghidra FUN_100013f04.
//
// saved-game restore tile builder. same hex-UV path as setVisual, but with
// explicit mirror / rotationStep (the saved values, not RNG rolls), and
// explicit content / snag stats supplied by the snapshot. used by
// GameBoard::restoreFromSnapshot for rack, placed, and reserve tiles.
void TileObject::setFromSnapshot(int gridLayout, int gridIdx, int mirror,
                                 int rotationStep, uint32_t contentType,
                                 int contentMagnitude,
                                 const uint32_t snagFields[6],
                                 const std::vector<int64_t>* decorations,
                                 PlayerSystem* player) {
    setHexUVs(gridLayout, gridIdx, mirror, rotationStep);

    // content: allocate + init from (type, magnitude), replacing any existing
    // TileContent. binary allocates the new one first, then frees the old via
    // vtable[1], then assigns; order preserved here.
    if (contentType != 0) {
        TileContent* tc = new TileContent();
        tc->init(contentType, contentMagnitude, nullptr);   // = thunk_FUN_1000148a8

        if (content != nullptr) {
            delete content;
        }

        content = tc;
    }

    // snag: allocate + initExplicit from the saved stats, then attach (which
    // also registers it in trackedContent). matches FUN_10003d614 + FUN_1000136bc.
    if (snagFields[0] != 0) {
        SnagContent* sc = new SnagContent();
        sc->initExplicit(snagFields[0], (int)snagFields[1], (int)snagFields[2],
                         (int)snagFields[3], snagFields[4], snagFields[5],
                         this, player);
        attachSnag(sc);
    }

    // decorations: replay each saved (kind, value) pair via pushDecoration
    // (flag 0). entries are packed int64 (kind | value << 32).
    if (decorations != nullptr) {

        for (int64_t packed : *decorations) {
            const int decoKind  = (int)(packed & 0xFFFFFFFF);
            const int decoValue = (int)(packed >> 32);
            pushDecoration(decoKind, decoValue, 0);
        }
    }
}

// reconstructed from Ghidra FUN_1000135d0.
//
// replace snagContent with a brand-new SnagContent of the given snag kind. the
// old snagContent gets freed; the trackedContent vector is collapsed (binary
// shrinks the vector to 0 elements before pushing the new one). then the new
// SnagContent gets its position synced to the tile and revive() starts its
// pop-in.
void TileObject::transformToSnag(uint32_t snagKind, PlayerSystem* player,
                                 int levelTurnCount, int pickupsFound, int worldIndex) {
    if (snagContent) {
        delete snagContent;
        snagContent = nullptr;
    }

    // binary shrinks vector to size 0 (end = begin).
    trackedContent.clear();

    SnagContent* sc = new SnagContent();
    sc->init(snagKind, this, player, levelTurnCount, pickupsFound, worldIndex);
    snagContent = sc;

    trackedContent.push_back(sc);

    // sync sub-object position via SnagContent::setPosition (port of vtable[4]).
    sc->setPosition(mainQuad.posX, mainQuad.posY);

    // FUN_1000135d0 ends with movableActorRevive(snag): start the new snag's
    // pop-in (scaleOutT = -2, hidden until onScaleOut ramps it back in).
    sc->revive();
}

// FUN_100013450. call MovableActor::killAndFade (FUN_100038e8c) on
// content + snagContent if non-null and visible. result: tile renders as
// a blank hex (face stays, inner icon fades to invisible).
void TileObject::erase() {

    if (content && content->visible) {
        content->killAndFade();
    }

    if (snagContent && snagContent->visible) {
        snagContent->killAndFade();
    }
}

// reconstructed from Ghidra FUN_100013540. swap the existing TileContent
// for a new one (delete via the vtable's deleting-dtor path = vtable[1] +
// operator delete; new(0x178) + init).
void TileObject::setTileContent(uint32_t type, int magnitude) {

    if (content) {
        // the binary dispatches vtable[1] (placement dtor) on the existing content.
        // C++ delete drives the same destruction sequence.
        delete content;
    }

    content = new TileContent();
    content->init(type, magnitude, &gridCol);

    // setPosition propagates from this tile's posXY (vtable[4] override
    // applies the per-class positioning offset).
    content->setPosition(mainQuad.posX, mainQuad.posY);

    content->revive();
}

// reconstructed from Ghidra FUN_1000136bc. attach a freshly-chased snag to
// this tile's snagContent slot. called from SnagContent::update when a move
// animation completes; without this hook, sendToward leaves the snag
// detached from every tile and combat queries can never find it.
//
// the binary's two paths:
//   1. existing snag alive: call FUN_100013728 (overlap path), invokes
//      FUN_10003df70 (snag-vs-snag interaction), removes the incoming snag
//      from oldParent's trackedContent, then calls vtable[1] on the
//      incoming snag (= deleting destructor). i.e. the incoming snag is
//      consumed when the target is already occupied.
//   2. empty (or dead snag in slot): destruct any dead remnant, set
//      snagContent = sc, push to trackedContent.
//
// the "collision" path is rare: happens when two snags converge on the
// same tile in the same turn. for chase, the common case is the empty
// branch.
void TileObject::attachSnag(SnagContent* sc) {

    if (snagContent && snagContent->visible) {
        // collision path. binary: FUN_10003df70(old, sc); FUN_100013778(this, sc);
        // delete sc (vtable[1]). the surviving snag absorbs the incoming
        // one's stats (and identity, if the surviving is a generic).
        //
        // achievement fan-out (= binary's FUN_10004e2c0). placed before
        // the merge so the surviving snag's pre-merge type is still
        // available; mirrors the binary's call order.
        {
            AchievementTracker& tracker = getGame()->achievementTracker();
            int typeA = snagContent->type;
            int typeB = sc->type;

            // "Our Powers Combined": both snags are special-typed.
            if (typeA != 1 && typeB != 1) {
                tracker.increment(AchievementId::OurPowersCombined);
            }

            // "The Wages of Truth": Honesty (0x6C) merged with a generic
            // snag (1), in either direction.
            if ((typeA == 0x6C && typeB == 1)
                || (typeA == 1    && typeB == 0x6C)) {
                tracker.increment(AchievementId::TheWagesOfTruth);
            }
        }

        // absorb the incoming snag's stats / identity / scratch fields.
        snagContent->mergeFrom(*sc);

        // remove sc from this tile's trackedContent (binary's
        // FUN_100013778), then free it (vtable[1]).
        auto& vec = trackedContent;
        vec.erase(std::remove(vec.begin(), vec.end(), sc), vec.end());

        delete sc;
        return;
    }

    // empty branch. matches the binary's sequence after the `if (lVar1
    // != 0 && visible)` gate:
    //   FUN_100013778(this, snagContent);     remove the dead remnant from
    //                                          trackedContent (no-op if not
    //                                          present).
    //   if (snagContent) delete snagContent;  free the dead one.
    //   snagContent = sc;                      attach new one.
    //   FUN_100012ad8(this, sc);               push to trackedContent (dedup).
    if (snagContent) {
        auto& vec = trackedContent;
        vec.erase(std::remove(vec.begin(), vec.end(), snagContent), vec.end());
        delete snagContent;
    }

    snagContent = sc;

    // push to trackedContent (skip if already present, matches
    // FUN_100012ad8's leading dedup loop).
    bool alreadyTracked = false;

    for (SnagContent* existing : trackedContent) {

        if (existing == sc) {
            alreadyTracked = true;
            break;
        }
    }

    if (!alreadyTracked) {
        trackedContent.push_back(sc);
    }
}

// reconstructed from Ghidra FUN_100013870 (insert) + FUN_1000140b8 (alloc).
//
// sorted-by-kind doubly-linked-list insert into the decoration list at
// three per-kind paths (UV / size / pos / sound), plus an existing-
// node update path that toggles `suppressed` and re-seeds the alpha lerp.
//
// never reached on level 1 (the only Phase 3 caller is populateRack's
// `hasSnagInBoard(0x15) -> pushDecoration(rack[0], 1, 0, 0)` branch, and
// there's no Darkness snag in level 1's reserve), so this is forward-looking
// work for Phase C combat / Darkness encounters.
void TileObject::pushDecoration(int kind, int value, int flag) {
    // 1. sound trigger (kind=0 -> 0x3C, kind=2 -> 0x3D; kind=1 and any
    //    unknown kinds skip the trigger but still continue into the body,
    //    matches binary's `if (param_2 != 0) goto LAB_1000138d4` flow which
    //    skips the sound for kind != 0/2 but doesn't return).
    if (kind == 0) {
        Game* g = getGame();

        if (g) {
            g->soundQueue.trigger(0x3C);
        }
    } else if (kind == 2) {
        Game* g = getGame();

        if (g) {
            g->soundQueue.trigger(0x3D);
        }
    }

    // 2. sorted-list scan: find the first node whose kind is >= the new kind
    //    (the list stays sorted ascending, one node per kind).
    auto it = decorations.begin();

    while (it != decorations.end() && it->kind < kind) {
        ++it;
    }

    // 3. existing-node update path (a node of this kind already exists).
    if (it != decorations.end() && it->kind == kind) {

        if (it->suppressed) {
            it->suppressed = false;
            it->alphaT     = 1.0f - it->alphaT;            // mirror progress
        }

        if (kind != 2) {
            return;                                        // kind 0 / 1: just toggled
        }

        // kind 2: bump value, redraw digits, sync ColorTint position.
        it->value += value;
        it->colorTint.setNumber(it->value, 0, 1);
        it->colorTint.setPosition(it->iconSubQuad.posX + it->subOffsetX,
                                  it->iconSubQuad.posY + it->subOffsetY,
                                  1);
        return;
    }

    // 4. insert a new node before `it` (keeps the list sorted by kind). emplace
    //    value-initializes the DecorationValue: zeros the scalars and runs the
    //    Quad / ColorTint sub-object constructors.
    DecorationValue& d = *decorations.emplace(it);

    // 5. baseline field init.
    d.kind       = kind;
    d.alphaT     = 0.0f;
    d.suppressed = false;
    d.iconSubQuad.setAlpha(0);
    d.iconSubQuad.posX = mainQuad.posX;
    d.iconSubQuad.posY = mainQuad.posY;
    d.colorTint.init();
    d.colorTint.setAlpha(0);
    d.value      = 0;

    // 6. per-kind UV / size / vertex offset.
    if (kind == 0) {
        bool snagAlive = (snagContent != nullptr) && snagContent->visible;
        if (snagAlive || flag != 0) {
            d.iconSubQuad.setTexCoords(0.3876953f, 0.0f, 0.499f, 0.127f);
        } else {
            d.iconSubQuad.setTexCoords(0.27539063f, 0.0f, 0.387f, 0.127f);
        }

        d.iconSubQuad.setSize(0.178125f, 0.203125f);
        d.iconSubQuad.addVertexOffset(0.0f, 0.0f);
        // subOffsetX/Y stay 0 (value-initialized by emplace).
    } else if (kind == 1) {
        d.iconSubQuad.setTexCoords(0.5f, 0.0f, 0.574f, 0.0947f);
        d.iconSubQuad.setSize(0.11875f, 0.1515625f);
        d.iconSubQuad.addVertexOffset(0.00625f, 0.0078125f);
        // subOffsetX/Y stay 0.
    } else if (kind == 2) {
        // (the trailing "addVertexOffset for kind=0/1" branch doesn't run
        // for kind=2; binary gotos out via LAB_100013b34.)
        d.iconSubQuad.setTexCoords(0.0f, 0.14257813f, 0.125f, 0.211f);
        d.iconSubQuad.setSize(0.2f, 0.109375f);
        d.iconSubQuad.addVertexOffset(0.0f, -0.09375f);
        d.value      = value;
        d.subOffsetX = 0.0f;
        d.subOffsetY = -0.0906f;          // not the -0.09375 addVertexOffset above
        d.colorTint.setNumber(value, 0, 1);
        d.colorTint.setPosition(d.iconSubQuad.posX + d.subOffsetX,
                                       d.iconSubQuad.posY + d.subOffsetY,
                                       1);
    }
    // kind not in {0, 1, 2}: no per-kind UV/size setup. matches binary's
    // `if (param_2 != 2) return;` early-return inside the kind=1/2 branch
    // (effectively letting the new node ship with default-only state).
}
