#pragma once

#include "quad.h"
#include "title_menu.h"  // for TileIcon
#include <cstddef>
#include <cstdint>

// reconstructed from Ghidra:
//   constructor:        FUN_100029a28
//   draw:               FUN_100029cdc
//   addCharge:          FUN_10002a268
//   contains:           FUN_10002a2dc
//   dimColor:           FUN_10002a068
//   resetColor:         FUN_10002a118
//   eventType getter:   FUN_10002a1e0   (= EVENT_TABLE[eventType].key)
//   name string:        FUN_10002a1fc   (= EVENT_TABLE[eventType].name)
//   bonus desc string:  FUN_10002a218   (= EVENT_TABLE[eventType].desc[idx])
//
// EventSlot is the game-logic + visual half of a charge-event ability. when
// the player completes the slot's tracked event (defeating a Snag, placing
// a tile of a specific kind, etc.) the slot's currentCharges increments;
// once currentCharges == chargesMax the event is ready and a tap activates
// it. taps before that play the "locked" sound (0x12).
//
// 5 event types (decoded from the surrounding wrapper init in FUN_10000e834):
//   0  Experience Event  - gains 1 charge per Snag defeated
//   1  Control Event     - gains 1 charge per {C} tile placed on a {C} spot
//   2  Attack Event      - gains 1 charge per {A} tile placed
//   3  Defence Event     - gains 1 charge per {D} tile placed
//   4  Health Event      - gains 1 charge per {H} gained from a {H} tile
//
// allocated via operator_new(0xDD0) by GameBoard::update during the event
// install path (game_board.cpp:3417 uses the legacy "item display" label;
// the ctor at FUN_100029a28 confirms it's actually an event install).
//
// EventSlots are owned by the typed EventChoicePanel (GameBoard.eventChoicePanel): its
// slots[].slotPtr hold the candidate events while the picker is open. once an
// event is installed, the HUD's typed eventTray[4] references it
// via eventTray[i].slotPtr. both holders carry the per-slot display TextItems
// for the event's name and bonus descriptions.
//
// note on the variable-length child arrays: storage is fixed-size 7
// entries each (confirmed by the ctor's init loops, stride 0xD8 / 0xE0).
// chargesMax (<= 7) controls how many
// are used for hit-test / draw / dim+reset color updates; the rest stay
// default-Quad.

struct EventSlot {
    // ---- public methods (port of the binary's EventSlot member functions) ----

    // FUN_100029a28, full ctor. inits all Quads, looks up event-type table
    // entries to set UVs/sizes for mainQuad + eventFrame + chargeSlots[i] +
    // chargeMarkers[i] for i in [0..chargesMax). param magnitudeBase is the
    // requested currentCharges; clamped to chargesMax.
    void init(int eventType, int magnitudeBase);

    // FUN_100029cdc, draw the slot. binds tex 12 (items1 atlas), pushes
    // matrix with hoverState-driven Y shift if hoverState > 0, draws
    // mainQuad + eventFrame + chargeSlots[0..chargesMax) (always all of
    // them, full set = chrome) + chargeMarkers[0..currentCharges) (only the
    // filled ones, so the bar fills up as charges accumulate).
    void draw();

    // FUN_10002a268, increment currentCharges by 1. seeds the newly-
    // added chargeMarker's animation state (alpha = 0, scaleX = scaleY = 0)
    // so the next-frame update tweens it into view. plays sound 0x2F
    // (charge fill). no-op when already at chargesMax.
    void addCharge();

    // FUN_100029dc8, per-frame internal animation tick. updates
    // hoverState (lerps 0..1 based on the `awaitingTileSelection` flag) and
    // animates each filled chargeMarker's alpha (0->1 over 0.3s) +
    // cosine-eased scale (0->slightly past 1 overshoot, then settles).
    // when the last marker finishes its animation, plays sound 0x30
    // ("event fully charged" chime). called every frame from
    // GameplayHUD::update.
    void tickAnimation(float dt);

    // FUN_100029f20, place mainQuad + eventFrame + chargeSlots[i] +
    // chargeMarkers[i] relative to a center xy. used by HUD::addEventSlot
    // (initial install) and by the events-bar update path (per-frame
    // animation tween between tray positions).
    //   mainQuad   centered at (xy[0], xy[1])
    //   eventFrame centered at (xy[0], xy[1] - 0.01172)   (1/85 below)
    //   chargeSlots/chargeMarkers laid out as a row Y = xy[1] + 0.053125,
    //     each X spread by (i - (chargesMax-1)/2) * 15/640 from a center
    //     biased 0.5px when chargesMax is odd.
    void setPosition(const float* xy);

    // FUN_100029fdc, propagate alpha onto mainQuad, eventFrame, and the
    // active sub-Quads (chargeSlots[i].quad + chargeMarkers[i].quad for
    // i in [0, chargesMax)). distinct from dimColor / resetColor which
    // set full RGBA; setAlpha only touches the alpha byte (mirroring the
    // setColor / setAlpha split that Quad / TextItem / ColorTint use).
    void setAlpha(uint8_t alpha);

    // FUN_10002a2dc, bbox hit-test. when hoverState > 0 (slot is being
    // held), shifts touch Y by `hoverState * -0.015625f` (DAT_10005a074)
    // before testing mainQuad.contains. NaN-safe (NaN fails the gt check).
    bool contains(float touchX, float touchY) const;

    // FUN_10002a068, write 0xFFB4B4B4 (grey) to mainQuad + eventFrame,
    // then iterate chargeSlots[0..chargesMax) and chargeMarkers[0..chargesMax).
    void dimColor();

    // FUN_10002a118, mirror of dimColor with 0xFFFFFFFF (white).
    void resetColor();

    // FUN_10002a1e0, returns EVENT_TABLE[eventType].key (charge category).
    // used by HUD::pushEventCharge to find which held slot to charge in
    // response to a gameplay action.
    uint32_t getEventTypeKey() const;

    // FUN_10002a1fc, display name for this Event. lookup into EVENT_TABLE.
    const char* getName() const;

    // FUN_10002a218, bonus desc string. lineIdx 0 / 1 valid; >=2 returns "".
    const char* getDescriptionLine(int lineIdx) const;

    // ---- byte-exact field layout ----

    int     eventType;             // set by ctor from param_2 (0..4)
    int     chargesMax;            // capacity, looked up from
                                   // EVENT_TABLE[eventType].chargesMax. <= 7.
    int     currentCharges;        // filled-charge count.
                                   // locked iff currentCharges < chargesMax.

    TileIcon mainQuad;             // hit-test target + main category icon
                                   // (per-EventKey: Experience / Health /
                                   //  Attack / Defence / Control).
    TileIcon eventFrame;           // per-Event colored border that wraps
                                   // around the charge bar; matches the
                                   // unique Event's color/identity.

    TileIcon chargeSlots[7];       // empty-slot chrome for the charge bar.
                                   // fixed-size; only the first chargesMax
                                   // entries are used for visuals + dim/reset.

    // awaitingTileSelection = 1 means the player tapped this Event and its
    // effect requires a tile-selection step (FlashOfInsight discard,
    // Metamorphosis special-snag pick, SoothingMelody blank, etc.); the
    // discard-staging panel is now open in this Event's name and the
    // Event sits "lifted" visually until the player commits or cancels
    // the panel. drives hoverState's lerp toward 1.0. set by
    // GameBoard::fireEvent's tile-selection branch (binary FUN_10002a2c8).
    // cleared by the dispatcher that consumes the event on confirm /
    // by the panel cancel path. while non-zero, tickAnimation lerps
    // hoverState += dt / 0.1s (clamped 0..1); when zero, lerps back down.
    uint8_t  awaitingTileSelection;   // (ctor writes 0)

    float    hoverState;           // (ctor writes 0.0f). when > 0,
                                   // contains() shifts touch Y by
                                   // hoverState * -0.015625f.

    // ChargeMarker, the filled-slot indicator that's drawn on top of a
    // chargeSlot once that slot is filled. shared author idiom with
    // MarkerSlot (gameplay_hud.h) and SpecialAbility (item.h): a Quad
    // followed by 8 bytes of trailing (alpha float + 4-byte pad). the
    // Quad itself owns its 0x10-byte animation-target rect; alpha sits
    // immediately after the Quad. addCharge() resets scaleX/Y +
    // alpha to 0 on the newly-filled slot so the update path tweens
    // them into view.
    struct ChargeMarker {
        Quad    quad;              // (0xD8 bytes)
        float   alpha;             // (init 1.0f)
    };

    ChargeMarker chargeMarkers[7]; // fixed-size, same usage rule as
                                   // chargeSlots (first chargesMax used);
                                   // only [0, currentCharges) are drawn.
};
