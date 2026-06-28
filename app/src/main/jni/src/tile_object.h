#pragma once

#include "quad.h"
#include "tile_decoration.h"
#include "tile_content.h"
#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>

class TileContent;
class SnagContent;
class PlayerSystem;
class GameBoard;

// reconstructed from Ghidra:
//   ctor:           FUN_1000120ec
//   setVisual:      FUN_100012b5c (regular world tile)
//   setSnagVisual:  FUN_100012850 (snag/enemy encounter tile, also reused for
//                    the kind=1 start tile)
//   setHexUVs:      FUN_100012930 (computes UV from tileVariant/gridIdx)
//   setPosition:    FUN_100012c34 (writes posX/posY, propagates to sub-objects)
//   setRackPosition:FUN_100012d28 (rack-slot slide animation start)
//   finalizeStart:  FUN_100013410 (verifies snagContent alive)
//   getContentType: FUN_1000133b0 (returns content type if alive, else 0)
//   getSnagType:    FUN_1000133f0 (returns snag type if alive, else 0)
//   erase:          FUN_100013450 (kill+fade both contents, blanks the tile)
//   transformToSnag:FUN_1000135d0 (replaces snagContent with a new snag)
//   pushDecoration: FUN_100013870 (adds entry to the decorations list)
//
// TileObject is one hex on the playable grid, or one tile in the 5-rack at the
// bottom of the screen. it owns:
//   - the main hex Quad (the rendered face)
//   - a smaller icon Quad (overlay decoration on top of the face)
//   - hex-grid coordinates (col, row); screen XY is computed from these
//   - a TileContent* (0x178 bytes), for regular world tiles, holds
//     the "what's on this tile" data: type, variant, color tint.
//   - a SnagContent* (0x498 bytes), for snag (enemy) encounter tiles
//     with the recognisable atk/def/hp 3-stat fight UI. the same struct is
//     also allocated for the start tile (kind=1), where it acts as a non-fight
//     "portal" variant with special stat scaling per FUN_10003ccc8's
//     `if (uVar8 == 1)` branch; the 3 stat quads exist in memory but are
//     never shown as a fight because no combat is triggered on level entry.
//     also reused by transformToSnag to spawn an effect-driven snag.
//   - a small vector tracking SnagContent allocations so
//     that transformToSnag can replace snagContent without leaking the old one.
//   - a doubly-linked list of "decorations" added via pushDecoration
//     (small icons drawn over the tile to indicate atk-boost / def-boost / etc.)
//
// allocation: operator_new(0x248), constructed by FUN_1000120ec, populated
// either by FUN_100012b5c (world tile) + new TileContent or by
// FUN_100012850 (snag tile or kind=1 start tile) + new SnagContent.
//
// pushed onto the GameBoard's world tile list (for world tiles
// and the start tile) or stored in GameBoard.rack[5] (for rack tiles).
//
// the decorations std::list stores DecorationValue nodes (added by
// pushDecoration); see tile_decoration.h for that value's field layout.

class TileObject {
public:
    // ---- Phase 1 methods (decompiled, simple) ----

    // FUN_1000120ec: zero the struct, init the two Quads, set up the linked-
    // list sentinel, set the icon Quad's UV/size/alpha defaults.
    void init();

    // dtor, defined in tile_object.cpp where TileContent / SnagContent are
    // visible so the `delete content / snagContent` calls compile. not virtual
    // (see the comment block on draw() / update() below for why).
    ~TileObject();

    // FUN_100012c34: write posX/posY to mainQuad and propagate to iconQuad
    // (offset by DAT_100059e6c), the decoration list nodes, and both content
    // ptrs (via their MovableActor vtable[4] = setPosition).
    void setPosition(float x, float y);

    // FUN_10001349c: write gridCol/gridRow and propagate the same pair to
    // TileContent's and SnagContent's grid coords when those sub-actors are
    // alive. called by the page-commit path when a tile drops onto a hex.
    void setGridCoord(int col, int row);

    // FUN_100013410: returns snagContent if it's alive (visible byte set),
    // else 0. used by FUN_1000201e8 / FUN_10001ffd4 / FUN_100018180 to test
    // "is this an active snag/start tile?"
    SnagContent* getSnagIfAlive();

    // FUN_100013430: returns content if it's alive (visible byte set),
    // else 0. used by the post-pickup chain (FUN_100023ac4) to resync
    // the displayed magnitude after engagement, and by the tile-content
    // effect dispatcher (FUN_10003fb44) to look up the source TileContent.
    TileContent* getTileContentIfAlive();

    // FUN_1000133d0: returns content->displayedMagnitude when alive, else 0.
    // used by the post-commit dispatch in onPointerReleasedDuringDrag's
    // sub-path 3 (b) to read the current magnitude before doubling it on
    // a hex with HexMap kind == 7 (= "double-magnitude" cell).
    int getContentMagnitude();

    // FUN_1000133b0: returns content->type if content alive, else 0.
    int getContentType();

    // FUN_100014014: true while either the content or the snag is still
    // playing its scale-in animation (visible and scaleOutT < 1.0). the
    // ambient-hint cascade uses it to wait for a dropped tile to settle.
    bool isContentScaleAnimating() const;

    // FUN_1000134d8: propagate an alpha byte onto whichever of content
    // and snagContent is alive (via each one's vtable[5] setAlpha hook).
    // hex face stays opaque; the icon and stat displays dim. driven from
    // GameBoard::syncGlobalTileAlpha during pickup/rotate drag mode.
    void setTileAlpha(uint8_t alpha);

    // FUN_1000133f0: returns snagContent->type if snagContent alive, else 0.
    int getSnagType();

    // FUN_100012e0c: returns the number of open hex edges (2..5) for this
    // tile's (tileVariant, gridIdx). backed by kGridPoolTable[tileVariant]
    // [gridIdx].exitCount. snag-9 ("Tunnel Vision") uses this to gate
    // pickup: only tiles with exactly 2 exits (or the snag itself) can
    // be placed while it's in the rack.
    int getExitCount() const;

    // FUN_100012470: walk the decoration list and return true if any node
    // has the given `kind` and its `suppressed` flag is 0 (= visible /
    // active). used by tryPickupRackTile to detect tiles whose pickup is
    // gated by an active "effect-0" overlay.
    bool hasActiveDecorationOfKind(int kind) const;

    // FUN_100012d70: does this tile permit an exit edge toward `dir`
    // (0..5) given a base rotation `baseRot` (0..5)? `baseRot == -1` reads
    // the tile's own rotationStep. `mirror` flips the exit set (5 - exit).
    // backed by kGridPoolTable[tileVariant][gridIdx].exits.
    bool permitsDirection(int dir, int baseRot = -1) const;

    // FUN_100012844: write `degrees` to mainQuad.rotation and
    // iconQuad.rotation. used by the back-button
    // rotation drag to apply continuous rotation from finger movement
    // without going through the lerp animator.
    void setRotationDirect(float degrees);

    // FUN_100013bfc: walk the decoration list; for
    // each node whose `kind` matches and whose `suppressed` byte is 0,
    // set suppressed = 1 and alphaT = 0 (which kicks the alpha-fade-out
    // animation in the per-frame decoration walk inside update()). used
    // by the page-push hook + the post-resolve dispatch in
    // applyEndOfTurnPipeline.
    void suppressDecorationsOfKind(int kind);

    // FUN_100013c3c: return the first non-suppressed decoration of the
    // given `kind`'s `value` field. returns 0 when no matching
    // active decoration exists. consumed by HexMap dispatch's snag-2
    // ("Hound") damage tally: kind=2 decorations stash the damage stack
    // count, which the placed-tile dispatch reads then clears.
    int decorationValueOfKind(int kind) const;

    // ---- Phase 2 methods (set visuals, RNG, content alloc) ----

    // FUN_100012b5c: world-tile init. calls setHexUVs with RNG-rolled
    // mirror + rotationStep, then if contentType != 0 allocates a new
    // TileContent(0x178) and stores at this->content. `magnitude` is
    // the caller-supplied (FUN_100020450) per-content pool index, passed
    // to TileContent's ctor as its sub-type roll. not the same thing as
    // rotationStep above; this lives inside TileContent and selects which
    // sub-icon UV the content pulls from its lookup table.
    void setVisual(int gridLayout, int gridIdx, uint32_t contentType,
                   int magnitude);

    // FUN_100012850: snag-tile / encounter init. also used by populateRack's
    // hardcoded kind=1 generic-snag spawn. same setHexUVs as setVisual,
    // then allocates a SnagContent(0x498) populated with player-context
    // stats (totalTurnCount, levelTurnCount, worldIndex). stored at
    // this->snagContent and pushed into the tracking vector via
    // FUN_100012ad8.
    void setSnagVisual(int gridLayout, int gridIdx, uint32_t kind,
                       PlayerSystem* player, int levelTurnCount,
                       int pickupsFound, int worldIndex);

    // FUN_100013f04: rebuild a tile from saved-snapshot values (used by the
    // saved-game restore path FUN_100016b18 for rack / placed / reserve
    // tiles). unlike setVisual / setSnagVisual it takes explicit mirror /
    // rotationStep (no RNG roll) and explicit content / snag stats. allocates
    // a TileContent when contentType != 0 (replacing any existing one), a
    // SnagContent via SnagContent::initExplicit when snagFields[0] != 0
    // (attached via attachSnag), and replays each saved decoration via
    // pushDecoration.
    //   snagFields = {kind, hp, atk, def, extra0, extra1}; kind 0 = no snag.
    //   decorations = nullable; each int64 entry is (kind | value << 32).
    void setFromSnapshot(int gridLayout, int gridIdx, int mirror, int rotationStep,
                         uint32_t contentType, int contentMagnitude,
                         const uint32_t snagFields[6],
                         const std::vector<int64_t>* decorations,
                         PlayerSystem* player);

    // FUN_100012930: compute the main-hex UV and size from tileVariant/gridIdx.
    // internal helper, used by both setVisual and setSnagVisual.
    // `rotationStep` is the 0..5 hex-rotation index (rotation = step * 60deg).
    void setHexUVs(int gridLayout, int gridIdx, int mirror, int rotationStep);

    // FUN_100012d28: kick off rack slide-in. resets slideTimer=0,
    // slideStartPos=current, slideTargetPos=arg. delay is per-slot stagger.
    void setRackPosition(float slideDelay, const float* targetPos,
                         bool immediate, bool flag);

    // FUN_100013450: kill+fade both content and snagContent (sets visible=0,
    // seeds fade-out timer). the tile becomes "blank": hex face stays
    // visible, but the inner content/snag icon fades away. used by snag
    // effects that erase tile contents (Blinding Light / Loneliness / Zeal).
    void erase();

    // FUN_100013540: replace `content` with a freshly-allocated TileContent
    // of `(type, magnitude)`. dtors the old content (which fades it via the
    // vtable's deleting-dtor path), assigns the new content, and
    // calls revive() so it scale-pops in. used by snag-death effects that
    // mutate the killed snag's parent tile into a stat-boost tile (e.g.
    // PainfulMemory swap, snag-0x57 in FUN_100025dcc).
    void setTileContent(uint32_t type, int magnitude);

    // FUN_1000135d0: replace snagContent with a newly-spawned SnagContent
    // (size 0x498), tracked in the trackedContent vector. used when an effect turns a
    // tile into a snag (e.g. Parasite via Infestation, type 0x48).
    void transformToSnag(uint32_t snagKind, PlayerSystem* player,
                         int levelTurnCount, int pickupsFound, int worldIndex);

    // FUN_100013870: add (or update) a decoration sub-icon on this tile.
    //   kind == 0: simple icon (UV varies by tile content presence)
    //   kind == 1: another icon
    //   kind == 2: icon + ColorTint with a numeric value
    void pushDecoration(int kind, int value, int flag);

    // FUN_1000136bc: attach `sc` to this tile's snagContent slot. called
    // from SnagContent::update when a chased snag finishes its move
    // animation and needs to be reattached to its new parent tile. two
    // paths:
    //   - empty: snagContent is null or dead -> free any dead remnant,
    //     set snagContent = sc, push to trackedContent (dedup).
    //   - collision: snagContent already holds a live snag -> call
    //     FUN_10003df70 to merge / overlap, then delete the incoming sc.
    //     this is the rare "two snags on the same tile" case; the
    //     incoming one is consumed.
    void attachSnag(SnagContent* sc);

    // not virtual: TileObject begins with its Quad, and the binary's vtable[2]
    // dispatch goes straight to Quad::draw on the first 0xC8 bytes.
    // adding C++ virtuals here would inject a TileObject vtable at the start
    // and break the layout.

    // FUN_100012278: full per-tile draw. optional rotation matrix, optional
    // iconQuad overlay, hex face on tiles{1..4}.png, content layer
    // (drawContent helper), decoration list. drawTints controls SnagContent
    // digit-tint visibility downstream.
    void draw(bool drawContent, bool drawTints);

    // FUN_1000123c4: content layer of draw. early-out if an active kind=1
    // (Darkness "?") decoration is present, else draw content + tracked snags.
    void drawContent(bool drawTints);

    // FUN_1000124ac: per-tile per-frame update. rotation timer, slide
    // animation (cos-eased, calls setPosition), iconFadeT alpha lerp,
    // rotationLerpT, decoration list alpha, content/snagContent dispatch.
    void update(float dt);

    // ---- byte-exact fields ----

    // main Quad (the hex face). 0xD8 bytes; owns the trailing anim rect.
    Quad mainQuad;

    int gridLayout;              // (texture pool layout 0..11; not a board coord)
    int gridIdx;                 // (variant within layout 0..23)
    bool mirror;                 // (50% RNG roll -> horizontal mirror)
    int rotationStep;            // (RNG roll [0..5]; rotation = step * 60deg)
    int gridCol;                 // (placed-on-board hex column; 0 until commit)
    int gridRow;                 // (placed-on-board hex row;    0 until commit)

    // set to true when this tile is committed onto the page list
    //        (= placed on the hex grid by commitRackTilePickup). dispatch
    //        Hex Map PostCommit's snag-0x28 grow gate reads this to detect
    //        "the snag's parent tile is committed", which forces the
    //        Threat-token burst to animate from the GameBoard's positionXY
    //        instead of (0, 0). only set, never cleared (sticky flag).
    bool    committed;

    // small overlay icon Quad (0xD8; owns the trailing anim rect).
    Quad iconQuad;

    // iconQuad alpha-fade timer 0..1, rate dt/0.1s. fades 0->180 (slide
    // start) or 180->0 (slide finish). init 1 = idle.
    float iconFadeT;
    bool slideAnimActive;
    bool slideImmediate;
    bool slideFlag;
    float slideStartPos[2];
    float slideTargetPos[2];
    // slide timer 0..1, rate dt/0.2s, cos-eased. setRackPosition writes 0.
    float slideTimer;
    // per-slot slide-start delay (seconds). populateRack passes slot * 0.05.
    float slideDelay;
    // rotation-lerp endpoints; combat / event hooks set these (Phase C).
    float rotationLerpStart;
    float rotationLerpTarget;
    // rotation-lerp timer 0..1, rate dt/0.2s. drives mainQuad+iconQuad rotation.
    float rotationLerpT;

    TileContent* content;
    SnagContent* snagContent;

    // libc++'s std::vector<T*> matches the binary's 3-pointer triple
    // (begin, end, end_cap = 0x18 bytes); same trick we used for
    // PlayerSystem::perks. tracks SnagContent allocations so
    // transformToSnag can replace snagContent without leaking the previous one.
    std::vector<SnagContent*> trackedContent;

    // decorations stacked in front of the hex face, sorted ascending by `kind`
    // (pushDecoration inserts before the first node with a greater kind).
    std::list<DecorationValue> decorations;   // (0x18 bytes)

    // tile rotation timer 0..1. draw() rotates by rotationAnimT * 180deg.
    float rotationAnimT;
    // when true, update() drives rotationAnimT toward 1 (factor +1);
    // when false but rotationAnimT > 0, drives it toward 0 (factor -1).
    bool    rotationAnimActive;
};
