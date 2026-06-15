#pragma once

#include "color_tint.h"
#include "event_slot.h"
#include "quad.h"
#include "title_menu.h"  // for TileIcon
#include <cstddef>       // offsetof, sizing assertions
#include <cstdint>
#include <list>
#include <vector>

// reconstructed from Ghidra:
//   constructor:    FUN_10000b160
//   draw:           FUN_10000c004
//   update:         FUN_10000c18c
//   level config:   FUN_10000d0dc, FUN_10000d134, FUN_10000d18c,
//                   FUN_10000d228, FUN_10000d9ac, FUN_10000d778
//   helpers:        FUN_10000bab8 (conditional icon setup),
//                   FUN_10000bb60 (stat bar layout)
//
// GameplayHUD lives at GameBoard+0x6418 (0x1E58 bytes).
//
// the parent pointer at +0x000 isn't GameBoard directly; it's the DetailPanel
// at GameBoard+0x4408. various helpers traverse from HUD->parent to read
// DetailPanel state.
//
// renders:
//   - status bar quad across the top
//   - 4 stat-bar quads (configured by FUN_10000bb60 from values at +0x968/+0x96c)
//   - 6 button-frame / icon quads (button frames, large frame, player + menu icons,
//     conditional icon)
//   - 10 main markers (small dots) and 10 indicator markers (small dots)
//   - 3 ColorTints layered on top
//   - 2 overlay quads drawn on texture 8 when overlay progress > 0
//   - a linked list of dynamic items at +0x1C58
//
// each "marker slot" is 0xE0 bytes (Quad 0xD8 + 8 bytes trailing fade state).

struct MarkerSlot {
    Quad    quad;             // +0x000..+0x0D7 (0xD8; owns anim rect at +0xC8)
    float   fadeT;            // +0x0D8 (0..1 fade-in progress; FUN_10000c568)
    float   fadeDelay;        // +0x0DC (countdown-to-start; ramp seeded by FUN_10000cf34)
};
static_assert(sizeof(MarkerSlot) == 0xE0, "MarkerSlot must be 0xE0 bytes");

// discard-staging entry, one per rack tile the player has staged for
// discard. carries the floating quad that bobs above the rack column while
// staged, plus a back-pointer to the rack slot and a flag gating whether
// the next commit pass will discard it.
struct DiscardEntry {
    Quad    quad;             // +0x000..+0x0D7 (floating visual above rack column; 0xD8 bytes)
    int32_t rackSlot;         // +0x0D8 (rack column this entry stages)
    uint8_t staged;           // +0x0DC (0 = idle / skipped this batch, 1 = discard on commit)
    uint8_t pad0DD[3];        // +0x0DD..+0x0DF
};
static_assert(sizeof(DiscardEntry) == 0xE0, "DiscardEntry must be 0xE0 bytes");

class GameplayHUD {
public:
    // FUN_10000b160: full constructor. takes a parent pointer (the DetailPanel
    // at GameBoard+0x4408 in the binary), sets up all 12 standalone quads,
    // 3 color tints, 10+10 marker arrays, the linked-list sentinel, and the
    // 2 overlay quads. matches the binary line-for-line.
    void init(void* parent);

    // FUN_10000c004: draw all elements in the binary's specific order.
    void draw();

    // FUN_10000c18c: per-frame update. driven by GameBoard::update.
    void update(float dt);

    // FUN_10000ca90, press-and-hold inspect of the top-row stat icons. when
    // the touch lands inside stat region `index` (0..4 = Control / Attack /
    // Health / Defence / XP), populate the DetailPanel with that stat's card
    // and return true; otherwise return false.
    bool tryInspectStatRegion(int index);

    // public setters called by GameBoard::initLevel and event handlers.
    void setAttack(int value);                       // FUN_10000d0dc
    void setDefence(int value);                      // FUN_10000d134
    void setHealth(int value, int holdRatio);        // FUN_10000d18c
    void setMaxHealth(int value);                    // FUN_10000d228
    void resetOverlay(int hardClear);                // FUN_10000d9ac

    // FUN_10000da70, rebuild HUD state from a saved-run snapshot (called by
    // GameBoard::restoreFromSnapshot). resets touch / overlay state, seeds
    // the ATK/DEF tints + health bar from the restored player stats, clears
    // the event tray, zeroes both marker banks, re-seeds the XP / CONTROL
    // banks from the saved running totals, and rebuilds the event tray.
    // takes plain ints (not PlayerSystem / the save struct) so the HUD stays
    // decoupled from both; the caller unpacks the snapshot. eventTray entries
    // are packed (eventType | currentCharges << 32).
    void restoreFromSnapshot(int playerAttack, int playerDefence,
                             int playerMaxHealth, int playerCurrentHealth,
                             int xpTotal, int controlTotal,
                             const std::vector<int64_t>& eventTraySnapshot);

    // FUN_10000c794, death-heart pulse animator. invoked from update().
    // gated on (overlayState != 0 || overlayProgress > 0); advances both
    // overlayProgress (0..1 fade ramp) and pulseTimer (cyclic heartbeat
    // phase). modulates overlayQuad1 (slow swell) + overlayQuad2 (shockwave
    // burst) and fires sound 0x34 once per beat.
    void tickDeathHeart(float dt);

    // configure the conditionalIcon (HUD+0x880) for one of three states.
    // anchors to largeButtonFrame's posX/Y plus a small Y nudge
    // (HUD_COND_Y_NUDGE = -0.003125f) in all states. ports of:
    //   FUN_10000bab8 (Default)        discard button idle.
    //   FUN_10000d300 (StagingActive)  discard mode active, no tiles selected.
    //   FUN_10000d250 (ConfirmDiscard) at least one tile staged for commit.
    enum class ConditionalIconState {
        Default,
        StagingActive,
        ConfirmDiscard,
    };
    void setConditionalIcon(ConditionalIconState state);
    void clearEventSlots();                        // FUN_10000d778

    // FUN_10000d010 / FUN_10000d034, advance the xp / control marker
    // bank by `delta` lit slots. silentFill clears playSoundNextFill so
    // the next batch of slot animations runs without chime sounds;
    // gameplay-side callers pass false. queues onto xpQueuedDelta /
    // controlQueuedDelta when an advance is already in flight.
    void advanceXPSlot(int delta, bool silentFill);
    void advanceCTRLSlot(int delta, bool silentFill);

    // FUN_10000d05c, drain the xp marker bank by `delta` slots. clamps
    // delta to (xpReceivedTotal % 10) so only the in-progress cycle's
    // earned slots can be drained (a "fresh" cycle with recv % 10 == 0
    // is a no-op). silentDrain mirrors advanceXPSlot's silentFill flag.
    // used by snag 0x73 (Mockery) in the rack walk.
    void drainXPSlot(int delta, bool silentDrain);

    // FUN_10000d3a0, visual ATK/DEF swap. updates tintAttack to show
    // newAtkValue at the resting ATK position, tintDefence to show
    // newDefValue at the resting DEF position, then swaps their current
    // anchor positions and resets fieldA18 = 0 to kick the slide
    // animation in update(). called by snag 5 (Mania) one-shot in the
    // rack walk after the player ATK/DEF stats have already been swapped.
    void swapAtkDefDisplays(int newAtkValue, int newDefValue);

    // FUN_10000d474, install a freshly-constructed EventSlot into the
    // first empty eventTray slot. caller (GameBoard install path or snag
    // 0x76 Spirit Drain) allocates via operator_new(0xDD0) + EventSlot::
    // init before calling. if all 4 tray slots are full, addEventSlot
    // deletes the passed slot to avoid leaking. updates the tray entry's
    // anim state so the new slot animates from its initial position into
    // the tray slot.
    void addEventSlot(EventSlot* slot);

    // FUN_10000d610, remove an EventSlot from the tray. queues an
    // animated removal node into removalAnims (slides slot up and off
    // the bar before the consumer frees it), then compacts the remaining
    // tray slots leftward into the gap. param `compact`:
    //   true  = full path: queue anim + shift later slots left
    //   false = wipe only: queue anim, leave gap (used by HUD reset)
    // no-op when `slot` isn't in the tray.
    void removeEventSlot(EventSlot* slot, bool compact);

    // FUN_10000d7ac, count non-null eventTray slots. used to gate snag
    // 0x76 (Spirit Drain)'s "first vs nth event" install path and to
    // feed the SpoiltForChoice / NeverADullMoment achievement check after
    // an EventChoicePanel commit.
    int countEventsHeld() const;

    // FUN_10000dc84, true while any non-empty event tray slot is still
    // sliding into place (progress < 1.0). the ambient-hint cascade waits
    // for this to clear before pointing hint 0x14 at the event tray.
    bool anyEventSlotSlidingIn() const;

    // FUN_10000dcc8, the mainQuad position (posX/posY at EventSlot+0xb8/
    // +0xbc) of the first non-empty event tray slot, or (0, 0) if empty.
    // paired-float return; Ghidra drops the s1 (Y), recovered from disasm.
    void firstEventSlotPos(float& outX, float& outY) const;

    // FUN_10000d7dc, walk the 4 eventTray entries; for the first slot
    // whose key matches `eventTypeKey` and has charges < cap, bump its
    // currentCharges by 1 (sound 0x2F fires from EventSlot::addCharge).
    void pushEventCharge(int eventTypeKey);

    // FUN_10000d850, same matcher as pushEventCharge but read-only:
    // returns true when at least one slot matches `eventTypeKey` and has
    // room for another charge. caller (applyTileTypeEffect's snag-0x1e
    // branch) uses this to decide whether to fall back to the action
    // queue when no slot can absorb the charge.
    bool canPushEventCharge(int eventTypeKey);

    // FUN_10000d0c8, clear the control-marker bank (zeros 13 bytes at
    // +0x1BB0..+0x1BBC = controlCount + controlQueuedDelta + controlReceived
    // Total + controlAdvanceBusy + first byte of controlDrainPhase). called
    // by FUN_100025238's case 0x51 (Discord) when controlReceivedTotal
    // % 10 != 0 (a charge is mid-fill, so Discord resets it).
    void clearCTRLBank();

    // FUN_10000d0b4, clear the xp-marker bank (zeros 13 bytes at
    // +0x12E0..+0x12EC = xpCount + xpQueuedDelta + xpReceivedTotal +
    // xpAdvanceBusy + first byte of xpDrainPhase). called from
    // FUN_1000161fc (Level::generate) section 7 to reset per-run XP state.
    void clearXPBank();

    // FUN_10000cb84, touch-release / engagement state machine. dispatches
    // touches to the 3 button-frame quads + 4 EventSlot pointer-entries.
    // returns 1 when the touch was consumed (caller stops further dispatch),
    // 0 when nothing was hit. branches:
    //   - getGame()->gameState in {1, 2}     -> engagement branch
    //   - any other gameState                -> release branch (switch on
    //                                          engagementState 0/1/2/3)
    // see gameplay_hud.cpp for the full body.
    int queryReleaseTouch();

    // --- byte-exact struct fields ---

    // +0x0000
    void* parent;                     // back-ref to DetailPanel (GameBoard+0x4408)

    // +0x0008..+0x043F: 5 stat-bar quads, each 0xD8 (TileIcon stride).
    // the 4 health-bar quads stack to visualize currentHealth / maxHealth.
    // layoutStatBars (FUN_10000bb60) sizes/positions them per the current
    // ratio. roles confirmed against the binary:
    //   healthBarFill     = filled portion of the bar (always visible)
    //   healthBarGain     = heal-in-progress extension above filled (col A,
    //                       same color as fill, so it looks like a smooth grow)
    //   healthBarOverflow = damage-in-progress overlay above filled (col B,
    //                       different color, highlights the loss)
    //   healthBarTip      = a 2-pixel cap at the top of filled (col A)
    TileIcon statusBar;               // +0x0008  (top status bar)
    TileIcon healthBarFill;           // +0x00E0  (filled portion, height = filled)
    TileIcon healthBarGain;           // +0x01B8  (heal segment, col A)
    TileIcon healthBarOverflow;       // +0x0290  (damage segment, col B)
    TileIcon healthBarTip;            // +0x0368  (top cap, height = min(filled, 2))

    // +0x0440: 2 packed ints managed by FUN_10000cb84 (queryReleaseTouch).
    //   engagementState  = which header the touch is currently captured on
    //                      (verified on device by tapping each):
    //                      0 = none / EventSlot
    //                      1 = user icon (top-left, paired with playerIcon)
    //                      2 = hamburger / settings menu (top-right, paired
    //                          with menuIcon)
    //                      3 = largeButtonFrame, gated by conditionalFlag
    //                          (paired with conditionalIcon; TBD which
    //                          on-screen button; only fires when
    //                          conditionalFlag is set)
    //                      written on touch-down hit, restored to 0 on release.
    //   publishedState   = engagementState when the release lands on the same
    //                      header. read by GameBoard::update to dispatch the click.
    int32_t engagementState;          // +0x0440
    int32_t publishedState;           // +0x0444

    // +0x0448..+0x0957: 6 button-frame and icon TileIcons
    TileIcon buttonFrame1;            // +0x0448
    TileIcon buttonFrame2;            // +0x0520
    TileIcon largeButtonFrame;        // +0x05F8
    TileIcon playerIcon;              // +0x06D0
    TileIcon menuIcon;                // +0x07A8
    TileIcon conditionalIcon;         // +0x0880  (UV/pos set by FUN_10000bab8)

    // +0x0958
    bool conditionalFlag;             // +0x0958  (gates conditionalIcon + largeButtonFrame draw)
    uint8_t pad959[3];                // +0x0959

    // health values driving both the center HP number and the 4 health-bar
    // quads. setHealth (FUN_10000d18c) updates currentHealth and rebuilds
    // tintHealth; setMaxHealth (FUN_10000d228) updates maxHealth and recomputes
    // the ratios. layoutStatBars consumes targetHealthRatio / currentHealthRatio.
    int32_t currentHealth;            // +0x095C  (numerator; init = 1)
    int32_t maxHealth;                // +0x0960  (denominator; init = 1)
    float previousHealthRatio;        // +0x0964
    float targetHealthRatio;          // +0x0968  (= currentHealth/maxHealth)
    float currentHealthRatio;         // +0x096C  (animated toward target)

    // +0x0970..+0x0A17: 3 ColorTints (each 0x38). the 3 tints display the
    // player's combat stats numerically at the top of the screen.
    ColorTint tintAttack;             // +0x0970  (top-left ATK number; FUN_10000d0dc)
    ColorTint tintDefence;            // +0x09A8  (top-right DEF number; FUN_10000d134)
    ColorTint tintHealth;             // +0x09E0  (top-center HP number; FUN_10000d18c)
                                       //          init color 0xff64ffff alpha 0x78

    // +0x0A18: ATK/DEF tint slide-animation phase. init 1.0 (settled).
    // swapAtkDefDisplays rewinds it to 0; update() drives the tint cross-slide
    // while it's < 1.0 (advances by 2*dt per frame), then it rests at 1.0.
    float fieldA18;                   // +0x0A18
    uint8_t padA1C[4];                // +0x0A1C

    // +0x0A20..+0x12DF: 10 XP markers on the right side of the screen (stride 0xE0).
    // count drawn = xpCount; each lit marker = 1 experience point earned.
    MarkerSlot xpMarkers[10];

    // +0x12E0..+0x12EF: xp marker bank companion fields. drives the
    // FUN_10000cf34 slot-advance animation: queueing logic when an
    // animation is already in flight (xpAdvanceBusy = 1), running total
    // of every advance request received this level (for the level-up
    // detection path), and a 1-byte "busy" gate that the animation
    // tick clears once it finishes.
    int32_t xpCount;                  // +0x12E0  (init = 0)
    int32_t xpQueuedDelta;            // +0x12E4  (deltas pushed while busy)
    int32_t xpReceivedTotal;          // +0x12E8  (running sum across the level)
    uint8_t xpAdvanceBusy;            // +0x12EC  (set by FUN_10000cf34, cleared
                                       //           by tick when no slot animating)
    uint8_t xpDrainPhase;             // +0x12ED  (FUN_10000c568 enters this when
                                       //           count >= 10 + queue pending;
                                       //           slots reverse-animate out)
    uint8_t pad12EE[2];               // +0x12EE..+0x12EF

    // +0x12F0..+0x1BAF: 10 CONTROL markers on the left side (stride 0xE0).
    // count drawn = controlCount; each lit marker = 1 control point.
    MarkerSlot controlMarkers[10];

    // +0x1BB0..+0x1BC7: control marker bank companion fields. same shape
    // as the xp bank: count + queued + total + busy + drainPhase, then the
    // two marker-bank "full" output flags at +0x1BC0/+0x1BC1.
    int32_t controlCount;             // +0x1BB0  (init = 0)
    int32_t controlQueuedDelta;       // +0x1BB4
    int32_t controlReceivedTotal;     // +0x1BB8
    uint8_t controlAdvanceBusy;       // +0x1BBC  (busy gate, mirrors xpAdvanceBusy)
    uint8_t controlDrainPhase;        // +0x1BBD  (mirrors xpDrainPhase)
    uint8_t pad1BBE[2];               // +0x1BBE..+0x1BBF

    // marker-bank "full" output signals. the marker tick (FUN_10000c568)
    // sets the matching flag to 1 once its bank reaches 10 lit markers and
    // begins draining. GameBoard::update reads each flag (as GameBoard+0x7FD8
    // / +0x7FD9), clears it, and opens the corresponding reward panel.
    uint8_t levelUpReady;             // +0x1BC0  XP bank full -> LevelUpPanel (= GameBoard+0x7FD8)
    uint8_t itemChoiceReady;          // +0x1BC1  CTRL bank full -> ItemChoicePanel (= GameBoard+0x7FD9)
    uint8_t pad1BC2[6];               // +0x1BC2..+0x1BC7

    // +0x1BC8: the event tray. 4 fixed slots, each 0x20 bytes. each slot
    // holds an EventSlot* + animation state used by addEventSlot to
    // tween the slot icon from off-screen to its tray position, and by
    // removeEventSlot to compact remaining slots leftward when one is
    // consumed. tray order is left-to-right; null slotPtr = empty slot.
    struct Entry {
        EventSlot* slotPtr;       // +0x00  null = empty
        float      currentX;      // +0x08  animation current X (lerped each frame)
        float      currentY;      // +0x0C  animation current Y (lerped each frame)
        float      targetX;       // +0x10  resting X position in the tray
        float      targetY;       // +0x14  resting Y position in the tray (= 0.1934)
        float      progress;      // +0x18  animation progress 0..1; init = 1.0
                                  //         on empty slots (animation done),
                                  //         reset to 0.0 on install (slide-in start)
        float      shiftDelay;    // +0x1C  staggered delay used by compaction
                                  //         (= i * 0.1 for the i'th shifted slot)
    };
    static_assert(sizeof(Entry) == 0x20, "EventSlotEntry must be 0x20 bytes");

    Entry eventTray[4];               // +0x1BC8..+0x1C47

    // +0x1C48
    int32_t selectedItem;             // +0x1C48  (init = -1)
    uint8_t pad1C4C[4];               // +0x1C4C
    // queryReleaseTouch's "release on EventSlot" output. when a touch is
    // successfully released on an event slot, this is set to the slot's
    // pointer (the value at eventTray[selectedItem].slotPtr); the
    // GameBoard update consumer reads it to know which event was activated.
    EventSlot* releasedEventSlot;     // +0x1C50  (init = nullptr)

    // ---- removal-animation list ----
    //
    // removeEventSlot push_backs a 0x20-byte RemovalAnim here when an
    // event slot is consumed (or wiped). the events-bar update path walks
    // the list and animates each entry's `slot` from its current position
    // toward (targetX, targetY = startY - 0.15625): a slide-up-and-off
    // effect. when an animation finishes, the consumer frees the
    // EventSlot and erases the entry.
    //
    // the binary allocates each node via operator_new(0x30) and links it
    // into a doubly-linked list with sentinel anchor at HUD+0x1C58. layout
    // is byte-identical to a libc++ std::list<RemovalAnim> on aarch64:
    //
    //   list = { sentinel_prev (8) | sentinel_next (8) | size_t size (8) }
    //   node = { node_prev (8)     | node_next (8)     | RemovalAnim (0x20) }
    //
    // so we port as the real C++ type. iteration, push_back, and erase
    // all match the binary's operations 1:1.
    struct RemovalAnim {
        EventSlot* slot;              // +0x00  the slot being removed
        float      currentX;          // +0x08  current animated X
        float      currentY;          // +0x0C  current animated Y
        float      targetX;           // +0x10  destination X (= slot.posX)
        float      targetY;           // +0x14  destination Y (= slot.posY - 0.15625)
        float      progress;          // +0x18  animation progress 0..1
        float      shiftDelay;        // +0x1C  countdown before progress
                                      //         starts advancing (0 = start
                                      //         immediately). same offset
                                      //         + semantics as Entry's
                                      //         shiftDelay; the binary's
                                      //         per-entry animator
                                      //         (FUN_10000c9d0) is shared.
    };
    static_assert(sizeof(RemovalAnim) == 0x20, "RemovalAnim must be 0x20 bytes");

    std::list<RemovalAnim> removalAnims;   // +0x1C58..+0x1C6F  (24 bytes)

    // +0x1C70..+0x1E1F: 2 overlay TileIcons (stride 0xD8). drawn on tex 8 when
    // overlayProgress > 0. FUN_10000d9ac resets them.
    TileIcon overlayQuad1;            // +0x1C70
    TileIcon overlayQuad2;            // +0x1D48

    // +0x1E20: death-heart overlay state. when overlayProgress > 0, draw()
    // binds tex 8 and renders both overlay quads (the broken-beating-heart
    // visual that pulses while the player watches Nemesis advance). the
    // tick is GameplayHUD::tickDeathHeart (= FUN_10000c794): overlayState
    // gates direction (0 = wind down, 1 = wind up); pulseTimer is the
    // cyclic 0..1 beat phase driven by fmodf each frame.
    // FUN_10000d9ac(this, 1) clears overlayProgress; FUN_10000d9ac(this, 0)
    // only clears overlayState.
    float overlayProgress;            // +0x1E20  (init = 0; 0..1 fade-in ramp)
    float pulseTimer;                 // +0x1E24  (cyclic 0..1 heartbeat phase)
    uint8_t overlayState;             // +0x1E28  (init = 0; 1 = death active)
    uint8_t touchReEntryGuard;        // +0x1E29  (FUN_10000cb84 sets to 1 on first
                                      //           call this gesture, clears on release.
                                      //           prevents engagement-branch reentry.)
    bool playSoundNextFill;           // +0x1E2A  (init = 1; read by FUN_10000c568)
    uint8_t pad1E2B[5];               // +0x1E2B..+0x1E2F

    // discard-staging queue. each entry is a floating quad anchored above
    // a rack column with a back-pointer to the staged rack slot. case-3 of
    // the central touch dispatcher commits the whole batch on release,
    // calling discardRackTile per staged entry and feeding Nemesis.
    // selectedItem (below) optionally decorates each tap with extra
    // effects but the discard itself is the universal action.
    std::vector<DiscardEntry> pendingDiscards;  // +0x1E30..+0x1E47 (libc++ begin/end/cap)
    float    pendingDiscardBobTimer;            // +0x1E48 (drives the per-entry bob easing)
    uint8_t  pad1E4C[4];                        // +0x1E4C..+0x1E4F (alignment for the 8-byte ptr below)

    // pointer to the kind-int of the Event the player has selected to
    // fire next (Events are the one-shot charged specials gated by the
    // Eventful perk, distinct from Items / Perks). set when the player
    // taps a charged EventSlot; consumed and cleared by whichever
    // dispatcher fires it next: FUN_10001d51c on a rack-tile tap,
    // FUN_100020f80 on the Event-button release, or the case-3 commit on
    // gesture-release with staged tiles. null when no Event is queued.
    int32_t* selectedEvent;           // +0x1E50 (= GameBoard+0x8268)
};
static_assert(sizeof(GameplayHUD) == 0x1E58, "GameplayHUD must be exactly 0x1E58 bytes");
static_assert(sizeof(std::vector<DiscardEntry>) == 0x18,
              "std::vector layout must be 0x18 (libc++ begin/end/cap)");
static_assert(sizeof(std::list<GameplayHUD::RemovalAnim>) == 0x18,
              "std::list layout must be 0x18 (libc++ sentinel pair + size)");
static_assert(offsetof(GameplayHUD, eventTray)       == 0x1BC8,
              "eventTray must be at HUD+0x1BC8");
static_assert(offsetof(GameplayHUD, releasedEventSlot) == 0x1C50,
              "releasedEventSlot must be at HUD+0x1C50");
static_assert(offsetof(GameplayHUD, removalAnims)    == 0x1C58,
              "removalAnims must be at HUD+0x1C58");
static_assert(offsetof(GameplayHUD, overlayProgress) == 0x1E20,
              "overlayProgress must be at HUD+0x1E20");
static_assert(offsetof(GameplayHUD, pulseTimer)      == 0x1E24,
              "pulseTimer must be at HUD+0x1E24");
static_assert(offsetof(GameplayHUD, overlayState)    == 0x1E28,
              "overlayState must be at HUD+0x1E28");
static_assert(offsetof(GameplayHUD, pendingDiscards) == 0x1E30,
              "pendingDiscards must be at HUD+0x1E30 (= GameBoard+0x8248)");
static_assert(offsetof(GameplayHUD, selectedEvent) == 0x1E50,
              "selectedEvent must be at HUD+0x1E50 (= GameBoard+0x8268)");

// constants from FUN_10000b160 / FUN_10000bab8 / FUN_10000bb60
namespace GameplayHUDConstants {
    // despite the historical "Y_BIG" naming, this is the X-OFFSET applied to
    // buttonFrame2 to put it on the right side of the screen. (the binary's
    // formula adds it to posX, not posY; confirmed against FUN_10000b160.)
    inline constexpr float HUD_BUTTON_X_RIGHT = 0.8843750f;  // DAT_100059ca0
    inline constexpr float HUD_ICON_NUDGE    = -0.0015625f;  // DAT_100059ca4
    inline constexpr float HUD_MARKER_X_IND  =  167.5f;      // DAT_100059ca8
    inline constexpr float HUD_MARKER_Y_BASE =   22.5f;      // DAT_100059cac
    inline constexpr float HUD_MARKER_DENOM  =  640.0f;      // DAT_100059cb0
    inline constexpr float HUD_MARKER_X_MAIN =  472.5f;      // DAT_100059cb4
    inline constexpr float HUD_COND_Y_NUDGE  = -0.0031250f;  // DAT_100059cb8
}
