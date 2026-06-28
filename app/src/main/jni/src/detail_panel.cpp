#include "detail_panel.h"
#include "game.h"
#include "game_board.h"
#include "item.h"
#include "item_table.h"
#include "perk.h"
#include "renderer.h"   // bindTexture
#include "hex_map_table.h"
#include "snag_content.h"
#include "snag_table.h"
#include "event_slot.h"
#include "tile_content.h"
#include "tile_content_table.h"
#include <GLES/gl.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// reconstructed from Ghidra FUN_10003e5e0
void DetailPanel::init() {
    mode = 0;
    visible = false;

    // 13 frame TileIcons + iconQuad + outerFrame: bare TileIcon init (vtable set,
    // verts in default unit-quad layout). UVs/sizes assigned below for the
    // frames that the binary configures statically; iconQuad and outerFrame
    // stay at default (0..1) UVs, modes 1..5 reconfigure them at draw time.
    for (int i = 0; i < 13; i++) {
        frames[i] = TileIcon();
    }
    iconQuad   = TileIcon();
    outerFrame = TileIcon();

    iconTexIndex = 0;

    // 3 ColorTints
    tintFrame.init();

    // text items: the binary calls FUN_10002fa50 on textCenter (with the
    // panel font glyph table) and FUN_10002fa08 on each of textLines[].
    // textLines get their glyphTablePtr written to the panel font glyph table
    // in the per-line loop further down. that table is BMFontTable[0]
    // ("font.fnt"), populated by Game::loadFonts at startup.
    {
        Game* g = getGame();
        const BMFontTable* glyphSrc = g ? g->bmfontTablePtr(0) : nullptr;
        textCenter.init(glyphSrc);
    }

    for (int i = 0; i < 3; i++) {
        textLines[i].init();
    }

    hasTitle = true;
    combatSimPreview = TileIcon();

    snagDeathTurns.init();
    playerDeathTurns.init();

    snagPreviewEnabled = false;
    perkSource = nullptr;

    currentAlpha = 0;
    fadeT = 1.0f;

    startPosX  = 0.0f;
    startPosY  = 0.0f;
    targetPosX = 0.0f;
    targetPosY = 0.0f;

    interpolatePosition = false;
    touchHoldArea = 0;
    touchOrigX = 0.0f;
    touchOrigY = 0.0f;

    // helper to assign UV+size in one go
    auto setTile = [](TileIcon& t, float u0, float v0, float u1, float v1,
                      float w, float h) {
        t.quad.setTexCoords(u0, v0, u1, v1);
        t.quad.setSize(w, h);
    };

    // frames[0]: top-left corner of the panel border
    setTile(frames[0], 0.06542969f, 0.44726563f, 0.09082031f, 0.4716797f,
            0.040625f, 0.0390625f);
    // frames[1]: top edge horizontal stripe (1px-wide source, stretched)
    setTile(frames[1], 0.09082031f, 0.44726563f, 0.091796875f, 0.4716797f,
            0.0015625f, 0.0390625f);
    // frames[3]: same UV as frames[1] (decoration variant when subMode==1)
    setTile(frames[3], 0.09082031f, 0.44726563f, 0.091796875f, 0.4716797f,
            0.0015625f, 0.0390625f);
    // frames[4]: top-right corner
    setTile(frames[4], 0.091796875f, 0.44726563f, 0.119140625f, 0.4716797f,
            0.04375f, 0.0390625f);
    // frames[5]: left edge vertical stripe
    setTile(frames[5], 0.06542969f, 0.4716797f, 0.09082031f, 0.47265625f,
            0.040625f, 0.0015625f);
    // frames[6]: 1x1 fill pixel for the panel body
    setTile(frames[6], 0.09082031f, 0.4716797f, 0.091796875f, 0.47265625f,
            0.0015625f, 0.0015625f);
    // frames[7]: right edge vertical stripe
    setTile(frames[7], 0.091796875f, 0.4716797f, 0.119140625f, 0.47265625f,
            0.04375f, 0.0015625f);
    // frames[8]: bottom-left corner
    setTile(frames[8], 0.06542969f, 0.47265625f, 0.09082031f, 0.5f,
            0.040625f, 0.04375f);
    // frames[9]: bottom edge horizontal stripe
    setTile(frames[9], 0.09082031f, 0.47265625f, 0.091796875f, 0.5f,
            0.0015625f, 0.04375f);
    // frames[11]: same UV as frames[9] (decoration variant when subMode==2)
    setTile(frames[11], 0.09082031f, 0.47265625f, 0.091796875f, 0.5f,
            0.0015625f, 0.04375f);
    // frames[12]: bottom-right corner
    setTile(frames[12], 0.091796875f, 0.47265625f, 0.119140625f, 0.5f,
            0.04375f, 0.04375f);
    // frames[10]: large title-banner sprite (wide, occupies the panel head)
    setTile(frames[10], 0.0009765625f, 0.44921875f, 0.06347656f, 0.5f,
            0.1f, 0.08125f);
    // frames[2]: small icon background (used when subMode==1 with frames[3])
    setTile(frames[2], 0.12109375f, 0.45117188f, 0.17871094f, 0.49902344f,
            0.0921875f, 0.0765625f);
    // combatSimPreview: a small tile-preview quad (drawn in mode 0 when enabled)
    setTile(combatSimPreview, 0.07421875f, 0.19433594f, 0.18554688f, 0.26367188f,
            0.178125f, 0.1109375f);

    // frames[0]: posX = width/2, posY = height/2 (binary's standard pattern).
    frames[0].quad.posX = frames[0].quad.width  * 0.5f;
    frames[0].quad.posY = frames[0].quad.height * 0.5f;

    // vertex-offset adjustments for the two decoration variant icons
    // (FUN_100008238). shift the icon up/down by one half-step so the title
    // sprite hugs the top and the subMode==1 icon sits below.
    frames[2].quad.addVertexOffset(0.0f, -0.01875f);
    frames[10].quad.addVertexOffset(0.0f, 0.01875f);

    // text-line setup. binary writes per line (FUN_10003e5e0):
    //   glyphTablePtr = the panel font glyph table
    //   scaleX = scaleY = 0.07
    //   rgba = 0xffb4aab4
    //   applyColor()
    // and textCenter gets its own scale (0.085, 0.085) before the loop.
    {
        Game* g = getGame();
        const BMFontTable* glyphSrc = g ? g->bmfontTablePtr(0) : nullptr;

        textCenter.scaleX = 0.085f;
        textCenter.scaleY = 0.085f;

        for (int i = 0; i < 3; i++) {
            textLines[i].glyphTablePtr = glyphSrc;
            textLines[i].scaleX        = 0.07f;
            textLines[i].scaleY        = 0.07f;
            textLines[i].rgba          = 0xffb4aab4u;
            textLines[i].applyColor();
        }
    }
}

// reconstructed from Ghidra FUN_10003ef28
void DetailPanel::draw() {
    // early-out: if hidden and fade has completed, skip the whole pass
    if (!visible && fadeT >= 1.0f) {
        return;
    }

    bindTexture(9);
    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    // always-drawn frame quads (9-slice border)
    frames[0].quad.draw();
    frames[1].quad.draw();
    frames[4].quad.draw();
    frames[5].quad.draw();
    frames[6].quad.draw();
    frames[7].quad.draw();
    frames[8].quad.draw();
    frames[9].quad.draw();
    frames[12].quad.draw();

    // subMode==1: draw the first decoration variant pair
    if (subMode == 1) {
        frames[2].quad.draw();
        frames[3].quad.draw();
    }
    // subMode==2: draw the second decoration variant pair
    if (subMode == 2) {
        frames[10].quad.draw();
        frames[11].quad.draw();
    }

    // mode-dispatch (matches the binary's switch on *param_1).
    // every non-default branch finishes with bindTexture + draw of either
    // iconQuad or outerFrame. mode 0 additionally draws the snag combat
    // preview + outer-frame stat block before the icon.
    switch (mode) {
        case 0:
            // snag card: combat preview (gated) + outerFrame + tintFrame,
            // then bindTexture(iconTexIndex) + draw iconQuad.
            if (snagPreviewEnabled) {
                combatSimPreview.quad.draw();
                playerDeathTurns.draw();
                snagDeathTurns.draw();
            }
            outerFrame.quad.draw();
            tintFrame.draw();
            bindTexture((GLuint)iconTexIndex);
            iconQuad.quad.draw();
            break;
        case 1:
        case 5:
            // content / hexmap card: bindTexture(9), draw iconQuad.
            bindTexture(9);
            iconQuad.quad.draw();
            break;
        case 2:
            // Item card. bindTexture(11) = items1.
            bindTexture(11);
            iconQuad.quad.draw();
            break;
        case 3:
            // perk preview: forward to Perk::drawAt with the iconQuad's
            // anchor and current fade alpha. encounter.update populates
            // perkSource via FUN_10003f6f8 prior to setting mode = 3.
            if (perkSource) {
                perkSource->drawAt(iconQuad.quad.posX,
                                   iconQuad.quad.posY,
                                   currentAlpha);
            }
            break;
        case 4:
            // Event card (mode 4). texture stays bound at 9 (already bound
            // for the frame draws above); render outerFrame instead of
            // iconQuad.
            outerFrame.quad.draw();
            break;
        default:
            // mode 6 (Nemesis) and other future modes: text-only card.
            break;
    }

    // text items (textCenter + 3 textLines). binary draws the title
    // (textCenter) only when hasTitle is set, then the 3 lines unconditionally.
    if (hasTitle) {
        textCenter.draw();
    }

    for (int i = 0; i < 3; i++) {
        textLines[i].draw();
    }

    glPopMatrix();
}

// reconstructed from Ghidra FUN_1000404b0 (panel reset / dismiss).
// early-out if already invisible. otherwise: clear visible, clear
// interpolatePosition, set fadeT = 0 (animate out) or 1 (snap), then apply
// via a single update(0.0) tick, same dispatch the binary does.
void DetailPanel::reset(int hardReset) {

    if (!visible) {
        return;
    }

    visible = false;
    interpolatePosition = false;
    fadeT = (hardReset == 0) ? 0.0f : 1.0f;
    update(0.0f);
}

// reconstructed from Ghidra FUN_10003f0e4
void DetailPanel::update(float dt) {

    if (fadeT >= 1.0f) {
        return;
    }

    constexpr float FADE_DURATION = 0.2f;   // DAT_10005a378
    constexpr float MAX_ALPHA     = 255.0f; // DAT_10005a37c

    fadeT += dt / FADE_DURATION;
    if (fadeT > 1.0f) {
        fadeT = 1.0f;
    }

    // fade direction: when interpolatePosition is set, fade in (alpha rises
    // 0->255 over fadeT 0->1) and lerp posX/posY from start to target.
    // when clear, fade out (alpha rises but with inverted t, uses 1-t).
    float t = fadeT;

    if (interpolatePosition) {
        posX = (1.0f - t) * startPosX + t * targetPosX;
        posY = (1.0f - t) * startPosY + t * targetPosY;
    } else {
        t = 1.0f - t;
    }

    currentAlpha = (uint8_t)(int)(t * MAX_ALPHA);

    // apply alpha to every frame, optional quad, icon quad, outer frame,
    // and the 3 ColorTints. matches the binary's per-quad FUN_100008388
    // and per-tint FUN_10003c948 calls.
    frames[0].quad.setAlpha(currentAlpha);
    frames[1].quad.setAlpha(currentAlpha);
    frames[3].quad.setAlpha(currentAlpha);
    frames[4].quad.setAlpha(currentAlpha);
    frames[5].quad.setAlpha(currentAlpha);
    frames[6].quad.setAlpha(currentAlpha);
    frames[7].quad.setAlpha(currentAlpha);
    frames[8].quad.setAlpha(currentAlpha);
    frames[9].quad.setAlpha(currentAlpha);
    frames[11].quad.setAlpha(currentAlpha);
    frames[12].quad.setAlpha(currentAlpha);
    frames[2].quad.setAlpha(currentAlpha);
    frames[10].quad.setAlpha(currentAlpha);
    combatSimPreview.quad.setAlpha(currentAlpha);

    playerDeathTurns.setAlpha(currentAlpha);
    snagDeathTurns.setAlpha(currentAlpha);
    outerFrame.quad.setAlpha(currentAlpha);
    tintFrame.setAlpha(currentAlpha);
    iconQuad.quad.setAlpha(currentAlpha);

    // text items get FUN_1000301fc (TextItem::setAlpha) propagating the
    // panel's currentAlpha through their glyph vectors.
    textCenter.setAlpha(currentAlpha);
    for (int i = 0; i < 3; i++) {
        textLines[i].setAlpha(currentAlpha);
    }
}

namespace {

// FUN_100057374, pixel-snap to a 1/640 grid; same code used elsewhere in the
// port. local copy here so we don't drag GameBoard / Renderer into the panel
// for a 4-line math helper.
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

// reconstructed from Ghidra FUN_100040234 (the panel-frame layout helper).
//
// after layoutAndFade has computed the title-bar width, panel height, and
// title-x offset, this places every frame Quad's pos/size to draw the
// 9-slice border + the title-bar half-strips on either side of the title
// icon. orientation:
//   1: title icon nests in the top edge (frames[1]/[3] split, frames[2] icon)
//   2: title icon nests in the bottom edge (frames[9]/[11] split, frames[10] icon)
//
// frame anatomy (9-slice border):
//   [0] top-left corner       [4] top-right corner
//   [1] top edge stripe       [3] top edge stripe (orientation 1 right half)
//   [2] title icon (orientation 1)
//   [5] left edge stripe      [7] right edge stripe
//   [6] body fill
//   [8] bottom-left corner    [12] bottom-right corner
//   [9] bottom edge stripe    [11] bottom edge stripe (orientation 2 right half)
//   [10] title icon (orientation 2)
//
// titleWidth: already snapped width of the whole title bar (>= title text)
// panelHeight: vertical span between top + bottom edges
// titleXOffset: distance from panel left edge to the icon's left side
void DetailPanel::applyLayoutGeometry(float titleWidth, float panelHeight,
                                      float titleXOffset, int orientation) {
    float snappedTW = pixelSnap640(titleWidth);
    subMode = orientation;

    // ---- sizes ----
    // top edge: orientation 1 splits frames[1] (left half) + frames[3] (right
    // half) around the small icon frames[2]. orientation 2 stretches frames[1]
    // across the full top width and leaves frames[3] alone.
    if (orientation == 1) {
        frames[1].quad.setSize(titleXOffset - frames[2].quad.width * 0.5f,
                               frames[1].quad.height);
        frames[3].quad.setSize((snappedTW - titleXOffset) - frames[2].quad.width * 0.5f,
                               frames[3].quad.height);
    } else {
        frames[1].quad.setSize(snappedTW, frames[1].quad.height);
    }

    // bottom edge: mirror of the top, with frames[9]/[11] around frames[10].
    if (orientation == 2) {
        frames[9].quad.setSize(titleXOffset - frames[10].quad.width * 0.5f,
                               frames[9].quad.height);
        frames[11].quad.setSize((snappedTW - titleXOffset) - frames[10].quad.width * 0.5f,
                                frames[11].quad.height);
    } else {
        frames[9].quad.setSize(snappedTW, frames[9].quad.height);
    }

    // side edges + body fill stretch over the panel's vertical span.
    frames[5].quad.setSize(frames[5].quad.width, panelHeight);
    frames[6].quad.setSize(snappedTW,            panelHeight);
    frames[7].quad.setSize(frames[7].quad.width, panelHeight);

    // ---- positions ----
    // frames[0] (top-left corner) keeps its origin from init(). everything
    // else gets placed relative to it.
    float tl_x = frames[0].quad.posX;
    float tl_y = frames[0].quad.posY;

    // mid-row Y baseline (left edge, body fill, right edge).
    //   bodyMidY = TL.posY + TL.height/2 + left.height/2
    float bodyMidY = tl_y + frames[0].quad.height * 0.5f
                          + frames[5].quad.height * 0.5f;

    // left edge sits at (TL.posX, bodyMidY).
    frames[5].quad.posX = tl_x;
    frames[5].quad.posY = bodyMidY;

    // body fill: half(left.width) + half(body.width) past TL.posX.
    float bodyCenterX = tl_x + frames[5].quad.width * 0.5f
                             + frames[6].quad.width * 0.5f;
    frames[6].quad.posX = bodyCenterX;
    frames[6].quad.posY = bodyMidY;

    // right edge: half(body.width) + half(right.width) past body center.
    float rightCenterX = bodyCenterX + frames[6].quad.width * 0.5f
                                     + frames[7].quad.width * 0.5f;
    frames[7].quad.posX = rightCenterX;
    frames[7].quad.posY = bodyMidY;

    // top-edge stripe frames[1] sits immediately to the right of TL corner:
    //   frames[1].posX = TL.posX + TL.width/2 + frames[1].width/2
    //   frames[1].posY = TL.posY
    // and top-right corner frames[4] sits at (rightCenterX, TL.posY), same
    // X as the right-edge stripe.
    float topStripeX = tl_x + frames[0].quad.width * 0.5f
                            + frames[1].quad.width * 0.5f;
    frames[1].quad.posX = topStripeX;
    frames[1].quad.posY = tl_y;
    frames[4].quad.posX = rightCenterX;
    frames[4].quad.posY = tl_y;

    // orientation 1: title icon nests in the top edge between frames[1] and
    // frames[3]. frames[2] (icon BG) sits past frames[1]; frames[3] (right
    // half stripe) sits past frames[2].
    if (orientation == 1) {
        float icoX = topStripeX + frames[1].quad.width * 0.5f
                                + frames[2].quad.width * 0.5f;
        frames[2].quad.posX = icoX;
        frames[2].quad.posY = tl_y;
        frames[3].quad.posX = icoX + frames[2].quad.width * 0.5f
                                   + frames[3].quad.width * 0.5f;
        frames[3].quad.posY = tl_y;
    }

    // bottom-edge Y baseline:
    //   bottomY = bodyMidY + left.height/2 + BL.height/2
    float bottomY = bodyMidY + frames[5].quad.height * 0.5f
                             + frames[8].quad.height * 0.5f;

    // bottom-left corner.
    frames[8].quad.posX = tl_x;
    frames[8].quad.posY = bottomY;

    // bottom-edge stripe frames[9] sits to the right of BL corner. note: the
    // binary uses BL.width here (frames[8].width), not left-edge.width.
    float bottomStripeX = tl_x + frames[8].quad.width * 0.5f
                               + frames[9].quad.width * 0.5f;
    frames[9].quad.posX = bottomStripeX;
    frames[9].quad.posY = bottomY;

    // bottom-right corner -> same X as right-edge stripe, same Y as bottom row.
    frames[12].quad.posX = rightCenterX;
    frames[12].quad.posY = bottomY;

    // orientation 2: title icon nests in the bottom edge between frames[9]
    // and frames[11].
    if (orientation == 2) {
        float icoX = bottomStripeX + frames[9].quad.width * 0.5f
                                   + frames[10].quad.width * 0.5f;
        frames[10].quad.posX = icoX;
        frames[10].quad.posY = bottomY;
        frames[11].quad.posX = icoX + frames[10].quad.width * 0.5f
                                    + frames[11].quad.width * 0.5f;
        frames[11].quad.posY = bottomY;
    }
}

// reconstructed from Ghidra FUN_10003f3b8 (the shared text-layout core).
//
// every populator funnels through here after setting `mode`. the function:
//   - marks the panel visible + starts the fade-in by setting fadeT = 0
//   - writes the 4 text strings into textCenter + textLines[0..2]
//   - counts non-empty desc lines to pick compact vs full layout metrics
//   - sizes the title scale via mode5Bit (wider for the mode-5 / hex-map card)
//   - computes the panel posX/posY clamp against the anchor point and screen
//   - sets startPos = posY + tiny offset (sign depends on portrait/landscape)
//     so the panel slides into target by lerp during update()
//   - hands off to applyLayoutGeometry to place every frame
//   - triggers the popup sound (slot 0x13).
void DetailPanel::layoutAndFade(float headerY, const float* anchor,
                                uint8_t inhibitOpt, uint32_t mode5Bit,
                                const char* title, const char* desc0,
                                const char* desc1, const char* desc2) {
    visible             = true;
    interpolatePosition = true;
    fadeT               = 0.0f;

    float anchorX = anchor[0];
    float anchorY = anchor[1];

    hasTitle = (title != nullptr);
    if (title == nullptr) {
        title = "";
    }
    textCenter   .setString(title, -1);
    textLines[0] .setString(desc0 ? desc0 : "", -1);
    textLines[1] .setString(desc1 ? desc1 : "", -1);
    textLines[2] .setString(desc2 ? desc2 : "", -1);

    int nonEmpty = 0;
    for (int i = 0; i < 3; i++) {
        const char* s = textLines[i].storedText.c_str();
        if (s[0] != '\0') {
            nonEmpty++;
        }
    }

    int orientation = (anchorY < 0.625f) ? 1 : 2;

    // title-text scale + x baseline. mode5Bit=1 widens the title bar so it
    // can host the HexMap cell icon comfortably; mode5Bit=0 keeps the
    // standard narrow title scale.
    float titleBaselineX;
    if (mode5Bit & 1u) {
        textCenter.posX = 0.203125f;     // wide title bar (mode 5)
        textCenter.posY = 0.071875f;
        titleBaselineX  = 0.203125f;
    } else {
        textCenter.posX = 0.054688f;
        textCenter.posY = 0.071875f;
        titleBaselineX  = 0.054688f;
    }

    // y baseline + per-line spacing depend on how many desc lines are non-empty.
    float baseY      = (nonEmpty == 3) ? 0.114063f : 0.134375f;
    float lineSpacing= (nonEmpty == 3) ? 0.037500f : 0.040625f;
    if (!hasTitle) {
        baseY += -0.040625f;
    }

    // place the 3 description lines + accumulate the widest scaled line.
    float maxWidth = textCenter.renderedWidth * textCenter.scaleX;
    for (int i = 0; i < 3; i++) {
        textLines[i].posX = titleBaselineX;
        textLines[i].posY = baseY + lineSpacing * (float)i;
        float scaled = textLines[i].renderedWidth * textLines[i].scaleX;
        if (scaled > maxWidth) {
            maxWidth = scaled;
        }
    }

    // title vertical override: moves textCenter slightly higher when all
    // 3 desc lines are filled so the title doesn't crowd the top edge.
    textCenter.posY = (nonEmpty == 3) ? 0.071875f : 0.081250f;

    // total title-bar width = (constant padding) + max line width.
    float titleBaselinePad = (mode5Bit & 1u) ? 0.171875f : 0.023438f;
    float titleWidth       = titleBaselinePad + maxWidth;

    // clamp panel posX so the title bar fits on screen with both margins.
    posX = clamp01(anchorX - titleWidth * 0.5f,
                   0.006250f,
                   1.0f - titleWidth - 0.087500f);

    // posY pre-snap depends on orientation. portrait: panel hangs below the
    // anchor (anchor.y + headerY). landscape: panel hangs above the anchor
    // by an extra -0.259375.
    if (anchorY < 0.625f) {
        posY = anchorY + headerY;
    } else {
        posY = anchorY - headerY - 0.259375f;
    }

    // pixel-snap both anchors.
    posX = pixelSnap640(posX);
    posY = pixelSnap640(posY);

    // start / target positions for the slide-in animation. an upper anchor
    // (anchorY < 0.625) drops in from above; a lower anchor rises from below.
    // DAT_10005a3f0 = {+1.0, -1.0} indexed by (anchorY < 0.625).
    float slideSign = (anchorY < 0.625f) ? -1.0f : 1.0f;
    startPosX = posX;
    startPosY = posY + slideSign * 0.031250f;
    targetPosX = posX;
    targetPosY = posY;

    // panel total height: compact when few desc lines and inhibitOpt set;
    // shrink further when there's no title at all.
    bool compactMode = (nonEmpty < 2) && (inhibitOpt != 0);
    float panelHeight = compactMode ? 0.115625f : 0.156250f;
    if (!hasTitle) {
        panelHeight += -0.031250f;
    }

    // title-x offset inside the panel (where the title-bar icon nests).
    float halfTitleSprite = frames[10].quad.width * 0.5f;
    float titleXOffset    = clamp01(anchorX - posX - frames[0].quad.width,
                                    halfTitleSprite,
                                    titleWidth - halfTitleSprite);

    applyLayoutGeometry(titleWidth, panelHeight, titleXOffset, orientation);

    // popup sound: iOS uses slot 0x13 ("popup chime").
    if (Game* g = getGame()) {
        g->soundQueue.trigger(0x13);
    }
}

// reconstructed from Ghidra FUN_10003fd34. mode 6 (Nemesis info card).
// title + 2 desc lines are sprintf'd inline with the current nemesis level
// (plural "s" / "" picker for tile counts); desc2 is a literal.
void DetailPanel::populateForNemesis(float headerY, const float* anchor) {
    mode = 6;

    Game* g = getGame();
    int level = 0;
    if (g && g->boardPtr()) {
        level = g->boardPtr()->nemesis.nemesisLevel;
    }

    char titleBuf[64];
    char d0Buf[256];
    char d1Buf[256];
    const char* plural = (level == 1) ? "" : "s";

    std::snprintf(titleBuf, sizeof(titleBuf), "Nemesis (Level %d)", level);
    std::snprintf(d0Buf, sizeof(d0Buf),
                  "Consumes nearest {X} or %d tile%s when you discard.",
                  level, plural);
    std::snprintf(d1Buf, sizeof(d1Buf),
                  "Devours %d {X} tile%s when your {H} reaches zero.",
                  level, plural);

    layoutAndFade(headerY, anchor, /*inhibitOpt=*/0, /*mode5Bit=*/0,
                  titleBuf, d0Buf, d1Buf,
                  "Gains 1 experience per Snag you discard.");
    update(0.0f);
}

// reconstructed from Ghidra FUN_10003fb44. mode 1 (content tile card).
// looks up name / 3 desc lines + UV from the per-content-type tables, sets
// iconQuad to the content sprite, then hands off to the shared layout core.
void DetailPanel::populateForContent(float headerY, const float* anchor,
                                     TileContent* content) {
    mode = 1;

    int contentType = content ? content->type : 0;

    // icon UV: same path as the in-game tile sprite (FUN_100014980).
    float uvOriginPx[2] = {0, 0};
    float uvSizePx[2]   = {0, 0};
    lookupContentIconUVPx(contentType, uvOriginPx, uvSizePx);

    // FUN_100014d84 converts pixel UV/size to the 0..1 quad space at scale=1.
    // we already have the per-pixel constants in tile_content.cpp; replicate
    // its math inline so this populator stays self-contained.
    constexpr float TEX_PIXEL_INV    = 1.0f / 1024.0f;
    constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;
    float u0 = uvOriginPx[0]                * TEX_PIXEL_INV;
    float v0 = uvOriginPx[1]                * TEX_PIXEL_INV;
    float u1 = (uvOriginPx[0] + uvSizePx[0]) * TEX_PIXEL_INV;
    float v1 = (uvOriginPx[1] + uvSizePx[1]) * TEX_PIXEL_INV;
    iconQuad.quad.setTexCoords(u0, v0, u1, v1);
    iconQuad.quad.setSize(uvSizePx[0] * SCREEN_PIXEL_INV,
                          uvSizePx[1] * SCREEN_PIXEL_INV);

    // iconQuad position inside the panel. 0.117188 is the icon-anchor offset
    // both modes 1/5 use; snap to the 1/640 pixel grid.
    iconQuad.quad.posX = 0.117188f;
    iconQuad.quad.posY = 0.117188f;
    iconQuad.quad.snapToPixelGrid();

    const TileTextEntry& e = kTileTextTable[contentType];
    layoutAndFade(headerY, anchor, /*inhibitOpt=*/0, /*mode5Bit=*/1,
                  e.name, e.desc1, e.desc2, e.desc3);
    update(0.0f);
}

// reconstructed from Ghidra FUN_10003fc3c. mode 5 (hex-map cell card).
void DetailPanel::populateForHexMapCell(float headerY, const float* anchor,
                                        uint32_t cellType) {
    mode = 5;

    float uvOriginPx[2] = {0, 0};
    float uvSizePx[2]   = {0, 0};
    lookupHexMapCellUVPx(cellType, uvOriginPx, uvSizePx);

    constexpr float TEX_PIXEL_INV    = 1.0f / 1024.0f;
    constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;
    float u0 = uvOriginPx[0]                * TEX_PIXEL_INV;
    float v0 = uvOriginPx[1]                * TEX_PIXEL_INV;
    float u1 = (uvOriginPx[0] + uvSizePx[0]) * TEX_PIXEL_INV;
    float v1 = (uvOriginPx[1] + uvSizePx[1]) * TEX_PIXEL_INV;
    iconQuad.quad.setTexCoords(u0, v0, u1, v1);
    iconQuad.quad.setSize(uvSizePx[0] * SCREEN_PIXEL_INV,
                          uvSizePx[1] * SCREEN_PIXEL_INV);

    // iconQuad position inside the panel. 0.117188 is
    // the icon-anchor offset both modes 1/5 use; snap to the 1/640 pixel grid.
    iconQuad.quad.posX = 0.117188f;
    iconQuad.quad.posY = 0.117188f;
    iconQuad.quad.snapToPixelGrid();

    layoutAndFade(headerY, anchor, /*inhibitOpt=*/0, /*mode5Bit=*/1,
                  hexMapCellName(cellType),
                  hexMapCellDesc(cellType, 0),
                  hexMapCellDesc(cellType, 1),
                  hexMapCellDesc(cellType, 2));
    update(0.0f);
}

// reconstructed from Ghidra FUN_10003f2c0. mode 2 (Item gear card).
//
// structurally identical to populateForContent: configures iconQuad with
// the per-Item silhouette atlas rect from ICON_RECTS, anchors it at the
// fixed (0.117188, 0.117188) icon position, then hands the title + 3 desc
// lines to layoutAndFade. mode 2 selects the items1 (tex 11) icon variant
// inside DetailPanel::draw.
void DetailPanel::populateForItem(float headerY, const float* anchor,
                                  Item* item) {
    mode = 2;

    // icon UV: per-(type, subType) silhouette from ICON_RECTS. binary calls
    // FUN_10003326c which writes 4 ints into a pair of local floats; we
    // already have the same table indexed in item_data_table.h.
    const IconRect& rect = ICON_RECTS[item->type][item->subType];

    constexpr float TEX_PIXEL_INV    = 1.0f / 1024.0f;
    constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;
    float u0 = (float)rect.atlasX                * TEX_PIXEL_INV;
    float v0 = (float)rect.atlasY                * TEX_PIXEL_INV;
    float u1 = (float)(rect.atlasX + rect.atlasW) * TEX_PIXEL_INV;
    float v1 = (float)(rect.atlasY + rect.atlasH) * TEX_PIXEL_INV;
    iconQuad.quad.setTexCoords(u0, v0, u1, v1);
    iconQuad.quad.setSize((float)rect.atlasW * SCREEN_PIXEL_INV,
                          (float)rect.atlasH * SCREEN_PIXEL_INV);

    // same (0.117188, 0.117188) icon anchor as populateForContent /
    // populateForHexMapCell.
    iconQuad.quad.posX = 0.117188f;
    iconQuad.quad.posY = 0.117188f;
    iconQuad.quad.snapToPixelGrid();

    // title + 3 desc lines via Item accessors. getDescriptionLine(2) always
    // returns "" (line 2+ guard inside Item::getDescriptionLine), but the
    // binary passes the call through and layoutAndFade counts non-empty
    // lines internally, so an empty 3rd line is harmless and faithful.
    layoutAndFade(headerY, anchor, /*inhibitOpt=*/0, /*mode5Bit=*/1,
                  item->getName(),
                  item->getDescriptionLine(0),
                  item->getDescriptionLine(1),
                  item->getDescriptionLine(2));
    update(0.0f);
}

// reconstructed from Ghidra FUN_100040124. mode 1 with caller-supplied
// strings and an optional content-type icon. used exclusively from
// UserStatsPanel's per-row hit-tests: each stat row's chrome / hit-test
// Quad dispatch hard-coded "Attack Range" / "Heal" / "Defence" / etc.
// labels here, and the bottom-stats hit dispatches contentType=0 +
// null name + three literal description lines.
//
// when contentType == 0, iconQuad is zeroed out (no icon rendered);
// when nonzero, the same FUN_100014980 + FUN_100014d84 path used by
// populateForContent installs the matching content-tile UV.
void DetailPanel::populateForStatRow(float headerY, const float* anchor,
                                     uint32_t contentType,
                                     const char* name,
                                     const char* desc0,
                                     const char* desc1,
                                     const char* desc2) {
    mode = 1;

    if (contentType == 0) {
        // binary inlines two FUN_100008* calls that zero the iconQuad
        // tex-coord rectangle + size pair to (0, 0, 0, 0).
        iconQuad.quad.setTexCoords(0.0f, 0.0f, 0.0f, 0.0f);
        iconQuad.quad.setSize(0.0f, 0.0f);
    }
    else {
        // same content-icon path as populateForContent: pixel UV -> normalized
        // UV via TEX_PIXEL_INV; pixel size -> quad size via SCREEN_PIXEL_INV.
        // anchor pinned to (0.117188, 0.117188).
        float uvOriginPx[2] = {0, 0};
        float uvSizePx[2]   = {0, 0};
        lookupContentIconUVPx((int)contentType, uvOriginPx, uvSizePx);

        constexpr float TEX_PIXEL_INV    = 1.0f / 1024.0f;
        constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;
        iconQuad.quad.setTexCoords(
            uvOriginPx[0] * TEX_PIXEL_INV,
            uvOriginPx[1] * TEX_PIXEL_INV,
            (uvOriginPx[0] + uvSizePx[0]) * TEX_PIXEL_INV,
            (uvOriginPx[1] + uvSizePx[1]) * TEX_PIXEL_INV);
        iconQuad.quad.setSize(uvSizePx[0] * SCREEN_PIXEL_INV,
                              uvSizePx[1] * SCREEN_PIXEL_INV);
        iconQuad.quad.posX = 0.117188f;
        iconQuad.quad.posY = 0.117188f;
        iconQuad.quad.snapToPixelGrid();
    }

    // mode5Bit = (contentType != 0): same gate the binary uses to toggle
    // the wider title-bar layout (populateForContent passes 1; this passes
    // 0 when no icon is shown to compact the bar over the missing tile).
    const uint32_t mode5Bit = (contentType != 0) ? 1u : 0u;
    layoutAndFade(headerY, anchor, /*inhibitOpt=*/0, mode5Bit,
                  name, desc0, desc1, desc2);
    update(0.0f);
}

// reconstructed from Ghidra FUN_10003f6f8. mode 3 (Perk preview).
// zeroes the iconQuad (DetailPanel::draw routes mode 3 through
// perkSource->drawAt instead), captures the Perk* so the renderer can
// pick it up, then hands off to layoutAndFade with the Perk's name +
// 3 description lines.
void DetailPanel::populateForPerk(float headerY, const float* anchor,
                                  Perk* perk) {
    mode = 3;
    perkSource = perk;

    // zero iconQuad, mode 3 doesn't draw it (Perk::drawAt does its own).
    iconQuad.quad.setTexCoords(0.0f, 0.0f, 0.0f, 0.0f);
    iconQuad.quad.setSize(0.0f, 0.0f);
    iconQuad.quad.posX = 0.117188f;
    iconQuad.quad.posY = 0.117188f;
    iconQuad.quad.snapToPixelGrid();

    if (perk) {
        layoutAndFade(headerY, anchor, /*inhibitOpt=*/0, /*mode5Bit=*/1,
                      perk->getName(),
                      perk->getDescriptionLine(0),
                      perk->getDescriptionLine(1),
                      perk->getDescriptionLine(2));
    }
    update(0.0f);
}

// reconstructed from Ghidra FUN_10003f7f8. mode 4 (Event card).
//
// title + 3 desc lines come from the Event (EVENT_TABLE accessors); then the
// outerFrame sub-icon UV/size is chosen by the Event kind (0..4) and anchored
// off frames[4] (top-right corner), which layoutAndFade positions just above.
// the UVs are atlas pixels / 1024; sizes / offsets are screen pixels / 640.
void DetailPanel::populateForEvent(float headerY, const float* anchor,
                                   EventSlot* slot) {
    mode = 4;

    layoutAndFade(headerY, anchor, /*inhibitOpt=*/1, /*mode5Bit=*/0,
                  slot->getName(),
                  slot->getDescriptionLine(0),
                  slot->getDescriptionLine(1),
                  slot->getDescriptionLine(2));

    // sub-icon position is frames[4] + a per-kind nudge (DAT_10005a3a4..a3bc).
    const float baseX = frames[4].quad.posX;
    const float baseY = frames[4].quad.posY;

    switch (slot->getEventTypeKey()) {
    case 0:
        outerFrame.quad.setTexCoords(487.0f / 1024.0f,  20.0f / 1024.0f,
                                     525.0f / 1024.0f,  67.0f / 1024.0f);
        outerFrame.quad.setSize(38.0f / 640.0f, 47.0f / 640.0f);
        outerFrame.quad.posX = baseX + (-5.0f / 640.0f);   // DAT_10005a3b8
        outerFrame.quad.posY = baseY + ( 6.0f / 640.0f);   // DAT_10005a3bc
        break;

    case 1:
        outerFrame.quad.setTexCoords(440.0f / 1024.0f,  20.0f / 1024.0f,
                                     486.0f / 1024.0f,  66.0f / 1024.0f);
        outerFrame.quad.setSize(46.0f / 640.0f, 46.0f / 640.0f);
        outerFrame.quad.posX = baseX + (-4.0f / 640.0f);   // DAT_10005a3b0
        outerFrame.quad.posY = baseY + ( 5.0f / 640.0f);   // DAT_10005a3b4
        break;

    case 2:
        outerFrame.quad.setTexCoords(186.0f / 1024.0f,   0.0f,
                                     234.0f / 1024.0f,  46.0f / 1024.0f);
        outerFrame.quad.setSize(48.0f / 640.0f, 46.0f / 640.0f);
        outerFrame.quad.posX = baseX + (-4.0f / 640.0f);   // DAT_10005a3b0
        outerFrame.quad.posY = baseY + ( 7.0f / 640.0f);   // DAT_10005a3ac
        break;

    case 3:
        outerFrame.quad.setTexCoords(287.0f / 1024.0f,   0.0f,
                                     331.0f / 1024.0f,  46.0f / 1024.0f);
        outerFrame.quad.setSize(44.0f / 640.0f, 46.0f / 640.0f);
        outerFrame.quad.posX = baseX + (-6.0f / 640.0f);   // DAT_10005a3a4
        outerFrame.quad.posY = baseY + ( 7.0f / 640.0f);   // DAT_10005a3ac
        break;

    case 4:
        outerFrame.quad.setTexCoords(235.0f / 1024.0f,   0.0f,
                                     285.0f / 1024.0f,  46.0f / 1024.0f);
        outerFrame.quad.setSize(50.0f / 640.0f, 46.0f / 640.0f);
        outerFrame.quad.posX = baseX + (-6.0f / 640.0f);   // DAT_10005a3a4
        outerFrame.quad.posY = baseY + ( 8.0f / 640.0f);   // DAT_10005a3a8
        break;

    default:
        // kind out of range: leave outerFrame as-is (binary skips the pos
        // write but still snaps + updates below).
        break;
    }

    outerFrame.quad.snapToPixelGrid();   // FUN_1000573a8
    update(0.0f);                          // FUN_10003f0e4
}

// reconstructed from Ghidra FUN_10003fe64. mode 0 (snag combat card).
//
// the most decorated card: snag icon (with a Doppelganger special case),
// outerFrame hex-tile preview with the tier digit on top, plus an optional
// combatSimPreview block in the top-right corner showing the simulated
// player-wins / snag-wins turn counts.
//
// order matches the binary exactly because the combatSimPreview position
// depends on frames[4] (top-right corner) which is only positioned by
// layoutAndFade's inner applyLayoutGeometry call. running layoutAndFade
// after the snag decorations would leave frames[4].posX uninitialized.
void DetailPanel::populateForSnag(float headerY, const float* anchor,
                                  SnagContent* snag, int playerWinTurns_,
                                  int snagWinTurns_, uint8_t extraFlag) {
    mode = 0;
    snagPreviewEnabled = (extraFlag != 0);

    int snagType = snag ? snag->type : 0;
    if (snagType < 0 || snagType >= 119) {
        snagType = 0;
    }
    const SnagInfo& s = SNAG_TABLE[snagType];

    // ---- 1) snag icon UV+size (port of FUN_10003dedc) ----
    // Doppelganger pulls UV/size off the live snag's own baseQuad; every
    // other kind reads SNAG_TABLE. delegate the whole thing to the snag,
    // writing through to our panel-local icon Quad / iconTexIndex.
    if (snag) {
        snag->refreshBaseSprite(&iconQuad.quad, &iconTexIndex);
    } else {
        // no live snag (e.g. preview path); apply the table sprite directly.
        iconTexIndex = 10;
        applySpriteUV(iconQuad.quad, s.spriteU, s.spriteV, s.spriteW, s.spriteH);

        if (snagType == (int)SnagKind::Change) {
            iconQuad.quad.addVertexOffset(-0.0046875f, -0.003125f);
        }
    }

    iconQuad.quad.posX = 0.117188f;
    iconQuad.quad.posY = 0.117188f;
    iconQuad.quad.snapToPixelGrid();

    // ---- 2) layout the panel frames now ----
    // applyLayoutGeometry inside layoutAndFade fills in frames[4].posX/posY,
    // which the combatSimPreview block below references for its anchor.
    //
    // Obsession (kind 0x06) is the only snag whose desc lines carry runtime
    // sprintf format placeholders (FUN_10003e154's special-case path). desc2
    // takes the snag's consumedFlag (seeded to 100 = 100% chance and
    // depleted on re-draws); desc3 takes obsessionCount (the turn
    // count). desc1 has no placeholder and is passed through untouched.
    // substitute here so the player doesn't see literal %d in the card.
    const char* desc2 = s.desc2;
    const char* desc3 = s.desc3;
    char obsessionBuf2[256];
    char obsessionBuf3[256];

    if (snagType == (int)SnagKind::Obsession && snag) {
        std::snprintf(obsessionBuf2, sizeof(obsessionBuf2), s.desc2,
                      (int)snag->consumedFlag);
        std::snprintf(obsessionBuf3, sizeof(obsessionBuf3), s.desc3,
                      (int)snag->obsessionCount);
        desc2 = obsessionBuf2;
        desc3 = obsessionBuf3;
    }
    layoutAndFade(headerY, anchor, /*inhibitOpt=*/0, /*mode5Bit=*/1,
                  s.name, s.desc1, desc2, desc3);

    // ---- 3) outerFrame: a small hex-tile sprite drawn below the snag icon ----
    outerFrame.quad.setTexCoords(0.475586f, 0.019531f, 0.512695f, 0.064453f);
    outerFrame.quad.setSize(0.059375f, 0.071875f);
    outerFrame.quad.posX = 0.029688f;
    outerFrame.quad.posY = 0.201562f;
    outerFrame.quad.snapToPixelGrid();

    // ---- 4) tintFrame: the tier digit drawn on top of outerFrame ----
    {
        static constexpr float kTierOffX[4] = {-0.003125f, -0.003125f, -0.001563f, -0.003125f};
        static constexpr float kTierOffY[4] = { 0.006250f,  0.004688f,  0.004688f,  0.006250f};
        int tier = s.tier;
        float offX = -0.003125f;          // DAT_10005a3c0 default
        float offY =  0.004688f;          // DAT_10005a3c4 default

        if ((unsigned)tier < 4) {
            offX = kTierOffX[tier];
            offY = kTierOffY[tier];
        }
        tintFrame.setNumber(tier, 0, 1);
        tintFrame.setPosition(outerFrame.quad.posX + offX,
                              outerFrame.quad.posY + offY, 1);
    }

    // ---- 5) combatSimPreview: top-right "vs" tile, anchored to frames[4] ----
    // (the top-right corner of the panel border). offsets put combatSimPreview
    // slightly left of and below the corner so the player-/snag-win digits on
    // either side fit inside the top edge of the panel.
    combatSimPreview.quad.posX = frames[4].quad.posX + -0.065625f;   // DAT_10005a3c8
    combatSimPreview.quad.posY = frames[4].quad.posY +  0.018750f;   // DAT_10005a3cc
    combatSimPreview.quad.snapToPixelGrid();

    // snagDeathTurns digits to the left of combatSimPreview.
    snagDeathTurns.setPosition(combatSimPreview.quad.posX + -0.045313f,  // DAT_10005a3d0
                             combatSimPreview.quad.posY, 1);

    // playerDeathTurns digits to the right.
    playerDeathTurns.setPosition(combatSimPreview.quad.posX + 0.040625f,  // DAT_10005a3d4
                                combatSimPreview.quad.posY, 1);

    // both side digits get their numbers set last (matches the binary's
    // setNumber-after-setPosition ordering for the win-turn pair).
    playerDeathTurns.setNumber(playerWinTurns_, 0, 1);
    snagDeathTurns  .setNumber(snagWinTurns_, 0, 1);

    update(0.0f);
}
