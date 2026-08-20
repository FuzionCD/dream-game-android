#include "renderer.h"
#include "quad.h"
#include "game.h"
#include <GLES/gl.h>
#include <SDL.h>

// global texture-binding cache. wraps glBindTexture with a guard so
// back-to-back binds of the same texture skip the GL state change.
static GLuint sCurrentTexture = 0xFFFFFFFF;

// GL names for each atlas slot, indexed by slot number. slot 0 is the
// untextured binding and stays 0. Game::loadTextures fills in 1..13 with
// whatever names glGenTextures returned for each atlas.
static GLuint sTextureSlots[NUM_TEXTURE_SLOTS + 1] = { 0 };

void Renderer::bindTextureName(GLuint glName) {

    if (sCurrentTexture == glName) {
        return;
    }

    sCurrentTexture = glName;
    glBindTexture(GL_TEXTURE_2D, glName);
}

void Renderer::setTextureSlot(GLuint textureSlot, GLuint glName) {

    if (textureSlot == 0 || textureSlot > NUM_TEXTURE_SLOTS) {
        SDL_Log("setTextureSlot: slot %u out of range", textureSlot);
        return;
    }

    sTextureSlots[textureSlot] = glName;
}

void Renderer::resetTextureSlots() {

    for (int i = 0; i <= NUM_TEXTURE_SLOTS; i++) {
        sTextureSlots[i] = 0;
    }

    sCurrentTexture = 0xFFFFFFFF;
}

void bindTexture(GLuint textureSlot) {

    if (textureSlot > NUM_TEXTURE_SLOTS) {
        SDL_Log("bindTexture: slot %u out of range", textureSlot);
        return;
    }

    Renderer::bindTextureName(sTextureSlots[textureSlot]);
}

static int sScreenW = 0;
static int sScreenH = 0;
static float sVirtualHeight = 0.0f;
static float sScaleFactor = 0.0f;

// letterbox threshold from binary: DAT_10005a4a4 = 1.32
static const float LETTERBOX_THRESHOLD = 1.32f;

// reconstructed from FUN_100044764
void Renderer::init(int screenW, int screenH, void* gameData) {
    sScreenW = screenW;
    sScreenH = screenH;

    glViewport(0, 0, screenW, screenH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    // virtual height = aspect ratio (height / width)
    sVirtualHeight = (float)screenH / (float)screenW;
    sScaleFactor = 0.0f;

    float orthoLeft;

    if (LETTERBOX_THRESHOLD <= sVirtualHeight) {
        // normal aspect ratio (tall enough): no letterboxing
        orthoLeft = 0.0f;
    } else {
        // aspect too wide: add letterbox bars on left/right
        sScaleFactor = ((float)screenW * LETTERBOX_THRESHOLD) / (float)(screenH * 2) - 0.5f;
        sVirtualHeight = 1.32f;

        // set up letterbox bar quads in the game struct if available
        if (gameData) {
            Game* game = (Game*)gameData;

            game->scaleFactor() = sScaleFactor;

            Quad& bar1 = game->letterboxQuad1();
            bar1.setColor(0, 0, 0, 255);
            bar1.setSize(sScaleFactor, sVirtualHeight);
            bar1.posX = -sScaleFactor * 0.5f;
            bar1.posY = sVirtualHeight * 0.5f;

            Quad& bar2 = game->letterboxQuad2();
            bar2.setColor(0, 0, 0, 255);
            bar2.setSize(sScaleFactor, sVirtualHeight);
            bar2.posX = sScaleFactor * 0.5f + 1.0f;
            bar2.posY = sVirtualHeight * 0.5f;
        }

        orthoLeft = -sScaleFactor;
    }

    // orthographic projection matching the binary exactly:
    // left = -scaleFactor (or 0 if no letterbox)
    // right = scaleFactor + 1.0 (or 1.0 if no letterbox)
    // bottom = virtualHeight
    // top = 0
    // near = -10, far = 10
    glOrthof(orthoLeft, sScaleFactor + 1.0f, sVirtualHeight, 0.0f, -10.0f, 10.0f);

    glMatrixMode(GL_MODELVIEW);

    // enable texturing and blending (matching binary: GL_TEXTURE_2D, GL_BLEND)
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // enable vertex arrays (matching binary: VERTEX, TEXTURE_COORD, COLOR)
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    SDL_Log("Renderer: %dx%d, virtualHeight=%.3f, scaleFactor=%.3f",
            screenW, screenH, sVirtualHeight, sScaleFactor);
}

void Renderer::beginFrame() {
    // pure black background matching the game's palette
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

int Renderer::getScreenWidth() {
    return sScreenW;
}

int Renderer::getScreenHeight() {
    return sScreenH;
}

float Renderer::getVirtualHeight() {
    return sVirtualHeight;
}

float Renderer::getScaleFactor() {
    return sScaleFactor;
}
