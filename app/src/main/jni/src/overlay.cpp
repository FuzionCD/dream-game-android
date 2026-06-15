#include "overlay.h"
#include "game.h"
#include "renderer.h"
#include <cmath>
#include <SDL.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void Overlay::init(float virtualHeight) {
    visible = false;
    opening = false;
    progress = 0.0f;
    targetY = 0.0f;
    screenHeight = virtualHeight;

    // the quads are full-width black bars, sized to cover the screen.
    // in the game's coordinate system, width = 1.0, height = virtualHeight.
    // they start off-screen (above and below) and slide inward.
    // because Game is allocated with SDL_malloc (not new), constructors
    // don't run for member objects. manually initialize all Quad fields
    // that the constructor would normally set.
    topQuad = Quad();
    bottomQuad = Quad();

    topQuad.setSize(1.0f, virtualHeight);
    topQuad.setColor(0, 0, 0, 255);
    topQuad.posX = 0.5f;
    topQuad.posY = -virtualHeight * 0.5f;

    bottomQuad.setSize(1.0f, virtualHeight);
    bottomQuad.setColor(0, 0, 0, 255);
    bottomQuad.posX = 0.5f;
    bottomQuad.posY = virtualHeight + virtualHeight * 0.5f;
}

// FUN_10001061c. the formula:
//   progress += (dt * 2) * speedTable[direction]
//   speedTable[0] = -1.0 (closing), speedTable[1] = 1.0 (opening)
//   progress = clamp(progress, 0, 1)
//   t = 0.5 - cos(progress * pi) * 0.5     (cosine easing)
//   quad1.posY = quad1.height * -0.5 + targetY * t
//   quad2.posY = (virtualHeight + quad1.height * 0.5) - (virtualHeight - targetY) * t
//   if closing and progress <= 0: visible = false
void Overlay::update(float dt) {

    if (!visible) {
        return;
    }

    // advance progress with direction-dependent speed
    if (opening) {
        progress += dt * 2.0f * OVERLAY_SPEED_OPEN;
    } else {
        progress += dt * 2.0f * OVERLAY_SPEED_CLOSE;
    }

    // clamp to 0-1
    if (progress < 0.0f) {
        progress = 0.0f;
    }

    if (progress > 1.0f) {
        progress = 1.0f;
    }

    // cosine easing: 0 at progress=0, 1 at progress=1
    float t = 0.5f - cosf((float)(progress * M_PI)) * 0.5f;

    // move quads
    // quad1 slides down from above, quad2 slides up from below
    float halfH = topQuad.height * 0.5f;

    topQuad.posY = -halfH + targetY * t;
    bottomQuad.posY = (screenHeight + halfH) - (screenHeight - targetY) * t;

    // if closing and fully closed, hide
    if (!opening && progress <= 0.0f) {
        visible = false;
    }
}

// FUN_1000105ec.
void Overlay::draw() {

    if (!visible) {
        return;
    }

    // the original ios code draws with texture 0 bound and GL_TEXTURE_2D enabled.
    // on iOS (PowerVR), this produces vertex color. on android GPUs, sampling
    // an unbound texture often produces transparent pixels. we disable texturing
    // explicitly for solid-color draws, which is the correct cross-platform approach.
    bindTexture(0);
    glDisable(GL_TEXTURE_2D);

    topQuad.draw();
    bottomQuad.draw();

    glEnable(GL_TEXTURE_2D);
}

// FUN_10001070c.
void Overlay::start(float height) {
    visible = true;
    targetY = height;
    opening = true;
    progress = 0.0f;
}

// FUN_100010740.
void Overlay::reset() {
    opening = false;
}
