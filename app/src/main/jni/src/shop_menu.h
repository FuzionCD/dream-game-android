#pragma once

#include "animation_controller.h"
#include "label.h"
#include "quad.h"
#include "text_item.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <set>
#include <vector>

class EventSlot;  // forward decl, used by Shop::heapPreview (event unlocks)

// ----------------------------------------------------------------------
// Persistent shop save buffer (0x50 bytes, slot 2 "unl"). lives at
// Game.shopSaveBuffer_, fed by Shop::dirtyXfer (FUN_100054298) on each run-end
// where the player earned keys, written to the "unl" file by the SaveSystem
// when the slot-2 dirty bit fires, and consumed by Shop::restoreFromSave
// (FUN_100054338) on game-init. layout mirrors the binary's slot-2
// loader / encoder offsets exactly:
//
// uint32 versionMagic    (slot-2 magic; first 4 bytes of blob)
// int    keys
// std::vector<int> faceUnlocks
// std::vector<int> snagUnlocks
// std::vector<int> eventUnlocks
// ----------------------------------------------------------------------
struct PersistentUnlocks {
    int32_t              versionMagic;      // slot 2 magic
    int32_t              keys;
    std::vector<int>     faceUnlocks;
    std::vector<int>     snagUnlocks;
    std::vector<int>     eventUnlocks;
};

// ----------------------------------------------------------------------
// Single "floating key icon", one of these per available key, drawn
// across the top of the shop panel. operator_new(0x100) in the binary
// allocates a libc++ std::list<ShopKeyIcon> node: 16 bytes of list
// __prev/__next plus 0xF0 bytes of payload below.
//
// per-node decay state lives in the 3 longs after the Quad. binary's
// lerp = (1 - decayPhase) * decayFromXY + decayPhase * decayToXY, so
// decayFromXY is the start (where it's coming from, set to current pos when
// the anim begins) and decayToXY is the end (landing target). decayPhase
// crosses 0->1 over the consumption animation; decayDelay counts down
// from a per-icon stagger before phase advances.
// ----------------------------------------------------------------------
struct ShopKeyIcon {
    Quad     quad;                   // drawn each frame
    uint64_t decayFromXY;            // packed (fromX, fromY)
    uint64_t decayToXY;              // packed (toX, toY)
    float    decayPhase;             // interp phase 0..1
    float    decayDelay;             // per-icon stagger
};

// reconstructed from Ghidra:
//   init (ctor + chrome):    FUN_1000502d4
//   seedPools:               FUN_100051a14   (helper, called by init + restore)
//   addKeys:                 FUN_100054254
//   open:                    FUN_100052058
//   draw:                    FUN_100053a70
//   update:                  FUN_1000525f4
//   recomputeRowAvailability:FUN_100052478   (helper)
//   formatUnlockedCounts:    FUN_10005250c   (helper)
//   setRowState:             FUN_100053f1c   (helper, 3 visual states)
//   dirtyXfer:               FUN_100054298   (push shop state to save buffer)
//   restoreFromSave:         FUN_100054338   (save-state restore)
//
// Shop is the between-runs UI at Game.shop_. holds the persistent key
// balance, three unlock rows (Face / Snag / Event), and the pools of
// candidates each row can spend keys on.
//
// flow:
//   - earned keys land here via addKeys(delta), called from
//     Game::update's scoreRequested-branch tail (= Forfeit / death).
//     dirty is set so Game::update pushes the new balance into the
//     persistent save buffer on the next frame (via dirtyXfer).
//   - the title-indicator click (shop badge) drives the case-2 transition
//     to open this UI for browsing.
//   - tapping a row consumes (rowIndex + 1) keys, picks a random entry
//     from that row's pool, and appends to the matching unlockedIds
//     vector (the rest of the game then sees the unlock via the read
//     sites in Game / WorldMenu / GameBoard).

// ----------------------------------------------------------------------
// Per-row preview slot (0x360 bytes). each ShopUnlockRow has 3 of these
// trailing; they hold the preview-icon Quads animating across the row
// during a successful unlock. binary's setRowState (FUN_100053f1c) tints
// 3 of the 4 quads per slot (the 4th's purpose is still un-decoded; types
// as `decorationOrPad` until a read site surfaces).
// ----------------------------------------------------------------------
struct ShopPreviewSlot {
    Quad    decorationA;   // tinted by setRowState
    Quad    decorationOrPad; // (un-tinted; possibly inert / pad)
    Quad    decorationB;   // tinted by setRowState
    Quad    decorationC;   // tinted by setRowState
};

// ----------------------------------------------------------------------
// Per-unlock row (0xED8 bytes). 3 of these live in Shop (rows[0..2]).
// each row carries:
//   - a 9-slice Label background (the row's pressable chrome).
//   - 3 small icon Quads (icon / price marker / unlocked marker; only
//     2 of 3 light up depending on the row's availability state).
//   - 3 TextItems (title "Unlock Face|Snag|Event" / description / "X/Y
//     Unlocked" counter).
//   - 3 preview slots, used during the unlock-reveal animation.
// ----------------------------------------------------------------------
struct ShopUnlockRow {
    // FUN_1000550b0, init: rowFrame.init() + 3 Quad ctors + 3 TextItem
    // init's + 12 preview-slot Quad ctors. called once per row by
    // Shop::init. C++ default construction handles the Quad ctors; this
    // method only needs to call init() on the Label and TextItems.
    void init();

    // Label::setColor (FUN_10004c794) call site in
    // FUN_100053f1c hands this to the per-state tint helper. 9-slice
    // glyph stack and position get installed by Shop::init.
    Label    rowFrame;

    // icon Quad (always visible). binary writes a packed RGBA
    // color directly into vertex 0..3 via FUN_10000826c.
    Quad     iconQuad;

    // price marker Quad (lit only when the row is currently
    // affordable and the pool has stock; state == 2 in setRowState).
    Quad     priceQuad;

    // "unlocked / sold-out" marker Quad (lit only on the
    // not-affordable state; state == 1 in setRowState).
    Quad     unlockedQuad;

    // "Unlock Face / Snag / Event" header text.
    TextItem title;

    // flavor description ("A new face to hide behind", etc.).
    TextItem description;

    // formatted "X/Y Unlocked" counter (rebuilt every open by
    // formatUnlockedCounts, FUN_10005250c).
    TextItem unlockedCountText;

    // 3 preview slots (0x360 each, total 0xA20). each slot is
    // 4 Quads stacked; setRowState tints 3 of them.
    ShopPreviewSlot previews[3];
};

// ----------------------------------------------------------------------
// Shop main struct (0x39E8 bytes). lives embedded at Game.shop_.
// ----------------------------------------------------------------------
class Shop {
public:
    // FUN_1000502d4, full chrome construction. inits the header Label,
    // 7 chrome Quads, the 3 ShopUnlockRows (each with rowFrame + inner
    // Quads + TextItems + 3 preview slots), the unlockAvatar + name
    // overlay, the 5 trailing anim Quads, and seeds the 3 pool sets.
    // called once from Game::create after placement-new resets RAII
    // members to their empty-sentinel state.
    void init();

    // FUN_100051a14. clears the 3 pool sets + 3 unlocks vectors, then
    // re-fills the sets with the canonical face/snag/event ID list.
    // public because the save-restore path (FUN_100054338) also calls it.
    void seedPools();

    // FUN_100054254. clamps keys + delta to [0, 20] and flags dirty if
    // delta > 0. called from Game::update's scoreRequested-branch tail
    // with the per-run keys-earned count, so each run's earnings
    // accumulate. dirty is later consumed by dirtyXfer (FUN_100054298),
    // which persists keys + the 3 unlock pools to the save buffer.
    void addKeys(int delta);

    // FUN_100052058. opens the shop UI: frees any leftover event-preview
    // EventSlot, snapshots keys -> keysBackup, clears + repopulates the
    // floating key-icon list, sets up the 3 unlock rows' TextItems, and
    // primes one update tick. called by Game::update's case-2 transition
    // (= title indicator "shop" tap).
    void open();

    // FUN_100053a70. renders the shop chrome + 3 rows + key icons. drawn
    // from Game::draw between World and TitleMenu when visible. includes the
    // unlock-anim branches (shakeActive / unlockAnimActive); in the basic
    // browse view those flags are clear so the animated-reveal Quads stay
    // unrendered.
    void draw();

    // FUN_1000525f4. per-frame tick. drives the key-icon decay list, the
    // unlock-reveal multi-stage animation (path follow + scale + shake),
    // and the touch hit-test for row press/release. called from
    // Game::update every frame while shop is visible.
    void update(float dt, float param2);

    // FUN_100052478. tints each of the 3 rows based on pool stock + key
    // affordability. state 0 = affordable + stocked, state 1 =
    // unaffordable OR pool empty.
    void recomputeRowAvailability();

    // FUN_10005250c. formats "X/Y Unlocked" into each row's
    // unlockedCountText.
    void formatUnlockedCounts();

    // FUN_100053f1c. applies one of 3 visual states to a single row:
    // 0 = white tint (default), 1 = darker gray (unaffordable / sold out),
    // 2 = light gray (hover highlight). state determines per-element rgba
    // colors for rowFrame Label + iconQuad + price/unlocked Quad +
    // 3 TextItems + 3 preview slot Quads.
    void setRowState(int rowIndex, int state);

    // FUN_100053d8c. helper used by Shop::update when the user releases
    // on a valid row: kicks off the unlock-reveal animation by zeroing
    // the 5 anim timers, snapping the keyIcons head onto animQuad[1],
    // and computing the path the avatar will follow.
    void beginUnlockSequence();

    // FUN_100054298. pushes live shop state (keys + 3 unlocks vectors)
    // into the persistent save buffer. fired from Game::update when
    // shop.dirty is set (= addKeys called during a run). dirty cleared
    // here after the push so the next frame won't re-fire.
    void dirtyXfer(PersistentUnlocks& snap);

    // FUN_100054338. inverse of dirtyXfer: pulls the persistent unlocks
    // back into shop on Game::init, then erases each unlocked ID from
    // the matching pool set so future random draws can't re-offer them.
    // called from Game::init after SaveSystem::load populates the save
    // buffer from disk.
    void restoreFromSave(const PersistentUnlocks& snap);

    // ---- byte-exact field layout (0x39E8 bytes total) ----

    // panel visibility / close-request flags, checked by Game::update
    // every frame.
    bool     visible;
    bool     closeRequested;
    bool     dirty;

    // big "Shop" header Label. 6-glyph 9-slice frame (corners + edges)
    // installed by Shop::init at scale 0.640625 x scale 0.2875.
    Label    headerLabel;

    // 7 chrome Quads. positions / UVs assigned by Shop::init's tail
    // block; roles:
    //   chromeQuad0  upper "Shop" badge; its posX (set to 0.84375) is
    //                the reference X for the floating key-icon row in
    //                Shop::open. tracked separately for clarity.
    //   chromeQuad1  thin horizontal divider above the rows.
    //   chromeQuad2  upper-right icon button (likely "close").
    //   chromeQuad3  thin vertical divider between rows and side panel.
    //   chromeQuad4  lower-right icon button.
    //   chromeQuad5  thin horizontal divider below the rows.
    //   chromeQuad6  lower "Shop" badge (mirror of chromeQuad0).
    Quad     chromeQuad0;                       // keyRefQuad
    Quad     chromeQuad1;
    Quad     chromeQuad2;
    Quad     chromeQuad3;
    Quad     chromeQuad4;
    Quad     chromeQuad5;
    Quad     chromeQuad6;

    // 3 unlock rows. ordered Face / Snag / Event (matches the binary's
    // iVar13 == 0 / 1 / 2 switch in FUN_100052058).
    ShopUnlockRow rows[3];

    // hoveredRow tracks the in-progress press (-1 when nothing held).
    // press writes the row index here; release re-checks contains and
    // either fires the unlock OR clears the highlight.
    int32_t  hoveredRow;

    // libc++ std::list of floating key icons drawn across the top of the
    // panel. populated by Shop::open in proportion to `keys`; nodes get
    // popped + freed by Shop::update's per-frame decay animation when
    // an unlock commits. each node = 16-byte list header + ShopKeyIcon
    // payload (0xF0) = 0x100-byte heap block (matches binary's
    // operator_new(0x100) site in FUN_100052058).
    std::list<ShopKeyIcon>  keyIcons;

    // persistent key balance, clamped 0..20 in addKeys.
    int32_t  keys;

    // snapshot of `keys` taken at Shop::open. used by the unlock-anim
    // tick to compute how many keys to drain visually.
    int32_t  keysBackup;

    // unlock-animation state machine bytes:
    //   unlockAnimActive  set on commit, drives the multi-stage
    //                     reveal sequence in Shop::update.
    //   unlockAnimStep2   second-stage gate inside the same anim.
    uint8_t  unlockAnimActive;
    uint8_t  unlockAnimStep2;

    // committedRow: captured from hoveredRow on press. survives the
    // release re-check (since hoveredRow gets cleared to -1 after).
    // keysToConsume: initial price = committedRow + 1.
    int32_t  committedRow;
    int32_t  keysToConsume;

    // 5 anim timers (key decay, reveal pulse, shake, etc.) driven by the
    // unlock-reveal sequence in update().
    float    animTimers[5];

    // path-interpolation state: std::vector of (x, y) pairs stored flat
    // (each point = 2 floats consecutively) + a parallel std::vector of
    // cumulative-fraction arc lengths. used by Shop::update's path-
    // follow stage to drag the avatar Quad along a curve from one row
    // to its committed slot. libc++ aarch64 std::vector head = 24 bytes
    // (3 pointers), matching the binary's pathPoints / pathLengths slots.
    std::vector<float> pathPoints;
    std::vector<float> pathLengths;

    // shake-step state (second stage of unlock-reveal: avatar jitters
    // before settling). shakeActive gates the per-frame interp; shakeStep
    // gates the secondary stage; progress / from / to drive the
    // cosine-eased motion between the two endpoints.
    uint8_t  shakeActive;
    uint8_t  shakeStep;
    float    shakeProgress;
    float    shakeFromX;
    float    shakeFromY;
    float    shakeToX;
    float    shakeToY;

    // avatar Quad, set to the chosen unlock's portrait UV via
    // FUN_100056478 (face) / FUN_10003e0a8 (snag). drawn during reveal.
    Quad     unlockAvatarQuad;

    // texture-index hint for the avatar (8 = face atlas, 10 = snag
    // atlas; un-decoded for event unlocks).
    int32_t  avatarTextureIndex;

    // operator_new(0xDD0) = EventSlot, allocated when committing an
    // EVENT unlock (the binary's iVar7 == 2 path in Shop::update);
    // face / snag unlocks leave this null (they just retarget the
    // unlockAvatarQuad via portrait / snag-atlas UV helpers). freed on
    // the next Shop::open. virtual dtor on EventSlot handles the
    // per-Quad cascade the binary inlines.
    EventSlot* heapPreview;
    float    heapPreviewAnimPhase;

    // unlock-name overlay, an AnimationController, not a TextItem,
    // despite the same 0x88 footprint. FUN_10003a010 (= update),
    // FUN_10003a408 (= reset), FUN_100039ea4 (= startText) are all
    // called on this slot from Shop::update / beginUnlockSequence /
    // commitUnlock. drives the "HELLO" + character-name flyout text.
    AnimationController unlockNameOverlay;

    // overlay offset from the avatar (computed each anim tick by the
    // FUN_100039ea4 string-builder return).
    float    overlayOffsetX;
    float    overlayOffsetY;

    // 5 trailing animation Quads (highlight overlays / spark effects),
    // driven by the unlock-reveal animation in draw() / update().
    Quad     animQuads[5];

    // 3 pool sets, remaining-to-unlock IDs for each row. std::set so
    // that "pick random + erase" is fast and "is X in pool?" is O(log n).
    // libc++ aarch64 std::set<int> = 0x18 bytes (begin + end_node + size),
    // matching the binary's seedPools sentinel-init pattern. seeded by
    // Shop::seedPools (FUN_100051a14) with the canonical face/snag/event
    // ID lists; pruned per unlock by Shop::update's commit path.
    std::set<int>    facePool;                  // 28 IDs
    std::set<int>    snagPool;                  // 20 IDs
    std::set<int>    eventPool;                 // 15 IDs

    // 3 unlocks vectors, the persistent already-unlocked ID lists,
    // serialized to save data. grows over time as the player commits
    // unlocks. FUN_100054338 (restore) reads these from save and erases
    // each entry from the matching pool set.
    std::vector<int> faceUnlocks;
    std::vector<int> snagUnlocks;
    std::vector<int> eventUnlocks;
};
