#pragma once

#include "quad.h"
#include "text_item.h"
#include "title_menu.h"   // for TileIcon
#include <cstddef>
#include <cstdint>

// reconstructed from Ghidra:
//   constructor: FUN_1000404e8
//   draw:        FUN_100040e9c
//
// DialogPanel lives at GameBoard.dialogPanel (0xF60 bytes). it's the tutorial
// ambient-hint popup: a speech bubble (9-slice frame + a directional tail) over
// a darkened backdrop, with 3 lines of text, that points at a gameplay element.
//
// the tail is chosen from two flags set in showHint from the anchor position:
//   configA = anchor in the top half of the screen  -> box sits below it, the
//             tail points up    (tailUp,  on the top edge)
//   !configA = anchor in the bottom half             -> box sits above it, tail
//             points down       (tailDown, on the bottom edge)
//   configB = anchor in the right half               -> the right-mirrored tail
//             sprite ([1]); else the left one ([0]). this is what keeps the tail
//             centered on elements near a screen edge.
//
// the dark backdrop overlay is drawn first on tex 0 (no texture, solid color),
// then the frame + tail + text on tex 9. fadeProgress drives the show/hide
// animation; the panel is drawn even after `visible` clears, until the fade
// completes.

class DialogPanel {
public:
    // FUN_1000404e8
    void init();
    // FUN_100040e9c
    void draw();
    // FUN_100040fac
    void update(float dt);

    // FUN_10004128c, reset / dismiss. mirrors FUN_1000404b0 for DetailPanel.
    // early-out if already invisible; otherwise clear visible +
    // interpolatePosition, set fadeProgress = 0 or 1 depending on hardReset,
    // run one update tick at dt=0.
    void reset(int hardReset);

    // FUN_1000412e4, populate + show a tutorial hint popup. pos = {x, y} is
    // the screen-space anchor (the element the hint points at); vGap is the
    // vertical gap between the anchor and the popup (the cascade's per-hint
    // "fade-rate" constant is actually this gap). hintId indexes
    // TUTORIAL_HINT_TABLE. mirrors DetailPanel::layoutAndFade.
    void showHint(float vGap, const float pos[2], int hintId);

    // FUN_1000415b0, size + position the 9-slice frame pieces + the active tail
    // for a popup of width w / body height bodyH, splitting the tail's edge
    // around the pointer at tailX. configA picks the top vs bottom tail. called
    // by showHint. (public only so it sits with showHint; internal helper.)
    void applyHintFrameLayout(float w, float bodyH, float tailX);

    // FUN_100041188, set the panel's color: (r,g,b,a) onto all 15 frame /
    // button quads, alpha-only onto the 3 text lines, and half-alpha onto the
    // backdrop. called by update() each fade frame and on the dismiss flash.
    void applyColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // --- byte-exact struct fields ---

    bool    visible;
    float   posX;
    float   posY;
    bool    configA;               // selects A* vs B* button pair
    bool    configB;               // selects *A vs *B within the pair

    // --- speech-bubble frame: a 9-slice border + a directional tail/pointer ---
    // every piece is a TileIcon (0xD8). init() sets each one's atlas UV (corners
    // fixed-size, edges stretch); applyHintFrameLayout sizes + positions them per
    // popup. the tail points at the anchored element: configA picks the vertical
    // side (tailUp when the box sits below the anchor, tailDown when above), and
    // configB picks the horizontal mirror ([0] = anchor in the left half of the
    // screen, [1] = right half). the edge the tail sits on splits into a piece
    // left of the tail (topEdge / bottomEdge) and one right of it (*AfterTail).

    TileIcon cornerTL;             // top-left corner
    TileIcon topEdge;              // top edge (left of the tail when split)

    // upward-pointing tail, drawn when configA (box sits below the anchor).
    TileIcon tailUp[2];            // [0] left-mirror, [1] right-mirror
    TileIcon topEdgeAfterTail;     // top edge segment right of the tail

    TileIcon cornerTR;             // top-right corner
    TileIcon leftEdge;             // left edge
    TileIcon centerFill;           // center fill
    TileIcon rightEdge;            // right edge
    TileIcon cornerBL;             // bottom-left corner
    TileIcon bottomEdge;           // bottom edge (left of the tail when split)

    // downward-pointing tail, drawn when !configA (box sits above the anchor).
    TileIcon tailDown[2];          // [0] left-mirror, [1] right-mirror
    TileIcon bottomEdgeAfterTail;  // bottom edge segment right of the tail

    TileIcon cornerBR;             // bottom-right corner

    // 3 text items (title + 2 description lines)
    TextItem textLines[3];         // (3 * 0x88 = 0x198)

    // fullscreen darkened backdrop, drawn first on tex 0 (no texture).
    // color 0xFF000000 (opaque black).
    TileIcon backdropQuad;

    float   fadeProgress;          // init = 1.0; <1.0 = animating

    // FUN_100040fac (update) lerps posX/posY between (lerpStartX, lerpStartY)
    // and (lerpTargetX, lerpTargetY) by fadeProgress when interpolatePosition
    // is set. otherwise just alpha animates.
    float   lerpStartX;
    float   lerpStartY;
    float   lerpTargetX;
    float   lerpTargetY;
    bool    interpolatePosition;   // gates the start/target lerp.
                                   // cleared by reset() (FUN_10004128c).
    bool    dismissReady;          // set when touch lands on dismiss-
                                   // button bounds; ready for a confirm tap.
    bool    dismissTriggered;      // set on confirm tap; gates the
                                   // panel's actual close on next update.

    // tutorial ambient-hint "already-shown" markers. these physically live in
    // DialogPanel's trailing storage but are a GameBoard concern: the cascade
    // GameBoard::tickAmbientPickupHinting (FUN_10001980c) reaches them through
    // the GameBoard base. each byte, once its hint has been shown, stays 1 so
    // the hint never re-fires this game.
    uint8_t hintShown[24];
    uint8_t anyHintFiredThisFrame;
};
