#include "menu.h"
#include "game.h"             // getGame() (sound queue)
#include "renderer.h"         // bindTexture + Renderer::getVirtualHeight
#include "sound_queue.h"
#include <GLES/gl.h>
#include <cmath>

// scalar pixel-snap helper (matches Quad::snapToPixelGrid's grid). exposed
// to subclasses via menu.h so per-subclass init/open code can snap anchors
// for content positioning. binary's FUN_100057374, same body.
float snapMenuPixel(float v) {
    constexpr float SNAP_REF = 640.0f;
    float scaled = v * SNAP_REF + (v < 0.0f ? -0.5f : 0.5f);
    return (float)(int)scaled / SNAP_REF;
}

// FUN_100057424, Menu::init.
//
// shared chrome ctor. populates the 4 chrome Quads (bgDim, titleQuad, closeBg,
// confirmButton) + 1 Label (frame9slice) inside the Menu header. titleQuad's
// UV/size come from the caller-supplied atlas-pixel rect; other Quads use
// fixed UVs.
void Menu::init(int titleAtlasX, int titleAtlasY, int titleAtlasW, int titleAtlasH) {

    frame9slice.init();   // FUN_10004c014

    // bgDim: fullscreen darkening backdrop. centered horizontally,
    // virtualHeight-tall so it covers any aspect ratio. opaque black.
    {
        float virtualHeight = Renderer::getVirtualHeight();
        bgDim.posX = 0.5f;
        bgDim.posY = virtualHeight * 0.5f;
        bgDim.setSize(1.0f, virtualHeight);
        bgDim.setColor(0, 0, 0, 0xFF);
    }

    // frame9slice: 9-slice chrome border. 9 addGlyph calls forming a
    // 3x3 grid on atlas pixels (columns 72/121/122, rows 351/399/400).
    // modes encode the corner/edge/center stretch semantics for layoutGlyphs:
    //   row 0 (y=351, bottom): modes 2/1/2 (corner / edge / corner)
    //   row 1 (y=399, middle): modes 0/3/0 (full stretch)
    //   row 2 (y=400, top):    modes 2/1/2 (same as bottom)
    {
        static constexpr float UV_ORIG[9][2] = {
            { 72.0f, 351.0f}, {121.0f, 351.0f}, {122.0f, 351.0f},
            { 72.0f, 399.0f}, {121.0f, 399.0f}, {122.0f, 399.0f},
            { 72.0f, 400.0f}, {121.0f, 400.0f}, {122.0f, 400.0f},
        };
        static constexpr float UV_SIZE[9][2] = {
            {49.0f, 48.0f}, { 1.0f, 48.0f}, {49.0f, 48.0f},
            {49.0f,  1.0f}, { 1.0f,  1.0f}, {49.0f,  1.0f},
            {49.0f, 50.0f}, { 1.0f, 50.0f}, {49.0f, 50.0f},
        };
        static constexpr uint32_t MODES[9] = {
            2u, 1u, 2u,
            0u, 3u, 0u,
            2u, 1u, 2u,
        };

        for (int i = 0; i < 9; ++i) {
            GlyphOffset off = {0.0f, 0.0f};
            frame9slice.addGlyph(-1.0f, UV_ORIG[i], UV_SIZE[i], MODES[i], off);

            if (i == 2 || i == 5) {
                frame9slice.pendingLineIndex += 1;
            }
        }
    }

    // titleQuad: per-subclass title bar. caller passes atlas-pixel coords;
    // binary's FUN_100014d84 (pixel-rect helper) converts to UV + size.
    {
        constexpr float ATLAS_TO_UV     = 1.0f / 1024.0f;   // DAT_100059ebc
        constexpr float PIXEL_TO_SCREEN = 1.0f / 640.0f;    // DAT_100059ec0

        const float px_x = (float)titleAtlasX;
        const float px_y = (float)titleAtlasY;
        const float px_w = (float)titleAtlasW;
        const float px_h = (float)titleAtlasH;

        titleQuad.setTexCoords(
             px_x          * ATLAS_TO_UV,  px_y          * ATLAS_TO_UV,
            (px_x + px_w)  * ATLAS_TO_UV, (px_y + px_h)  * ATLAS_TO_UV);
        titleQuad.setSize(px_w * PIXEL_TO_SCREEN, px_h * PIXEL_TO_SCREEN);
    }

    // closeBg: round backdrop behind confirmButton. UV+size from inline
    // literals in FUN_100057424 (shared across all Menu subclasses).
    closeBg.setTexCoords(0.180664f, 0.577148f, 0.292969f, 0.690430f);
    closeBg.setSize(0.179688f, 0.18125f);

    // confirmButton: the close/OK icon Quad. shared UV+size.
    confirmButton.setTexCoords(0.409180f, 0.065430f, 0.475586f, 0.132812f);
    confirmButton.setSize(0.10625f, 0.107813f);
}

// FUN_100057b98, Menu::setSize.
//
// resize frame9slice + reposition the 3 chrome quads against the new (w, h)
// rect. then write anchorX. titleQuad goes horizontally centered;
// closeBg + confirmButton land at the bottom-right with fixed offsets.
void Menu::setSize(float panelWidth, float panelHeight) {
    // shared button offset constants (live in __TEXT.__const next to the
    // Menu code, so they're shared across all subclasses).
    constexpr float CLOSE_BG_OFFSET    = -0.020313f;   // DAT_10005a740
    constexpr float CONFIRM_DX         = -0.002344f;   // DAT_10005a744
    constexpr float CONFIRM_DY         = -0.003125f;   // DAT_10005a748
    constexpr float TITLE_QUAD_POS_Y   =  0.015625f;   // immediate 0x3c800000

    // size the chrome border to the full panel rect.
    frame9slice.setSize(panelWidth, panelHeight);

    // titleQuad: horizontally centered, snapped to pixel grid.
    titleQuad.posX = panelWidth * 0.5f;
    titleQuad.posY = TITLE_QUAD_POS_Y;
    titleQuad.snapToPixelGrid();

    // closeBg + confirmButton: bottom-right corner with fixed offsets.
    closeBg.posX = panelWidth  + CLOSE_BG_OFFSET;
    closeBg.posY = panelHeight + CLOSE_BG_OFFSET;
    closeBg.snapToPixelGrid();

    confirmButton.posX = closeBg.posX + CONFIRM_DX;
    confirmButton.posY = closeBg.posY + CONFIRM_DY;
    confirmButton.snapToPixelGrid();

    // anchorX: pixel-snapped, centered horizontally on the screen's 0..1
    // virtual coord range.
    anchorX = snapMenuPixel(0.5f - panelWidth * 0.5f);
}

// FUN_100057c54, Menu::setAnchorY.
void Menu::setAnchorY(float y) {
    anchorY = snapMenuPixel(y);
}

// Android adapter; see header doc. equivalent to:
//   setSize(w, h);
//   setAnchorY((Renderer::getVirtualHeight() - h) * 0.5f);
// centers the panel vertically on any aspect ratio (the binary's
// hard-coded anchorY constants assume iOS's virtualHeight = 1.5 and
// land off-screen on Android's wider aspect ratios).
void Menu::setSizeAndCenterY(float panelWidth, float panelHeight) {
    setSize(panelWidth, panelHeight);
    const float vh = Renderer::getVirtualHeight();
    setAnchorY((vh - panelHeight) * 0.5f);
}

// FUN_100057ac8, Menu::panelShow.
//
// opens the panel: shows it, rewinds the fade timer, clears the tap-commit
// state, whitens confirmButton, and plays sound 8 (panel open whoosh).
// readyByte + confirmPressed are one ushort write.
void Menu::panelShow() {
    visible          = true;
    secondaryVisible = true;
    animTimer0       = 0.0f;
    readyByte        = false;
    confirmPressed   = false;
    confirmButton.setColor(0xFF, 0xFF, 0xFF, 0xFF);

    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x08);
    }
}

// FUN_100057b1c, Menu::panelHide.
//
// flag-gated dual-mode close. callers pass either true (= "just hide; keep
// the deferred-close state for animation tail") or false (= full close with
// audio + state reset).
void Menu::panelHide(bool justClearVisible) {

    if (justClearVisible) {
        visible = false;
        return;
    }

    secondaryVisible = false;
    animTimer0       = 0.0f;
    readyByte        = false;
    // confirmPressed deliberately not cleared here; binary writes
    // one byte (readyByte) only. matters when a tap is in flight at close time;
    // subsequent baseUpdate gates on readyByte first so the stale
    // confirmPressed is harmless until next panelShow rewinds it.

    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x08);
    }
}

// FUN_100057878, Menu::draw.
//
// base chrome render; subclasses call this then draw their slot content.
//   0. early-out if !visible (binary's `if (*(char *)(param_1 + 8) != '\0')`).
//      essential when this is reached through an embedded panel (e.g.
//      PauseMenu::draw -> forfeitConfirmPanel.draw -> here): without the gate
//      the never-positioned bgDim / chrome leak out at full opaque black.
//   1. bind tex 0, draw bgDim (under a no-matrix-push context).
//   2. push matrix, translate by (anchorX, anchorY), bind tex 9.
//   3. draw frame9slice + titleQuad.
//   4. if readyByte: draw closeBg + confirmButton.
//   5. pop matrix.
void Menu::draw() {

    if (!visible) {
        return;
    }

    bindTexture(0);
    bgDim.draw();

    glPushMatrix();
    glTranslatef(anchorX, anchorY, 0.0f);
    bindTexture(9);
    frame9slice.draw();
    titleQuad.draw();

    if (readyByte) {
        closeBg.draw();
        confirmButton.draw();
    }

    glPopMatrix();
}

// FUN_100057b58, Menu::setAlpha (base impl).
//
// fades the chrome: bgDim gets half alpha so the backdrop only ever reaches
// ~50% opacity at full fade-in (matching the artist intent; the gameplay
// world should still be partially visible through the panel). frame9slice
// and titleQuad get full alpha. confirmButton + closeBg are not touched
// here; they're tinted by panelShow/baseUpdate's state machine.
void Menu::setAlpha(uint8_t a) {
    bgDim.setAlpha(static_cast<uint8_t>(a >> 1));
    frame9slice.setAlpha(a);
    titleQuad.setAlpha(a);
}

// FUN_1000578f8, Menu::baseUpdate.
//
// drives the cosine-eased fade animation + confirm-button tap state machine.
// uses three DAT constants:
//   DAT_10005a734 = 0.3f   (fade duration in seconds)
//   DAT_10005a738 = PI     (3.1415927)
//   DAT_10005a73c = 255.0f (alpha max)
//
// fade math (cosine-eased):
//   t          = animTimer0 + dt / DURATION       (clamped to 1.0)
//   easeT      = t when opening (secondaryVisible == 1), else 1 - t
//   ease       = 0.5 - cos(easeT * PI) * 0.5      (smoothstep)
//   alpha      = ease * 255
//
// once fully open (animTimer0 >= 1.0) and readyByte set by the subclass, the
// same function handles the confirm-button tap state via Quad::contains.
// the alpha each frame is pushed via virtual setAlpha so subclass also
// fades its content.
void Menu::baseUpdate(float dt) {
    constexpr float FADE_DURATION = 0.3f;       // DAT_10005a734
    constexpr float FADE_PI       = 3.1415927f; // DAT_10005a738
    constexpr float ALPHA_MAX     = 255.0f;     // DAT_10005a73c

    if (!visible) {
        return;
    }

    if (animTimer0 >= 1.0f) {
        // fully visible: process confirm-button tap state
        if (!readyByte) {
            return;   // subclass hasn't enabled commit yet
        }

        Game* g = getGame();

        if (!g) {
            return;
        }

        float touchX = g->touchX() - anchorX;
        float touchY = g->touchY() - anchorY;
        int   touchState = g->inputState();

        if (touchState == 1) {
            // press-or-hold: hit-test the confirm button.
            if (!confirmButton.contains(touchX, touchY)) {
                return;
            }

            confirmPressed = true;
            confirmButton.setColor(0xC8, 0xC8, 0xC8, 0xFF);
            g->soundQueue.trigger(5);   // "press" sound
            return;
        }

        if (touchState != 0) {
            return;   // touched-state but not idle, ignore
        }

        if (!confirmPressed) {
            return;   // never pressed -> nothing to release
        }

        // released this frame. re-test bbox: still-over = commit, drifted = cancel.
        bool stillOver = confirmButton.contains(touchX, touchY);

        if (stillOver) {
            onConfirmTapped();
            g->soundQueue.trigger(6);   // "confirm" sound
        } else {
            g->soundQueue.trigger(7);   // "cancel" sound
        }

        confirmPressed = false;
        confirmButton.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        return;
    }

    // still ramping the fade animation
    float t = animTimer0 + dt / FADE_DURATION;

    if (t >= 1.0f) {
        t = 1.0f;
    }

    animTimer0 = t;

    // closing-complete detection: when secondaryVisible was cleared (= close
    // in progress) and the timer just hit 1.0, finalize hide.
    bool useInverted = !secondaryVisible;

    if (useInverted && t >= 1.0f) {
        visible = false;
    }

    float easeT = useInverted ? (1.0f - t) : t;
    float ease  = 0.5f - std::cos(easeT * FADE_PI) * 0.5f;
    uint8_t alpha = static_cast<uint8_t>(static_cast<int>(ease * ALPHA_MAX) & 0xFF);

    // virtual dispatch: fades subclass content too.
    setAlpha(alpha);
}
