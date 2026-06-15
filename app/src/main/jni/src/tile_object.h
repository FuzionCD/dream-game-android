#pragma once

#include "quad.h"
#include "tile_decoration.h"
#include "tile_content.h"
#include <cstddef>
#include <cstdint>
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
//   finalizeStart:  FUN_100013410 (verifies +0x208 alive)
//   getContentType: FUN_1000133b0 (returns content+0x134 if alive, else 0)
//   getSnagType:    FUN_1000133f0 (returns snagContent+0x134 if alive, else 0)
//   erase:          FUN_100013450 (kill+fade both contents, blanks the tile)
//   transformToSnag:FUN_1000135d0 (replaces snagContent with a new snag)
//   pushDecoration: FUN_100013870 (adds entry to decoration list at +0x228)
//
// TileObject is one hex on the playable grid, or one tile in the 5-rack at the
// bottom of the screen. it owns:
//   - the main hex Quad (the rendered face)
//   - a smaller icon Quad (overlay decoration on top of the face)
//   - hex-grid coordinates (col, row); screen XY is computed from these
//   - a TileContent* (+0x200, 0x178 bytes), for regular world tiles, holds
//     the "what's on this tile" data: type, variant, color tint.
//   - a SnagContent* (+0x208, 0x498 bytes), for snag (enemy) encounter tiles
//     with the recognisable atk/def/hp 3-stat fight UI. the same struct is
//     also allocated for the start tile (kind=1), where it acts as a non-fight
//     "portal" variant with special stat scaling per FUN_10003ccc8's
//     `if (uVar8 == 1)` branch; the 3 stat quads exist in memory but are
//     never shown as a fight because no combat is triggered on level entry.
//     also reused by transformToSnag to spawn an effect-driven snag.
//   - a small vector at +0x210..+0x227 tracking SnagContent allocations so
//     that transformToSnag can replace +0x208 without leaking the old one.
//   - a doubly-linked list at +0x228 of "decorations" added via pushDecoration
//     (small icons drawn over the tile to indicate atk-boost / def-boost / etc.)
//
// allocation: operator_new(0x248), constructed by FUN_1000120ec, populated
// either by FUN_100012b5c (world tile) + new TileContent or by
// FUN_100012850 (snag tile or kind=1 start tile) + new SnagContent.
//
// pushed onto the GameBoard's world tile list at +0x96D8 (for world tiles
// and the start tile) or stored in GameBoard.rack[5] at +0x158 (for rack tiles).
//
// ---- byte map (0x248 bytes total, derived from FUN_1000120ec, 12930, 12c34,
// 12d28, 13870 access patterns) ----
//
//   +0x000..+0x0C7  Quad mainQuad           the hex face Quad (texture 8 / 9)
//   +0x0C8..+0x0D7  (gap, unused?)
//   +0x0D8          int tileVariant          texture pool index (0..11). FUN_100012930
//                                            uses (tileVariant, gridIdx) to pick the
//                                            UV cell on tiles{1..4}.png, and
//                                            FUN_100012d70 reads the same pair
//                                            against kGridPoolTable to determine
//                                            which 6-direction exits the variant
//                                            permits. not a board placement coord.
//   +0x0DC          int gridIdx             variant index within layout (0..23).
//                                            see tileVariant above.
//   +0x0E0          bool mirror             RNG-rolled in setVisual (50% chance);
//                                            triggers a horizontal-mirror swap
//                                            of the hex face's UV coordinates
//   +0x0E1..+0x0E3  (pad)
//   +0x0E4          int rotationStep        RNG-rolled [0..5] in setVisual.
//                                            controls in-engine rotation only:
//                                            mainQuad.rotation = step * 60deg.
//                                            FUN_100012d70 also reads it as the
//                                            base rotation when computing exit
//                                            permits.
//   +0x0E8          int gridCol             placed hex-grid column (set only by
//                                            FUN_10001349c when the tile commits
//                                            to the page list). stays 0 for tiles
//                                            sitting in the rack pre-pickup.
//                                            FUN_100020880 reads this to detect
//                                            cell occupancy on the board.
//   +0x0EC          int gridRow             placed hex-grid row (paired with
//                                            gridCol; 8 bytes total written
//                                            atomically by FUN_10001349c).
//   +0x0F0..+0x0F7  (zeroed by ctor)
//   +0x0F8..+0x1BF  Quad iconQuad           small overlay icon Quad
//                                            (UV 0.127x0.142, size 0.203x0.227,
//                                             alpha 0xB4)
//   +0x1C0..+0x1CF  (gap, unused?)
//   +0x1D0          float scale_1D0         init 1.0, overall scale modifier
//   +0x1D4          bool slideAnimActive    set by setRackPosition (rack slide-in)
//   +0x1D5          bool slideImmediate     setRackPosition param_4
//   +0x1D6          bool slideFlag          setRackPosition param_5
//   +0x1D7          (pad)
//   +0x1D8          float slideStartPos[2]  mirrored from mainQuad.pos at slide start
//   +0x1E0          float slideTargetPos[2] target position for rack slide
//   +0x1E8          float slideTimer        init 1.0; setRackPosition writes 0
//   +0x1EC          int   slideOffsetX      setRackPosition param_1
//   +0x1F0..+0x1F7  (gap)
//   +0x1F8          float scale_1F8         init 1.0, secondary scale modifier
//   +0x1FC          (pad)
//   +0x200          TileContent*  content       new(0x178); set by setVisual
//                                                when content type != 0
//   +0x208          SnagContent*  snagContent   new(0x498); set by setSnagVisual
//                                                (snag tile / kind=1 start tile)
//                                                or transformToSnag
//   +0x210..+0x227  vector<SnagContent*>        (begin, end, end_cap)
//                                                tracks 0x498-byte allocations
//                                                so they don't leak when
//                                                transformToSnag replaces +0x208
//   +0x228          DecorationNode* decorListHead   sentinel; init = self+0x228
//   +0x230          DecorationNode* decorListTail   sentinel; init = self+0x228
//   +0x238          int64_t        decorCount       0 at init
//   +0x240          int            field240         init 0; small counter
//   +0x244          bool           field244         init 0
//   +0x245..+0x247  (pad)
//
//   total: 0x248 bytes
//
// "decoration" linked list nodes (added by FUN_100013870):
//   +0x00  DecorationNode* next
//   +0x08  DecorationNode* prev
//   +0x10  int decorKind         (0, 1, or 2; distinguishes which sub-icon)
//   +0x18  Quad iconSubQuad      (small icon, UVs differ per kind)
//   +0xE0..+0x117  (extra state)
//   +0x100..+0x137 ColorTint     (only for kind==2)
//   +0x138  float subOffsetX
//   +0x13C  float subOffsetY
//   +0xF4   bool   suppressed
//   +0xF8   int    valueShown   (only for kind==2)
//   +0xFC   ?
//   +0x140  ?
// approx size >= 0x150. exact size needs phase-1 verification (FUN_100007df0
// is the dtor; FUN_1000140b8 is the allocate-and-link helper).

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

    // FUN_10001349c: write gridCol/gridRow to +0xE8/+0xEC and propagate the
    // same pair to TileContent (+0x200 -> its +0xE8) and SnagContent (+0x208
    // -> its +0xE8) when those sub-actors are alive. called by the page-commit
    // path when a tile drops onto a hex.
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

    // FUN_100012844: write `degrees` to mainQuad.rotation (+0xC0) and
    // iconQuad.rotation (+0x1B8 = iconQuad+0xC0). used by the back-button
    // rotation drag to apply continuous rotation from finger movement
    // without going through the lerp animator.
    void setRotationDirect(float degrees);

    // FUN_100013bfc: walk the decoration list (sentinel at +0x228); for
    // each node whose `kind` matches and whose `suppressed` byte is 0,
    // set suppressed = 1 and alphaT = 0 (which kicks the alpha-fade-out
    // animation in the per-frame decoration walk inside update()). used
    // by the page-push hook + the post-resolve dispatch in
    // applyEndOfTurnPipeline.
    void suppressDecorationsOfKind(int kind);

    // FUN_100013c3c: return the first non-suppressed decoration of the
    // given `kind`'s `value` field (+0xF8). returns 0 when no matching
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
    // this->snagContent and pushed into the tracking vector at +0x210 via
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
    // vtable's deleting-dtor path), pushes the new content into +0x200, and
    // calls revive() so it scale-pops in. used by snag-death effects that
    // mutate the killed snag's parent tile into a stat-boost tile (e.g.
    // PainfulMemory swap, snag-0x57 in FUN_100025dcc).
    void setTileContent(uint32_t type, int magnitude);

    // FUN_1000135d0: replace snagContent with a newly-spawned SnagContent
    // (size 0x498), tracked in the +0x210 vector. used when an effect turns a
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

    // not virtual: TileObject+0x000 is Quad, and the binary's vtable[2]
    // dispatch goes straight to Quad::draw on the first 0xC8 bytes.
    // adding C++ virtuals here would inject a TileObject vtable at offset 0
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

    // 0x000..0x0D7: main Quad (the hex face). 0xD8 bytes; owns anim rect at +0xC8.
    Quad mainQuad;

    int gridLayout;              // 0x0D8  (texture pool layout 0..11; not a board coord)
    int gridIdx;                 // 0x0DC  (variant within layout 0..23)
    bool mirror;                 // 0x0E0  (50% RNG roll -> horizontal mirror)
    uint8_t pad0E1[3];           // 0x0E1
    int rotationStep;            // 0x0E4  (RNG roll [0..5]; rotation = step * 60deg)
    int gridCol;                 // 0x0E8  (placed-on-board hex column; 0 until commit)
    int gridRow;                 // 0x0EC  (placed-on-board hex row;    0 until commit)

    // 0x0F0: set to true when this tile is committed onto the page list
    //        (= placed on the hex grid by commitRackTilePickup). dispatch
    //        Hex Map PostCommit's snag-0x28 grow gate reads this to detect
    //        "the snag's parent tile is committed", which forces the
    //        Threat-token burst to animate from the GameBoard's positionXY
    //        instead of (0, 0). only set, never cleared (sticky flag).
    bool    committed;           // 0x0F0
    uint8_t pad0F1[7];           // 0x0F1..0x0F7

    // 0x0F8..0x1CF: small overlay icon Quad (0xD8; owns anim rect at +0x1C0).
    Quad iconQuad;

    // iconQuad alpha-fade timer 0..1, rate dt/0.1s. fades 0->180 (slide
    // start) or 180->0 (slide finish). init 1 = idle.
    float iconFadeT;             // 0x1D0
    bool slideAnimActive;        // 0x1D4
    bool slideImmediate;         // 0x1D5
    bool slideFlag;              // 0x1D6
    uint8_t pad1D7;              // 0x1D7
    float slideStartPos[2];      // 0x1D8/0x1DC
    float slideTargetPos[2];     // 0x1E0/0x1E4
    // slide timer 0..1, rate dt/0.2s, cos-eased. setRackPosition writes 0.
    float slideTimer;            // 0x1E8
    // per-slot slide-start delay (seconds). populateRack passes slot * 0.05.
    float slideDelay;            // 0x1EC
    // rotation-lerp endpoints; combat / event hooks set these (Phase C).
    float rotationLerpStart;     // 0x1F0
    float rotationLerpTarget;    // 0x1F4
    // rotation-lerp timer 0..1, rate dt/0.2s. drives mainQuad+iconQuad rotation.
    float rotationLerpT;         // 0x1F8
    uint8_t pad1FC[0x4];         // 0x1FC..0x1FF

    TileContent* content;             // 0x200
    SnagContent* snagContent;         // 0x208

    // libc++'s std::vector<T*> matches the binary's 3-pointer triple
    // (begin, end, end_cap = 0x18 bytes); same trick we used for
    // PlayerSystem::perks. tracks SnagContent allocations so
    // transformToSnag can replace +0x208 without leaking the previous one.
    std::vector<SnagContent*> trackedContent;  // 0x210..0x227

    // doubly-linked list of TileDecoration nodes (0x140 bytes each, sized in
    // tile_decoration.h). sentinel = &decorListPrev. when empty,
    // decorListPrev == decorListNext == address of decorListPrev.
    // sorted ascending by `kind`; pushDecoration walks until the existing
    // first-greater-or-equal node.
    TileDecoration* decorListPrev;   // 0x228, sentinel.prev (newest)
    TileDecoration* decorListNext;   // 0x230, sentinel.next (oldest)
    int64_t         decorCount;      // 0x238, number of live decoration nodes

    // tile rotation timer 0..1. draw() rotates by rotationAnimT * 180deg.
    float rotationAnimT;             // 0x240
    // when true, update() drives rotationAnimT toward 1 (factor +1);
    // when false but rotationAnimT > 0, drives it toward 0 (factor -1).
    bool    rotationAnimActive;      // 0x244
    uint8_t pad245[3];               // 0x245..0x247
};

static_assert(sizeof(TileObject) == 0x248,
              "TileObject must be exactly 0x248 bytes to match the binary");
