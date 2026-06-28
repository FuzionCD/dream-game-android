#pragma once

// DEBUG_FAST_XP: comment out to disable the +2 XP per tile placed debug
// shortcut. matches no binary behavior; purely for iterating on the
// level-up flow. delete this define and the matching block in
// game_board.cpp (search "DEBUG_FAST_XP") when shipping.
//#define DEBUG_FAST_CTRL_ENABLED

#include "player_system.h"
#include "animation_controller.h"
#include "color_tint.h"          // for TweenBody (embeds ColorTint by value)
#include "detail_panel.h"
#include "dialog_panel.h"
#include "event_choice_panel.h"
#include "gameplay_hud.h"
#include "hex_map.h"
#include "item_choice_panel.h"
#include "level_up_panel.h"
#include "nemesis_renderable.h"
#include "pause_menu.h"
#include "achievement_banner.h"
#include "quad.h"
#include "stat_bars.h"
#include "tile_weight_pool.h"
#include "tile_object.h"
#include "title_menu.h"          // for TileIcon
#include "user_stats_panel.h"
#include <cstddef>
#include <cstdint>
#include <list>
#include <set>
#include <vector>

// reconstructed from Ghidra:
//   constructor: FUN_100014ed8
//   destructor:  FUN_100015c1c
//   draw:        FUN_100018514
//   update:      FUN_100018ac8
//   handleIdle:  FUN_1000184e4
//   initLevel:        FUN_1000161fc
//   initLevelContent: FUN_1000165e8
//
// GameBoard is the primary gameplay container. once the player taps Start,
// picks a character, and picks a difficulty, this 0x10E18-byte struct owns
// every visible piece of the gameplay screen: the Nemesis, the GameplayHUD,
// the PlayerSystem (player avatar), the tile reserve queue, the 5-tile
// rack, and the placed-tiles page list.
//
// allocated with operator_new, pointer stored at Game.boardPtr_ (boardPtr).
// uses texture 9 (ui1.png) for most UI elements, texture 8 (sheet1.png) for
// portraits and icons, and tiles{1..4}.png for hex faces.

// Payload structs for the five GameBoard doubly-linked lists. each list is
// laid out by the binary as a libc++ std::list head + node sequence:
//   - head:  { sentinel.prev (8), sentinel.next (8), size (8) } = 24 bytes
//   - node:  { prev (8), next (8), payload (sizeof) } = 16 + sizeof(Body)
//
// the libc++ aarch64 std::list head order matches the binary's existing
// (Prev, Next, Count) field order at each list's base offset, and the node
// layout matches the binary's heap-allocated node objects exactly. these
// payload types are the body the binary stores in each node, and what
// std::list<Body> wraps with its prev/next link header.

// rack-render-order list at GameBoard.rackOrder, `value` = int slot index 0..4.
// std::list<int> node = 16 (prev/next) + 4 (int) + 4 (pad) = 24 bytes.
// nothing to declare here; we use std::list<int> directly.

// action queue at GameBoard.actionQueue, `value` = per-tile cosmetic side-effect.
// `actionType == 0` is the visual icon-burst variant (10 particle TileIcons
// spinning around a source point; see kickActionAnim). other types stash
// data for off-tick consumption (cross-rack rules + Pain HP-drain).
// `tileRef` is nullable; when set, the tile's mainQuad pos is added to the
// source pos each tick so the burst tracks a moving tile.
// particles is (re)sized to 10 by type-0 push and ticked / drawn by
// tickActionQueue / drawActionQueue. count is a separate 64-bit live count
// the binary keeps in sync with the vector's size; the draw walk reads
// count, not vector::size.
struct ActionBody {
    int32_t               actionType;
    float                 animT;       // 1.0 = completed
    float                 dataX;       // source pos X
    float                 dataY;       // source pos Y
    class TileObject*     tileRef;     // nullable; pos added to data
    std::vector<TileIcon> particles;   // 24 bytes (libc++ aarch64)
    uint64_t              count;       // live particle count

    ActionBody()
        : actionType(0)
        , animT(0.0f)
        , dataX(0.0f)
        , dataY(0.0f)
        , tileRef(nullptr)
        , particles()
        , count(0) {}
};

// stat tween queue at GameBoard.statTween: the floating "+N" / "-N" digit
// display that pops up next to a stat number when ATK / DEF / HP changes.
//
// each tween embeds a ColorTint (the digit-display widget rendering the
// signed delta) plus a (source, target) screen-pos pair that the tick lerps
// between over animT in [0, 1]. animT advances at 1.0/sec, so the animation
// lasts exactly one second, alpha-fading over the second half.
//
// pushed by setATK / setDEF / setHP (FUN_100020c40 / ce0 / d80) and by
// deeper combat-resolution paths (FUN_10001df54, FUN_10001f91c,
// FUN_100020f80) that surface stat changes from end-of-turn dispatch.
struct TweenBody {
    ColorTint   tint;       // (0x38; the digit display)
    float       sourceX;
    float       sourceY;
    float       targetX;
    float       targetY;
    float       animT;

    // tint is value-init'd by ColorTint having no default ctor; fields stay
    // garbage. push sites call tint.init() right after the list emplace.
    TweenBody()
        : tint()
        , sourceX(0.0f)
        , sourceY(0.0f)
        , targetX(0.0f)
        , targetY(0.0f)
        , animT(0.0f) {}
};

// tile reserve queue at GameBoard.tileReserve: a FIFO of pre-built tiles waiting to
// be dealt into the rack. each entry pairs a TileObject* with drawCountdown, a
// per-turn countdown gating when the tile is dealt: rollRackTile deals an entry
// only once drawCountdown < 0. almost every push sets it to -1 (dealable immediately);
// Obsession snags (kind 6) push obsessionCount so the spawned tile is held that many
// turns. the per-turn code loop decrements every entry, and the save system keeps
// the value verbatim.
struct TileReserveEntry {
    class TileObject* tile;
    int32_t           drawCountdown;

    TileReserveEntry()
        : tile(nullptr)
        , drawCountdown(0) {}

    TileReserveEntry(class TileObject* t, int32_t cp)
        : tile(t)
        , drawCountdown(cp) {}
};

// discard-slide list at GameBoard.discardSlide: tiles being animated off the rack.
// each entry holds the discarding tile plus its slide animation state
// (start/target positions + timer). when timer >= 1.0, the slide is complete
// and tickDiscardingTilesAnimation deletes the tile and erases the entry.
struct DiscardSlideBody {
    class TileObject* tile;
    float             startX;     // (= tile.posX at the moment discard fires)
    float             startY;
    float             targetX;    // (= rack column X, off-screen)
    float             targetY;    // (= virtualHeight + offsets, ~1.68 below screen)
    float             timer;      // (0..1, dt/0.3 per frame)

    DiscardSlideBody()
        : tile(nullptr)
        , startX(0.0f)
        , startY(0.0f)
        , targetX(0.0f)
        , targetY(0.0f)
        , timer(0.0f) {}
};

// 5 rack slots: the tiles in the player's hand (binary: GameBoard.rack).
// slot 0 is "freshest"; every populateRack() pops a tile from the reserve
// queue and slides existing slots up one index. when the player plays a
// tile, it moves from rack[N] into the page list.
#define RACK_SLOT_COUNT 5

// number of category tabs across the top of the menu UI (FUN_100014ed8 ctor
// loop iterates 6 times for the tileCursorQuads init).
#define MAX_CURSOR_COUNT 6

class GameBoard {
public:
    // allocate and construct (FUN_100014ed8)
    static GameBoard* create();

    // destruct and free (FUN_100015c1c)
    void destroy();

    // FUN_100018514: draw all elements
    void draw();

    // FUN_100018ac8: update state machine
    void update(float dt, float touchInput);

    // Android back button while in-game. no binary equivalent (iOS has no
    // back button); routes the press to existing in-game actions: a staged
    // tile returns to the rack; an open user-stats card or tutorial hint is
    // dismissed; the pause menu toggles; an in-progress modal (level-up /
    // item / event choice) or active drag absorbs the press.
    void handleBackPressed();

    // FUN_1000184e4: reset to idle state
    void handleIdle();

    // FUN_1000161fc: initialize for a new level (sets visible, populates
    // content). characterIndex feeds the character token sprite +
    // PlayerSystem reset; worldIndex stored for the level loader
    // / stat selection. snagFilter / eventFilter come from shop.snagPool
    // / shop.eventPool; seeds the gameplay-side exclusion sets so only
    // unlocked snags / events appear in rack rolls + event choice panel.
    void initLevel(int characterIndex, uint32_t worldIndex,
                   const std::set<int>& snagFilter,
                   const std::set<int>& eventFilter);

    // FUN_1000165e8: second-stage init (positions, tiles, animation)
    void initLevelContent();

    // FUN_1000269b8, pack the live GameBoard run-state into the slot-0
    // GameSnapshot. called from Game::update's mid-run save-trigger gate
    // (dirty != 0 && state == 1 && !eventChoicePanel.visible). clears
    // this->dirty on entry. covers the 7-int counter block,
    // worldIndex/tutorialFlag/gridLayout/exitGridCol/exitGridRow/keysRequired/
    // levelTurnCount/pickupSnagThreshold, hintFlags region, variantsUsed
    // + animBannerSeedHistory clones, 5 rack tiles, reserveItems list,
    // placedTiles list (+ placedContentMap + placedSnagMap derived from each
    // placed tile's TileContent / SnagContent), PlayerSystem snapshot,
    // Nemesis snapshot, HUD snapshot, HexMap snapshot, TileWeightPool
    // clone, and the 5 RNG seeds.
    void dirtyXferSnapshot(struct GameSnapshot& snap);

    // FUN_100016b18, rebuild the GameBoard's live state from a previously
    // captured GameSnapshot. called from Game::update's case-1 transition
    // (= the Start-button "Continue saved run" path). orchestrates: reset
    // achievement tracker session + banner, restore basic gb fields and
    // hint flags, rebuild variantsRemaining set, restore PlayerSystem +
    // 3 baseItems + perks, free + reallocate the 5 rack tiles from the
    // saved per-tile fields, free + rebuild the placedTiles page list
    // with each tile's placedContentMap / placedSnagMap values, reset various
    // sub-system flags (nemesis interlock, audio cues, etc.), restore
    // HUD + Nemesis + HexMap + reserve queue + TileWeightPool, re-seed
    // the 5 RNG streams, and run the layout pass to recompute scroll
    // bounds.
    //
    // snagFilter / eventFilter mirror initLevel's filters (= shop.snagPool
    // and shop.eventPool); they constrain which snags / events appear in
    // RNG rolls after the restore completes. seVolume / bgmVolume come
    // from getGame()'s global settings accessors (matches initLevel's
    // pattern; the binary's FUN_100016b18 takes them as an inline param
    // pair, but our Game struct has typed global accessors that read the
    // same bytes).
    void restoreFromSnapshot(const struct GameSnapshot& snap,
                             const std::set<int>& snagFilter,
                             const std::set<int>& eventFilter);

    // ---- Phase 3 tile generators + RNG helpers ----

    // FUN_100017fb8, push a content tile (TileContent) onto the reserve
    // queue. returns the new tile (binary returns its pointer).
    TileObject* pushReserveTile(uint32_t contentType, int32_t drawCountdown);

    // FUN_10001809c, push a snag tile (SnagContent) onto the reserve queue.
    // returns the just-allocated SnagContent (binary tail-calls
    // FUN_100013410 = getSnagIfAlive on the new tile, putting the snag
    // pointer in x0 at return). callers that want to mutate the new snag's
    // displayed atk / def / hp (e.g. FUN_100025dcc cases 0x1b / 0x4c / 0x5d)
    // capture the return; cases that just want to drop a tile in the
    // reserve queue discard it.
    class SnagContent* pushReserveSnagTile(uint32_t kind, int32_t drawCountdown);

    // FUN_100017f28, seed pickupSnagThreshold from levelTurnCount * scale + RNG.
    // called by initLevelContent and rollRackTile (when the reserve is empty
    // and the player has reached the threshold). scale = -0.12 on the first
    // level of any world (worldLevelIndex == 1), -0.08 thereafter.
    void seedPickupSnagThreshold();

    // FUN_100018180, fill the rack from slot 0; shifts existing tiles toward
    // higher indices. one tile per call. handles the snag-effect tile erasures
    // (Blinding Light / Loneliness / Zeal) and the Parasite swap-in.
    void populateRack();

    // FUN_1000203d4, roll a grid-idx 0..23 for the next tile placement.
    int rollContentGridIdx();

    // FUN_100020450, roll a per-content-type magnitude. driven by stat ranges
    // for types 2/3/6, SpecialAbility-driven RNG for type 5, default 1 otherwise.
    int rollContentMagnitude(uint32_t contentType);

    // FUN_10001a8fc, true if a snag of the given type is in rack or pages.
    bool hasSnagInBoard(int snagType);

    // FUN_1000201e8, walk rack[0..4]; if a tile's snag type matches, return
    // FUN_100013410(tile) (= the SnagContent if alive, else null). caller uses
    // the returned pointer to read SnagContent fields like consumedFlag.
    SnagContent* findSnagInRack(int snagType);

    // FUN_1000268c8, same idea but walks the page list instead.
    SnagContent* findSnagInPages(int snagType);

    // FUN_100026750, return the first snag of `snagType` in the rack, or
    // on rack-miss the page list. mirrors the binary's
    // `findSnagInRack(t) ?: findSnagInPages(t)`. used by Chunk E's snag-0x28
    // grow gate and FUN_100020f80's end-of-turn snag-effect pass.
    SnagContent* findSnagInRackOrPage(int snagType);

    // FUN_10001ffd4, pop a tile from the reserve queue (or roll fresh from
    // the draw pool if the queue is empty / no candidate matches).
    TileObject* rollRackTile(uint32_t flag);

    // FUN_100020254, roll a content type for a fresh rack tile from the
    // per-level draw pool, with type-specific filtering.
    int rollSnagType();

    // FUN_100018a80, predicate that controls whether SnagContent stat
    // tints render for a given tile. true = draw tints, false = skip them.
    // logic: if the tile has no live snag, true. otherwise checks the
    // snag's type against specific kinds (99 / 75) and the parent tile's
    // flag, plus a findSnagInRack(99) lookup.
    bool shouldDrawTintsFor(TileObject* tile);

    // ---- Step 9 update helpers (port of FUN_100018ac8 sub-routines).
    //
    // names below describe what each helper actually does on a typical
    // frame, not its anchor address. each one's binary anchor is recorded
    // in its inline comment + the comment on its definition in the cpp.

    // showNextAchievementBanner, top-of-FUN_100018ac8 hook (before the
    // `if (dialogPanel.visible) return` early-out). when achievementBanner.resetTimer
    // < 0 (= idle), pops the next pending-unlock idx from AchievementTracker
    // and opens a banner for it via FUN_10004fd7c. with resetTimer = 0 (the
    // first frame), nothing pops yet.
    void showNextAchievementBanner();

    // tickAmbientPickupHinting, port of FUN_10001980c. drives the
    // ambient "look at this thing" hint quad that points the player
    // toward an unread tile / panel / button. cascading priority over
    // ~20 hint targets keyed off a run of hint-state byte flags.
    // with no flags set and no targets, mostly a no-op.
    void tickAmbientPickupHinting(float dt, float touchInput);

    // tickDetailPanelIdleDismiss, FUN_10001a690. when the detail panel's
    // dismiss-watch byte is set, monitors timer / pointer-
    // distance to decide whether to auto-dismiss the panel. no-op while
    // that flag is clear.
    void tickDetailPanelIdleDismiss();

    // syncGlobalTileAlpha, FUN_10001a9b4. when tileAlphaProgress differs
    // from tileAlphaMirror, lerps every rack tile, page tile, and the
    // playerSystem character-token to the new alpha (40 dim -> 255 idle).
    // with both fields = 0 the walk is skipped.
    void syncGlobalTileAlpha();

    // tickInertialPanScroll, FUN_10001aa84. drives inertial finger
    // scroll: when panInertiaActive is set, applies panVelocityX/panVelocityY
    // to positionX/positionY, decays it, and pixel-snaps when the velocity
    // drops below threshold. also runs a separate cosine-eased pan that drives
    // positionX/Y between (panStartX, panStartY) and (panTargetX, panTargetY)
    // per the panProgress timer.
    void tickInertialPanScroll(float dt);

    // animateCleanupQuadBob, FUN_10001ac04. for each entry in
    // hud.pendingDiscards, snaps the floating quad to its rack column X
    // and bobs Y via a cosine-eased timer in hud.pendingDiscardBobTimer.
    // no-op on an empty queue.
    void animateCleanupQuadBob(float dt);

    // tickDiscardingTilesAnimation, FUN_10001ad30. walks the discard-slide
    // list; advances each node's slide timer; expired nodes
    // (timer >= 1) get unlinked + their TileObject deleted (= discard
    // complete). otherwise each node lerps its TileObject's screen pos
    // toward the target via FUN_100012c34. no-op on an empty list.
    void tickDiscardingTilesAnimation(float dt);

    // animateMidGrabHexHighlight, FUN_10001bd2c. while the player's
    // finger is dragging a rack tile, this drives the hex-cursor
    // mirror + the predictive "where will this land" hex marker.
    // three sub-paths: state-1 (touch begin), state-2 (touch move),
    // state-0 (touch released). with selectedRackSlot = -1, all paths
    // early-out.
    void animateMidGrabHexHighlight(float dt);

    // tryConsumeXButton, FUN_10001b484. dead-end / X-button tap handler.
    // gates on xButtonVisible; press branch tints + flags xButtonPressed,
    // release branch fires nemesis.eatTarget = max(pageCount - 1, eatTarget)
    // so Nemesis catches up to one tile behind the player. returns true
    // when the gesture was consumed (= the bbox hit).
    bool tryConsumeXButton();

    // dispatchHexAndRackTouch, FUN_10001ae10. central touch dispatcher
    // that routes the touch event against rack / hex grid / button quads
    // to advance the state machine. returns true if the touch was
    // consumed; false when no touch landed.
    bool dispatchHexAndRackTouch(float dt, float touchInput);

    // fireEvent, FUN_100020f80. per-kind effect dispatch for a released
    // Event. switches on slot->eventType (0..80) and applies that Event's
    // unique effect to the game state (discard tiles, gain stats, advance
    // Nemesis, etc.). returns true when the event was successfully
    // consumed (= the dispatcher should run the removal/sound/counter
    // path). some events return false when their precondition fails (e.g.
    // "Discard a held special snag" with no special snags held); in that
    // case the slot stays in the tray and no consumption happens.
    //
    // currently a scaffold: only a handful of EventKinds have effect bodies
    // implemented; everything else returns true (consume) with no effect.
    // see the body's per-case switch for which Kinds are real.
    bool fireEvent(EventSlot* slot, float dt, float anchorX, float anchorY);

    // commitRackTilePickup, FUN_100023ac4. binds the given rack slot as
    // the actively-dragged tile: captures drag offset (touch -> tile center),
    // sets draggedRackSlot, kicks the slide animation, lays out the cursor-
    // tab quads via setupDragCursorTabs (FUN_100020580), and plays sound 0xE
    // ("tileGrab"). called by tryPickupRackTile after a successful pickup
    // predicate.
    void commitRackTilePickup(int slotIdx);

    // FUN_100020880, true if the given hex (col, row) is already occupied
    // by a placed page tile, or by the Nemesis's reserved cell when alive.
    bool cellIsOccupied(int col, int row) const;

    // FUN_100024bd8, count page tiles whose grid coord is adjacent (hex
    // distance == 1) to the given tile's grid coord. consumed by the
    // content-type switch in updateNavArrowAndConfirmDrag (case 0xa, the
    // XP / experience tile; its magnitude scales with how many neighbors
    // it just landed next to).
    int countAdjacentPageTiles(TileObject* tile) const;

    // FUN_100020c40, write `value` (absolute) to ATK, refresh the
    // HUD ATK ColorTint, push a delta-signed tween into the StatBars row,
    // and fire the audio-engine ATK-changed hook. no-op when the new value
    // matches the current (= debounce). callers that want a relative change
    // pass `currentATK + delta`.
    void setATK(int value);

    // FUN_100020ce0, same shape as setATK but for DEF (the DEF field / DEF
    // ColorTint / DEF stat-bar slot).
    void setDEF(int value);

    // FUN_100020d80, HP set. clamps to [0, maxHP],
    // skips when level is already lost (`playerDowned`), refreshes
    // the HUD HP ColorTint, fires audio (sound 0x17 on heal, 0x18 on damage),
    // and on the death edge case (HP reaches 0) sets playerDowned + disables
    // the HUD bar + walks the page list to set nemesis.eatTarget for the
    // upcoming consume cycle.
    void setHP(uint32_t value);

    // FUN_100025c84, apply a content-type effect to the events bar.
    // looks up snag-0x1e in rack: when absent, charge the matching event
    // slot directly via HUD::pushEventCharge. when present, try the
    // charge first (HUD::canPushEventCharge); on failure, queue the
    // charge in the action queue at GameBoard.actionQueue.
    void applyTileTypeEffect(int eventTypeKey);

    // FUN_100024cc8, post-commit HexMap dispatch. fires once a rack tile
    // lands on the grid: visited-cell kind switch, snag-0x28 (Fear) grow,
    // snag-100 (Neglect) blank draw, snag-2 (Hound) damage tally, perk-6
    // DEF boost, and snag-0x2e (Pride) rack/page stat walks. full 7-section
    // breakdown at the definition.
    void dispatchHexMapPostCommit(TileObject* placedTile);

    // FUN_100025238, per-snag-type post-commit dispatch. fires when the
    // just-committed tile carried a live snag. tail-calls resolveSnagCombat
    // for most types.
    void dispatchSnagPostCommit(class SnagContent* snag);

    // FUN_100025dcc, combat resolution. damage exchange, ATK/DEF degrade,
    // bump anims, and (when the snag dies) the type-specific death effect
    // plus XP/control gain. shared by dispatchSnagPostCommit's section 6
    // and tryActivateNewestPageSnag.
    void resolveSnagCombat(class SnagContent* snag);

    // FUN_10001df54, end-of-turn resolution pipeline. fires after a tile
    // commits and the post-commit chain (dispatchHexMapPostCommit /
    // dispatchSnagPostCommit / resolveSnagCombat) finishes. four sections:
    // (1) pre-pass: suppress rack decorations, fade-tick HexMap, age
    //     world-tile counters, count specific snag types in rack;
    // (2) rack walk: per-rack-slot snag/content effects;
    // (3) page list walk: per-page-tile snag effects;
    // (4) final stat sync: defense floor, low-HP atk boost, HP regen,
    //     SPA item bonuses on rack content tiles.
    // see M5_END_OF_TURN_SCOPING.md for the full breakdown.
    void applyEndOfTurnPipeline(float dt);

    // FUN_10002678c, count rack + page-list tiles whose snag.type matches.
    // used in section 1 of applyEndOfTurnPipeline for the procrastination
    // counter (snag-0x10 across rack + page).
    int countSnagTypeInBoard(int snagType) const;

    // count rack[0..4] tiles whose snag.type matches. inline-equivalent
    // helper used 3 times in section 1 (Panic / Tragedy / Comedy counts).
    int countSnagTypeInRack(int snagType) const;

    // FUN_100024c40, count rack tiles by content predicate. arg semantics:
    //   0     -> rack tiles that are blank (no alive snag and no alive content)
    //   1     -> rack tiles that have an alive snag
    //   2..n  -> rack tiles whose getContentType() == arg
    int countContentTypeInRack(int code) const;

    // FUN_100026818, count rack tiles whose alive snag has type != excluded.
    // used by snag 0x71 (Bitterness).
    int countAliveSnagsExceptType(int excludedType) const;

    // helper used by 5 snag-type cases in section 2 of applyEndOfTurnPipeline
    // (Bite, Choking Grasp, Indecision, Cruelty, Paranoia, Repressed Memory).
    // walks rack[0..4], collects every slot whose tile is non-null and the
    // predicate returns true, then picks one uniformly at random via stream
    // 4 (= rngInt(0, n-1, 4)). returns -1 when no slot matches.
    //
    // mirrors the binary's open-coded `std::vector<int> + push_back loop +
    // rngInt` pattern. the binary inlines this with different predicates;
    // we extract it once here. each call site passes a file-static
    // predicate function.
    int pickRandomRackSlotMatching(bool (*predicate)(TileObject*)) const;

    // FUN_1000174c4, recompute scroll bounds (scrollMinX..scrollMaxY) by walking
    // the page list and taking min/max of each tile's hex coord. clears
    // bounds to (0,0,0,0) when the page list is empty.
    void recomputeScrollBounds(float touchInputY);

    // ---- action queue (GameBoard.actionQueue) ----
    // FUN_1000386b0, push or recycle a node onto the action queue. type 0
    // also seeds the 10 particle TileIcons + runs one zero-dt kick to set
    // their initial pose. originXY is a 2-float (x, y) source position;
    // tileRef is nullable.
    void pushAction(int actionType, const float* originXY, TileObject* tileRef);

    // FUN_100038530, advance one node's animT by dt/0.6 and rewrite every
    // particle's pos / scale / alpha for the current animT. used by the
    // initial seeding push and per-frame tickActionQueue.
    void kickActionAnim(float dt, ActionBody* node);

    // FUN_1000384cc, per-frame tick: walk the queue, advance every animating
    // (animT < 1.0) actionType==0 node via kickActionAnim. invoked from
    // GameBoard::update.
    void tickActionQueue(float dt);

    // FUN_100038440, per-frame draw: walk the queue and draw every animating
    // node's particle Quads. caller (GameBoard::draw) is responsible for the
    // texture binding (the tex 9 UI atlas, set right before this dispatch).
    void drawActionQueue();

    // FUN_1000388cc, mark every queued node animT = 1.0, abandoning all
    // in-flight animations without freeing the nodes. called on level
    // transition; the next push reuses the now-completed slots.
    void clearActionQueue();

    // ---- stat-change tween queue (GameBoard.statTween) ----
    // FUN_10002c00c, push or recycle a "+N" / "-N" floating digit display
    // for a stat change. textStyle picks the digit glyph table (0/1/2 =
    // ATK / DEF / HP). delta is the signed amount (sign drives green vs.
    // red color + whether a sign char is included). sourceXY is a 2-float
    // start position; the target is sourceXY + (0, +/-0.1); the +N text
    // floats up or down depending on `direction` (0 = down, !0 = up).
    void pushStatTween(int textStyle, int delta, const float* sourceXY,
                       int direction);

    // FUN_10002bee0, per-frame tick. gated by anyAnimating; advances each
    // animating node's animT by dt, lerps its ColorTint pos source->target,
    // applies an alpha curve (full opacity for first half, fade-out over
    // second half).
    void tickStatTween(float dt);

    // FUN_10002be84, per-frame draw. gated by anyAnimating; walks the
    // queue and draws each animating node's ColorTint via the bitmap-font
    // texture (tex 9, bound by the helper itself).
    void drawStatTween();

    // FUN_10002c170, abandon every in-flight tween. clears anyAnimating
    // and marks every node's animT = 1.0. nodes stay linked for recycling.
    void clearStatTween();

    // FUN_100024308, finalize a tile's rotation when it commits to a hex.
    // useFitting=false: snap rotation to nearest of 6 step values.
    // useFitting=true:  use the rules engine to pick the best rotation,
    //                   optionally biased toward facing the level exit
    //                   (useDirHint=true). called from sub-path 3 (b)'s
    //                   commit branch with both flags = true.
    void finalizeTileRotation(TileObject* tile, bool useFitting, bool useDirHint);

    // FUN_1000245cc, predicate used by the control-tile range overlay
    // (FUN_100024678) and the post-commit content-type-5 dispatch in
    // sub-path 3 (b). cells with HexMap kind == 4 always pass; cells with
    // kind == 0 may pass based on a column-parity / Item-driven rule.
    bool controlTileCellPredicate(int col, int row) const;

    // FUN_100024678, control-tile range overlay. clears gameplayItems
    // live count, then walks a 9x9 hex neighborhood around the newest
    // page tile (or origin when empty); per cell, if controlTileCellPredicate
    // passes and hex distance from anchor < 5, allocates / reuses a
    // gameplayItem TileIcon at the cell's screen position, with alpha
    // decaying by distance bracket (1->255, 2->150, 3->100, 4->50). called
    // from commitRackTilePickup when the picked tile is contentType 5
    // (control) and has no active kind-1 decoration.
    void updateControlTileHints();

    // FUN_1000244d0, release-while-holding snap-back. clears the cursor
    // visibility, then for the held tile (selectedRackSlot != -1) snaps
    // its screen position to its current local pos + board offset, kicks
    // a rack-slide animation back to the rack slot, runs finalizeTileRotation
    // (no fitting, no dir hint), and resyncs the magnitude. clears
    // selectedRackSlot afterward.
    void snapTileBackToRack();

    // FUN_100020580, lay out the 6 directional cursor tabs for the picked
    // tile. with an empty page list, only tab[0] gets the center cell. with
    // a non-empty page list, walks the most-recently-placed tile's permitted
    // exits and marks each free neighbor with a tab; the rules engine paints
    // each accessible neighbor white (placeable) or red (rejected by the
    // rules engine / reaches the level-end target too early).
    void setupDragCursorTabs(TileObject* picked);

    // tryPickupRackTile, FUN_100023ba8. validates whether the player can
    // pick up the given rack tile under current rack/snag state. returns
    // true on "normal pickup OK"; returns false when blocked or when a
    // special tile-effect action gets pushed instead (caller plays sound
    // 0x12 = "disabled" on false). when commit=true and a special rule
    // fires, the function pushes an entry onto the action queue.
    bool tryPickupRackTile(TileObject* tile, bool commit);

    // onPointerReleasedDuringDrag, FUN_10001b614. drives the rack-pickup
    // state machine on touch-down + touch-up: pick up a rack tile, snap it
    // back if released over itself, or directionally commit if released on
    // a hex target. resets selectedRackSlot / draggedRackSlot /
    // gameplayItemsLiveCount / exitArrowVisible, the back/nav button alphas.
    // with nothing in progress, most branches early-out.
    void onPointerReleasedDuringDrag();

    // openSnagDetailWithCombatSim, FUN_100023f84. starts the DetailPanel
    // dismiss watch (state = mode: 1 from rack tap, 2 from board tap),
    // runs a turn-by-turn combat simulation (player ATK/DEF/HP vs snag),
    // then opens the snag DetailPanel with the resulting win-turn counts.
    // anchor is the world-space position of the snag tile's quad.
    void openSnagDetailWithCombatSim(int mode, const float* anchor,
                                     SnagContent* snag);

    // updateNavArrowAndConfirmDrag, FUN_10001c450. drives the nav-arrow
    // / back-button rotation animation while a tile is grabbed. also
    // contains the "tile commit" branch that pushes the held tile onto
    // the page list when released over a valid hex.
    int updateNavArrowAndConfirmDrag(float dt);

    // nemesisSpawnAtTrailTail, FUN_10001c1a4. when Nemesis is dormant +
    // page list non-empty, picks a random "away-from-trail" exit on the
    // oldest page tile and places Nemesis there facing the trail. when
    // already alive, just calls nemesisScheduleNextStep.
    void nemesisSpawnAtTrailTail();

    // marchPageSnags, FUN_10001d1d8. walks the page list
    // looking for type-13 tiles, batches them into a small queue, and
    // for each batched-after-the-second tile dispatches the chain-
    // scoring effect (FUN_10003de74). second pass walks for snag-type
    // 0xF tiles and bumps nemesis.eatTarget.
    void marchPageSnags();

    // tryActivateNewestPageSnag, FUN_10001d414. if pageCount >= 2,
    // grabs the alive snag on the second-newest page tile (= last
    // committed) and runs its activation hook (FUN_100025dcc). returns
    // 1 if dispatched, 0 otherwise; pageCount = 0 returns 0.
    int tryActivateNewestPageSnag();

    // armNemesisInterlockOnSpecialTiles, FUN_10001d46c. walks the page
    // list, sets nemesis.eatTarget = max(1, eatTarget) and nemesis.eatActive
    // = true for each tile whose alive snag has type 0x18 or whose
    // tile-content has type 0xD. no-op on an empty page list.
    void armNemesisInterlockOnSpecialTiles();

    // applyHexPickupConsumeEffect, FUN_10001d51c. invoked by the
    // hex-pickup state. the player has just released over a page-tile
    // that contains an "effect" tile (12 specific content types);
    // dispatches the effect (drop snag, replace, give pickup, etc.).
    void applyHexPickupConsumeEffect(float dt);

    // refillRackPostCommit, FUN_10001d9dc. invoked at the end of state
    // commit: walks rack, refills empty slots from the reserve queue
    // via populateRack(), and applies the type-3 / type-0x3C scoring
    // logic for every refilled slot.
    void refillRackPostCommit();

    // applyChainStatBumpsToRack, FUN_10001db40. bumps each rack tile's
    // type-8 (snag-marker) magnitude by (pageCount + 1) then dispatches
    // FUN_1000178fc (post-tile recompute) + FUN_100017c04 (status icon
    // picker). with no type-8 in rack the walk is a fast no-op.
    void applyChainStatBumpsToRack(float dt);

    // placeExitAndKeys, FUN_1000175f0's body. writes the level-exit grid
    // position (exitGridCol/exitGridRow), adds the exit cell (kind 1) + its 6 hex
    // neighbors as key cells (kind 2) to the hex map, and configures the
    // exitTileIcon Quad's UV / size / position. shared by initLevel (which
    // rolls a fresh (col, row)) and restoreFromSnapshot (which passes the
    // saved exit position). does not seed keysRequired / keysCollected;
    // callers invoke setExitKeysRequired + recomputeExitKeysCollected after.
    void placeExitAndKeys(int col, int row);

    // setExitKeysRequired, FUN_100017794. seeds keysRequired
    // = clamp(requestedCount, 0, 4), then for each i in [0, keysRequired)
    // sizes / positions exitLockIcons[i] in a row stacked just below
    // exitTileIcon (uses indicator.posX/posY as the anchor). seeds each
    // with the hollow / "still needed" UV; FUN_1000178fc later flips
    // the first `keysCollected` icons to the filled UV.
    void setExitKeysRequired(int requestedCount);

    // recomputeExitKeysCollected, FUN_1000178fc. counts page-list tiles
    // at hex distance 1 from (exitGridCol, exitGridRow) into keysCollected, then
    // re-seeds each exitLockIcons[i] UV (filled if i < keysCollected,
    // hollow otherwise) and flips exitTileIcon UV between locked /
    // unlocked. when collected just reached required this turn, fires
    // sound 0x3F via getGame()->soundQueue.play.
    void recomputeExitKeysCollected();

    // updateExitArrowVisualState, FUN_10002493c. called after every rack
    // tile pickup. when the anchor cell (oldest page tile, or origin when
    // empty) is more than 3 hex steps from the exit, configures the
    // exit-arrow Quad's position + rotation and the digit ColorTint to
    // point toward the exit with the hex distance, then sets the visible
    // flag. when within 3 steps, leaves everything untouched.
    void updateExitArrowVisualState();

    // tryShowXButton, FUN_100017c04. scans the 6 directions from the
    // newest committed tile for a hex that would be a dead-end commit
    // (target is unblocked, either reachable from another page tile that
    // connects, or has all 6 of its own neighbors occupied). on match,
    // positions xButtonQuad at the target's screen coords and sets
    // xButtonVisible. on any direction's target with a free sub-neighbor
    // and no page tile fit, aborts so the button stays hidden. called
    // from applyChainStatBumpsToRack after every tile commit.
    void tryShowXButton();

    // helpers used by dispatchHexAndRackTouch's case-3 discard pass.

    // canDiscardRackTile, FUN_1000208f8. predicate: true when the rack
    // tile is in a state that allows the discard button to trash it.
    // gates on content-type whitelist, snag type, blocking snags in rack,
    // decoration-kind-0 absence, and Zeal's empty-slot protection.
    bool canDiscardRackTile(TileObject* tile);

    // pushDiscardStagingEntry, FUN_100023940. push a new DiscardEntry
    // (with staged = 0) onto hud.pendingDiscards for the given rack slot.
    // configures the floating quad's UV/size and seeds off-screen pos;
    // animateCleanupQuadBob takes over positioning from there.
    void pushDiscardStagingEntry(int slotIdx);

    // refreshDiscardButtonAvailability, FUN_100017b44. recomputes
    // hud.conditionalFlag based on whether the discard button should be
    // showing this frame (any rack tile passes canDiscardRackTile and the
    // game isn't in a blocking state).
    void refreshDiscardButtonAvailability();

    // togglePendingDiscardStage, FUN_100025d2c. flips the entry's staged
    // byte (0 <-> 1) and slides the underlying rack tile to the matching
    // visual position (preview above rack vs selected-to-discard zone).
    void togglePendingDiscardStage(DiscardEntry& entry);

    // refreshDiscardConfirmIcon, FUN_100025d48. scans pendingDiscards;
    // if any entry has staged != 0, swap the conditional icon to the
    // "confirm discard" appearance, else back to "staging active".
    void refreshDiscardConfirmIcon();

    // unstageDiscardEntryVisual, FUN_10002692c. snap a discard-staging
    // entry's rack tile back to its preview row (no staged-byte toggle;
    // caller already knows the entry is about to be popped). plays sound
    // 0x10 (the same unstage sound togglePendingDiscardStage uses). used
    // by fireEvent's prologue when abandoning a stale selection panel.
    void unstageDiscardEntryVisual(DiscardEntry& entry);

    // commitPendingDiscards, tail of FUN_10001ae10's case-3 branch when
    // hud.pendingDiscards has staged entries. resolves the staged rack
    // tiles: XP/score accumulation, snag/content stat side-effects,
    // nemesis spawn-vs-advance, perk-scaled stat buffs, queue clear.
    void commitPendingDiscards();

    // applyOnDeathHpRefill, FUN_10001dc44. fires on the death edge case
    // (HP reaches 0). reads playerSystem.perkLevel(9), the player's
    // level for perkType 9 ("Refills your current HP to N% when your HP
    // reaches 0"). scales playerSystem.maxHealth by 50% / 75% / 100%
    // (perk levels 0 / 1 / 2) and pushes the value via setHP
    // (= FUN_100020d80). the 50% default applies when the player doesn't
    // own the perk; the call site likely gates on a separate condition
    // (e.g. an "extra life" already consumed) we haven't fully traced.
    void applyOnDeathHpRefill();

    // applyPostTurnTileEffects, FUN_10001f91c. second-half of the
    // post-turn pipeline. iterates rack again applying type-0x18 +
    // type-0xC snag effects, plus a complex "decoration via type 0x55"
    // path that randomly picks a rack tile to add a kind=0 decoration.
    void applyPostTurnTileEffects();

    // tryOpenEventChoicePanel, FUN_10001fea4. fires at end-of-turn and
    // after each Event consume. when the player's eventTray has fewer
    // active slots than perkLevel(Eventful) allows, opens the
    // EventChoicePanel so they pick a fresh Event. when the panel is
    // already open (visible), the per-frame gameBoardUpdate routes
    // through eventChoicePanel.update instead and this call is a no-op
    // (the activeCount-vs-maxEvents check still gates re-opening).
    void tryOpenEventChoicePanel();

    // discardRackTile, FUN_10001dd14. helper used by state-5 / state-8
    // transitions: takes a rack slot's tile and starts its discard
    // animation by pushing it onto the discard-slide list
    // with start = current rack position and target = current tile
    // position. applies per-content-type extras (slot-0 darkness wipe,
    // type-0x11 split, type-2/3/6 stat-bonus chain). plays sound 0x14
    // (= "discard"). the tile gets actually deleted later when the
    // slide completes in tickDiscardingTilesAnimation.
    //
    // skipExtraEffects = true (binary's `param_3 & 1` set) skips the
    // per-content-type chain effects; used when the discard is part
    // of a different chain that already accounted for them.
    void discardRackTile(int slotIdx, bool skipExtraEffects);

    // tickExitArrowFade, FUN_10001a5a0. per-frame ramp on exitArrowFade,
    // driven by exitArrowVisible: byte set -> ramp toward 1 over 0.2s,
    // byte clear -> ramp back toward 0. each frame, alpha = fade * 255
    // is pushed into exitArrowQuad and exitArrowDigit.
    void tickExitArrowFade(float dt);

    // nemesisAdvance, FUN_10001dbe4. wake Nemesis at the trail tail
    // if dormant, then credit `xpAmount` to its XP track. drives every
    // path that grows Nemesis: chain-XP from snag kills, Loneliness
    // (snag 0x45) per-turn ping, content-type-4 (XP) tile placement,
    // close-strike snag/XP triggers.
    void nemesisAdvance(int xpAmount);

    // nemesisScheduleNextStep, FUN_1000209dc. compute eatStep (the per-
    // batch step countdown) from eatTarget vs pageCount, set the log-scaled
    // step duration, and fire the first step via nemesisStepForward. called
    // by nemesisSpawnAtTrailTail when entering an active eat cycle, and by
    // updateNemesisAndCloseStrike's followup-attack path.
    void nemesisScheduleNextStep();

    // nemesisStepForward, FUN_100020aac. execute a single "eat one tile"
    // step: decrement eatStep, place Nemesis on the next-oldest page tile
    // facing the trail, play the eat-content/snag/XP sound. when eatStep
    // hits 0 (was already 1) this just returns; the per-cycle countdown
    // is exhausted and the state-machine's state-7 routes to state-8.
    void nemesisStepForward();

    // setNemesisPanTarget, FUN_10001dca0. shared helper used by spawn /
    // step / commit paths. pans the camera toward (col, row)'s screen pos.
    void setNemesisPanTarget(int col, int row);

    // updateNemesisAndCloseStrike, FUN_10001a780. always runs nemesis.
    // update (FUN_100009094, nemesis spin / level number tint).
    // additionally: when nemesis.eatStep > 0, checks nemesis.posTransitionT;
    // if it just hit 1.0, executes the "nemesis lands on player"
    // close-strike: removes the newest page tile from play, applies a
    // type-1 / type-other split effect, decrements the pageCount, and
    // prepares the follow-up attack via FUN_100020aac if reformT also
    // at 1.0.
    void updateNemesisAndCloseStrike(float dt, float touchInput);

    // tickInGameStateMachine, the big switch on `state` at the
    // bottom of FUN_100018ac8. cases 1..10 + default. starts in case 1;
    // all other cases get walked when input lands.
    void tickInGameStateMachine(float dt, float touchInput);

    // --- struct fields, laid out to match the binary ---

    // core state
    bool visible;
    // semantic note: the scoreRequested / exitRequested bytes are exclusive
    // termination signals from PauseMenu's two paths.
    // scoreRequested = "show me my score" (Forfeit confirmation):
    //         Game::update opens ScorePanel for the dead-run summary.
    // exitRequested  = "just exit, no score" (Main Menu tap):
    //         Game::update fires the case-5 overlay transition straight
    //         back to TitleMenu, skipping ScorePanel entirely.
    // these match the binary's scoreRequested / exitRequested reads at
    // FUN_100045410's post-update branch cascade (the scoreRequested ->
    // exitRequested -> normal-play chain).
    bool scoreRequested;    // (Forfeit -> ScorePanel)
    bool exitRequested;     // (Main Menu -> TitleMenu, no score)
    bool dirty;
    bool saveNewSettings;
    uint32_t worldIndex;    // (set by initLevel from FUN_1000161fc param_3)
    // tutorialFlag, toggled by PauseMenu's Tutorial tab (XOR'd in
    // PauseMenu::update release path; copied back here in the consumer
    // chain). when this changes, GameBoard wipes the hint-state region at
    // so tutorial hints re-fire for the new mode.
    char tutorialFlag;
    // SE / BGM volume settings driven by PauseMenu's two volume sliders.
    // initialized to 0.5 (= centered slider). Game::dispatchSounds reads
    // seVolume as the per-frame SE gain context (FUN_100035cfc); Game's
    // music dispatch reads bgmVolume each frame as the target music vol
    // (FUN_100045410 -> FUN_100008800 = MusicController::setTargetVolume).
    float seVolume;         // (initially 0.5)
    float bgmVolume;        // (initially 0.5)
    int state;              // (game state tracker, initially 1)
    bool snagActivationSuppressed;
    bool snagMarchPending;            // gates marchPageSnags() in turn-resolve state 3; armed after a committed turn
    bool combatEffectsSuppressed;
    // run-wide turn counter. ++ in state-8 tile-resolution. resets only in
    // initLevel (= new run), not per-level despite the historical name; the
    // score panel reads GameBoard.totalTurnCount as "Turns Taken" (run-wide). also drives the
    // exp2(totalTurnCount/N) snag stat scaling in SnagContent::computeBaseStat
    // (more turns taken = exponentially harder fresh-spawned snags).
    int totalTurnCount;
    int worldLevelIndex;
    // snagsDefeated / specialSnagsDefeated: snag-kill counters surfaced as score-panel rows 2 + 3
    // ("Snags Defeated" and "Special Snags Defeated"). zeroed in initLevel;
    // incremented inside resolveSnagCombat's XP-gain branch.
    int snagsDefeated;        // ("Snags Defeated" score-panel row)
    int specialSnagsDefeated; // ("Special Snags Defeated")
    // levels-gained counter. ++ after the player commits a level-up panel
    // pick (game_board.cpp:12003). historically named "snagsDefeated"; the
    // score panel reads GameBoard.levelsGained as "Levels Gained".
    int levelsGained;
    // items-found counter. ++ after the player commits an item-choice panel
    // pick (game_board.cpp:11943). historically named "turnsTaken"; the
    // score panel reads GameBoard.itemsFound as "Items Found".
    int itemsFound;
    int eventsFired;        // (++ per consumed Event in fireEvent)
    // per-level byte that's activated when a tile is placed on the exit token.
    char exitReached;
    // count of bonus / "pickup" tiles the player has collected. drives the
    // snag-spawn threshold below.
    int levelTurnCount;
    // next levelTurnCount value at which the rack roll switches to producing
    // snag tiles instead of bonus content. seeded by seedPickupSnagThreshold.
    int pickupSnagThreshold;

    Quad titleQuad;         // (0xD8; owns its trailing anim rect)
                            // UV: (0.0, 0.843) to (0.625, 1.0), size 1.0 x 0.25

    // per-level cosmetic variant (0..0xB, 12 values) picked at level start by
    // FUN_1000165e8 / FUN_100016b18. determines which of 12 hex sprite sheets
    // the board renders this level. orthogonal to gridIdx (the 24-per-variant
    // directionality index) which is rolled per-tile.
    int gridLayout;

    // anti-repeat variant rotation. each level rolls one variant from
    // variantsRemaining (RNG stream 0), erases it from the set, and appends
    // it to variantsUsed. when variantsRemaining empties, we clear
    // variantsUsed and re-seed the set with 0..0xB for the next cycle.
    //
    // libc++ aarch64 layouts: std::vector<int> = 3 pointers (24B);
    // std::set<int> = begin_node + end_node + size (24B).
    // matches binary's FUN_1000283bc (vec push-with-grow), FUN_100028d5c
    // (set::insert), FUN_100026db0 (idx-th roll + erase), FUN_100028aa8
    // (set::erase by value, used by FUN_100016b18 level transition).
    std::vector<int> variantsUsed;
    std::set<int>    variantsRemaining;

    // the player's 5-tile hand. populateRack pops from the reserve queue at
    // to fill slot 0; existing tiles slide up one index per pop.
    TileObject* rack[RACK_SLOT_COUNT];  // (5 x 8-byte pointers)

    // doubly-linked rack-render-order list. each entry is a slot index
    // (0..4); GameBoard::draw walks this list to draw rack tiles in the
    // correct stacking order, dereferencing rack[slot] per visit.
    // libc++ aarch64 std::list head { sentinel.prev (8), sentinel.next (8),
    // size (8) } = 24 bytes; matches the binary's (Prev, Next, Count) trio
    // exactly. push-back = "draw this slot last"
    // (= on top of the stacking order).
    std::list<int> rackOrder;

    // rack slot index of the tile the player is currently dragging from the
    // rack (mid-drag, pre-commit). set by FUN_100023ac4 when pickup commits;
    // copied into selectedRackSlot when the user drops on a hex (entering
    // rotation/confirm phase); cleared back to -1 when the gesture ends.
    int draggedRackSlot;        // (init -1)

    // index of the currently-grabbed rack slot during a tile-place gesture.
    // -1 = no active grab (the default). gameplay sets this when the player
    // touches a rack tile; GameBoard::draw treats it as "draw this tile last
    // so it appears on top of the others". Phase C wires the input.
    int selectedRackSlot;       // (init -1)

    // drag offset captured at pickup (FUN_100023ac4 writes both): the touch's
    // offset from the held tile's center at the moment pickup committed. used
    // to position the tile under the finger as it moves. cleared on release.
    float dragOffsetX;
    float dragOffsetY;

    // doubly-linked list of placed tiles on the hex grid. push_back = "newest
    // placed tile is now the back of the list"; front() = "oldest placed tile
    // (= first one to fall off if Nemesis catches up)".
    // libc++ aarch64 std::list head matches the binary's (sentinel.prev,
    // sentinel.next, size) trio.
    std::list<TileObject*> pageList;
    // rotation / nav-drag state used by updateNavArrowAndConfirmDrag
    // (FUN_10001c450). -1 = idle, 0 = nav-arrow drag (pick adjacent
    // direction), 1 = back-button rotation drag. drives both the
    // GameBoard::draw nav-arrow gate and the rotation commit logic.
    int navDragState;           // (init -1)

    // back-button rotation drag state. all three fields are seeded on the
    // touch-down that lands on the back button (navDragState becomes 1),
    // mutated each touch-move frame, and consumed on touch-up to decide
    // between "tile rotated enough to commit the new orientation" and
    // "tile barely moved -> advance to next-allowed rotation step in the
    // dirList". reset to 0 on touch-up (navDragAnchorRotation /
    // navDragAnchorAngle stay at their last value but are unused while
    // navDragState == -1).
    float navDragAnchorRotation;  // tile.rotation in degrees at touch-down
    float navDragAnchorAngle;     // atan2(touch - tile center) at touch-down, radians
    float navDragDuration;        // seconds accumulated during touch-move

    // 6 tab buttons, each a TileIcon (a Quad plus 0x10 extra bytes, 0xD8 total).
    // confirmed from FUN_100014ed8 init loop: stride 0xD8 across the 6 entries.
    // UV: (0.032, 0.0) to (0.275, 0.134), size 0.234 x 0.253
    TileIcon tileCursorQuads[MAX_CURSOR_COUNT];

    bool tileCursorVisible[MAX_CURSOR_COUNT];
    bool tileCursorState[MAX_CURSOR_COUNT];

    // these three are all TileIcons (0xD8) per the binary, not bare Quads.
    // gaps between offsets confirmed: 0x7C8 - 0x6F0 = 0xD8, 0x8A0 - 0x7C8 = 0xD8.
    TileIcon navArrowQuad;   // (UV: 0.083,0.0 to 0.181,0.098, size 0.156x0.156)
    TileIcon backButtonQuad; // (UV: 0.0,0.0 to 0.082,0.193, size 0.131x0.309)
    // the "X" / dead-end escape button. tryShowXButton (FUN_100017c04)
    // scans the 6 directions from the newest committed tile for a hex that
    // is reachable and has all 6 of its own neighbors blocked, i.e.
    // committing here would trap the player. when found, positions the
    // Quad at that hex's screen coords and sets xButtonVisible = 1; the
    // draw step in GameBoard::draw renders it gated on visible + state == 1.
    // tap handler tryConsumeXButton bumps nemesis.eatTarget = max(pageCount
    // - 1, eatTarget), letting Nemesis catch up to one tile behind the
    // player so play can continue past the bottleneck.
    TileIcon xButtonQuad;  // (UV: 0.904,0.533 to 1.0,0.629, size 0.153x0.153)

    bool xButtonVisible;   // (set by tryShowXButton)
    bool xButtonPressed;   // (set by tryConsumeXButton on touch-down)
    float positionX;       // (draw translate X)
    float positionY;       // (draw translate Y)

    // board drag + inertial-pan-scroll tracking. seeded while the user drags
    // the board (FUN_10001bd2c) and consumed by the inertial fling
    // (FUN_10001aa84) after the finger lifts.
    float   panAnchorX;       // (positionX - touchX, captured at drag-begin)
    float   panAnchorY;
    float   panVelocityX;     // (smoothed fling-velocity pair)
    float   panVelocityY;
    float   panPrevTouchX;    // (prev-frame touch, for the velocity delta)
    float   panPrevTouchY;
    bool    panDragActive;    // (finger down, dragging the board)
    bool    panInertiaActive; // (post-release inertial fling running)

    // pan-animation slots. writes by FUN_10001dca0 (camera
    // pan after Nemesis spawn / step / commit). pan source = current
    // (positionX, positionY) snapshot; target = pixel-snapped destination
    // in normalized board space; progress = 0..1 timer (init 1.0 = idle).
    float panStartX;
    float panStartY;
    float panTargetX;
    float panTargetY;
    float panProgress;
    float scrollMinX;
    float scrollMinY;
    float scrollMaxX;
    float scrollMaxY;
    // tile-alpha ramp during drag mode. tileAlphaProgress is driven by
    // animateMidGrabHexHighlight (0->1 while held, 1->0 on release); each
    // frame syncGlobalTileAlpha lerps an alpha byte (200 idle -> 40 full
    // drag) and propagates it onto every rack / page tile + the player
    // avatar so the held tile reads clearly against the dimmed board.
    // tileAlphaMirror caches the prior frame's value to skip redundant
    // dispatches.
    float   tileAlphaMirror;
    float   tileAlphaProgress;

    // Nemesis enemy: 25-segment body + XP dot ring + level number tint.
    // an instant-kill antagonist that follows the player and consumes their path.
    NemesisRenderable nemesis;  // (0x3A30 bytes, ends at 0x4400)

    // per-eat animation duration (log-scaled by step count). set in
    // nemesisScheduleNextStep, consumed by the visual advance in
    // nemesisStepForward.
    float   eatStepDuration;

    // multi-mode info card. 6 display modes, fade animation, 3 text lines.
    DetailPanel detailPanel;    // (0x10B0 bytes, ends at 0x54B8)

    // modal confirm dialog with backdrop dim and 4 button-pair configurations.
    // strongly suspected to be the tutorial system.
    DialogPanel dialogPanel;    // (0xF60 bytes, ends at 0x6418)

    // gameplay HUD: status bar, stat bars, button frames, icons, marker rings,
    // 3 ColorTints, overlay quads.
    GameplayHUD hud;  // (0x1E58 bytes, ends at 0x8270)

    // player entity: character token Quad, stats, 3 starter Item slots,
    // dynamic Item vector, movement queue. fully ported: Items, Special
    // Abilities, Perks, level-up, and movement all live here.
    PlayerSystem playerSystem;     // (0x1A8 bytes, ends at 0x8418)

    // per-frame "level-progression pending" flag. update case 8 reads
    // this to gate world-advance vs level-advance branching, then clears it.
    bool    playerDowned;

    // bottom-of-screen stat-bar segment row (20 Quads + per-slot anim state).
    // ctor FUN_10003bc20, draw FUN_10003be88, update FUN_10003beec.
    StatBars statBars;               // (0x1230 bytes, ends at 0x9650)

    // stat-change tween queue (see TweenBody above): pending floating
    // "+N" / "-N" digit displays that pop up when a stat changes. push_back
    // adds the newest tween; iteration is oldest->newest. anyAnim gates the
    // tick / draw walks entirely (no walk fires when no slot is active).
    std::list<TweenBody> statTween;
    bool                 statTweenAnyAnim;  // any slot animating this frame

    // action queue (see ActionBody above): pending per-tile cosmetic /
    // side-effect entries. completed slots (animT >= 1.0) get recycled by
    // the push helper or torn down by the tail iterator.
    std::list<ActionBody> actionQueue;

    // anti-streak weighted-roll table for fresh rack-tile content types.
    // 5 entries are installed in GameBoard::create() (mirroring the binary's
    // ctor pushes). consumed by rollRackTile's fresh-roll branch via
    // weightedRollTileType(tileWeightPool, 4).
    TileWeightPool tileWeightPool;

    // discard-slide list (see DiscardSlideBody above): tiles being animated
    // off the rack during discard. each entry's timer advances at dt/0.3
    // per frame in tickDiscardingTilesAnimation; on completion the tile is
    // deleted and the entry erased.
    // previously misnamed "decoration list"; confirmed via sound 0x14
    // ("discard") triggered by discardRackTile (FUN_10001dd14).
    std::list<DiscardSlideBody> discardSlide;

    // dynamic gameplay items: vector of TileIcons drawn during the
    // FUN_100018514 step that fires before the page-list head tile. binary
    // tracks an explicit live count (= visible items, may be < vector
    // capacity), separate from the std::vector's own size.
    std::vector<TileIcon> gameplayItems;     // (24 bytes, std::vector head)
    int64_t               gameplayItemsLiveCount;

    // tile reserve queue: std::list of TileReserveEntry. populateRack pops
    // one entry per call from the front (= oldest) to fill a rack slot. if
    // the queue is empty when populateRack runs, the pop helper
    // (FUN_10001ffd4) falls through to RNG-rolling a brand-new tile from
    // the per-level tile draw pool.
    //
    // who pre-populates the queue depends on the entry path:
    //   - fresh level 1 (FUN_1000161fc -> FUN_1000165e8): pushes 9
    //     hardcoded entries, 8 content tiles `[3,3,6,2,2,3,2,5]` + 1 kind=1
    //     generic snag tile. the player's first hand is drawn from these.
    //   - fresh level > 1 (same path, never normally reached on
    //     fresh runs): no hardcoded pushes; populateRack falls through to
    //     RNG-rolled tiles for every slot.
    //   - loading a saved game (FUN_100016b18, the new-game / continue
    //     path triggered by the title menu, not the in-engine level
    //     advance): the snapshot's starter-encounter vector
    //     supplies the queue contents.
    std::list<TileReserveEntry> tileReserve;

    // hex grid map: std::map<(tileVariant, gridIdx), Cell> tracking the cells
    // the character has visited / can see. drawn by GameBoard::draw. cells
    // populated by the layout pass (FUN_1000175f0) at level start and as
    // the character moves through Phase C input.
    HexMap hexMap;                   // (0x18 bytes, std::map head)

    // hex grid coordinates of the level-exit cell: the locked hex the
    // player must reach to advance to the next level. set once per level
    // by the layout pass (FUN_1000175f0, rolls a jitter origin within +/-
    // range). the exit's 6 hex neighbors are the "key" cells; placing a
    // page tile on each key adjacent to the exit increments keysCollected.
    // when keysCollected >= keysRequired, the player
    // can place a tile that connects to the exit; before then, the rules
    // engine paints those placements red.
    //
    // historically labeled exitGridCol/exitGridRow because Phase C will
    // show a character avatar walking from the start tile to the exit;
    // the binary's gameplay treats this slot as the exit position, not
    // the character's instantaneous position. TODO: rename to
    // exitGridCol/exitGridRow once the avatar-walk port confirms it
    // doesn't update this slot.
    int32_t exitGridCol;             // (semantic: exit/level-end col)
    int32_t exitGridRow;             // (semantic: exit/level-end row)

    // visible Exit tile. the HexMap cell at (exitGridCol, exitGridRow) is added
    // with kind=1 but explicitly uses zero UV (invisible); the player
    // sees the exit through this quad. its UV flips: "locked" art while
    // keysCollected < keysRequired, "unlocked" art the first time the
    // gate opens (FUN_1000178fc also fires sound 0x3F on the transition).
    // positioned with X = hexCellLinearX(exitCol, exitRow) + 0.0015625,
    // Y = (param_2 from caller) + (-0.01875), pixel-snapped.
    TileIcon exitTileIcon;           // (0xD8 bytes)

    // lock icons stacked on the Exit tile (1..4 per level, sized by
    // FUN_100017794). each one shows "key still required"; FUN_1000178fc
    // flips the first `keysCollected` icons to the filled UV.
    TileIcon exitLockIcons[4];       // (4 * 0xD8 = 0x360 bytes)

    // number of key cells (= path tiles adjacent to the exit hex)
    // required to unlock the exit. set per-level by FUN_100017794:
    // `keysRequired = clamp(worldLevelIndex + 1, 0, 4)`. the rules
    // engine forbids placing a tile that connects directly to the exit
    // until keysCollected >= keysRequired.
    int32_t keysRequired;

    // running tally of key cells collected: number of page-list tiles
    // currently at hex distance 1 from the exit. recomputed by
    // FUN_1000178fc each time the page list changes (post-tile-commit
    // recompute).
    int32_t keysCollected;

    // exit-arrow visualization. drawn when the player is more than 3 hex
    // cells from the exit, near screen center, slightly offset toward the
    // exit. rotation points toward the exit; the digit ColorTint shows the
    // hex distance. configured by updateExitArrowVisualState (port of
    // FUN_10002493c) on every rack tile pickup.
    Quad      exitArrowQuad;         // (0xD8; owns its trailing anim rect)

    // digit display for the hex distance. positioned at exitArrowQuad.posXY.
    ColorTint exitArrowDigit;

    // FUN_10002493c sets this to 1 when the arrow becomes visible; cleared
    // to 0 by initLevelContent. on its own does not gate the draw; that's
    // exitArrowFade's job.
    bool      exitArrowVisible;

    // fade-in / fade-out progress for the arrow. drawn only while > 0.
    // also zeroed by initLevelContent. ramped per frame by tickExitArrowFade
    // (FUN_10001a5a0): toward 1 while exitArrowVisible, back to 0 once cleared.
    float     exitArrowFade;

    // full-screen dim Quad. drawn (on no texture) when dimProgress > 0;
    // its alpha is ramped by case-10 of the state machine via a cosine
    // ease tied to the dimProgress timer.
    Quad dimQuad;                    // (0xD8; owns its trailing anim rect)

    // case-10 fade timer. 0.0 = no dim, 1.0 = fully dimmed. on level exit
    // it ramps 0 -> 1, then initLevelContent runs, then ramps 1 -> 0 to
    // reveal the new level. only dimProgress has an observed read site so far;
    // stays raw until a consumer needs it.
    float    dimProgress;

    // transient overlay text animation (dream text shown on new level start).
    AnimationController animController;

    // animation-banner "draw without replacement" bookkeeping. on each
    // level transition (FUN_1000165e8 tail, skipped on level 1) one index
    // is popped from the pool and pushed onto the history; when the pool
    // empties, history clears and the pool refills 1..animTableSize-1.
    std::vector<int64_t> animBannerSeedHistory;
    std::set<int64_t>    animBannerSeedPool;

    // snag-type exclusion filter consumed by rollSnagType. populated at
    // startNewRun (FUN_1000161fc tail) by copy-from-source
    // TODO is this even used properly?
    std::set<int> excludedSnagTypes;

    // achievement-unlock banner popup. ctor FUN_10004efec, reset
    // FUN_10004f3ac, update FUN_10004f490, open FUN_10004fd7c.
    AchievementBanner achievementBanner;

    // user-stats overlay (the read-only panel opened by tapping the
    // top-left HUD player-icon button). ctor FUN_100009a00, open FUN_10000a9f4,
    // update FUN_10000a594, draw FUN_10000a3dc. publishedState == 1 frpm
    // HUD's queryReleaseTouch routes to userStatsPanel.open(playerSystem, &worldIndex).
    UserStatsPanel userStatsPanel;

    // event-choice panel (FUN_10000e128 ctor + e6d4 draw + e834 update).
    // presents 1..4 candidate Events for the player to pick. on confirm,
    // onConfirmTapped writes the source EventSlot* into
    // selectedEventOnConfirm; gameBoardUpdate clones it and installs via
    // hud.addEventSlot.
    EventChoicePanel eventChoicePanel;

    // level-up panel (FUN_10002d278 opener). triggered when the HUD's
    // XP level-up trigger byte is set by the XP marker bank filling
    // 10 markers (one level per cycle). gameBoardUpdate sees that byte set,
    // clears it, and calls levelUpPanel.open(playerSystem); commit drains
    // via getNextStatPick / getNextPerkPick.
    LevelUpPanel levelUpPanel;

    // item-choice panel (FUN_100034d60 opener). triggered when the HUD's
    // CTRL item-choice trigger byte is set by the CTRL marker bank filling.
    // gameBoardUpdate clears the trigger byte and calls
    // itemChoicePanel.open(&playerSystem); commit installs the selected
    // candidate Item into playerSystem.baseItems[type].
    ItemChoicePanel itemChoicePanel;

    // in-game pause menu (0x1990 bytes). a Menu-derived popup
    // with 3 tabs, 2 info rows, 2 standalone icon Quads, an embedded
    // ForfeitConfirmPanel, and a trailing std::vector. ctor at
    // FUN_10002e838; opened by
    // FUN_10001ae10's case-2 branch when the HUD's menu icon is tapped.
    //
    // not the post-run score panel; that lives heap-allocated at
    // Game.scorePanel_. see pause_menu.h for full layout.
    PauseMenu pauseMenu;
};