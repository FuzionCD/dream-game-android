#include "event_slot.h"
#include "event_table.h"
#include "game.h"
#include "renderer.h"   // bindTexture
#include <GLES/gl.h>
#include <cmath>

// helper: setPixelRect.
// FUN_100014d84, same path used by Item::postInitVisuals. atlas pixel
// coords on 1024x1024 atlas -> UV; pixel display size on 640 reference
// width. duplicated locally to keep event_slot.cpp self-contained
// (matching how item.cpp keeps its own copy).
static void setPixelRect(Quad& q, float atlasX, float atlasY,
                         float atlasW, float atlasH, float displayScale) {
    constexpr float ATLAS_TO_UV     = 1.0f / 1024.0f;
    constexpr float PIXEL_TO_SCREEN = 1.0f / 640.0f;

    q.setTexCoords(
        atlasX            * ATLAS_TO_UV, atlasY            * ATLAS_TO_UV,
        (atlasX + atlasW) * ATLAS_TO_UV, (atlasY + atlasH) * ATLAS_TO_UV);
    q.setSize(atlasW * displayScale * PIXEL_TO_SCREEN,
              atlasH * displayScale * PIXEL_TO_SCREEN);
}

// pack RGBA into the 32-bit form Quad::setColor expects, then write.
// matches FUN_10000826c's behavior in the binary (4 bytes copied into
// every vertex.color).
static void quadSetColorBytes(Quad& q, uint32_t packedRGBA) {
    uint8_t r = (uint8_t)(packedRGBA >>  0);
    uint8_t g = (uint8_t)(packedRGBA >>  8);
    uint8_t b = (uint8_t)(packedRGBA >> 16);
    uint8_t a = (uint8_t)(packedRGBA >> 24);
    q.setColor(r, g, b, a);
}

// DAT constants used by init / draw.
// mainQuad and eventFrame have fixed atlas-pixel sizes shared across all
// event-keys; only the atlas X/Y change per key / per eventType.
namespace {
constexpr float MAIN_QUAD_ATLAS_W = 131.0f;   // DAT_10005a03c
constexpr float MAIN_QUAD_ATLAS_H = 102.0f;   // DAT_10005a040
constexpr float EVENT_FRAME_W     =  97.0f;   // DAT_10005a044
constexpr float EVENT_FRAME_H     =  57.0f;   // DAT_10005a048
constexpr float HOVER_Y_SHIFT     =  0.015625f;  // DAT_10005a04c = 1/64
constexpr float CONTAINS_Y_SCALE  = -0.015625f;  // DAT_10005a074 = -1/64

// chargeSlot UV: universal 20x20 backdrop sprite at atlas (0, 929) on
// icons1.png. always drawn for every chargesMax position; the "empty
// container" the player sees showing event capacity.
constexpr float SLOT_U0           =   0.0f / 1024.0f;
constexpr float SLOT_V0           = 929.0f / 1024.0f;
constexpr float SLOT_U1           =  20.0f / 1024.0f;
constexpr float SLOT_V1           = 949.0f / 1024.0f;
constexpr float SLOT_SIZE         =  20.0f /  640.0f;  // 0.03125

// chargeMarker size: per-key 12x12 colored-fill sprite atlas coords come
// from EventKeyIconUV.markerAtlasX/Y. drawn only for currentCharges
// positions; overlays on top of the chargeSlot backdrop when that
// position fills.
constexpr float MARKER_W          =  12.0f;
constexpr float MARKER_H          =  12.0f;

}   // namespace

// FUN_100029a28, EventSlot::init.
void EventSlot::init(int evType, int magnitudeBase) {

    // (1) header: eventType, then default-construct all owned quads.
    eventType = evType;
    mainQuad   = TileIcon();
    eventFrame = TileIcon();

    for (int i = 0; i < 7; ++i) {
        chargeSlots[i] = TileIcon();
    }

    awaitingTileSelection = 0;
    hoverState            = 0.0f;

    for (int i = 0; i < 7; ++i) {
        chargeMarkers[i].quad = Quad();
    }

    // (2) per-eventType + per-key lookups.
    const EventInfo&     info = EVENT_TABLE[evType];
    const EventKey       key  = info.key;
    const EventKeyIconUV& uv  = EVENT_KEY_ICON_UV[(uint32_t)key];

    // (3) mainQuad (key-indexed atlas)
    setPixelRect(mainQuad.quad,
                 (float)uv.mainAtlasX, (float)uv.mainAtlasY,
                 MAIN_QUAD_ATLAS_W, MAIN_QUAD_ATLAS_H, 1.0f);

    // (4) eventFrame (eventType-indexed atlas; per-Event)
    setPixelRect(eventFrame.quad,
                 (float)info.eventFrameAtlasX, (float)info.eventFrameAtlasY,
                 EVENT_FRAME_W, EVENT_FRAME_H, 1.0f);

    // (5) chargesMax + currentCharges (clamped down to chargesMax)
    chargesMax     = (int)info.chargesMax;
    currentCharges = (magnitudeBase > chargesMax) ? chargesMax : magnitudeBase;

    // (6) per-active-position setup. only [0, chargesMax) get configured.
    //   - chargeSlots[i]: the 20x20 backdrop sprite (always drawn); uses
    //                     a single universal UV at atlas (0, 929).
    //   - chargeMarkers[i]: the 12x12 per-key colored-fill sprite (only
    //                       drawn when filled). UV comes from the per-key
    //                       icon table.
    //   - chargeMarker alpha = 1.0 by default; addCharge resets the
    //     newly-filled marker to alpha=0 / scale=0 to start its fade-in.
    for (int i = 0; i < chargesMax; ++i) {
        Quad& slotQuad = chargeSlots[i].quad;
        slotQuad.setTexCoords(SLOT_U0, SLOT_V0, SLOT_U1, SLOT_V1);
        slotQuad.setSize(SLOT_SIZE, SLOT_SIZE);

        setPixelRect(chargeMarkers[i].quad,
                     (float)uv.markerAtlasX, (float)uv.markerAtlasY,
                     MARKER_W, MARKER_H, 1.0f);
        chargeMarkers[i].alpha = 1.0f;
    }
}

// FUN_100029cdc, EventSlot::draw.
void EventSlot::draw() {
    bindTexture(12);    // items1 atlas

    // hoverState > 0 -> translate up by hoverState * HOVER_Y_SHIFT.
    // NaN-safe: a plain > 0 already fails for NaN, so no extra check needed.
    const bool hovered = (hoverState > 0.0f);

    if (hovered) {
        glPushMatrix();
        glTranslatef(0.0f, hoverState * HOVER_Y_SHIFT, 0.0f);
    }

    mainQuad.quad.draw();
    eventFrame.quad.draw();

    // chargeSlots[0..chargesMax): full chrome of the bar (empty + filled
    // slots both rendered). binary iterates via vtable[2] = TileIcon::draw.
    for (int i = 0; i < chargesMax; ++i) {
        chargeSlots[i].quad.draw();
    }

    // chargeMarkers[0..currentCharges): only the filled ones. these stack
    // on top of the chargeSlots to indicate which slots are charged.
    // note: binary calls Quad::draw directly here, not vtable dispatch.
    for (int i = 0; i < currentCharges; ++i) {
        chargeMarkers[i].quad.draw();
    }

    if (hovered) {
        glPopMatrix();
    }
}

// FUN_100029dc8, EventSlot::tickAnimation.
// per-frame internal animation tick. drives two independent animations:
//
// 1. hoverState (slot+0x7AC): lerps 0..1 based on awaitingTileSelection.
//    when set (panel open in this Event's name): += dt / +0.1s.
//    when clear (panel closed / no selection):   += dt / -0.1s (decay).
//    used by draw() to bob the whole slot upward by hoverState * 1/64.
//
// 2. per-filled-chargeMarker fill animation: each marker's `alpha` field
//    serves as the animation timer (init = 1.0 = "done"; addCharge resets
//    a new marker to 0.0 = "start"). every frame, alpha advances toward
//    1.0 over 0.3s; scaleX/scaleY are recomputed each frame from alpha
//    using a cosine ease with slight overshoot:
//      scale = (1 - cos(alpha * pi * 1.3)) / 1.588
//    when the last marker finishes (alpha just hit 1.0 and i ==
//    chargesMax - 1), play sound 0x30, the "event fully charged" chime.
void EventSlot::tickAnimation(float dt) {
    constexpr float HOVER_IN_DURATION    =  0.1f;     // DAT_10005a050
    constexpr float HOVER_OUT_DURATION   = -0.1f;     // DAT_10005a054 (negative)
    constexpr float MARKER_FILL_DURATION =  0.3f;     // DAT_10005a058
    constexpr float MARKER_SCALE_MUL_A   =  3.14159274f;  // DAT_10005a05c (= pi)
    constexpr float MARKER_SCALE_MUL_B   =  1.3f;     // DAT_10005a060
    constexpr float MARKER_SCALE_DIVISOR =  1.58778548f;  // DAT_10005a064

    // ---- 1. hoverState lerp ----
    if (awaitingTileSelection != 0) {
        // hovered: advance toward 1.0
        hoverState += dt / HOVER_IN_DURATION;

        if (hoverState > 1.0f) {
            hoverState = 1.0f;
        }

    } else if (hoverState > 0.0f) {
        // released and still > 0: advance toward 0 (the / negative duration
        // makes the step subtract).
        hoverState += dt / HOVER_OUT_DURATION;

        if (hoverState < 0.0f) {
            hoverState = 0.0f;
        }
    }

    // ---- 2. per-filled-marker fill animation ----
    const float alphaStep = dt / MARKER_FILL_DURATION;

    for (int i = 0; i < currentCharges; ++i) {
        ChargeMarker& cm = chargeMarkers[i];

        if (cm.alpha < 1.0f) {
            cm.alpha += alphaStep;

            if (cm.alpha < 0.0f) {
                cm.alpha = 0.0f;
            } else if (cm.alpha > 1.0f) {
                cm.alpha = 1.0f;
            }

            float scale = (1.0f - std::cos(cm.alpha
                                          * MARKER_SCALE_MUL_A
                                          * MARKER_SCALE_MUL_B))
                        / MARKER_SCALE_DIVISOR;
            cm.quad.scaleX = scale;
            cm.quad.scaleY = scale;

            // play "fully charged" chime when the last marker just
            // finished. matches the binary's `if (alpha >= 1 && i ==
            // chargesMax - 1)` gate.
            if (cm.alpha >= 1.0f && i == chargesMax - 1) {
                Game* g = getGame();

                if (g) {
                    g->soundQueue.trigger(0x30);
                }
            }
        }
    }
}

// FUN_10002a268, EventSlot::addCharge.
// no-op when already at max. seeds the new chargeMarker's animation state
// (scaleX/scaleY = 0 -> grows to 1; alpha = 0 -> fades to 1) so the
// next-frame update path (EventSlot::tickAnimation) tweens it into view.
// plays sound 0x2F (charge-fill chime).
void EventSlot::addCharge() {

    if (currentCharges >= chargesMax) {
        return;
    }

    ChargeMarker& cm = chargeMarkers[currentCharges];
    cm.quad.scaleX = 0.0f;
    cm.quad.scaleY = 0.0f;
    cm.alpha       = 0.0f;
    currentCharges += 1;

    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x2F);
    }
}

// FUN_100029f20, EventSlot::setPosition.
// places every sub-Quad relative to a single center (xy[0], xy[1]).
// constants pulled from the binary:
//   DAT_10005a068 = -7.5/640   (eventFrame Y offset from center)
//   DAT_10005a06c = 640        (X spread divisor, screen reference px)
//   DAT_10005a070 =  34/640    (chargeSlots / chargeMarkers Y offset)
//   DAT_10005a078[chargesMax & 1] = (0, 0.5/640)  (X bias: 0 if even, 0.5px if odd)
//
// the chargeSlot/chargeMarker row is laid out symmetrically around the
// center X: each i in [0, chargesMax) gets X offset
//   ((i - (chargesMax-1)/2) * 15 / 640) - bias
// a 15px-spaced row, center-anchored. odd counts shift by 0.5px to keep
// the middle slot on a whole pixel.
void EventSlot::setPosition(const float* xy) {
    constexpr float EVENT_FRAME_Y_OFFSET = -7.5f / 640.0f;   // DAT_10005a068
    constexpr float X_SPREAD_DIVISOR     = 640.0f;            // DAT_10005a06c
    constexpr float CHARGE_ROW_Y_OFFSET  =  34.0f / 640.0f;   // DAT_10005a070
    constexpr float X_BIAS_EVEN          = 0.0f;              // DAT_10005a078
    constexpr float X_BIAS_ODD           = 0.5f / 640.0f;     // DAT_10005a07c
    constexpr float CHARGE_SPACING_PX    = 15.0f;

    mainQuad.quad.posX   = xy[0];
    mainQuad.quad.posY   = xy[1];
    eventFrame.quad.posX = xy[0];
    eventFrame.quad.posY = xy[1] + EVENT_FRAME_Y_OFFSET;

    const float xBias  = (chargesMax & 1) ? X_BIAS_ODD : X_BIAS_EVEN;
    const float yRow   = xy[1] + CHARGE_ROW_Y_OFFSET;
    const float halfN  = (float)(chargesMax - 1) * -0.5f;

    for (int i = 0; i < chargesMax; ++i) {
        const float xOff = (((float)i + halfN) * CHARGE_SPACING_PX) / X_SPREAD_DIVISOR
                         - xBias;
        const float x    = xy[0] + xOff;
        chargeSlots[i].quad.posX   = x;
        chargeSlots[i].quad.posY   = yRow;
        chargeMarkers[i].quad.posX = x;
        chargeMarkers[i].quad.posY = yRow;
    }
}

// FUN_100029fdc, EventSlot::setAlpha.
// alpha-only propagation onto mainQuad + eventFrame + active chargeSlots
// + active chargeMarkers. distinct from dimColor / resetColor which set
// the full RGBA. mirrors the setColor / setAlpha split used by Quad,
// TextItem, and ColorTint.
void EventSlot::setAlpha(uint8_t alpha) {
    mainQuad.quad.setAlpha(alpha);
    eventFrame.quad.setAlpha(alpha);

    for (int i = 0; i < chargesMax; ++i) {
        chargeSlots[i].quad.setAlpha(alpha);
        chargeMarkers[i].quad.setAlpha(alpha);
    }
}

// FUN_10002a2dc, EventSlot::contains.
bool EventSlot::contains(float touchX, float touchY) const {

    if (hoverState > 0.0f) {
        return mainQuad.quad.contains(
            touchX,
            touchY + hoverState * CONTAINS_Y_SCALE);
    }

    return mainQuad.quad.contains(touchX, touchY);
}

// FUN_10002a068, EventSlot::dimColor.
// writes grey to every active sub-quad. loop bound is chargesMax, not
// currentCharges: the bar chrome is dimmed wholesale on hover.
void EventSlot::dimColor() {
    constexpr uint32_t DIM_RGBA = 0xFFB4B4B4u;

    quadSetColorBytes(mainQuad.quad,   DIM_RGBA);
    quadSetColorBytes(eventFrame.quad, DIM_RGBA);

    for (int i = 0; i < chargesMax; ++i) {
        quadSetColorBytes(chargeSlots[i].quad, DIM_RGBA);
        quadSetColorBytes(chargeMarkers[i].quad, DIM_RGBA);
    }
}

// FUN_10002a118, EventSlot::resetColor.
void EventSlot::resetColor() {
    constexpr uint32_t WHITE_RGBA = 0xFFFFFFFFu;

    quadSetColorBytes(mainQuad.quad,   WHITE_RGBA);
    quadSetColorBytes(eventFrame.quad, WHITE_RGBA);

    for (int i = 0; i < chargesMax; ++i) {
        quadSetColorBytes(chargeSlots[i].quad, WHITE_RGBA);
        quadSetColorBytes(chargeMarkers[i].quad, WHITE_RGBA);
    }
}

// FUN_10002a1e0 / 1fc / 218, EVENT_TABLE accessors.
uint32_t EventSlot::getEventTypeKey() const {
    return (uint32_t)EVENT_TABLE[eventType].key;
}

const char* EventSlot::getName() const {
    return EVENT_TABLE[eventType].name;
}

const char* EventSlot::getDescriptionLine(int lineIdx) const {

    if (lineIdx < 0 || lineIdx > 1) {
        return "";
    }

    return EVENT_TABLE[eventType].desc[lineIdx];
}
