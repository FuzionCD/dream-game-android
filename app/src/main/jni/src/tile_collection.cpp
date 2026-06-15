#include "tile_collection.h"
#include "game.h"
#include <SDL.h>
#include <GLES/gl.h>
#include <cstring>

static const float SIZE_DIVISOR = 640.0f;  // DAT_10005a54c and DAT_10005a548

// reconstructed from FUN_10004c014
void TileCollection::init() {
    items.clear();
    modes.clear();
    itemExtra.clear();
    displayParams.clear();
    additionalData.clear();
    stagingValue = 0;
    scale = 1024.0f;
    pad80 = 0.0f;
    totalWidth = 0.0f;
    flag88 = false;
    posX = 0.0f;
    posY = 0.0f;
    rotated = false;
}

// reconstructed from FUN_100008238
// adds an offset to all 4 vertex positions of a quad
static void addVertexOffset(Quad& q, float dx, float dy) {

    for (int i = 0; i < 4; i++) {
        q.vertices[i].x += dx;
        q.vertices[i].y += dy;
    }
}

// reconstructed from FUN_10004c150
// recalculates all item sizes and positions based on modes
void TileCollection::recalculate() {
    if (items.empty()) {
        return;
    }

    // first pass: calculate total fixed width (modes 0 and 2)
    float fixedWidth = 0.0f;
    int stretchCount = 0;

    for (size_t i = 0; i < items.size(); i++) {
        int mode = modes[i];

        if (mode == 0 || mode == 2) {
            // fixed width from UV span
            float uvW = items[i].quad.vertices[3].u - items[i].quad.vertices[0].u;
            float w = (uvW * scale) / SIZE_DIVISOR;
            fixedWidth += w;
        } else if (mode == 1) {
            stretchCount++;
        }
    }

    // second pass: set sizes and position each item left-to-right
    float xAccum = 0.0f;

    for (size_t i = 0; i < items.size(); i++) {
        int mode = modes[i];
        float w, h;

        // height from UV span
        float uvH = items[i].quad.vertices[3].v - items[i].quad.vertices[0].v;
        h = (uvH * scale) / SIZE_DIVISOR;

        if (mode == 1) {
            // stretchable: width fills remaining space
            // for a 3-part button, this is the text label width
            // (total width is set externally, stretch fills the gap)
            float uvW = items[i].quad.vertices[3].u - items[i].quad.vertices[0].u;
            w = (uvW * scale) / SIZE_DIVISOR;

            // if totalWidth was set externally, stretch to fill
            if (totalWidth > 0.0f && stretchCount > 0) {
                float remaining = totalWidth - fixedWidth;
                w = remaining / (float)stretchCount;
            }
        } else {
            // fixed width from UV
            float uvW = items[i].quad.vertices[3].u - items[i].quad.vertices[0].u;
            w = (uvW * scale) / SIZE_DIVISOR;
        }

        // reset vertex positions to unit quad then set proper size
        items[i].quad.setSize(w, h);

        // setSize centers the quad at origin, so shift by xAccum + w/2 to
        // place its left edge at xAccum (left-to-right layout, y untouched).
        addVertexOffset(items[i].quad, xAccum + w * 0.5f, 0.0f);

        xAccum += w;
    }

    // update cached total if not set externally
    if (totalWidth <= 0.0f) {
        totalWidth = xAccum;
    }
}

// reconstructed from FUN_10004c310
void TileCollection::pushItem(float displayParam, float* pixelPos, float* pixelSize,
                               int mode, int extraData) {
    TileIcon item;
    item.quad = Quad();

    // set UV from pixel coordinates
    float invScale = 1.0f / scale;
    float u0 = pixelPos[0] * invScale;
    float v0 = pixelPos[1] * invScale;
    float u1 = (pixelPos[0] + pixelSize[0]) * invScale;
    float v1 = (pixelPos[1] + pixelSize[1]) * invScale;
    item.quad.setTexCoords(u0, v0, u1, v1);

    // push to all parallel vectors
    items.push_back(item);
    modes.push_back(mode);
    itemExtra.push_back((int64_t)extraData);
    displayParams.push_back((int)displayParam);
    additionalData.push_back(stagingValue);

    // recalculate all sizes and positions (from FUN_10004c150, called after each push)
    recalculate();
}

// reconstructed from FUN_10004c09c
void TileCollection::draw() {

    if (items.empty()) {
        return;
    }

    if (rotated) {
        glPushMatrix();
        glRotatef(90.0f, 0.0f, 0.0f, 1.0f);
    }

    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    for (size_t i = 0; i < items.size(); i++) {
        items[i].quad.draw();
    }

    glPopMatrix();

    if (rotated) {
        glPopMatrix();
    }
}

// reconstructed from FUN_10004c61c
float TileCollection::getWidth() {
    return totalWidth;
}

// reconstructed from FUN_10004c93c
float TileCollection::getLeftX() {

    if (rotated) {
        return -posY - getWidth();
    }

    return posX;
}

// reconstructed from FUN_10004c4f8
void TileCollection::setPosition(float leftX, float y) {
    posX = leftX;
    posY = y;
}

// reconstructed from FUN_10004c8cc
// checks X bounds from getLeftX/getWidth, Y bounds from posY and height param.
// the original's Y check logic with fadeInProgress doesn't translate directly
// (likely due to coordinate differences between iOS and our projection), so
// we use the frame's actual position and height for Y bounds.
bool TileCollection::hitTest(float touchX, float touchY, float height) {
    float left = getLeftX();
    float right = left + getWidth();

    if (touchX < left || touchX > right) {
        return false;
    }

    if (touchY < posY - height * 0.5f || touchY > posY + height * 0.5f) {
        return false;
    }

    return true;
}

// reconstructed from FUN_10004c84c
void TileCollection::setAlpha(uint8_t alpha) {

    for (size_t i = 0; i < items.size(); i++) {

        for (int v = 0; v < 4; v++) {
            uint32_t c = items[i].quad.vertices[v].color;
            c = (c & 0x00FFFFFF) | ((uint32_t)alpha << 24);
            items[i].quad.vertices[v].color = c;
        }
    }
}
