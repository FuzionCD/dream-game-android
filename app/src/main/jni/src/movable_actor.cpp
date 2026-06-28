#include "movable_actor.h"
#include <cmath>
#include <cstring>

// reconstructed from Ghidra FUN_100038b18.
//
// the binary's parent argument is `undefined8 *param_2`, a pointer to a
// packed (gridCol, gridRow) int pair (8 bytes). the ctor dereferences once
// (the binary copies `*param_2` into gridCol/gridRow) so the pointed-to
// coords get cached in the base.
// observed call sites pass either &local_zero (PlayerSystem, SnagContent)
// or `&parentTile->gridCol` (TileContent), so the cached coords are 0
// pre-commit and re-synced by FUN_10001349c (= TileObject::setGridCoord)
// once the parent tile drops onto a hex.
void MovableActor::initBase(const void* parentCoordPtr) {
    // C++ implicit vtable pointer is set by the compiler-generated
    // ctor; the binary's explicit vtable assignment is replaced by C++
    // virtual dispatch on onFade / onScaleOut and the destructor chain.

    visible = true;

    baseQuad = Quad();                               // FUN_100007d78

    if (parentCoordPtr) {
        const int* p = static_cast<const int*>(parentCoordPtr);
        gridCol = p[0];
        gridRow = p[1];
    } else {
        gridCol = 0;
        gridRow = 0;
    }

    spawnT     = 1.0f;                               // (no spawn anim)
    spawnFromX = 0.0f; spawnFromY = 0.0f;
    spawnToX   = 0.0f; spawnToY   = 0.0f;

    moveT          = 1.0f;                           // (no move anim)
    moveFromX = 0.0f;
    moveFromY = 0.0f;

    // movement queue: std::list default-constructs to an empty list with
    // self-aliased sentinel (matches the binary's empty-state layout).
    // push_back queues newest target; front() = next target to walk toward.
    moveQueue.clear();
    moveStepRate = 0.0f;

    fadeT     = 1.0f;
    scaleOutT = 1.0f;
}

// reconstructed from Ghidra FUN_100038e8c. onFade is vtable[6]; the binary
// dispatches it once here with fadeT=0 after starting the fade.
void MovableActor::killAndFade() {
    visible = false;
    fadeT   = 0.0f;
    onFade(0.0f);
}

// reconstructed from Ghidra FUN_100038ea4. onScaleOut is vtable[7]; the binary
// dispatches it once here with scaleOutT=0, hiding the actor before the pop-in.
void MovableActor::revive() {
    visible   = true;
    scaleOutT = -2.0f;
    onScaleOut(0.0f);
}

// reconstructed from Ghidra FUN_100038ec4, vtable[4] base. PlayerSystem
// doesn't override this; TileContent / SnagContent do. skipLayout&1 (set only
// during the spawn-in stage) skips the pixel snap so the actor can glide
// sub-pixel; it settles onto the grid once spawn/move completes with the flag
// clear.
void MovableActor::setPosition(float x, float y, int skipLayout) {
    baseQuad.posX = x;
    baseQuad.posY = y;

    if ((skipLayout & 1) == 0) {
        baseQuad.snapToPixelGrid();
    }
}

// reconstructed from Ghidra FUN_100038f64. base setAlpha, just propagates
// onto baseQuad. derived classes (TileContent / SnagContent) override to
// also dim their stat displays and ColorTints. PlayerSystem inherits this
// base so the character avatar dims via the same hook.
void MovableActor::setAlpha(uint8_t alpha) {
    baseQuad.setAlpha(alpha);
}

// reconstructed from Ghidra FUN_100038db4. one-shot lurch toward a hex
// neighbour. consumed by FUN_100025dcc (snag death) where the player and
// dying snag both call this with opposite direction codes; the visual
// is a small bump toward each other on the killing blow.
//
// constants:
//   DAT_10005a248 = 1.0471975 (pi / 3 = 60 degrees in radians)
//   DAT_10005a24c = 0.07      (lurch distance in screen units)
void MovableActor::triggerBumpAnim(int direction) {
    constexpr float HEX_ANGLE_STEP = 1.0471976f;   // 60 degrees in radians
    constexpr float BUMP_DISTANCE  = 0.07f;

    spawnT     = 0.0f;
    spawnFromX = baseQuad.posX;
    spawnFromY = baseQuad.posY;

    float angle = (float)(direction - 1) * HEX_ANGLE_STEP;
    spawnToX    = spawnFromX + BUMP_DISTANCE * std::cos(angle);
    spawnToY    = spawnFromY + BUMP_DISTANCE * std::sin(angle);
}

// reconstructed from Ghidra FUN_100038e10. push a (posPair, gridPair) target
// onto the move queue and kick the move animation. shared base method;
// PlayerSystem and SnagContent both use this directly via inheritance.
//
// note: moveStepRate is multiplied by 1.15 (vs the binary's bare cast). this
// is a deliberate pacing tweak from task #72 that smooths multi-step moves.
void MovableActor::stepToward(const float* posPair, const int* gridPair) {
    moveT = 0.0f;

    moveFromX = baseQuad.posX;
    moveFromY = baseQuad.posY;

    moveQueue.push_back(MoveTarget(posPair[0], posPair[1]));

    gridCol = gridPair[0];
    gridRow = gridPair[1];

    moveStepRate = static_cast<float>(moveQueue.size()) * 1.15f;
}

// reconstructed from Ghidra FUN_100038bac, base 4-stage state-machine update.
// each stage runs in sequence: only the first stage whose timer is < 1.0
// advances per frame (returns early after that stage). PlayerSystem layers
// its character-pulse on top of this; TileContent/SnagContent inherit the
// same body unchanged.
void MovableActor::update(float dt) {
    constexpr float STAGE_DURATION = 0.2f;       // DAT_10005a23c, fade/scale
    constexpr float MOVE_DURATION  = 0.3f;       // DAT_10005a240, base move
    constexpr float MOVE_PI        = 3.1415927f; // DAT_10005a244

    // -- stage 1: spawn-in (lerp from spawnFrom to spawnTo with eased shape) --
    if (spawnT < 1.0f) {
        float t = spawnT + dt + dt;

        if (t > 1.0f) {
            t = 1.0f;
        }

        spawnT = t;
        // shape function: 1 - (2t-1)^2 (peaks at t=0.5, zero at edges)
        float u     = (t - 0.5f) + (t - 0.5f);
        float shape = 1.0f - u * u;
        float lerpedX = (1.0f - shape) * spawnFromX + shape * spawnToX;
        float lerpedY = (1.0f - shape) * spawnFromY + shape * spawnToY;
        setPosition(lerpedX, lerpedY, 1);   // spawn-in: skip snap/relayout (binary mov w2,#1)
        return;
    }

    // -- stage 2: move animation (cos-eased lerp from moveFrom to queueFront target) --
    if (moveT < 1.0f) {
        float t = moveT + (dt / MOVE_DURATION) * moveStepRate;

        if (t > 1.0f) {
            t = 1.0f;
        }

        moveT = t;

        // queue front (oldest end = FIFO front) holds the active move target.
        const MoveTarget& target = moveQueue.front();
        float targetX = target.posX;
        float targetY = target.posY;

        // cos-eased u: starts slow, peaks at midpoint, slows at end.
        float u = 0.5f - std::cos(t * MOVE_PI) * 0.5f;
        float lerpedX = u * targetX + (1.0f - u) * moveFromX;
        float lerpedY = u * targetY + (1.0f - u) * moveFromY;
        setPosition(lerpedX, lerpedY, 0);   // move: snap + relayout (binary mov w2,#0)

        if (moveT >= 1.0f) {
            // move complete: copy the target into moveFromX/Y so the next
            // move's lerp starts from where we just landed, then pop.
            moveFromX = targetX;
            moveFromY = targetY;

            moveQueue.pop_front();

            if (!moveQueue.empty()) {
                // more queued: restart move animation toward the next target.
                moveT = 0.0f;
            }
        }
        return;
    }

    // -- stage 3: fade-out --
    if (fadeT < 1.0f) {
        float t = fadeT + dt / STAGE_DURATION;

        if (t > 1.0f) {
            t = 1.0f;
        }

        fadeT = t;
        onFade(t);
        return;
    }

    // -- stage 4: scale-out --
    if (scaleOutT < 1.0f) {
        float t = scaleOutT + dt / STAGE_DURATION;

        if (t > 1.0f) {
            t = 1.0f;
        }

        scaleOutT = t;

        // skip the dispatch when the clamped value went negative (0x100038d80 fcmp / 0x100038d84 b.lt)
        if (t < 0.0f) {
            return;
        }

        onScaleOut(t);
        return;
    }
}
