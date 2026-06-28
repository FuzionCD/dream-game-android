#include "dialog_panel.h"
#include "game.h"
#include "renderer.h"
#include "tutorial_hint_table.h"
#include <GLES/gl.h>
#include <cmath>
#include <cstring>

namespace {

// FUN_100057374, snap a coordinate to the nearest 1/640 pixel grid.
inline float pixelSnap640(float v) {
    constexpr float SNAP_SCALE = 640.0f;
    return (v >= 0.0f) ? (float)(int)(v * SNAP_SCALE + 0.5f) / SNAP_SCALE
                       : (float)(int)(v * SNAP_SCALE - 0.5f) / SNAP_SCALE;
}

// FUN_1000570d4, 3-arg clamp(value, lo, hi).
inline float clamp01(float value, float lo, float hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

}  // namespace

// reconstructed from Ghidra FUN_1000404e8
void DialogPanel::init() {
    visible = false;
    posX = 0.0f;
    posY = 0.0f;
    configA = false;
    configB = false;

    // 9-slice frame pieces + 2 mirrored tail pairs (default-constructed here,
    // then UV + size set below).
    cornerTL            = TileIcon();
    topEdge             = TileIcon();
    tailUp[0]           = TileIcon();
    tailUp[1]           = TileIcon();
    topEdgeAfterTail    = TileIcon();
    cornerTR            = TileIcon();
    leftEdge            = TileIcon();
    centerFill          = TileIcon();
    rightEdge           = TileIcon();
    cornerBL            = TileIcon();
    bottomEdge          = TileIcon();
    tailDown[0]         = TileIcon();
    tailDown[1]         = TileIcon();
    bottomEdgeAfterTail = TileIcon();
    cornerBR            = TileIcon();

    // text items: binary calls FUN_10002fa08 (TextItem::init) on each in a
    // loop. fields configured per-line below.
    for (int i = 0; i < 3; i++) {
        textLines[i].init();
    }

    backdropQuad = TileIcon();
    fadeProgress = 1.0f;
    lerpStartX = 0.0f;

    auto setTile = [](TileIcon& t, float u0, float v0, float u1, float v1,
                      float w, float h) {
        t.quad.setTexCoords(u0, v0, u1, v1);
        t.quad.setSize(w, h);
    };

    // cornerTL: top-left corner sprite. UV = the LEFT 9-slice cell
    // (0.9345703, 0.82714844) -> (0.9658203, 0.85839844); not the thin edge
    // cell that the top bar uses.
    setTile(cornerTL, 0.9345703f, 0.82714844f, 0.9658203f, 0.85839844f,
            0.05f, 0.05f);
    // topEdge: top-edge horizontal stripe (stretched to fill)
    setTile(topEdge, 0.9658203f, 0.82714844f, 0.9667969f, 0.85839844f,
            0.0015625f, 0.05f);
    // topEdgeAfterTail: same stripe UV, fills the top edge right of the up-tail
    setTile(topEdgeAfterTail, 0.9658203f, 0.82714844f, 0.9667969f, 0.85839844f,
            0.0015625f, 0.05f);
    // cornerTR
    setTile(cornerTR, 0.9667969f, 0.82714844f, 1.0f, 0.85839844f,
            0.053125f, 0.05f);
    // leftEdge
    setTile(leftEdge, 0.9345703f, 0.85839844f, 0.9658203f, 0.859375f,
            0.05f, 0.0015625f);
    // centerFill
    setTile(centerFill, 0.9658203f, 0.85839844f, 0.9667969f, 0.859375f,
            0.0015625f, 0.0015625f);
    // rightEdge
    setTile(rightEdge, 0.9667969f, 0.85839844f, 1.0f, 0.859375f,
            0.053125f, 0.0015625f);
    // cornerBL
    setTile(cornerBL, 0.9345703f, 0.859375f, 0.9658203f, 0.8925781f,
            0.05f, 0.053125f);
    // bottomEdge
    setTile(bottomEdge, 0.9658203f, 0.859375f, 0.9667969f, 0.8925781f,
            0.0015625f, 0.053125f);
    // bottomEdgeAfterTail
    setTile(bottomEdgeAfterTail, 0.9658203f, 0.859375f, 0.9667969f, 0.8925781f,
            0.0015625f, 0.053125f);
    // cornerBR
    setTile(cornerBR, 0.9667969f, 0.859375f, 1.0f, 0.8925781f,
            0.053125f, 0.053125f);

    // downward tail (drawn when !configA): two horizontally-mirrored sprites.
    // tailDown[0] = anchor in left half, tailDown[1] = anchor in right half.
    setTile(tailDown[0], 0.8828125f, 0.6308594f, 0.9394531f, 0.7011719f,
            0.090625f, 0.1125f);
    setTile(tailDown[1], 0.9423828f, 0.6308594f, 0.99902344f, 0.7011719f,
            0.090625f, 0.1125f);

    // upward tail (drawn when configA): same left/right mirror pair.
    setTile(tailUp[0], 0.7636719f, 0.63183594f, 0.8203125f, 0.7001953f,
            0.090625f, 0.109375f);
    setTile(tailUp[1], 0.8232422f, 0.63183594f, 0.8798828f, 0.7001953f,
            0.090625f, 0.109375f);

    // cornerTL: posX = width/2, posY = height/2 (standard pattern)
    cornerTL.quad.posX = cornerTL.quad.width  * 0.5f;
    cornerTL.quad.posY = cornerTL.quad.height * 0.5f;

    // nudge each tail off the edge it grows from: the up-tail rises above the
    // top edge, the down-tail drops below the bottom edge.
    tailUp[0].quad.addVertexOffset(0.0f, -0.0296875f);
    tailUp[1].quad.addVertexOffset(0.0f, -0.0296875f);
    tailDown[0].quad.addVertexOffset(0.0f,  0.0296875f);
    tailDown[1].quad.addVertexOffset(0.0f,  0.0296875f);

    // text-line per-item state. binary writes per line:
    //   glyphTablePtr = the dialog font's glyph table
    //   scaleX        = 0.072
    //   scaleY        = 0.072
    //   rgba          = 0xff1e3232
    //   applyColor()  (no-op on empty glyph vec)
    //
    // dialog panel uses BMFontTable[1] ("fontClean.fnt").
    // populated by Game::loadFonts at startup. typed accessor returns the
    // int* expected by TextItem (textureIndex at offset 0).
    Game* g = getGame();
    const BMFontTable* glyphSrc = g ? g->bmfontTablePtr(1) : nullptr;

    for (int i = 0; i < 3; i++) {
        textLines[i].glyphTablePtr = glyphSrc;
        textLines[i].scaleX        = 0.072f;
        textLines[i].scaleY        = 0.072f;
        textLines[i].rgba          = 0xff1e3232u;
        textLines[i].applyColor();
    }

    // backdrop overlay: full-screen darken layer drawn on tex 0 (no texture)
    // before the frames. position = (0.5, virtualHeight*0.5), size = (1.0, vh).
    // virtualHeight is runtime aspect-ratio-dependent (~2.2 on Android,
    // ~1.33 on iPad); never hardcode the iOS default 1.5.
    const float virtualHeight = Renderer::getVirtualHeight();
    backdropQuad.quad.posX = 0.5f;
    backdropQuad.quad.posY = virtualHeight * 0.5f;
    backdropQuad.quad.setSize(1.0f, virtualHeight);
    backdropQuad.quad.setColor(0x00, 0x00, 0x00, 0xFF);  // opaque black
}

// reconstructed from Ghidra FUN_10004128c (panel reset / dismiss).
// early-out if already invisible. when visible: clear visible +
// interpolatePosition, set fadeProgress = 0 or 1 depending on hardReset, then
// call update(0) to apply.
void DialogPanel::reset(int hardReset) {

    if (!visible) {
        return;
    }

    visible = false;
    interpolatePosition = false;
    fadeProgress = (hardReset == 0) ? 0.0f : 1.0f;
    update(0.0f);
}

// reconstructed from Ghidra FUN_1000412e4 (the body of DialogPanel::showHint,
// dispatched from FUN_1000412bc which looks up TUTORIAL_HINT_TABLE[hintId]).
// populates the 3 text lines, sizes the popup, clamps its on-screen position,
// seeds the slide-in lerp, lays out the frame quads, plays the show sound, and
// applies the first frame via update(0). mirrors DetailPanel::layoutAndFade.
//
// `vGap` is the per-hint vertical gap between the anchor and the popup (the
// cascade passes 0.098 for board-tile hints, 0.047 for HUD hints). it is NOT a
// fade speed; the fade rate is the fixed constant inside update().
void DialogPanel::showHint(float vGap, const float pos[2], int hintId) {
    const TutorialHintEntry& h = TUTORIAL_HINT_TABLE[hintId];

    visible             = true;
    interpolatePosition = true;
    fadeProgress        = 0.0f;
    dismissReady        = false;
    dismissTriggered    = false;

    const float anchorX = pos[0];
    const float anchorY = pos[1];

    // clamp the anchor Y to the screen, then pick top/bottom + left/right.
    const float clampedY = clamp01(anchorY, 0.0f, Renderer::getVirtualHeight());
    configA = (clampedY < 0.625f);   // anchor in the top half of the screen
    // binary: anchorX != 0.5 && (anchorX < 0.5) == NAN(anchorX), i.e. anchorX > 0.5
    // (anchor in the right half). selects the mirrored tail sprite + tail offset.
    configB = (anchorX > 0.5f);

    textLines[0].setString(h.main, -1);
    textLines[1].setString(h.button, -1);
    textLines[2].setString(h.subtext, -1);

    // count the non-empty lines (drives the popup height).
    int lineCount = 0;

    for (int i = 0; i < 3; i++) {

        if (textLines[i].storedText.c_str()[0] != '\0') {
            lineCount++;
        }
    }

    // place each line + accumulate the widest scaled line (drives the width).
    float maxWidth = 0.0f;

    for (int i = 0; i < 3; i++) {
        textLines[i].posX = 0.0625f;                          // 40/640
        textLines[i].posY = (float)i * (26.0f / 640.0f)       // DAT_a428 line spacing
                            + (56.0f / 640.0f);               // DAT_a42c first-line Y
        const float scaled = textLines[i].renderedWidth * textLines[i].scaleX;

        if (scaled > maxWidth) {
            maxWidth = scaled;
        }
    }

    const float titleWidth  = maxWidth + (15.0f / 640.0f);                // DAT_a430 width pad
    const float panelHeight = (float)(lineCount * 0x18) / 640.0f          // DAT_a434
                              + (11.0f / 640.0f);                         // DAT_a438 height base

    // clamp posX so the popup fits with both screen margins.
    posX = clamp01(anchorX - titleWidth * 0.5f,
                   4.0f / 640.0f,                                         // DAT_a440
                   (1.0f - titleWidth) + (-66.0f / 640.0f));             // DAT_a43c

    // posY: top half hangs below the anchor by vGap; bottom half hangs above
    // by vGap + height. then a fixed per-orientation offset.
    if (configA) {
        posY = (clampedY + vGap) + (30.0f / 640.0f);                     // DAT_a444
    } else {
        posY = ((clampedY - vGap) - panelHeight) + (-94.0f / 640.0f);    // DAT_a448
    }

    posX = pixelSnap640(posX);
    posY = pixelSnap640(posY);

    // slide-in: top half springs down from above (-1), bottom springs up (+1).
    const float slideSign = configA ? -1.0f : 1.0f;
    lerpStartX  = posX;
    lerpStartY  = posY + slideSign * (30.0f / 640.0f);                   // DAT_a444
    lerpTargetX = posX;
    lerpTargetY = posY;

    // tail X within the panel: where the pointer sits along its edge. clamped so
    // it stays inside the panel even when the box was shoved off a screen edge,
    // which is what keeps the tail pointing at the anchor. configB nudges it
    // (DAT_a44c when the anchor is in the right half, DAT_a450 otherwise).
    const float tailOffset = configB ? (-17.0f / 640.0f)                 // DAT_a44c
                                     : (18.0f / 640.0f);                 // DAT_a450
    const float halfTailW = tailDown[0].quad.width * 0.5f;
    const float tailX = clamp01(
        (anchorX - posX - cornerTL.quad.width) + tailOffset,
        halfTailW,
        titleWidth - halfTailW);

    applyHintFrameLayout(titleWidth, panelHeight, tailX);

    if (Game* g = getGame()) {
        g->soundQueue.trigger(0xC);
    }

    update(0.0f);
}

// reconstructed from Ghidra FUN_1000415b0. lays out the 9-slice speech-bubble
// frame for the current popup width (w) and body height (bodyH). the tail sits
// at tailX along its edge; configA picks which edge carries it: configA (box
// below the anchor) -> top edge + up-tail; !configA (box above) -> bottom edge +
// down-tail. the tail's edge splits into a piece before the tail and one after
// it; the opposite edge stays a single full-width piece.
void DialogPanel::applyHintFrameLayout(float w, float bodyH, float tailX) {
    const float snappedW = pixelSnap640(w);

    // top edge: split around the up-tail when the tail is on top, else full width.
    if (!configA) {
        topEdge.quad.setSize(snappedW, topEdge.quad.height);
    } else {
        topEdge.quad.setSize(tailX - tailUp[0].quad.width * 0.5f,
                             topEdge.quad.height);
        topEdgeAfterTail.quad.setSize((snappedW - tailX) - tailUp[0].quad.width * 0.5f,
                                      topEdgeAfterTail.quad.height);
    }

    // bottom edge: split around the down-tail when the tail is on the bottom.
    if (!configA) {
        bottomEdge.quad.setSize(tailX - tailDown[0].quad.width * 0.5f,
                                bottomEdge.quad.height);
        bottomEdgeAfterTail.quad.setSize((snappedW - tailX) - tailDown[0].quad.width * 0.5f,
                                         bottomEdgeAfterTail.quad.height);
    } else {
        bottomEdge.quad.setSize(snappedW, bottomEdge.quad.height);
    }

    // middle row: left + right edges keep their width, the center fill spans the gap.
    leftEdge.quad.setSize(leftEdge.quad.width, bodyH);
    centerFill.quad.setSize(snappedW, bodyH);
    rightEdge.quad.setSize(rightEdge.quad.width, bodyH);

    // --- positions, all anchored off cornerTL (placed by init) ---
    const float baseX    = cornerTL.quad.posX;
    const float baseY    = cornerTL.quad.posY;
    const float halfMidH = leftEdge.quad.height * 0.5f;
    const float midRowY  = baseY + cornerTL.quad.height * 0.5f + halfMidH;

    // middle row, left to right: left edge, center fill, right edge.
    leftEdge.quad.posX = baseX;
    leftEdge.quad.posY = midRowY;

    const float halfCenterW = centerFill.quad.width * 0.5f;
    const float centerX     = baseX + leftEdge.quad.width * 0.5f + halfCenterW;
    centerFill.quad.posX = centerX;
    centerFill.quad.posY = midRowY;

    const float rightColX = centerX + halfCenterW + rightEdge.quad.width * 0.5f;
    rightEdge.quad.posX = rightColX;
    rightEdge.quad.posY = midRowY;

    // top row: top edge, then top-right corner.
    const float halfTopEdgeW = topEdge.quad.width * 0.5f;
    const float topEdgeX     = baseX + cornerTL.quad.width * 0.5f + halfTopEdgeW;
    topEdge.quad.posX = topEdgeX;
    topEdge.quad.posY = baseY;

    cornerTR.quad.posX = rightColX;
    cornerTR.quad.posY = baseY;

    if (configA) {   // up-tail + the top-edge segment trailing it.
        const float afterTopEdge = topEdgeX + halfTopEdgeW;
        const float halfTailW    = tailUp[0].quad.width * 0.5f;
        const float tailUpX      = afterTopEdge + halfTailW;
        tailUp[0].quad.posX = tailUpX;
        tailUp[0].quad.posY = baseY;
        tailUp[1].quad.posX = afterTopEdge + tailUp[1].quad.width * 0.5f;
        tailUp[1].quad.posY = baseY;
        topEdgeAfterTail.quad.posX = tailUpX + halfTailW + topEdgeAfterTail.quad.width * 0.5f;
        topEdgeAfterTail.quad.posY = baseY;
    }

    // bottom row: bottom-left corner, bottom edge, bottom-right corner.
    const float bottomRowY = midRowY + halfMidH + cornerBL.quad.height * 0.5f;
    cornerBL.quad.posX = baseX;
    cornerBL.quad.posY = bottomRowY;

    const float halfBottomEdgeW = bottomEdge.quad.width * 0.5f;
    const float bottomEdgeX     = baseX + cornerBL.quad.width * 0.5f + halfBottomEdgeW;
    bottomEdge.quad.posX = bottomEdgeX;
    bottomEdge.quad.posY = bottomRowY;

    cornerBR.quad.posX = rightColX;
    cornerBR.quad.posY = bottomRowY;

    if (!configA) {   // down-tail + the bottom-edge segment trailing it.
        const float afterBottomEdge = bottomEdgeX + halfBottomEdgeW;
        const float halfTailW       = tailDown[0].quad.width * 0.5f;
        const float tailDownX       = afterBottomEdge + halfTailW;
        tailDown[0].quad.posX = tailDownX;
        tailDown[0].quad.posY = bottomRowY;
        tailDown[1].quad.posX = afterBottomEdge + tailDown[1].quad.width * 0.5f;
        tailDown[1].quad.posY = bottomRowY;
        bottomEdgeAfterTail.quad.posX = tailDownX + halfTailW + bottomEdgeAfterTail.quad.width * 0.5f;
        bottomEdgeAfterTail.quad.posY = bottomRowY;
    }
}

// reconstructed from Ghidra FUN_100041188, push a single color to the panel.
void DialogPanel::applyColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // (r,g,b,a) onto every frame / button quad, in the binary's dispatch order.
    cornerTL           .quad.setColor(r, g, b, a);
    topEdge            .quad.setColor(r, g, b, a);
    topEdgeAfterTail   .quad.setColor(r, g, b, a);
    cornerTR           .quad.setColor(r, g, b, a);
    leftEdge           .quad.setColor(r, g, b, a);
    centerFill         .quad.setColor(r, g, b, a);
    rightEdge          .quad.setColor(r, g, b, a);
    cornerBL           .quad.setColor(r, g, b, a);
    bottomEdge         .quad.setColor(r, g, b, a);
    bottomEdgeAfterTail.quad.setColor(r, g, b, a);
    cornerBR           .quad.setColor(r, g, b, a);
    tailUp[0]          .quad.setColor(r, g, b, a);
    tailUp[1]          .quad.setColor(r, g, b, a);
    tailDown[0]        .quad.setColor(r, g, b, a);
    tailDown[1]        .quad.setColor(r, g, b, a);

    // alpha-only onto the 3 text lines; half-alpha onto the dark backdrop.
    for (int i = 0; i < 3; i++) {
        textLines[i].setAlpha(a);
    }

    backdropQuad.quad.setAlpha((uint8_t)(a >> 1));
}

// reconstructed from Ghidra FUN_100040fac. drives the show/hide animation.
// runs only while fading (fadeProgress < 1) OR the panel is still visible.
void DialogPanel::update(float dt) {

    if (!(fadeProgress < 1.0f || visible)) {
        return;
    }

    if (fadeProgress < 1.0f) {
        // --- fading: advance fadeProgress, lerp position (fade-in) or just
        //     ramp alpha (fade-out), then push the color. ---
        float fp = fadeProgress + dt / 0.2f;   // DAT_a420

        if (fp >= 1.0f) {
            fp = 1.0f;
        }

        fadeProgress = fp;

        float alphaFactor;

        if (interpolatePosition) {
            // fade-in with slide: position lerps start -> target by fadeProgress,
            // alpha ramps up with it.
            posX = (1.0f - fp) * lerpStartX + fp * lerpTargetX;
            posY = (1.0f - fp) * lerpStartY + fp * lerpTargetY;
            alphaFactor = fp;
        } else {
            // fade-out (reset / dismiss): alpha ramps down, no slide.
            alphaFactor = 1.0f - fp;
        }

        uint8_t alpha = (uint8_t)(int)(alphaFactor * 255.0f);   // DAT_a424 = 255
        applyColor(0xFF, 0xFF, 0xFF, alpha);
        return;
    }

    // --- settled (fadeProgress >= 1.0) and visible: handle dismiss input. ---
    if (!visible) {
        return;
    }

    Game* g = getGame();

    if (g == nullptr) {
        return;
    }

    if (g->inputState() == 1) {
        // finger down: once dismissReady is set, hit-test inside the panel
        // bounds (panel origin -> cornerBR center) and commit the dismiss.
        if (dismissReady) {
            float dx = g->touchX() - posX;
            float boundX = cornerBR.quad.posX + cornerBR.quad.width * 0.5f;

            if (dx > 0.0f && dx < boundX) {
                float dy = g->touchY() - posY;
                float boundY = cornerBR.quad.posY + cornerBR.quad.height * 0.5f;

                if (dy > 0.0f && dy < boundY) {
                    dismissTriggered = true;
                    applyColor(0xC8, 0xC8, 0xC8, 0xFF);   // pressed flash (0xffc8c8c8)
                    g->soundQueue.trigger(0xD);
                }
            }
        }
    } else if (g->inputState() == 0) {
        // finger up: set dismissReady on the first release, then close on the
        // release that follows a committed press.
        if (!dismissTriggered) {
            dismissReady = true;
        } else if (visible) {
            visible = false;
            interpolatePosition = false;
            fadeProgress = 0.0f;
            update(0.0f);   // re-enter the fading branch to start the fade-out
        }
    }
}

// reconstructed from Ghidra FUN_100040e9c
void DialogPanel::draw() {
    // early-out: hidden AND fade complete
    if (!visible && fadeProgress >= 1.0f) {
        return;
    }

    // backdrop: drawn first, on tex 0 (solid color, no texture sample)
    bindTexture(0);
    backdropQuad.quad.draw();

    bindTexture(9);
    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    cornerTL.quad.draw();
    topEdge.quad.draw();
    cornerTR.quad.draw();
    leftEdge.quad.draw();
    centerFill.quad.draw();
    rightEdge.quad.draw();
    cornerBL.quad.draw();
    bottomEdge.quad.draw();
    cornerBR.quad.draw();

    // pick the button-pair configuration
    if (configA) {
        // configA=1 path (tailUp pair + topEdgeAfterTail)
        if (configB) {
            tailUp[1].quad.draw();
        } else {
            tailUp[0].quad.draw();
        }
        topEdgeAfterTail.quad.draw();
    } else {
        // configA=0 path (tailDown pair + bottomEdgeAfterTail)
        if (configB) {
            tailDown[1].quad.draw();
        } else {
            tailDown[0].quad.draw();
        }
        bottomEdgeAfterTail.quad.draw();
    }

    // text items, binary: loop FUN_100030014 (TextItem::draw) over textLines.
    for (int i = 0; i < 3; i++) {
        textLines[i].draw();
    }

    glPopMatrix();
}
