#include "title_menu.h"
#include "game.h"
#include "renderer.h"
#include <SDL.h>
#include <GLES/gl.h>
#include "random.h"
#include <cstring>
#include <cmath>

// constants from binary (verified against data segments)
static const float ROTATION_CAP        = 360.0f;   // DAT_10005A518
static const float ROTATION_WRAP       = -360.0f;  // DAT_10005A51C
static const float CYCLE_PERIOD        = 6.2832f;  // DAT_10005A520 (2*pi)
static const float CYCLE_SPEED         = 1.5708f;  // DAT_10005A524 (pi/2)
static const float DIST_NORMALIZE      = 1.7f;     // DAT_10005A528
static const float PARTICLE_HALF_SIZE  = 0.078125f; // DAT_10005A52C
static const float COLOR_THRESH_1      = 240.0f;   // DAT_10005A530
static const float COLOR_THRESH_2      = 180.0f;   // DAT_10005A534
static const float ROTATION_SPEED_MULT = 80.0f;    // DAT_10005A538
static const float FADE_COS_MULT       = 3.1416f;  // DAT_10005A53C (pi)
static const float FADE_START_ALPHA    = 255.0f;   // DAT_10005A540

// initVisuals constants
static const float PARTICLE_SPREAD_X   = 0.17f;    // DAT_10005A504
static const float PARTICLE_SPREAD_Y   = 0.3f;     // DAT_10005A508
static const float TILE_ROTATION_RANGE = 360.0f;   // DAT_10005A50C
static const float PARTICLE_INIT_SCALE = 0.02f;    // DAT_10005A510
static const float PARTICLE_WARMUP_DT  = 0.1f;     // DAT_10005A514

// helper: set alpha on all 4 vertices, preserving RGB
// reconstructed from FUN_100008388
static void setQuadAlpha(Quad& q, uint8_t alpha) {

    for (int i = 0; i < 4; i++) {
        uint32_t c = q.vertices[i].color;
        c = (c & 0x00FFFFFF) | ((uint32_t)alpha << 24);
        q.vertices[i].color = c;
    }
}

// helper: set all 4 vertices to the same color
// reconstructed from FUN_10000826c
static void setQuadColor(Quad& q, uint32_t rgba) {

    for (int i = 0; i < 4; i++) {
        q.vertices[i].color = rgba;
    }
}

// helper: offset all vertex positions by (dx, dy)
// reconstructed from FUN_100008494
static void offsetQuadVertices(Quad& q, float dx, float dy) {

    for (int i = 0; i < 4; i++) {
        q.vertices[i].x += dx;
        q.vertices[i].y += dy;
    }
}

// helper: set vertex colors selectively. reconstructed from FUN_1000082a4.
// mode 0 skips i where (i & ~2)==0, i.e. i=0 and i=2; mode 1 skips i==1 and i==4.
static void setSelectiveColor(Quad& q, uint32_t color, int mode) {

    for (int i = 0; i < 4; i++) {

        if (mode == 0) {
            if ((i & 0xFFFFFFFD) == 0) {
                continue;
            }
        } else {
            if (i == 1 || i == 4) {
                continue;
            }
        }

        q.vertices[i].color = color;
    }
}

// helper: negate all vertex Y positions
// reconstructed from FUN_100008454
static void flipQuadVertically(Quad& q) {

    for (int i = 0; i < 4; i++) {
        q.vertices[i].y = -q.vertices[i].y;
    }
}

// helper: set tile shape (modifies specific vertex Y positions)
// reconstructed from FUN_1000084c4
static void setTileShape(Quad& q, float param) {
    q.vertices[2].y = param;
    q.vertices[0].y = 0.0f;
    q.vertices[3].y = q.vertices[3].y * 2.0f;
    q.vertices[1].y = 0.0f;
}

// helper: interpolate between two RGBA colors
// reconstructed from FUN_10003a808
static uint32_t lerpColor(float t, uint32_t colorA, uint32_t colorB) {
    uint8_t aR = (colorA >> 0) & 0xFF;
    uint8_t aG = (colorA >> 8) & 0xFF;
    uint8_t aB = (colorA >> 16) & 0xFF;
    uint8_t aA = (colorA >> 24) & 0xFF;

    uint8_t bR = (colorB >> 0) & 0xFF;
    uint8_t bG = (colorB >> 8) & 0xFF;
    uint8_t bB = (colorB >> 16) & 0xFF;
    uint8_t bA = (colorB >> 24) & 0xFF;

    float inv = 1.0f - t;
    uint8_t r = (uint8_t)(aR * inv + bR * t);
    uint8_t g = (uint8_t)(aG * inv + bG * t);
    uint8_t b = (uint8_t)(aB * inv + bB * t);
    uint8_t a = (uint8_t)(aA * inv + bA * t);

    return (a << 24) | (b << 16) | (g << 8) | r;
}

// helper: random float in range. TitleMenu particle init / update both use
// stream 0 (FUN_10004ad80, FUN_10004b574 -> FUN_1000571d0 with stream=0).
static float randomFloat(float min, float max) {
    return rngFloat(min, max, 0);
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

// reconstructed from FUN_1000083bc
// hit test: checks if point (px, py) is inside a quad's scaled bounding box
static bool hitTestQuad(Quad& q, float px, float py) {
    float left  = q.vertices[0].x * q.scaleX + q.posX;
    float right = q.vertices[3].x * q.scaleX + q.posX;
    float top   = q.vertices[0].y * q.scaleY + q.posY;
    float bottom = q.vertices[3].y * q.scaleY + q.posY;

    return (px >= left && px <= right && py >= top && py <= bottom);
}

// reconstructed from FUN_10004be04
void TitleMenu::draw() {

    if (!visible) {
        return;
    }

    bindTexture(8);  // sheet1.png

    // draw the tile grid with rotation around the pivot point
    // pivot = first tile's position (tileGrid[0].quad.posX/posY)
    float px = pivotX();
    float py = pivotY();

    glPushMatrix();
    glTranslatef(px, py, 0.0f);
    glRotatef(boardRotation, 0.0f, 0.0f, 1.0f);
    glTranslatef(-px, -py, 0.0f);

    // draw 20 rows x 2 columns
    for (int row = 0; row < BOARD_GRID_ROWS; row++) {

        for (int col = 0; col < BOARD_GRID_COLS; col++) {
            int idx = row * BOARD_GRID_COLS + col;
            tileGrid[idx].quad.draw();
        }
    }

    glPopMatrix();

    // draw 300 particles
    for (int i = 0; i < SMOKE_PARTICLE_COUNT; i++) {
        particles[i].quad.draw();
    }

    // draw main overlay
    mainOverlayObj.quad.draw();

    // draw indicator pairs (conditionally)
    if (shopEnabled) {
        shopObjA.quad.draw();
        shopObjB.quad.draw();
    }

    if (leaderboardEnabled) {
        leaderboardObjA.quad.draw();
        leaderboardObjB.quad.draw();
    }

    if (achievementsEnabled) {
        achievementsObjA.quad.draw();
        achievementsObjB.quad.draw();
    }

    // draw fade overlay (title screen state: fadeProgress < 1.0)
    if (fadeProgress < 1.0f) {
        bindTexture(0);
        glDisable(GL_TEXTURE_2D);
        fadeOverlay.quad.draw();
        glEnable(GL_TEXTURE_2D);
        bindTexture(8);
    }

    // draw cursor
    cursorObj.quad.draw();

    // draw 3 extra quads (title screen gear elements)
    for (int i = 0; i < BOARD_EXTRA_QUADS; i++) {
        logoPieces[i].quad.draw();
    }
}

// FUN_10004a774, board constructor. sets up all TileIcon objects with vtables,
// UVs, sizes, and positions.
void TitleMenu::construct() {
    visible = false;
    fadeProgress = 0.0f;
    boardRotation = 0.0f;

    // fade overlay quad
    fadeOverlay.quad = Quad();

    // 300 particle quads
    // each gets: size 0.078 x 0.078, UV (0.5, 0.0957) to (0.5234, 0.126)
    for (int i = 0; i < SMOKE_PARTICLE_COUNT; i++) {
        particles[i].quad = Quad();
        particles[i].quad.setSize(0.078f, 0.078f);
        particles[i].quad.setTexCoords(0.5f, 0.0957f, 0.5234f, 0.126f);
    }

    // 40 grid tile quads
    for (int i = 0; i < BOARD_GRID_ROWS * BOARD_GRID_COLS; i++) {
        tileGrid[i].quad = Quad();
    }

    // vertical offset to center elements on modern android screens; the
    // original positions were tuned for shorter iOS aspect ratios circa 2016.
    const float yShift = 0.15f;

    // cursor quad: "I KEEP HAVING THIS DREAM" curved text
    cursorObj.quad = Quad();
    cursorObj.quad.setTexCoords(0.574f, 0.275f, 1.0f, 0.477f);
    cursorObj.quad.setSize(0.681f, 0.322f);
    cursorObj.quad.posX = 0.494f;
    cursorObj.quad.posY = 0.219f + yShift;

    // extra quad 0: the gear/cog logo
    logoPieces[0].quad = Quad();
    logoPieces[0].quad.setTexCoords(0.418f, 0.478f, 1.0f, 0.823f);
    logoPieces[0].quad.setSize(0.931f, 0.553f);
    logoPieces[0].quad.posX = 0.5f;
    logoPieces[0].quad.posY = 0.622f + yShift;

    // extra quad 1: the bullseye target
    logoPieces[1].quad = Quad();
    logoPieces[1].quad.setTexCoords(0.0f, 0.42f, 0.348f, 0.736f);
    logoPieces[1].quad.setSize(0.556f, 0.506f);
    logoPieces[1].quad.posX = 0.5f;
    logoPieces[1].quad.posY = 0.378f + yShift;

    // extra quad 2: internal bullseye ring
    logoPieces[2].quad = Quad();
    logoPieces[2].quad.setTexCoords(0.432f, 0.335f, 0.572f, 0.476f);
    logoPieces[2].quad.setSize(0.225f, 0.225f);
    logoPieces[2].quad.posX = 0.5f;
    logoPieces[2].quad.posY = 0.406f + yShift;

    // main overlay quad: game start button
    mainOverlayObj.quad = Quad();

    // indicator quads
    shopObjA.quad = Quad();
    shopObjB.quad = Quad();
    leaderboardObjA.quad = Quad();
    leaderboardObjB.quad = Quad();
    achievementsObjA.quad = Quad();
    achievementsObjB.quad = Quad();

    // initial speeds
    particleSpeed = 1.5f;
    rotationSpeed = 5.0f;
    colorCycleTimer = 0.0f;

    // FUN_10004a774 zeros the four click-result flags here, not the anim
    // fields (titleAnimState / titleButtonPressed are initialized in
    // initVisuals / FUN_10004ad80).
    startButtonClicked = false;
    shopClicked = false;
    leaderboardClicked = false;
    achievementsClicked = false;

    SDL_Log("TitleMenu::construct() complete");
}

// reconstructed from FUN_10004ad80, line by line against decompilation
void TitleMenu::initVisuals(bool hasKeys, bool hasAnyScore) {
    visible = true;
    titleAnimState = 0.0f;
    titleButtonPressed = false;

    overlayClickFlag = false;
    startButtonClicked = false;

    shopHighlight = false;
    leaderboardHighlight = false;
    achievementsHighlight = false;

    // leaderboard and achievements share hasAnyScore; only the shop indicator
    // gates on save progress.
    shopEnabled = hasKeys;
    leaderboardEnabled = hasAnyScore;
    achievementsEnabled = hasAnyScore;

    shopClicked = false;
    leaderboardClicked = false;
    achievementsClicked = false;

    // all overlay/indicator quads start opaque white.
    setQuadColor(mainOverlayObj.quad, 0xFFFFFFFF);
    setQuadColor(shopObjA.quad, 0xFFFFFFFF);
    setQuadColor(leaderboardObjA.quad, 0xFFFFFFFF);
    setQuadColor(achievementsObjA.quad, 0xFFFFFFFF);
    setQuadColor(shopObjB.quad, 0xFFFFFFFF);
    setQuadColor(leaderboardObjB.quad, 0xFFFFFFFF);
    setQuadColor(achievementsObjB.quad, 0xFFFFFFFF);

    float virtualHeight = Renderer::getVirtualHeight();

    if (!initialized) {
        initialized = true;
        fadeProgress = 0.0f;

        // fade overlay setup (size depends on runtime virtualHeight)
        setQuadColor(fadeOverlay.quad, 0xFF000000);
        fadeOverlay.quad.setSize(1.0f, virtualHeight);
        fadeOverlay.quad.posX = 0.5f;
        fadeOverlay.quad.posY = virtualHeight * 0.5f;

        // main overlay = start button
        mainOverlayObj.quad.setTexCoords(0.0f, 0.2129f, 0.2051f, 0.418f);
        mainOverlayObj.quad.setSize(0.3281f, 0.3281f);
        // posY derived from the gear's position:
        //   posY = (virtualHeight + gear.posY + gear.height * 0.5 + DAT_10005a4f4) * 0.5
        //   DAT_10005a4f4 = -0.09375
        float gearPosY = logoPieces[0].quad.posY;
        float gearHeight = logoPieces[0].quad.height;
        float buttonY = virtualHeight + gearPosY + gearHeight * 0.5f + (-0.09375f);
        mainOverlayObj.quad.posX = 0.5f;
        mainOverlayObj.quad.posY = buttonY * 0.5f;
        mainOverlayObj.quad.snapToPixelGrid();  // FUN_1000573a8; indicators derive from the snapped position

        // each indicator is two stacked Quads: A = the gear/circle background
        // (shared UV across all three), B = the unique icon overlay (key /
        // medal / trophy). positions derive from mainOverlayObj.
        //
        // DAT constants (binary anchors):
        //   DAT_10005a4f8 = -0.33438  (shop X offset from mainOverlay)
        //   DAT_10005a4fc = +0.33438  (leaderboard X offset)
        //   DAT_10005a500 = +0.09375  (achievements Y offset, below main)
        //   shop / leaderboard Y nudge = -0.15625 (above main; inline immediate)
        constexpr float kIndicatorShopXOff   = -0.33437499f;  // DAT_10005a4f8
        constexpr float kIndicatorLbXOff     =  0.33437499f;  // DAT_10005a4fc
        constexpr float kIndicatorAchYOff    =  0.09375f;     // DAT_10005a500
        constexpr float kIndicatorUpperYNudge = -0.15625f;    // immediate

        const float mainX = mainOverlayObj.quad.posX;
        const float mainY = mainOverlayObj.quad.posY;

        shopObjA.quad.setTexCoords(0.7051f, 0.0f, 0.873f, 0.168f);
        shopObjA.quad.setSize(0.2688f, 0.2688f);
        shopObjA.quad.posX = mainX + kIndicatorShopXOff;
        shopObjA.quad.posY = mainY + kIndicatorUpperYNudge;
        shopObjA.quad.snapToPixelGrid();

        // shop key icon overlay: same position as the gear background.
        shopObjB.quad.setTexCoords(0.6846f, 0.1689f, 0.7783f, 0.2627f);
        shopObjB.quad.setSize(0.15f, 0.15f);
        shopObjB.quad.posX = shopObjA.quad.posX;
        shopObjB.quad.posY = shopObjA.quad.posY;

        leaderboardObjA.quad.setTexCoords(0.7051f, 0.0f, 0.873f, 0.168f);
        leaderboardObjA.quad.setSize(0.2688f, 0.2688f);
        leaderboardObjA.quad.posX = mainX + kIndicatorLbXOff;
        leaderboardObjA.quad.posY = mainY + kIndicatorUpperYNudge;
        leaderboardObjA.quad.snapToPixelGrid();

        leaderboardObjB.quad.setTexCoords(0.7793f, 0.1689f, 0.873f, 0.2627f);
        leaderboardObjB.quad.setSize(0.15f, 0.15f);
        leaderboardObjB.quad.posX = leaderboardObjA.quad.posX;
        leaderboardObjB.quad.posY = leaderboardObjA.quad.posY;

        // achievements sits at (leaderboardX, mainY + 0.09375): same X as
        // leaderboard (both on the right) but below the start button.
        achievementsObjA.quad.setTexCoords(0.7051f, 0.0f, 0.873f, 0.168f);
        achievementsObjA.quad.setSize(0.2688f, 0.2688f);
        achievementsObjA.quad.posX = leaderboardObjA.quad.posX;
        achievementsObjA.quad.posY = mainY + kIndicatorAchYOff;
        achievementsObjA.quad.snapToPixelGrid();

        achievementsObjB.quad.setTexCoords(0.5039f, 0.1719f, 0.6f, 0.2656f);
        achievementsObjB.quad.setSize(0.15f, 0.15f);
        achievementsObjB.quad.posX = achievementsObjA.quad.posX;
        achievementsObjB.quad.posY = achievementsObjA.quad.posY;

        // set random positions for 300 particles
        // (UVs and sizes already set by construct(), just need positions)
        for (int i = 0; i < SMOKE_PARTICLE_COUNT; i++) {
            particles[i].quad.posX = randomFloat(0.0f, 1.0f);
            particles[i].quad.posY = randomFloat(0.0f, virtualHeight);
        }

        // set up 20x2 tile grid backgrounds
        // UV: (0.1533, 0.1602) to (0.1543, 0.207), size varies
        for (int row = 0; row < BOARD_GRID_ROWS; row++) {
            float randomWidth = randomFloat(PARTICLE_SPREAD_X, PARTICLE_SPREAD_Y);
            float tileH = randomWidth * (virtualHeight / 1.5f) * 0.5f;

            for (int col = 0; col < BOARD_GRID_COLS; col++) {
                int idx = row * BOARD_GRID_COLS + col;

                tileGrid[idx].quad.setTexCoords(0.1533f, 0.1602f, 0.1543f, 0.207f);
                tileGrid[idx].quad.setSize(virtualHeight - 0.40625f, tileH);

                uint32_t color1 = lerpColor(randomFloat(0.0f, 1.0f),
                    0xFF321428, 0xFF3C1E32);
                uint32_t color2 = lerpColor(randomFloat(0.0f, 1.0f),
                    0xFF000000, 0xFF000000);
                setSelectiveColor(tileGrid[idx].quad, color1, 1);
                setSelectiveColor(tileGrid[idx].quad, color2, 0);

                offsetQuadVertices(tileGrid[idx].quad,
                    tileGrid[idx].quad.width * 0.5f, 0.0f);

                // FUN_10004ad80: rotation = (row * DAT_10005a50c[=360.0]) / 20.0,
                // so 20 beams 18 degrees apart, spanning the full circle.
                float rotAngle = ((float)row * 360.0f) / 20.0f;
                setTileShape(tileGrid[idx].quad, PARTICLE_INIT_SCALE);  // FUN_1000084c4 takes DAT_10005a510, not the rotation

                tileGrid[idx].quad.posX = 0.5f;
                tileGrid[idx].quad.posY = 0.55625f;     // binary 0.40625 + yShift 0.15, to fit modern screens
                tileGrid[idx].quad.rotation = rotAngle;

                if (col == 1) {
                    flipQuadVertically(tileGrid[idx].quad);
                }
            }
        }

        // warm up the particle simulation (100 ticks)
        for (int i = 0; i < 100; i++) {
            update(PARTICLE_WARMUP_DT, false);
        }

        // reset fade after warmup
        fadeProgress = 0.0f;
        setQuadColor(fadeOverlay.quad, 0xFF000000);
    }

    SDL_Log("TitleMenu::initVisuals() complete");
}

// reconstructed from FUN_10004b574
void TitleMenu::update(float dt, bool interactable,
                       SoundQueue* soundQueue) {

    if (!visible) {
        return;
    }

    // update board rotation
    float newRot = boardRotation + rotationSpeed * dt;

    if (newRot > ROTATION_CAP) {
        newRot += ROTATION_WRAP;
    }

    boardRotation = newRot;

    // update color cycle timer
    colorCycleTimer = fmodf(colorCycleTimer + dt * 2.0f, CYCLE_PERIOD);

    // update the 3 extra quads with color cycling
    for (int i = 0; i < BOARD_EXTRA_QUADS; i++) {

        if (!titleButtonPressed || i == 0) {
            float sine = sinf((float)i * CYCLE_SPEED + colorCycleTimer);
            uint32_t color = lerpColor((sine + 1.0f) * 0.5f,
                0xFFDC96DC, 0xFFFFFFFF);
            setQuadColor(logoPieces[i].quad, color);
        } else {
            setQuadColor(logoPieces[i].quad, 0xFFFFFFFF);
        }
    }

    // update 300 particles
    for (int i = 0; i < SMOKE_PARTICLE_COUNT; i++) {
        Quad& p = particles[i].quad;

        // center shifted to match yShift from construct() (0.40625 + 0.15)
        float dx = p.posX - 0.5f;
        float dy = p.posY - 0.55625f;
        float dist = sqrtf(dx * dx + dy * dy);
        float normDist = clampf(dist / DIST_NORMALIZE, 0.0f, 1.0f);

        // scale based on distance
        float scale = normDist * 12.0f + (1.0f - normDist);
        float halfSize = scale * PARTICLE_HALF_SIZE;

        // drift outward from center (dx/dy point away from it), so particles
        // accelerate off-screen and get respawned near center below.
        float newX = p.posX + dx * dt * particleSpeed;
        float newY = p.posY + dy * dt * particleSpeed;

        // check bounds
        bool offScreen = (newX + halfSize < 0.0f) || (newX - halfSize > 1.0f) ||
                         (newY + halfSize < 0.0f);

        float vHeight = Renderer::getVirtualHeight();

        if (!offScreen) {
            float checkY = newY - halfSize;

            if (checkY < vHeight) {
                // particle is on screen, keep it
                p.posX = newX;
                p.posY = newY;
                p.scaleX = scale;
                p.scaleY = scale;
            } else {
                offScreen = true;
            }
        }

        if (offScreen) {
            // respawn on a circle of radius 0.1875 around center
            // (FUN_100057250 = radius * sincos). random angle 0 to 360 degrees,
            // fed raw to sincos.
            float angle = randomFloat(0.0f, ROTATION_CAP);
            float rad = angle;  // sincos_stret takes the raw value
            // FUN_100057250 returns radius*cos in s0 (newX), radius*sin in s1 (newY)
            newX = cosf(rad) * 0.1875f + 0.5f;
            newY = sinf(rad) * 0.1875f + 0.55625f;
            p.posX = newX;
            p.posY = newY;
            scale = 0.5f;
            p.scaleX = scale;
            p.scaleY = scale;
        }

        // color based on particle index (3 bands of purple)
        if ((float)i <= COLOR_THRESH_2) {
            setQuadColor(p, lerpColor(normDist, 0xFF783278, 0x321E0A1E));
        } else if ((float)i <= COLOR_THRESH_1) {
            setQuadColor(p, lerpColor(normDist, 0xFF963C96, 0x1E50283C));
        } else {
            setQuadColor(p, lerpColor(normDist, 0xFFC864B4, 0x0A1E140A));
        }
    }

    // fade-in animation
    if (fadeProgress < 1.0f) {
        fadeProgress = clampf(fadeProgress + dt * 2.0f, 0.0f, 1.0f);
        float t = 0.5f - cosf(fadeProgress * FADE_COS_MULT) * 0.5f;
        uint8_t alpha = (uint8_t)((1.0f - t) * FADE_START_ALPHA);
        setQuadAlpha(fadeOverlay.quad, alpha);

        // fade the extra quads. binary calls FUN_10003a808 with param_4=1, which
        // applies the cosine smoothstep (same remap as t above) before lerping.
        for (int i = 0; i < BOARD_EXTRA_QUADS; i++) {
            uint32_t baseColor = logoPieces[i].quad.vertices[0].color;
            uint32_t fadedColor = lerpColor(t, 0xFFFFFFFF, baseColor);
            setQuadColor(logoPieces[i].quad, fadedColor);
        }

        return;
    }

    // post-fade: title state animation
    if (!titleButtonPressed) {
        titleAnimState -= dt / 5.0f;
    } else {
        titleAnimState += dt / 5.0f;
    }

    titleAnimState = clampf(titleAnimState, 0.0f, 1.0f);
    particleSpeed = titleAnimState * 10.0f + (1.0f - titleAnimState) * 1.5f;
    rotationSpeed = titleAnimState * ROTATION_SPEED_MULT + (1.0f - titleAnimState) * 5.0f;

    // touch handling (from FUN_10004b574)
    if (!interactable || !getGame()) {
        return;
    }

    int touchState = getGame()->inputState();
    float touchX = getGame()->touchX();
    float touchY = getGame()->touchY();

    if (touchState == 1) {
        // touch began: hit test against the bullseye area (logoPieces[1])
        if (hitTestQuad(logoPieces[1].quad, touchX, touchY)) {
            titleButtonPressed = true;
            return;
        }

        // the four buttons are a mutually exclusive else-if cascade: each
        // enabled-but-missed button falls through to the next, and only the
        // first hit highlights. any hit converges on a single soundQueue
        // trigger(0) at the shared tail (FUN_100035ccc(soundQueue, 0)).
        bool pressed = false;

        if (hitTestQuad(mainOverlayObj.quad, touchX, touchY)) {
            overlayClickFlag = true;
            setQuadColor(mainOverlayObj.quad, 0xFFB4B4B4);
            pressed = true;
        } else if (shopEnabled && hitTestQuad(shopObjA.quad, touchX, touchY)) {
            shopHighlight = true;
            setQuadColor(shopObjA.quad, 0xFFB4B4B4);
            setQuadColor(shopObjB.quad, 0xFFB4B4B4);
            pressed = true;
        } else if (leaderboardEnabled && hitTestQuad(leaderboardObjA.quad, touchX, touchY)) {
            leaderboardHighlight = true;
            setQuadColor(leaderboardObjA.quad, 0xFFB4B4B4);
            setQuadColor(leaderboardObjB.quad, 0xFFB4B4B4);
            pressed = true;
        } else if (achievementsEnabled && hitTestQuad(achievementsObjA.quad, touchX, touchY)) {
            achievementsHighlight = true;
            setQuadColor(achievementsObjA.quad, 0xFFB4B4B4);
            setQuadColor(achievementsObjB.quad, 0xFFB4B4B4);
            pressed = true;
        }

        // this was a test to use sound 0, but it doesn't really
        // sound very good. struck out for now.
        /*if (pressed && soundQueue) {
            soundQueue->trigger(0);
        }*/

    } else if (touchState == 0) {
        // touch released
        titleButtonPressed = false;

        if (overlayClickFlag) {
            overlayClickFlag = false;
            setQuadColor(mainOverlayObj.quad, 0xFFFFFFFF);

            if (hitTestQuad(mainOverlayObj.quad, touchX, touchY)) {
                // released on button: set result flag, sound 1
                startButtonClicked = true;

                if (soundQueue) {
                    soundQueue->trigger(1);
                }
            } else {
                // missed: sound 2
                if (soundQueue) {
                    soundQueue->trigger(2);
                }
            }
        }

        if (shopHighlight) {
            shopHighlight = false;
            setQuadColor(shopObjA.quad, 0xFFFFFFFF);
            setQuadColor(shopObjB.quad, 0xFFFFFFFF);

            if (hitTestQuad(shopObjA.quad, touchX, touchY)) {
                // indicator 1 released on target: set result flag, sound 1
                shopClicked = true;

                if (soundQueue) {
                    soundQueue->trigger(1);
                }
            } else {
                // missed: sound 2
                if (soundQueue) {
                    soundQueue->trigger(2);
                }
            }
        }

        if (leaderboardHighlight) {
            leaderboardHighlight = false;
            setQuadColor(leaderboardObjA.quad, 0xFFFFFFFF);
            setQuadColor(leaderboardObjB.quad, 0xFFFFFFFF);

            if (hitTestQuad(leaderboardObjA.quad, touchX, touchY)) {
                // released on target: set result flag, sound 1
                leaderboardClicked = true;

                if (soundQueue) {
                    soundQueue->trigger(1);
                }
            } else {

                if (soundQueue) {
                    soundQueue->trigger(2);
                }
            }
        }

        if (achievementsHighlight) {
            achievementsHighlight = false;
            setQuadColor(achievementsObjA.quad, 0xFFFFFFFF);
            setQuadColor(achievementsObjB.quad, 0xFFFFFFFF);

            if (hitTestQuad(achievementsObjA.quad, touchX, touchY)) {
                // released on target: set result flag, sound 1
                achievementsClicked = true;

                if (soundQueue) {
                    soundQueue->trigger(1);
                }
            } else {

                if (soundQueue) {
                    soundQueue->trigger(2);
                }
            }
        }
    }
}
