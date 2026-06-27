#include "world.h"
#include "game.h"
#include "random.h"
#include "renderer.h"
#include "sound_queue.h"
#include <SDL.h>
#include <GLES/gl.h>
#include <cmath>
#include <cstring>
#include <iterator>

// constants from binary
static const float GRID_SPACING = 0.15625f;
static const float GRID_CENTER_X = 0.578125f;  // DAT_10005a6cc
static const float GRID_OFFSET_Y = 0.078125f;  // DAT_10005a6c8
static const float BG_EXTRA_H = 0.1f;          // DAT_10005a6c4
// portrait table moved to portrait_table.h (shared with PlayerSystem so the
// in-game character token uses the same sprite mapping).
#include "portrait_table.h"

// helper: set alpha on all 4 vertices, preserving RGB
static void setQuadAlpha(Quad& q, uint8_t alpha) {

    for (int i = 0; i < 4; i++) {
        uint32_t c = q.vertices[i].color;
        c = (c & 0x00FFFFFF) | ((uint32_t)alpha << 24);
        q.vertices[i].color = c;
    }
}

// helper: set all 4 vertices to the same color
static void setQuadColor(Quad& q, uint32_t rgba) {

    for (int i = 0; i < 4; i++) {
        q.vertices[i].color = rgba;
    }
}

// helper: hit test
static bool hitTestQuad(Quad& q, float px, float py) {
    float left  = q.vertices[0].x * q.scaleX + q.posX;
    float right = q.vertices[3].x * q.scaleX + q.posX;
    float top   = q.vertices[0].y * q.scaleY + q.posY;
    float bottom = q.vertices[3].y * q.scaleY + q.posY;

    return (px >= left && px <= right && py >= top && py <= bottom);
}

// helper: clamp float
static float clampf(float val, float lo, float hi) {

    if (val < lo) {
        return lo;
    }

    if (val > hi) {
        return hi;
    }

    return val;
}

// portrait UV setter is now setPortraitVisual() in portrait_table.cpp
// (shared with PlayerSystem).

// reconstructed from FUN_100055268 (World constructor)
void World::construct() {
    visible = false;
    active = false;

    // construct background overlay quad
    backgroundOverlay.quad = Quad();

    // construct 30 character portrait tiles (max)
    for (int i = 0; i < WORLD_MAX_TILES; i++) {
        tiles[i].quad = Quad();
    }

    // construct 3 difficulty buttons (from FUN_100055268)
    //
    // each button has:
    //   - unselectedFrame: TileCollection with 3 quads (left cap + mid + right cap)
    //   - selectedFrame: TileCollection with 3 quads (same layout, different UVs)
    //   - unselectedText: text label quad
    //   - selectedText: text label quad
    //
    // frame sub-quads from ui1.png (pixel coords x, y, w, h):
    //   all buttons share the same frame shapes, just different text labels
    //
    // unselected frame pieces: (718,246,26,68), (744,246,1,68), (745,246,26,68)
    // selected frame pieces:   (772,246,26,68), (798,246,1,68), (799,246,26,68)

    // pixel coords for frame pieces (same for all 3 buttons)
    float unselFrameLeft[2]  = {718.0f, 246.0f};
    float unselFrameMid[2]   = {744.0f, 246.0f};
    float unselFrameRight[2] = {745.0f, 246.0f};
    float selFrameLeft[2]    = {772.0f, 246.0f};
    float selFrameMid[2]     = {798.0f, 246.0f};
    float selFrameRight[2]   = {799.0f, 246.0f};
    float capSize[2]         = {26.0f, 68.0f};
    float midSize[2]         = {1.0f, 68.0f};

    // text label UVs first (need text width to size the frames)

    // button 0: Easy
    diffButtons[0].unselectedText.quad = Quad();
    diffButtons[0].unselectedText.quad.setTexCoords(0.921f, 0.963f, 1.0f, 1.0f);
    diffButtons[0].unselectedText.quad.setSize(0.127f, 0.059f);
    diffButtons[0].selectedText.quad = Quad();
    diffButtons[0].selectedText.quad.setTexCoords(0.921f, 0.925f, 1.0f, 0.962f);
    diffButtons[0].selectedText.quad.setSize(0.127f, 0.059f);

    // button 1: Normal
    diffButtons[1].unselectedText.quad = Quad();
    diffButtons[1].unselectedText.quad.setTexCoords(0.878f, 0.336f, 1.0f, 0.366f);
    diffButtons[1].unselectedText.quad.setSize(0.195f, 0.048f);
    diffButtons[1].selectedText.quad = Quad();
    diffButtons[1].selectedText.quad.setTexCoords(0.878f, 0.894f, 1.0f, 0.924f);
    diffButtons[1].selectedText.quad.setSize(0.195f, 0.048f);

    // button 2: Hard
    diffButtons[2].unselectedText.quad = Quad();
    diffButtons[2].unselectedText.quad.setTexCoords(0.807f, 0.304f, 0.891f, 0.335f);
    diffButtons[2].unselectedText.quad.setSize(0.134f, 0.050f);
    diffButtons[2].selectedText.quad = Quad();
    diffButtons[2].selectedText.quad.setTexCoords(0.916f, 0.367f, 1.0f, 0.398f);
    diffButtons[2].selectedText.quad.setSize(0.134f, 0.050f);

    // build frames: all buttons have the same frame width of 0.3203125
    // (DAT_10005a6b8, passed to FUN_10004c990 in the original constructor)
    const float BUTTON_FRAME_WIDTH = 0.3203125f;

    for (int i = 0; i < 3; i++) {
        diffButtons[i].unselectedFrame.init();
        diffButtons[i].selectedFrame.init();

        // set the frame's total width so the middle section stretches correctly
        diffButtons[i].unselectedFrame.totalWidth = BUTTON_FRAME_WIDTH;
        diffButtons[i].selectedFrame.totalWidth = BUTTON_FRAME_WIDTH;

        // push 3 frame pieces: left cap (fixed), mid (stretch), right cap (fixed)
        diffButtons[i].unselectedFrame.pushItem(-1.0f, unselFrameLeft, capSize, 2, 0);
        diffButtons[i].unselectedFrame.pushItem(-1.0f, unselFrameMid, midSize, 1, 0);
        diffButtons[i].unselectedFrame.pushItem(-1.0f, unselFrameRight, capSize, 2, 0);

        diffButtons[i].selectedFrame.pushItem(-1.0f, selFrameLeft, capSize, 2, 0);
        diffButtons[i].selectedFrame.pushItem(-1.0f, selFrameMid, midSize, 1, 0);
        diffButtons[i].selectedFrame.pushItem(-1.0f, selFrameRight, capSize, 2, 0);
    }

    // position buttons (FUN_100055268):
    //   X left edge = xStart / 640.0 (DAT_10005a6bc)
    //   xStart: 11, 217, 423
    //   Y = DAT_10005a6c0 = 0.0203125
    //   + 0.15 yShift for modern android screens
    //
    //   text posX = frame center X (from FUN_10000aefc = getLeftX + getWidth * 0.5)
    //   text posY = frame Y + small per-button offset
    //   per-button Y offsets: 0.003125, -0.003125, -0.001563
    const float yShift = 0.15f;
    const float btnY = 0.0203125f + yShift;  // base 0.0203125 = DAT_10005a6c0
    const int xStarts[3] = { 11, 217, 423 };
    const float textYOffsets[3] = { 0.003125f, -0.003125f, -0.001563f };

    for (int i = 0; i < 3; i++) {
        float leftX = (float)xStarts[i] / 640.0f;

        // position frame collections at left edge X, button Y
        diffButtons[i].unselectedFrame.setPosition(leftX, btnY);
        diffButtons[i].selectedFrame.setPosition(leftX, btnY);

        // text posX = frame center X (FUN_10000aefc: getLeftX + getWidth * 0.5)
        float frameCenterX = diffButtons[i].unselectedFrame.getLeftX() +
                             diffButtons[i].unselectedFrame.getWidth() * 0.5f;

        // text posY = button Y + per-button offset
        float textPosY = btnY + textYOffsets[i];

        // write the unselected text position, then snap it to the pixel grid
        // (FUN_1000573a8 = snapToPixelGrid) before mirroring the snapped pair
        // onto the selected text quad (binary copies +0x1c88 -> +0x1d60 post-snap).
        diffButtons[i].unselectedText.quad.posX = frameCenterX;
        diffButtons[i].unselectedText.quad.posY = textPosY;
        diffButtons[i].unselectedText.quad.snapToPixelGrid();

        diffButtons[i].selectedText.quad.posX = diffButtons[i].unselectedText.quad.posX;
        diffButtons[i].selectedText.quad.posY = diffButtons[i].unselectedText.quad.posY;
    }

    SDL_Log("World::construct() complete");
}

// reconstructed from FUN_100056048
void World::draw() {

    if (!visible) {
        return;
    }

    // draw black background overlay (no texture)
    bindTexture(0);
    glDisable(GL_TEXTURE_2D);
    backgroundOverlay.quad.draw();
    glEnable(GL_TEXTURE_2D);

    // draw character portrait tiles (texture 8 = sheet1.png)
    bindTexture(8);

    if (tileCount > 0) {

        for (int i = 0; i < tileCount; i++) {
            tiles[i].quad.draw();
        }
    }

    // draw 3 difficulty selector buttons (texture 9 = ui1.png): frame first
    // via TileCollection, then the text label on top.
    bindTexture(9);

    for (int i = 0; i < 3; i++) {

        if (i == (int)worldIndex) {
            diffButtons[i].selectedFrame.draw();
            diffButtons[i].selectedText.quad.draw();
        } else {
            diffButtons[i].unselectedFrame.draw();
            diffButtons[i].unselectedText.quad.draw();
        }
    }
}

// reconstructed from FUN_100055970
void World::generate(uint32_t newWorldIndex, std::set<int>& tileTypeSet) {
    visible = true;
    active = true;
    characterSelected = false;
    selectedTile = -1;
    selectedCharType = 0;
    fadeInProgress = 0.0f;
    selAnimProgress = 0.0f;
    selStartX = 0.0f;
    selStartY = 0.0f;
    selTargetX = 0.0f;
    selTargetY = 0.0f;
    // carry the caller's worldIndex through (param_2), so the chosen
    // difficulty survives a back-to-character-select transition.
    worldIndex = newWorldIndex;

    float virtualHeight = Renderer::getVirtualHeight();

    // background overlay at +0x08
    backgroundOverlay.quad = Quad();
    setQuadAlpha(backgroundOverlay.quad, 0xFF);
    backgroundOverlay.quad.posX = 0.5f;
    backgroundOverlay.quad.posY = virtualHeight * 0.5f;
    backgroundOverlay.quad.setSize(1.1f, virtualHeight + BG_EXTRA_H);
    setQuadColor(backgroundOverlay.quad, 0xFF000000);

    // tile count = (clamped) filter-set size. binary clamps via the
    // WORLD_MAX_TILES cap in tiles[]; we mirror that here.
    tileCount = (int)tileTypeSet.size();

    if (tileCount > WORLD_MAX_TILES) {
        tileCount = WORLD_MAX_TILES;
    }

    if (tileCount <= 0) {
        return;
    }

    // calculate grid layout (sqrt-based)
    int gridCols = (int)sqrtf((float)tileCount);

    if (gridCols * gridCols < tileCount) {
        gridCols++;
    }

    int gridRows = 0;

    if (gridCols != 0) {
        gridRows = tileCount / gridCols;
    }

    if (tileCount - gridRows * gridCols == 1) {
        gridCols++;
    }

    gridRows = 0;

    if (gridCols != 0) {
        gridRows = tileCount / gridCols;
    }

    if (gridCols * gridRows < tileCount) {
        gridRows++;
    }

    // grid base Y and last-row centering offset.
    float gridBaseY = (virtualHeight - (float)gridRows * GRID_SPACING * 0.5f) -
                      (1.0f - ((float)gridCols * GRID_SPACING * 0.5f + 0.5f)) +
                      GRID_OFFSET_Y;
    float lastRowOffset = (float)(gridRows * gridCols - tileCount) * GRID_OFFSET_Y;

    for (int i = 0; i < tileCount; i++) {
        tiles[i].quad = Quad();

        // pop a random face ID from the filter set (binary: FUN_10000f800
        // = rngPopSet on stream 0). this consumes one ID per tile so
        // each tile shows a distinct unlocked face.
        const int rngIdx = rngInt(0, (int)tileTypeSet.size() - 1, 0);
        auto it = std::next(tileTypeSet.begin(), rngIdx);
        tileTypes[i] = *it;
        tileTypeSet.erase(it);

        // set up visual from portrait table using the popped ID.
        setPortraitVisual(tileTypes[i], tiles[i].quad);

        // compute grid position
        int row = 0;

        if (gridCols != 0) {
            row = i / gridCols;
        }

        int col = i - row * gridCols;

        // x position centered at GRID_CENTER_X (0.578125)
        float x = ((float)col - (float)gridCols * 0.5f) * GRID_SPACING + GRID_CENTER_X;

        if (row == gridRows - 1) {
            x += lastRowOffset;
        }

        // y position
        float y = gridBaseY + ((float)row - (float)gridRows * 0.5f) * GRID_SPACING;

        tiles[i].quad.posX = x;
        tiles[i].quad.posY = y;
    }

    // FUN_100055b94 with dt=0: runs the fade-in branch once at progress 0, so
    // every tile starts at alpha 0 (invisible) and fades in from there.
    update(0.0f, nullptr);

    SDL_Log("World::generate - %d tiles in %dx%d grid, worldIndex=%d",
            tileCount, gridCols, gridRows, newWorldIndex);
}

// apply a uniform alpha to every tile + difficulty button. shared by update()'s
// fade-in and tickFadeOut()'s fade-out so the two stay exact mirrors.
void World::applyContentAlpha(uint8_t alpha) {
    for (int i = 0; i < tileCount; i++) {
        setQuadAlpha(tiles[i].quad, alpha);
    }

    for (int b = 0; b < 3; b++) {
        diffButtons[b].unselectedFrame.setAlpha(alpha);
        diffButtons[b].selectedFrame.setAlpha(alpha);
        setQuadAlpha(diffButtons[b].unselectedText.quad, alpha);
        setQuadAlpha(diffButtons[b].selectedText.quad, alpha);
    }
}

// back-button fade-out: the exact reverse of update()'s fade-in ramp. drives
// fadeInProgress 1->0 at the same dt*2 rate and fades the content out to black.
// returns true once fully faded out. no binary equivalent (iOS has no back
// button); used by the character-select -> title back transition.
bool World::tickFadeOut(float dt) {
    fadeInProgress = clampf(fadeInProgress - dt * 2.0f, 0.0f, 1.0f);
    applyContentAlpha((uint8_t)(fadeInProgress * 255.0f));
    return fadeInProgress <= 0.0f;
}

// reconstructed from FUN_100055b94
void World::update(float dt, SoundQueue* soundQueue) {

    if (!visible) {
        return;
    }

    if (!characterSelected) {
        // phase 1: fade-in animation
        if (fadeInProgress < 1.0f) {
            fadeInProgress = clampf(fadeInProgress + dt * 2.0f, 0.0f, 1.0f);

            // fade all tiles + difficulty buttons in (alpha proportional to
            // progress). DAT_10005a6d4 = 255.0 (confirmed from binary).
            applyContentAlpha((uint8_t)(fadeInProgress * 255.0f));
            return;
        }

        // phase 2: interactive (fade complete)
        if (!getGame()) {
            return;
        }

        int touchState = getGame()->inputState();
        float touchX = getGame()->touchX();
        float touchY = getGame()->touchY();

        if (touchState == 1) {
            // touch began: hit test character tiles
            for (int i = 0; i < tileCount; i++) {

                if (hitTestQuad(tiles[i].quad, touchX, touchY)) {
                    selectedTile = i;
                    // depress effect: dim to alpha 0x96 (150)
                    setQuadAlpha(tiles[i].quad, 0x96);

                    if (soundQueue) {
                        soundQueue->trigger(7);
                    }

                    break;
                }
            }

            // if no tile hit, check difficulty buttons
            if (selectedTile == -1) {
                float btnHeight = 68.0f / 640.0f;  // frame height in game units

                for (int b = 0; b < 3; b++) {
                    TileCollection& frame = (b == (int)worldIndex)
                        ? diffButtons[b].selectedFrame
                        : diffButtons[b].unselectedFrame;

                    if (frame.hitTest(touchX, touchY, btnHeight)) {
                        worldIndex = (uint32_t)b;

                        if (soundQueue) {
                            soundQueue->trigger(6);
                        }

                        break;
                    }
                }
            }

        } else if (touchState == 0 && selectedTile != -1) {
            // touch released with a tile selected
            // restore alpha to full
            int prevSelected = selectedTile;

            setQuadAlpha(tiles[prevSelected].quad, 0xFF);

            // check if released on the same tile
            if (hitTestQuad(tiles[prevSelected].quad, touchX, touchY)) {
                // character confirmed!
                characterSelected = true;

                selectedCharType = tileTypes[selectedTile];

                // seed the fly-to-center animation: start at the tile's current
                // spot, target dead center.
                selAnimProgress = 0.0f;
                selStartX = tiles[selectedTile].quad.posX;
                selStartY = tiles[selectedTile].quad.posY;
                selTargetX = 0.5f;
                selTargetY = Renderer::getVirtualHeight() * 0.5f;

                SDL_Log("World: character %d confirmed (type %d, difficulty %d)",
                        selectedTile, selectedCharType, worldIndex);
            } else {
                // released off tile, cancel selection
                selectedTile = -1;
            }
        }

    } else {
        // phase 3: selection animation (from decompilation, 3 sub-phases)
        selAnimProgress = clampf(selAnimProgress + dt / 1.5f, 0.0f, 1.0f);
        float animProgress = selAnimProgress;

        if (animProgress >= 1.0f) {
            // animation complete, hide world
            visible = false;
            return;
        }

        // sub-phase 1: non-selected tiles fade out (0% to 50% of animation)
        float fadeT = clampf(animProgress * 2.0f, 0.0f, 1.0f);
        float cosT1 = 0.5f - cosf(fadeT * 3.14159f) * 0.5f;
        uint8_t tileAlpha = (uint8_t)((1.0f - cosT1) * 255.0f);

        for (int i = 0; i < tileCount; i++) {

            if (i != selectedTile) {
                setQuadAlpha(tiles[i].quad, tileAlpha);
            }
        }

        // fade difficulty buttons with the same alpha as non-selected tiles
        for (int b = 0; b < 3; b++) {
            diffButtons[b].unselectedFrame.setAlpha(tileAlpha);
            diffButtons[b].selectedFrame.setAlpha(tileAlpha);
            setQuadAlpha(diffButtons[b].unselectedText.quad, tileAlpha);
            setQuadAlpha(diffButtons[b].selectedText.quad, tileAlpha);
        }

        // sub-phase 2: selected tile moves to center over 30% to 70% of the
        // animation; moveT = clamp((progress - 0.3) / 0.4, 0, 1), cosine-eased.
        if (selectedTile >= 0 && selectedTile < tileCount) {
            float moveT = clampf((animProgress - 0.3f) / 0.4f, 0.0f, 1.0f);
            float cosT2 = 0.5f - cosf(moveT * 3.14159f) * 0.5f;

            tiles[selectedTile].quad.posX = selStartX * (1.0f - cosT2) + selTargetX * cosT2;
            tiles[selectedTile].quad.posY = selStartY * (1.0f - cosT2) + selTargetY * cosT2;
        }

        // sub-phase 3: background and selected tile fade out over 70% to 100%;
        // bgT = clamp((progress - 0.7) / 0.3, 0, 1).
        float bgT = clampf((animProgress - 0.7f) / 0.3f, 0.0f, 1.0f);
        float cosT3 = 0.5f - cosf(bgT * 3.14159f) * 0.5f;
        uint8_t bgAlpha = (uint8_t)((1.0f - cosT3) * 255.0f);

        setQuadAlpha(backgroundOverlay.quad, bgAlpha);

        if (selectedTile >= 0 && selectedTile < tileCount) {
            setQuadAlpha(tiles[selectedTile].quad, bgAlpha);
        }
    }
}
