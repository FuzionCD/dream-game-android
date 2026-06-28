#pragma once

#include "quad.h"
#include <GLES/gl.h>

// reconstructed from Ghidra:
//   draw:  FUN_1000105ec
//   update: FUN_10001061c
//   start: FUN_10001070c
//   reset: FUN_100010740
//
// the overlay is a pair of quads that slide in from top/bottom
// to create a screen transition effect (like closing/opening curtains).
// embedded in the game struct at offset 0x2E178, size 0x1C8 bytes.
//
// the animation uses cosine easing: t = 0.5 - cos(progress * pi) * 0.5

// speed table from DAT_100059dc0 in binary:
//   [0] = -1.0 (close direction, negative = reverse)
//   [1] =  1.0 (open direction)
//   [2] =  pi  (used for cosine easing)
//   [3] =  0.1 (not used in overlay, possibly elsewhere)
// update() does: progress += dt * 2 * speedTable[direction], so opening counts
// progress up at +1.0 and closing counts it back down at -1.0.
#define OVERLAY_SPEED_OPEN   1.0f
#define OVERLAY_SPEED_CLOSE  -1.0f

class Overlay {
public:
    void init(float virtualHeight);

    // FUN_10001061c: advance the transition animation.
    void update(float dt);

    // FUN_1000105ec: draw the overlay quads (disables texturing first).
    void draw();

    // FUN_10001070c: start an opening (sliding-in) transition.
    void start(float targetHeight);

    // FUN_100010740: clear `opening` so update() switches to the reveal
    // slide-out; the board stays frozen until then.
    void reset();

    // is the overlay currently visible?
    [[nodiscard]] bool isVisible() const { return visible; }

    // is a transition actively closing the screen? true from start() until the
    // transition fires reset() to begin the reveal slide-out. this is the
    // binary's `opening` flag; callers gate "freeze the board" on it.
    [[nodiscard]] bool isOpening() const { return opening; }

    // has the opening transition fully completed? (progress >= 1.0 while opening)
    [[nodiscard]] bool isOpenComplete() const { return visible && opening && progress >= 1.0f; }

private:
    // binary layout: the struct lives at Game.overlay_ and the transition
    // driver (FUN_100045410) reads visible / opening / progress at these exact
    // offsets, so the field order is binary-exact.
    bool    visible;
    Quad    topQuad;
    Quad    bottomQuad;
    bool    opening;        // (true = sliding in, false = sliding out)
    float   progress;       // (0..1)
    float   targetY;        // (target y the quads slide to)
    float   screenHeight;   // (non-binary convenience; the binary reads
                            //          DAT_10007ddb8 directly. fills the slot's
                            //          4 trailing pad bytes.)
};
