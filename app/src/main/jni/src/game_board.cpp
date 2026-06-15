#include "game_board.h"
#include "item.h"
#include "color_tint.h"         // tickExitArrowFade, ColorTint::setAlpha
#include "dream_snippet_table.h" // level-start banner seed pool sizing
#include "event_table.h"        // EventKind, fireEvent's per-kind switch
#include "event_slot.h"         // EventSlot, eventTray snapshot field reads
#include "game.h"
#include "level_up_panel.h"
#include "perk.h"               // Perk, perks snapshot field reads
#include "random.h"             // rngInt, Phase 3 RNG helpers
#include "renderer.h"
#include "snag_content.h"       // rollRackTile reads SnagContent::consumedFlag
#include "tile_content.h"       // TileContent::setMagnitude, pickup magnitude resync
#include "tile_decoration.h"    // TileDecoration, snapshot decorations walk
#include "tile_grid_pool.h"     // kGridPoolTable, findGridIdxPool consumer
#include "tile_content_table.h" // kTileTextTable, DetailPanel content-name gate
#include <SDL.h>
#include <algorithm>            // std::clamp / std::remove_if, pool helpers
#include <cmath>                // std::cos / std::sqrt / std::fmod, Step 9 update helpers
#include <cstring>
#include <iterator>             // std::next, variant pool roll
#include <list>                 // std::list, commitCleanupQuadBatch involved-tiles list
#include <new>



namespace {

// FUN_100012f04(col, row, mode=1): pixel-snapped hex cell X position with
// half-cell offset. derived from disassembly + raw __TEXT,__const reads:
//   X = round((col * 0.196875 + row * -0.0984375 + -0.0875) * 640) / 640 + 0.0875
// the binary returns Y in s1 too (snap(row * 0.1704988 + -0.0984375) + 0.0984375),
// but Ghidra's decompile drops the s1 output. hexCellSnappedXY exposes both.
    struct HexCellPos { float x; float y; };

    HexCellPos hexCellSnappedXY(int col, int row) {
        constexpr float HEX_X_COL_STEP   =  0.196875f;       // DAT_100059e80
        constexpr float HEX_X_ROW_OFFSET = -0.098437503f;    // DAT_100059e84
        constexpr float HEX_Y_ROW_STEP   =  0.170498759f;    // DAT_100059e88
        constexpr float HEX_X_BASE_OFFS  = -0.0875f;          // DAT_100059e8c
        constexpr float HEX_X_OUT_OFFS   =  0.0875f;          // DAT_100059e90
        constexpr float HEX_Y_OUT_OFFS   =  0.098437503f;    // DAT_100059e94
        constexpr float SNAP_SCALE       =  640.0f;           // DAT_10005a72c (FUN_100057374)

        auto snap = [&](float v) {
            // FUN_100057374: round(value * SNAP_SCALE) / SNAP_SCALE.
            return (v >= 0.0f)
                   ? (float)(int)(v * SNAP_SCALE + 0.5f) / SNAP_SCALE
                   : (float)(int)(v * SNAP_SCALE - 0.5f) / SNAP_SCALE;
        };

        float rawX = (float)col * HEX_X_COL_STEP +
                     (float)row * HEX_X_ROW_OFFSET +
                     HEX_X_BASE_OFFS;
        // binary's pre-snap Y is `row * DAT_e88 + DAT_e84`; the inner
        // offset is DAT_e84 (= -0.0984375), the same constant used as ROW_OFFSET
        // in X. the matching outer offset DAT_e94 (= +0.0984375) cancels it at
        // (col,row)=(0,0), giving Y=0 for the origin cell.
        float rawY = (float)row * HEX_Y_ROW_STEP + HEX_X_ROW_OFFSET;

        return { snap(rawX) + HEX_X_OUT_OFFS, snap(rawY) + HEX_Y_OUT_OFFS };
    }


// -----------------------------------------------------------------------------
// hex-grid placement rules engine (FUN_100013060 / FUN_100013084 / FUN_10001325c
// / FUN_100013218 / FUN_10001209c).
//
// the chain answers "can the picked tile be placed at neighborCoord adjacent
// to the most recent page tile?" by:
//   1. requiring the picked tile to have an exit edge facing the lastPageTile
//      (= they connect into a single path).
//   2. forbidding any other tile in the page list from forming a connection to
//      this placement (= no branches: the path is strictly sequential).
//   3. forbidding a connection to the level-end target until the right number
//      of turns have elapsed.
// -----------------------------------------------------------------------------

// FUN_10001209c, hex-grid distance between two (col, row) points. uses a
// signed Manhattan distance with axial-coordinate adjustment for the cube-
// distance shape (= max of |dCol|, |dRow|, |dCol + dRow| in our layout).
    int hexGridDistance(int aCol, int aRow, int bCol, int bRow) {
        int dCol = aCol - bCol;
        int dRow = aRow - bRow;
        int absCol = (dCol < 0) ? -dCol : dCol;
        int absRow = (dRow < 0) ? -dRow : dRow;

        // signs same or exactly one is zero: cube-distance is sum of absolutes.
        if ((dCol < 1 || dRow < 1) && ((dRow & dCol) >= 0)) {
            return absCol + absRow;
        }

        return (absRow > absCol) ? absRow : absCol;
    }

// FUN_100013218, given two hex coords, compute the direction index
// (0..5) that points from `from` to `to`, then ask `tile->permitsDirection`
// at that direction with `baseRot`. returns true if the tile has an exit
// facing `to` from the `from` cell.
//
// direction lookup mirrors the binary's csel chain:
//   dCol=0  ->  dRow > 0 ? 0 : 3
//   dCol<0  ->  dRow < 0 ? 2 : 1
//   dCol>0  ->  dRow > 0 ? 5 : 4
    bool tilePermitsExitToward(const TileObject* tile,
                               int fromCol, int fromRow,
                               int toCol,   int toRow,
                               int baseRot) {
        int dCol = fromCol - toCol;
        int dRow = fromRow - toRow;

        int dir;

        if (dCol > 0) {
            dir = (dRow > 0) ? 5 : 4;
        } else if (dCol < 0) {
            dir = (dRow < 0) ? 2 : 1;
        } else {
            dir = (dRow > 0) ? 0 : 3;
        }

        return tile->permitsDirection(dir, baseRot);
    }

// FUN_10001325c, fit check at one rotation. returns 1 (fits) when:
//   - the picked tile has an exit edge toward the last page tile.
//   - if endCoord is set and adjacent: the picked tile does not have an
//     exit edge toward endCoord.
//   - for every other orthogonal-adjacent tile in the page list:
//       neither the picked tile (at this rotation) nor that other tile
//       has an exit edge toward each other (= no branching path).
//
// when there's no last tile (empty page list), returns 1 by default.
    int placementFitsAtRotation(const TileObject* picked,
                                int neighborCol, int neighborRow,
                                const TileObject* lastTile,
                                const std::list<TileObject*>& pageList,
                                const int* endCoord,
                                int rotation) {

        // binary checks `lastTile != 0` and `pageCount != 0`.
        if (!lastTile || pageList.empty()) {
            return 1;
        }

        if (endCoord) {
            int distToEnd = hexGridDistance(neighborCol, neighborRow,
                                            endCoord[0], endCoord[1]);

            if (distToEnd == 1 &&
                tilePermitsExitToward(picked, neighborCol, neighborRow,
                                      endCoord[0], endCoord[1], rotation)) {
                return 0;
            }
        }

        if (!tilePermitsExitToward(picked, neighborCol, neighborRow,
                                   lastTile->gridCol, lastTile->gridRow, rotation)) {
            return 0;
        }

        // walk the page list (oldest to newest); skip lastTile.
        for (const TileObject* other : pageList) {

            if (!other || other == lastTile) {
                continue;
            }

            int dCol = neighborCol - other->gridCol;
            int dRow = neighborRow - other->gridRow;

            // adjacency window: |dCol| <= 1 and |dRow| <= 1.
            int absCol = (dCol < 0) ? -dCol : dCol;
            int absRow = (dRow < 0) ? -dRow : dRow;

            if (absCol > 1 || absRow > 1) {
                continue;
            }

            // exclude the two diagonals that aren't valid hex neighbors
            // ((-1, +1) and (+1, -1) in our 6-direction grid).
            if ((dCol == -1 && dRow == 1) || (dCol == 1 && dRow == -1)) {
                continue;
            }

            // genuine adjacent tile: forbid any connection between picked and other.
            if (tilePermitsExitToward(picked, neighborCol, neighborRow,
                                      other->gridCol, other->gridRow, rotation)) {
                return 0;
            }

            if (tilePermitsExitToward(other, other->gridCol, other->gridRow,
                                      neighborCol, neighborRow,
                                      other->rotationStep)) {
                return 0;
            }
        }

        return 1;
    }

// FUN_100013084, find the best rotation 0..5 for placing `picked` at
// neighborCoord adjacent to lastPageTile. returns the chosen rotation, or
// -1 if no rotation passes placementFitsAtRotation.
//
// scoring: for each rotation that fits, prefer the one with smallest cyclic
// distance from `seedRotation`. callers with no orientation hint pass
// seedRotation=0 and dirHint=null, so the score collapses to
// `min(|r|, |r-6|, |r+6|)`: picks the lowest rotation index that fits.
    int findFittingRotation(const TileObject* picked,
                            int neighborCol, int neighborRow,
                            const TileObject* lastTile,
                            const std::list<TileObject*>& pageList,
                            const int* endCoord,
                            float seedRotation,
                            const float* dirHint) {

        constexpr float DIR_HINT_WEIGHT = 100.0f;  // DAT_100059eac

        int bestRot = -1;
        float bestScore = 0.0f;

        auto cyclicDist = [](float a, float b) {
            float d0 = (a - b)         < 0 ? -(a - b)         : (a - b);
            float d1 = (a - (b + 6.0f)) < 0 ? -(a - (b + 6.0f)) : (a - (b + 6.0f));
            float d2 = ((a + 6.0f) - b) < 0 ? -((a + 6.0f) - b) : ((a + 6.0f) - b);
            float m  = (d0 < d1) ? d0 : d1;
            return (d2 < m) ? d2 : m;
        };

        for (int rot = 0; rot < 6; rot++) {
            float score = cyclicDist((float)rot, seedRotation);

            if (dirHint) {
                float hintScore = DIR_HINT_WEIGHT;

                for (int qDir = 0; qDir < 6; qDir++) {

                    if (picked->permitsDirection(qDir, rot)) {
                        float d = cyclicDist((float)qDir, *dirHint);

                        if (d < hintScore) {
                            hintScore = d;
                        }
                    }
                }

                score = score + hintScore * DIR_HINT_WEIGHT;
            }

            bool considerThis = (bestRot < 0) || (score < bestScore);

            if (considerThis) {
                int fits = placementFitsAtRotation(picked, neighborCol, neighborRow,
                                                   lastTile, pageList,
                                                   endCoord, rot);

                if (fits != 0) {
                    bestRot   = rot;
                    bestScore = score;
                }
            }
        }

        return bestRot;
    }

// FUN_100013060, wrapper. true iff at least one rotation fits.
    bool canPlaceAtNeighbor(const TileObject* picked,
                            int neighborCol, int neighborRow,
                            const TileObject* lastTile,
                            const std::list<TileObject*>& pageList,
                            const int* endCoord) {
        return findFittingRotation(picked, neighborCol, neighborRow,
                                   lastTile, pageList,
                                   endCoord, 0.0f, nullptr) >= 0;
    }

// FUN_100012f04(col, row, mode=0): linear (un-snapped) hex grid to screen
// position. used when computing hint-quad positions and the rack-slide
// target during placement, where the tiny pixel-snap offset from the
// mode=1 form would shift the lerp endpoint by ~1 pixel.
HexCellPos hexCellLinearXY(int col, int row) {
    constexpr float HEX_X_COL_STEP   =  0.196875f;       // DAT_100059e80
    constexpr float HEX_X_ROW_OFFSET = -0.098437503f;    // DAT_100059e84
    constexpr float HEX_Y_ROW_STEP   =  0.170498759f;    // DAT_100059e88

    float x = (float)col * HEX_X_COL_STEP + (float)row * HEX_X_ROW_OFFSET;
    float y = (float)row * HEX_Y_ROW_STEP;
    return { x, y };
}

// FUN_100012ecc, current rotation in degrees, normalized [0, 360).
float tileCurrentRotationDegrees(const TileObject* tile) {
    constexpr float FULL_REV = 360.0f;          // DAT_100059e7c
    float r = std::fmod(tile->mainQuad.rotation, FULL_REV);

    if (r < 0.0f) {
        r += FULL_REV;
    }

    return r;
}

// FUN_100012e2c, kick a rotation lerp animation. seeds rotationLerpStart
// from the tile's current rotation (mod 360), seeds rotationLerpTarget
// from `targetDegrees`, with a wrap-around correction when the direct
// path between them is more than 180 degrees (so the short way crosses
// 0/360).
//
// constants used (verified against the binary's `b.le` flow):
//   DAT_100059e70 = 360.0  (full revolution)
//   DAT_100059e74 = 180.0  (half revolution: wrap-around threshold)
//   DAT_100059e78 = -360.0 (wrap-around adjust on one side)
void tileSetRotationLerp(TileObject* tile, float targetDegrees) {
    constexpr float FULL_REV    =  360.0f;
    constexpr float HALF_REV    =  180.0f;
    constexpr float WRAP_ADJUST = -360.0f;

    float current = std::fmod(tile->mainQuad.rotation, FULL_REV);

    if (current < 0.0f) {
        current += FULL_REV;
    }

    // when the direct angular distance is greater than 180 degrees, the
    // short way crosses 0/360. shift whichever endpoint is larger by -360
    // so the lerp goes the short way. when current <= target we shift the
    // target (it's the larger one); else we shift current.
    float diff = std::fabs(current - targetDegrees);

    if (diff > HALF_REV) {

        if (current <= targetDegrees) {
            targetDegrees = targetDegrees + WRAP_ADJUST;
        } else {
            current = current + WRAP_ADJUST;
        }
    }

    tile->rotationLerpStart  = current;
    tile->rotationLerpTarget = targetDegrees;
    tile->rotationLerpT      = 0.0f;

    // FUN_10001282c(tile, 1): start slide-anim if not already on; reset
    // iconFadeT for the icon fade-in.
    if (!tile->slideAnimActive) {
        tile->slideAnimActive = true;
        tile->iconFadeT       = 0.0f;
    }
}

// FUN_100012c10, set rotationStep (the integer 0..5) and optionally apply
// the corresponding angle to mainQuad.rotation + iconQuad.rotation. used
// as the "settle" step after the rotation lerp completes.
void tileSetRotationStep(TileObject* tile, int rotationStep, bool applyToQuads) {
    constexpr float DEG_PER_STEP = 60.0f;       // DAT_100059e68
    tile->rotationStep = rotationStep;

    if (applyToQuads) {
        float deg = (float)rotationStep * DEG_PER_STEP;
        tile->mainQuad.rotation = deg;
        tile->iconQuad.rotation = deg;
    }
}

// FUN_100057250, polar-to-rect on (radius, angle in radians). returns
// both components: s0 (= return) = radius*cos(angle), s1 (= secondary
// return) = radius*sin(angle). standard polar form: X = cos, Y = sin.
// Ghidra's decompile collapses the second register, but the disassembly
// shows the function ends with `mov v0, v2 (= radius*cos)` and leaves
// s1 = radius*sin live for the caller to read.
HexCellPos polarToRect(float radius, float angleRadians) {
    float s = std::sin(angleRadians);
    float c = std::cos(angleRadians);
    return { radius * c, radius * s };
}

// FUN_100057288, normalize two angles to a common period so their shortest
// arc distance can be computed by simple subtraction. wraps each angle to
// [0, 360); when their direct distance exceeds 180, shifts the smaller one
// up by 360 so the diff measures the short way.
void normalizeAnglePair(float& a, float& b) {
    constexpr float FULL_REV = 360.0f;          // DAT_10005a724
    constexpr float HALF_REV = 180.0f;          // DAT_10005a728
    a = std::fmod(a, FULL_REV);
    b = std::fmod(b, FULL_REV);
    float diff = std::fabs(a - b);

    if (diff > HALF_REV) {

        if (b <= a) {
            b += FULL_REV;
        } else {
            a += FULL_REV;
        }
    }
}

// FUN_100057310, shortest-arc absolute angular distance between two angles
// (in degrees). returns |a - b| measured along the short way around 360.
float angularDistance(float a, float b) {
    normalizeAnglePair(a, b);
    return std::fabs(a - b);
}

// FUN_100024a9c, build the list of rotations 0..5 that pass the placement
// rules engine for the given tile at its current grid coord. matches the
// binary's two end-gate semantics: skip the exit gate when the player has
// the keys (or is exactly one-from-end), otherwise feed the exit coord so
// the engine forbids early connection.
//
// inputs:
//   board       - GameBoard owning the page list / exit coord / key counts.
//   pickedTile  - tile at its committed (gridCol, gridRow). the function
//                 enumerates rotations 0..5 and asks placementFitsAtRotation
//                 whether each fits.
//   out         - pre-allocated vector to fill (caller manages storage).
void buildDirList(GameBoard* board, TileObject* pickedTile,
                  std::vector<int>& out) {
    out.clear();

    // mirror FUN_100020580's end-gate logic: pass the exit coord only if
    // we're not yet able to legally close (= keysCollected < keysRequired)
    // and the proposed cell isn't exactly 1-from-end.
    const int* endCoord = nullptr;
    int picCol = pickedTile->gridCol;
    int picRow = pickedTile->gridRow;
    int exitCol = board->exitCol;
    int exitRow = board->exitRow;

    if (board->keysCollected < board->keysRequired) {

        if (board->keysCollected + 1 < board->keysRequired) {
            endCoord = &board->exitCol;
        } else {
            int dist = hexGridDistance(picCol, picRow, exitCol, exitRow);

            if (dist != 1) {
                endCoord = &board->exitCol;
            }
        }
    }

    // lastTile = newest placed tile (= pageList.back()): the one the next
    // placement has to connect to.
    TileObject* lastTile = board->pageList.empty() ? nullptr : board->pageList.back();

    for (int rot = 0; rot < 6; rot++) {
        int fits = placementFitsAtRotation(pickedTile, picCol, picRow,
                                           lastTile, board->pageList,
                                           endCoord, rot);

        if (fits != 0) {
            out.push_back(rot);
        }
    }
}

}  // anonymous namespace



// reconstructed from FUN_100014ed8
GameBoard* GameBoard::create() {
    // the original uses operator_new, so we use new (constructors do run here)
    GameBoard* pBoard = new(std::nothrow) GameBoard();

    if (!pBoard) {
        SDL_Log("Failed to allocate GameBoard struct");
        return nullptr;
    }

    // `new GameBoard()` with parens triggers value-initialization: POD pad
    // arrays get zeroed automatically, and non-POD members (Quad, std::vector,
    // std::map, ColorTint, etc.) run their default ctors. matches the binary's
    // end state (FUN_100014ed8 explicitly inits every field) without our
    // earlier memset workaround that was trampling container internals.

    // --- core state ---
    pBoard->visible = false;
    pBoard->exitRequested = false;
    pBoard->scoreRequested = false;
    pBoard->dirty = false;
    pBoard->saveNewSettings = false;
    pBoard->seVolume  = 0.5f;
    pBoard->bgmVolume = 0.5f;
    pBoard->state = 1;

    // --- title quad (0x48) ---
    pBoard->titleQuad.setTexCoords(0.0f, 0.84375f, 0.625f, 1.0f);
    pBoard->titleQuad.setSize(1.0f, 0.25f);

    float virtualHeight = Renderer::getVirtualHeight();
    pBoard->titleQuad.posX = 0.5f;
    pBoard->titleQuad.posY = virtualHeight - pBoard->titleQuad.height * 0.5f;

    // --- rack[5] (0x158): null until populateRack runs in initLevelContent ---
    for (int i = 0; i < RACK_SLOT_COUNT; i++) {
        pBoard->rack[i] = nullptr;
    }

    // --- rack-render-order list at 0x180 (populated below with 5 entries) ---
    // std::list default-constructs to empty (self-aliased sentinel); no
    // explicit init needed. matches the binary's FUN_100008894 empty-state
    // (head = tail = &head; count = 0).
    pBoard->selectedRackSlot = -1;

    // page list (0x1A8), tile reserve queue (0x96D8), stat tween (0x9650),
    // action queue (0x9670), discard slide (0x96A0): all std::list members,
    // default-constructed to empty by `new GameBoard()` above. matches the
    // binary's empty-state self-aliasing sentinel exactly.
    pBoard->statTweenAnyAnim = false;

    // --- tab quads (0x1D0, 6 entries, each TileIcon 0xD8) ---
    // UV / size from FUN_100014ed8's per-tab init loop:
    //   u0 = 0.12793, v0 = 0, u1 = 0.27441, v1 = 0.15820
    //   size = (0.234375, 0.253125)
    for (int i = 0; i < MAX_CURSOR_COUNT; i++) {
        pBoard->tileCursorQuads[i] = TileIcon();
        pBoard->tileCursorQuads[i].quad.setTexCoords(0.128, 0.0f, 0.2744, 0.1582f);
        pBoard->tileCursorQuads[i].quad.setSize(0.234375f, 0.253125f);
        pBoard->tileCursorVisible[i] = false;
        pBoard->tileCursorState[i] = false;
    }

    // --- navigation/button quads (TileIcons 0xD8 each) ---
    pBoard->navArrowQuad = TileIcon();
    pBoard->navArrowQuad.quad.setTexCoords(0.083f, 0.0f, 0.181f, 0.098f);
    pBoard->navArrowQuad.quad.setSize(0.15625f, 0.15625f);

    pBoard->backButtonQuad = TileIcon();
    pBoard->backButtonQuad.quad.setTexCoords(0.0f, 0.0f, 0.082f, 0.193f);
    pBoard->backButtonQuad.quad.setSize(0.13125f, 0.309375f);

    pBoard->xButtonQuad = TileIcon();
    pBoard->xButtonQuad.quad.setTexCoords(0.904f, 0.533f, 1.0f, 0.629f);
    pBoard->xButtonQuad.quad.setSize(0.153125f, 0.153125f);

    pBoard->xButtonVisible = false;

    // exit-arrow Quad UV / size / vertex-offset setup. FUN_100014ed8 does
    // this right after the title quad; the per-frame fade tick + rack-pickup
    // configure rely on it being in place. addVertexOffset shifts all four
    // vertices up by 0.028 so the rotation pivot sits at the arrow's base
    // (the tip points wherever rotation aims, not the center).
    pBoard->exitArrowQuad.setTexCoords(0.226563f, 0.467773f, 0.299805f, 0.576172f);
    pBoard->exitArrowQuad.setSize(0.117188f, 0.173438f);
    pBoard->exitArrowQuad.addVertexOffset(0.0f, -0.028125f);

    // exit-arrow digit ColorTint init. seeds the per-digit color to white
    // (0xFFFFFFFF) so the fade-tick's setAlpha calls modulate a visible
    // color. without this the value-initialized 0x00000000 leaves the
    // digits transparent even when alpha ramps up.
    pBoard->exitArrowDigit.init();

    // full-screen dim Quad init. matches FUN_100014ed8: size 2.0 x 2.0
    // (large enough to cover the visible area at any aspect), centered at
    // (0.5, vh * 0.5), color (0, 0, 0, 255). the case-10 fade tick uses
    // setAlpha to ramp 0..255 keeping RGB at 0, so the result blends to
    // black.
    pBoard->dimQuad.setSize(2.0f, 2.0f);
    pBoard->dimQuad.setColor(0, 0, 0, 255);
    pBoard->dimQuad.posX = 0.5f;
    pBoard->dimQuad.posY = virtualHeight * 0.5f;

    // pBoard position: centered on screen (from FUN_1000165e8)
    pBoard->positionX = 0.5f;
    pBoard->positionY = virtualHeight * 0.5f;

    // Nemesis renderable init (FUN_1000088f0 in FUN_100014ed8)
    pBoard->nemesis.init();

    // detail / dialog panel init (FUN_10003e5e0 + FUN_1000404e8). without
    // these the panels' fadeT / fadeProgress sit at value-init 0.0 instead
    // of 1.0, which makes the draw gate `(!visible && fade >= 1.0)` evaluate
    // false and the panels render their default-state quads each frame.
    pBoard->detailPanel.init();
    pBoard->dialogPanel.init();

    // gameplay HUD init (FUN_10000b160 in FUN_100014ed8). the binary passes
    // the DetailPanel ptr (GameBoard+0x4408) as the parent. the HUD only
    // stores it for read-back; doesn't dereference until Phase C.
    pBoard->hud.init(((uint8_t*)pBoard) + 0x4408);

    // PlayerSystem init (FUN_100056138, called from FUN_100014ed8). the
    // binary passes a pointer-to-zero, so the parent field ends up null and
    // is never read; subsystems use getGame() to reach the Game struct.
    pBoard->playerSystem.init();

    // LevelUpPanel ctor (FUN_10002c230). populates each stat slot's
    // TileContent icon + 2 9-slice background Labels + numberTint, sets the
    // panel's anchorX/Y, and lays out both the stat slots and the perk slots
    // (init() phases 3 + 6).
    pBoard->levelUpPanel.init();
    pBoard->itemChoicePanel.init(&pBoard->detailPanel);
    pBoard->eventChoicePanel.init(&pBoard->detailPanel);

    // UserStatsPanel ctor (FUN_100009a00). lays out the backdrop, the
    // 9-slice header, the 3 stat rows (ATK / HP / DEF) and the bottom
    // World/Level/Items strip. opens later via FUN_10000a9f4 when the
    // HUD's top-left player-icon button is tapped (publishedState == 1).
    pBoard->userStatsPanel.init(&pBoard->detailPanel);

    // PauseMenu ctor (FUN_10002e838). sets up Menu chrome, 3 tabs, 2 info
    // rows, 2 standalone icon Quads, and chains to the embedded
    // ForfeitConfirmPanel. opens later via FUN_10002f688 when the player
    // taps the HUD's menu icon.
    pBoard->pauseMenu.init();

    // AchievementBanner ctor (FUN_10004efec). adds panel-frame + divider
    // 9-slice chrome glyphs, sets sparkle deco UV / size, styles title +
    // description TextItems. opens later when AchievementTracker pops a
    // pending unlock from the GameBoard::update prologue.
    pBoard->achievementBanner.init();

    // --- build the rack-render-order list at +0x180 ---
    // the original pushes 5 entries with slot indices 0..4. each
    // GameBoard::draw walks this list and dereferences `rack[slot]` to
    // draw rack tiles in their layered order.
    for (int i = 0; i < RACK_SLOT_COUNT; i++) {
        pBoard->rackOrder.push_back(i);
    }

    // tileWeightPool at +0x9688: zero-init (FUN_10004cfe0) followed by 5
    // push calls (FUN_10004cfec at 100015460..1000154ac in the binary's ctor).
    // typeIds + initial weights:
    //   2 (DEF) = 30   3 (ATK) = 30   6 (HP) = 10   5 (CTRL) = 15   1 (snag) = 15
    // sums to 100. type 0 (blank) and type 4 (XP) are deliberately absent:
    // blanks come from specific snag mechanics (Wretch / Neglect / etc.) and
    // XP drops only on snag kill.
    pBoard->tileWeightPool.push_back({2, 30, 30});
    pBoard->tileWeightPool.push_back({3, 30, 30});
    pBoard->tileWeightPool.push_back({6, 10, 10});
    pBoard->tileWeightPool.push_back({5, 15, 15});
    pBoard->tileWeightPool.push_back({1, 15, 15});

#ifdef DEBUG_FAST_CTRL_ENABLED
    // DEBUG: pre-install 2 events into the HUD tray so Phase E3 can be
    // visually verified before the events panel (E4) and firing (E5)
    // land. one Attack-key event (charges per {A} tile placed) + one
    // Defence-key event (charges per {D} tile placed); both pick up
    // charges through the already-wired applyTileTypeEffect commit path.
    // delete with the rest of the DEBUG_FAST_CTRL block when shipping.
    {
        EventSlot* dbgEvent1 = new EventSlot();
        dbgEvent1->init(/*eventType=*/(int)EventKind::HardWork, /*magnitudeBase=*/0);   // SkeletonKey (Attack, chargesMax=5)
        pBoard->hud.addEventSlot(dbgEvent1);

        EventSlot* dbgEvent2 = new EventSlot();
        dbgEvent2->init(/*eventType=*/(int)EventKind::Backtrack, /*magnitudeBase=*/0);   // HardenedShell (Defence, chargesMax=5)
        pBoard->hud.addEventSlot(dbgEvent2);

        EventSlot* dbgEvent3 = new EventSlot();
        dbgEvent3->init(/*eventType=*/(int)EventKind::Countdown, /*magnitudeBase=*/0);   // HardenedShell (Defence, chargesMax=5)
        pBoard->hud.addEventSlot(dbgEvent3);

        EventSlot* dbgEvent4 = new EventSlot();
        dbgEvent4->init(/*eventType=*/(int)EventKind::IdyllicLandscape, /*magnitudeBase=*/0);   // HardenedShell (Defence, chargesMax=5)
        pBoard->hud.addEventSlot(dbgEvent4);


    }
#endif

    SDL_Log("GameBoard::create() complete (state=%d, virtualHeight=%.3f)", pBoard->state, virtualHeight);
    return pBoard;
}

// reconstructed from FUN_100015c1c, full GameBoard destructor.
//
// the binary walks the four owning linked lists and the rack array, freeing
// every held tile, then runs sub-object destructors in reverse-construction
// order. C++'s `delete this` runs the embedded-sub-object destructors for us
// (Quad arrays, Nemesis, DetailPanel, DialogPanel, GameplayHUD, PlayerSystem,
// StatBars, TileWeightPool/gameplayItems vectors, HexMap std::map,
// AnimationController vectors), so we only need to handle the raw-pointer
// fields manually: rack[0..4], the page/decoration/reserve linked-list nodes,
// and the rack-order list nodes.
void GameBoard::destroy() {
    // 1. rack[0..4]: delete each held TileObject (binary lVar3 = 0x158 loop).
    for (int i = 0; i < RACK_SLOT_COUNT; i++) {
        if (rack[i] != nullptr) {
            delete rack[i];
            rack[i] = nullptr;
        }
    }

    // 2. page list at +0x1A8: each entry is a TileObject* owned by us.
    // delete each held tile; the std::list's nodes are freed by its own
    // dtor when GameBoard is destroyed.
    for (TileObject* tile : pageList) {

        if (tile) {
            delete tile;
        }
    }

    pageList.clear();

    // 3. discard-slide list at +0x96A0: tiles in flight when destroy()
    // runs are still held by these entries; delete each before clearing.
    for (DiscardSlideBody& entry : discardSlide) {

        if (entry.tile) {
            delete entry.tile;
        }
    }

    discardSlide.clear();

    // 3a. stat-change tween queue at +0x9650: each entry owns a ColorTint
    // by value (auto-freed by TweenBody's implicit dtor cascade when clear
    // pops them). matches FUN_100029704 in the binary's GameBoard dtor.
    statTween.clear();
    statTweenAnyAnim = false;

    // 3b. action queue at +0x9670: each entry owns a std::vector<TileIcon>
    // (auto-freed by ActionBody's implicit dtor). matches FUN_100029774.
    actionQueue.clear();

    // 4. tile reserve queue at +0x96D8: each entry owns a TileObject*.
    // matches FUN_10002899c plus the earlier inner walk that deletes each
    // held tile.
    for (TileReserveEntry& entry : tileReserve) {

        if (entry.tile) {
            delete entry.tile;
        }
    }

    tileReserve.clear();

    // 5. rack-order list at +0x180 (FUN_100008894): entries are just slot
    // indices, no owned pointers. clear pops them all.
    rackOrder.clear();

    // 6. delete this: C++ value-destruction fires every embedded sub-object's
    // dtor in reverse declaration order (matches the binary's cascade).
    delete this;
}

// reconstructed from FUN_1000184e4, idle-state reset.
//   FUN_10003a408(this + 0x9d48);   // animController.reset()
//   *(int*)(this + 0x18) = 1;        // state = 1
void GameBoard::handleIdle() {
    animController.reset();
    state = 1;
}

// FUN_100018a80, predicate for drawTints: fetch the tile's live snag, then
// defer to FUN_100026878 for the per-kind decision.
bool GameBoard::shouldDrawTintsFor(TileObject* tile) {

    if (!tile) {
        return true;
    }

    SnagContent* sc = tile->getSnagIfAlive();   // FUN_100013410

    if (sc == nullptr) {
        return true;
    }

    // FUN_100026878: a committed parent or a Myopia snag always shows tints; a
    // Lie snag ("can't see its stats") hides them; otherwise tints hide iff a
    // Myopia snag sits in the rack (Myopia blanks every other snag's stats).
    bool committed = sc->tileParent && sc->tileParent->committed;

    if (committed || sc->type == (int)SnagKind::Myopia) {
        return true;
    }

    if (sc->type == (int)SnagKind::Lie) {
        return false;
    }

    return findSnagInRack((int)SnagKind::Myopia) == nullptr;
}

// port of FUN_100018514: full GameBoard render. step labels (8.1..8.22)
// match the binary's branch order; every sub-system draws with its own
// visible/empty early-out, so an empty board collapses to a clean no-op.
void GameBoard::draw() {

    if (!visible) {
        return;
    }

    glPushMatrix();
    glTranslatef(positionX, positionY, 0.0f);

    // 8.1 transient overlay text (animController.draw). early-outs when
    //     timer1 <= 0 or timer2 >= 1; both sit at 0 when idle, so no-op.
    animController.draw();

    bindTexture(9);

    // 8.2 dynamic gameplay items vector (binary's +0x96B8 vector + +0x96D0
    //     live count). before any items spawn, liveCount = 0 so the loop is empty.
    if (gameplayItemsLiveCount > 0 && !gameplayItems.empty()) {

        for (int64_t i = 0; i < gameplayItemsLiveCount &&
                            i < (int64_t)gameplayItems.size(); ++i) {
            gameplayItems[(size_t)i].quad.draw();
        }
    }

    // 8.3 page-list head tile + Nemesis interlock dispatch.
    //
    // mirrors the binary's mid-section of FUN_100018514. structure depends
    // on pageCount + nemesis.eatStep / nemesis.eatFired:
    //
    //   pageCount == 0:    drewSegments = nemesis.visible. no head draw.
    //   pageCount > 0:
    //     eatStep == 0:    oldest.draw(showContent=0, drawTints=1);
    //                      nemesis.drawSegments();
    //                      oldest.drawContent(drawTints=1);  // Darkness-?
    //                      drewSegments = false.
    //     eatStep == 1:    if eatFired: same as eatStep==0 (hide content,
    //                                   draw segments, drawContent override).
    //                      else !eatFired:
    //                          oldest.draw(showContent=1, drawTints=1);
    //                          if pageCount == 1: bind tex 8, playerSystem
    //                              .draw, bind tex 9, nemesis.drawSegments,
    //                              drewSegments = true;
    //                          else: nemesis.drawSegments, drewSegments=false.
    //     eatStep >= 2:    oldest.draw(showContent=1, drawTints=1);
    //                      same pageCount==1 vs >1 split as eatStep==1
    //                      eatFired-clear case above.
    //
    // the pageCount==1 sub-branch z-orders the avatar earlier (between
    // the head tile and the hex-map indicator) so the rest of the UI can
    // overlay him. with pageCount > 1 the avatar draws late (gated by
    // !drewSegments at step 8.12).
    bool drewSegments = false;
    const size_t pageCount = pageList.size();

    if (pageCount == 0) {
        drewSegments = nemesis.visible;
    } else {
        TileObject* oldest = pageList.front();
        int  interlock     = nemesis.eatStep;
        bool flag43FC      = nemesis.eatFired;

        // eat state to head draw params:
        //   eatStep == 0              : (hidden, segs+content)
        //   eatStep == 1, eatFired    : (hidden, segs+content)
        //   eatStep == 1, !eatFired   : (visible, pageCount==1-aware)
        //   eatStep >= 2              : (visible, pageCount==1-aware)
        bool hideContent = (interlock == 0) ||
                           (interlock == 1 && flag43FC);

        if (hideContent) {
            // hex face only, then nemesis body, then explicit drawContent
            // (the latter handles the kind=1 Darkness-? decoration override:
            // a kind=1 decoration with suppressed=0 returns early without
            // drawing the underlying content sprite).
            if (oldest) {
                oldest->draw(/*showContent=*/false, /*drawTints=*/true);
            }

            nemesis.drawSegments();

            if (oldest) {
                oldest->drawContent(/*drawTints=*/true);
            }

            drewSegments = false;
        } else {
            // full hex + content, then either avatar-early (pageCount==1)
            // or just segments (pageCount > 1).
            if (oldest) {
                oldest->draw(/*showContent=*/true, /*drawTints=*/true);
            }

            if (pageCount == 1) {
                // early avatar draw: z-orders the character on top of the
                // first committed tile, before the rest of the UI overlays.
                bindTexture(8);
                playerSystem.draw();
                bindTexture(9);
                nemesis.drawSegments();
                drewSegments = true;
            } else {
                nemesis.drawSegments();
                drewSegments = false;
            }
        }
    }

    bindTexture(9);

    // 8.4 hex grid map: cells the character has visited / can see + the
    //     exit/key markers added at level start. binary passes the oldest
    //     tile's (col, row) (the cell Nemesis sits on) so hexMap.draw
    //     skips it: Nemesis is there and obscures any marker. an empty page
    //     list (pageCount == 0) collapses to (0, 0).
    int  oldestCol = 0;
    int  oldestRow = 0;

    if (pageCount > 0) {
        const TileObject* oldest = pageList.front();

        if (oldest) {
            oldestCol = oldest->gridCol;
            oldestRow = oldest->gridRow;
        }
    }
    hexMap.draw(oldestCol, oldestRow);

    // 8.5 exit tile + lock icons. binary gates these on "oldest tile is
    //     not on the exit hex": once the player commits a tile at the
    //     exit, the exit visual disappears (the player's tile sits there).
    if (oldestCol != exitCol || oldestRow != exitRow) {
        exitTileIcon.quad.draw();

        for (int i = 0; i < keysRequired; i++) {
            exitLockIcons[i].quad.draw();
        }
    }

    // 8.6 rest of page list: everything except the oldest, which was
    //     drawn above (step 8.3). an empty page list (pageCount=0) skips this.
    if (pageCount > 1) {
        auto it = pageList.begin();
        ++it;   // skip oldest (front)

        for (; it != pageList.end(); ++it) {
            TileObject* t = *it;

            if (t) {
                t->draw(/*showContent=*/true, /*drawTints=*/true);
            }
        }
    }

    // 8.7 tab buttons on tex 8. when idle: all tileCursorVisible[] = false.
    bindTexture(8);

    for (int i = 0; i < MAX_CURSOR_COUNT; i++) {

        if (tileCursorVisible[i]) {
            tileCursorQuads[i].quad.draw();
        }
    }

    // 8.8 Nemesis foreground (drawFrame: outline + level number tint).
    nemesis.drawFrame();

    // 8.9 selected rack tile (drawn over everything else when grabbed).
    //     when nothing is grabbed: selectedRackSlot = -1, skip.
    if (selectedRackSlot != -1 && rack[selectedRackSlot]) {
        TileObject* selTile = rack[selectedRackSlot];
        bool drawTints = shouldDrawTintsFor(selTile);
        selTile->draw(/*showContent=*/true, drawTints);
    }

    // 8.10 exit-arrow visualization. binary gates the draw on
    //      `*(float *)(+0x9C64) > 0`: the per-frame fade timer (separate
    //      from the visible byte at +0x9C60, which updateExitArrowVisualState
    //      sets but does not ramp). tickExitArrowFade ramps exitArrowFade each
    //      frame, so the arrow fades in after pickup.
    if (exitArrowFade > 0.0f) {
        bindTexture(9);
        exitArrowQuad.draw();
        exitArrowDigit.draw();
    }

    // 8.11 stat bars row (StatBars at +0x8420). when idle: visible = 0.
    statBars.draw();

    // 8.12 character token. binary skips this when nemesis already drew
    //      its segments above (drewSegments == true): the segments wrap
    //      the character so re-drawing him would overlay them.
    if (!drewSegments) {
        bindTexture(8);
        playerSystem.draw();
    }

    bindTexture(9);

    // 8.13 selected-rack-slot info icons (back button + nav arrow). when
    //      nothing is selected: selectedRackSlot = -1, skip.
    if (selectedRackSlot != -1) {

        // hide nav arrow during back-button rotation drag (state 1) so the
        // rotation gesture isn't visually competing with the directional
        // picker.
        if (navDragState != 1) {
            navArrowQuad.quad.draw();
        }

        // back button only shows when no rack-snag of kind 0x31 or 0x43
        // is present (= no "Back" suppressor active).
        if (findSnagInRack(0x31) == nullptr &&
            findSnagInRack(0x43) == nullptr) {
            backButtonQuad.quad.draw();
        }
    }

    // 8.14 status icon overlay. when idle: xButtonVisible = false.
    if (xButtonVisible && state == 1) {
        xButtonQuad.quad.draw();
    }

    glPopMatrix();

    // 8.15 full-screen dim overlay. binary draws this on no-texture (tex 0)
    //      whenever dimProgress > 0 (i.e. mid-transition). the gate is the
    //      binary's `fVar11 != 0 && !signbit(fVar11)` which is just > 0.
    if (dimProgress > 0.0f) {
        bindTexture(0);
        dimQuad.draw();
    }

    // 8.16 title bar quad (titleQuad), drawn outside the matrix transform.
    bindTexture(9);
    titleQuad.draw();

    // 8.17 discard-slide list at +0x96A0: TileObjects mid-discard, drawn
    //      on top of the rack so they slide cleanly over it. each tile sits
    //      in this list for ~0.3s while its slide timer animates from 0 to
    //      1, then gets deleted. when idle: empty list.
    for (DiscardSlideBody& entry : discardSlide) {

        if (entry.tile) {
            bool drawTints = shouldDrawTintsFor(entry.tile);
            entry.tile->draw(/*showContent=*/true, drawTints);
        }
    }

    // 8.18 rack-order list at +0x180: walks the slot indices in stacking
    //      order, drawing rack[slot] for each slot that's not the selected
    //      one (selectedRackSlot already drew above on top).
    for (int slot : rackOrder) {

        if (slot >= 0 && slot < RACK_SLOT_COUNT && rack[slot] &&
            slot != selectedRackSlot) {
            TileObject* rt = rack[slot];
            bool drawTints = shouldDrawTintsFor(rt);
            rt->draw(/*showContent=*/true, drawTints);
        }
    }

    // 8.19 gameplay HUD (FUN_10000c004): its quads are screen-space, drawn
    //      after the matrix pop in the binary.
    hud.draw();

    bindTexture(9);

    // 8.20 draw the discard-staging queue (hud.pendingDiscards). visible
    //      iff staged == 0 (= still available for player to select).
    //      tapping a rack tile flips staged to 1, hiding the preview and
    //      queuing the underlying rack slot for the next commit.
    for (DiscardEntry& e : hud.pendingDiscards) {

        if (e.staged == 0) {
            e.quad.draw();
        }
    }

    // 8.21 sub-collections at +0x9650 (stat-change tweens) and +0x9670
    //      (action queue). both walks early-out when idle (empty
    //      lists / anyAnimating=false). caller (us) has tex 9 bound
    //      already from bindTexture(9) above.
    drawStatTween();           // FUN_10002be84
    drawActionQueue();         // FUN_100038440

    // 8.22 sub-screen draws. order matches binary FUN_100018514 tail
    // at 0x100018a08..0x100018a64:
    //   pauseMenu -> tileInspect -> eventChoice -> levelUp -> itemChoice
    //   -> detailPanel -> dialogPanel -> achievementBanner.
    // each panel has a visible-flag early-out so an idle board naturally
    // skips them. achievementBanner draws last so it overlays everything.
    pauseMenu.draw();             // FUN_10002f0cc (GameBoard+0xF488)
    userStatsPanel.draw();        // FUN_10000a3dc (GameBoard+0xA3F8)
    eventChoicePanel.draw();      // FUN_10000e6d4 (GameBoard+0xB2A8)
    levelUpPanel.draw();          // FUN_10002cd20 (GameBoard+0xC7B8)
    itemChoicePanel.draw();       // FUN_1000349ac (GameBoard+0xDEF0)
    detailPanel.draw();           // FUN_10003ef28 (GameBoard+0x4408)
    dialogPanel.draw();           // FUN_100040e9c (GameBoard+0x54B8)
    achievementBanner.draw();     // FUN_10004f3b8 (GameBoard+0x9E18)
}

// =============================================================================
// Step 9 helpers: port of FUN_100018ac8's 30+ sub-routines.
//
// every helper below carries its FUN_X anchor in the comment header. the cpp
// flow mirrors the binary's branch order, sub-screen guard cascade, and main
// state machine. helpers grouped here in the same order they get invoked from
// GameBoard::update at the bottom of this file.
// =============================================================================

// FUN_100018ac8 inline section (top): pop the next pending achievement
// unlock and open a banner for it. fires only when achievementBanner is
// idle (resetTimer < 0).
void GameBoard::showNextAchievementBanner() {

    if (!(achievementBanner.resetTimer < 0.0f)) {
        return;
    }

    Game* g = getGame();

    if (!g) {
        return;
    }

    const uint32_t idx = g->achievementTracker().popNextUnlock();

    if (idx != 50) {
        achievementBanner.open(idx);
    }
}

// FUN_10001980c, per-frame ambient-hint dispatcher.
//
// tutorialFlag = 0: dismiss any open popup (DialogPanel::reset(1)) and bail.
// tutorialFlag != 0:
//   1. tick the active popup (DialogPanel::update); while it's still visible,
//      return (it modally owns the frame).
//   2. return if the popup hasn't finished its previous fade (fadeProgress,
//      gb+0x63E0, < 1.0); don't stack a new hint on an in-flight one.
//   3. walk the 24-branch priority cascade: the first hint whose `hintShown`
//      flag is clear and whose live board condition holds wins; it computes
//      the anchor, marks itself shown + anyHintFiredThisFrame + dirty, and
//      shows the popup via DialogPanel::showHint.
//
// touchInput is a decompiler artifact (FUN_10001980c's apparent 2nd float arg
// is a dropped paired-return register, never a real parameter).
void GameBoard::tickAmbientPickupHinting(float dt, float touchInput) {
    (void)touchInput;

    if (tutorialFlag == 0) {

        if (dialogPanel.visible) {
            dialogPanel.reset(1);
        }

        return;
    }

    dialogPanel.update(dt);

    if (dialogPanel.visible) {
        return;
    }

    if (dialogPanel.fadeProgress < 1.0f) {   // gb+0x63E0
        return;
    }

    // ---- ambient-hint cascade ----
    //
    // strict priority order: the first hint whose hintShown flag is clear and
    // whose live board condition holds fires, then returns. the binary nests
    // this as if/else; flattened here to a fire-and-return sequence in the same
    // priority order (00,01,02,16,03,04,05,06,07,08,09,0a,0b,0c,0d,0e,0f,10,11,
    // 12,13,14,15,17); behaviorally identical, every branch converges on one
    // showHint. see tutorial_hint_table.h for each hint's trigger + anchor.
    //
    // anyHintFiredThisFrame persists across frames (cleared per-turn, not per-
    // frame) so the few anyFired-gated hints suppress until the next turn.
    constexpr float VGAP_BOARD = 0.0984375f;   // DAT_100059f6c (board-tile hints)
    constexpr float VGAP_PANEL = 0.046875f;    // DAT_100059f70 (HUD / panel hints)

    // shared convergence (= the binary's LAB_100019c44): mark the hint shown +
    // anyHintFiredThisFrame + dirty, then show the popup. id == hintShown index.
    auto fireHint = [&](int id, float vGap, float x, float y) {
        dialogPanel.hintShown[id]         = 1;
        dialogPanel.anyHintFiredThisFrame = 1;
        dirty                             = true;
        float pos[2] = { x, y };
        dialogPanel.showHint(vGap, pos, id);   // FUN_1000412bc
    };

    // a rack / page tile is "settled" once its slide-in + rotation lerps finish.
    auto settled = [](const TileObject* t) {
        return t->slideTimer >= 1.0f && t->rotationLerpT >= 1.0f;
    };

    // 0x00: player avatar (fires immediately, once).
    if (dialogPanel.hintShown[0x00] == 0) {
        fireHint(0x00, VGAP_BOARD,
                 playerSystem.baseQuad.posX + positionX,
                 playerSystem.baseQuad.posY + positionY);
        return;
    }

    // 0x01: starting rack tiles (slots 0 and 2) settled into place.
    if (dialogPanel.hintShown[0x01] == 0 &&
        rack[2] != nullptr && settled(rack[2]) &&
        rack[0] != nullptr && settled(rack[0])) {
        fireHint(0x01, VGAP_BOARD, rack[2]->mainQuad.posX, rack[2]->mainQuad.posY);
        return;
    }

    // 0x02: a picked-up tile settled into the placement preview.
    if (dialogPanel.hintShown[0x02] == 0 &&
        selectedRackSlot != -1 && settled(rack[selectedRackSlot])) {
        fireHint(0x02, VGAP_BOARD,
                 rack[selectedRackSlot]->mainQuad.posX + positionX,
                 rack[selectedRackSlot]->mainQuad.posY + positionY);
        return;
    }

    // 0x16: dead end: the X-button is showing and the board is idle.
    if (dialogPanel.hintShown[0x16] == 0 && xButtonVisible && state == 1) {
        fireHint(0x16, VGAP_PANEL,
                 xButtonQuad.quad.posX + positionX,
                 xButtonQuad.quad.posY + positionY);
        return;
    }

    // 0x03 / 0x04 / 0x05: the picked-up tile carries ATK(2) / DEF(3) / HP(6).
    if (dialogPanel.hintShown[0x03] == 0 && dialogPanel.anyHintFiredThisFrame == 0 &&
        selectedRackSlot != -1 && settled(rack[selectedRackSlot]) &&
        rack[selectedRackSlot]->getContentType() == 2) {
        fireHint(0x03, VGAP_BOARD,
                 rack[selectedRackSlot]->mainQuad.posX + positionX,
                 rack[selectedRackSlot]->mainQuad.posY + positionY);
        return;
    }

    if (dialogPanel.hintShown[0x04] == 0 && dialogPanel.anyHintFiredThisFrame == 0 &&
        selectedRackSlot != -1 && settled(rack[selectedRackSlot]) &&
        rack[selectedRackSlot]->getContentType() == 3) {
        fireHint(0x04, VGAP_BOARD,
                 rack[selectedRackSlot]->mainQuad.posX + positionX,
                 rack[selectedRackSlot]->mainQuad.posY + positionY);
        return;
    }

    if (dialogPanel.hintShown[0x05] == 0 && dialogPanel.anyHintFiredThisFrame == 0 &&
        selectedRackSlot != -1 && settled(rack[selectedRackSlot]) &&
        rack[selectedRackSlot]->getContentType() == 6) {
        fireHint(0x05, VGAP_BOARD,
                 rack[selectedRackSlot]->mainQuad.posX + positionX,
                 rack[selectedRackSlot]->mainQuad.posY + positionY);
        return;
    }

    // 0x06: the actively-dragged tile carries Control(5).
    if (dialogPanel.hintShown[0x06] == 0 &&
        draggedRackSlot != -1 &&
        rack[draggedRackSlot]->getContentType() == 5) {
        fireHint(0x06, VGAP_BOARD,
                 rack[draggedRackSlot]->mainQuad.posX,
                 rack[draggedRackSlot]->mainQuad.posY);
        return;
    }

    // 0x07: a settled XP(4) tile sits on the placed trail.
    if (dialogPanel.hintShown[0x07] == 0) {
        for (TileObject* tile : pageList) {
            if (tile->getContentType() == 4 && !tile->isContentScaleAnimating()) {
                fireHint(0x07, VGAP_BOARD,
                         tile->mainQuad.posX + positionX,
                         tile->mainQuad.posY + positionY);
                return;
            }
        }
    }

    // 0x08: the exit-direction arrow has faded in.
    if (dialogPanel.hintShown[0x08] == 0 && dialogPanel.anyHintFiredThisFrame == 0 &&
        exitArrowVisible && exitArrowFade >= 1.0f) {
        fireHint(0x08, VGAP_PANEL,
                 exitArrowQuad.posX + positionX,
                 exitArrowQuad.posY + positionY);
        return;
    }

    // 0x09: a snag tile is settled in the rack.
    if (dialogPanel.hintShown[0x09] == 0) {
        for (int i = 0; i < RACK_SLOT_COUNT; ++i) {
            TileObject* t = rack[i];
            if (t != nullptr && t->getSnagIfAlive() != nullptr && settled(t)) {
                fireHint(0x09, VGAP_BOARD, t->mainQuad.posX, t->mainQuad.posY);
                return;
            }
        }
    }

    // 0x0a: a special snag (type != 1) is settled in the rack.
    if (dialogPanel.hintShown[0x0a] == 0) {
        for (int i = 0; i < RACK_SLOT_COUNT; ++i) {
            TileObject* t = rack[i];
            if (t != nullptr && t->getSnagIfAlive() != nullptr &&
                t->getSnagType() != 1 && settled(t)) {
                fireHint(0x0a, VGAP_BOARD, t->mainQuad.posX, t->mainQuad.posY);
                return;
            }
        }
    }

    // 0x0b: the snag-inspect DetailPanel (mode 0) is up and settled.
    if (dialogPanel.hintShown[0x0b] == 0 &&
        detailPanel.visible && detailPanel.mode == 0 && detailPanel.fadeT >= 1.0f) {
        fireHint(0x0b, VGAP_PANEL,
                 detailPanel.combatSimPreview.quad.posX + detailPanel.posX,
                 detailPanel.combatSimPreview.quad.posY + detailPanel.posY);
        return;
    }

    // 0x0c: a tile is staged for discard (anchor: the discard button).
    if (dialogPanel.hintShown[0x0c] == 0 &&
        !hud.pendingDiscards.empty() && hud.selectedEvent == nullptr) {
        fireHint(0x0c, VGAP_PANEL,
                 hud.largeButtonFrame.quad.posX, hud.largeButtonFrame.quad.posY);
        return;
    }

    // 0x0d: one of the discard-staged tiles holds a live snag.
    if (dialogPanel.hintShown[0x0d] == 0 &&
        !hud.pendingDiscards.empty() && hud.selectedEvent == nullptr) {
        for (const DiscardEntry& entry : hud.pendingDiscards) {
            if (entry.staged != 0 && rack[entry.rackSlot]->getSnagIfAlive() != nullptr) {
                fireHint(0x0d, VGAP_BOARD,
                         rack[entry.rackSlot]->mainQuad.posX,
                         rack[entry.rackSlot]->mainQuad.posY);
                return;
            }
        }
    }

    // 0x0e: Nemesis is visible and the camera pan has settled.
    if (dialogPanel.hintShown[0x0e] == 0 && nemesis.visible && panProgress >= 1.0f) {
        HexCellPos np = hexCellLinearXY(nemesis.nemesisGridCol, nemesis.nemesisGridRow);
        fireHint(0x0e, VGAP_BOARD, np.x + positionX, np.y + positionY);
        return;
    }

    // 0x0f: the stats panel is open (anchor: the stats button).
    if (dialogPanel.hintShown[0x0f] == 0 &&
        userStatsPanel.visible && userStatsPanel.fadeTimer >= 1.0f) {
        fireHint(0x0f, VGAP_PANEL,
                 hud.buttonFrame1.quad.posX, hud.buttonFrame1.quad.posY);
        return;
    }

    // 0x10: the pause / settings menu is open (anchor: the menu button).
    if (dialogPanel.hintShown[0x10] == 0 &&
        pauseMenu.visible && pauseMenu.animTimer0 >= 1.0f) {
        fireHint(0x10, VGAP_PANEL,
                 hud.buttonFrame2.quad.posX, hud.buttonFrame2.quad.posY);
        return;
    }

    // 0x11: the level-up panel is open.
    if (dialogPanel.hintShown[0x11] == 0 &&
        levelUpPanel.visible && levelUpPanel.animTimer0 >= 1.0f) {
        fireHint(0x11, VGAP_PANEL,
                 levelUpPanel.titleQuad.posX + levelUpPanel.anchorX,
                 levelUpPanel.titleQuad.posY + levelUpPanel.anchorY);
        return;
    }

    // 0x12: the item-choice panel is open.
    if (dialogPanel.hintShown[0x12] == 0 &&
        itemChoicePanel.visible && itemChoicePanel.animTimer0 >= 1.0f) {
        fireHint(0x12, VGAP_PANEL,
                 itemChoicePanel.titleQuad.posX + itemChoicePanel.anchorX,
                 itemChoicePanel.titleQuad.posY + itemChoicePanel.anchorY);
        return;
    }

    // 0x13: the event-choice panel is open.
    if (dialogPanel.hintShown[0x13] == 0 &&
        eventChoicePanel.visible && eventChoicePanel.animTimer0 >= 1.0f) {
        fireHint(0x13, VGAP_PANEL,
                 eventChoicePanel.titleQuad.posX + eventChoicePanel.anchorX,
                 eventChoicePanel.titleQuad.posY + eventChoicePanel.anchorY);
        return;
    }

    // 0x14: the player holds an event and the tray icons have settled.
    if (dialogPanel.hintShown[0x14] == 0 &&
        hud.countEventsHeld() > 0 && !hud.anyEventSlotSlidingIn()) {
        float ex, ey;
        hud.firstEventSlotPos(ex, ey);
        fireHint(0x14, VGAP_BOARD, ex, ey);
        return;
    }

    // 0x15: the player's health has hit 0 (anchor: the HP display).
    if (dialogPanel.hintShown[0x15] == 0 && playerDowned) {
        fireHint(0x15, VGAP_PANEL, hud.tintHealth.posX, hud.tintHealth.posY);
        return;
    }

    // 0x17: wrap-up, once the four stat-tile hints (0x03..0x06) have all shown.
    if (dialogPanel.hintShown[0x17] == 0 &&
        dialogPanel.hintShown[0x03] && dialogPanel.hintShown[0x04] &&
        dialogPanel.hintShown[0x05] && dialogPanel.hintShown[0x06] &&
        dialogPanel.anyHintFiredThisFrame == 0) {
        fireHint(0x17, VGAP_PANEL, hud.tintHealth.posX, hud.tintHealth.posY);
        return;
    }
}

// FUN_10001a690, detail panel idle-dismiss gate.
//
// while detailPanel.visible, watches detailPanel.touchHoldArea (which press-and-
// hold inspect is open) and auto-closes per its rule:
//   area 0: not open; do nothing.
//   area 1 (rack hold): close when the pointer rises above the title quad
//             (pointer.y < title.y - title.h * 0.5).
//   area 2 (board hold): close when the pointer moves >= DAT_100059F7C from
//             (touchOrigX, touchOrigY).
// also closes immediately when the finger lifts (inputState == 0).
// close path: touchHoldArea = 0, DetailPanel::reset(0).
//
// when nothing is open: detailPanel.visible = 0, early-out at top.
void GameBoard::tickDetailPanelIdleDismiss() {
    bool armed = detailPanel.visible;

    if (!armed) {
        return;
    }

    Game* g = getGame();

    if (!g) {
        return;
    }

    auto dismiss = [&]() {
        detailPanel.touchHoldArea = 0;
        detailPanel.reset(0);
    };

    if (g->inputState() == 0) {
        dismiss();
        return;
    }

    int s = detailPanel.touchHoldArea;

    if (s == 1) {
        float titleBottomY = titleQuad.posY + titleQuad.height * -0.5f;

        if (g->touchY() < titleBottomY) {
            dismiss();
            return;
        }

        s = detailPanel.touchHoldArea;
    }

    if (s == 2) {
        constexpr float DISMISS_DRAG_DISTANCE = 0.0984375f;  // DAT_100059F7C

        float dx = g->touchX() - detailPanel.touchOrigX;
        float dy = g->touchY() - detailPanel.touchOrigY;
        float dist = std::sqrt(dx * dx + dy * dy);

        if (dist > DISMISS_DRAG_DISTANCE) {
            dismiss();
            return;
        }
    }
}

// FUN_10001a9b4, when tileAlphaProgress differs from tileAlphaMirror,
// recompute a lerped alpha byte and propagate to every rack tile, every
// page tile, and the player avatar. when idle: both 0, early-out.
void GameBoard::syncGlobalTileAlpha() {

    if (tileAlphaProgress == tileAlphaMirror) {
        return;
    }

    constexpr float ALPHA_IDLE = 255.0f;  // DAT_100059F80 (progress=0: opaque)
    constexpr float ALPHA_DRAG =  40.0f;  // DAT_100059F84 (progress=1: translucent)

    tileAlphaMirror = tileAlphaProgress;

    uint8_t alpha = static_cast<uint8_t>(static_cast<int>(
        tileAlphaProgress * ALPHA_DRAG + (1.0f - tileAlphaProgress) * ALPHA_IDLE));

    // dispatch on rack tiles
    for (int i = 0; i < RACK_SLOT_COUNT; i++) {

        if (rack[i]) {
            rack[i]->setTileAlpha(alpha);
        }
    }

    // dispatch on page tiles
    for (TileObject* t : pageList) {

        if (t) {
            t->setTileAlpha(alpha);
        }
    }

    // player avatar dims through MovableActor's base setAlpha (vtable[5]).
    playerSystem.setAlpha(alpha);
}

// FUN_10001aa84, inertial finger scroll + cosine-eased pan animation.
//
// two independent animations applied to (positionX, positionY):
//
// 1. inertial fling. when panInertiaActive is set:
//      positionX += vx * dt;  positionY += vy * dt;   (apply velocity)
//      then decays velocity by (1 - clamp(dt*7, 0..1)), a
//      framerate-aware exponential decay.
//      when sqrt(vx^2 + vy^2) drops below 0.0015625 (approx 1 pixel @ 640px),
//      pixel-snap positionX/Y and clear the active flag.
//
// 2. cosine-eased pan. when +0x9B0 < 1.0:
//      timer += dt * 5;
//      if timer < 1.0: pos = lerp(start, target, 0.5 - cos(timer*PI)*0.5).
//      else: snap pos to pixel grid + clamp timer = 1.0.
void GameBoard::tickInertialPanScroll(float dt) {
    constexpr float DRAG_DECAY_RATE      = 7.0f;
    constexpr float STOP_THRESHOLD       = 0.0015625f;  // DAT_100059F88
    constexpr float ANIM_PI              = 3.1415927f;  // DAT_100059F8C
    constexpr float PAN_RATE             = 5.0f;

    if (panInertiaActive) {
        positionX += panVelocityX * dt;
        positionY += panVelocityY * dt;

        // exponential-style decay: vx *= (1 - clamp(dt*7, 0..1)).
        float clampDecay = dt * DRAG_DECAY_RATE;

        if (clampDecay > 1.0f) {
            clampDecay = 1.0f;
        } else if (clampDecay < 0.0f) {
            clampDecay = 0.0f;
        }

        float keep = 1.0f - clampDecay;
        panVelocityX = panVelocityX * keep;
        panVelocityY = panVelocityY * keep;

        float speed = std::sqrt(panVelocityX * panVelocityX + panVelocityY * panVelocityY);

        if (speed < STOP_THRESHOLD) {
            // FUN_100057374: snap to 1/640 pixel grid.
            constexpr float SNAP_SCALE = 640.0f;
            auto snap = [&](float v) {
                float t = v * SNAP_SCALE;
                return ((t >= 0.0f) ? (float)(int)(t + 0.5f)
                                    : (float)(int)(t - 0.5f)) / SNAP_SCALE;
            };
            positionX = snap(positionX);
            positionY = snap(positionY);
            panInertiaActive = false;
        }
    }

    // cosine-eased pan animation from (panStartX, panStartY) to (panTargetX,
    // panTargetY). panProgress idle == 1.0; reset to 0.0 to kick a pan.
    if (panProgress < 1.0f) {
        float t = panProgress + dt * PAN_RATE;
        panProgress = t;

        if (t < 1.0f) {
            float u = 0.5f - std::cos(t * ANIM_PI) * 0.5f;
            positionX = panTargetX * u + panStartX * (1.0f - u);
            positionY = panTargetY * u + panStartY * (1.0f - u);
        } else {
            panProgress = 1.0f;
            // snap to grid (binary calls FUN_100057374 on each)
            constexpr float SNAP_SCALE = 640.0f;
            auto snap = [&](float v) {
                float t2 = v * SNAP_SCALE;
                return ((t2 >= 0.0f) ? (float)(int)(t2 + 0.5f)
                                     : (float)(int)(t2 - 0.5f)) / SNAP_SCALE;
            };
            positionX = snap(panTargetX);
            positionY = snap(panTargetY);
        }
    }
}

// FUN_10001ac04, cleanup-quad vector bob animation.
//
// for each entry in hud.pendingDiscards, snaps the floating quad to its
// rack-column X and animates a cosine-eased bob along Y. when idle: queue
// empty, no-op.
void GameBoard::animateCleanupQuadBob(float dt) {

    if (hud.pendingDiscards.empty()) {
        return;
    }

    constexpr float BOB_FREQ_HZ      = 1.5f;
    constexpr float BOB_PHASE_PI     = 3.1415927f;  // DAT_100059F90
    constexpr float BOB_AMPLITUDE_X  = 0.09375f;    // DAT_100059F94
    constexpr float BOB_AMPLITUDE_Y  = 0.03125f;    // DAT_100059F98
    constexpr float BOB_OFFSET_X     = 0.109375f;   // DAT_100059F9C
    constexpr float SLOT_X_STRIDE    = 0.1953125f;

    float t = std::fmod(dt * BOB_FREQ_HZ + hud.pendingDiscardBobTimer, 1.0f);
    hud.pendingDiscardBobTimer = t;

    // bob = (1 - cos((t+t) * PI)) / 2
    float ease = 0.5f - std::cos((t + t) * BOB_PHASE_PI) * 0.5f;
    float bobX = (1.0f - ease) * BOB_AMPLITUDE_X;
    float bobY = ease * BOB_AMPLITUDE_Y;
    float boardH = titleQuad.height;

    for (DiscardEntry& e : hud.pendingDiscards) {
        e.quad.posX = (float)e.rackSlot * SLOT_X_STRIDE + BOB_OFFSET_X;
        e.quad.posY = (titleQuad.posY - boardH * 0.5f) - (bobX + bobY);
    }
}

// FUN_10001ad30, discard-slide list at +0x96A0 per-frame animation.
//
// each node is 0x30 bytes (allocated by discardRackTile via operator_new(0x30)).
// layout (start = tile's current pos at discard, target = rack-X-going-
// off-screen; the tile slides back to its rack column then drops below):
//   +0x00  prev          // sentinel-style doubly-linked
//   +0x08  next
//   +0x10  TileObject*   // the discarding tile
//   +0x18  startX        // = tile's screen X at the moment discard fires
//   +0x1C  startY        //   (= where the slide begins)
//   +0x20  targetX       // = (slotIdx * 0.1953125 + 0.109375), rack column X
//   +0x24  targetY       // = virtualHeight + offsets approx 1.68 (off-screen below)
//   +0x28  timer         // 0..1; advances at dt/0.3 per frame
//
// per node: advance timer; when timer >= 1.0: unlink, delete the held
// TileObject, free the node (= discard complete). otherwise lerp the held
// tile's screen pos via setPosition(lerp(start, target, timer)).
void GameBoard::tickDiscardingTilesAnimation(float dt) {

    if (discardSlide.empty()) {
        return;
    }

    constexpr float TIMER_DURATION = 0.3f;  // DAT_100059FA0

    float dtRate = dt / TIMER_DURATION;

    for (auto it = discardSlide.begin(); it != discardSlide.end(); ) {
        DiscardSlideBody& entry = *it;
        entry.timer += dtRate;

        if (entry.timer >= 1.0f) {
            // discard complete: delete the held tile, erase the entry.

            if (entry.tile) {
                delete entry.tile;
            }

            it = discardSlide.erase(it);
        } else {
            float tx = (1.0f - entry.timer) * entry.startX + entry.timer * entry.targetX;
            float ty = (1.0f - entry.timer) * entry.startY + entry.timer * entry.targetY;

            if (entry.tile) {
                entry.tile->setPosition(tx, ty);
            }

            ++it;
        }
    }
}

// FUN_10001a5a0, per-frame exit-arrow fade tick.
//
// ramp exitArrowFade toward 1 while exitArrowVisible is set, or back
// toward 0 once it's cleared. while ramping (= still has work to do in
// at least one direction), push the eased alpha into the arrow Quad and
// digit ColorTint over a 0.2s window.
void GameBoard::tickExitArrowFade(float dt) {
    bool isAnimating = (exitArrowFade < 1.0f) || !exitArrowVisible;
    bool needsAnim   = (exitArrowFade > 0.0f) ||  exitArrowVisible;

    if (!(isAnimating && needsAnim)) {
        return;
    }

    constexpr float FADE_DURATION = 0.2f;    // DAT_100059F74
    constexpr float ALPHA_END     = 255.0f;  // DAT_100059F78

    // direction: visible -> forward (+1, fade in), !visible -> reverse (-1).
    constexpr float DIR_HIDDEN  = -1.0f;  // DAT_100059F00
    constexpr float DIR_VISIBLE =  1.0f;  // DAT_100059F04
    float dir = exitArrowVisible ? DIR_VISIBLE : DIR_HIDDEN;

    float t = exitArrowFade + (dt / FADE_DURATION) * dir;

    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }

    exitArrowFade = t;

    uint8_t alpha = static_cast<uint8_t>(static_cast<int>(t * ALPHA_END));
    exitArrowQuad.setAlpha(alpha);   // FUN_100008388
    exitArrowDigit.setAlpha(alpha);  // FUN_10003c948
}

// FUN_10001dbe4, nemesisAdvance(xpAmount).
//
// the central entry point for Nemesis getting fed. if Nemesis is dormant,
// wake it at the trail tail first; then credit XP. used by every game
// path that grows Nemesis (Loneliness per-turn, XP-tile placement,
// close-strike consume, chain-XP from snag kills).
void GameBoard::nemesisAdvance(int xpAmount) {

    if (!nemesis.visible) {
        nemesisSpawnAtTrailTail();
    }

    nemesis.creditXP(xpAmount);

    // achievement milestones tied to Nemesis level (= binary's FUN_10004db44).
    // checked after creditXP because that's the call that bumps nemesisLevel.
    AchievementTracker& tracker = getGame()->achievementTracker();

    if (nemesis.nemesisLevel == 5) {

        // "A Sporting Chance": nemesis lvl 5 with player still lvl 0.
        if (playerSystem.currentLevel == 0) {
            tracker.increment(AchievementId::ASportingChance);
        }
    } else if (nemesis.nemesisLevel == 10) {

        // "Don't Look Back": nemesis lvl 10 (no player-state gate).
        tracker.increment(AchievementId::DontLookBack);
    }
}

// FUN_10001a780, nemesis update + close-strike resolution.
//
// always runs nemesis.update (= FUN_100009094: nemesis spin + level number
// tint). additionally:
//   if nemesis.eatStep > 0:
//     advance fires when timer at +0x2184 hits 1.0 and nemesis.eatFired
//     is still 0:
//       set eatFired.
//       newest page-tile = pageList.back().
//       if alive snag exists: dispatch differs by snag type 1 vs other.
//       else if content type == 4 (XP, dropped on snag kill): dispatch
//         finalize-rack-place + HUD XP-marker bump.
//       pop the page-list front (delete its tile + erase the entry).
//     followup attack via FUN_100020aac when +0x2180 also at 1.0.
void GameBoard::updateNemesisAndCloseStrike(float dt, float touchInput) {
    nemesis.update(dt);

    if (nemesis.eatStep <= 0) {
        return;
    }

    if (nemesis.posTransitionT >= 1.0f && !nemesis.eatFired) {
        nemesis.eatFired = true;

        // oldest page-tile (= pageList.front()). semantically: the nemesis
        // closes in from behind the player's most-recent move, consuming
        // the tiles the player visited first.
        if (!pageList.empty() && pageList.front()) {
            TileObject* targetTile = pageList.front();
            SnagContent* sc        = targetTile->getSnagIfAlive();

            if (sc != nullptr) {
                // dispatch by snag type 1 vs other. type 1 (generic snag)
                // feeds 1 XP; everything else feeds 5.
                int xpFromSnag = (sc->type == 1) ? 1 : 5;
                nemesisAdvance(xpFromSnag);

                // achievement "Symbiosis" (binary's FUN_10004db2c).
                // fires when sc->type != 1 (= a non-generic / "special" snag).
                if (sc->type != 1) {
                    getGame()->achievementTracker().increment(AchievementId::Symbiosis);
                }
            } else {
                int contentType = targetTile->getContentType();

                // binary structure: `if (contentType == 4 (XP) && (nemesisAdvance(1),
                // !playerDowned))`. nemesisAdvance always fires when type ==
                // 4; the HUD XP-marker bump only fires when also !playerDowned.
                if (contentType == 4) {
                    nemesisAdvance(1);

                    if (!playerDowned) {
                        // FUN_10000d010(hud, magnitude, 0) = advanceXPSlot
                        // with the silent-fill clear gated on the 3rd arg
                        // (= 0 here -> no flag clear, equivalent to our
                        // advanceXPSlot(delta, false)).
                        hud.advanceXPSlot(targetTile->getContentMagnitude(), false);
                    }
                }
            }

            // delete the held tile + pop the front of the page list.
            delete targetTile;
            pageList.pop_front();
        }
    }

    if (nemesis.eatFired && nemesis.reformT >= 1.0f) {
        // FUN_100020aac, advance one more step.
        nemesisStepForward();
    }
}

// FUN_10001dd14, discardRackTile helper.
//
// pops a tile from rack[slotIdx], pushes a DiscardSlideBody entry onto the
// discard-slide list at +0x96A0 (carrying the held tile + slide start/target
// pos pairs + a slide timer), then applies per-content-type extras:
//
// regardless of tile content, sets up:
//   entry.startX  = current tile X (where the slide begins)
//   entry.startY  = current tile Y
//   entry.targetX = slotIdx * 0.1953125 + 0.109375 (= rack column X)
//   entry.targetY = virtualHeight + Y constants (~1.68, off-screen below)
//   entry.timer   = 0.0
//
// when skipExtraEffects (binary's `param_3 & 1`) is false, additionally:
//   if snag-type == 0x49 (slot-3-darkness-trigger):
//     applyTileResolutionDispatch(20 - +0x21A4)   // FUN_10001dbe4
//     append a kind=0 decoration to the +0x9670 sub-collection
//   if content-type == 0x11 (= 17, magnitude > 1): split tile via
//     pushReserveTile(0x11, 0xFFFFFFFF) then apply half-magnitude.
//   if content-type == 6 (HP tile):  chain to FUN_100020d80 (= setHP).
//   if content-type == 3 (DEF tile): chain to FUN_100020ce0 (= setDEF).
//   if content-type == 2 (ATK tile): chain to FUN_100020c40 (= setATK).
//
// finally sets rack[slotIdx] = nullptr and triggers sound 0x14 (= "discard").
// the tile pointer continues living through tickDiscardingTilesAnimation
// until the slide timer hits 1.0, at which point the tile is deleted.
void GameBoard::discardRackTile(int slotIdx, bool skipExtraEffects) {

    if (slotIdx < 0 || slotIdx >= RACK_SLOT_COUNT) {
        return;
    }

    TileObject* tile = rack[slotIdx];

    if (!tile) {
        return;
    }

    constexpr float SLOT_X_STRIDE  = 0.1953125f;
    constexpr float SLOT_X_OFFSET  = 0.109375f;     // DAT_100059FDC
    constexpr float SLOT_Y_OFFSET_A = -0.0984375f;  // DAT_100059FE0
    constexpr float SLOT_Y_OFFSET_B = -0.020312499f;// DAT_100059FE4
    constexpr float SLOT_Y_OFFSET_C =  0.3f;        // DAT_100059FE8
    const float     VIRTUAL_HEIGHT  =  Renderer::getVirtualHeight();        // DAT_10007ddb8

    // push a fresh DiscardSlideBody onto the discard-slide list with the
    // current tile pos as the slide start and the off-screen rack column
    // as the target. tickDiscardingTilesAnimation then lerps from start to
    // target each frame, deleting the tile when the slide completes.
    float targetX = (float)slotIdx * SLOT_X_STRIDE + SLOT_X_OFFSET;
    float targetY = VIRTUAL_HEIGHT + SLOT_Y_OFFSET_A + SLOT_Y_OFFSET_B + SLOT_Y_OFFSET_C;

    DiscardSlideBody entry;
    entry.tile    = tile;
    entry.startX  = tile->mainQuad.posX;
    entry.startY  = tile->mainQuad.posY;
    entry.targetX = targetX;
    entry.targetY = targetY;
    entry.timer   = 0.0f;
    discardSlide.push_back(entry);

    if (!skipExtraEffects) {
        SnagContent* sc = tile->getSnagIfAlive();

        // snag 0x49 (Scapegoat): "Can be discarded, which levels up Nemesis."
        // feed Nemesis the XP needed to finish its current level (20 - its
        // within-level XP, nemesis.nemesisXP = gb+0x21A4), then queue the burst.
        if (sc && sc->type == (int)SnagKind::Scapegoat) {
            nemesisAdvance(20 - nemesis.nemesisXP);   // FUN_10001dbe4

            // action queue push: type 0 burst at origin (0, 0) with the
            // discarded tile as tileRef.
            float origin[2] = { 0.0f, 0.0f };
            pushAction(0, origin, tile);
        }

        int contentType = tile->getContentType();

        // Pain (content type 0x11) split-on-discard: a discarded Pain tile
        // with magnitude > 1 spawns a fresh reserve tile of the same kind
        // carrying half (integer-division) of its magnitude.
        if (contentType == 0x11) {
            int mag = tile->getContentMagnitude();

            if (mag > 1) {
                TileObject* spawned = pushReserveTile(0x11u, 0xFFFFFFFFu);

                if (spawned) {
                    TileContent* tc = spawned->getTileContentIfAlive();

                    if (tc) {
                        tc->setRawAndDisplayMagnitude(mag / 2);
                    }
                }
            }
        }

        // chain "+stat per discarded tile" SpecialAbility bonuses by content
        // type. each branch looks up the player's matching SpecialAbility
        // value (held by one of the 3 base Items) and adds it to the relevant
        // stat. queryType numbers come from the SpecialAbility pool at
        // DAT_100079da0 (see project_dream_item_system.md):
        //   type 1  ("+%d {A} per discarded {A}") -> ATK
        //   type 8  ("+%d {H} per discarded {H}") -> HP
        //   type 13 ("+%d {D} per discarded {D}") -> DEF
        // binary fires this regardless of whether the tile would have committed
        // to the page list; the stat moves on every discard.
        if (contentType == 6) {
            int bonus = playerSystem.baseItemSpecialAbilityValue(8);
            setHP(static_cast<uint32_t>(playerSystem.currentHealth + bonus));
        } else if (contentType == 3) {
            int bonus = playerSystem.baseItemSpecialAbilityValue(13);
            setDEF(playerSystem.defence + bonus);
        } else if (contentType == 2) {
            int bonus = playerSystem.baseItemSpecialAbilityValue(1);
            setATK(playerSystem.attack + bonus);
        }
    }

    rack[slotIdx] = nullptr;

    // sound 0x14 (= "discard" per game.cpp's sound table at line 90)
    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x14);
    }
}

// FUN_10001d414, tryActivateNewestPageSnag.
//
// when pageList.size() >= 2 and the second-newest page tile has an alive
// snag, activate it via FUN_100025dcc. returns 1 if dispatched, 0 otherwise.
//
// the binary reads the second-newest tile, i.e. the tile one step back
// from the newest. semantically: the player just committed
// the newest tile (a content tile, normally), and the snag they're now
// adjacent to lives on the tile they were standing on the turn before
// (= second-newest). the function name is a bit of a misnomer; "newest
// page snag" really means "the snag on the tile immediately behind the
// player's current position."
int GameBoard::tryActivateNewestPageSnag() {

    if (pageList.size() < 2) {
        return 0;
    }

    // second-newest = the tile immediately behind the newest (= player's
    // position one step back). std::list iteration: rbegin() is newest,
    // ++rbegin is second-newest.
    auto it = pageList.rbegin();
    ++it;
    TileObject* topTile = *it;

    if (!topTile) {
        return 0;
    }

    SnagContent* sc = topTile->getSnagIfAlive();

    if (!sc) {
        return 0;
    }

    resolveSnagCombat(sc);
    return 1;
}

// FUN_10001d46c, armNemesisInterlockOnSpecialTiles.
//
// walks the page list. for each tile whose alive snag has type 0x18
// (= 24, "Nemesis trigger") or whose tile-content has type 0xD (= 13,
// "chain bonus"), sets nemesis eat-cycle state:
//   nemesis.eatTarget = max(1, eatTarget)
//   nemesis.eatActive = true
void GameBoard::armNemesisInterlockOnSpecialTiles() {

    for (TileObject* t : pageList) {

        if (!t) {
            continue;
        }

        SnagContent* sc = t->getSnagIfAlive();

        if (sc && sc->type == 0x18) {

            if (nemesis.eatTarget < 1) {
                nemesis.eatTarget = 1;
            }

            nemesis.eatActive = true;
            // matches binary: no continue/early-out here. binary falls
            // through to also evaluate the tile-content path. effects are
            // idempotent (count clamps to max(1, current), eatActive = true)
            // so the duplicate dispatch is silent; we mirror it for
            // structural fidelity.
        }

        // FUN_100013430(t) -> returns the TileContent pointer when alive.
        // our equivalent: tile->content (when content && visible).
        TileContent* tc = (t->content && t->content->visible) ? t->content : nullptr;

        if (tc) {
            int contentType = tc->type;

            if (contentType == 0xD) {

                if (nemesis.eatTarget < 1) {
                    nemesis.eatTarget = 1;
                }

                nemesis.eatActive = true;
            }
        }
    }
}

// FUN_10001fea4, tryOpenEventChoicePanel.
//
// when the player has the Eventful perk and an empty event-tray slot
// (= eventTray count < perkLevel(Eventful)), open the EventChoicePanel
// so they pick a new Event to install. fired at end-of-turn and after
// any Event consume. when the panel is already open, the next tick's
// gameBoardUpdate routes through eventChoicePanel.update instead, so
// this trigger is short-circuited by the visibility check there.
void GameBoard::tryOpenEventChoicePanel() {
    int activeCount = 0;
    for (int i = 0; i < 4; ++i) {
        if (hud.eventTray[i].slotPtr != nullptr) activeCount++;
    }

    int maxEvents = playerSystem.perkLevel(0x12);   // Eventful perk

    if (activeCount >= maxEvents) {
        return;
    }

    // build the current-tray snapshot for the candidate-roll's exclusion
    // pass (binary FUN_10000d8fc, same shape; inline here since we don't
    // need a heap allocation).
    std::vector<EventSlot*> activeSlots;
    activeSlots.reserve(4);
    for (int i = 0; i < 4; ++i) {

        if (hud.eventTray[i].slotPtr) {
            activeSlots.push_back(hud.eventTray[i].slotPtr);
        }
    }
    eventChoicePanel.open(playerSystem, activeSlots);
}

// FUN_10001dc44, applyOnDeathHpRefill.
//
// fires on the death edge case (HP reaches 0). reads
// playerSystem.perkLevel(9): the player's level for perkType 9 ("Refills
// your current HP to N% when your HP reaches 0"). scales playerSystem.maxHealth
// by 50% / 75% / 100% (perk levels 0 / 1 / 2) and pushes the value via setHP
// (= FUN_100020d80). the 50% default applies when the player doesn't own
// the perk. the sole call site is the state-8 revive branch
// (gameBoardUpdate), gated on playerDowned: setHP sets playerDowned on HP=0
// (freezing further HP writes), and case 8 clears it just before this
// refill so the setHP here applies.
void GameBoard::applyOnDeathHpRefill() {
    int revivePerkLvl = playerSystem.perkLevel(9);

    // refill amount derives from playerSystem.maxHealth.
    uint32_t maxHP    = static_cast<uint32_t>(playerSystem.maxHealth);
    uint32_t refillHP;

    if (revivePerkLvl == 1) {
        refillHP = (maxHP * 3) >> 2;  // 75%
    } else if (revivePerkLvl == 2) {
        refillHP = maxHP;             // 100%
    } else {
        refillHP = maxHP >> 1;        // 50% (default)
    }

    setHP(refillHP);
}

// FUN_10001db40, applyChainStatBumpsToRack.
//
// walks rack[0..4]. for each rack tile whose alive snag has type 8 (= snag
// marker), bumps its magnitude by (pageCount + 1) via FUN_10003d3a4 (=
// SnagContent::setStatX with delta).
//
// then dispatches:
//   FUN_1000178fc(this) -> post-tile recompute (game.recomputeIcons)
//   FUN_100017c04(this) -> status icon picker
//   FUN_10004e0d0(audioEngine, &pageList, +0x9B4C) -> audio sync
void GameBoard::applyChainStatBumpsToRack(float dt) {

    for (int i = 0; i < RACK_SLOT_COUNT; i++) {

        if (!rack[i]) {
            continue;
        }

        SnagContent* sc = rack[i]->getSnagIfAlive();

        if (!sc) {
            continue;
        }

        if (sc->type != 8) {
            continue;
        }

        // type-8 rack snag: snap its atk display to (pageList.size() + 1).
        // binary uses null tween queue (no floating +N), so no animation.
        sc->setAtkDisplay(static_cast<int>(pageList.size()) + 1);
    }

    // FUN_1000178fc -- post-tile recompute: walk page list, refresh
    // keysCollected and re-flip the lock / exit-tile UV.
    recomputeExitKeysCollected();

    // FUN_100017c04 -- refresh dead-end / X-button state.
    tryShowXButton();

    // achievement fan-out (= binary's FUN_10004e0d0). counts page-list
    // tiles whose snag is alive; 5 chasing snags fires KeepRunning, and
    // 6+ keys collected fires TooManyKeys.
    (void)dt;
    {
        AchievementTracker& tracker = getGame()->achievementTracker();

        int snagCount = 0;

        for (TileObject* t : pageList) {

            if (t != nullptr && t->getSnagIfAlive() != nullptr) {
                snagCount++;
            }
        }

        if (snagCount == 5) {
            tracker.increment(AchievementId::KeepRunning);
        }

        if (keysCollected > 5) {
            tracker.increment(AchievementId::TooManyKeys);
        }
    }
}

// FUN_100017c04 -- the dead-end / X-button enabler. unconditionally clears
// the visible + pressed flags + tints the button quad white. then, if the
// page list is non-empty, scans the 6 directions from the newest committed
// tile for a target hex that is unblocked, not exit-locked, and either:
//   (a) reachable from another page tile that connects to it, or
//   (b) surrounded: all 6 of its own neighbors are occupied.
// when a matching target exists, sets xButtonVisible = 1 and positions
// the quad at the target's screen coords. when no target matches and any
// target's sub-neighbor is free (= player has an escape), aborts early
// to leave the button hidden.
void GameBoard::tryShowXButton() {
    xButtonVisible = false;
    xButtonPressed = false;
    xButtonQuad.quad.setColor(0xFF, 0xFF, 0xFF, 0xFF);

    if (pageList.empty()) {
        return;
    }

    const TileObject* newest = pageList.back();

    if (!newest) {
        return;
    }

    static constexpr int kDirDeltas[6][2] = {
        { 0, -1},  // 0
        { 1,  0},  // 1
        { 1,  1},  // 2
        { 0,  1},  // 3
        {-1,  0},  // 4
        {-1, -1},  // 5
    };

    bool       found        = false;
    HexCellPos foundCellPos = { 0.0f, 0.0f };

    for (int dir = 0; dir < 6; dir++) {

        if (!newest->permitsDirection(dir, -1)) {
            continue;
        }

        int targetCol = newest->gridCol + kDirDeltas[dir][0];
        int targetRow = newest->gridRow + kDirDeltas[dir][1];

        if (cellIsOccupied(targetCol, targetRow)) {
            continue;
        }

        // exit-locked: skip the exit hex while still need keys.
        if (targetCol == exitCol && targetRow == exitRow &&
            keysCollected < keysRequired) {
            continue;
        }

        // (a) does some OTHER page tile fit at target?
        bool pageFits = false;

        for (const TileObject* pageTile : pageList) {

            if (!pageTile || pageTile == newest) {
                continue;
            }

            if (hexGridDistance(targetCol, targetRow,
                                pageTile->gridCol, pageTile->gridRow) != 1) {
                continue;
            }

            if (tilePermitsExitToward(pageTile,
                                      pageTile->gridCol, pageTile->gridRow,
                                      targetCol, targetRow,
                                      pageTile->rotationStep)) {
                pageFits = true;
                break;
            }
        }

        if (pageFits) {
            foundCellPos = hexCellLinearXY(targetCol, targetRow);
            found = true;
            continue;
        }

        // (b) no page tile fits, check target's 6 sub-neighbors. if any is
        // free, abort the whole scan (the player has an escape route past
        // this hex so the X button shouldn't appear).
        bool allSubBlocked = true;

        for (int sub = 0; sub < 6; sub++) {
            int subCol = targetCol + kDirDeltas[sub][0];
            int subRow = targetRow + kDirDeltas[sub][1];

            if (!cellIsOccupied(subCol, subRow)) {
                allSubBlocked = false;
                break;
            }
        }

        if (!allSubBlocked) {
            return;
        }

        foundCellPos = hexCellLinearXY(targetCol, targetRow);
        found = true;
    }

    if (!found) {
        return;
    }

    xButtonVisible = true;
    xButtonQuad.quad.posX = foundCellPos.x;
    xButtonQuad.quad.posY = foundCellPos.y;
    xButtonQuad.quad.snapToPixelGrid();
}

// FUN_1000175f0 -- place the level-exit cell + its 6 key-cell neighbors on
// the hex map, and configure the visible exitTileIcon Quad. the (col, row)
// is supplied by the caller (initLevel rolls it; restoreFromSnapshot passes
// the saved exit position). keysRequired / keysCollected are seeded by the
// caller's following setExitKeysRequired + recomputeExitKeysCollected.
void GameBoard::placeExitAndKeys(int col, int row) {
    exitCol = col;
    exitRow = row;

    // exit cell: kind=1, fadeTimer=0. addCell special-cases kind=1 to use
    // zero UV (the exit's icon comes from the exitTileIcon Quad below).
    hexMap.addCell(1, exitCol, exitRow, 0);

    // key cells: walk a 3x3 neighborhood of the exit; for each offset whose
    // hex distance from origin is exactly 1 (= an actual hex neighbor, not a
    // diagonal-2 cell), addCell(kind=2). matches the binary's nested loop
    // with the FUN_10001209c distance filter.
    for (int dx = -1; dx <= 1; dx++) {

        for (int dy = -1; dy <= 1; dy++) {

            if (hexGridDistance(0, 0, dx, dy) == 1) {
                hexMap.addCell(2, exitCol + dx, exitRow + dy, 0);
            }
        }
    }

    // configure exitTileIcon (the visible Exit tile; the HexMap kind=1 cell
    // is invisible, this Quad supplies the artwork). UV gets overwritten by
    // recomputeExitKeysCollected; we still seed it here.
    exitTileIcon.quad.setTexCoords(0.386719f, 0.214844f, 0.478516f, 0.262695f);
    exitTileIcon.quad.setSize(0.145313f, 0.076563f);

    // exit position. binary leans on a side effect of FUN_100012f04: in mode=0
    // the function "returns" hexX in s0 but also leaves hexY in s1, which
    // FUN_1000175f0 offsets by DAT_100059f30 (-0.01875) for posY. net: the
    // exit tile sits AT the exit hex with a small upward offset.
    HexCellPos exitPos = hexCellLinearXY(exitCol, exitRow);
    exitTileIcon.quad.posX = exitPos.x + 0.0015625f;
    exitTileIcon.quad.posY = exitPos.y + (-0.01875f);
    exitTileIcon.quad.snapToPixelGrid();
}

// FUN_100017794 -- seed keysRequired + size/position each lock icon row
// entry below exitTileIcon. seeds each with the hollow / "still needed"
// UV; recomputeExitKeysCollected flips the first `keysCollected` to filled.
void GameBoard::setExitKeysRequired(int requestedCount) {
    // clamp to [0, 4] (FUN_10005722c with min=0, max=4).
    int clamped = requestedCount;

    if (clamped < 0) {
        clamped = 0;
    }

    if (clamped > 4) {
        clamped = 4;
    }
    keysRequired = clamped;

    for (int i = 0; i < keysRequired; i++) {
        TileIcon& dot = exitLockIcons[i];

        // hollow / "still needed" lock UV.
        dot.quad.setTexCoords(0.293945f, 0.663086f, 0.320313f, 0.694336f);
        dot.quad.setSize(0.042188f, 0.05f);

        // stack the locks horizontally, centered on exitTileIcon.posX.
        // 22.0 / 640.0 = the binary's literal divisor pair (DAT_100059f34 = 640).
        dot.quad.posX = (((float)i - (float)keysRequired * 0.5f + 0.5f) * 22.0f) / 640.0f
                        + exitTileIcon.quad.posX;
        dot.quad.posY = exitTileIcon.quad.posY + 0.054688f;  // DAT_100059f38
        dot.quad.snapToPixelGrid();
    }
}

// FUN_1000178fc -- walk page list, count tiles at hex distance 1 from
// (exitCol, exitRow) into keysCollected; flip each lock UV (filled if
// i < keysCollected) and exitTileIcon UV (unlocked once collected
// reaches required); fire sound 0x3F on the locked -> unlocked transition.
void GameBoard::recomputeExitKeysCollected() {
    int oldKeysCollected = keysCollected;
    int required         = keysRequired;
    keysCollected        = 0;

    for (const TileObject* t : pageList) {

        if (t && hexGridDistance(t->gridCol, t->gridRow, exitCol, exitRow) == 1) {
            keysCollected++;
        }
    }

    int collected = keysCollected;

    for (int i = 0; i < required; i++) {
        TileIcon& dot = exitLockIcons[i];

        if (i < collected) {
            // filled (key delivered) UV.
            dot.quad.setTexCoords(0.321289f, 0.663086f, 0.347656f, 0.694336f);
        } else {
            // hollow / "still needed" UV.
            dot.quad.setTexCoords(0.293945f, 0.663086f, 0.320313f, 0.694336f);
        }
    }

    if (collected < required) {
        // locked exit art.
        exitTileIcon.quad.setTexCoords(0.386719f, 0.214844f, 0.478516f, 0.262695f);
        exitTileIcon.quad.setSize(0.145313f, 0.076563f);
    } else {
        // unlocked exit art (slightly different UV rectangle, same size).
        exitTileIcon.quad.setTexCoords(0.314453f, 0.263672f, 0.405273f, 0.311523f);
        exitTileIcon.quad.setSize(0.145313f, 0.076563f);

        // first frame in which collected reached required: chime.
        if (oldKeysCollected < required) {
            Game* g = getGame();

            if (g) {
                g->soundQueue.trigger(0x3F);
            }
        }
    }
}

// FUN_10002493c -- sets up the exit-arrow visualization. fires from
// FUN_100023ac4 (= our handleRackTilePickup) on every rack tile pickup.
// when the anchor cell (oldest page tile, or origin if the page list is
// empty) is more than 3 hex steps from the exit, points the arrow Quad
// toward the exit and updates the distance digit; closer than that, leaves
// everything untouched.
void GameBoard::updateExitArrowVisualState() {
    // anchor coord = newest committed tile (= pageList.back()), or origin
    // when the page list is empty.
    int anchorCol = 0;
    int anchorRow = 0;

    if (!pageList.empty()) {
        const TileObject* newest = pageList.back();

        if (newest) {
            anchorCol = newest->gridCol;
            anchorRow = newest->gridRow;
        }
    }

    int dist = hexGridDistance(anchorCol, anchorRow, exitCol, exitRow);

    if (dist < 4) {
        return;
    }

    exitArrowVisible = true;

    HexCellPos anchorPos = hexCellLinearXY(anchorCol, anchorRow);
    HexCellPos exitPos   = hexCellLinearXY(exitCol,   exitRow);

    float dx       = exitPos.x - anchorPos.x;
    float dy       = exitPos.y - anchorPos.y;
    float worldLen = std::sqrt(dx * dx + dy * dy);
    float unitX    = dx / worldLen;
    float unitY    = dy / worldLen;

    // arrow position: 1 step of length DAT_10005a010 along the unit vector
    // pointing from anchor toward exit, starting from anchor's screen pos.
    constexpr float ARROW_OFFSET = 0.167188f;  // DAT_10005a010
    exitArrowQuad.posX = anchorPos.x + unitX * ARROW_OFFSET;
    exitArrowQuad.posY = anchorPos.y + unitY * ARROW_OFFSET;

    // rotation: atan2(unitY, unitX) in radians, rescaled to degrees with a
    // +90 deg offset (sprite is drawn pointing up, so the +90 aligns it
    // with the +X-axis convention atan2 returns from).
    constexpr float DEG_PER_TURN = 180.0f;     // DAT_10005a014
    constexpr float PI_RAD       = 3.1415927f; // DAT_10005a018
    constexpr float SPRITE_BIAS  = 90.0f;      // DAT_10005a01c
    float angleRad = std::atan2(unitY, unitX);
    exitArrowQuad.rotation = (angleRad * DEG_PER_TURN) / PI_RAD + SPRITE_BIAS;

    exitArrowDigit.setNumber(dist, /*textStyle=*/0, /*positionMode=*/1);
    exitArrowDigit.setPosition(exitArrowQuad.posX, exitArrowQuad.posY,
                               /*mode=*/1);
}

// FUN_10001d9dc, refillRackPostCommit.
//
// walks rack[0..4]; for each null slot, calls populateRack() (= FUN_100018180,
// already ported). then walks again applying per-tile "type-3C" finalization
// (FUN_10003d3a4 / FUN_10003d468 / FUN_10003d530, magnitude bumps for
// kind 0x3C = 60 = "snag-spawn-after-refill") and FUN_1000208f8 +
// FUN_100023ba8 (rack-validation hooks).
void GameBoard::refillRackPostCommit() {
    int refilled = 0;

    for (int i = 0; i < RACK_SLOT_COUNT; i++) {

        if (rack[i] == nullptr) {
            populateRack();
            refilled++;
        }
    }

    if (refilled <= 0) {
        return;
    }

    AchievementTracker& tracker = getGame()->achievementTracker();

    // EndOfTheLine optimization gate (= binary's `!FUN_10004d6c4(.., 0x27)`):
    // once unlocked, we skip the rack-validation chain entirely. otherwise
    // any tile that passes either validator flips this true, suppressing the
    // increment at the bottom. matches the binary's bVar2 tracking exactly.
    bool anyTilePlayable = !tracker.isLocked(
        static_cast<uint32_t>(AchievementId::EndOfTheLine));

    for (int i = 0; i < RACK_SLOT_COUNT; i++) {

        if (!rack[i]) {
            continue;
        }

        // type-0x3C rack snag: gains atk/def/hp equal to refilled-count.
        // floating "+N" tween fires from each stat tint at zero offset.
        if (rack[i]->getSnagType() == 0x3C) {
            SnagContent* sn = rack[i]->getSnagIfAlive();

            if (sn) {
                float zero[2] = { 0.0f, 0.0f };
                sn->setAtkDisplay(sn->atk + refilled, this, zero);
                sn->setDefDisplay(sn->def + refilled, this, zero);
                sn->setHpDisplay (sn->hp  + refilled, this, zero);
            }
        }

        if (!anyTilePlayable
            && !canDiscardRackTile(rack[i])) {
            // dry-run pickup check (commit=false): no action queue side
            // effects, returns true iff the tile would be a legal pickup.
            //
            // TODO_polish: tryPickupRackTile doesn't walk the hex grid to
            // verify a valid placement target exists, so a rack of e.g. 5
            // special snags with no legal hexes still reads as "playable"
            // and suppresses this achievement. matches the binary's
            // behavior but is looser than the table description ("hold 5
            // unplaceable/undiscardable tiles"). worth tightening once the
            // rest of the binary is faithfully ported.
            anyTilePlayable = tryPickupRackTile(rack[i], false);
        } else {
            anyTilePlayable = true;
        }
    }

    // achievement "End of the Line" (= binary's FUN_10004e35c). fires when
    // the rack contains nothing the player can play or discard.
    if (!anyTilePlayable) {
        tracker.increment(AchievementId::EndOfTheLine);
    }
}

// FUN_10001b484, X-button hit-test (dead-end escape). consumes a tap on
// the X-button quad at +0x8A0 when visible. on confirmed tap, bumps
// nemesis.eatTarget = max(pageCount - 1, eatTarget) and clears eatActive,
// which routes through the state-7 -> state-9 trail-extension pipeline so
// Nemesis crawls forward to one tile behind the player.
//
// outer guard splits into two branches by touch state:
//   touchState != 1 or touchY below title bar bottom: release branch,
//     fires only when (touchState == 0 and xButtonPressed).
//   else: press branch, touchState == 1 and touch above title bar
//     bottom. bbox-tests xButtonQuad to confirm the touch is on the
//     X button, not just somewhere on the title row.
//
// returns true only when the gesture consumed the touch (= the bbox hit).
bool GameBoard::tryConsumeXButton() {

    if (!xButtonVisible) {
        return false;
    }

    Game* g = getGame();

    if (!g) {
        return false;
    }

    int   touchState   = g->inputState();
    float titleBottomY = titleQuad.posY + titleQuad.height * -0.5f;
    bool  takeReleaseBranch = (touchState != 1) || (titleBottomY <= g->touchY());

    // the X-button Quad is drawn inside the board's glTranslate, so its
    // posX/Y are in world space. binary converts screen-space touch to
    // world before bbox-testing: touchWorld = touchScreen - boardPos.
    float touchWorldX = g->touchX() - positionX;
    float touchWorldY = g->touchY() - positionY;

    if (takeReleaseBranch) {
        // release branch: fires only when touchState == 0 and pressed.
        if (touchState != 0 || !xButtonPressed) {
            return false;
        }

        // bbox test to verify pointer still over button on release.
        bool stillOver = xButtonQuad.quad.contains(touchWorldX, touchWorldY);

        if (stillOver) {
            // bump nemesis.eatTarget toward pageList.size() - 1, clear eatActive
            // so Nemesis trail-extends to one tile behind the player.
            int newMin = static_cast<int>(pageList.size()) - 1;

            if (newMin > nemesis.eatTarget) {
                nemesis.eatTarget = newMin;
            }

            nemesis.eatActive = false;
            g->soundQueue.trigger(6);
        } else {
            g->soundQueue.trigger(7);
        }

        xButtonPressed = false;
        xButtonQuad.quad.setColor(0xFF, 0xFF, 0xFF, 0xFF);
        return true;
    }

    // press branch: touchState == 1, touch above title bar bottom.
    if (xButtonQuad.quad.contains(touchWorldX, touchWorldY)) {
        xButtonPressed = true;
        xButtonQuad.quad.setColor(0xB4, 0xB4, 0xB4, 0xFF);
        g->soundQueue.trigger(5);
        return true;
    }

    return false;
}

// FUN_10001ae10, central touch dispatcher.
//
// HUD-level touch routing. early-out chain:
//   1. HUD::queryReleaseTouch returns 0 -> no touch this frame -> false.
//   2. game-internal touch state (+0x685C, an int): cases 1 / 2 / 3.
//      case 1 -> encounter panel hit
//      case 2 -> score panel hit
//      case 3 -> discard-staging commit pass on hud.pendingDiscards
//   3. held Event button at +0x8068 -> fire its effect via FUN_100020f80.
//   4. case-3 commit: if pendingDiscards has staged entries, hand off to
//      commitPendingDiscards.
//
// returns true when the touch was consumed by ANY of the above paths.
bool GameBoard::dispatchHexAndRackTouch(float dt, float touchInput) {
    // FUN_10000cb84, hud.queryReleaseTouch(). returns non-zero when the
    // HUD's engagement / release state machine consumed the touch this
    // frame. with nothing engaged (gameState = 0, engagementState = 0,
    // selectedItem = -1) it returns 0, taking the early-out below.
    int hudHit = hud.queryReleaseTouch();

    if (hudHit == 0) {
        return false;
    }

    // game-internal sub-screen touch state
    int  s = hud.publishedState;

    if (s == 1) {
        // HUD top-left player-icon tapped -> open the user-stats panel.
        // binary (FUN_10001ae10 release-dispatch case 1):
        //   FUN_1000404b0(detailPanel, 0)  -> detailPanel.reset(0)
        //   FUN_10000a9f4(gb+0xA3F8, gb+0x8270, gb+0x20)
        //                = userStatsPanel.open(playerSystem, &worldIndex)
        // historically mislabeled "encounter" in this branch.
        detailPanel.reset(0);
        // binary passes gb+0x20 (= &totalTurnCount); the panel reads three
        // sequential ints (totalTurnCount, worldLevelIndex, snagsDefeated)
        // and formats them under the "World: %d Level: %d Items: %d" line.
        // the field-name vs displayed-label pairings don't perfectly
        // correspond to our renames; we preserve the binary's pointer
        // shape and let the user see whatever it shows.
        userStatsPanel.open(playerSystem, &totalTurnCount);
        (void)touchInput;
        return true;
    }

    if (s == 2) {
        // HUD menu icon tapped -> open the pause menu. binary:
        //   FUN_1000404b0(detailPanel, 0)  -> detailPanel.reset(0)
        //   FUN_10002f688(gb+0x10, gb+0x14, gb+0xF488, gb+0xC)
        //                = pauseMenu.open(seVolume, bgmVolume, tutorialFlag)
        detailPanel.reset(0);
        pauseMenu.open(seVolume, bgmVolume, tutorialFlag);
        return true;
    }

    // case 3: a released Event button -> fire its effect via fireEvent,
    // then run the discard-staging post-pass.
    EventSlot* heldEvent = hud.releasedEventSlot;

    if (heldEvent != nullptr) {
        // fireEvent dispatches the per-kind effect. returns true when the
        // event was successfully consumed (= we should remove it from the
        // tray). some events return false when their precondition fails;
        // in that case the slot stays in the tray.
        const bool eventConsumed = fireEvent(heldEvent, dt, touchInput, 0.0f);

        if (eventConsumed && hud.releasedEventSlot != nullptr) {
            getGame()->achievementTracker().noteEventActivated(*heldEvent);

            Game* g2 = getGame();

            if (g2) {
                g2->soundQueue.trigger(0x2D);
            }

            hud.removeEventSlot(heldEvent, /*compact=*/true);
            eventsFired += 1;
            dirty = true;
        }

        // post-Event-effect: if any discards are still staged and the
        // Event pointer wasn't cleared by the dispatch, play sound 0x2E.
        if (hud.pendingDiscards.empty()) {
            return true;
        }

        if (hud.selectedEvent == nullptr) {
            return true;
        }

        Game* g3 = getGame();

        if (g3) {
            g3->soundQueue.trigger(0x2E);
        }

        return true;
    }

    if (s != 3) {
        return true;
    }

    // case 3: discard-button tap. empty queue -> enter staging mode.
    if (hud.pendingDiscards.empty()) {
        hud.setConditionalIcon(GameplayHUD::ConditionalIconState::StagingActive);
        hud.selectedEvent = nullptr;

        SnagContent* tantrum = findSnagInRack(0x75);

        if (tantrum == nullptr) {
            // no Tantrum: stage every discardable rack tile.
            for (int i = 0; i < RACK_SLOT_COUNT; i++) {

                if (rack[i] && canDiscardRackTile(rack[i])) {
                    pushDiscardStagingEntry(i);
                }
            }
        } else {
            // Tantrum forces "rightmost only" staging.
            pushDiscardStagingEntry(4);
        }

        refreshDiscardButtonAvailability();
        return true;
    }

    // queue not empty: discard-staging commit path.
    commitPendingDiscards();
    refreshDiscardButtonAvailability();
    return true;
}

// FUN_100020f80, GameBoard::fireEvent.
//
// per-EventKind effect dispatch for a released Event button. the binary
// drives this via a 0x51-entry jump table at 0x1000237D0 (one entry per
// EventKind). preconditions live INSIDE each case body; the wrapper
// has no pre-validation.
//
// return convention (binary):
//   true  -> effect committed; outer dispatcher consumes the slot
//            (= remove from tray, ++eventsFired, dirty=true).
//   false -> not consumed (precondition failed, popup gate absorbed
//            the fire, or a player-selection panel is now open; slot
//            stays in the tray awaiting the follow-up confirm).
//
// LAB_100021a50: when a case body wants the player to choose tiles to
// act on, it calls pushDiscardStagingEntry for each candidate, sets
// hud.selectedEvent = &slot->eventType, sets slot->awaitingTileSelection
// = 1, and returns false. the discard-panel commit path
// (applyHexPickupConsumeEffect) runs the case's actual effect once the
// player taps a staged tile, then removes the slot from the HUD tray.
bool GameBoard::fireEvent(EventSlot* slot, float dt, float anchorX, float anchorY) {
    (void)dt;
    (void)anchorX;
    (void)anchorY;

    if (slot == nullptr) {
        return false;
    }

    // ---- prologue (binary 0x100020f80..0x1000212e4) ----
    //
    // (1) popup gate: if Sudden Tension (snag type 0x25) is on the board,
    //     it absorbs every event fire. pushes a deferred action so the
    //     snag's eat-animation handles the cancel, then returns false.
    //     anchor heuristic: when the snag's parent tile is still in the
    //     rack and isn't the currently-selected rack tile, anchor = (0,0);
    //     otherwise anchor = current touch input position. (binary uses
    //     game_board+0x97c which IS positionX/Y read as one qword.)
    if (SnagContent* tension = findSnagInRackOrPage(0x25)) {
        TileObject* tensionTile = tension->tileParent;
        float origin[2] = { 0.0f, 0.0f };

        bool useTouchAnchor = tensionTile->committed
                           || (selectedRackSlot != -1
                               && rack[selectedRackSlot] == tensionTile);

        if (useTouchAnchor) {
            origin[0] = positionX;
            origin[1] = positionY;
            // binary re-fetches tensionTile here; both sides of the
            // branch end with tensionTile pointing at the same object,
            // so the re-fetch is a no-op semantically.
        }
        pushAction(0, origin, tensionTile);
        return false;
    }

    // (2) FUN_10000d8c0, clear the awaitingTileSelection flag on every
    //     event slot currently in the HUD tray. firing any event drops
    //     the previously-selected slot's "lifted" visual state.
    for (int i = 0; i < 4; ++i) {

        if (EventSlot* es = hud.eventTray[i].slotPtr) {
            es->awaitingTileSelection = 0;
        }
    }

    // (3) pendingDiscards pre-pass. when the player previously tapped an
    //     Event that opened the tile-selection panel and now taps a
    //     different (or the same) Event, the binary:
    //       - if no event is currently selected (= dangling stale entries):
    //         unstage anything still staged + flush the queue.
    //       - else, branch by the selected event's kind:
    //           0x2f StrongMedicine -> if same event re-tapped, commit
    //             its "erase staged tile" effect; gain DEF per blank.
    //           0x23 ABriefPause   -> if same event re-tapped, commit
    //             its "convert staged tile to Pause" effect.
    //           anything else      -> abandon: pop all entries, clear
    //             selectedEvent, refresh icon. if same event re-tapped
    //             return false (cancel); else fall through to fire new.
    //       in StrongMedicine / ABriefPause branches, when committed or
    //       the same event was re-tapped, return bVar33 (= "did we
    //       commit?"); else fall through to fire the new event.
    bool prologueAbandonedAndContinues = false;

    if (!hud.pendingDiscards.empty()) {

        if (hud.selectedEvent == nullptr) {
            // stale entries with no selected event: unstage + clear.
            for (DiscardEntry& e : hud.pendingDiscards) {

                if (e.staged != 0) {
                    unstageDiscardEntryVisual(e);
                }
            }
            hud.pendingDiscards.clear();
            hud.setConditionalIcon(GameplayHUD::ConditionalIconState::Default);
        }

        if (!hud.pendingDiscards.empty()) {
            int  armedKind       = *hud.selectedEvent;
            bool sameEventTapped = (hud.selectedEvent == &slot->eventType);
            bool committed       = false;

            if (armedKind == 0x2f) {
                // StrongMedicine: "Convert selected tiles to blanks.
                // Gain 3 {D} per held blank."
                for (DiscardEntry& e : hud.pendingDiscards) {

                    if (e.staged != 0) {
                        unstageDiscardEntryVisual(e);

                        if (sameEventTapped) {
                            TileObject* tile = rack[e.rackSlot];

                            if (tile) {
                                tile->erase();
                                committed = true;
                            }
                        }
                    }
                }

                if (committed) {
                    int blanks = countContentTypeInRack(0);
                    setDEF(playerSystem.defence + blanks * 3);
                }
            } else if (armedKind != 0x23) {
                // generic abandon path (any non-StrongMedicine / non-
                // ABriefPause selected kind, including all batch-1 cases).
                hud.pendingDiscards.clear();
                hud.setConditionalIcon(GameplayHUD::ConditionalIconState::Default);
                hud.selectedEvent = nullptr;

                if (sameEventTapped) {
                    return false;   // re-tap on the selected event = cancel
                }
                prologueAbandonedAndContinues = true;
                // fall through to fire the new event
            } else {
                // ABriefPause (0x23): "Convert selected tiles to Pauses.
                // Pauses stop snags and give {A} {D}."
                for (DiscardEntry& e : hud.pendingDiscards) {

                    if (e.staged != 0) {
                        unstageDiscardEntryVisual(e);

                        if (sameEventTapped) {
                            TileObject* tile = rack[e.rackSlot];

                            if (tile) {
                                tile->erase();
                                tile->setTileContent(0xe, 0);   // 0xe = Pause
                                committed = true;
                            }
                        }
                    }
                }
            }

            if (!prologueAbandonedAndContinues) {
                hud.pendingDiscards.clear();
                hud.setConditionalIcon(GameplayHUD::ConditionalIconState::Default);
                hud.selectedEvent = nullptr;

                if (committed || sameEventTapped) {
                    return committed;
                }
                // otherwise: fall through to fire the new event
            }
        }
    }

    const EventKind kind = static_cast<EventKind>(slot->eventType);

    // ---- LAB_100021a50 helper: open the tile-selection panel for the
    // player to pick which staged tiles the Event should act on. shared
    // by every case that defers its effect to a "you pick the targets"
    // step (Kinds 1, 13, 14, 19, 35, 47, 65, 68 in EVENT_TABLE). returns
    // false (= slot stays awaiting selection) whenever at least one
    // candidate got staged. no-op when nothing was staged.
    auto openTileSelectionPanel = [this, slot]() -> bool {
        if (hud.pendingDiscards.empty()) {
            return true;   // no candidates, nothing to confirm
        }
        snapTileBackToRack();
        hud.selectedEvent = &slot->eventType;
        refreshDiscardButtonAvailability();
        slot->awaitingTileSelection = 1;   // FUN_10002a2c8: drives hover-lerp
        return false;
    };

    switch (kind) {

        case EventKind::ShatteredMemory: {
            // case 0: "Discard all discardable held tiles."
            snapTileBackToRack();
            bool any = false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile != nullptr && canDiscardRackTile(tile)) {
                    discardRackTile(i, false);
                    any = true;
                }
            }

            // joined_r0x000100022928: !any -> false; else refill + return true.
            if (!any) {
                return false;
            }
            refillRackPostCommit();
            return true;
        }

        case EventKind::FlashOfInsight: {
            // case 1: "Discard any held tile." stage every rack tile whose
            // alive-snag is null or snagType == 1 (= placeholder/normal),
            // then open the discard panel for player confirm.
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    pushDiscardStagingEntry(i);
                }
            }

            openTileSelectionPanel();
            return false;   // LAB_100021a50 always returns false
        }

        case EventKind::SkeletonKey: {
            // case 2: "Double all held {A} tile values."
            bool any = false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getContentType() != 2) continue;

                TileContent* tc = tile->getTileContentIfAlive();
                int          m  = tile->getContentMagnitude();
                tc->setMagnitude(m * 2);

                float origin[2] = { 0.0f, 0.0f };
                pushAction(0, origin, tile);
                any = true;
            }

            return any;
        }

        case EventKind::HardenedShell: {
            // case 3: "Double all held {D} tile values."
            bool any = false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getContentType() != 3) continue;

                TileContent* tc = tile->getTileContentIfAlive();
                int          m  = tile->getContentMagnitude();
                tc->setMagnitude(m * 2);

                float origin[2] = { 0.0f, 0.0f };
                pushAction(0, origin, tile);
                any = true;
            }

            return any;
        }

        case EventKind::PrimalRoar: {
            // case 4: "Gain 10 {A}, {D} and {H}".
            setATK(playerSystem.attack + 10);
            setDEF(playerSystem.defence + 10);
            setHP(static_cast<uint32_t>(playerSystem.currentHealth + 10));
            return true;   // LAB_100021f48 -> break -> return true
        }

        case EventKind::NexusOfPower: {
            // case 5: "Gain {C} equal to the number of exits on the current
            // tile, minus 2." current tile = newest page tile.
            if (pageList.empty()) {
                return false;
            }
            TileObject* current = pageList.back();
            int exits = current->getExitCount();
            if (exits < 3) {
                return false;
            }
            hud.advanceCTRLSlot(exits - 2, false);
            return true;
        }

        case EventKind::SinisterCrowd: {
            // case 6: "Gain {X} per held special snag." (special = alive
            // snag with type != 1.)
            int count = 0;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagIfAlive() == nullptr) continue;
                if (tile->getSnagType() == 1) continue;

                count++;
            }

            if (count < 1) {
                return false;
            }
            hud.advanceXPSlot(count, false);
            return true;
        }

        case EventKind::CircleOfProtection: {
            // case 7: "Discard all held {H} tiles. Draw {D} tile per tile
            // discarded." (contentType 6 = HP tile.)
            snapTileBackToRack();
            bool any = false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getContentType() != 6) continue;

                discardRackTile(i, false);
                pushReserveTile(3, 0xffffffff);   // {D} replacement
                any = true;
            }

            if (!any) {
                return false;
            }
            refillRackPostCommit();
            return true;
        }

        case EventKind::BorrowedTime: {
            // case 8: "Advance Nemesis to current tile. Gain {H} per tile
            // it moves over." Nemesis advance distance = pageList.size() - 1.
            const int pageSize = static_cast<int>(pageList.size());

            if (pageSize < 2) {
                return false;
            }
            snapTileBackToRack();
            setHP(static_cast<uint32_t>(playerSystem.currentHealth + pageSize - 1));
            int distance = pageSize - 1;
            if (distance < nemesis.eatTarget) {
                distance = nemesis.eatTarget;
            }
            nemesis.eatTarget = distance;
            nemesis.eatActive = false;   // LAB_1000234dc
            return true;
        }

        case EventKind::Bait: {
            // case 9: "Advance Nemesis into the next {X}. Gain double {X}
            // from it." {X} = XP tile (contentType 4). walk page list
            // oldest -> newest, find first XP tile; if none, fail.
            snapTileBackToRack();

            if (pageList.empty()) {
                return false;
            }

            TileObject* foundTile = nullptr;
            int         distance  = 1;

            for (TileObject* tile : pageList) {

                if (tile && tile->getContentType() == 4) {
                    foundTile = tile;
                    break;
                }

                distance++;
            }

            if (foundTile == nullptr) {
                return false;
            }
            TileContent* tc = foundTile->getTileContentIfAlive();
            int          m  = foundTile->getContentMagnitude();
            tc->setMagnitude(m * 2);   // double the {X} value

            if (distance < nemesis.eatTarget) {
                distance = nemesis.eatTarget;
            }
            nemesis.eatTarget = distance;
            nemesis.eatActive = false;
            return true;
        }

        case EventKind::Maelstrom: {
            // case 10: "Discard all your events. Gain {X} per event
            // discarded." count is what FUN_10000d7ac returns; mirror its
            // body inline (4 tray slots at HUD+0x1bc8 with non-null head).
            int count = 0;

            for (int i = 0; i < 4; ++i) {

                if (hud.eventTray[i].slotPtr != nullptr) {
                    count++;
                }
            }
            hud.advanceXPSlot(count, false);
            hud.clearEventSlots();
            return true;
        }

        case EventKind::SuckerPunch: {
            // case 11: "Double your current {A}. Set your current {H} to 1."
            setATK(playerSystem.attack * 2);
            setHP(1);
            return true;
        }

        case EventKind::ShiftingSands: {
            // case 12: "Push all placed snags back 5 tiles." each placed
            // tile's snag moves to the tile 5 steps closer to the start
            // (= 5 entries earlier in pageList: front = oldest, back = newest).
            // uses snag->sendToward(target, reparent=false).
            //
            // binary maintains a sliding 6-node window via an explicit
            // doubly-linked list it builds inline; std::vector indexed by
            // (i - 5) is the equivalent operation.
            std::vector<TileObject*> page(pageList.begin(), pageList.end());

            bool any = false;
            for (size_t i = 5; i < page.size(); ++i) {
                SnagContent* sc = page[i]->getSnagIfAlive();

                if (sc != nullptr) {
                    sc->sendToward(page[i - 5], false);
                    any = true;
                }
            }
            return any;
        }

        case EventKind::Metamorphosis: {
            // case 13: "Discard a held special snag. Draw another special
            // snag." stage every rack tile with alive snag and snagType != 1
            // (= special snag); open discard panel.
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagIfAlive() == nullptr) continue;
                if (tile->getSnagType() == 1) continue;

                pushDiscardStagingEntry(i);
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::SoothingMelody: {
            // case 14: "Blank a held normal snag tile." stage every rack
            // tile whose snagType == 1 (normal snag); open discard panel.
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagType() != 1) continue;

                pushDiscardStagingEntry(i);
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::AWarmSmile: {
            // case 15: "Set your {H} to half your max {H}." formula:
            //   newHP = (maxHP + 1) / 2; fail if current already equals it.
            uint32_t target = static_cast<uint32_t>(playerSystem.maxHealth + 1) / 2;

            if (static_cast<uint32_t>(playerSystem.currentHealth) == target) {
                return false;
            }
            setHP(target);
            return true;
        }

        case EventKind::Falling: {
            // case 16: "Blank a random held tile. Gain triple value if
            // it's {A} {H} {D} {C}." candidate = rack slot whose snag is
            // not alive or is a normal snag (type 1).
            std::vector<int> candidates;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();
                if (sc != nullptr && tile->getSnagType() != 1) continue;

                candidates.push_back(i);
            }

            if (candidates.empty()) {
                return false;
            }

            snapTileBackToRack();
            int         pick     = rngInt(0, static_cast<int>(candidates.size()) - 1, 4);
            int         slotIdx  = candidates[pick];
            TileObject* tile     = rack[slotIdx];
            int         contType = tile->getContentType();

            switch (contType) {
                case 2: {   // {A}
                    int m = tile->getContentMagnitude();
                    setATK(playerSystem.attack + m * 3);
                    break;
                }
                case 3: {   // {D}
                    int m = tile->getContentMagnitude();
                    setDEF(playerSystem.defence + m * 3);
                    break;
                }
                case 5: {   // {C}
                    int m = tile->getContentMagnitude();
                    hud.advanceCTRLSlot(m * 3, false);
                    break;
                }
                case 6: {   // {H}
                    int m = tile->getContentMagnitude();
                    setHP(static_cast<uint32_t>(playerSystem.currentHealth + m * 3));
                    break;
                }
                default: break;
            }

            float origin[2] = { 0.0f, 0.0f };
            pushAction(0, origin, tile);
            tile->erase();
            return true;
        }

        case EventKind::Checkmate: {
            // case 17: "Defeat all placed normal snags. Gain 1 {C} per
            // defeated snag." walk page list, each tile whose snag has
            // type 1 (normal): convert/upgrade to XP tile + kill snag,
            // increment defeated count. final gain = advanceCTRL(count).
            if (pageList.empty()) {
                return false;
            }

            int defeated = 0;

            for (TileObject* tile : pageList) {

                if (!tile || tile->getSnagType() != 1) continue;

                SnagContent* sc = tile->getSnagIfAlive();
                int tier = sc->tier();

                if (tile->getContentType() == 4) {
                    // already an XP tile: bump its magnitude by tier
                    TileContent* tc = tile->getTileContentIfAlive();
                    int          m  = tile->getContentMagnitude();
                    tc->setMagnitude(m + tier);
                } else {
                    // convert to fresh XP tile valued at tier
                    tile->setTileContent(4, tier);
                }
                sc->killAndFade();
                defeated++;
            }

            if (defeated < 1) {
                return false;
            }
            hud.advanceCTRLSlot(defeated, false);   // LAB_100023000
            return true;
        }

        case EventKind::AMomentOfRest: {
            // case 18: "Halve your current {A}. Double your current {D}."
            // binary precondition: skip when DEF is already 0 (since
            // doubling 0 is a no-op).
            if (playerSystem.defence == 0) {
                return false;
            }
            setATK(static_cast<int>(static_cast<uint32_t>(playerSystem.attack) >> 1));
            setDEF(playerSystem.defence * 2);   // LAB_10002310c
            return true;
        }

        case EventKind::EarlyWarning: {
            // case 19: "Set a held normal snag's {H} to 1." stage every
            // rack tile whose snagType == 1; open discard panel. (the
            // discard-panel confirm path applies the actual {H} = 1
            // mutation, but selection is what we set up here.)
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagType() != 1) continue;

                pushDiscardStagingEntry(i);
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::UntappedPotential: {
            // case 0x14: "Convert a held tile into Potential. Potential
            // gives {X} when placed." stage non-special-snag rack tiles,
            // open tile-selection panel. commit dispatch in
            // applyHexPickupConsumeEffect's case 0x14 erases the picked
            // tile and replaces its content with type 10 (Potential).
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    pushDiscardStagingEntry(i);
                }
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::SuddenPhoneCall: {
            // case 0x15: "Convert a held special snag into a normal snag.
            // Nemesis levels up." stage special-snag rack tiles (alive
            // snag and snagType != 1); open panel. commit dispatch in
            // applyHexPickupConsumeEffect's case 0x15 erases + transformToSnag
            // (normal snag) + nemesisAdvance(20 - nemesisXP).
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagIfAlive() == nullptr) continue;
                if (tile->getSnagType() == 1) continue;

                pushDiscardStagingEntry(i);
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::Interception: {
            // case 0x16: "Set your {A} equal to your {D}".
            int newAtk = playerSystem.defence;

            if (playerSystem.attack == newAtk) {
                return false;
            }
            setATK(newAtk);   // LAB_100022838
            return true;
        }

        case EventKind::SubtleFragrance: {
            // case 0x17: "Create 1 {X} at the start of the trail."
            // pans camera to the oldest page tile, then either creates a
            // fresh XP tile there (magnitude 1) or bumps an existing
            // XP tile's magnitude by 1.
            if (pageList.empty()) {
                return false;
            }
            TileObject* oldest = pageList.front();
            setNemesisPanTarget(oldest->gridCol, oldest->gridRow);

            if (oldest->getContentType() != 4) {
                oldest->setTileContent(4, 1);
                return true;
            }
            TileContent* tc = oldest->getTileContentIfAlive();
            int          m  = oldest->getContentMagnitude();
            tc->setMagnitude(m + 1);
            return true;
        }

        case EventKind::Gallop: {
            // case 0x18: "Add 2 charges to all other events." binary
            // builds a vector of non-null tray slotPtrs and addCharge's
            // each twice. we iterate the tray directly with the same
            // result. "all other events" in the desc is misleading:
            // the binary includes the firing slot too (it's about to
            // be consumed, so the extra charges don't surface).
            for (int i = 0; i < 4; ++i) {

                if (EventSlot* es = hud.eventTray[i].slotPtr) {
                    es->addCharge();
                    es->addCharge();
                }
            }
            return true;
        }

        case EventKind::LatentDiscipline: {
            // case 0x19: "Draw a Discipline tile." contentType 0xb =
            // Discipline. its per-discard value growth ("gains 1 value per
            // tile you discard") is ported in the batch-commit pass-3 rack
            // walk (content 0xb branch).
            pushReserveTile(11, 0xffffffffu);
            return true;
        }

        case EventKind::DeepClarity: {
            // case 0x1a: "Draw a Clarity tile." contentType 12 = Clarity.
            pushReserveTile(12, 0xffffffffu);
            return true;
        }

        case EventKind::Pacifism: {
            // case 0x1b: "Refill your {H}, set your {A} to 0."
            if (playerSystem.currentHealth == playerSystem.maxHealth) {
                return false;
            }
            setHP(static_cast<uint32_t>(playerSystem.maxHealth));
            setATK(0);   // LAB_100022838 with iVar10 = 0
            return true;
        }

        case EventKind::Tranquility: {
            // case 0x1c: "Discard all normal snags."
            snapTileBackToRack();
            bool any = false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagType() != 1) continue;

                discardRackTile(i, false);
                any = true;
            }

            if (!any) {
                return false;
            }
            refillRackPostCommit();   // joined_r0x000100022928 -> LAB_1000229f4
            return true;
        }

        case EventKind::GrindingGears: {
            // case 0x1d: "Gain 3 {C}. Nemesis gains 5 experience."
            hud.advanceCTRLSlot(3, false);
            nemesisAdvance(5);
            return true;
        }

        case EventKind::AlluringSigil: {
            // case 0x1e: "Draw a Lure tile." contentType 13 = Lure.
            pushReserveTile(13, 0xffffffffu);
            return true;
        }

        case EventKind::TowerOfCards: {
            // case 0x1f: "Create control spots around the current tile."
            // walks the 3x3 (dCol, dRow) box around the newest page tile;
            // for each offset whose hex distance is exactly 1 (= the 6
            // hex neighbours), addCell(hexMap, kind=4, fadeTimer=3) at
            // (tile.gridCol + dCol, tile.gridRow + dRow). kind 4 +
            // fadeTimer 3 = "control spot" tag the post-turn HexMap pass
            // reads to convert tiles placed there into control gains.
            if (pageList.empty()) {
                return false;
            }
            TileObject* newest = pageList.back();

            for (int dCol = -1; dCol < 2; ++dCol) {

                for (int dRow = -1; dRow < 2; ++dRow) {

                    if (hexGridDistance(0, 0, dCol, dRow) != 1) continue;

                    hexMap.addCell(
                        4,
                        newest->gridCol + dCol,
                        newest->gridRow + dRow,
                        3u);
                }
            }
            return true;
        }

        case EventKind::BruteForce: {
            // case 0x20: "Gain 3 {A} per held snag." counts rack tiles
            // whose getSnagIfAlive() is non-null.
            int snagCount = 0;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagIfAlive() == nullptr) continue;

                snagCount++;
            }

            if (snagCount == 0) {
                return false;
            }
            setATK(playerSystem.attack + snagCount * 3);
            return true;
        }

        case EventKind::InverseRefrain: {
            // case 0x21: "Swap your {A} and {D}." note: the HUD swap
            // visual receives the old (pre-swap) values; the tints
            // animate to their new resting positions while continuing
            // to display their old numbers until the slide settles.
            int oldAtk = playerSystem.attack;
            int oldDef = playerSystem.defence;

            if (oldAtk == oldDef) {
                return false;
            }
            playerSystem.attack  = oldDef;
            playerSystem.defence = oldAtk;
            hud.swapAtkDefDisplays(oldAtk, oldDef);
            return true;
        }

        case EventKind::FamiliarFaces: {
            // case 0x22: "Multiply the value of held {D} tiles by the
            // number of held snags." preconditions: >= 2 held snags and
            // >= 1 held DEF tile.
            int snagCount = 0;
            int defCount  = 0;

            for (int i = 0; i < 5; ++i) {

                if (rack[i] == nullptr) continue;

                if (rack[i]->getSnagIfAlive() != nullptr) snagCount++;

                if (rack[i]->getContentType()  == 3)      defCount++;
            }

            if (snagCount < 2) return false;
            if (defCount  < 1) return false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getContentType() != 3) continue;

                int m = tile->getContentMagnitude();
                pushStatTween(1, m * (snagCount - 1), &tile->mainQuad.posX, 0);
                TileContent* tc = tile->getTileContentIfAlive();
                tc->setMagnitude(m * snagCount);
            }
            return true;
        }

        case EventKind::ABriefPause: {
            // case 0x23: "Convert selected tiles to Pauses. Pauses stop
            // snags and give {A} {D}." stages non-special-snag tiles and
            // opens the panel. commit is special: the discard panel's
            // confirm icon (not a tile tap) calls fireEvent on the same
            // slot, which the prologue routes to the ABriefPause-commit
            // path (convert each staged tile to content type 0xe).
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    pushDiscardStagingEntry(i);
                }
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::BarricadedDoor: {
            // case 0x24: "Draw a Barricade tile." contentType 15 = Barricade.
            pushReserveTile(15, 0xffffffffu);
            return true;
        }

        case EventKind::SublimeRadiance: {
            // case 0x25: "Set {D} of held normal snags to 0, add the
            // {D} value to their {H}." per normal-snag rack tile (alive
            // snag with snagType == 1 AND nonzero def): HP += def, DEF = 0.
            snapTileBackToRack();
            bool any = false;
            float zero[2] = { 0.0f, 0.0f };

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagType() != 1) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc->def == 0) continue;

                sc->setHpDisplay(sc->def + sc->hp, this, zero);
                sc->setDefDisplay(0, this, zero);
                any = true;
            }
            return any;
        }

        case EventKind::AirSupport: {
            // case 0x26: "Halve {H} of held normal snags." precondition
            // per tile: snag.hp > 1 (no-op on 1-HP snags so they don't
            // round to 1 again). formula uses (hp + 1) / 2 = ceil(hp/2).
            snapTileBackToRack();
            bool any = false;
            float zero[2] = { 0.0f, 0.0f };

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagType() != 1) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc->hp <= 1) continue;

                sc->setHpDisplay((sc->hp + 1) / 2, this, zero);
                any = true;
            }
            return any;
        }

        case EventKind::ALongRoad: {
            // case 0x27: "Double the value of held {A} {H} {D} tiles
            // which have 2 exits." binary's bitmask (1 << contType) &
            // 0x4C != 0 selects contType in {2, 3, 6} = ATK / DEF / HP.
            snapTileBackToRack();
            bool any = false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getExitCount() != 2) continue;

                int ct = tile->getContentType();

                if (ct != 2 && ct != 3 && ct != 6) continue;

                int m = tile->getContentMagnitude();
                pushStatTween(1, m, &tile->mainQuad.posX, 0);
                TileContent* tc = tile->getTileContentIfAlive();
                tc->setMagnitude(m * 2);
                any = true;
            }
            return any;
        }

        case EventKind::HiddenWealth: {
            // case 0x28: "Convert random tiles to Wealth. Wealth gives
            // {C} and reduces {A}." builds a vector of non-special-snag
            // slot indices, then rng-pops 1..(N-1) of them and replaces
            // each picked tile's content with type 16 (Wealth).
            std::vector<int> candidates;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    candidates.push_back(i);
                }
            }

            if (candidates.empty()) {
                return false;
            }

            snapTileBackToRack();
            int n = static_cast<int>(candidates.size());
            int count = rngInt(1, n - 1, 4);

            for (int k = 0; k < count; ++k) {
                int pick    = rngInt(0, static_cast<int>(candidates.size()) - 1, 4);
                int slotIdx = candidates[pick];
                candidates.erase(candidates.begin() + pick);
                TileObject* tile = rack[slotIdx];

                if (tile) {
                    tile->erase();
                    tile->setTileContent(0x10u, 0);   // content type 16 = Wealth
                }
            }
            return true;
        }

        case EventKind::HypnoticGaze: {
            // case 0x29: "Move the nearest placed snag to the start of
            // the trail." walks pageList second-newest -> second-oldest
            // (excludes the newest, where the player stands, and the
            // oldest, the destination), finding the first live snag.
            // moves it via SnagContent::sendToward(oldest, reparent=false).
            if (pageList.size() < 2) {
                return false;
            }

            TileObject* oldest = pageList.front();

            // walk newest -> oldest (reverse) skipping the newest (which is
            // the player's position) and stopping when we reach oldest.
            auto rit = pageList.rbegin();
            ++rit;   // skip newest

            for (; rit != pageList.rend(); ++rit) {
                TileObject* tile = *rit;

                if (tile == oldest) {
                    break;   // reached the destination; nothing to move
                }

                SnagContent* sc = tile ? tile->getSnagIfAlive() : nullptr;

                if (sc != nullptr) {
                    sc->sendToward(oldest, false);
                    return true;
                }
            }
            return false;
        }

        case EventKind::Concentration: {
            // case 0x2a: "Halve the {D} of a held normal or special snag."
            // stages snag-holding rack tiles whose snag has nonzero def;
            // commit dispatch in applyHexPickupConsumeEffect halves the
            // snag's def display.
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || sc->def == 0) continue;

                pushDiscardStagingEntry(i);
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::GlaringHeadlights: {
            // case 0x2b: "Gain 2 {A} per placed {X} tile." counts content
            // type 4 (XP) tiles in the page list.
            if (pageList.empty()) return false;
            int xpCount = 0;

            for (TileObject* tile : pageList) {

                if (tile && tile->getContentType() == 4) {
                    xpCount++;
                }
            }

            if (xpCount == 0) return false;
            setATK(playerSystem.attack + xpCount * 2);
            return true;
        }

        case EventKind::Flying: {
            // case 0x2c: "Discard all non-snag held tiles. Gain the
            // value of {A} {H} {D} tiles." sums each discarded tile's
            // magnitude by content type, then applies via setATK / setDEF
            // / setHP. (binary always applies the setters even when sums
            // are 0; we mirror that.)
            snapTileBackToRack();
            bool any = false;
            int hpSum = 0, defSum = 0, atkSum = 0;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagIfAlive() != nullptr) continue;

                int ct = tile->getContentType();
                int m  = tile->getContentMagnitude();

                if      (ct == 6) hpSum  += m;
                else if (ct == 3) defSum += m;
                else if (ct == 2) atkSum += m;

                discardRackTile(i, false);
                any = true;
            }

            setATK(playerSystem.attack  + atkSum);
            setDEF(playerSystem.defence + defSum);
            setHP(static_cast<uint32_t>(playerSystem.currentHealth + hpSum));

            if (!any) return false;
            refillRackPostCommit();
            return true;
        }

        case EventKind::UnlockedDoor: {
            // case 0x2d: "Reduce the number of keys required to unlock
            // exit by 1." setExitKeysRequired clamps internally to [0, 4].
            setExitKeysRequired(keysRequired - 1);
            recomputeExitKeysCollected();
            return true;
        }

        case EventKind::ConsolidatePower: {
            // case 0x2e: "Combine all held {A} into one tile." precondition:
            // >= 2 ATK tiles. first ATK tile absorbs the others' magnitudes;
            // every subsequent ATK tile gets discarded.
            int atkCount = 0;
            for (int i = 0; i < 5; ++i) {
                if (rack[i] && rack[i]->getContentType() == 2) atkCount++;
            }
            if (atkCount < 2) return false;

            snapTileBackToRack();
            int         magSum   = 0;
            TileObject* firstAtk = nullptr;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getContentType() != 2) continue;

                if (firstAtk == nullptr) {
                    firstAtk = tile;
                } else {
                    magSum += tile->getContentMagnitude();
                    discardRackTile(i, false);
                }
            }

            if (firstAtk != nullptr) {
                TileContent* tc = firstAtk->getTileContentIfAlive();
                int          m  = firstAtk->getContentMagnitude();
                tc->setMagnitude(m + magSum);
                pushStatTween(1, magSum, &firstAtk->mainQuad.posX, 0);
            }
            refillRackPostCommit();
            return true;
        }

        case EventKind::StrongMedicine: {
            // case 0x2f: "Convert selected tiles to blanks. Gain 3 {D}
            // per held blank." stages non-special-snag tiles, opens the panel.
            // commit happens via fireEvent prologue when StrongMedicine is
            // re-tapped (already wired); the on-tile-tap path uses
            // togglePendingDiscardStage instead.
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    pushDiscardStagingEntry(i);
                }
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::Premonition: {
            // case 0x30: "Discard a selected tile. Draw a snag with 1 {A}."
            // stages non-special-snag tiles, opens the panel. commit
            // (applyHexPickupConsumeEffect 0x30) discards tapped tile +
            // pushes a normal snag with atk display = 1.
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    pushDiscardStagingEntry(i);
                }
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::WantonDestruction: {
            // case 0x31: "Discard 2 random {D} tiles. Draw a snag with 1 {H}."
            // precondition: >= 2 DEF tiles in rack.
            std::vector<int> defSlots;

            for (int i = 0; i < 5; ++i) {

                if (rack[i] && rack[i]->getContentType() == 3) {
                    defSlots.push_back(i);
                }
            }

            if (defSlots.size() < 2) {
                return false;
            }

            snapTileBackToRack();
            for (int k = 0; k < 2; ++k) {
                int pick    = rngInt(0, static_cast<int>(defSlots.size()) - 1, 4);
                int slotIdx = defSlots[pick];
                defSlots.erase(defSlots.begin() + pick);
                discardRackTile(slotIdx, false);
            }

            SnagContent* snag = pushReserveSnagTile(1u, 0xffffffffu);
            float zero[2] = { 0.0f, 0.0f };
            snag->setHpDisplay(1, nullptr, zero);
            refillRackPostCommit();
            return true;
        }

        case EventKind::OldFaith: {
            // case 0x32: "Draw one Faith tile per held {H}." precondition:
            // >= 1 HP tile in rack.
            int hpCount = 0;
            for (int i = 0; i < 5; ++i) {
                if (rack[i] && rack[i]->getContentType() == 6) hpCount++;
            }
            if (hpCount == 0) return false;
            for (int k = 0; k < hpCount; ++k) {
                pushReserveTile(0x14u, 0xffffffffu);   // content type 20 = Faith
            }
            return true;
        }

        case EventKind::LongRangeScan: {
            // case 0x33: "Draw a Foresight tile." content type 21 = Foresight.
            pushReserveTile(0x15u, 0xffffffffu);
            return true;
        }

        case EventKind::Rewind: {
            // case 0x34: "Discard the leftmost held tile. Affects special
            // snags." precondition: rack[0] non-null.
            if (rack[0] == nullptr) return false;
            snapTileBackToRack();
            discardRackTile(0, false);
            refillRackPostCommit();   // LAB_1000229f4
            return true;
        }

        case EventKind::Overcharge: {
            // case 0x35: "Double value of a selected {C} tile." stages
            // CTRL tiles (contentType == 5); commit
            // (applyHexPickupConsumeEffect 0x35) doubles tapped tile's
            // content magnitude.
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getContentType() != 5) continue;

                pushDiscardStagingEntry(i);
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::WellOiledMachine: {
            // case 0x36: "Divide {H} of held normal snags by the number
            // of held {C} tiles." precondition: >= 2 CTRL tiles. per
            // normal-snag tile with snag.hp > 1: newHp = max(1, hp / ctrlCount).
            int ctrlCount = 0;
            for (int i = 0; i < 5; ++i) {
                if (rack[i] && rack[i]->getContentType() == 5) ctrlCount++;
            }
            if (ctrlCount < 2) return false;

            snapTileBackToRack();
            bool any = false;
            float zero[2] = { 0.0f, 0.0f };

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagType() != 1) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc->hp <= 1) continue;

                unsigned newHp = static_cast<unsigned>(sc->hp)
                               / static_cast<unsigned>(ctrlCount);
                if (newHp < 2) newHp = 1;
                sc->setHpDisplay(static_cast<int>(newHp), this, zero);
                any = true;
            }
            return any;
        }

        case EventKind::SweetMemento: {
            // case 0x37: "Convert {H} tiles to Mementos. Mementos give
            // {A} {H} {D}." per HP tile in rack: erase + setTileContent
            // (0x16 = Memento) preserving the original magnitude.
            bool any = false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getContentType() != 6) continue;

                int m = tile->getContentMagnitude();
                tile->erase();
                tile->setTileContent(0x16u, m);   // content type 22 = Memento
                any = true;
            }
            return any;
        }

        case EventKind::WarmGlow: {
            // case 0x38: "Draw a Warmth tile." content type 23 = Warmth.
            pushReserveTile(0x17u, 0xffffffffu);
            return true;
        }

        case EventKind::Sideswipe: {
            // case 0x39: "Halve {A} of a normal held snag." stages normal-
            // snag tiles; commit (applyHexPickupConsumeEffect 0x39) halves
            // the snag's atk display.
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagType() != 1) continue;

                pushDiscardStagingEntry(i);
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::StrongVitals: {
            // case 0x3a: "Gain {H} equal to Nemesis XP. Gain {D} equal
            // to Nemesis level."
            setHP(static_cast<uint32_t>(nemesis.nemesisXP
                                       + playerSystem.currentHealth));
            setDEF(nemesis.nemesisLevel + playerSystem.defence);
            return true;
        }

        case EventKind::Stampede: {
            // case 0x3b: "Advance Nemesis to next snag. Set that snag's
            // {D} to 0." walks page list oldest -> newest, finds first
            // live snag, sets its def display to 0, then nemesis.eatTarget
            // = max(distance, eatTarget). distance starts at 0 (oldest).
            snapTileBackToRack();
            if (pageList.empty()) return false;

            int          distance = 0;
            SnagContent* found    = nullptr;

            for (TileObject* tile : pageList) {
                SnagContent* sc = tile ? tile->getSnagIfAlive() : nullptr;

                if (sc != nullptr) {
                    found = sc;
                    break;
                }
                distance++;
            }

            if (found == nullptr) return false;

            found->setDefDisplay(0, this, &positionX);

            if (distance < nemesis.eatTarget) {
                distance = nemesis.eatTarget;
            }
            nemesis.eatTarget = distance;
            nemesis.eatActive = false;   // LAB_1000234dc
            return true;
        }

        case EventKind::LuckyDraw: {
            // case 0x3c: "Creates Good Luck tokens at exits of current tile."
            // for each of the 6 hex directions that the newest tile permits,
            // place a hexMap cell kind=5 (good luck token) at the neighbour
            // hex if it isn't already occupied. binary 6-direction offsets
            // are hard-coded (= the only place this exact axial table
            // appears; we mirror it inline).
            if (pageList.empty()) return false;
            snapTileBackToRack();
            TileObject* newest = pageList.back();

            for (int dir = 0; dir < 6; ++dir) {

                if (!newest->permitsDirection(dir, -1)) continue;

                int col = newest->gridCol;
                int row = newest->gridRow;

                switch (dir) {
                    case 0: row -= 1;            break;
                    case 1: col += 1;            break;
                    case 2: col += 1; row += 1;  break;
                    case 3: row += 1;            break;
                    case 4: col -= 1;            break;
                    case 5: col -= 1; row -= 1;  break;
                }

                if (cellIsOccupied(col, row)) continue;
                hexMap.addCell(5, col, row, 3u);
            }
            return true;
        }

        case EventKind::HardWork: {
            // case 0x3d: "Draw an Effort tile." content type 24 = Effort.
            pushReserveTile(0x18u, 0xffffffffu);
            return true;
        }

        case EventKind::FlameBreath: {
            // case 0x3e: "Gain {A} {D} equal to your {C} multiplied by
            // your event count." {C} value = controlReceivedTotal % 10
            // (the current control gauge). event count = active HUD slots.
            int controlMod = hud.controlReceivedTotal % 10;
            int eventCount = 0;

            for (int i = 0; i < 4; ++i) {
                if (hud.eventTray[i].slotPtr) eventCount++;
            }
            int gain = eventCount * controlMod;
            if (gain == 0) return false;
            setATK(playerSystem.attack  + gain);
            setDEF(playerSystem.defence + gain);
            return true;
        }

        case EventKind::Snapshot: {
            // case 0x3f: "Gain 1 {X} immediately." LAB_100023618.
            hud.advanceXPSlot(1, false);
            return true;
        }

        case EventKind::AmmoDump: {
            // case 0x40: "Convert all held {A} tiles to value 2 {C} tiles."
            bool any = false;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getContentType() != 2) continue;

                tile->erase();
                tile->setTileContent(5u, 2);
                any = true;
            }

            if (!any) return false;
            snapTileBackToRack();
            return true;
        }

        case EventKind::Backtrack: {
            // case 0x41: "Convert a held tile to Milestone, which gives
            // {H} and moves a snag." stages non-special-snag tiles, opens
            // the panel. commit (applyHexPickupConsumeEffect 0x41) erases +
            // setTileContent(0x19 = Milestone, 1).
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    pushDiscardStagingEntry(i);
                }
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::Disarm: {
            // case 0x42: "Create Weakness tokens around current tile."
            // 6-neighbor pattern at hex distance 1, kind=6 (weakness).
            if (pageList.empty()) return false;
            TileObject* newest = pageList.back();

            for (int dCol = -1; dCol < 2; ++dCol) {

                for (int dRow = -1; dRow < 2; ++dRow) {

                    if (hexGridDistance(0, 0, dCol, dRow) != 1) continue;

                    hexMap.addCell(
                        6,
                        newest->gridCol + dCol,
                        newest->gridRow + dRow,
                        3u);
                }
            }
            return true;
        }

        case EventKind::SafetyInNumbers: {
            // case 0x43: "Combine held normal snags, draw {H} tiles to
            // replace combined tiles." precondition: >= 2 normal snags.
            // first normal-snag tile's snag absorbs all the others' stats;
            // every subsequent normal-snag slot gets discarded + replaced
            // with an HP tile draw.
            std::vector<int> snagSlots;

            for (int i = 0; i < 5; ++i) {

                if (rack[i] == nullptr) continue;
                if (rack[i]->getSnagType() != 1) continue;

                snagSlots.push_back(i);
            }

            if (snagSlots.size() < 2) {
                return false;
            }

            snapTileBackToRack();
            int atkSum = 0, defSum = 0, hpSum = 0;

            for (size_t k = 0; k < snagSlots.size(); ++k) {
                SnagContent* sc = rack[snagSlots[k]]->getSnagIfAlive();
                int snagAtk = sc->atk;
                int snagDef = sc->def;
                int snagHp  = sc->hp;

                if (k != 0) {
                    discardRackTile(snagSlots[k], false);
                    pushReserveTile(6u, 0xffffffffu);   // content type 6 = HP
                }
                atkSum += snagAtk;
                defSum += snagDef;
                hpSum  += snagHp;
            }

            SnagContent* firstSnag = rack[snagSlots[0]]->getSnagIfAlive();
            float zero[2] = { 0.0f, 0.0f };
            firstSnag->setAtkDisplay(atkSum, this, zero);
            firstSnag->setDefDisplay(defSum, this, zero);
            firstSnag->setHpDisplay (hpSum,  this, zero);
            refillRackPostCommit();
            return true;
        }

        case EventKind::Breakthrough: {
            // case 0x44: "Discard a held normal snag. Gain half its {D}."
            // stages normal-snag tiles, opens the panel. commit
            // (applyHexPickupConsumeEffect 0x44) reads snag.def, discards
            // the tile, then setDEF(def + snag.def / 2).
            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagType() != 1) continue;

                pushDiscardStagingEntry(i);
            }
            openTileSelectionPanel();
            return false;
        }

        case EventKind::FearlessDefence: {
            // case 0x45: "Discard a random tile, draw {D} valued at number
            // of held exits." rng-pop one non-special-snag slot, discard,
            // sum exits across remaining rack tiles, draw a DEF tile with
            // that magnitude.
            std::vector<int> candidates;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    candidates.push_back(i);
                }
            }

            if (candidates.empty()) {
                return false;
            }

            snapTileBackToRack();
            int pick    = rngInt(0, static_cast<int>(candidates.size()) - 1, 4);
            int slotIdx = candidates[pick];
            discardRackTile(slotIdx, false);

            int exitSum = 0;
            for (int i = 0; i < 5; ++i) {
                if (rack[i]) {
                    exitSum += rack[i]->getExitCount();
                }
            }

            TileObject* drawn = pushReserveTile(3u, 0xffffffffu);

            if (drawn) {
                TileContent* tc = drawn->getTileContentIfAlive();
                if (tc) tc->setMagnitude(exitSum);
            }
            refillRackPostCommit();
            return true;
        }

        case EventKind::Clocktower: {
            // case 0x46: "Creates Talent tokens around the current tile,
            // these double {A} {H} {D}." places kind=7 hexMap cells along
            // two axes radiating from the newest page tile: the (col==dRow)
            // diagonal and the (dCol==0) vertical, distance up to 2.
            // 8 cells total, center skipped.
            if (pageList.empty()) return false;
            TileObject* newest = pageList.back();

            for (int dCol = -2; dCol < 3; ++dCol) {

                for (int dRow = -2; dRow < 3; ++dRow) {

                    if (dCol == 0 && dRow == 0) continue;
                    bool onDiagonal = (dCol == dRow);
                    bool onVertical = (dCol == 0);

                    if (!onDiagonal && !onVertical) continue;

                    hexMap.addCell(
                        7,
                        newest->gridCol + dCol,
                        newest->gridRow + dRow,
                        3u);
                }
            }
            return true;
        }

        case EventKind::IdyllicLandscape: {
            // case 0x47: "Move all placed snags to the start of the trail."
            // compacts snags toward the oldest page tile: each placed snag
            // moves to the oldest empty tile that's older than it.
            if (pageList.empty()) return false;
            bool any = false;

            auto destCursor = pageList.begin();

            for (auto srcIt = pageList.begin(); srcIt != pageList.end(); ++srcIt) {
                TileObject*  src = *srcIt;
                SnagContent* sc  = src ? src->getSnagIfAlive() : nullptr;

                if (sc == nullptr) continue;
                if (destCursor == srcIt) continue;   // dest caught up to src

                while (destCursor != srcIt) {
                    TileObject* dest = *destCursor;

                    if (dest && dest->getSnagIfAlive() == nullptr) {
                        sc->sendToward(dest, false);
                        any = true;
                        ++destCursor;
                        break;
                    }
                    ++destCursor;
                }
            }
            return any;
        }

        case EventKind::InnerStrength: {
            // case 0x48: "Gain {A} equal to half your {D}. Gain {D} equal
            // to half your {A}." precondition: at least one of ATK, DEF >= 2
            // (otherwise both halves round to 0 and there's no gain).
            int oldAtk = playerSystem.attack;
            int oldDef = playerSystem.defence;

            if (oldAtk < 2 && oldDef < 2) {
                return false;
            }
            setATK(oldAtk + oldDef / 2);
            setDEF(oldDef + oldAtk / 2);
            return true;
        }

        case EventKind::PuppetMaster: {
            // case 0x49: "Gain 1 {C} per 2 held snags." precondition:
            // >= 2 held snags.
            int snagCount = 0;
            for (int i = 0; i < 5; ++i) {
                if (rack[i] && rack[i]->getSnagIfAlive()) snagCount++;
            }
            if (snagCount < 2) return false;
            hud.advanceCTRLSlot(snagCount / 2, false);   // LAB_100023000
            return true;
        }

        case EventKind::EmergencyBroadcast: {
            // case 0x4a: "Discard 2 random tiles. Draw 2 {A} tiles."
            // precondition: >= 2 non-special-snag tiles available.
            std::vector<int> candidates;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    candidates.push_back(i);
                }
            }

            if (candidates.size() < 2) {
                return false;
            }

            snapTileBackToRack();
            for (int k = 0; k < 2; ++k) {
                int pick    = rngInt(0, static_cast<int>(candidates.size()) - 1, 4);
                int slotIdx = candidates[pick];
                candidates.erase(candidates.begin() + pick);
                discardRackTile(slotIdx, false);
            }
            pushReserveTile(2u, 0xffffffffu);
            pushReserveTile(2u, 0xffffffffu);
            refillRackPostCommit();
            return true;
        }

        case EventKind::Countdown: {
            // case 0x4b: "Draw an Honesty tile." Honesty (snag type 0x6c)
            // is a snag, not a content tile: pushReserveSnagTile (not
            // pushReserveTile). when placed it moves to the trail start
            // and gains 1 HP per turn, eventually delivering its HP as
            // ATK to the player on contact.
            pushReserveSnagTile(0x6cu, 0xffffffffu);
            return true;
        }

        case EventKind::CovertStrike: {
            // case 0x4c: "Defeat the nearest placed normal snag. Gain 1 {X}."
            // walks page list newest -> oldest (reverse), finds first tile
            // whose snagType == 1, kills + converts to XP tile (preserving
            // the snag's tier as XP magnitude), then advanceXPSlot(1).
            if (pageList.empty()) {
                return false;
            }

            TileObject* found = nullptr;

            for (auto rit = pageList.rbegin();
                 rit != pageList.rend();
                 ++rit) {
                TileObject* tile = *rit;

                if (tile && tile->getSnagType() == 1) {
                    found = tile;
                    break;
                }
            }

            if (found == nullptr) return false;

            SnagContent* sc   = found->getSnagIfAlive();
            int          tier = sc->tier();

            if (found->getContentType() == 4) {
                TileContent* tc = found->getTileContentIfAlive();
                int          m  = found->getContentMagnitude();
                tc->setMagnitude(m + tier);
            } else {
                found->setTileContent(4u, tier);
            }
            sc->killAndFade();
            hud.advanceXPSlot(1, false);   // LAB_100023618
            return true;
        }

        case EventKind::SapphireCity: {
            // case 0x4d: "Gain 1 {D} per 3 placed tiles." precondition:
            // pageList.size() >= 3.
            const size_t pageSize = pageList.size();
            if (pageSize < 3) return false;
            setDEF(playerSystem.defence + static_cast<int>(pageSize / 3));
            return true;
        }

        case EventKind::CostlyGift: {
            // case 0x4e: "Discard 1-5 random tiles. Gain 1 {X}." builds
            // a vector of non-special-snag slots, rng-picks 1..size of
            // them to discard, then advanceXPSlot(1).
            std::vector<int> candidates;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;

                SnagContent* sc = tile->getSnagIfAlive();

                if (sc == nullptr || tile->getSnagType() == 1) {
                    candidates.push_back(i);
                }
            }

            if (candidates.empty()) {
                return false;
            }

            snapTileBackToRack();
            int n = rngInt(1, static_cast<int>(candidates.size()), 4);

            for (int k = 0; k < n; ++k) {
                int pick    = rngInt(0, static_cast<int>(candidates.size()) - 1, 4);
                int slotIdx = candidates[pick];
                candidates.erase(candidates.begin() + pick);
                discardRackTile(slotIdx, false);
            }
            hud.advanceXPSlot(1, false);
            refillRackPostCommit();
            return true;
        }

        case EventKind::PureEfficiency: {
            // case 0x4f: "Discard all held {C} tiles. Gain 1 {C} per 2
            // tiles discarded." precondition: >= 1 CTRL tile.
            int ctrlCount = 0;
            for (int i = 0; i < 5; ++i) {
                if (rack[i] && rack[i]->getContentType() == 5) ctrlCount++;
            }
            if (ctrlCount == 0) return false;

            snapTileBackToRack();
            for (int i = 0; i < 5; ++i) {
                if (rack[i] && rack[i]->getContentType() == 5) {
                    discardRackTile(i, false);
                }
            }
            hud.advanceCTRLSlot(ctrlCount / 2, false);
            refillRackPostCommit();   // LAB_1000229f4
            return true;
        }

        case EventKind::Exhaustion: {
            // case 0x50: "Discard a random non-snag tile." rng-pop one
            // slot whose getSnagIfAlive() is null, discard.
            std::vector<int> candidates;

            for (int i = 0; i < 5; ++i) {
                TileObject* tile = rack[i];

                if (tile == nullptr) continue;
                if (tile->getSnagIfAlive() != nullptr) continue;

                candidates.push_back(i);
            }

            if (candidates.empty()) {
                return false;
            }

            snapTileBackToRack();
            int pick    = rngInt(0, static_cast<int>(candidates.size()) - 1, 4);
            int slotIdx = candidates[pick];
            discardRackTile(slotIdx, false);
            refillRackPostCommit();
            return true;
        }

        // unreached for in-range eventType values (0..0x50). out-of-range
        // values hit the binary's switch fall-through to `return true`;
        // we mirror that here.
        default:
            break;
    }

    return true;
}

// FUN_10001ae10 tail starting at the `*plVar1 != *plVar2` branch.
// see game_board.h for the high-level contract.
void GameBoard::commitPendingDiscards() {
    hud.setConditionalIcon(GameplayHUD::ConditionalIconState::Default);

    // pass 1: assemble involved rack tiles into a local list, used by the
    // achievement batch-discard fan-out below.
    std::list<TileObject*> involved;

    for (const DiscardEntry& e : hud.pendingDiscards) {

        if (!e.staged) {
            continue;
        }

        if (e.rackSlot >= 0 && e.rackSlot < RACK_SLOT_COUNT) {
            involved.push_back(rack[e.rackSlot]);
        }
    }

    // achievement batch-discard fan-out (= binary's FUN_10004dfbc).
    // EasyComeEasyGo per tile; the three terminal milestones gate on
    // counts assembled inside the walk.
    {
        AchievementTracker& tracker = getGame()->achievementTracker();
        int snagCount        = 0;
        int controlMag2Count = 0;
        int emptyCount       = 0;

        for (TileObject* tile : involved) {

            if (!tile) {
                continue;
            }

            tracker.increment(AchievementId::EasyComeEasyGo);

            SnagContent* sc = tile->getSnagIfAlive();

            if (sc != nullptr) {
                snagCount++;

                // non-generic snags fire LikeTearsInTheRain per tile.
                if (sc->type != 1) {
                    tracker.increment(AchievementId::LikeTearsInTheRain);
                }
            }

            if (tile->getContentType() == 5 && tile->getContentMagnitude() == 2) {
                controlMag2Count++;
            }

            if (tile->getTileContentIfAlive() == nullptr
                && tile->getSnagIfAlive() == nullptr) {
                emptyCount++;
            }
        }

        if (snagCount == 5) {
            tracker.increment(AchievementId::SpringCleaning);
        }

        if (controlMag2Count > 2) {
            tracker.increment(AchievementId::LostControl);
        }

        if (emptyCount == 5) {
            tracker.increment(AchievementId::TheSoundOfSilence);
        }
    }

    // pass 2: accumulate XP + score, dispatch per-tile effects, discard
    // each committed rack tile.
    int nemesisXP    = 0;
    int contentCount = 0;
    int committed    = 0;

    for (const DiscardEntry& e : hud.pendingDiscards) {

        if (!e.staged) {
            continue;
        }

        int          slotIdx = e.rackSlot;
        TileObject*  tile    = (slotIdx >= 0 && slotIdx < RACK_SLOT_COUNT)
                               ? rack[slotIdx] : nullptr;
        SnagContent* snag    = tile ? tile->getSnagIfAlive() : nullptr;

        if (snag == nullptr) {
            int t = tile ? tile->getContentType() : 0;

            if (t == 7) {
                nemesisXP += tile->getContentMagnitude();
            } else if (t == 6) {
                contentCount++;
            }
        } else {
            // discarding a snag normally feeds Nemesis +1 XP. Scapegoat (0x49)
            // is excluded here because its own discard path (discardRackTile's
            // 0x49 branch) already levels Nemesis fully; avoid double-counting.
            if (snag->type != (int)SnagKind::Scapegoat) {
                nemesisXP++;
            }

            if (playerSystem.perkLevel(7) > 0) {
                pushReserveTile(3u, 0xFFFFFFFFu);
            }
        }

        discardRackTile(slotIdx, /*skipExtraEffects=*/false);
        committed++;
    }

    if (committed > 0) {
        bool skipSpawn = false;

        if (contentCount > 0) {
            int p10 = playerSystem.perkLevel(10);

            // perkLevel(10) determines whether content-6 commits skip the
            // nemesis spawn. tier 1 only skips while HP < max; tier 2+
            // always skips.
            if (p10 >= 1) {
                int p10again = playerSystem.perkLevel(10);

                if (p10again >= 2 || playerSystem.currentHealth >= playerSystem.maxHealth) {
                    skipSpawn = true;
                }
            }
        }

        if (!skipSpawn) {
            nemesisSpawnAtTrailTail();
        }

        nemesisAdvance(nemesisXP);

        if (contentCount > 0) {
            int p2     = playerSystem.perkLevel(2);
            int newATK = playerSystem.attack  + p2 * contentCount;
            int newDEF = playerSystem.defence + p2 * contentCount;
            setATK(newATK);
            setDEF(newDEF);
        }

        // Incompetence (snag 0x5B) anywhere on the board pushes a content-0
        // reserve tile on each batch.
        if (hasSnagInBoard(0x5b)) {
            pushReserveTile(0u, 0xFFFFFFFFu);
        }

        // pass 3: rack-walk stat-change tween dispatch for tiles whose
        // displayed stats track the committed count.
        for (int s = 0; s < RACK_SLOT_COUNT; s++) {
            TileObject* tile = rack[s];

            if (!tile) {
                continue;
            }

            SnagContent* snag    = tile->getSnagIfAlive();
            TileContent* content = (snag == nullptr) ? tile->getTileContentIfAlive() : nullptr;

            if (content) {
                int t = content->type;

                if (t == 0x12) {
                    int newMag = content->displayedMagnitude - committed;

                    if (newMag <= 0) {
                        content->killAndFade();
                        // achievement "Keep It Safe" (= binary's
                        // FUN_10004e364). fires unconditionally on Secret
                        // (content type 0x12) exhaust.
                        getGame()->achievementTracker().increment(AchievementId::KeepItSafe);
                    } else {
                        content->setRawAndDisplayMagnitude(newMag);
                        float src[2] = { tile->mainQuad.posX, tile->mainQuad.posY };
                        pushStatTween(/*textStyle*/ 1, -committed, src, /*direction*/ 0);
                    }
                } else if (t == 0x0B) {
                    int cap  = 99 - content->displayedMagnitude;
                    int gain = (committed <= cap) ? committed : cap;

                    if (gain > 0) {
                        content->setRawAndDisplayMagnitude(content->displayedMagnitude + gain);
                        float src[2] = { tile->mainQuad.posX, tile->mainQuad.posY };
                        pushStatTween(/*textStyle*/ 1, gain, src, /*direction*/ 0);
                    }
                }

                continue;
            }

            if (!snag) {
                continue;
            }

            if (snag->type == 0x59) {  // Defiance: atk docks with commit count
                int newAtk = snag->atk - committed;

                if (newAtk < 1) {
                    newAtk = 0;
                }

                float zero[2] = { 0.0f, 0.0f };
                snag->setAtkDisplay(newAtk, this, zero);
            } else if (snag->type == 0x47) {  // Infestation: def docks
                int newDef = snag->def - committed;

                if (newDef < 1) {
                    newDef = 0;
                }

                float zero[2] = { 0.0f, 0.0f };
                snag->setDefDisplay(newDef, this, zero);
            }
        }
    }

    hud.pendingDiscards.clear();
}

// reconstructed from Ghidra FUN_1000208f8.
//
// the content-type blacklist (0xC0300) covers {8, 9, 18, 19}: tile kinds
// the discard button cannot trash directly. snags require type 1 (Snag) or
// 0x49 (Scapegoat) and no blocker snag in rack (types 6, 0x22, 0x29). Zeal
// (0x69) in board prevents discarding empty tiles.
bool GameBoard::canDiscardRackTile(TileObject* tile) {
    int contentType = tile->getContentType();

    bool contentAllowed = (contentType >= 0x14)
                       || ((1u << (contentType & 0x1Fu)) & 0xC0300u) == 0;

    SnagContent* snag = tile->getSnagIfAlive();
    bool snagAllowed;

    if (snag == nullptr) {
        snagAllowed = true;
    } else {
        snagAllowed = (snag->type == 1 || snag->type == 0x49)
                   && findSnagInRack(6)    == nullptr
                   && findSnagInRack(0x22) == nullptr
                   && findSnagInRack(0x29) == nullptr;
    }

    if (!contentAllowed || !snagAllowed) {
        return false;
    }

    if (tile->hasActiveDecorationOfKind(0)) {
        return false;
    }

    // non-empty tile always passes; empty tile passes only if Zeal isn't
    // protecting empty slots.
    if (tile->getTileContentIfAlive() != nullptr) {
        return true;
    }

    if (tile->getSnagIfAlive() != nullptr) {
        return true;
    }

    return !hasSnagInBoard(0x69);  // Zeal
}

// reconstructed from Ghidra FUN_100023940. binary writes a vtable pointer
// to the entry's Quad subobject; we leave Quad's existing vtable in place
// since DiscardEntry doesn't override anything we use.
void GameBoard::pushDiscardStagingEntry(int slotIdx) {

    if (hud.pendingDiscards.empty()) {
        hud.pendingDiscardBobTimer = 0.0f;
    }

    hud.pendingDiscards.emplace_back();
    DiscardEntry& e = hud.pendingDiscards.back();

    e.quad.setTexCoords(0.0f, 0.65234375f, 0.06738281f, 0.75292969f);
    e.quad.setSize(0.10781250f, 0.16093750f);
    e.quad.posX = -10.0f;  // off-screen seed; animateCleanupQuadBob bobs it into place
    e.quad.posY = -10.0f;
    e.rackSlot  = slotIdx;
    e.staged    = 0;
}

// reconstructed from Ghidra FUN_100017b44. sets hud.conditionalFlag (=
// "show the discard button this frame") when the game is in a state that
// can discard at least one rack tile.
void GameBoard::refreshDiscardButtonAvailability() {
    bool show = false;

    bool gameIdle = !pageList.empty()
                 && selectedRackSlot == -1
                 && draggedRackSlot  == -1
                 && hud.selectedEvent == nullptr
                 && findSnagInRack(0x34)  == nullptr   // Greed in rack blocks discard
                 && findSnagInPages(0x54) == nullptr;  // Frenzy in pages blocks discard

    if (gameIdle) {

        if (findSnagInRack(0x75) != nullptr) {
            // Tantrum in rack: discard is still allowed (constrained to slot 4).
            show = true;
        } else {

            for (int i = 0; i < RACK_SLOT_COUNT; i++) {

                if (rack[i] && canDiscardRackTile(rack[i])) {
                    show = true;
                    break;
                }
            }
        }
    }

    hud.conditionalFlag = show;
}

// reconstructed from Ghidra FUN_100025d2c. flips the staged byte and
// slides the underlying rack tile to its new visual position. each slide
// target uses the same rack-column X (slot * 0.195 + 0.109); only Y
// differs: the "preview" row is the normal rack row, and the "selected"
// row sits one nudge above (-0.0625).
void GameBoard::togglePendingDiscardStage(DiscardEntry& entry) {
    uint8_t wasStaged = entry.staged;
    entry.staged      = wasStaged ^ 1u;

    constexpr float COL_STRIDE      = 0.1953125f;
    constexpr float COL_X_OFFSET    = 0.10937500f;
    constexpr float PREVIEW_Y_NUDGE = -0.09843750f + -0.02031250f;
    constexpr float SELECTED_Y_DROP = -0.06250000f;
    const float     VIRTUAL_HEIGHT  = Renderer::getVirtualHeight();

    int slotIdx = entry.rackSlot;

    if (slotIdx < 0 || slotIdx >= RACK_SLOT_COUNT) {
        return;
    }

    TileObject* tile = rack[slotIdx];

    if (!tile) {
        return;
    }

    float target[2] = { (float)slotIdx * COL_STRIDE + COL_X_OFFSET, 0.0f };

    if (wasStaged == 1) {
        // toggled back to staged=0: tile slides up to the preview row.
        target[1] = VIRTUAL_HEIGHT + PREVIEW_Y_NUDGE;
        tile->setRackPosition(0.0f, target, /*immediate=*/true, /*flag=*/false);
    } else {
        // toggled to staged=1: tile slides down to the selected-to-discard row.
        target[1] = VIRTUAL_HEIGHT + PREVIEW_Y_NUDGE + SELECTED_Y_DROP;
        tile->setRackPosition(0.0f, target, /*immediate=*/false, /*flag=*/false);
    }

    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x10);
    }
}

// FUN_10002692c. snap a staged tile back to the preview row without
// flipping its staged byte. used by fireEvent's prologue when abandoning
// stale entries before firing a fresh event (the caller is about to pop
// the entry anyway, so toggling the byte first would just waste a write).
//
// position math matches togglePendingDiscardStage's wasStaged==1 branch
// exactly: same column-X (slotIdx * 0.195 + 0.109), same Y (virtualHeight
// + preview nudge), and the same sound 0x10. only difference is no
// `entry.staged ^= 1` and no choice between preview / selected Y rows.
void GameBoard::unstageDiscardEntryVisual(DiscardEntry& entry) {
    constexpr float COL_STRIDE      = 0.1953125f;
    constexpr float COL_X_OFFSET    = 0.10937500f;
    constexpr float PREVIEW_Y_NUDGE = -0.09843750f + -0.02031250f;
    const float     VIRTUAL_HEIGHT  = Renderer::getVirtualHeight();

    int slotIdx = entry.rackSlot;

    if (slotIdx < 0 || slotIdx >= RACK_SLOT_COUNT) {
        return;
    }

    TileObject* tile = rack[slotIdx];

    if (!tile) {
        return;
    }

    float target[2] = {
        (float)slotIdx * COL_STRIDE + COL_X_OFFSET,
        VIRTUAL_HEIGHT + PREVIEW_Y_NUDGE,
    };
    tile->setRackPosition(0.0f, target, /*immediate=*/true, /*flag=*/false);

    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x10);
    }
}

// reconstructed from Ghidra FUN_100025d48. scan pendingDiscards; if any
// entry is staged for commit, switch the conditional icon to confirm,
// else back to staging-active.
void GameBoard::refreshDiscardConfirmIcon() {
    bool anyStaged = false;

    for (const DiscardEntry& e : hud.pendingDiscards) {

        if (e.staged != 0) {
            anyStaged = true;
            break;
        }
    }

    if (anyStaged) {
        hud.setConditionalIcon(GameplayHUD::ConditionalIconState::ConfirmDiscard);
    } else {
        hud.setConditionalIcon(GameplayHUD::ConditionalIconState::StagingActive);
    }
}

// FUN_100020880. true if the given hex (col, row) is already occupied by:
//   a) the Nemesis's reserved cell, when the nemesis is alive (visible byte
//      at +0x9D0 set), or
//   b) any tile in the page list (gridCol/gridRow match).
bool GameBoard::cellIsOccupied(int col, int row) const {

    if (nemesis.visible &&
        nemesis.nemesisGridCol == col &&
        nemesis.nemesisGridRow == row) {
        return true;
    }

    for (const TileObject* t : pageList) {

        if (t && t->gridCol == col && t->gridRow == row) {
            return true;
        }
    }

    return false;
}

// reconstructed from Ghidra FUN_100024bd8.
int GameBoard::countAdjacentPageTiles(TileObject* tile) const {
    int count = 0;

    for (const TileObject* t : pageList) {

        if (t && hexGridDistance(tile->gridCol, tile->gridRow,
                                  t->gridCol, t->gridRow) == 1) {
            count++;
        }
    }
    return count;
}

// reconstructed from Ghidra FUN_100025c84.
void GameBoard::applyTileTypeEffect(int eventTypeKey) {
    SnagContent* snag1e = findSnagInRack(0x1e);

    if (!snag1e) {
        // no snag-0x1e in rack: charge the matching event slot directly.
        hud.pushEventCharge(eventTypeKey);
        return;
    }

    if (!hud.canPushEventCharge(eventTypeKey)) {
        return;
    }

    // a charge could land but snag-0x1e blocks the immediate apply, so queue
    // it onto the action queue with the snag's parent tile as tileRef. when
    // selectedRackSlot is the snag's parent tile, origin = (positionX,
    // positionY) so the burst tracks the held tile; otherwise origin =
    // (0, 0) and tileRef alone drives placement.
    TileObject* snagParent = snag1e->tileParent;
    float origin[2] = { 0.0f, 0.0f };

    if (selectedRackSlot != -1 && rack[selectedRackSlot] == snagParent) {
        origin[0] = positionX;
        origin[1] = positionY;
    }

    pushAction(0, origin, snagParent);
}

// reconstructed from Ghidra FUN_100024cc8.
//
// fired by tile-commit pipeline once a rack tile is placed onto the hex
// grid. dispatches in 7 sections:
//   1. visited-cell kind switch (1/3/5/6) on the just-occupied hex
//   2. snag-0x28 ("Fear") grow: register a kind-3 marker on a random
//      unoccupied permitted-direction neighbor of the placed tile
//   3. snag-100 ("Neglect") draws a blank reserve tile when an ATK or
//      DEF tile lands
//   4. snag-2 ("Hound") damage tally: the placed tile carries kind-2
//      decoration "value" stacks; heal HP by that amount, push burst,
//      clear the stack, play sound 0x3E
//   5. contentType==2 (ATK tile) + perk-6 ("Strength") -> DEF boost
//   6. snag-0x2e ("Pride") rack walk: boost the snag's stat that
//      matches the placed tile's contentType (2/3/6 -> atk/def/hp) by
//      half the placed tile's magnitude
//   7. snag-0x2e page walk: same as 6 but iterating page-list tiles
//      (so newly-placed Pride tiles also get the boost from already-
//      placed tiles)
//
// `placedTile` is the tile that was just dropped onto the grid: the
// page list's head (= sentinel.next) immediately after the commit.
void GameBoard::dispatchHexMapPostCommit(TileObject* placedTile) {

    if (!placedTile) {
        return;
    }

    // ===================================================================
    // Section 1: visited-cell kind dispatch
    // ===================================================================
    // when no cell exists at the visited hex (cellKind == 0), Section 1
    // fully skips, including the resetCell tail. matches the binary's
    // `goto switchD_caseD_0` past the FUN_10003b458 call.
    int cellKind = hexMap.cellKindAt(placedTile->gridCol, placedTile->gridRow);

    if (cellKind != 0) {

        switch (cellKind) {
            case 1: {
                // exit / goal cell. set the per-level "exit visited" byte.
                exitReached = 1;
                break;
            }

            case 3:
                pushReserveSnagTile(1, 0xFFFFFFFFu);
                break;

            case 5:
                hud.advanceXPSlot(1, 0);
                break;

            case 6: {
                SnagContent* snag = placedTile->getSnagIfAlive();

                if (snag) {
                    // halve the snag's atk; tween anchored on the stat
                    // queue with origin = GameBoard's positionXY so the
                    // "-N" marker animates over the board's current
                    // scroll point.
                    float anchor[2] = { positionX, positionY };
                    snag->setAtkDisplay(snag->atk >> 1, this, anchor);
                }
                break;
            }

            default:
                break;
        }

        // try to consume the visited cell. only fires when cell.fadeTimer
        // == 3 (binary disasm gate); cells from layout pass (kind 1 / 2,
        // fadeTimer 0) and snag-0x28 grow (kind 3, fadeTimer 2) are
        // not consumed by this; only the fadeTimer==3 producer cells are.
        hexMap.tryConsumeCellAt(placedTile->gridCol, placedTile->gridRow);
    }


    // ===================================================================
    // Section 2: snag-0x28 ("Fear") grow
    // ===================================================================
    // already-placed Fear snags continue to grow on subsequent tile
    // placements, so we look in both rack and pages.
    if (SnagContent* snag28 = findSnagInRackOrPage(0x28)) {
        std::vector<std::pair<int, int>> candidates;

        for (int dir = 0; dir < 6; dir++) {

            if (!placedTile->permitsDirection(dir)) {
                continue;
            }

            // hex neighbor offsets per direction. matches the binary's
            // switch-with-fall-through at 0x100024d8c..0x100024de4:
            //   dir 0: (col,   row-1)
            //   dir 1: (col+1, row  )
            //   dir 2: (col+1, row+1)   [case 2 falls through to case 3]
            //   dir 3: (col,   row+1)
            //   dir 4: (col-1, row  )
            //   dir 5: (col-1, row-1)   [case 5 falls through to case 0]
            int nCol = placedTile->gridCol;
            int nRow = placedTile->gridRow;

            switch (dir) {
                case 0:                    nRow -= 1; break;
                case 1: nCol += 1;                    break;
                case 2: nCol += 1;         nRow += 1; break;
                case 3:                    nRow += 1; break;
                case 4: nCol -= 1;                    break;
                case 5: nCol -= 1;         nRow -= 1; break;
            }

            if (!cellIsOccupied(nCol, nRow)) {
                candidates.push_back({ nCol, nRow });
            }
        }

        if (!candidates.empty()) {
            int idx = rngInt(0, (int)candidates.size() - 1, 0);
            const auto& chosen = candidates[idx];
            hexMap.addCell(3, chosen.first, chosen.second, 2);

            // pick origin: when the snag's parent tile is committed on the
            // page list, OR when the held rack tile IS the snag's parent,
            // animate from GameBoard's positionXY; else origin = (0, 0).
            // matches the binary's compound gate.
            TileObject* parent = snag28->tileParent;
            float origin[2] = { 0.0f, 0.0f };
            bool parentCommitted = parent && parent->committed;
            bool heldIsParent    = (selectedRackSlot != -1 &&
                                    rack[selectedRackSlot] == parent);

            if (parentCommitted || heldIsParent) {
                origin[0] = positionX;
                origin[1] = positionY;
            }

            pushAction(0, origin, parent);
        }
    }

    // ===================================================================
    // Section 3: snag-100 ("Neglect") cleanup on ATK / DEF tile
    // ===================================================================
    {
        int contentType = placedTile->getContentType();

        if ((contentType == 2 || contentType == 3) &&
            findSnagInRack(100) != nullptr) {
            // "Neglect: Draw a blank when you place an ATK or DEF tile":
            // pushes a contentType=0 (blank) tile onto the reserve queue.
            pushReserveTile(0, 0xFFFFFFFFu);
        }
    }

    // ===================================================================
    // Section 4: snag-2 ("Hound") damage tally -> heal + burst
    // ===================================================================
    {
        int hound = placedTile->decorationValueOfKind(2);

        if (hound > 0) {
            // damage the player by `hound` HP, clamped so currentHP
            // can't go negative. binary computes
            //   newHP = currentHP - clamp(hound, 0, currentHP)
            // which is currentHP - hound when hound <= currentHP, else 0.
            int currentHP = playerSystem.currentHealth;
            int clamped   = (hound <= currentHP) ? hound : currentHP;
            setHP((uint32_t)(currentHP - clamped));

            // burst at GameBoard positionXY anchored on the placed tile.
            float origin[2] = { positionX, positionY };
            pushAction(0, origin, placedTile);

            placedTile->suppressDecorationsOfKind(2);

            Game* g = getGame();

            if (g) {
                g->soundQueue.trigger(0x3E);
            }
        }
    }

    // ===================================================================
    // Section 5: contentType==2 + perk-6 ("Strength") -> DEF boost
    // ===================================================================
    if (placedTile->getContentType() == 2) {
        int currentDef = playerSystem.defence;
        int perkBoost  = playerSystem.perkLevel(6);
        setDEF(currentDef + perkBoost);
    }

    // ===================================================================
    // Section 6: snag-0x2e ("Pride") rack walk
    // ===================================================================
    {
        for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {
            TileObject* rackTile = rack[slot];

            if (!rackTile) {
                continue;
            }

            SnagContent* snag = rackTile->getSnagIfAlive();

            if (!snag || snag->type != 0x2e) {
                continue;
            }

            int contentType = placedTile->getContentType();
            int magnitude   = placedTile->getContentMagnitude();
            float zero[2]   = { 0.0f, 0.0f };

            if (contentType == 6) {
                snag->setHpDisplay (snag->hp  + magnitude / 2, this, zero);
            }
            else if (contentType == 3) {
                snag->setDefDisplay(snag->def + magnitude / 2, this, zero);
            }
            else if (contentType == 2) {
                snag->setAtkDisplay(snag->atk + magnitude / 2, this, zero);
            }
        }
    }

    // ===================================================================
    // Section 7: snag-0x2e ("Pride") page walk
    // ===================================================================
    {
        float anchor[2] = { positionX, positionY };

        for (TileObject* pageTile : pageList) {

            if (!pageTile) {
                continue;
            }

            SnagContent* snag = pageTile->getSnagIfAlive();

            if (!snag || snag->type != 0x2e) {
                continue;
            }

            int contentType = placedTile->getContentType();
            int magnitude   = placedTile->getContentMagnitude();

            if (contentType == 6) {
                snag->setHpDisplay (snag->hp  + magnitude / 2, this, anchor);
            }
            else if (contentType == 3) {
                snag->setDefDisplay(snag->def + magnitude / 2, this, anchor);
            }
            else if (contentType == 2) {
                snag->setAtkDisplay(snag->atk + magnitude / 2, this, anchor);
            }
        }
    }
}

// reconstructed from Ghidra FUN_100025238.
// fires from nemesisSpawnAtTrailTail when the just-committed tile
// carries a live snag (param_2). multi-section pass:
//
//   1. add the player's DEF item-ability bonus into player.def.
//   2. walk page list:
//        - content type 0x19 (Milestone): once-only;
//          send the snag walking toward this tile, kill the tile's
//          existing content visual. sets `milestoneActive`.
//        - content type 0x17 (Warmth): if hex-adjacent
//          to the snag's parent tile, push action burst on that tile
//          and halve+1 the snag's HP display.
//   3. trigger combat-impact sound (snag type 1 -> 0x20, else 0x24).
//   4. big switch on snag.type. cases reach the function tail through
//      one of four paths in the binary:
//        - LAB_100025554 fall-through: pushAction(&positionX, snagParent)
//          fires after the case body. cases: 0xd, 0xe, 0x17, 0x1a, 0x21,
//          0x3d, 0x76.
//        - LAB_100025760: pushAction fires at end of body. cases: 0x26
//          (always), 3 (only when at least one rack cleared).
//        - own pushAction (not via labels): 0x3f, 0x51 (gated), 0x67
//          (gated), 0x6b, 0x6f.
//        - no action push (LAB_100025570 skip-both): 2, 0x6c, 8, 0x2f,
//          0x36, 0x46, 0x4f, 0x56, 0x5e, 0x62, 0x6a.
//   5. rack tail walk: for each snag-0x3e (Bluff), set its atk display
//      to (snag.atk >> 1), so the displayed atk shows half its real value.
//   6. dispatch snag-death (FUN_100025dcc) when bVar15 holds; else set
//      GameBoard+0x1c = 1 ("snag-handled" flag for downstream callers).
//   7. final tail: when no Milestone active and snag has HP and page
//      list >= 2 and snag.type not in {10, 0x32}, send the snag walking
//      toward the tile placed just before its own (= second-newest,
//      pageList.rbegin()+1). alive snags chase forward toward the newest
//      tile, not back toward the oldest.
//
// flag dynamics (matching the binary's bVar4 / bVar15 names):
//   bVar14 = milestoneActive: set in section 2 on content-type 0x19.
//   bVar4  = !bVar14: initial value for both flags.
//   bVar15 = bVar4 initially.
//   case 2 / 0x6c (LAB_100025c40): bVar4 = false; bVar15 = false  (skip
//                                  death dispatch and skip final tail).
//   case 0x3f:                     bVar15 = false (skip death only;
//                                  bVar4 stays, so final tail still fires).
//
// section 6 dispatches snag-death iff bVar15 == true.
// section 7 fires final-tail sendToward iff bVar4 == true.
void GameBoard::dispatchSnagPostCommit(SnagContent* snag) {

    if (!snag) {
        return;
    }

    // shared origin pointer: every action burst in this function uses
    // &positionX (a 2-float pair at GameBoard+0x97C / +0x980) as its
    // source position. matches the binary's `(long *)(param_1 + 0x97c)`.
    const float* boardOrigin = &positionX;

    // ---- section 1: apply player DEF item-ability bonus ----------------
    setDEF(playerSystem.baseItemSpecialAbilityValue(0xf) +
           playerSystem.defence);

    // ---- section 2: page-list walk for content types 0x19 / 0x17 -------
    bool milestoneActive = false;

    for (TileObject* tile : pageList) {

        if (!tile) {
            continue;
        }

        int contentType = tile->getContentType();

        if (contentType == 0x19) {
            // Milestone pre-flight, once only.
            if (!milestoneActive) {
                snag->sendToward(tile, false);
                TileContent* tc = tile->getTileContentIfAlive();

                if (tc) {
                    tc->killAndFade();
                }
            }

            milestoneActive = true;
        }
        else if (contentType == 0x17) {
            // Warmth: push burst on this iter tile +
            // halve snag HP if adjacent to snag's parent.
            TileObject* snagParent = snag->tileParent;

            if (snagParent) {
                int dist = hexGridDistance(tile->gridCol,
                                           tile->gridRow,
                                           snagParent->gridCol,
                                           snagParent->gridRow);

                if (dist == 1) {
                    pushAction(0, boardOrigin, tile);
                    snag->setHpDisplay((snag->hp + 1) >> 1,
                                       this, boardOrigin);
                }
            }
        }
    }

    // ---- section 3: combat impact sound ---------------------------------
    {
        Game* g = getGame();
        uint32_t soundId = (snag->type == 1) ? 0x20u : 0x24u;

        if (g) {
            g->soundQueue.trigger(soundId);
        }
    }

    bool dispatchDeath        = !milestoneActive;   // bVar15
    bool allowFinalSendToward = !milestoneActive;   // bVar4

    // a tile-ref reused across many cases: the snag's parent tile.
    // null-tolerant; pushAction accepts a null tileRef.
    TileObject* snagParent = snag->tileParent;

    // helper: cases that fall through to LAB_100025554 in the binary
    // push pushAction(0, &positionX, snagParent) at the end of their body.
    auto pushFallThroughAction = [&]() {
        pushAction(0, boardOrigin, snagParent);
    };

    // ---- section 4: per-snag-type switch --------------------------------
    int snagType = snag->type;

    switch (snagType) {

        case 2:  // Hound: attack oldest committed tile (LAB_100025c40).
        case 0x6c: {  // Honesty: same body.

            if (!milestoneActive && !pageList.empty()) {
                TileObject* oldestTile = pageList.front();
                snag->sendToward(oldestTile, false);
                allowFinalSendToward = false;
                dispatchDeath        = false;
            }
            break;
        }

        case 3: {  // Bite: clear any rack tile whose snag died.
                   // pushes action only when at least one rack tile cleared
                   // (LAB_100025760, gated on the local bool from the loop).
            bool clearedAny = false;

            for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

                if (rack[slot] && !rack[slot]->getSnagIfAlive()) {
                    discardRackTile(slot, false);
                    clearedAny = true;
                }
            }

            if (clearedAny) {
                pushAction(0, boardOrigin, snagParent);
            }
            break;
        }

        case 8: {  // Regret: set snag atk display to current page count,
                   // no stat-tween. local stack (0,0) offset.
            float zero[2] = { 0.0f, 0.0f };
            snag->setAtkDisplay(static_cast<int>(pageList.size()), nullptr, zero);
            break;
        }

        case 0xd:  // Nostalgia: clear player ATK to 0.
            setATK(0);
            pushFallThroughAction();
            break;

        case 0xe:  // Hubris: clear player DEF to 0.
            setDEF(0);
            pushFallThroughAction();
            break;

        case 0x17: {  // Impending Doom: bump nemesis eatTarget toward pageList.size()-1.
            int v = static_cast<int>(pageList.size()) - 1;

            if (v <= nemesis.eatTarget) {
                v = nemesis.eatTarget;
            }

            nemesis.eatTarget = v;
            nemesis.eatActive = true;

            pushFallThroughAction();
            break;
        }

        case 0x1a: {  // Stranger: push a random snag tile onto reserve.
            int rolledType = rollSnagType();
            pushReserveSnagTile((uint32_t)rolledType, 0xffffffffu);
            pushFallThroughAction();
            break;
        }

        case 0x21:  // Alarm Bells: push a fixed snag-22 onto reserve.
            pushReserveSnagTile(0x22u, 0xffffffffu);
            pushFallThroughAction();
            break;

        case 0x26: {  // Broken Promise: wipe rack tiles with no snag or
                      // snag.type==1. always pushes action via LAB_100025760.
            for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

                if (!rack[slot]) {
                    continue;
                }

                SnagContent* slotSnag = rack[slot]->getSnagIfAlive();

                if (!slotSnag || slotSnag->type == 1) {
                    rack[slot]->erase();
                }
            }

            pushAction(0, boardOrigin, snagParent);
            break;
        }

        case 0x2f: {  // False Hope: DEF = HP + currentDEF; HP = 1.
            setDEF(playerSystem.currentHealth + playerSystem.defence);
            setHP(1u);
            break;
        }

        case 0x36:  // Repressed Memory: push 2x XP tile (content type 8)
                    // via FUN_100017fb8 (= pushReserveTile, not snag tile).
            pushReserveTile(8u, 0xffffffffu);
            pushReserveTile(8u, 0xffffffffu);   // LAB_100025a24 second call
            break;

        case 0x3d: {  // Malice: drain HP by current ATK amount.
            int playerATK = playerSystem.attack;

            if (0 < playerATK) {
                int playerHP = playerSystem.currentHealth;
                int loss = (playerATK <= playerHP) ? playerATK : playerHP;
                setHP((uint32_t)(playerHP - loss));
            }

            pushFallThroughAction();
            break;
        }

        case 0x3f: {  // Terror: kick HP=1 + bump nemesis eatTarget toward
                      // pageList.size()-2, own action push, dispatch death is
                      // skipped but final sendToward still fires.
            setHP(1u);

            int v = static_cast<int>(pageList.size()) - 2;

            if (v <= nemesis.eatTarget) {
                v = nemesis.eatTarget;
            }

            nemesis.eatTarget = v;
            nemesis.eatActive = true;

            pushAction(0, boardOrigin, snagParent);

            dispatchDeath = false;     // bVar15 = false; bVar4 stays.
            break;
        }

        case 0x46: {  // Heavy Burden: push a content-9 tile, set its
                      // magnitude to 9 via FUN_100014870.
            TileObject* spawned = pushReserveTile(9u, 0xffffffffu);

            if (spawned) {
                TileContent* tc = spawned->getTileContentIfAlive();

                if (tc) {
                    tc->setRawAndDisplayMagnitude(9);
                }
            }
            break;
        }

        case 0x4f: {  // Apathy: halve all 3 player stats (round up).
            setATK((playerSystem.attack       + 1) >> 1);
            setDEF((playerSystem.defence      + 1) >> 1);
            setHP((uint32_t)((playerSystem.currentHealth + 1) >> 1));
            break;
        }

        case 0x51: {  // Discord: clear control bank if the player's
                      // controlReceivedTotal isn't a clean multiple of 10
                      // (= a control charge is mid-fill). a clean multiple
                      // means no charge in progress, so the snag's reset is
                      // a no-op there.
            if ((hud.controlReceivedTotal % 10) > 0) {
                hud.clearCTRLBank();
                pushAction(0, boardOrigin, snagParent);
            }
            break;
        }

        case 0x56: {  // Sharp Pain: push content-0x11 tile with magnitude
                      // = playerMaxHP.
            TileObject* spawned = pushReserveTile(0x11u, 0xffffffffu);

            if (spawned) {
                TileContent* tc = spawned->getTileContentIfAlive();

                if (tc) {
                    tc->setRawAndDisplayMagnitude(playerSystem.maxHealth);
                }
            }
            break;
        }

        case 0x5e: {  // Suspicion: for each rack tile whose contentType is
                      // in {2, 3, 6} (ATK/DEF/HP, mask 0x6C bit 2/3/6),
                      // erase content + replace with type 0x12, magnitude
                      // doubled.
            for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

                if (!rack[slot]) {
                    continue;
                }

                int rackContentType = rack[slot]->getContentType();

                if (rackContentType >= 7) {
                    continue;
                }

                if (((1 << rackContentType) & 0x6c) == 0) {
                    continue;
                }

                int oldMag = rack[slot]->getContentMagnitude();
                rack[slot]->erase();
                rack[slot]->setTileContent(0x12u, oldMag << 1);
            }
            break;
        }

        case 0x62: {  // Losing Streak: RNG-rolled [1..6] copies of content
                      // type 0x13 onto reserve.
            int n = rngInt(1, 6, 4);

            while (n > 0) {
                pushReserveTile(0x13u, 0xffffffffu);
                n--;
            }
            break;
        }

        case 0x67: {  // Missed Opportunity: pick a random EventSlot from
                      // the HUD's tray, remove it, fire action burst.
            // binary builds a std::vector<EventSlot*> via buildEventSlotList
            // (FUN_10000d8fc), rolls rngInt(0, n-1, 4), then calls
            // hud.removeEventSlot(picked, compact=true). we walk eventTray
            // directly into a stack array of 4 to avoid the heap.
            EventSlot* candidates[4] = {};
            int        count = 0;

            for (int i = 0; i < 4; i++) {
                EventSlot* s = hud.eventTray[i].slotPtr;

                if (s != nullptr) {
                    candidates[count] = s;
                    count++;
                }
            }

            if (count > 0) {
                int pick = rngInt(0, count - 1, 4);
                hud.removeEventSlot(candidates[pick], /*compact=*/true);
                pushAction(0, boardOrigin, snagParent);
            }
            break;
        }

        case 0x6a:  // Sharp Shock: single XP-tile reserve push (only the
                    // LAB_100025a24 path runs; case 0x36's preceding call
                    // is skipped because we entered via direct `goto`).
            pushReserveTile(8u, 0xffffffffu);
            break;

        case 0x6b: {  // Trouble: register kind-3 HexMap cells in 3x3 around
                      // snag.parent (excluding center + arc-distance >= 3).
            TileObject* parentTile = snagParent;

            if (parentTile) {

                for (int dCol = -2; dCol < 3; dCol++) {

                    for (int dRow = -2; dRow < 3; dRow++) {

                        if (dCol == 0 && dRow == 0) {
                            continue;
                        }

                        int dist = hexGridDistance(0, 0, dCol, dRow);

                        if (dist >= 3) {
                            continue;
                        }

                        hexMap.addCell(3,
                                       parentTile->gridCol + dCol,
                                       parentTile->gridRow + dRow,
                                       0);
                    }
                }
            }

            // 0x6b's own action push (not via LAB_100025554; `lVar10 =
            // param_2[0x90]` is set inline, then falls into the common tail).
            pushAction(0, boardOrigin, snagParent);
            break;
        }

        case 0x6f: {  // Veiled Threat: register kind-3 HexMap cells in 3x3
                      // around the GameBoard's stored exit hex. matches
                      // case 0x6b but anchored to exit instead of snag.parent.

            for (int dCol = -2; dCol < 3; dCol++) {

                for (int dRow = -2; dRow < 3; dRow++) {

                    if (dCol == 0 && dRow == 0) {
                        continue;
                    }

                    int dist = hexGridDistance(0, 0, dCol, dRow);

                    if (dist >= 3) {
                        continue;
                    }

                    hexMap.addCell(3, exitCol + dCol, exitRow + dRow, 0);
                }
            }

            // 0x6f's own action push, mirrors 0x6b.
            pushAction(0, boardOrigin, snagParent);
            break;
        }

        case 0x76: {  // Spirit Drain: install a fresh Event of kind 0x50.
            // binary counts non-null entries in hud.eventTray (FUN_10000d7ac).
            // when zero: just addEventSlot. when >= 1: insertEventSlotAt(0)
            // (FUN_10000d56c) which removes whatever's at slot 0 (compact=
            // false) and then addEventSlot, or (quirk of the binary)
            // destroys the new slot if eventTray[0] happens to be null
            // while other slots are filled.
            int count = hud.countEventsHeld();

            EventSlot* newSlot = new EventSlot();
            newSlot->init(0x50, 0);

            if (count < 1) {
                hud.addEventSlot(newSlot);
            } else {

                if (hud.eventTray[0].slotPtr != nullptr) {
                    hud.removeEventSlot(hud.eventTray[0].slotPtr, /*compact=*/false);
                    hud.addEventSlot(newSlot);
                } else {
                    delete newSlot;
                }
            }

            pushFallThroughAction();
            break;
        }

        default:
            break;
    }

    // ---- section 5: rack tail walk for snag-0x3e (Bluff) ----------------
    {
        for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

            if (!rack[slot]) {
                continue;
            }

            if (rack[slot]->getSnagType() != 0x3e) {
                continue;
            }

            SnagContent* bluffSnag = rack[slot]->getSnagIfAlive();

            if (!bluffSnag) {
                continue;
            }

            // local-stack (0, 0) offset matches the binary's `local_b8`.
            float zero[2] = { 0.0f, 0.0f };
            bluffSnag->setAtkDisplay((int)((uint32_t)bluffSnag->atk >> 1),
                                     this, zero);
        }
    }

    // ---- section 6: dispatch combat resolution (or set "handled" flag) -
    if (dispatchDeath) {
        resolveSnagCombat(snag);
    } else {
        snagActivationSuppressed = true;
    }

    // ---- section 7: final tail sendToward(previous tile) ---------------
    // the tile placed just before the snag's own tile (= second-newest in
    // the page list).
    if (allowFinalSendToward &&
        snag->hp != 0 &&
        pageList.size() > 1 &&
        snag->type != 10 &&
        snag->type != 0x32) {

        auto rit = pageList.rbegin();
        ++rit;   // second-newest
        TileObject* previousTile = *rit;
        snag->sendToward(previousTile, false);
    }
}

// reconstructed from Ghidra FUN_100025dcc.
void GameBoard::resolveSnagCombat(SnagContent* snag) {

    if (!snag) {
        return;
    }

    Game* g = getGame();
    auto soundTrigger = [&](uint32_t id) {

        if (g) {
            g->soundQueue.trigger(id);
        }
    };

    // ---- Honesty (0x6c) early-return: gift snag.hp to player.atk ----
    if (snag->type == 0x6c) {
        setATK(snag->hp + playerSystem.attack);
        snag->killAndFade();
        soundTrigger(0x15);
        return;
    }

    // ---- Complacency (0x58) early-return: only fires when snag is hardier ----
    if (snag->type == 0x58 &&
        (uint32_t)snag->hp <= (uint32_t)playerSystem.currentHealth) {
        return;
    }

    snagActivationSuppressed = true;

    const float* boardOrigin = &positionX;
    uint32_t snagAtkInitial  = (uint32_t)snag->atk;
    uint32_t snagHpAtEntry   = (uint32_t)snag->hp;

    // PiercingGaze (0x1f) skips the player's def in the player-damage calc.
    int playerDefForCounter = (snag->type == 0x1f) ? 0 : playerSystem.defence;

    // ---- compute snagDamage with type-specific clamps -----------------
    uint32_t snagDamage = snagAtkInitial;   // Masochism (0x5c) keeps this default

    if (snag->type != 0x5c) {
        uint32_t rawDamage = (uint32_t)playerSystem.attack - (uint32_t)snag->def;
        snagDamage = rawDamage;

        if (snag->type == 0x70) {
            // Attrition: cap snag damage at 1.
            snagDamage = (int32_t)rawDamage < 2 ? rawDamage : 1u;
        }
        else if (snag->type == 0x5f && (int32_t)rawDamage > 0) {
            // Sadism: spawn a content-0x11 reserve tile with magnitude = damage.
            TileObject* spawned = pushReserveTile(0x11u, 0xffffffffu);

            if (spawned) {
                TileContent* tc = spawned->getTileContentIfAlive();

                if (tc) {
                    tc->setRawAndDisplayMagnitude((int)rawDamage);
                }
            }
        }
        else {
            // generic clamp path: when snag has more than 1 hp, snagDamage
            // gets capped at hp - 1 only for Stubbornness (0x20). other
            // types pass the full rawDamage through (the loop reuses the
            // same scratch but doesn't apply it elsewhere).
            uint32_t capped = snagHpAtEntry - 1u;

            if ((int32_t)rawDamage <= (int32_t)(snagHpAtEntry - 1u)) {
                capped = rawDamage;
            }

            if (snagHpAtEntry > 1u) {
                rawDamage = capped;
            }

            if (snag->type == 0x20) {
                snagDamage = rawDamage;
            }
        }
    }

    int playerDamage = (int)snagAtkInitial - playerDefForCounter;

    // ---- apply player HP loss (skipped under combatEffectsSuppressed) ----
    if (!combatEffectsSuppressed && playerDamage > 0) {
        int hpLoss = (playerDamage <= playerSystem.currentHealth)
                         ? playerDamage : playerSystem.currentHealth;
        setHP((uint32_t)(playerSystem.currentHealth - hpLoss));
    }

    // ---- snag HP loss display + sounds --------------------------------
    snag->deductHpClamped((int)snagDamage, this, boardOrigin);

    // player damaged sound vs blocked sound.
    soundTrigger((playerDamage < 1 || combatEffectsSuppressed) ? 0x1d : 0x1c);

    // snag survival sound: 0x26 (no damage), 0x25 (damaged but alive),
    // 0x1e (type 1 killed), 0x23 (other type killed).
    {
        uint32_t snagSound;

        if ((int32_t)snagDamage < 1) {
            snagSound = 0x26;
        }
        else if (snag->hp == 0) {
            snagSound = (snag->type == 1) ? 0x1eu : 0x23u;
        }
        else {
            snagSound = 0x25;
        }

        soundTrigger(snagSound);
    }

    // ---- player atk/def degrade ---------------------------------------
    {
        uint32_t pAtk = (uint32_t)playerSystem.attack;
        uint32_t pDef = (uint32_t)playerSystem.defence;

        if (!combatEffectsSuppressed) {
            playerSystem.degradeStatsAfterCombat(&pAtk, &pDef, snag,
                                                 (int)snagDamage);
        }

        setATK((int)pAtk);
        setDEF((int)pDef);
    }

    // ---- snag atk/def degrade -----------------------------------------
    {
        uint32_t sAtk = (uint32_t)snag->atk;
        uint32_t sDef = (uint32_t)snag->def;
        snag->resolveCombatDelta(&playerSystem, &sAtk, &sDef,
                                 playerDamage, snagDamage);

        snag->setAtkDisplay((int)sAtk, this, boardOrigin);
        snag->setDefDisplay((int)sDef, this, boardOrigin);
    }

    // ---- bump animations ----------------------------------------------
    // direction codes follow the binary's (col-comparison, row tiebreaker)
    // pattern. player and snag bump in mirrored directions: when the
    // player is left of the snag, player bumps right (1/2) and snag bumps
    // left (4/5). same-column ties resolve by row.
    auto bumpDir = [](int srcCol, int srcRow, int dstCol, int dstRow) -> int {

        if (srcCol < dstCol) {
            return (srcRow < dstRow) ? 2 : 1;
        }

        if (dstCol < srcCol) {
            return (dstRow < srcRow) ? 5 : 4;
        }

        return (srcRow <= dstRow) ? 3 : 0;
    };

    int playerBump = bumpDir(playerSystem.gridCol, playerSystem.gridRow,
                             snag->gridCol,        snag->gridRow);
    playerSystem.triggerBumpAnim(playerBump);

    int snagBump = bumpDir(snag->gridCol,        snag->gridRow,
                           playerSystem.gridCol, playerSystem.gridRow);
    snag->triggerBumpAnim(snagBump);

    // ---- snag survived branch -----------------------------------------
    if (snag->hp != 0) {

        if (snag->type == 0x32 && !pageList.empty()) {
            // Flashback: "Moves to the start of the trail after each attack."
            // start of trail = oldest committed tile = pageList.front().
            TileObject* startOfTrail = pageList.front();
            snag->sendToward(startOfTrail, false);
        }
        return;
    }

    // ---- snag died: type-specific on-death side-effect ----------------
    int snagTier = snag->tier();
    int snagType = snag->type;
    bool pushTrailingAction = false;
    bool skipXpGain         = false;

    if (snagType == 6) {
        // Obsession: rng-rolled chance to drop another snag-6 tile carrying
        // a decremented chain-count at +0x490 (consumedFlag, init 0x64) and
        // a bumped invocation counter at +0x494 (obsessionCount, init 2).
        int origConsumed = (int)snag->consumedFlag;
        int origCount    = snag->obsessionCount;

        if (rngInt(0, 99, 4) < origConsumed) {
            SnagContent* spawned = pushReserveSnagTile(6u, (uint32_t)origCount);

            if (spawned) {
                int next = origConsumed - 0x14;

                if (next < 1) {
                    next = 0;
                }

                spawned->consumedFlag   = (uint8_t)next;
                spawned->obsessionCount = origCount + 1;
            }
        }
    }
    else if (snagType == 0x1b) {
        // Malaise: when snag had def, drop a snag-0x1b copy carrying the
        // dying snag's atk/def stats; skip the XP/control path.
        if (snag->def == 0) {
            // no def: fall through to XP gain.
        } else {
            SnagContent* spawned = pushReserveSnagTile(0x1bu, 0xffffffffu);

            if (spawned) {
                float zero[2] = { 0.0f, 0.0f };
                spawned->setAtkDisplay(snag->atk, nullptr, zero);
                spawned->setDefDisplay(snag->def, nullptr, zero);
                spawned->setHpDisplay (snag->def, nullptr, zero);
            }

            skipXpGain = true;   // jump straight to the perk-3 ATK boost
        }
    }
    else if (snagType == 0x1d) {
        // Emptiness: discard rack tiles whose snag is dead or has type 1.
        for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

            if (!rack[slot]) {
                continue;
            }

            SnagContent* slotSnag = rack[slot]->getSnagIfAlive();

            if (!slotSnag || slotSnag->type == 1) {
                discardRackTile(slot, false);
            }
        }
    }
    else if (snagType == 0x22) {
        // DistantEcho: ~50% chance to spawn another snag-0x22.
        if (rngInt(0, 100, 4) > 0x32) {
            pushReserveSnagTile(0x22u, 0xffffffffu);
        }
    }
    else if (snagType == 0x44) {
        // Revenge: stamp pushDecoration(2, snagHpAtEntry, 0) on every rack
        // tile. binary uses uVar10 (snag.hp captured at function entry,
        // pre-damage), since at this point snag->hp is 0 (we're in the
        // death branch).
        for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

            if (rack[slot]) {
                rack[slot]->pushDecoration(2, (int)snagHpAtEntry, 0);
            }
        }
        pushTrailingAction = true;
    }
    else if (snagType == 0x4a) {
        // Liar: spawn 1..3 snag-0x4b tiles.
        int n = rngInt(1, 3, 4);

        while (n > 0) {
            pushReserveSnagTile(0x4bu, 0xffffffffu);
            n--;
        }
    }
    else if (snagType == 0x4c) {
        // Drama: when atk != 0 spawn a snag-0x4d carrying atk on all 3
        // displays. when def != 0 spawn a snag-0x4e carrying def on all 3.
        float zero[2] = { 0.0f, 0.0f };

        if (snag->atk != 0) {
            SnagContent* atkSnag = pushReserveSnagTile(0x4du, 0xffffffffu);

            if (atkSnag) {
                atkSnag->setAtkDisplay(snag->atk, nullptr, zero);
                atkSnag->setDefDisplay(snag->atk, nullptr, zero);
                atkSnag->setHpDisplay (snag->atk, nullptr, zero);
            }
        }

        if (snag->def != 0) {
            SnagContent* defSnag = pushReserveSnagTile(0x4eu, 0xffffffffu);

            if (defSnag) {
                defSnag->setAtkDisplay(snag->def, nullptr, zero);
                defSnag->setDefDisplay(snag->def, nullptr, zero);
                defSnag->setHpDisplay (snag->def, nullptr, zero);
            }
        }
    }
    else if (snagType == 0x53) {
        // Fatigue: zero player atk + def.
        setATK(0);
        setDEF(0);
    }
    else if (snagType == 0x57) {
        // PainfulMemory: replace dead-snag rack tiles with content-0x11
        // tiles whose magnitude = position-in-rack (1, 2, ...).
        int n = 1;

        for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

            if (!rack[slot]) {
                continue;
            }

            SnagContent* slotSnag = rack[slot]->getSnagIfAlive();

            if (slotSnag && slotSnag->type != 1) {
                continue;
            }

            rack[slot]->erase();
            rack[slot]->setTileContent(0x11u, n);
            n++;
        }
    }
    else if (snagType == 0x5a) {
        // Surprise: roll random snag-type and spawn it.
        int rolledType = rollSnagType();
        pushReserveSnagTile((uint32_t)rolledType, 0xffffffffu);
    }
    else if (snagType == 0x5d) {
        // HiddenAgenda: spawn a content-0x12 tile with atk magnitude (if
        // atk != 0) and another with def magnitude (if def != 0).
        if (snag->atk != 0) {
            TileObject* spawned = pushReserveTile(0x12u, 0xffffffffu);

            if (spawned) {
                TileContent* tc = spawned->getTileContentIfAlive();

                if (tc) {
                    tc->setRawAndDisplayMagnitude(snag->atk);
                }
            }
        }

        if (snag->def != 0) {
            TileObject* spawned = pushReserveTile(0x12u, 0xffffffffu);

            if (spawned) {
                TileContent* tc = spawned->getTileContentIfAlive();

                if (tc) {
                    tc->setRawAndDisplayMagnitude(snag->def);
                }
            }
        }
    }
    else if (snagType == 0x60) {
        // Glory: spawn a content-9 tile with magnitude = snag.hp at entry.
        TileObject* spawned = pushReserveTile(9u, 0xffffffffu);

        if (spawned) {
            TileContent* tc = spawned->getTileContentIfAlive();

            if (tc) {
                tc->setRawAndDisplayMagnitude((int)snagHpAtEntry);
            }
        }
        pushTrailingAction = true;
    }
    else if (snagType == 0x61) {
        // Distrust: spawn 2 content-9 tiles with rng[1..7] magnitudes.
        for (int i = 0; i < 2; i++) {
            TileObject* spawned = pushReserveTile(9u, 0xffffffffu);

            if (spawned) {
                TileContent* tc = spawned->getTileContentIfAlive();

                if (tc) {
                    tc->setRawAndDisplayMagnitude(rngInt(1, 7, 4));
                }
            }
        }
        pushTrailingAction = true;
    }

    // LAB_1000265e4: cases 0x44 / 0x60 / 0x61 push an action burst
    // before the XP gain path.
    if (pushTrailingAction) {
        TileObject* snagParent = snag->tileParent;
        pushAction(0, boardOrigin, snagParent);
    }

    // ---- XP / Control gain (when snag.tier > 0) -----------------------
    if (!skipXpGain && snagTier > 0) {

        if (snag->type == 1) {
            snagsDefeated++;
        }
        else {
            int ctrlLevel = playerSystem.perkLevel(0x11);
            hud.advanceCTRLSlot(ctrlLevel, false);
            specialSnagsDefeated++;
        }

        applyTileTypeEffect(0);

        // Grow XP magnitude on snag's parent tile (if it's an XP tile),
        // otherwise replace its content with a fresh XP (type 4) tile.
        TileObject* parent = snag->tileParent;

        if (parent) {

            if (parent->getContentType() == 4) {
                TileContent* tc = parent->getTileContentIfAlive();

                if (tc) {
                    int oldMag = parent->getContentMagnitude();
                    tc->setRawAndDisplayMagnitude(oldMag + snagTier);
                }
            }
            else {
                parent->setTileContent(4u, snagTier);
            }
        }
    }

    // ---- LAB_1000266a0: perk-3 ATK boost ------------------------------
    {
        int perk3 = playerSystem.perkLevel(3);
        int boostedAtk = playerSystem.attack;

        if (perk3 == 2) {
            boostedAtk += 5;
            setATK(boostedAtk);
        }
        else if (perk3 == 1) {
            boostedAtk += 2;
            setATK(boostedAtk);
        }
    }

    // ---- LAB_1000266d8: cleanup ---------------------------------------
    // achievement snag-kill fan-out (= binary's FUN_10004df18).
    {
        AchievementTracker& tracker = getGame()->achievementTracker();

        // "Opportunist": unconditional on every snag kill.
        tracker.increment(AchievementId::Opportunist);

        // non-generic snags also feed GottaCatchEmAll (= track distinct
        // special-snag types in tracker.snagKinds; the set's size IS the
        // progress count). insert here, increment polls it.
        if (snag->type != 1) {
            tracker.snagKinds.insert(snag->type);
            tracker.increment(AchievementId::GottaCatchEmAll);
        }

        // tier 3 (boss/elite) snags fire two back-to-back milestones.
        if (snag->tier() > 2) {
            tracker.increment(AchievementId::SomethingWicked);
            tracker.increment(AchievementId::BigGame);
        }

        // Doppelganger kill is the only path that fires SomeoneIUsedToKnow.
        if (snag->type == 0x19) {
            tracker.increment(AchievementId::SomeoneIUsedToKnow);
        }
    }

    snag->killAndFade();
}

// reconstructed from Ghidra FUN_10002678c. count rack + page list tiles
// whose snag.type matches.
int GameBoard::countSnagTypeInBoard(int snagType) const {
    int total = countSnagTypeInRack(snagType);

    for (TileObject* tile : pageList) {

        if (tile && tile->getSnagType() == snagType) {
            total++;
        }
    }

    return total;
}

int GameBoard::countSnagTypeInRack(int snagType) const {
    int count = 0;

    for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

        if (rack[slot] && rack[slot]->getSnagType() == snagType) {
            count++;
        }
    }

    return count;
}

// reconstructed from Ghidra FUN_100024c40. arg-keyed rack count.
int GameBoard::countContentTypeInRack(int code) const {
    int count = 0;

    for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {
        TileObject* tile = rack[slot];

        if (!tile) {
            continue;
        }

        if (code == 0) {
            // blank: no alive snag and no alive content.
            if (!tile->getSnagIfAlive() && !tile->getTileContentIfAlive()) {
                count++;
            }
        }
        else if (code == 1) {
            // has alive snag.
            if (tile->getSnagIfAlive()) {
                count++;
            }
        }
        else {
            // literal content type.
            if (tile->getContentType() == code) {
                count++;
            }
        }
    }

    return count;
}

// reconstructed from Ghidra FUN_100026818. count rack tiles whose alive
// snag has type != excludedType.
int GameBoard::countAliveSnagsExceptType(int excludedType) const {
    int count = 0;

    for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {
        TileObject* tile = rack[slot];

        if (!tile) {
            continue;
        }

        if (!tile->getSnagIfAlive()) {
            continue;
        }

        if (tile->getSnagType() != excludedType) {
            count++;
        }
    }

    return count;
}

// rack predicates used by pickRandomRackSlotMatching. each mirrors one of
// the inline filter expressions inside FUN_10001df54's section-2 random-pick
// patterns.
namespace {
bool predRackTrue(TileObject* /*t*/)             { return true; }
bool predRackHasNoAliveSnag(TileObject* t)       { return t->getSnagIfAlive() == nullptr; }
bool predRackHasAliveSnag(TileObject* t)         { return t->getSnagIfAlive() != nullptr; }
bool predRackNoSnagOrSnagType1(TileObject* t)    {
    return t->getSnagIfAlive() == nullptr || t->getSnagType() == 1;
}
bool predRackTypeOneOrAliveContent(TileObject* t) {
    return t->getSnagType() == 1 || t->getTileContentIfAlive() != nullptr;
}
}  // namespace

// shared body of the 5 random-rack-pick patterns the binary inlines in
// section 2 (Bite, Choking Grasp, Indecision, Cruelty, Paranoia, Repressed
// Memory). builds a list of matching slot indices, picks one via stream 4.
int GameBoard::pickRandomRackSlotMatching(bool (*predicate)(TileObject*)) const {
    std::vector<int> matches;

    for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

        if (rack[slot] && predicate(rack[slot])) {
            matches.push_back(slot);
        }
    }

    if (matches.empty()) {
        return -1;
    }

    int idx = rngInt(0, (int)matches.size() - 1, 4);
    return matches[idx];
}

// reconstructed from Ghidra FUN_10001df54. per-turn end-of-turn pipeline.
// see M5_END_OF_TURN_SCOPING.md for the full structural breakdown; this
// function is split into 4 sections.
void GameBoard::applyEndOfTurnPipeline(float dt) {

    const float* boardOrigin = &positionX;

    // ---- section 1: pre-pass --------------------------------------------

    // suppress kind-0 (Darkness) and kind-1 (locked) decorations on every
    // rack tile.
    for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

        if (rack[slot]) {
            rack[slot]->suppressDecorationsOfKind(0);
            rack[slot]->suppressDecorationsOfKind(1);
        }
    }

    // tick HexMap fade timers (2 -> 1 -> deactivate).
    hexMap.tickFade();

    // tick the per-entry eligibility counter on every reserve tile. colorParam
    // (TileReserveEntry+0x8) is a signed pop-eligibility gate: it starts at -1
    // (already poppable) and ticks further negative each turn; rollRackTile
    // pops an entry only when colorParam < 0. (= FUN_10001df54's --node+0x18
    // walk over the +0x96D8 reserve list.)
    for (TileReserveEntry& entry : tileReserve) {
        entry.colorParam--;
    }

    // section-1 snag-type counts driving the rack walk's pre-effect.
    int procrastinationCount = countSnagTypeInBoard(0x10);  // snag-0x10 board-wide
    int panicCount           = countSnagTypeInRack(0x1c);   // snag-0x1c rack-only
    int tragedyCount         = countSnagTypeInRack(0x4d);   // snag-0x4d rack-only
    int comedyCount          = countSnagTypeInRack(0x4e);   // snag-0x4e rack-only

    // bVar9 (one-shot Doubt parasite spawn flag) and bVar10 (one-shot
    // Mania ATK/DEF swap flag): declared up here so section 2's switch
    // body can flip them and skip subsequent occurrences.
    bool doubtFiredOnce = false;
    bool maniaFiredOnce = false;

    // ---- section 2: rack walk -------------------------------------------
    for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {
        TileObject* tile = rack[slot];

        if (!tile) {
            continue;
        }

        TileContent* aliveContent = tile->getTileContentIfAlive();

        if (aliveContent) {
            // content-type effects (only 3 fire on every turn). cases gated
            // on tc->type:
            //   0x11: Pain: magnitude++, "+1" stat tween, sound 0x4a
            //   9:    Dead Weight: magnitude--, erase when it would hit 0
            //   8:    Block: when it lands in slot 4, discard
            int contentType = aliveContent->type;

            if (contentType == 0x11) {
                aliveContent->setRawAndDisplayMagnitude(
                    aliveContent->displayedMagnitude + 1);
                pushStatTween(1, 1, &tile->mainQuad.posX, 0);
                Game* g = getGame();

                if (g) {
                    g->soundQueue.trigger(0x4a);
                }
            }
            else if (contentType == 9) {
                int newMag = aliveContent->displayedMagnitude - 1;

                if (newMag == 0 || aliveContent->displayedMagnitude < 1) {
                    tile->erase();
                }
                else {
                    aliveContent->setRawAndDisplayMagnitude(newMag);
                }
            }
            else if (contentType == 8 && slot == 4) {
                discardRackTile(4, false);
                break;   // matches binary's `break;` out of the rack loop
            }

            continue;
        }

        SnagContent* aliveSnag = tile->getSnagIfAlive();

        if (!aliveSnag) {
            continue;
        }

        // section 2 pre-effect: apply procrastination/panic/tragedy/comedy
        // counters to the rack snag's stat displays. binary computes the
        // atk delta as (tragedyCount + panicCount), preserved verbatim.
        float zero[2] = { 0.0f, 0.0f };

        if (procrastinationCount > 0) {
            aliveSnag->setHpDisplay(aliveSnag->hp + procrastinationCount * 2,
                                    this, zero);
        }

        if (tragedyCount + panicCount > 0) {
            aliveSnag->setAtkDisplay(
                aliveSnag->atk + tragedyCount + panicCount, this, zero);
        }

        if (comedyCount > 0) {
            aliveSnag->setDefDisplay(aliveSnag->def + comedyCount,
                                     this, zero);
        }

        // big snag-type dispatch. organized to match the binary's nested
        // if/else-if/switch tree exactly. on the very first turn the rack
        // starts empty, so none of these fire.
        int snagType = aliveSnag->type;
        Game* g = getGame();

        switch (snagType) {

        case 1: {
            // start-of-rack snag: burns up to SPA_5 of its own HP per turn.
            uint32_t hpMinus1 = (uint32_t)(aliveSnag->hp - 1);
            uint32_t spa5     = (uint32_t)playerSystem.baseItemSpecialAbilityValue(5);
            uint32_t loss     = (hpMinus1 <= spa5) ? hpMinus1 : spa5;
            aliveSnag->deductHpClamped((int)loss, this, zero);
            break;
        }

        case 3: {
            // Bite: picks a rack tile without an alive snag and discards it.
            int picked = pickRandomRackSlotMatching(predRackHasNoAliveSnag);

            if (picked >= 0) {
                discardRackTile(picked, false);
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 4: {
            // Choking Grasp: stamps a kind-0 decoration on a random rack tile.
            int picked = pickRandomRackSlotMatching(predRackTrue);

            if (picked >= 0) {
                rack[picked]->pushDecoration(0, 0, 0);
                pushAction(0, zero, tile);
            }
            break;
        }

        case 5: {
            // Mania: one-shot per turn. swaps player ATK <-> DEF and kicks
            // the HUD slide animation. the HUD call gets the post-swap
            // values so each tint ends up showing the correct number after
            // the slide settles: tintAttack = new atk (= old def), tintDefence
            // = new def (= old atk).
            if (!maniaFiredOnce) {
                uint32_t oldAtk = (uint32_t)playerSystem.attack;
                uint32_t oldDef = (uint32_t)playerSystem.defence;
                playerSystem.attack  = (int)oldDef;
                playerSystem.defence = (int)oldAtk;
                hud.swapAtkDefDisplays(playerSystem.attack,
                                       playerSystem.defence);
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            maniaFiredOnce = true;
            break;
        }

        case 0xc: {
            // Gaping Maw: walks rack neighbours at slot deltas +/- stride for
            // stride 1..4. first neighbour with (alive snag of type 1) or
            // (alive content) gets erased. binary structure:
            //   for stride in 1..4:
            //     for sign in [-1, +1]:
            //       cand = slot + sign * stride
            //       if cand in [0,4] and rack[cand]:
            //         if snag.type==1 or aliveContent: pick this; break both
            int targetSlot = -1;

            for (int stride = 1; stride < 5 && targetSlot < 0; stride++) {

                for (int sign = -1; sign <= 1 && targetSlot < 0; sign += 2) {
                    int cand = slot + sign * stride;

                    if (cand < 0 || cand >= RACK_SLOT_COUNT) {
                        continue;
                    }

                    TileObject* candTile = rack[cand];

                    if (!candTile) {
                        continue;
                    }

                    if (candTile->getSnagType() == 1 ||
                        candTile->getTileContentIfAlive() != nullptr) {
                        targetSlot = cand;
                    }
                }
            }

            if (targetSlot >= 0) {
                rack[targetSlot]->erase();
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 0x10:
            // Procrastination: counter consumed in section 1; this is just
            // the visual ping (action + sound 0x21) per occurrence.
            pushAction(0, zero, tile);

            if (g) {
                g->soundQueue.trigger(0x21);
            }
            break;

        case 0x11: {
            // Doubt: one-shot toggle of consumedFlag. spawns a Shred-of-
            // Doubt reserve tile (snag type 0x12) only when the toggle
            // transitions from 1 -> 0 (i.e. the snag was already "consumed"
            // a prior turn and now spits its parasite out).
            if (!doubtFiredOnce) {
                uint8_t oldFlag = aliveSnag->consumedFlag;
                aliveSnag->consumedFlag = oldFlag ^ 1;

                if (oldFlag != 0) {
                    pushReserveSnagTile(0x12u, 0xFFFFFFFFu);
                }
            }
            doubtFiredOnce = true;
            break;
        }

        case 0x13:
            // Passion: atk += 2 (with stat tween), sound 0x21.
            aliveSnag->setAtkDisplay(aliveSnag->atk + 2, this, zero);

            if (g) {
                g->soundQueue.trigger(0x21);
            }
            break;

        case 0x14: {
            // Rage: transfers min(def, 3) from def into atk. inactive when
            // def is already 0.
            uint32_t transfer = (uint32_t)aliveSnag->def;

            if (transfer > 2u) {
                transfer = 3u;
            }

            if ((int)transfer > 0) {
                aliveSnag->setAtkDisplay(aliveSnag->atk + (int)transfer,
                                         this, zero);
                aliveSnag->setDefDisplay(aliveSnag->def - (int)transfer,
                                         this, zero);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 0x19:
            // Doppelganger: copies the player's current atk/def into its
            // own displayed stats. no action / no sound (silent mirror).
            aliveSnag->setAtkDisplay((int)playerSystem.attack, this, zero);
            aliveSnag->setDefDisplay((int)playerSystem.defence, this, zero);
            break;

        case 0x21:
            // Alarm Bells: atk += 1, sound 0x21.
            aliveSnag->setAtkDisplay(aliveSnag->atk + 1, this, zero);

            if (g) {
                g->soundQueue.trigger(0x21);
            }
            break;

        case 0x24: {
            // Indecision: stamps a kind-0 decoration on a random rack tile
            // that has an alive snag.
            int picked = pickRandomRackSlotMatching(predRackHasAliveSnag);

            if (picked >= 0) {
                rack[picked]->pushDecoration(0, 0, 0);
                pushAction(0, zero, tile);
            }
            break;
        }

        case 0x27:
            // Despair: set consumedFlag = 1.
            aliveSnag->consumedFlag = 1;
            break;

        case 0x2b: {
            // Cruelty: stamps decoration (kind=2, value=3) on a random rack tile.
            int picked = pickRandomRackSlotMatching(predRackTrue);

            if (picked >= 0) {
                rack[picked]->pushDecoration(2, 3, 0);
                pushAction(0, zero, tile);
            }
            break;
        }

        case 0x2d: {
            // Paranoia: picks a rack tile with no-alive-snag or snag.type==1.
            // if its content is type-7: bump magnitude (already caution).
            // else: erase content + stamp setTileContent(7, 1) (caution new).
            int picked = pickRandomRackSlotMatching(predRackNoSnagOrSnagType1);

            if (picked >= 0) {
                TileObject* victim = rack[picked];

                if (victim->getContentType() == 7) {
                    TileContent* tc = victim->getTileContentIfAlive();

                    if (tc) {
                        tc->setRawAndDisplayMagnitude(tc->displayedMagnitude + 1);
                    }
                }
                else {
                    victim->erase();
                    victim->setTileContent(7u, 1);
                }

                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 0x30: {
            // Creeping Dread: walks slots forward (slot+1 .. 4):
            //   - all-empty:         atk += 5, sound 0x21 (no action).
            //   - first non-null tile has alive snag of type != 1: bail,
            //     sound 0x21 only (no atk gain, no discard, no action).
            //   - first non-null tile has alive content or snag type == 1:
            //     discard that slot, push action, sound 0x21.
            int probe = slot + 1;

            while (probe < RACK_SLOT_COUNT && rack[probe] == nullptr) {
                probe++;
            }

            if (probe >= RACK_SLOT_COUNT) {
                aliveSnag->setAtkDisplay(aliveSnag->atk + 5, this, zero);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            else {
                TileObject* probeTile = rack[probe];

                if (probeTile->getSnagIfAlive() != nullptr &&
                    probeTile->getSnagType() != 1) {
                    // bail on rack-blocked path: sound only, no discard
                    if (g) {
                        g->soundQueue.trigger(0x21);
                    }
                }
                else {
                    discardRackTile(probe, false);
                    pushAction(0, zero, tile);

                    if (g) {
                        g->soundQueue.trigger(0x21);
                    }
                }
            }
            break;
        }

        case 0x35: {
            // Ennui: picks a filled rack tile (alive content or snag.type
            // == 1) and erases it, then bumps its own hp by the count of
            // fully-blank rack tiles. the predicate finds a tile to consume
            // (not to count); blanks are tallied separately afterwards.
            int picked = pickRandomRackSlotMatching(predRackTypeOneOrAliveContent);

            if (picked >= 0) {
                rack[picked]->erase();
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }

            int blankCount = countContentTypeInRack(0);
            aliveSnag->setHpDisplay(blankCount + aliveSnag->hp, this, zero);
            break;
        }

        case 0x37:
            // Pity: adds a 2-damage trap to its parent
            // tile, then push action. no sound.
            tile->pushDecoration(2, 2, 0);
            pushAction(0, zero, tile);
            break;

        case 0x41: {
            // Slow Poison: when player has no def, halve player's HP;
            // otherwise drop player's def by min(rackAliveSnagCount, def).
            // always followed by action + sound 0x21.
            if ((uint32_t)playerSystem.defence == 0u) {
                setHP((uint32_t)((playerSystem.currentHealth + 1) >> 1));
            }
            else {
                uint32_t aliveSnagsInRack = 0u;

                for (int s = 0; s < RACK_SLOT_COUNT; s++) {

                    if (rack[s] && rack[s]->getSnagIfAlive() != nullptr) {
                        aliveSnagsInRack++;
                    }
                }

                uint32_t playerDef = (uint32_t)playerSystem.defence;

                if (playerDef <= aliveSnagsInRack) {
                    aliveSnagsInRack = playerDef;
                }
                setDEF((int)(playerDef - aliveSnagsInRack));
            }
            pushAction(0, zero, tile);

            if (g) {
                g->soundQueue.trigger(0x21);
            }
            break;
        }

        case 0x45: {
            // Loneliness: advances Nemesis by the count of empty rack tiles.
            int blankCount = countContentTypeInRack(0);

            if (blankCount != 0) {
                nemesisAdvance(blankCount);
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 0x50: {
            // Overconfidence: def gains the in-progress xp cycle's count
            // (xpReceivedTotal % 10). sound 0x21 only when something gained.
            int gain = hud.xpReceivedTotal % 10;

            if (gain > 0) {
                aliveSnag->setDefDisplay(aliveSnag->def + gain, this, zero);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 0x66: {
            // Stagnation: sums currentCharges across every installed
            // EventSlot in the HUD tray. sound 0x21 only fires when the
            // sum is positive. binary builds a vector<EventSlot*> via
            // buildEventSlotList; we walk eventTray directly.
            int eventSum = 0;

            for (int i = 0; i < 4; i++) {
                EventSlot* s = hud.eventTray[i].slotPtr;

                if (s != nullptr) {
                    eventSum += s->currentCharges;
                }
            }

            aliveSnag->setHpDisplay(aliveSnag->hp + eventSum, this, zero);

            if (eventSum > 0) {

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 0x68: {
            // Stress: boosts def and atk by the count of rack
            // tiles that hold an alive snag. sound 0x21 fires after.
            int aliveCount = 0;

            for (int s = 0; s < RACK_SLOT_COUNT; s++) {

                if (rack[s] && rack[s]->getSnagIfAlive() != nullptr) {
                    aliveCount++;
                }
            }

            aliveSnag->setDefDisplay(aliveSnag->def + aliveCount, this, zero);
            aliveSnag->setAtkDisplay(aliveSnag->atk + aliveCount, this, zero);

            if (g) {
                g->soundQueue.trigger(0x21);
            }
            break;
        }

        case 0x6e: {
            // Vice: erases every rack tile whose content type is 3 (DEF
            // tile). action + sound 0x21 fire only when at least one was
            // erased.
            bool eraseFired = false;

            for (int s = 0; s < RACK_SLOT_COUNT; s++) {

                if (rack[s] && rack[s]->getContentType() == 3) {
                    rack[s]->erase();
                    eraseFired = true;
                }
            }

            if (eraseFired) {
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 0x71: {
            // Bitterness: drains the player's HP by the count of alive
            // non-type-1 rack snags, clamped so playerHP never drops below
            // 1. matches the snag's in-game text "You lose 1 {H} per held
            // special snag per turn. This can't reduce your {H} to 0."
            // gated on (count > 0 and playerHP > 1).
            int count = countAliveSnagsExceptType(1);

            if (count > 0 && (uint32_t)playerSystem.currentHealth > 1u) {
                int newHp = playerSystem.currentHealth - count;

                if (newHp < 2) {
                    newHp = 1;
                }
                setHP((uint32_t)newHp);
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;
        }

        case 0x72:
            // Authority: clamps player's atk down to its def when atk > def.
            if ((uint32_t)playerSystem.defence < (uint32_t)playerSystem.attack) {
                setATK(playerSystem.defence);
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;

        case 0x73:
            // Mockery: drains 1 xp from the current cycle and halves the
            // snag's own hp (rounding up). gated on a non-empty cycle.
            if (hud.xpReceivedTotal % 10 > 0) {
                hud.drainXPSlot(1, false);
                aliveSnag->setHpDisplay(((uint32_t)aliveSnag->hp + 1u) >> 1,
                                        nullptr, zero);
                pushAction(0, zero, tile);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
            }
            break;

        default:
            break;
        }
    }

    // ---- section 3: page list walk --------------------------------------
    // binary's iVar13: separate counter from the rack-walk panicCount
    // (iVar17). functionally identical (both are countSnagTypeInRack(0x1c))
    // but kept as a distinct local to mirror the binary's variable layout.
    int panicCountForPage = countSnagTypeInRack(0x1c);
    {
        // newest page tile = the most recently placed (= pageList.back()).
        // used by Claustrophobia (case 0xb) for its 2-exit gate and by
        // Doubt (case 0x11) to exclude the newest tile from the parasite
        // candidate pool. read once before the loop.
        TileObject* newestTile =
            pageList.empty() ? nullptr : pageList.back();

        for (TileObject* tile : pageList) {

            if (!tile) {
                continue;
            }

            // pre-pre-pass: kill a kind-0xd content tile when its snag is
            // still alive. matches FUN_100038e8c(tc) on the live content.
            TileContent* tc = tile->getTileContentIfAlive();
            SnagContent* sn = tile->getSnagIfAlive();

            if (tc && tc->type == 0xd && sn) {
                tc->killAndFade();
            }

            sn = tile->getSnagIfAlive();

            if (!sn) {
                continue;
            }

            // pre-effect: page-walk snags get the procrastination + panic
            // boost. binary passes boardOrigin (= &positionX) as the stat-
            // tween offset so the floating "+N" appears in world-space (the
            // snag is on the scrolled board, not the HUD).
            if (procrastinationCount > 0) {
                sn->setHpDisplay(sn->hp + procrastinationCount * 2,
                                 this, boardOrigin);
            }

            if (panicCountForPage > 0) {
                sn->setAtkDisplay(sn->atk + panicCountForPage,
                                  this, boardOrigin);
            }

            // big snag-type dispatch (~12 cases). organized to match the
            // binary's nested if-tree by ascending type code.
            int snagType = sn->type;
            Game* g = getGame();

            switch (snagType) {

            case 1: {
                // Snag start (page): burns up to SPA_0x11 (the page-walk
                // counterpart of SPA_5 used in the rack walk) of its own HP.
                uint32_t hpMinus1 = (uint32_t)(sn->hp - 1);
                uint32_t spa11    =
                    (uint32_t)playerSystem.baseItemSpecialAbilityValue(0x11);
                uint32_t loss     = (hpMinus1 <= spa11) ? hpMinus1 : spa11;
                sn->deductHpClamped((int)loss, this, boardOrigin);
                break;
            }

            case 5:
                // Mania (page): visual swap of the snag's own atk/def
                // displays (not a player stat swap). sound 0x21.
                sn->swapAtkDefDisplay();

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
                break;

            case 7: {
                // Judgement: atk += getExitCount(sn.parent). sound 0x21.
                TileObject* snParent = sn->tileParent;
                int exitCount = snParent ? snParent->getExitCount() : 0;
                sn->setAtkDisplay(sn->atk + exitCount, this, boardOrigin);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
                break;
            }

            case 0xa: {
                // Deja Vu: spawn a fresh snag-type-10 reserve tile, copy
                // current atk/def/hp into the spawn, then kill self. binary
                // passes a null tweenBoard for the spawn's stat displays
                // (no floating "+N"; the spawn starts at zero).
                SnagContent* spawned = pushReserveSnagTile(10u, 0xFFFFFFFFu);

                if (spawned) {
                    spawned->setAtkDisplay(sn->atk);
                    spawned->setDefDisplay(sn->def);
                    spawned->setHpDisplay(sn->hp);
                }
                sn->killAndFade();
                break;
            }

            case 0xb:
                // Claustrophobia: when the most-recently-placed page tile
                // has exactly 2 exits, double snag's atk and def. binary
                // reads pageList.back() (= newest tile), not sn.parent.
                if (newestTile && newestTile->getExitCount() == 2) {
                    sn->setAtkDisplay(sn->atk * 2, this, boardOrigin);
                    sn->setDefDisplay(sn->def * 2, this, boardOrigin);

                    if (g) {
                        g->soundQueue.trigger(0x21);
                    }
                }
                break;

            case 0x10: {
                // Procrastination (page): push action anchored on sn.parent
                // with boardOrigin offset, then sound 0x21.
                TileObject* snParent = sn->tileParent;
                pushAction(0, boardOrigin, snParent);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
                break;
            }

            case 0x11: {
                // Doubt (page): toggle consumedFlag. on falling edge:
                // pick a random page tile that has no alive snag and is
                // not the newest tile, transform it into a Shred of Doubt
                // (snag 0x12), push action + sound 0x21. binary always re-walks the
                // whole page list to build the candidate vector; no early
                // optimization for empty or single-tile lists.
                if (sn->consumedFlag == 0) {
                    sn->consumedFlag = 1;
                }
                else {
                    std::vector<TileObject*> candidates;

                    for (TileObject* candidate : pageList) {

                        if (candidate &&
                            candidate->getSnagIfAlive() == nullptr &&
                            candidate != newestTile) {
                            candidates.push_back(candidate);
                        }
                    }

                    if (!candidates.empty()) {
                        sn->consumedFlag = 0;
                        int idx = rngInt(0, (int)candidates.size() - 1, 4);
                        candidates[idx]->transformToSnag(0x12u, &playerSystem,
                                                         totalTurnCount,
                                                         levelTurnCount,
                                                         (int)worldIndex);
                        TileObject* snParent = sn->tileParent;
                        pushAction(0, boardOrigin, snParent);

                        if (g) {
                            g->soundQueue.trigger(0x21);
                        }
                    }
                }
                break;
            }

            case 0x13:
                // Passion (page): atk += 2. sound 0x21.
                sn->setAtkDisplay(sn->atk + 2, this, boardOrigin);

                if (g) {
                    g->soundQueue.trigger(0x21);
                }
                break;

            case 0x19:
                // Doppelganger (page): silently mirror player.attack /
                // defence into the snag's own displays. no action, no sound.
                // runs after section 2's Mania, so the player stats are
                // already swapped if Mania fired this turn.
                sn->setAtkDisplay((int)playerSystem.attack, this, boardOrigin);
                sn->setDefDisplay((int)playerSystem.defence, this, boardOrigin);
                break;

            case 0x35: {
                // Ennui (page): hp gains the count of fully-blank rack
                // tiles. binary computes new hp = sn.hp + countContentType
                // InRack(0). silent (no sound, no action).
                int blank = countContentTypeInRack(0);
                sn->setHpDisplay(blank + sn->hp, this, boardOrigin);
                break;
            }

            case 0x5c:
                // Masochism (page): atk += playerATK. silent.
                sn->setAtkDisplay((int)playerSystem.attack + sn->atk,
                                  this, boardOrigin);
                break;

            case 0x6c:
                // Honesty (page): hp += 1. silent.
                sn->setHpDisplay(sn->hp + 1, this, boardOrigin);
                break;

            default:
                break;
            }
        }
    }

    // ---- section 4: final stat sync -------------------------------------

    // defense floor: when playerDef < playerATK, bump def by perkLevel(5).
    // (binary calls FUN_1000569a0, perkLevel, not FUN_1000567b0 / SPA. easy
    // to confuse since both use arg 5.)
    if ((uint32_t)playerSystem.defence < (uint32_t)playerSystem.attack) {
        int perk5 = playerSystem.perkLevel(5);
        setDEF(perk5 + playerSystem.defence);
    }

    // low-HP attack boost: when currentHealth < maxHealth/2, bump atk by
    // perkLevel(1).
    if ((uint32_t)playerSystem.currentHealth <
        (uint32_t)playerSystem.maxHealth >> 1) {
        int perk1 = playerSystem.perkLevel(1);
        setATK(perk1 + playerSystem.attack);
    }

    // HP regen sources accumulator.
    int hpRegen = 0;

    // +1 per page tile whose content type is 0x19 (binary calls
    // getContentType, FUN_1000133b0, not getSnagType, so this matches
    // Milestone (content-0x19) tiles, not Doppelganger (snag-0x19) snags).
    for (TileObject* tile : pageList) {

        if (tile && tile->getContentType() == 0x19) {
            hpRegen++;
        }
    }

    // perkLevel(0xb) IFF rack contains 2+ HP tiles (content type 6).
    {
        int perkB = playerSystem.perkLevel(0xb);

        if (perkB != 0) {
            int hpTiles = countContentTypeInRack(6);

            if (hpTiles < 2) {
                perkB = 0;
            }

            hpRegen += perkB;
        }
    }

    // SPA_7 IFF (totalTurnCount & 1) == 0: applies on even turns only.
    if ((totalTurnCount & 1) == 0) {
        int spa7 = playerSystem.baseItemSpecialAbilityValue(7);
        hpRegen += spa7;
    }

    if (hpRegen > 0) {
        setHP((uint32_t)(playerSystem.currentHealth + hpRegen));
    }

    // SPA_4 ATK-tile bonus: first content-type-2 (ATK) tile in rack gets
    // its magnitude grown by SPA_4. stat tween fires on the tile's pos.
    int spa4 = playerSystem.baseItemSpecialAbilityValue(4);

    if (spa4 != 0) {

        for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {
            TileObject* tile = rack[slot];

            if (!tile || tile->getContentType() != 2) {
                continue;
            }

            TileContent* tc = tile->getTileContentIfAlive();

            if (tc) {
                tc->setRawAndDisplayMagnitude(tc->displayedMagnitude + spa4);
                pushStatTween(1, spa4, &tile->mainQuad.posX, 0);
            }

            break;   // first match only
        }
    }

    // SPA_0x10 DEF-tile bonus: last content-type-3 (DEF) tile in rack
    // gets its magnitude grown. binary walks the rack and keeps the
    // last-seen match (no break).
    int spa10 = playerSystem.baseItemSpecialAbilityValue(0x10);

    if (spa10 != 0) {
        TileObject* lastDef = nullptr;

        for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

            if (rack[slot] && rack[slot]->getContentType() == 3) {
                lastDef = rack[slot];
            }
        }

        if (lastDef) {
            TileContent* tc = lastDef->getTileContentIfAlive();

            if (tc) {
                tc->setRawAndDisplayMagnitude(tc->displayedMagnitude + spa10);
                pushStatTween(1, spa10, &lastDef->mainQuad.posX, 0);
            }
        }
    }

    // SPA_0xc HP-tile bonus: random content-type-6 (HP) tile in rack.
    int spaC = playerSystem.baseItemSpecialAbilityValue(0xc);

    if (spaC != 0) {
        std::vector<TileObject*> hpTiles;

        for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {

            if (rack[slot] && rack[slot]->getContentType() == 6) {
                hpTiles.push_back(rack[slot]);
            }
        }

        if (!hpTiles.empty()) {
            int pick = rngInt(0, (int)hpTiles.size() - 1, 4);
            TileObject* pickedTile = hpTiles[pick];
            TileContent* tc = pickedTile->getTileContentIfAlive();

            if (tc) {
                tc->setRawAndDisplayMagnitude(tc->displayedMagnitude + spaC);
                pushStatTween(1, spaC,
                              &pickedTile->mainQuad.posX, 0);
            }
        }
    }

    (void)dt;        // section 2's snag-0x45 (Loneliness) consumes dt.
    (void)boardOrigin;   // sections 2/3 will use it for action pushes.
}

// reconstructed from Ghidra FUN_1000174c4. takes only `this` in the
// binary; param_2 / param_3 in the Ghidra decompile are stale registers
// from the call site and never read by the function (verified via
// disassembly).
void GameBoard::recomputeScrollBounds(float /*touchInputY*/) {
    constexpr float SCROLL_MIN_X_PAD =  0.10312500f;  // DAT_100059f14
    constexpr float SCROLL_MIN_Y_PAD =  0.22343750f;  // DAT_100059f18
    constexpr float SCROLL_MAX_X_PAD =  0.89687502f;  // DAT_100059f1c
    constexpr float SCROLL_MAX_Y_F20 = -0.23750000f;  // DAT_100059f20
    constexpr float SCROLL_MAX_Y_F24 = -0.09843750f;  // DAT_100059f24
    constexpr float SCROLL_MAX_Y_F28 = -0.01562500f;  // DAT_100059f28

    // start fresh: clear (scrollMinX, scrollMinY) and (scrollMaxX, scrollMaxY).
    scrollMinX = 0.0f;
    scrollMinY = 0.0f;
    scrollMaxX = 0.0f;
    scrollMaxY = 0.0f;

    if (pageList.empty()) {
        // empty page list: bounds default to (PAD_F14, PAD_F18, PAD_F1C,
        // 0 + virtualHeight + F20 + F24 + F28).
        scrollMinX = SCROLL_MIN_X_PAD;
        scrollMinY = SCROLL_MIN_Y_PAD;
        scrollMaxX = SCROLL_MAX_X_PAD;
        scrollMaxY = Renderer::getVirtualHeight() + SCROLL_MAX_Y_F20 +
                     SCROLL_MAX_Y_F24 + SCROLL_MAX_Y_F28;
        return;
    }

    // walk the page list accumulating min/max of (-tile pos).
    float runningMinX = 0.0f;
    float runningMinY = 0.0f;
    float runningMaxX = 0.0f;
    float runningMaxY = 0.0f;

    for (TileObject* tile : pageList) {
        HexCellPos pos = hexCellLinearXY(tile->gridCol, tile->gridRow);

        float negX = -pos.x;
        float negY = -pos.y;

        if (runningMinX > negX) {
            runningMinX = negX;
        }

        if (runningMinY > negY) {
            runningMinY = negY;
        }

        if (runningMaxX < negX) {
            runningMaxX = negX;
        }

        if (runningMaxY < negY) {
            runningMaxY = negY;
        }
    }

    scrollMinX = runningMinX + SCROLL_MIN_X_PAD;
    scrollMinY = runningMinY + SCROLL_MIN_Y_PAD;
    scrollMaxX = runningMaxX + SCROLL_MAX_X_PAD;
    scrollMaxY = runningMaxY + Renderer::getVirtualHeight() + SCROLL_MAX_Y_F20 +
                 SCROLL_MAX_Y_F24 + SCROLL_MAX_Y_F28;
}

// reconstructed from Ghidra FUN_100020c40.
void GameBoard::setATK(int value) {

    if (playerSystem.attack == value) {
        return;
    }

    int delta = value - playerSystem.attack;
    playerSystem.attack = value;

    // delegate the HUD update: hud.setAttack does the canonical
    // setNumber + setPosition pair (with the binary-exact tint coords).
    hud.setAttack(value);

    // floating "+N" / "-N" tween over the ATK display.
    float src[2] = { 0.375f, 0.078125f };
    pushStatTween(/*textStyle*/ 1, delta, src, /*direction*/ 0);

    // achievement "Wax On, Wax Off" (= binary's FUN_10004e398). fires when
    // both ATK and DEF are 100+.
    if (playerSystem.attack > 99 && playerSystem.defence > 99) {
        getGame()->achievementTracker().increment(AchievementId::WaxOnWaxOff);
    }
}

// reconstructed from Ghidra FUN_100020ce0. mirror of setATK for DEF.
void GameBoard::setDEF(int value) {

    if (playerSystem.defence == value) {
        return;
    }

    int delta = value - playerSystem.defence;
    playerSystem.defence = value;

    // delegate the HUD update: hud.setDefence has the binary-exact
    // tint coords (0.6125, 0.046875) so it stays vertically aligned
    // with the ATK tint on the same row.
    hud.setDefence(value);

    // floating "+N" / "-N" tween over the DEF display.
    float src[2] = { 0.6015625f, 0.078125f };
    pushStatTween(/*textStyle*/ 1, delta, src, /*direction*/ 0);

    // achievement "Wax On, Wax Off" (= binary's FUN_10004e398). fires when
    // both ATK and DEF are 100+.
    if (playerSystem.attack > 99 && playerSystem.defence > 99) {
        getGame()->achievementTracker().increment(AchievementId::WaxOnWaxOff);
    }
}

// reconstructed from Ghidra FUN_100020d80.
void GameBoard::setHP(uint32_t value) {
    if (playerDowned) {
        return;
    }

    uint32_t maxHP = (uint32_t)playerSystem.maxHealth;
    uint32_t newHP = (value <= maxHP) ? value : maxHP;
    uint32_t delta = newHP - (uint32_t)playerSystem.currentHealth;

    if (delta == 0) {
        return;
    }

    Game* g = getGame();

    if (g) {
        // sound 0x17 = healing, 0x18 = damage. signed cast: when delta is
        // negative (= damage taken), it underflows uint to a large value
        // which the binary's `(int)delta < 1` test reads as damage.
        bool damage = ((int)delta < 1);
        g->soundQueue.trigger(damage ? 0x18 : 0x17);

        // gate TheEasyWay (= no damage taken on the way to world 2 on
        // Hard). FUN_10004e3e4 sets the per-run damage flag; first call
        // dirties the tracker, subsequent calls no-op.
        if (damage) {
            g->achievementTracker().notePlayerDamaged();
        }
    }

    // re-clamp post-trigger (binary repeats the cmp; harmless for us).
    playerSystem.currentHealth = (newHP <= maxHP) ? (int)newHP : (int)maxHP;

    // delegate the digit/ratio/bar refresh to HUD (= binary FUN_10000d18c).
    // holdRatio=1 keeps currentHealthRatio lerping rather than snapping.
    hud.setHealth((int)newHP, 1);

    // floating "+N" / "-N" tween over the HP display. delta is interpreted
    // signed here (negative = damage, positive = heal) to drive the
    // green-vs-red color in pushStatTween.
    float src[2] = { 0.490625f, 0.09375f };
    pushStatTween(/*textStyle*/ 0, (int)delta, src, /*direction*/ 0);

    // death case: HP just hit 0. snagActivationSuppressed = true disables snag
    // activation for the upcoming Nemesis-takeover sequence; flag1D = false
    // clears the chain-bonus flag so the takeover runs uninterrupted.
    if (newHP == 0) {
        playerDowned    = true;
        snagActivationSuppressed = true;
        flag1D                   = false;

        // achievement "Just a Flesh Wound" (= binary's FUN_10004dc74).
        // unconditional on hp-hit-zero; target is 20 deaths.
        getGame()->achievementTracker().increment(AchievementId::JustAFleshWound);

        // FUN_10000d998: flip the death overlay flag so draw() renders the
        // grayed-out bar in place of the active bar.
        hud.overlayState = 1;

        // walk page list, count tiles up through the first kind-4 (XP) tile
        // that exhausts the nemesis-level countdown. result clamps the
        // eat-cycle target. each XP tile in the trail "feeds" the nemesis
        // and shortens its eat range by one; we stop when its level's worth
        // of XP would be consumed.
        //
        // TODO_polish: BINARY-SHARES-BUG (death-revive skip). the budget is
        // nemesisLevel and this walk counts only pre-existing type-4 (XP)
        // tiles. a snag killed this same turn becomes a type-4 tile only later
        // (resolveSnagCombat's setTileContent(4,...) runs after this setHP
        // call), so at Nemesis level 1 with no prior XP tile in the trail,
        // tally == pageList.size(): Nemesis eats the whole trail and the
        // state-7 game-over fires before the state-8 revive. faithful to the
        // binary (FUN_100020d80 + FUN_100025dcc ordering). a gameplay fix (e.g.
        // clamp tally to pageList.size()-1 so a level-1 death leaves one tile
        // and reaches the revive) would be a deliberate deviation.
        int tally = 0;

        if (!pageList.empty()) {
            int xpBudget = nemesis.nemesisLevel;
            int idx = 1;

            for (TileObject* t : pageList) {
                tally = idx;

                if (t && t->getContentType() == 4) {

                    if (xpBudget < 2) {
                        break;
                    }

                    xpBudget--;
                }

                idx = tally + 1;
            }
        }

        // nemesis eat-cycle target (= total trail tiles to consume this
        // cycle): clamp up to the tally we just computed, then start the cycle.
        if (tally > nemesis.eatTarget) {
            nemesis.eatTarget = tally;
        }

        nemesis.eatActive = true;
    }
}

// =============================================================================
// action queue (GameBoard+0x9670)
// =============================================================================

// reconstructed from Ghidra FUN_1000386b0.
void GameBoard::pushAction(int actionType, const float* originXY, TileObject* tileRef) {
    constexpr float ROT_PI         = 3.1415927f;     // DAT_10005a230
    constexpr float ROT_DEG_FACTOR = 180.0f;         // DAT_10005a234 (rad -> deg)
    constexpr float ROT_DEG_OFFSET = 90.0f;          // DAT_10005a238

    // particle UV: 71x101 px sprite at (0, 356) on tex 9 (UI atlas), drawn
    // 1:1 (71x101 px on screen at 640px reference width).
    constexpr float ICON_U0 = 0.0f;                  // 0    / 1024
    constexpr float ICON_V0 = 356.0f / 1024.0f;
    constexpr float ICON_U1 =  71.0f / 1024.0f;
    constexpr float ICON_V1 = 457.0f / 1024.0f;
    constexpr float ICON_W  =  71.0f /  640.0f;
    constexpr float ICON_H  = 101.0f /  640.0f;

    constexpr int   PARTICLE_COUNT = 10;

    // walk front-to-back for the first completed (animT >= 1.0) entry we can
    // recycle. binary uses ordered-fp `(animT < 1.0) == NAN(animT)` which
    // resolves to "animT >= 1.0 and not NaN".
    ActionBody* node = nullptr;

    for (ActionBody& entry : actionQueue) {

        if (!(entry.animT < 1.0f)) {
            node = &entry;
            break;
        }
    }

    if (!node) {
        // no completed slot: append a fresh entry at the back.
        actionQueue.emplace_back();
        node = &actionQueue.back();
    }

    // (re)set the node payload: origin posX/posY pair plus the tileRef.
    node->animT      = 0.0f;
    node->actionType = actionType;
    node->dataX      = originXY[0];
    node->dataY      = originXY[1];
    node->tileRef    = tileRef;

    // type 0: visual icon-burst variant. resize to 10 particles, configure
    // each (UV / size / per-i rotation), then run one zero-dt kick to seed
    // the t=0 pose.
    if (actionType == 0) {
        node->count = PARTICLE_COUNT;
        node->particles.resize(PARTICLE_COUNT);

        for (uint64_t i = 0; i < node->count; i++) {
            Quad& q = node->particles[i].quad;
            q.setTexCoords(ICON_U0, ICON_V0, ICON_U1, ICON_V1);
            q.setSize(ICON_W, ICON_H);
            // rotation deg = (2i / count) * 180 + 90.  for count=10:
            //   i=0 -> 90, i=1 -> 126, i=2 -> 162, ... i=9 -> 414 (= 54).
            q.rotation = ((float)(i * 2) * ROT_PI / (float)node->count)
                         * ROT_DEG_FACTOR / ROT_PI + ROT_DEG_OFFSET;
        }

        kickActionAnim(0.0f, node);
    }
}

// reconstructed from Ghidra FUN_100038530.
void GameBoard::kickActionAnim(float dt, ActionBody* node) {
    constexpr float ANIM_DURATION   = 0.6f;          // DAT_10005a218
    constexpr float RADIUS_T0_BASE  = 0.015625f;     // DAT_10005a21c (1/64)
    constexpr float SCALE_T0        = 0.1f;          // DAT_10005a220
    constexpr float SCALE_T1        = 0.7f;          // DAT_10005a224
    constexpr float ANIM_PI         = 3.1415927f;    // DAT_10005a228
    constexpr float ALPHA_END       = 255.0f;        // DAT_10005a22c
    constexpr float RADIUS_T1_BASE  = 0.1328125f;    // immediate at FUN_100038530+0xa4

    // advance animT by dt/0.6, clamp to 1.0.
    float t = node->animT + dt / ANIM_DURATION;

    if (t > 1.0f) {
        t = 1.0f;
    }

    node->animT = t;

    // base position = source pos + optional tile pos (binary cbz x8 branch).
    float baseX = node->dataX;
    float baseY = node->dataY;

    if (node->tileRef) {
        baseX += node->tileRef->mainQuad.posX;
        baseY += node->tileRef->mainQuad.posY;
    }

    // per-frame curves precomputed once:
    float invT     = 1.0f - t;
    float radius   = t * RADIUS_T1_BASE + invT * RADIUS_T0_BASE;
    float scale    = t * SCALE_T1       + invT * SCALE_T0;

    // alpha: 0 at t=0/1, peaks at 255 around t=0.5. binary multiplies the
    // (1-ease) term by an explicitly-zeroed register, so its contribution
    // is always 0; we drop the dead branch.
    float ease   = 0.5f - std::cos((t + t) * ANIM_PI) * 0.5f;
    uint8_t alpha = (uint8_t)(int)(ease * ALPHA_END);

    // walk every particle Quad.
    for (uint64_t i = 0; i < node->count; i++) {
        float angle = (float)(i * 2) * ANIM_PI / (float)node->count;
        Quad& q = node->particles[i].quad;
        q.posX   = baseX + radius * std::cos(angle);
        q.posY   = baseY + radius * std::sin(angle);
        q.scaleX = scale;
        q.scaleY = scale;
        q.setAlpha(alpha);
    }
}

// reconstructed from Ghidra FUN_1000384cc.
void GameBoard::tickActionQueue(float dt) {

    for (ActionBody& entry : actionQueue) {

        if (entry.animT < 1.0f && entry.actionType == 0) {
            kickActionAnim(dt, &entry);
        }
    }
}

// reconstructed from Ghidra FUN_100038440.
void GameBoard::drawActionQueue() {

    for (ActionBody& entry : actionQueue) {

        if (entry.animT < 1.0f && entry.count != 0) {

            for (uint64_t i = 0; i < entry.count; i++) {
                entry.particles[i].quad.draw();
            }
        }
    }
}

// reconstructed from Ghidra FUN_1000388cc.
void GameBoard::clearActionQueue() {

    for (ActionBody& entry : actionQueue) {
        entry.animT = 1.0f;
    }
}

// =============================================================================
// stat-change tween queue (GameBoard+0x9650)
// =============================================================================

// reconstructed from Ghidra FUN_10002c00c (with FUN_10002c198 inlined).
void GameBoard::pushStatTween(int textStyle, int delta, const float* sourceXY,
                              int direction) {
    constexpr float Y_OFFSET_DOWN =  0.1f;     // DAT_10005a094
    constexpr float Y_OFFSET_UP   = -0.1f;     // DAT_10005a098

    statTweenAnyAnim = true;

    // walk front-to-back for the first completed (animT >= 1.0) entry we can
    // recycle. matches FUN_10002c00c's `(animT < 1.0) == NAN(animT)` test.
    TweenBody* node = nullptr;

    for (TweenBody& entry : statTween) {

        if (!(entry.animT < 1.0f)) {
            node = &entry;
            break;
        }
    }

    if (!node) {
        // no completed slot: append a fresh entry at the back.
        // ColorTint has no default ctor; we run init() now to mirror the
        // binary's stack-then-copy push.
        statTween.emplace_back();
        node = &statTween.back();
        node->tint.init();
    }

    // configure the ColorTint: signed digits + position + alpha + color.
    // setSignedNumber prepends a sign char (10 = plus, 11 = minus) to the
    // glyph list when textStyle != 2, so the popup reads "+N" / "-N".
    node->tint.setSignedNumber(delta, textStyle, 0);
    node->tint.setPosition(sourceXY[0], sourceXY[1], 0);
    node->tint.setAlpha(0xff);

    // sign-driven color: red for negative, green for positive. binary stores
    // each as a 32-bit literal; we just hand setColor the (R, G, B) bytes.
    if (delta < 0) {
        node->tint.setColor(0xff, 0x64, 0x64);
    } else {
        node->tint.setColor(0x64, 0xff, 0x64);
    }

    // store source/target pos and reset animT. target.x = source.x; target.y
    // = source.y +/- 0.1 per the direction flag (0 = drop down, !0 = float up).
    float yOffset = (direction == 0) ? Y_OFFSET_DOWN : Y_OFFSET_UP;
    node->sourceX = sourceXY[0];
    node->sourceY = sourceXY[1];
    node->targetX = sourceXY[0];
    node->targetY = sourceXY[1] + yOffset;
    node->animT   = 0.0f;
}

// reconstructed from Ghidra FUN_10002bee0.
void GameBoard::tickStatTween(float dt) {

    if (!statTweenAnyAnim) {
        return;
    }

    constexpr float ALPHA_END = 255.0f;        // DAT_10005a090

    // assume done unless the walk finds at least one still-animating entry.
    statTweenAnyAnim = false;

    for (TweenBody& entry : statTween) {

        if (!(entry.animT < 1.0f)) {
            continue;
        }

        // advance animT by dt (no division: animation lasts exactly 1 second).
        float t = entry.animT + dt;

        if (t > 1.0f) {
            t = 1.0f;
        }

        entry.animT = t;

        // linear lerp position from source to target.
        float invT = 1.0f - t;
        float x = entry.targetX * t + entry.sourceX * invT;
        float y = entry.targetY * t + entry.sourceY * invT;
        entry.tint.setPosition(x, y, 0);

        // alpha: full opacity for first half, then linear fade to 0 over
        // the second half. binary uses a `(t <= 0.5)` short-circuit and
        // a remap `2t - 1` over [0.5, 1.0] for the fade tail.
        uint8_t alpha;

        if (t <= 0.5f) {
            alpha = 0xff;
        } else {
            float remapped = (t + t) - 1.0f;
            alpha = (uint8_t)(int)((1.0f - remapped) * ALPHA_END);
        }

        entry.tint.setAlpha(alpha);
        statTweenAnyAnim = true;
    }
}

// reconstructed from Ghidra FUN_10002be84.
void GameBoard::drawStatTween() {

    if (!statTweenAnyAnim) {
        return;
    }

    for (TweenBody& entry : statTween) {

        if (entry.animT < 1.0f) {
            entry.tint.draw();
        }
    }
}

// reconstructed from Ghidra FUN_10002c170.
void GameBoard::clearStatTween() {
    statTweenAnyAnim = false;

    for (TweenBody& entry : statTween) {
        entry.animT = 1.0f;
    }
}

// FUN_100024308, finalize a tile's rotation on commit.
//
// useFitting=false branch (snap-only): set rotationStep to round(currentDeg / 60)
// clamped to 0..5, kick the lerp + step apply.
//
// useFitting=true branch: feed the rules engine (findFittingRotation) with
// the tile's current rotation as `seedRotation`, optionally with a direction
// hint pointing toward the level exit (useDirHint=true). the dirHint is
// computed as `((atan2(exitY - tileY, exitX - tileX) in degrees + 60) %
// 360) / 60`, a [0, 6) "step" toward the exit. the rules engine then
// prefers rotations whose exit edges face the level-end while still
// satisfying the placement constraints.
void GameBoard::finalizeTileRotation(TileObject* tile, bool useFitting, bool useDirHint) {
    constexpr float DEG_PER_STEP = 60.0f;       // DAT_100059ff0
    constexpr float HALF_REV     = 180.0f;      // DAT_100059ff4
    constexpr float PI_VAL       = 3.1415927f;  // DAT_100059ff8
    constexpr float FULL_REV     = 360.0f;      // DAT_100059ffc

    float currentStep = std::fmod(tileCurrentRotationDegrees(tile) / DEG_PER_STEP, 6.0f);

    if (!useFitting) {
        // snap-only: round to nearest of 0..5.
        int step = (int)(currentStep + 0.5f);

        if (step >= 6) {
            step = 0;
        }

        float targetDeg = (float)step * DEG_PER_STEP;
        tileSetRotationLerp(tile, targetDeg);
        tileSetRotationStep(tile, step, false);
        return;
    }

    // useFitting=true: feed rules engine
    const int* effectiveEnd = nullptr;

    if (keysCollected < keysRequired) {

        if (keysCollected + 1 < keysRequired) {
            effectiveEnd = &exitCol;
        } else {
            // edge case: one key short. allow connection to exit only when
            // the tile is exactly 1 hex from the exit.
            int distToExit = hexGridDistance(tile->gridCol, tile->gridRow,
                                             exitCol, exitRow);

            if (distToExit != 1) {
                effectiveEnd = &exitCol;
            }
        }
    }

    float dirHint = 0.0f;

    if (useDirHint) {
        HexCellPos exitPos  = hexCellLinearXY(exitCol, exitRow);
        HexCellPos tilePos  = hexCellLinearXY(tile->gridCol, tile->gridRow);
        float dx = exitPos.x - tilePos.x;
        float dy = exitPos.y - tilePos.y;
        float angleRad = std::atan2(dy, dx);
        float angleDeg = (angleRad * HALF_REV) / PI_VAL + DEG_PER_STEP;

        if (angleDeg < 0.0f) {
            angleDeg += FULL_REV;
        }

        dirHint = std::fmod(angleDeg / DEG_PER_STEP, 6.0f);
    }

    TileObject* lastTile = pageList.empty() ? nullptr : pageList.back();

    int bestRot = findFittingRotation(tile, tile->gridCol, tile->gridRow,
                                      lastTile, pageList,
                                      effectiveEnd, currentStep,
                                      useDirHint ? &dirHint : nullptr);

    if (bestRot < 0) {
        // findFittingRotation can return -1 when nothing fits. binary
        // doesn't guard against that, but feeding -1 to (-1 * 60) gives
        // -60 which would lerp to the wrong direction. clamp to 0.
        bestRot = 0;
    }

    float targetDeg = (float)bestRot * DEG_PER_STEP;
    tileSetRotationLerp(tile, targetDeg);
    tileSetRotationStep(tile, bestRot, false);
}

// FUN_1000245cc, control-tile cell predicate. true when the cell is
// "interesting" for the control-tile range overlay and the post-commit
// content-type-5 magnitude rule:
//   - HexMap.kind == 4: always pass (= an always-permitted cell, e.g. a
//     marked checkpoint).
//   - origin (0, 0): always fail (handled by `(*param_2 != 0 || ...)`).
//   - even-column cells: pass when row falls on a stride-3 (or stride-2,
//     depending on perkLevel(0x10)) line and the HexMap has no
//     recorded kind at this cell.
bool GameBoard::controlTileCellPredicate(int col, int row) const {
    int kind = hexMap.cellKindAt(col, row);

    if (kind == 4) {
        return true;
    }

    // origin always fails: binary's `(*param_2 != 0 || param_2[1] != 0)` gate.
    if (col == 0 && row == 0) {
        return false;
    }

    int absCol = (col < 0) ? -col : col;

    // odd-column cells fail the second check unconditionally.
    if (absCol & 1) {
        return false;
    }

    int itemSub = playerSystem.perkLevel(0x10);
    int stride  = (itemSub < 1) ? 3 : 2;

    int absRow = (row < 0) ? -row : row;
    int rowQuot = (stride != 0) ? (absRow / stride) : 0;

    return absRow == rowQuot * stride && kind == 0;
}

// FUN_100024678, control-tile range overlay. populates gameplayItems
// with up to N hint quads at the cells where the held control tile would
// score, each with alpha by hex distance to the anchor.
void GameBoard::updateControlTileHints() {
    // anchor = newest committed tile (= pageList.back()), or origin when
    // the page list is empty.
    int anchorCol = 0;
    int anchorRow = 0;

    if (!pageList.empty()) {
        const TileObject* newest = pageList.back();

        if (newest) {
            anchorCol = newest->gridCol;
            anchorRow = newest->gridRow;
        }
    }

    gameplayItemsLiveCount = 0;

    // 9x9 hex neighborhood: dCol in [-4, +4], dRow in [-4, +4], skip the
    // anchor itself. binary walks (iVar12, iVar9) = (dCol, dRow) outer/inner.
    for (int dCol = -4; dCol < 5; dCol++) {

        for (int dRow = -4; dRow < 5; dRow++) {

            if (dCol == 0 && dRow == 0) {
                continue;
            }

            int targetCol = anchorCol + dCol;
            int targetRow = anchorRow + dRow;

            if (!controlTileCellPredicate(targetCol, targetRow)) {
                continue;
            }

            int dist = hexGridDistance(anchorCol, anchorRow,
                                       targetCol, targetRow);

            if (dist >= 5) {
                continue;
            }

            // grow gameplayItems if liveCount has caught up to the back.
            // binary uses FUN_100007d78 (Quad ctor) + memcpy / FUN_100027af0
            // (reserve+push) here; std::vector::emplace_back gets the same
            // result: the value-initialized TileIcon's Quad runs its ctor.
            if ((size_t)gameplayItemsLiveCount >= gameplayItems.size()) {
                gameplayItems.emplace_back();
            }

            TileIcon& hint = gameplayItems[(size_t)gameplayItemsLiveCount];

            // UV (0.443359, 0.524414) to (0.511719, 0.589844) and size
            // (0.109375, 0.104688) match FUN_100024678's inline writes.
            hint.quad.setTexCoords(0.443359f, 0.524414f,
                                   0.511719f, 0.589844f);
            hint.quad.setSize(0.109375f, 0.104688f);

            // position: cellPos + DAT_10005a00c (= 0.0015625) on both axes.
            // binary uses the s1-side-effect trick on FUN_100012f04(mode=0)
            // to read hexY, same pattern as the exit-arrow port.
            HexCellPos cellPos = hexCellLinearXY(targetCol, targetRow);
            hint.quad.posX = cellPos.x + 0.0015625f;
            hint.quad.posY = cellPos.y + 0.0015625f;
            hint.quad.snapToPixelGrid();

            // alpha by distance bracket: binary uses a packed lookup
            // 0x326496ff shifted right by ((dist - 1) * 8).
            //   dist 1 -> 0xFF (full)
            //   dist 2 -> 0x96
            //   dist 3 -> 0x64
            //   dist 4 -> 0x32
            //   dist > 4 already filtered by the < 5 gate above.
            constexpr uint8_t kAlphaByDist[5] = { 0x00, 0xFF, 0x96, 0x64, 0x32 };
            hint.quad.setAlpha(kAlphaByDist[dist]);

            gameplayItemsLiveCount++;
        }
    }
}

// FUN_1000244d0, snap the held tile back to the rack when the player
// releases inside the held tile's bbox (sub-path 1 of FUN_10001b614 calls
// this after re-binding via commitRackTilePickup). also called by sub-path
// 3 (b)'s snap-back branch.
//
// clears all tileCursorVisible[] flags, then if selectedRackSlot is set:
//   - lift tile to absolute screen pos (mainQuad + boardPos)
//   - kick a rack slide-back via setRackPosition
//   - finalizeTileRotation(no fitting, no dir hint)
//   - resync magnitude via FUN_100013430 + FUN_100014c6c
//   - clear selectedRackSlot.
void GameBoard::snapTileBackToRack() {
    constexpr float RACK_X_BASE   =  0.109375f;   // DAT_10005a000
    constexpr float RACK_Y_OFFS_A = -0.0984375f;  // DAT_10005a004
    constexpr float RACK_Y_OFFS_B = -0.0203125f;  // DAT_10005a008
    constexpr float RACK_X_STEP   =  0.1953125f;  // matches initLevelContent's rack-X stride
    float virtualHeight = Renderer::getVirtualHeight();

    for (int i = 0; i < MAX_CURSOR_COUNT; i++) {
        tileCursorVisible[i] = false;
    }

    if (selectedRackSlot == -1) {
        return;
    }

    TileObject* tile = rack[selectedRackSlot];

    if (!tile) {
        selectedRackSlot = -1;
        return;
    }

    // shift tile from board-local back to absolute screen pos.
    tile->setPosition(tile->mainQuad.posX + positionX,
                      tile->mainQuad.posY + positionY);

    float rackTarget[2];
    rackTarget[0] = (float)selectedRackSlot * RACK_X_STEP + RACK_X_BASE;
    rackTarget[1] = virtualHeight + RACK_Y_OFFS_A + RACK_Y_OFFS_B;
    tile->setRackPosition(0.0f, rackTarget, true, true);

    finalizeTileRotation(tile, false, false);

    if (TileContent* tc = tile->getTileContentIfAlive()) {
        tc->setMagnitude(tc->rawMagnitude);
    }

    selectedRackSlot = -1;
}

// Android back button while in-game. see game_board.h for the routing
// summary. no binary equivalent; this wires the press to existing actions.
//
// checks run front-to-back so the topmost-drawn element owns the press. the
// draw order (game_board.cpp's draw, bottom to top) is: ... rack / staged tile
// ... pauseMenu -> userStatsPanel -> eventChoice -> levelUp -> itemChoice ->
// detailPanel -> dialogPanel -> achievementBanner. so every panel sits above
// the staged rack tile, and within the panels userStatsPanel is above
// pauseMenu. (achievementBanner is a transient slide-in notification, not an
// interactive layer, so the press passes through it.)
void GameBoard::handleBackPressed() {
    // tutorial hint (top interactive layer) -> dismiss it.
    if (dialogPanel.visible) {
        dialogPanel.reset(1);
        return;
    }

    // an in-progress gameplay modal (item / level-up / event choice) ->
    // absorb, so back never nullifies a pending choice.
    if (itemChoicePanel.visible || levelUpPanel.visible ||
        eventChoicePanel.visible) {
        return;
    }

    // user-stats card open -> close it (read-then-close, like the pause menu).
    if (userStatsPanel.visible) {

        if (userStatsPanel.active) {
            userStatsPanel.requestClose();
        }
        return;
    }

    // pause menu open. if the Forfeit confirm popup is up on top of it,
    // cancel just the popup (back to the pause menu); mirrors its No button
    // (confirmResult = 0 + panelHide(false)). otherwise resume the game
    // (panelHide(false) is the tap-off-panel close the pause menu uses). the
    // pause layer draws above the rack, so it takes the press before a
    // staged tile can.
    if (pauseMenu.visible) {

        if (pauseMenu.forfeitConfirmPanel.visible) {
            pauseMenu.forfeitConfirmPanel.confirmResult = 0;
            pauseMenu.forfeitConfirmPanel.panelHide(false);
        } else {
            pauseMenu.panelHide(false);
        }
        return;
    }

    // a tile staged in the rotation/confirm menu (below the panels) -> return
    // it to the rack.
    if (selectedRackSlot != -1) {
        snapTileBackToRack();
        return;
    }

    // mid-drag -> absorb (don't interrupt the gesture).
    if (draggedRackSlot != -1) {
        return;
    }

    // normal interactive play -> open the pause menu (mirrors the HUD menu
    // button at FUN_10001ae10 release case 2).
    detailPanel.reset(0);
    pauseMenu.open(seVolume, bgmVolume, tutorialFlag);
}

// FUN_100020580, drag-begin cursor-tab layout.
//
// invoked by commitRackTilePickup once the picked tile is bound. clears the 6
// directional cursor tabs, then either:
//   (A) page list empty -> mark tab[0] visible+enabled at hex (0,0), white.
//   (B) page list non-empty -> walk the most-recent page tile's permitted
//       exits. for each direction whose neighbor cell is unoccupied:
//         - tileCursorVisible[dir] = true
//         - tileCursorState[dir]   = canPlaceAtNeighbor(...)  (= rules engine result)
//         - color = white when rules say yes, red when no or when the
//                   neighbor is the level-exit cell and keysCollected <
//                   keysRequired (= player can't unlock the exit yet).
//         - tab[dir].pos = hexCellSnappedXY(neighborCol, neighborRow).
void GameBoard::setupDragCursorTabs(TileObject* picked) {

    for (int i = 0; i < MAX_CURSOR_COUNT; i++) {
        tileCursorVisible[i] = false;
        tileCursorState[i]   = false;
    }

    if (pageList.empty()) {
        tileCursorVisible[0] = true;
        tileCursorState[0]   = true;
        tileCursorQuads[0].quad.setColor(255, 255, 255, 255);
        HexCellPos centerPos = hexCellSnappedXY(0, 0);
        tileCursorQuads[0].quad.posX = centerPos.x;
        tileCursorQuads[0].quad.posY = centerPos.y;
        return;
    }

    // most-recently-placed tile (= pageList.back()): the binary uses this
    // as the "last placed" anchor for exit-walking.
    TileObject* lastTile = pageList.empty() ? nullptr : pageList.back();

    if (!lastTile) {
        return;
    }

    // exit cell coord for the rules-engine end-gate. binary reads
    // `(int *)(param_3 + 0x9708)` = our exitCol/exitRow pair.
    const int exitCoord[2] = {exitCol, exitRow };

    static constexpr int kDirDeltas[6][2] = {
        { 0, -1},  // 0
        { 1,  0},  // 1
        { 1,  1},  // 2
        { 0,  1},  // 3
        {-1,  0},  // 4
        {-1, -1},  // 5
    };

    for (int dir = 0; dir < 6; dir++) {

        if (!lastTile->permitsDirection(dir, -1)) {
            continue;
        }

        int neighborCol = lastTile->gridCol + kDirDeltas[dir][0];
        int neighborRow = lastTile->gridRow + kDirDeltas[dir][1];

        if (cellIsOccupied(neighborCol, neighborRow)) {
            continue;
        }

        tileCursorVisible[dir] = true;

        // pass null endCoord to the rules engine when the player has
        // already collected enough keys to unlock the exit; otherwise pass
        // the exit coord so the engine forbids early connection. binary's
        // gate is `keysRequired <= keysCollected || (keysRequired <=
        // keysCollected + 1 && distOne)`. translate: only feed endCoord
        // when keysRequired > keysCollected and not at the "one-from-end"
        // edge case where the proposed neighbor is adjacent to the exit.
        const int* effectiveEnd = nullptr;

        if (!(keysRequired <= keysCollected) &&
            !(keysRequired <= keysCollected + 1 &&
              hexGridDistance(neighborCol, neighborRow,
                              exitCoord[0], exitCoord[1]) == 1)) {
            effectiveEnd = exitCoord;
        }

        bool fits = canPlaceAtNeighbor(picked, neighborCol, neighborRow,
                                       lastTile, pageList,
                                       effectiveEnd);

        tileCursorState[dir] = fits;

        // exit-locked gate: the proposed neighbor is the exit cell and
        // the player still has keys outstanding -> force red, force
        // tileCursorState=0 so the directional walk rejects it.
        bool exitLocked = (neighborCol == exitCol &&
                           neighborRow == exitRow &&
                          keysCollected < keysRequired);

        uint8_t r, g, b, a;

        if (exitLocked) {
            tileCursorState[dir] = false;
            r = 255; g = 0; b = 0; a = 255;
        } else if (!fits) {
            r = 255; g = 0; b = 0; a = 255;
        } else {
            r = 255; g = 255; b = 255; a = 255;
        }

        tileCursorQuads[dir].quad.setColor(r, g, b, a);
        HexCellPos cellPos = hexCellSnappedXY(neighborCol, neighborRow);
        tileCursorQuads[dir].quad.posX = cellPos.x;
        tileCursorQuads[dir].quad.posY = cellPos.y;
    }
}

// FUN_100023ac4, commitRackTilePickup.
//
// binds rack[slotIdx] as the actively-dragged tile. captures drag offset
// (touch-to-tile center) for the per-frame position update, sets
// draggedRackSlot, kicks the slide animation, lays out the cursor-tab quads
// via setupDragCursorTabs (FUN_100020580), and plays sound 0xE ("tileGrab").
void GameBoard::commitRackTilePickup(int slotIdx) {

    if (slotIdx < 0 || slotIdx >= RACK_SLOT_COUNT || !rack[slotIdx]) {
        return;
    }

    TileObject* tile = rack[slotIdx];
    Game*       g    = getGame();

    if (!g) {
        return;
    }

    // FUN_100012d64(tile): snap slideTimer to 1.0, completing any in-progress
    // rack-slide animation so the drag starts from the tile's resting pos.
    tile->slideTimer = 1.0f;

    // capture drag offset = (tile.center - touch).
    dragOffsetX = tile->mainQuad.posX - g->touchX();
    dragOffsetY = tile->mainQuad.posY - g->touchY();

    // bind active drag slot.
    draggedRackSlot = slotIdx;

    // FUN_10001ff44, rack draw-order LRU (the +0x180 rackOrder list): drop any
    // existing entry for this slot, then append the active drag slot so the
    // picked-up tile draws on top of the others in draw()'s rackOrder walk.
    rackOrder.remove(slotIdx);
    rackOrder.push_back(draggedRackSlot);

    // FUN_10001282c(tile, 1): set slideAnimActive and reset iconFadeT.
    if (!tile->slideAnimActive) {
        tile->slideAnimActive = true;
        tile->iconFadeT       = 0.0f;
    }

    // FUN_100020580, lay out the 6 directional cursor tabs around the
    // most-recently-placed page tile. drives the directional walk's
    // candidate set in onPointerReleasedDuringDrag's sub-path 3 (b).
    setupDragCursorTabs(tile);

    // FUN_100023ac4 dispatches a control-tile-specific path: when the tile's
    // contentType == 5 (control) and it has no active kind=1 decoration,
    // run FUN_100024678 (control-tile range overlay).
    if (tile->getContentType() == 5 && !tile->hasActiveDecorationOfKind(1)) {
        updateControlTileHints();
    }

    // FUN_100013430 + FUN_100014c6c, TileContent magnitude resync. ensures
    // the picked tile's displayed digits match its rawMagnitude (defensive
    // sync; usually a no-op). harmless on tiles without TileContent.
    if (TileContent* tc = tile->getTileContentIfAlive()) {
        tc->setMagnitude(tc->rawMagnitude);
    }

    // FUN_10002493c, kick off the exit-arrow visualization. cosmetic only;
    // gates internally on hex-distance > 3 from the newest page tile.
    updateExitArrowVisualState();

    // FUN_10003a430, fade out the level-start dream snippet so the
    // pickup engagement doesn't visually trample an in-flight banner.
    animController.startFadeOut();

    // sound 0xE = "tileGrab" (sSoundBaseNames index 14 in game.cpp).
    g->soundQueue.trigger(0xE);
}

// FUN_100023ba8, tryPickupRackTile.
//
// validates whether the rack tile can be picked up under current state.
// returns true for normal pickup (caller proceeds to FUN_100023ac4 commit).
// returns false when blocked or when a special-action push fires instead;
// caller plays sound 0x12 ("disabled") on false.
//
// outer gates:
//   - tile.contentType == 8 or 9 -> blocked (return false)
//   - tile already has a tracked effect of type 0 -> blocked (return false)
//
// chained pre-checks for special cross-rack interactions. when commit is
// true, all three rules push an actionType=0 entry whose origin is the
// held tile's pos (when the held rack tile is the snag's parent) or
// (0, 0), with tileRef = snag's parentTile, then return false:
//   - rack has snag 0x16 and tile.contentType == 5 -> cross-rack rule 1
//   - rack has snag  9   and contentType != 2 and tile.snagType != 9 ->
//                                                  cross-rack rule 2
//   - rack has snag 0x65 and tile.contentType == 3 -> cross-rack rule 3
//
// per-snag-type switch (when no cross-rack rule fired): each case has a
// can-place condition derived from the snag's description text. pass ->
// return true (normal pickup), fail -> when commit, push an actionType=0
// entry with origin (0, 0) and tileRef = picked tile, then return false.
// types handled:
//   0x23 Vanity         : requires 3+ content-6 ({H}) tiles in rack
//   0x29 Legion         : requires no other snags in rack
//   0x47 Infestation    : requires self.def == 0
//   0x49 Scapegoat      : requires 5 snags held (full rack)
//   0x52 Grief          : requires self.def > player.attack
//   0x6d Stalking Horse : requires 2+ "special" snags (type != 1) in rack
// all other snag types fall through to default (no special rule, return true).
bool GameBoard::tryPickupRackTile(TileObject* tile, bool commit) {

    if (!tile) {
        return false;
    }

    // outer gate 1: contentType 8/9 are special pickup-blocked content kinds.
    int contentType = tile->getContentType();

    if (contentType == 8 || contentType == 9) {
        return false;
    }

    // outer gate 2: tile already has an active "kind 0" decoration -> pickup
    // blocked. (port of FUN_100012470, walks the +0x228 decoration list.)
    if (tile->hasActiveDecorationOfKind(0)) {
        return false;
    }

    // chained cross-rack pre-checks. an early rack holds no snags 0x16 / 9
    // / 0x65, so all three early-outs skip and we fall through to the
    // per-snag-type switch.
    //
    // each rule, when it fires:
    //   - return false when commit == false (predicate-only call -> block)
    //   - on commit, push an action-queue type-0 entry whose origin is the
    //     held tile's pos when the held rack tile is the snag's parent
    //     (so the burst tracks the held tile), else (0, 0); tileRef =
    //     snag's parentTile. then return false (cross-rack always blocks
    //     pickup; the queued action carries the side effect).
    auto pushSnagBlockedAction = [this](SnagContent* snag) {
        TileObject* parent = snag->tileParent;
        float origin[2] = { 0.0f, 0.0f };

        if (selectedRackSlot != -1 && rack[selectedRackSlot] == parent) {
            origin[0] = positionX;
            origin[1] = positionY;
        }

        pushAction(0, origin, parent);
    };

    SnagContent* rackSnag16 = findSnagInRack(0x16);

    if (rackSnag16 && contentType == 5) {

        if (!commit) {
            return false;
        }

        pushSnagBlockedAction(rackSnag16);
        return false;
    }

    SnagContent* rackSnag9 = findSnagInRack(9);

    // rule 2: snag-9 ("Tunnel Vision") restricts placement to 2-exit
    // tiles only, or the snag-9 tile itself. fires when the rack holds
    // a snag-9 and the picked tile has != 2 exits and its snagType != 9.
    // matches the binary's negated-OR gate via FUN_100012e0c (now
    // TileObject::getExitCount).
    if (rackSnag9 && tile->getExitCount() != 2 && tile->getSnagType() != 9) {

        if (!commit) {
            return false;
        }

        pushSnagBlockedAction(rackSnag9);
        return false;
    }

    SnagContent* rackSnag65 = findSnagInRack(0x65);

    if (rackSnag65 && contentType == 3) {

        if (!commit) {
            return false;
        }

        pushSnagBlockedAction(rackSnag65);
        return false;
    }

    // per-snag-type switch. each case checks a rack / stat condition;
    // pass -> return true (normal pickup), fail -> when commit, push a
    // type-0 action with origin (0, 0) and tileRef = picked tile, then
    // return false. predicate-only (commit == 0) skips the push.
    auto pushTypePlacementBlocked = [this](TileObject* picked) {
        float origin[2] = { 0.0f, 0.0f };
        pushAction(0, origin, picked);
    };

    int snagType = tile->getSnagType();

    switch (snagType) {
        case 0x23: {
            // Vanity: requires >= 3 content-type-6 ({H}, HP) tiles in rack.
            int hpTiles = 0;

            for (int i = 0; i < RACK_SLOT_COUNT; i++) {

                if (rack[i] && rack[i]->getContentType() == 6) {
                    hpTiles++;
                }
            }
            bool ok = hpTiles > 2;

            if (ok) {
                return true;
            }

            if (!commit) {
                return false;
            }

            pushTypePlacementBlocked(tile);
            return false;
        }

        case 0x29: {
            // Legion: only placeable while not holding any other snags.
            // binary's FUN_100026818(this, 0x29) counts rack snags whose
            // type != 0x29, i.e. snags that are not Legion. allowed iff
            // that count < 1.
            int otherSnags = 0;

            for (int i = 0; i < RACK_SLOT_COUNT; i++) {

                if (rack[i] && rack[i]->getSnagIfAlive() &&
                    rack[i]->getSnagType() != 0x29) {
                    otherSnags++;
                }
            }
            bool ok = otherSnags < 1;

            if (ok) {
                return true;
            }

            if (!commit) {
                return false;
            }

            pushTypePlacementBlocked(tile);
            return false;
        }

        case 0x47: {
            // Infestation: only placeable if its own {D} (def) is 0. binary
            // dereferences FUN_100013410's result with no null check; safe
            // because reaching this case means getSnagType() == 0x47, which
            // already implies an alive snag.
            bool ok = tile->getSnagIfAlive()->def == 0;

            if (ok) {
                return true;
            }

            if (!commit) {
                return false;
            }

            pushTypePlacementBlocked(tile);
            return false;
        }

        case 0x49: {
            // Scapegoat: only placeable while holding 5 snags (all rack
            // slots occupied by tiles with an alive snag).
            int snagCount = 0;

            for (int i = 0; i < RACK_SLOT_COUNT; i++) {

                if (rack[i] && rack[i]->getSnagIfAlive()) {
                    snagCount++;
                }
            }
            bool ok = snagCount > 4;

            if (ok) {
                return true;
            }

            if (!commit) {
                return false;
            }

            pushTypePlacementBlocked(tile);
            return false;
        }

        case 0x52: {
            // Grief: only placeable if its own {D} is higher than the
            // player's current {A}. binary reads board+0x83b4 = playerSystem
            // .attack and dereferences the snag with no null guard (same
            // reasoning as Infestation).
            uint32_t griefDef  = tile->getSnagIfAlive()->def;
            uint32_t playerAtk = static_cast<uint32_t>(playerSystem.attack);
            bool ok = playerAtk < griefDef;

            if (ok) {
                return true;
            }

            if (!commit) {
                return false;
            }

            pushTypePlacementBlocked(tile);
            return false;
        }

        case 0x6d: {
            // Stalking Horse: only placeable while holding at least 2
            // "special" snags. binary's FUN_100026818(this, 1) counts rack
            // snags whose type != 1 (type-1 is the generic / common snag).
            int specialSnags = 0;

            for (int i = 0; i < RACK_SLOT_COUNT; i++) {

                if (rack[i] && rack[i]->getSnagIfAlive() &&
                    rack[i]->getSnagType() != 1) {
                    specialSnags++;
                }
            }
            bool ok = specialSnags > 1;

            if (ok) {
                return true;
            }

            if (!commit) {
                return false;
            }

            pushTypePlacementBlocked(tile);
            return false;
        }

        default:
            break;
    }

    // default: no special rule -> normal pickup OK.
    return true;
}

// FUN_10001b614, onPointerReleasedDuringDrag.
//
// runs the touch-down + touch-up handling for the rack-pickup state machine:
//   sub-path 1: selectedRackSlot != -1 (tile post-commit, in rotation menu)
//     and touch-down over the held tile -> snap tile to drag pos, re-pickup
//     via FUN_100023ac4, clear selectedRackSlot.
//   sub-path 2: draggedRackSlot == -1 (no active drag) and touch-down
//     in rack zone -> walk rack, FUN_100023ba8 (pickup predicate) on the
//     touched tile, on success FUN_100023ac4 (commit pickup) sets
//     draggedRackSlot.
//   sub-path 3: draggedRackSlot != -1 (drag in progress) -> walks the 6
//     directional neighbor positions (tab quads at +0x6E0..+0x6E5), picks
//     the best fit, commits the tile to the page list or drops back into
//     drag-tracking mode.
//
// substantial body (~250 lines). with both slot fields = -1 (nothing held),
// all three branches early-out.
// reconstructed from Ghidra FUN_100023f84. wraps the snag DetailPanel
// populator with a combat simulation: ATK/DEF/HP exchange between the
// player and the snag until one side reaches 0 (or 99 turns elapse). the
// resulting (playerDeathTurn, snagDeathTurn) get fed into the panel's two
// side-stat ColorTints so the player can preview "this snag wins in N
// turns, the player wins in M turns".
//
// before simulating, the binary reshapes the player's starting stats via
// the pre-loop per-snag-type switch (Nostalgia 0xd, Hubris 0xe,
// PiercingGaze 0x1f, FalseHope 0x2f, Malice 0x3d, Terror 0x3f, Apathy
// 0x4f), all ported in the switch below. each turn both sides degrade
// through degradeStatsAfterCombat (FUN_100056658) and resolveCombatDelta
// (FUN_10003e2f8), the same helpers resolveSnagCombat uses, so the
// preview is bit-for-bit consistent with actual combat.
void GameBoard::openSnagDetailWithCombatSim(int mode, const float* anchor,
                                            SnagContent* snag) {
    detailPanel.touchHoldArea = mode;

    if (mode == 2) {
        Game* g = getGame();

        if (g) {
            detailPanel.touchOrigX = g->touchX();
            detailPanel.touchOrigY = g->touchY();
        }
    }

    if (!snag) {
        return;
    }

    // initial stats: snag and player both pull from their owning structs.
    int snagAtk   = snag->atk;
    int snagDef   = snag->def;
    int snagHP    = snag->hp;
    int playerAtk = playerSystem.attack;
    int playerDef = playerSystem.defence;
    int playerHP  = playerSystem.currentHealth;
    int snagType  = snag->type;
    bool inRack   = !(snag->tileParent && snag->tileParent->committed);

    // ---- pre-loop per-snag-type stat modifiers (FUN_100023f84 head) ----
    // most fire only when the snag is still in the rack (parent tile not
    // yet committed); PiercingGaze fires unconditionally.
    switch (snagType) {
        case (int)SnagKind::Nostalgia:     // 0x0d - "Resets your {A} to zero"

            if (inRack) {
                playerAtk = 0;
            }
            break;

        case (int)SnagKind::Hubris:        // 0x0e - "Resets your {D} to zero"

            if (inRack) {
                playerDef = 0;
            }
            break;

        case (int)SnagKind::PiercingGaze:  // 0x1f - "Ignores your {D}"
            playerDef = 0;
            break;

        case (int)SnagKind::FalseHope:     // 0x2f - "{D} added to {H}"

            if (inRack) {
                playerDef += playerHP;
                playerHP   = 1;
            }
            break;

        case (int)SnagKind::Malice:        // 0x3d - "Lose {H} equal to your {A}"

            if (inRack) {
                playerHP -= playerAtk;
            }
            break;

        case (int)SnagKind::Terror:        // 0x3f - "Reduces your {H} to 1"

            if (inRack) {
                playerHP = 1;
            }
            break;

        case (int)SnagKind::Apathy:        // 0x4f - "Halves your {A} {H} {D}"

            if (inRack) {
                playerAtk = (playerAtk + 1) / 2;
                playerDef = (playerDef + 1) / 2;
                playerHP  = (playerHP  + 1) / 2;
            }
            break;

        default:
            break;
    }

    int playerDeathTurn = 99;
    int snagDeathTurn   = 99;
    int turn            = 1;
    int prevPlayerTurn  = 99;
    int prevSnagTurn    = 99;

    while (true) {
        // ---- damage to snag (= player-source damage) ----
        // most snags: clamp(player.atk - snag.def, 0). overrides:
        //   Masochism (0x5c): damage is the snag's own attack instead
        //     (the snag turns its own power against itself).
        //   Stubbornness (0x20): damage cannot reduce snag.hp below 1
        //     unless snag.hp is already 1.
        //   Attrition (0x70): damage is clamped to 1 max per turn.
        int playerDamageOnSnag;

        if (snagType == (int)SnagKind::Masochism) {
            playerDamageOnSnag = snagAtk;
        } else {
            playerDamageOnSnag = playerAtk - snagDef;

            if (playerAtk < snagDef || playerDamageOnSnag <= 0) {
                playerDamageOnSnag = 0;
            } else if (snagType == (int)SnagKind::Stubbornness && snagHP > 1) {
                int cap = snagHP - 1;

                if (playerDamageOnSnag > cap) {
                    playerDamageOnSnag = cap;
                }
            }

            if (snagType == (int)SnagKind::Attrition && playerDamageOnSnag > 1) {
                playerDamageOnSnag = 1;
            }
        }

        // ---- damage to player (= snag-source damage) ----
        // standard clamp(snag.atk - player.def, 0). no per-type overrides.
        int snagDamageOnPlayer = snagAtk - playerDef;

        if (snagAtk < playerDef || snagDamageOnPlayer <= 0) {
            snagDamageOnPlayer = 0;
        }

        snagHP   -= playerDamageOnSnag;
        playerHP -= snagDamageOnPlayer;

        // first turn at which each side dies; preserve once set.
        playerDeathTurn = (prevPlayerTurn != 99 || playerHP > 0) ? prevPlayerTurn : turn;
        snagDeathTurn   = (prevSnagTurn   != 99 || snagHP   > 0) ? prevSnagTurn   : turn;

        if (playerDeathTurn < 99 && snagDeathTurn < 99) {
            break;
        }

        // ---- per-turn stat decay -------------------------------------
        // both sides degrade through the same helpers the real combat uses
        // (FUN_100025dcc / resolveSnagCombat calls these too), so the
        // preview numbers match actual combat exactly: SpecialAbility
        // pierce bonuses, ShredOfDoubt's no-decay, Stubbornness's
        // def = damage-dealt, Parasite's atk-preserve, all included.
        // arg order mirrors the real-combat site: degrade takes the
        // player-damage-on-snag gate; resolveCombatDelta's posDelta is
        // snagDamageOnPlayer and defDelta is playerDamageOnSnag.
        uint32_t pAtk = (uint32_t)playerAtk;
        uint32_t pDef = (uint32_t)playerDef;
        playerSystem.degradeStatsAfterCombat(&pAtk, &pDef, snag,
                                             playerDamageOnSnag);
        playerAtk = (int)pAtk;
        playerDef = (int)pDef;

        uint32_t sAtk = (uint32_t)snagAtk;
        uint32_t sDef = (uint32_t)snagDef;
        snag->resolveCombatDelta(&playerSystem, &sAtk, &sDef,
                                 snagDamageOnPlayer, playerDamageOnSnag);
        snagAtk = (int)sAtk;
        snagDef = (int)sDef;

        turn++;

        if (turn > 0x62) {
            break;
        }

        prevPlayerTurn = playerDeathTurn;
        prevSnagTurn   = snagDeathTurn;
    }

    // FUN_100026878, decide whether to render combat-preview decoration
    // (combatSimPreview + win-turn digits). show the preview when:
    //   - the snag's parent tile is already placed on the board, or
    //   - the snag is Myopia (kind 0x63, inspecting itself: its
    //     "While Held: hide stats" effect doesn't gate its own card), or
    //   - the snag is not Lie (kind 0x4B, "While Held: Can't see its
    //     stats") and no Myopia snag sits in the rack right now (Myopia
    //     blanks combat previews for every other rack snag).
    bool extraFlag;
    bool committed = snag->tileParent && snag->tileParent->committed;

    if (committed || snag->type == (int)SnagKind::Myopia) {
        extraFlag = true;
    } else if (snag->type == (int)SnagKind::Lie) {
        extraFlag = false;
    } else {
        // FUN_1000201e8, scan the rack for a Myopia snag. cheap inline.
        extraFlag = true;

        for (int i = 0; i < RACK_SLOT_COUNT; i++) {

            if (rack[i] && rack[i]->getSnagType() == (int)SnagKind::Myopia) {
                extraFlag = false;
                break;
            }
        }
    }

    // matches the binary's FUN_100023f84 -> FUN_10003fe64 arg order:
    //   param_5 = iVar2 = playerDeathTurn  -> playerDeathTurns field (+0x1038)
    //   param_6 = iVar3 = snagDeathTurn    -> snagDeathTurns   field (+0x1000)
    // so the left slot shows the turn-snag-dies count and the right slot
    // shows the turn-player-dies count, matching what iOS renders.
    detailPanel.populateForSnag(0.098438f, anchor, snag,
                                playerDeathTurn, snagDeathTurn,
                                extraFlag ? 1 : 0);
}

void GameBoard::onPointerReleasedDuringDrag() {
    Game* g = getGame();

    // sub-path 1: held-tile snap-back.
    if (selectedRackSlot != -1 && g && g->inputState() == 1) {
        TileObject* heldTile = rack[selectedRackSlot];

        // binary computes the touch in board-local coords:
        //   touchOffset = (touchX - boardX, touchY - boardY)
        // then bbox-tests against the tile's local-space quad. we mirror
        // that here even though our bbox helper isn't ported yet; without
        // the gate, we'd snap-back on every release-while-held, losing the
        // player's drag-and-place gesture entirely.
        // FUN_1000083bc port = Quad::contains.
        bool overHeldTile = (g != nullptr) && heldTile &&
                            heldTile->mainQuad.contains(g->touchX(), g->touchY());

        if (overHeldTile && heldTile) {
            // matches binary: setPosition(tile.posX + boardX, tile.posY +
            // boardY). this writes screen coords into the tile's local pos,
            // so the next frame's render shows the tile at its captured
            // screen position. commitRackTilePickup (= FUN_100023ac4)
            // below is the rack-anchor-restore follow-up.
            heldTile->setPosition(heldTile->mainQuad.posX + positionX,
                                  heldTile->mainQuad.posY + positionY);
            // re-bind the held tile as the actively-dragged one (cancel the
            // commit-pending state from the rotation menu, fall back into
            // drag mode).
            commitRackTilePickup(selectedRackSlot);
            selectedRackSlot = -1;
        }
    }

    // sub-path 2: pointer-down over a rack tile, no drag in progress.
    if (draggedRackSlot == -1) {

        if (g && g->inputState() == 1) {
            float titleBottomY = titleQuad.posY + titleQuad.height * -0.5f;

            if (!(g->touchY() == titleBottomY ||
                  g->touchY() < titleBottomY)) {
                // walk rack to find the tile the pointer is over.
                for (int i = 0; i < RACK_SLOT_COUNT; i++) {

                    if (!rack[i]) {
                        continue;
                    }

                    // bbox test: pointer over rack[i]'s mainQuad?
                    // FUN_1000083bc port = Quad::contains.
                    bool over = rack[i]->mainQuad.contains(g->touchX(), g->touchY());

                    if (over) {
                        // run the pickup predicate. true: commit pickup;
                        // false: play "disabled" sound 0x12.
                        bool ok = tryPickupRackTile(rack[i], /*commit=*/true);

                        if (ok) {
                            commitRackTilePickup(i);

                            // FUN_10001dca0: pan the camera onto the newest
                            // placed tile (= the player's current spot) when a
                            // rack tile is picked up; cell (0,0) if nothing has
                            // been placed yet. (mislabeled "sub-anim seed"; it's
                            // setNemesisPanTarget.)
                            if (!pageList.empty()) {
                                TileObject* newest = pageList.back();
                                setNemesisPanTarget(newest->gridCol, newest->gridRow);
                            } else {
                                setNemesisPanTarget(0, 0);
                            }
                        } else {
                            Game* gnd = getGame();

                            if (gnd) {
                                gnd->soundQueue.trigger(0x12);
                            }
                        }

                        // post-tap: open the DetailPanel for this rack tile.
                        // both branches gate on `!hasActiveDecorationOfKind(1)`:
                        // if the tile has a kind-1 decoration active (the
                        // "drawn-this-turn" marker etc.) the binary skips the
                        // panel to avoid distracting the player mid-effect.
                        SnagContent* sc = rack[i] ? rack[i]->getSnagIfAlive() : nullptr;
                        constexpr float RACK_DETAIL_HEADER_Y = 0.098438f;  // DAT_100059fa4

                        if (sc) {

                            if (!rack[i]->hasActiveDecorationOfKind(1)) {
                                float anchor[2] = {
                                    rack[i]->mainQuad.posX,
                                    rack[i]->mainQuad.posY
                                };
                                openSnagDetailWithCombatSim(/*mode=*/1, anchor, sc);
                            }
                        } else if (rack[i]) {
                            TileContent* tc = rack[i]->getTileContentIfAlive();

                            if (tc && (unsigned)tc->type < 26 &&
                                kTileTextTable[tc->type].name[0] != '\0' &&
                                !rack[i]->hasActiveDecorationOfKind(1)) {
                                detailPanel.touchHoldArea = 1;
                                float anchor[2] = {
                                    rack[i]->mainQuad.posX,
                                    rack[i]->mainQuad.posY
                                };
                                detailPanel.populateForContent(
                                    RACK_DETAIL_HEADER_Y, anchor, tc);
                            }
                        }

                        break;
                    }
                }
            }
        }

        if (draggedRackSlot == -1) {
            return;
        }
    }

    // sub-path 3: drag in progress (draggedRackSlot != -1). binary splits
    // into:
    //   (a) inputState != 0: drag-track tile to (touch + dragOffset),
    //       then return.
    //   (b) inputState == 0: directional walk to commit / snap back.
    TileObject* heldTile = (draggedRackSlot >= 0 && draggedRackSlot < RACK_SLOT_COUNT)
                           ? rack[draggedRackSlot] : nullptr;

    if (g && g->inputState() != 0) {

        if (heldTile) {
            heldTile->setPosition(g->touchX() + dragOffsetX,
                                  g->touchY() + dragOffsetY);
        }

        return;
    }

    // (b) released: directional walk for commit / snap-back.
    //
    // build two parallel vectors:
    //   candidates[i] = (screen_x, screen_y) of candidate landing positions.
    //   indices[i]    = direction index 0..5 for cursor candidates, or 0
    //                   for the rack-slot snap-back position at i==0.
    //
    // candidate 0 is always the rack snap-back position; candidates 1..N
    // are the enabled cursor cells in cursor-index order. choose the one
    // whose screen position is closest to the held tile's current pos.
    // bestIdx == 0: snap back to rack; bestIdx > 0: commit at
    // indices[bestIdx]'s direction.
    if (heldTile) {
        constexpr float RACK_X_BASE   =  0.109375f;   // DAT_100059fa8
        constexpr float RACK_Y_OFFS_A = -0.0984375f;  // DAT_100059fac
        constexpr float RACK_Y_OFFS_B = -0.0203125f;  // DAT_100059fb0
        constexpr float RACK_X_STEP   =  0.1953125f;  // DAT_10005a224 (rack-X stride)
        constexpr float HINT_OFFSET   =  0.015625f;   // DAT_100059fb8
        constexpr float CLOSEST_INIT  =  1000.0f;     // DAT_100059fb4
        float virtualHeight = Renderer::getVirtualHeight();

        std::vector<HexCellPos> candidates;
        std::vector<int>        indices;

        // candidate 0: rack snap-back position (slot * stride + base, virtualHeight - offsets).
        candidates.push_back({
            (float)draggedRackSlot * RACK_X_STEP + RACK_X_BASE,
            virtualHeight + RACK_Y_OFFS_A + RACK_Y_OFFS_B
        });
        indices.push_back(0);

        // candidates 1..N: enabled cursor cells.
        for (int dir = 0; dir < MAX_CURSOR_COUNT; dir++) {

            if (!tileCursorState[dir]) {
                continue;
            }

            candidates.push_back({
                tileCursorQuads[dir].quad.posX + positionX,
                tileCursorQuads[dir].quad.posY + positionY
            });
            indices.push_back(dir);
        }

        // find the closest candidate to the held tile's screen pos.
        int bestIdx = 0;
        float bestDist = CLOSEST_INIT;

        for (size_t i = 0; i < candidates.size(); i++) {
            float dx = heldTile->mainQuad.posX - candidates[i].x;
            float dy = heldTile->mainQuad.posY - candidates[i].y;
            float d2 = dx * dx + dy * dy;

            if (d2 < bestDist) {
                bestDist = d2;
                bestIdx  = (int)i;
            }
        }

        if (bestIdx > 0) {
            // commit branch: place tile on a hex.
            int chosenDir = indices[bestIdx];

            // compute the placed (col, row).
            int neighborCol;
            int neighborRow;

            if (pageList.empty()) {
                neighborCol = 0;
                neighborRow = 0;
            } else {
                TileObject* anchorTile = pageList.back();

                static constexpr int kCommitDeltas[6][2] = {
                    { 0, -1},  // 0
                    { 1,  0},  // 1
                    { 1,  1},  // 2
                    { 0,  1},  // 3
                    {-1,  0},  // 4
                    {-1, -1},  // 5
                };
                neighborCol = anchorTile->gridCol + kCommitDeltas[chosenDir][0];
                neighborRow = anchorTile->gridRow + kCommitDeltas[chosenDir][1];
            }

            heldTile->setGridCoord(neighborCol, neighborRow);

            // hint-quad positions at +0x798 (navArrowQuad, neighbor east)
            // and +0x870 (backButtonQuad, neighbor west). only the
            // backButton's X gets the 0.015625 offset; the Y component
            // adds nothing.
            HexCellPos navHint  = hexCellLinearXY(heldTile->gridCol + 1,
                                                  heldTile->gridRow);
            navArrowQuad.quad.posX = navHint.x;
            navArrowQuad.quad.posY = navHint.y;

            HexCellPos backHint = hexCellLinearXY(heldTile->gridCol - 1,
                                                  heldTile->gridRow);
            backButtonQuad.quad.posX = backHint.x + HINT_OFFSET;
            backButtonQuad.quad.posY = backHint.y;

            navArrowQuad.quad.snapToPixelGrid();
            backButtonQuad.quad.snapToPixelGrid();

            // shift held tile from absolute screen pos to board-local pos.
            heldTile->setPosition(heldTile->mainQuad.posX - positionX,
                                  heldTile->mainQuad.posY - positionY);

            // kick rack-slide to the hex's local screen pos.
            HexCellPos hexLocal = hexCellLinearXY(heldTile->gridCol,
                                                  heldTile->gridRow);
            float rackTarget[2] = { hexLocal.x, hexLocal.y };
            heldTile->setRackPosition(0.0f, rackTarget, true, true);

            // run snapTileBackToRack: clears all cursor flags and snaps any
            // pre-existing held tile back. on this path selectedRackSlot
            // is -1 (we haven't bound it yet), so the body just clears
            // tileCursorVisible[0..5] and returns.
            snapTileBackToRack();

            // when the rack has a snag of type 0x43 in it, skip the rotation
            // finalize (binary's `if (FUN_1000201e8(0x43)) skip`).
            if (findSnagInRack(0x43) == nullptr) {
                finalizeTileRotation(heldTile, true, true);
            }

            // set selectedRackSlot for the next state-machine step.
            selectedRackSlot = draggedRackSlot;

            // mark only the chosen direction's cursor visible.
            for (int j = 0; j < MAX_CURSOR_COUNT; j++) {
                tileCursorVisible[j] = (j == chosenDir);
            }

            // post-commit content-type dispatch:
            //   type 2/3/6 (ATK/CTRL/DEF boost): if HexMap kind == 7, double
            //     the displayed magnitude.
            //   type 5 (CTRL): if controlTileCellPredicate(neighbor) returns
            //     false, zero out the magnitude.
            int contentType = heldTile->getContentType();

            if (contentType == 2 || contentType == 3 || contentType == 6) {
                int kind = hexMap.cellKindAt(heldTile->gridCol, heldTile->gridRow);

                if (kind == 7) {
                    TileContent* tc  = heldTile->getTileContentIfAlive();
                    int          mag = heldTile->getContentMagnitude();

                    if (tc) {
                        tc->setMagnitude(mag * 2);
                    }
                }
            } else if (contentType == 5) {

                if (!controlTileCellPredicate(heldTile->gridCol,
                                              heldTile->gridRow)) {
                    TileContent* tc = heldTile->getTileContentIfAlive();

                    if (tc) {
                        tc->setMagnitude(0);
                    }
                }
            }
        } else {
            // snap-back branch: bestIdx == 0 or no candidates.
            float rackTarget[2];
            rackTarget[0] = (float)draggedRackSlot * RACK_X_STEP + RACK_X_BASE;
            rackTarget[1] = virtualHeight + RACK_Y_OFFS_A + RACK_Y_OFFS_B;
            heldTile->setRackPosition(0.0f, rackTarget, true, true);
            finalizeTileRotation(heldTile, false, false);

            // clear all 6 tileCursorVisible flags (binary's strh + str pair
            // covers +0x6E0..+0x6E5).
            for (int j = 0; j < MAX_CURSOR_COUNT; j++) {
                tileCursorVisible[j] = false;
            }
        }
    }

    // common cleanup (LAB_10001bc94).
    gameplayItemsLiveCount = 0;
    exitArrowVisible = false;
    draggedRackSlot = -1;
}

// FUN_10001bd2c, animateMidGrabHexHighlight.
//
// drives the hex-cursor mirror + "where will this land" predictive marker
// while the player is mid-drag of a rack tile. three branches keyed off
// the touch state field at +0x99C:
//   touch state 1 (begin): hex-mirror kickoff. computes where the tile
//     would land via FUN_100012fa4 (= hex-grid coord to screen coord), checks
//     if that hex already has a tile in the page list, and starts the
//     appropriate visual feedback.
//   touch state 2 (moving): apply velocity-tracked smoothing. updates the
//     pan anchor / prev-touch / velocity fields.
//   touch state 0 (released): decay the +0x9C8 progress toward 0.
//
// when no drag is active (+0x99C = 0, the default), no branch matches input.
void GameBoard::animateMidGrabHexHighlight(float dt) {
    Game* g = getGame();

    if (!g) {
        return;
    }

    int touchState = g->inputState();

    bool& dragActive = panDragActive;

    constexpr float DRAG_SPEED_CAP    = 3.0f;
    constexpr float MIN_DT            = 0.01f;       // DAT_100059FBC
    constexpr float DRAG_SMOOTH_OLD   = 0.2f;        // DAT_100059FC0
    constexpr float DRAG_SMOOTH_NEW   = 0.8f;        // DAT_100059FC4

    if (touchState == 1) {
        // touch state 1: drag begin. only fires when:
        //   - touch.y < titleQuad.y - titleQuad.h*0.5
        //   - pageList non-empty (= we have something to drag onto)
        //   - navDragState == -1 (no rotation/nav drag in progress)
        //   - draggedRackSlot == -1 (no rack tile actively held)
        float titleBottomY = titleQuad.posY + titleQuad.height * -0.5f;

        if (!(g->touchY() < titleBottomY) ||
            pageList.empty() ||
            navDragState != -1 || draggedRackSlot != -1) {
            return;
        }

        // drag-begin hook (FUN_100020f1c). sets dragActive and seeds the
        // velocity-tracking state (pan anchor + prev-touch + velocity).
        dragActive = true;
        panAnchorX = positionX - g->touchX();
        panAnchorY = positionY - g->touchY();
        panPrevTouchX = g->touchX();
        panPrevTouchY = g->touchY();
        panVelocityX = 0.0f;
        panVelocityY = 0.0f;

        // tap-inspect dispatch (FUN_10001bd2c body, continued).
        //
        // 1) convert touch to hex cell (FUN_100012fa4) in board-local space.
        // 2) compare against the Nemesis cell at GameBoard+0x2168.
        // 3) walk the page list looking for a tile at that cell. if found:
        //      - active snag => snag detail panel
        //      - active named content => content detail panel
        // 4) if no page tile, check HexMap.cellKindAt for hex-map detail.
        // 5) if cell == nemesis cell => Nemesis detail panel.
        //
        // anchor[2] passed to the populator is the world position of the
        // matched object (cell screen pos + GameBoard origin), so the panel
        // lerps in from there.
        float localTouchX = g->touchX() - positionX;
        float localTouchY = g->touchY() - positionY;

        // FUN_100012fa4, nearest-neighbor hex lookup over a 3x3 candidate
        // grid around the touch point.
        constexpr float ROW_Y_INV   =  5.865145f;     // DAT_100059e98 (= 1 / row stride)
        constexpr float COL_X_STEP  =  0.196875f;     // DAT_100059e9c (col stride)
        constexpr float CANDIDATE_INIT = 999999.0f;   // DAT_100059ea0
        constexpr float ROW_X_OFFS  = -0.098438f;     // DAT_100059ea4 (col-x shift per row)
        constexpr float ROW_Y_STEP  = -0.170499f;     // DAT_100059ea8 (-sqrt(3) * 0.0984375)

        int rowF = (int)(localTouchY * ROW_Y_INV);
        int colF = (int)(localTouchX / COL_X_STEP + localTouchY * ROW_Y_INV * 0.5f);

        int bestCol = colF;
        int bestRow = rowF;
        float bestDist = CANDIDATE_INIT;

        for (int colC = colF - 1; colC <= colF + 1; colC++) {
            for (int rowC = rowF - 1; rowC <= rowF + 1; rowC++) {
                float dx = localTouchX -
                           ((float)colC * COL_X_STEP + (float)rowC * ROW_X_OFFS);
                float dy = localTouchY + (float)rowC * ROW_Y_STEP;
                float d  = dx * dx + dy * dy;

                if (d < bestDist) {
                    bestDist = d;
                    bestCol  = colC;
                    bestRow  = rowC;
                }
            }
        }

        int  tapCol      = bestCol;
        int  tapRow      = bestRow;
        bool nemesisHere = nemesis.visible &&
                           (tapCol == nemesis.nemesisGridCol) &&
                           (tapRow == nemesis.nemesisGridRow);

        constexpr float DETAIL_PANEL_HEADER_Y = 0.098438f;   // DAT_100059fc8

        // set up the dismiss watcher (touchHoldArea = 2 (board), origin = touchXY).
        auto armDismissWatcher = [&]() {
            detailPanel.touchHoldArea = 2;
            detailPanel.touchOrigX = g->touchX();
            detailPanel.touchOrigY = g->touchY();
        };

        if (!nemesisHere) {
            // walk the page list looking for a tile at (tapCol, tapRow).
            for (TileObject* tile : pageList) {

                if (!tile || tile->gridCol != tapCol || tile->gridRow != tapRow) {
                    continue;
                }

                SnagContent*  sc = tile->getSnagIfAlive();
                TileContent*  tc = tile->getTileContentIfAlive();

                if (sc) {
                    // snag detail panel: run combat sim then open the panel.
                    // openSnagDetailWithCombatSim sets touchHoldArea + origin
                    // for the dismiss watcher (mode 2 = board tap).
                    float anchor[2] = {
                        tile->mainQuad.posX + positionX,
                        tile->mainQuad.posY + positionY
                    };
                    openSnagDetailWithCombatSim(/*mode=*/2, anchor, sc);
                    return;
                }

                if (!tc) {
                    return;
                }

                // gate: only named content gets a detail panel (= entry has a
                // non-empty name string). matches FUN_100014cb8 behaviour.
                if (tc->type < 0 || tc->type >= 26) {
                    return;
                }
                if (kTileTextTable[tc->type].name[0] == '\0') {
                    return;
                }

                armDismissWatcher();
                float anchor[2] = {
                    tile->mainQuad.posX + positionX,
                    tile->mainQuad.posY + positionY
                };
                detailPanel.populateForContent(DETAIL_PANEL_HEADER_Y, anchor, tc);
                return;
            }

            // no page tile at the tap cell -> check the hex map.
            int cellKind = hexMap.cellKindAt(tapCol, tapRow);

            if (cellKind != 0) {
                armDismissWatcher();
                HexCellPos cell = hexCellLinearXY(tapCol, tapRow);
                float anchor[2] = {
                    cell.x + positionX,
                    cell.y + positionY
                };
                detailPanel.populateForHexMapCell(DETAIL_PANEL_HEADER_Y, anchor,
                                                  (uint32_t)cellKind);
            }
        } else {
            // tap is on the Nemesis's hex.
            armDismissWatcher();
            HexCellPos cell = hexCellLinearXY(tapCol, tapRow);
            float anchor[2] = {
                cell.x + positionX,
                cell.y + positionY
            };
            detailPanel.populateForNemesis(DETAIL_PANEL_HEADER_Y, anchor);
        }
        return;
    }

    if (touchState == 2) {
        // touch state 2: drag continue / pointer move tracking.
        char prevDrag = panDragActive;

        if (draggedRackSlot < 0 && prevDrag == 0) {

            if (navDragState != 1) {
                return;
            }

            prevDrag = 0;
        }

        // ramp tileAlphaProgress 0 to 1 (8 units/sec) so syncGlobalTileAlpha
        // dims the rest of the board while a tile is held.
        float t = dt * 8.0f + tileAlphaProgress;

        if (t > 1.0f) {
            t = 1.0f;
        }

        tileAlphaProgress = t;

        if (prevDrag == 0) {
            return;
        }

        // velocity-tracked smoothing: positionX/Y follow touch with anchor
        // offset; +0x98C/+0x990 carry the smoothed velocity for inertial
        // release.
        positionX = g->touchX() + panAnchorX;
        positionY = g->touchY() + panAnchorY;

        // pixel snap
        constexpr float SNAP_SCALE = 640.0f;
        auto snap = [&](float v) {
            float t2 = v * SNAP_SCALE;
            return ((t2 >= 0.0f) ? (float)(int)(t2 + 0.5f)
                                 : (float)(int)(t2 - 0.5f)) / SNAP_SCALE;
        };
        positionX = snap(positionX);
        positionY = snap(positionY);

        // velocity smoothing: vx = vx*0.2 + (deltaX/dt)*0.8.
        float dtClamped = (dt < MIN_DT) ? MIN_DT : dt;
        float prevX = panPrevTouchX;
        float prevY = panPrevTouchY;
        float dxRate = (g->touchX() - prevX) / dtClamped;
        float dyRate = (g->touchY() - prevY) / dtClamped;

        float& vx = panVelocityX;
        float& vy = panVelocityY;
        vx = vx * DRAG_SMOOTH_OLD + dxRate * DRAG_SMOOTH_NEW;
        vy = vy * DRAG_SMOOTH_OLD + dyRate * DRAG_SMOOTH_NEW;

        panPrevTouchX = g->touchX();
        panPrevTouchY = g->touchY();

        // cap velocity magnitude at 3.0 (= roughly 4 hex-cells per second).
        float speedSq = vx * vx + vy * vy;
        float speed   = std::sqrt(speedSq);

        if (speed >= DRAG_SPEED_CAP) {
            vx = (vx / speed) * DRAG_SPEED_CAP;
            vy = (vy / speed) * DRAG_SPEED_CAP;
        }

        return;
    }

    // touch state 0: released. decay tileAlphaProgress toward 0 +
    // clear drag flag. syncGlobalTileAlpha picks up the change and lifts
    // the dim off non-held tiles.
    if (tileAlphaProgress > 0.0f) {
        float t = tileAlphaProgress + dt * -8.0f;

        if (t < 0.0f) {
            t = 0.0f;
        }

        tileAlphaProgress = t;
    }

    if (dragActive) {
        // clears panDragActive and sets panInertiaActive in one store:
        // drag ends, the post-release inertial scroll begins.
        dragActive = false;
        panInertiaActive = true;
    }
}

// FUN_10001c450, updateNavArrowAndConfirmDrag.
//
// drives the confirm-button + rotate-button gesture state machine while a
// rack tile is staged at a hex (selectedRackSlot != -1). navDragState
// semantics:
//   -1: idle, waiting for a touch on either the confirm button (=
//       navArrowQuad at +0x6F0, right of placed tile) or the rotate button
//       (= backButtonQuad at +0x7C8, left of placed tile).
//    0: confirm button held; on release over the same button, the tile
//       commits to the page list.
//    1: rotate button held; continuous rotation of the held tile follows
//       the angle delta from the touch's anchor point. on release: a "big
//       drag" (duration >= 0.5s or angle delta >= 15 deg) snaps to the
//       closest fitting rotation; a "small drag" (= a tap) advances to the
//       next-allowed rotation in dirList (= each tap cycles the orientation).
//
// returns 1 when the gesture commits the tile to the page list, 0 otherwise.
//
// at idle (navDragState = -1), every branch early-outs without effect.
int GameBoard::updateNavArrowAndConfirmDrag(float dt) {
    constexpr float DRAG_DURATION_THRESHOLD = 0.5f;     // sec
    constexpr float DRAG_ANGLE_THRESHOLD    = 15.0f;    // degrees
    constexpr float DEG_PER_STEP            = 60.0f;    // DAT_100059fcc
    constexpr float HINT_RADIUS             = 0.181250f; // DAT_100059fd0
    constexpr float PI_VAL                  = 3.1415927f; // DAT_100059fd4
    constexpr float RAD_TO_DEG              = 57.295776f; // DAT_100059fd8

    Game* g = getGame();

    if (!g) {
        return 0;
    }

    int touchState = g->inputState();

    // ---- touch down (inputState == 1): capture which button was hit ----
    if (touchState == 1) {
        // hit-test in board-local coords (touch - boardPos).
        float relX = g->touchX() - positionX;
        float relY = g->touchY() - positionY;
        bool overConfirm = navArrowQuad.quad.contains(relX, relY);
        bool overRotate  = backButtonQuad.quad.contains(relX, relY);

        if (overConfirm) {
            navDragState = 0;
            navArrowQuad.quad.setColor(0xB4, 0xB4, 0xB4, 0xFF);  // dim
        } else if (overRotate) {
            // rotate gesture is suppressed by rack snag 0x31 or 0x43.
            if (findSnagInRack(0x31) != nullptr) return 0;
            if (findSnagInRack(0x43) != nullptr) return 0;

            navDragState = 1;
            backButtonQuad.quad.setColor(0xB4, 0xB4, 0xB4, 0xFF);

            // anchor seeds for the rotation drag. only fires when
            // selectedRackSlot is valid (which the caller guarantees: the
            // rotate button only draws when selectedRackSlot != -1).
            TileObject* held = (selectedRackSlot >= 0 &&
                                selectedRackSlot < RACK_SLOT_COUNT)
                               ? rack[selectedRackSlot]
                               : nullptr;

            if (held) {
                // FUN_10001282c(held, 1): kick slide-anim if not already on.
                if (!held->slideAnimActive) {
                    held->slideAnimActive = true;
                    held->iconFadeT       = 0.0f;
                }

                // anchor rotation = current rotation in degrees [0, 360).
                navDragAnchorRotation = tileCurrentRotationDegrees(held);

                // anchor angle = atan2 from the tile's screen center to
                // the touch position. binary uses raw screen coords minus
                // tile.posX/posY minus boardX/boardY.
                float dx = g->touchX() - (held->mainQuad.posX + positionX);
                float dy = g->touchY() - (held->mainQuad.posY + positionY);
                navDragAnchorAngle = std::atan2(dy, dx);
            }

            navDragDuration = 0.0f;
        } else {
            return 0;
        }

        g->soundQueue.trigger(5);  // buttonDown
        return 0;
    }

    // ---- touch dragging (inputState == 2): rotate held tile under finger ----
    if (touchState == 2) {

        if (navDragState != 1) {
            return 0;
        }

        TileObject* held = (selectedRackSlot >= 0 &&
                            selectedRackSlot < RACK_SLOT_COUNT)
                           ? rack[selectedRackSlot]
                           : nullptr;

        if (!held) {
            return 0;
        }

        // current angle from tile center to touch.
        float dx = g->touchX() - (held->mainQuad.posX + positionX);
        float dy = g->touchY() - (held->mainQuad.posY + positionY);
        float currentAngle = std::atan2(dy, dx);
        float deltaAngle   = currentAngle - navDragAnchorAngle;

        // apply rotation directly: tile.rotation = anchor + delta(rad to deg)
        held->setRotationDirect(navDragAnchorRotation + deltaAngle * RAD_TO_DEG);

        // hint quad position (+0x870 = backButtonQuad.posX/posY). offset
        // from the tile center by polarToRect(0.18125, deltaAngle + pi) so
        // the rotate button visually orbits the tile as the player drags.
        HexCellPos hintOffset = polarToRect(HINT_RADIUS, deltaAngle + PI_VAL);
        backButtonQuad.quad.posX = held->mainQuad.posX + hintOffset.x;
        backButtonQuad.quad.posY = held->mainQuad.posY + hintOffset.y;

        // stash the angle delta (degrees) in backButtonQuad.quad.rotation.
        // the binary's +0x888 is exactly this field (backButtonQuad at
        // +0x7C8, Quad.rotation at +0xC0, so 0x7C8 + 0xC0 = 0x888). reusing
        // the rotation slot also drives the button's visible spin around
        // the tile during the drag.
        backButtonQuad.quad.rotation = deltaAngle * RAD_TO_DEG;

        navDragDuration += dt;
        return 0;
    }

    // ---- touch released (inputState == 0 or 3) ----

    // rotate button release: pick between "snap rotation to closest fit" and
    // "advance to next-allowed dir in dirList".
    if (navDragState == 1) {
        backButtonQuad.quad.setColor(0xFF, 0xFF, 0xFF, 0xFF);  // restore white
        navDragState = -1;

        TileObject* held = (selectedRackSlot >= 0 &&
                            selectedRackSlot < RACK_SLOT_COUNT)
                           ? rack[selectedRackSlot]
                           : nullptr;

        if (held) {
            float angleDelta = backButtonQuad.quad.rotation;

            bool bigDrag = (navDragDuration >= DRAG_DURATION_THRESHOLD) ||
                           (angularDistance(angleDelta, 0.0f) >= DRAG_ANGLE_THRESHOLD);

            if (bigDrag) {
                finalizeTileRotation(held, true, false);
            } else {
                std::vector<int> dirList;
                buildDirList(this, held, dirList);

                if (dirList.empty()) {
                    finalizeTileRotation(held, true, false);
                } else {
                    // walk dirList for the current rotation step; advance to
                    // (idx + 1) % size as the next step.
                    size_t idx = 0;

                    for (; idx < dirList.size(); idx++) {

                        if (dirList[idx] == held->rotationStep) {
                            break;
                        }
                    }

                    size_t nextIdx = (idx + 1) % dirList.size();
                    int    nextRot = dirList[nextIdx];
                    tileSetRotationLerp(held, (float)nextRot * DEG_PER_STEP);
                    tileSetRotationStep(held, nextRot, false);
                }
            }

            // reset the rotate-button hint quad to its resting position
            // (just left of the tile, no Y offset). only the X axis gets
            // the polar offset; Y stays equal to the tile's posY.
            HexCellPos restOffset = polarToRect(HINT_RADIUS, PI_VAL);
            backButtonQuad.quad.posX = held->mainQuad.posX + restOffset.x;
            backButtonQuad.quad.posY = held->mainQuad.posY;

            backButtonQuad.quad.rotation = 0.0f;
            backButtonQuad.quad.snapToPixelGrid();
        }

        g->soundQueue.trigger(7);  // buttonCancel
        return 0;
    }

    if (navDragState != 0) {
        return 0;
    }

    // ---- confirm button release: commit the tile to the page list ----
    navArrowQuad.quad.setColor(0xFF, 0xFF, 0xFF, 0xFF);
    navDragState = -1;

    // hit-test: did the release land on the confirm button? in board-local
    // coords (= same frame as the touch-down test).
    {
        float relX = g->touchX() - positionX;
        float relY = g->touchY() - positionY;

        if (!navArrowQuad.quad.contains(relX, relY)) {
            g->soundQueue.trigger(7);  // buttonCancel, released off the button
            return 0;
        }
    }

    TileObject* held = (selectedRackSlot >= 0 &&
                        selectedRackSlot < RACK_SLOT_COUNT)
                       ? rack[selectedRackSlot]
                       : nullptr;

    if (!held) {
        g->soundQueue.trigger(7);
        return 0;
    }

    // snag-0x74 path: rather than the rules-engine pick, the binary RNGs a
    // direction from dirList. on a different rotation than current, pushes
    // an action-queue entry with the snag's bonus payload + plays sound 0x21.
    SnagContent* snag74 = findSnagInRack(0x74);

    if (snag74) {
        std::vector<int> dirList;
        buildDirList(this, held, dirList);

        if (!dirList.empty()) {
            int randIdx = rngInt(0, (int)dirList.size() - 1, 4);
            int chosen  = dirList[randIdx];

            if (chosen != held->rotationStep) {
                tileSetRotationLerp(held, (float)chosen * DEG_PER_STEP);
                tileSetRotationStep(held, chosen, false);

                // action queue push (type 0) with origin (0, 0) and
                // tileRef = snag's parentTile (= snag74->tileParent). the
                // queued entry carries the snag-0x74 bonus payload.
                float origin[2] = { 0.0f, 0.0f };
                pushAction(0, origin,
                           snag74->tileParent);

                g->soundQueue.trigger(0x21);
            }
        }
    } else {
        // standard path: rules engine picks the best fit. confirm button
        // uses useDirHint=false (only the rotate button's "small drag,
        // cycle" path benefits from the dir hint).
        finalizeTileRotation(held, true, false);
    }

    // ---- push tile to page list ----
    // mark the tile as committed onto the page list. read by Section 2 of
    // dispatchHexMapPostCommit to detect "snag's parent is on the board".
    held->committed = true;

    pageList.push_back(held);

    rack[selectedRackSlot] = nullptr;
    selectedRackSlot       = -1;

    // achievement place-tile fan-out (= binary's FUN_10004dc7c).
    {
        AchievementTracker& tracker = getGame()->achievementTracker();
        const int contentType = held->getContentType();
        const int contentMag  = held->getContentMagnitude();

        // "Double Edged": place an HP tile (content 6) over a trap (kind 2
        // decoration) of equal value.
        if (contentType == 6
            && held->hasActiveDecorationOfKind(2)
            && contentMag == held->decorationValueOfKind(2)) {
            tracker.increment(AchievementId::DoubleEdged);
        }

        // "Lucid": place a value-2 control tile (content 5).
        if (contentType == 5 && contentMag == 2) {
            tracker.increment(AchievementId::Lucid);
        }

        // "A Beautiful Garden": the placed tile completes some other tile's
        // 6-neighbor surround. gated on isLocked (skip the walk once
        // unlocked) and pageList.size() > 6 (need 7+ tiles before anyone
        // can be fully surrounded). matches the binary's nested FUN_10004de70
        // walk by using countAdjacentPageTiles on each hex-adjacent placed
        // tile.
        if (tracker.isLocked(static_cast<uint32_t>(AchievementId::ABeautifulGarden))
            && pageList.size() > 6) {

            for (TileObject* adj : pageList) {

                if (!adj) {
                    continue;
                }

                if (hexGridDistance(held->gridCol, held->gridRow,
                                    adj->gridCol, adj->gridRow) != 1) {
                    continue;
                }

                if (countAdjacentPageTiles(adj) == 6) {
                    tracker.increment(AchievementId::ABeautifulGarden);
                }
            }
        }

        // "Focus on the Pain": place a high-value pain tile (content 0x11,
        // value > 29) and survive (value < currentHealth).
        if (contentType == 0x11
            && contentMag > 0x1D
            && contentMag < playerSystem.currentHealth) {
            tracker.increment(AchievementId::FocusOnThePain);
        }

        // "Spice of Life": place a "special" tile (= not ATK/DEF/HP/control,
        // no snag). gate on isLocked to avoid the per-place check once
        // unlocked.
        if (tracker.isLocked(static_cast<uint32_t>(AchievementId::SpiceOfLife))
            && held->getSnagIfAlive() == nullptr
            && contentType != 2
            && contentType != 3
            && contentType != 6
            && contentType != 5) {
            tracker.increment(AchievementId::SpiceOfLife);
        }
    }

    // tile post-commit hook: flip the suppressed flag on any kind=1
    // (Darkness "?") decoration so the underlying content fades back in.
    held->suppressDecorationsOfKind(1);

    // clear the cursor visibility flags (the directional cursors are no
    // longer needed once the tile commits). matches the binary's
    // `strh wzr,[x19, #0x6e4]; str wzr,[x19, #0x6e0]` pair = clear all 6
    // visibility bytes at +0x6E0..+0x6E5.
    for (int i = 0; i < MAX_CURSOR_COUNT; i++) {
        tileCursorVisible[i] = false;
    }

    // ---- 25-case content-type switch ----
    // dispatches on the committed tile's content type, applying stat
    // changes (setATK / setDEF / setHP), HUD CTRL / XP slot advances,
    // action-queue pushes, and per-type audio events. saved hp / maxHp
    // are read at switch entry; case 6 / 0xc test them later to detect
    // "tile applied no real heal because HP was already maxed".
    int hpAtEntry    = playerSystem.currentHealth;
    int maxHpAtEntry = playerSystem.maxHealth;
    int contentType  = held->getContentType();

    // -1 = no sound; >= 0 = play this code after the switch. cases that
    // "default fallthrough" leave it -1 (binary: goto switchD_caseD_1).
    int audioCode = -1;

    switch (contentType) {
    case 0: {
        // empty / blank tile: only fires "no effect" sound when there's
        // also no live snag on the tile (otherwise something else picks
        // up the tile's effect via Chunk D's dispatch).
        if (!held->getSnagIfAlive()) {
            audioCode = 0x19;  // "blank"
        }
        break;
    }
    case 2: {
        // ATK boost: setATK(currentATK + magnitude). sound 0x15 = "atkUp".
        int mag = held->getContentMagnitude();
        setATK(playerSystem.attack + mag);
        audioCode = 0x15;
        break;
    }
    case 3: {
        // DEF tile: applies magnitude to DEF, AND looks up the player's
        // base-Item SpecialAbility 3 ("+%d {A} per placed {D}") to
        // additionally bump ATK.
        int mag = held->getContentMagnitude();
        setDEF(playerSystem.defence + mag);
        int atkBonus = playerSystem.baseItemSpecialAbilityValue(3);
        setATK(playerSystem.attack + atkBonus);
        audioCode = 0x16;  // "defUp"
        break;
    }
    case 5: {
        // CTRL tile: advances the control marker bank by `mag` slots,
        // and applies the player's base-Item SpecialAbility 0xb
        // ("+%d {H} per placed {C}") as an HP heal. sound 0x19 fires
        // only when the tile's magnitude rolled to 0 (= flat tile, no
        // CTRL gain). otherwise silent; the marker bank's own chime
        // covers the audio cue.
        int mag = held->getContentMagnitude();
        hud.advanceCTRLSlot(mag, false);
        int hpBonus = playerSystem.baseItemSpecialAbilityValue(0xb);
        setHP((uint32_t)(playerSystem.currentHealth + hpBonus));

        if (mag == 0) {
            audioCode = 0x19;
        }
        break;
    }
    case 6:
    case 0xc: {
        // HP tile: heals by magnitude. when HP was already at max at
        // switch entry, setHP no-ops and we play the "no effect" cue.
        int mag = held->getContentMagnitude();
        setHP((uint32_t)(playerSystem.currentHealth + mag));

        if (hpAtEntry == maxHpAtEntry) {
            audioCode = 0x19;
        }
        break;
    }
    case 0xa: {     // = Potential
        // potential tile: counts adjacent page tiles (binary subtracts 1
        // unconditionally and clamps to 0, excluding self), writes that
        // count into the tile's content
        // magnitude (so the tile's number sprite shows it), and advances
        // the XP slot bank by the same. sound 0x4D / 0x19 by count > 0.
        int adjacent = countAdjacentPageTiles(held) - 1;

        if (adjacent < 1) {
            adjacent = 0;
        }

        TileContent* tc = held->getTileContentIfAlive();

        if (tc) {
            // = binary FUN_100014870; also fires the Serendipity check
            // when an XP tile (type 4) grows past magnitude 3.
            tc->setRawAndDisplayMagnitude(adjacent);
        }
        hud.advanceXPSlot(adjacent, false);

        audioCode = (adjacent == 0) ? 0x19 : 0x4D;
        break;
    }
    case 0xb: {
        // DEF boost variant: magnitude only, no SpecialAbility lookup.
        int mag = held->getContentMagnitude();
        setDEF(playerSystem.defence + mag);
        audioCode = 0x16;
        break;
    }
    case 0xd:       // = Lure
        audioCode = 0x47;
        break;
    case 0xe: {     // = Pause
        // walks the rack (slots 0..4) counting tiles whose contentType
        // is also 0xe; adds that count to BOTH ATK and DEF. also clears
        // snagActivationSuppressed at +0x1D.
        snagActivationSuppressed = false;
        int count = 0;

        for (int i = 0; i < RACK_SLOT_COUNT; i++) {

            if (rack[i] && rack[i]->getContentType() == 0xe) {
                count++;
            }
        }
        setDEF(playerSystem.defence + count);
        setATK(playerSystem.attack + count);
        audioCode = 0x4C;
        break;
    }
    case 0xf:   // = Barricade
        audioCode = 0x43;
        break;
    case 0x10: {    // = Wealth
        // negative ATK from same-type rack count: ATK = max(0, currATK - count*3).
        // also adds 1 lit slot to the control bank (the binary's
        // FUN_10000d034(hud, 1, 0)).
        hud.advanceCTRLSlot(1, false);
        int count = 0;

        for (int i = 0; i < RACK_SLOT_COUNT; i++) {

            if (rack[i] && rack[i]->getContentType() == 0x10) {
                count++;
            }
        }
        int newATK = playerSystem.attack - count * 3;

        if (newATK < 1) {
            newATK = 0;
        }
        setATK(newATK);
        audioCode = 0x50;
        break;
    }
    case 0x11: {    // = Pain
        // HP drain by magnitude (clamped to currentHP), then push the
        // tile reference into the action queue with origin = GameBoard's
        // (positionX, positionY) so the burst tracks the held tile.
        int mag = held->getContentMagnitude();

        if (mag > 0) {
            int hpToDrain = (mag <= hpAtEntry) ? mag : hpAtEntry;
            setHP((uint32_t)(hpAtEntry - hpToDrain));
        }

        float origin[2] = { positionX, positionY };
        pushAction(0, origin, held);
        audioCode = 0x4B;
        break;
    }
    case 0x12: {    // = Secret
        // negative ATK + DEF by magnitude: both clamp to 0 floor.
        int mag = held->getContentMagnitude();
        int newATK = playerSystem.attack - mag;

        if (newATK < 1) {
            newATK = 0;
        }
        setATK(newATK);
        int newDEF = playerSystem.defence - mag;

        if (newDEF < 1) {
            newDEF = 0;
        }
        setDEF(newDEF);
        audioCode = 0x4E;
        break;
    }
    case 0x13: {    // = Bad Luck
        // clamp nemesis.eatTarget up to nemesis.nemesisLevel and start the
        // eat cycle.
        if (nemesis.eatTarget < nemesis.nemesisLevel) {
            nemesis.eatTarget = nemesis.nemesisLevel;
        }

        nemesis.eatActive = true;
        audioCode = 0x42;
        break;
    }
    case 0x14: {    // = Faith
        // walks the rack counting tiles of contentType 0x14, adds
        // count * 2 to ATK. sound 0x15 when any matched, 0x19 otherwise.
        int count = 0;

        for (int i = 0; i < RACK_SLOT_COUNT; i++) {

            if (rack[i] && rack[i]->getContentType() == 0x14) {
                count++;
            }
        }
        setATK(playerSystem.attack + count * 2);
        audioCode = (count < 1) ? 0x19 : 0x15;
        break;
    }
    case 0x15: {    // = Foresight
        // sets combatEffectsSuppressed at +0x1E.
        combatEffectsSuppressed = true;
        audioCode = 0x46;
        break;
    }
    case 0x16: {    // = Memento
        // boosts ATK + DEF + HP all by magnitude (in that order: order
        // matters because each setter re-reads playerSystem state).
        int mag = held->getContentMagnitude();
        setATK(playerSystem.attack + mag);
        setDEF(playerSystem.defence + mag);
        setHP((uint32_t)(playerSystem.currentHealth + mag));
        audioCode = 0x48;
        break;
    }
    case 0x17:      // = Warmth
        audioCode = 0x4F;
        break;
    case 0x18: {    // = Effort
        // boosts ATK + DEF by magnitude.
        int mag = held->getContentMagnitude();
        setATK(playerSystem.attack + mag);
        setDEF(playerSystem.defence + mag);
        audioCode = 0x45;
        break;
    }
    case 0x19:      // = Milestone
        audioCode = 0x49;
        break;
    default:
        // cases 1, 4, 7, 8, 9 (and any other unhandled type): silent
        // dispatch, no setter, no audio. Chunk D still runs.
        break;
    }

    if (audioCode >= 0) {
        g->soundQueue.trigger(audioCode);
    }

    // ---- post-switch dispatch ----
    // HexMap-kind side effects + per-snag-type post-commit (combat).
    dispatchHexMapPostCommit(held);

    SnagContent* heldSnag = held->getSnagIfAlive();

    if (heldSnag) {
        dispatchSnagPostCommit(heldSnag);
    }

    // ---- second content-type switch + bitmap-erase ----
    // dispatches the visible burst + applyTileTypeEffect + decides
    // whether the just-committed tile should be erased now or kept on
    // the board. when contentType == 0 the entire block is skipped
    // (no erase, no burst, no event charge).
    if (contentType != 0) {
        int  mag             = held->getContentMagnitude();
        bool eraseTile       = true;
        bool useBitmapCheck  = (mag == 0);

        if (!useBitmapCheck) {

            switch (contentType) {
            case 2:
                applyTileTypeEffect(2);
                statBars.spawnIconBurst(&held->mainQuad.posX, contentType);
                break;
            case 3:
                applyTileTypeEffect(3);
                statBars.spawnIconBurst(&held->mainQuad.posX, contentType);
                break;
            case 5:
                applyTileTypeEffect(1);  // CTRL tile, Health Event slot
                statBars.spawnIconBurst(&held->mainQuad.posX, contentType);
                break;
            case 6:

                if (hpAtEntry != maxHpAtEntry) {
                    applyTileTypeEffect(4);
                    statBars.spawnIconBurst(&held->mainQuad.posX, contentType);
                    useBitmapCheck = true;
                }
                // hp maxed: fallthrough to default erase
                break;
            case 0xa:
            case 0xb:
            case 0x16:
            case 0x18:
                statBars.spawnIconBurst(&held->mainQuad.posX, contentType);
                useBitmapCheck = true;
                break;
            case 0xc:

                if (hpAtEntry != maxHpAtEntry) {
                    statBars.spawnIconBurst(&held->mainQuad.posX, contentType);
                    useBitmapCheck = true;
                }
                // hp maxed: fallthrough to default erase
                break;
            case 0xd:
            case 0xf:
            case 0x17:
            case 0x19:
                eraseTile = false;
                break;
            default:
                // 1, 4, 7, 8, 9, 0xe, 0x10..0x15: no second-switch
                // body, fallthrough to default erase.
                break;
            }
        }

        if (useBitmapCheck) {
            // bitmap 0x280a000 covers contentType in {0xd, 0xf, 0x17, 0x19}.
            // those tile types are preserved (not erased) when reached
            // via the bitmap-check fast path.

            if (contentType < 0x1A &&
                ((1u << (uint32_t)contentType) & 0x280a000u) != 0) {
                eraseTile = false;
            }
        }

        if (eraseTile) {
            held->erase();
        }
    }

    // ---- PlayerSystem step toward placed cell + scroll bounds ----
    // always runs, even for contentType == 0 (the binary jumps here from
    // the contentType == 0 early-out).
    playerSystem.stepToward(&held->mainQuad.posX, &held->gridCol);
    recomputeScrollBounds(0.0f);

    return 1;
}

// FUN_10001c1a4, nemesisSpawnAtTrailTail.
//
// when Nemesis is dormant and the page list is non-empty: walk the oldest
// page tile's 6 exit directions, drop any direction that would step back
// onto the second-oldest tile (= toward the player's path), pick a random
// remaining direction via RNG stream 0, compute the spawn (col, row), and
// place Nemesis there facing toward the trail. then optionally schedule
// the first eat step (if nemesis.eatTarget > 0).
//
// when Nemesis is already alive: skips the spawn placement and just
// schedules the next eat step (FUN_1000209dc = nemesisScheduleNextStep).
//
// called from:
//   - tickInGameStateMachine state 7 (entered when an interlock-staged
//     case 1 / 5 / 6 falls through with nemesis.eatTarget > 0).
//   - GameBoard::nemesisAdvance (FUN_10001dbe4) when Nemesis is dormant.
//
// no-op on an empty page list.
void GameBoard::nemesisSpawnAtTrailTail() {

    if (nemesis.visible) {
        // already alive: skip placement, just schedule the next eat step.
        // covers the "snag activation woke us mid-cycle" path.
        nemesisScheduleNextStep();
        return;
    }

    if (pageList.empty()) {
        return;  // no trail to spawn at
    }

    // oldest page tile (= pageList.front()). second-oldest used to filter
    // out directions that would step back onto the trail.
    TileObject* oldest       = pageList.front();
    TileObject* secondOldest = nullptr;

    if (pageList.size() > 1) {
        auto it = pageList.begin();
        ++it;
        secondOldest = *it;
    }

    // hex direction offsets indexed 0..5. matches the binary's switch in
    // FUN_10001c1a4 (with the case-2/case-5 fallthrough flattened):
    //   0=(0,-1) 1=(+1,0) 2=(+1,+1) 3=(0,+1) 4=(-1,0) 5=(-1,-1)
    constexpr int DIR_DC[6] = { 0,  1,  1, 0, -1, -1 };
    constexpr int DIR_DR[6] = { -1, 0,  1, 1,  0, -1 };

    // walk directions, collect those that (a) have an exit on oldest and
    // (b) don't step back onto secondOldest.
    std::vector<int> validDirs;

    for (int dir = 0; dir < 6; dir++) {

        if (!oldest->permitsDirection(dir)) {
            continue;
        }

        if (secondOldest != nullptr) {
            int targetCol = oldest->gridCol + DIR_DC[dir];
            int targetRow = oldest->gridRow + DIR_DR[dir];

            if (targetCol == secondOldest->gridCol &&
                targetRow == secondOldest->gridRow) {
                continue;
            }
        }

        validDirs.push_back(dir);
    }

    if (validDirs.empty()) {
        return;  // dead end - can't spawn anywhere
    }

    // RNG-pick one direction from the collected set via cosmetic stream 0.
    int pickIdx = rngInt(0, (int)validDirs.size() - 1, 0);
    int pickedDir = validDirs[pickIdx];

    int spawnCol = oldest->gridCol + DIR_DC[pickedDir];
    int spawnRow = oldest->gridRow + DIR_DR[pickedDir];

    // compute facing direction (which neighbor index of `oldest` is `spawn`
    // sitting at?). matches the binary's branchy comparison ladder.
    int facing;

    if (spawnCol < oldest->gridCol) {
        facing = (spawnRow < oldest->gridRow) ? 2 : 1;
    } else if (spawnCol == oldest->gridCol) {
        facing = (spawnRow <= oldest->gridRow) ? 3 : 0;
    } else {
        facing = (spawnRow > oldest->gridRow) ? 5 : 4;
    }

    int64_t hexConfig = ((int64_t)(uint32_t)spawnRow << 32) |
                         (int64_t)(uint32_t)spawnCol;
    nemesis.placeOnHexGrid(hexConfig, (float)facing);

    setNemesisPanTarget(spawnCol, spawnRow);

    if (nemesis.eatTarget > 0) {
        nemesisScheduleNextStep();
    }

    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x27);   // "nemesisSpawn"
    }
}

// FUN_10001dca0, setNemesisPanTarget. snapshot positionX/Y into pan-source,
// set pan-target to the cell's screen position, kick the cosine pan.
void GameBoard::setNemesisPanTarget(int col, int row) {
    constexpr float SNAP_SCALE = 640.0f;

    auto pixelSnap = [&](float v) {
        return (v >= 0.0f)
               ? (float)(int)(v * SNAP_SCALE + 0.5f) / SNAP_SCALE
               : (float)(int)(v * SNAP_SCALE - 0.5f) / SNAP_SCALE;
    };

    HexCellPos cell = hexCellLinearXY(col, row);
    float virtualHeight = Renderer::getVirtualHeight();

    panStartX   = positionX;
    panStartY   = positionY;
    panTargetX  = pixelSnap(0.5f - cell.x);
    panTargetY  = pixelSnap(virtualHeight * 0.5f - cell.y);
    panProgress = 0.0f;
}

// FUN_1000209dc, nemesisScheduleNextStep.
//
// compute eatStep (per-batch countdown) and eatStepDuration (log-scaled
// animation time), then immediately fire nemesisStepForward for the first
// step. logic:
//   eatStep = min(eatTarget, pageCount) + 1
// when eatTarget <= 0 (degenerate, shouldn't happen in normal play):
// walk the page list to the first XP tile and use that index as the
// fallback, otherwise nemesisLevel.
void GameBoard::nemesisScheduleNextStep() {
    int target = nemesis.eatTarget;
    int cap;

    if (target < 1) {
        // walk page list to the first content-4 (XP) tile.
        int walked = 0;

        if (!pageList.empty()) {
            int idx = 1;

            for (TileObject* t : pageList) {
                walked = idx;

                if (t && t->getContentType() == 4) {
                    break;
                }

                idx = walked + 1;
            }
        }

        cap = static_cast<int>(pageList.size());

        if (walked >= cap) {
            walked = nemesis.nemesisLevel;
        }

        target = walked;
    } else {
        cap = static_cast<int>(pageList.size());
    }

    if (target > cap) {
        target = cap;
    }

    nemesis.eatStep = target + 1;

    // log-scaled step duration: longer per-step animation when the cycle
    // is short, so the few eats are visually pronounced; shorter when
    // many. matches the binary's `log(eatStep - 1) + 1.0`.
    eatStepDuration = std::log((float)(target + 1) - 1.0f) + 1.0f;

    // binary passes 1.0f as the per-step posY constant (the visual Y
    // anchor for the pan target). matches FUN_1000209dc's call to
    // FUN_100020aac(..., 1.0, ...).
    nemesisStepForward();
}

// FUN_100020aac, nemesisStepForward.
//
// execute one eat step: decrement eatStep, place Nemesis on the next-oldest
// page tile facing the trail, play the eat-content/snag/XP sound. when
// eatStep was already <= 1, this decrements and returns; the per-cycle
// countdown is exhausted and the state-machine routes to state-8 (sated
// revive) on the next state-7 tick.
//
// special case: when oldest page tile has content type 0xF and the level
// hasn't reached the exit yet, the tile's content is killed-and-faded
// instead of Nemesis eating, and the eat cycle terminates (eatStep = 0).
void GameBoard::nemesisStepForward() {
    nemesis.eatFired = false;

    if (pageList.empty()) {
        nemesis.eatStep = 0;
        return;
    }

    int before = nemesis.eatStep;
    nemesis.eatStep = before - 1;

    if (before < 2) {
        return;  // countdown done - next state-7 will route to state-8
    }

    TileObject* oldest       = pageList.front();
    TileObject* secondOldest = nullptr;

    if (pageList.size() >= 2) {
        auto it = pageList.begin();
        ++it;
        secondOldest = *it;
    }

    // content type 0xF (Barricade) on oldest + !exitReached -> the tile's
    // content gets killAndFade'd instead of Nemesis eating, and the eat
    // cycle terminates (eatStep = 0). matches FUN_100020aac's special
    // branch: getTileContentIfAlive + movableActorKillAndFade + fall
    // through to the trailing `*piVar1 = 0;`.
    if (!exitReached && oldest->getContentType() == 0xF) {
        TileContent* tc = oldest->getTileContentIfAlive();

        if (tc) {
            tc->killAndFade();
        }

        nemesis.eatStep = 0;
        return;
    }

    // compute facing from oldest -> secondOldest (= which way is Nemesis
    // looking as it stands on `oldest` glancing at the trail).
    int facing;

    if (secondOldest == nullptr) {
        facing = 0;
    } else if (oldest->gridCol < secondOldest->gridCol) {
        facing = (oldest->gridRow < secondOldest->gridRow) ? 2 : 1;
    } else if (oldest->gridCol == secondOldest->gridCol) {
        facing = (oldest->gridRow <= secondOldest->gridRow) ? 3 : 0;
    } else {
        facing = (oldest->gridRow > secondOldest->gridRow) ? 5 : 4;
    }

    nemesis.beginMoveTo(eatStepDuration, oldest->gridCol, oldest->gridRow, facing);
    setNemesisPanTarget(oldest->gridCol, oldest->gridRow);

    // audio cue: 0x28 / 0x29 / 0x2A based on what's on the tile.
    Game* g = getGame();

    if (g) {
        SnagContent* sc = oldest->getSnagIfAlive();
        int sound;

        if (sc != nullptr) {
            sound = 0x29;   // "eatSnag"
        } else if (oldest->getContentType() == 4) {
            // XP tile: 0x29 if player is downed (= dramatic), 0x2A otherwise.
            sound = playerDowned ? 0x29 : 0x2A;
        } else {
            sound = 0x28;   // "eatContent"
        }

        g->soundQueue.trigger(sound);
    }
}

// FUN_10001d51c, applyHexPickupConsumeEffect.
//
// player has just released a finger over a page-tile that contains an
// effect. matches the binary's flow:
//   1. hit-test rack[i] against pointer position.
//   2. find the cleanup-quad entry that matches that slot's index.
//   3. dispatch by tile content type (12 distinct cases for kinds
//      0xD..0x44).
//   4. trigger sound 0x2D + run the cleanup-vector-shrink.
//
// no-op on an empty rack with no cleanup entries.
void GameBoard::applyHexPickupConsumeEffect(float /*dt*/) {
    Game* g = getGame();

    if (!g || g->inputState() != 1) {
        return;
    }

    float titleBottomY = titleQuad.posY + titleQuad.height * -0.5f;

    if (g->touchY() <= titleBottomY) {
        return;
    }

    // pass 1: walk rack, find the slot the pointer hit.
    int slotIdx = -1;

    for (int i = 0; i < RACK_SLOT_COUNT; i++) {

        if (!rack[i]) {
            continue;
        }

        // bbox test: pointer over rack[i]'s mainQuad?
        // FUN_1000083bc port = Quad::contains.
        bool over = rack[i]->mainQuad.contains(g->touchX(), g->touchY());

        if (over) {
            slotIdx = i;
            break;
        }
    }

    if (slotIdx < 0) {
        return;
    }

    // pass 2: walk staged discard entries, find one matching the slot.
    if (hud.pendingDiscards.empty()) {
        Game* gnd = getGame();

        if (gnd) {
            gnd->soundQueue.trigger(0x12);
        }

        return;
    }

    DiscardEntry* matched = nullptr;

    for (DiscardEntry& e : hud.pendingDiscards) {

        if (e.rackSlot == slotIdx) {
            matched = &e;
            break;
        }
    }

    if (!matched) {
        Game* gnd = getGame();

        if (gnd) {
            gnd->soundQueue.trigger(0x12);
        }

        return;
    }

    // pass 3: with no Event selected, the tap toggles the matched entry's
    // staged flag (= "select this tile for discard" / "deselect").
    if (hud.selectedEvent == nullptr) {
        togglePendingDiscardStage(*matched);
        refreshDiscardConfirmIcon();
        return;
    }

    // an Event is selected: dispatch by its kind. each kind decides whether
    // to commit (and thus advance to the cleanup pass) or early-out without
    // firing it; unhandled kinds early-out unconditionally.
    int kind = *hud.selectedEvent;
    TileObject* tappedTile = rack[slotIdx];

    // 0x23 / 0x2f are "toggle-stage" kinds (ABriefPause / StrongMedicine):
    // tapping a tile in the panel toggles its staged byte rather than
    // committing the effect right away. their commit happens on tap of
    // the conditional confirm icon, or by re-tap on the Event button
    // (handled by fireEvent's prologue). early-out here without cleanup.
    if (kind == 0x23 || kind == 0x2f) {
        togglePendingDiscardStage(*matched);
        return;
    }

    // remaining kinds commit the effect on tile-tap.
    bool dispatched = false;

    switch (kind) {
        case 1:    // FlashOfInsight: "Discard any held tile."
            discardRackTile(slotIdx, false);
            dispatched = true;
            break;

        case 0xd:  // Metamorphosis: "Discard a held special snag.
                   //                Draw another special snag."
            discardRackTile(slotIdx, false);
            {
                int rolled = rollSnagType();
                pushReserveSnagTile(static_cast<uint32_t>(rolled), 0xffffffffu);
            }
            dispatched = true;
            break;

        case 0xe:  // SoothingMelody: "Blank a held normal snag tile."
            tappedTile->erase();
            dispatched = true;
            break;

        case 0x13: // EarlyWarning: "Set a held normal snag's {H} to 1."
            {
                SnagContent* sc = tappedTile->getSnagIfAlive();

                if (sc) {
                    float zero[2] = { 0.0f, 0.0f };
                    sc->setHpDisplay(1, this, zero);
                }
            }
            dispatched = true;
            break;

        case 0x14: // UntappedPotential: "Convert a held tile into Potential."
            tappedTile->erase();
            tappedTile->setTileContent(10, 0);   // content type 10 = Potential
            dispatched = true;
            break;

        case 0x15: // SuddenPhoneCall: "Convert held special snag into normal,
                   //                  Nemesis levels up." erase + transformToSnag
                   //                  (snag kind 1 = normal) + fill nemesis XP
                   //                  to next level (= 20 - nemesisXP).
            tappedTile->erase();
            tappedTile->transformToSnag(1u, &playerSystem,
                                        totalTurnCount, levelTurnCount, worldIndex);
            nemesisAdvance(20 - nemesis.nemesisXP);
            dispatched = true;
            break;

        case 0x2a: // Concentration: "Halve the {D} of a held normal or
                   //                special snag."
            {
                SnagContent* sc = tappedTile->getSnagIfAlive();

                if (sc) {
                    float zero[2] = { 0.0f, 0.0f };
                    sc->setDefDisplay(static_cast<int>(static_cast<unsigned>(sc->def) >> 1),
                                       this, zero);
                }
            }
            dispatched = true;
            break;

        case 0x30: // Premonition: "Discard a selected tile. Draw a snag
                   //              with 1 {A}."
            discardRackTile(slotIdx, false);
            {
                SnagContent* snag = pushReserveSnagTile(1u, 0xffffffffu);
                float zero[2] = { 0.0f, 0.0f };
                snag->setAtkDisplay(1, nullptr, zero);
            }
            dispatched = true;
            break;

        case 0x35: // Overcharge: "Double value of a selected {C} tile."
            {
                TileContent* tc = tappedTile->getTileContentIfAlive();
                int          m  = tappedTile->getContentMagnitude();
                tc->setMagnitude(m * 2);
            }
            dispatched = true;
            break;

        case 0x39: // Sideswipe: "Halve {A} of a normal held snag."
            {
                SnagContent* sc = tappedTile->getSnagIfAlive();

                if (sc) {
                    float zero[2] = { 0.0f, 0.0f };
                    sc->setAtkDisplay(static_cast<int>(static_cast<unsigned>(sc->atk) >> 1),
                                       this, zero);
                }
            }
            dispatched = true;
            break;

        case 0x41: // Backtrack: "Convert a held tile to Milestone, which
                   //            gives {H} and moves a snag." content type
                   //            25 = Milestone, magnitude 1.
            tappedTile->erase();
            tappedTile->setTileContent(0x19u, 1);
            dispatched = true;
            break;

        case 0x44: // Breakthrough: "Discard a held normal snag. Gain half
                   //               its {D}." capture snag.def before discard;
                   //               erase clears the snagContent pointer.
            {
                SnagContent* sc = tappedTile->getSnagIfAlive();
                int snagDef = (sc ? sc->def : 0);
                discardRackTile(slotIdx, false);
                setDEF(playerSystem.defence
                       + static_cast<int>(static_cast<unsigned>(snagDef) >> 1));
            }
            dispatched = true;
            break;

        // ---- all batch-1..4 panel-commit kinds covered. unhandled
        // kinds return without firing the cleanup pass (matches binary).
        default:
            return;
    }

    if (!dispatched) {
        return;
    }

    // ---- cleanup pass (binary's tail at 0x100021d51c bottom) ----
    EventSlot* armedSlot =
        reinterpret_cast<EventSlot*>(hud.selectedEvent);
    getGame()->achievementTracker().noteEventActivated(*armedSlot);

    Game* gnd = getGame();

    if (gnd) {
        gnd->soundQueue.trigger(0x2D);
    }

    hud.pendingDiscards.clear();

    hud.removeEventSlot(armedSlot, /*compact=*/true);

    eventsFired += 1;
    hud.selectedEvent = nullptr;
    dirty = true;
}

// FUN_10001f91c, applyPostTurnTileEffects. second pass over rack + page
// list after the main end-of-turn pipeline. drives a handful of "siphon"
// snag/content interactions: rack tiles that absorb stats from their
// neighbours, or page-tile snags that stamp decorations onto specific
// rack slots. fires in state-8 between applyEndOfTurnPipeline and
// applyChainStatBumpsToRack.
void GameBoard::applyPostTurnTileEffects() {
    const float* boardOrigin = &positionX;
    float zero[2] = { 0.0f, 0.0f };

    // ---- rack walk ----
    for (int slot = 0; slot < RACK_SLOT_COUNT; slot++) {
        TileObject* tile = rack[slot];

        if (!tile) {
            continue;
        }

        SnagContent* sn = tile->getSnagIfAlive();

        if (sn) {
            // alive snag: small switch over rack-walk snag types.
            int snagType = sn->type;

            if (snagType == 0x2a) {
                // Spotlight: stamp a kind-0 decoration on rack[2].
                if (rack[2]) {
                    rack[2]->pushDecoration(0, 0, 0);
                    pushAction(0, zero, tile);
                }
            }
            else if (snagType == 0x33) {
                // Bookends: stamp a kind-2 / value-2 decoration on rack
                // slot 0 and slot 4 if either exists. one action push
                // regardless of how many tiles were stamped.
                if (rack[0]) {
                    rack[0]->pushDecoration(2, 2, 0);
                }

                if (rack[4]) {
                    rack[4]->pushDecoration(2, 2, 0);
                }

                pushAction(0, zero, tile);
            }
            else if (snagType == 0x42) {
                // Bookend (single-end): stamp kind-0 on rack[0].
                if (rack[0]) {
                    rack[0]->pushDecoration(0, 0, 0);
                    pushAction(0, zero, tile);
                }
            }
            else if (snagType == 0x55) {
                // Hex: pick a rack tile with no alive snag or a basic
                // (type-1) snag. if its content is already a content-0x11
                // hex, grow its magnitude by 2; otherwise erase + stamp
                // a fresh content-0x11/magnitude-2. action + sound 0x21.
                int picked = pickRandomRackSlotMatching(predRackNoSnagOrSnagType1);

                if (picked >= 0) {
                    TileObject* victim = rack[picked];

                    if (victim->getContentType() == 0x11) {
                        TileContent* victimTc = victim->getTileContentIfAlive();

                        if (victimTc) {
                            victimTc->setRawAndDisplayMagnitude(
                                victimTc->displayedMagnitude + 2);
                        }
                    }
                    else {
                        victim->erase();
                        victim->setTileContent(0x11, 2);
                    }

                    pushAction(0, zero, tile);
                    Game* g = getGame();

                    if (g) {
                        g->soundQueue.trigger(0x21);
                    }
                }
            }

            continue;
        }

        // no alive snag: check content for the two stat-siphon types.
        TileContent* tc = tile->getTileContentIfAlive();

        if (!tc) {
            continue;
        }

        int contentType = tc->type;

        if (contentType == 0x18) {
            // Effort: pulls magnitude from neighbour stat tiles into self.
            // capped at magnitude 99. binary filters neighbours to types
            // 2/3/5/6 (= ATK/DEF/HP/CTRL) via the 0x6c bitmask.
            if (tc->displayedMagnitude >= 99) {
                continue;
            }

            std::vector<TileContent*> neighbours;

            if (slot > 0 && rack[slot - 1]) {
                TileContent* n = rack[slot - 1]->getTileContentIfAlive();

                if (n) {
                    neighbours.push_back(n);
                }
            }

            if (slot < 4 && rack[slot + 1]) {
                TileContent* n = rack[slot + 1]->getTileContentIfAlive();

                if (n) {
                    neighbours.push_back(n);
                }
            }

            // filter to stat-tile types (2 ATK / 3 DEF / 5 HP / 6 CTRL).
            // mask 0x6c has bits set at positions 2, 3, 5, 6.
            neighbours.erase(
                std::remove_if(neighbours.begin(), neighbours.end(),
                    [](TileContent* n) {
                        int t = n->type;
                        return !(t < 7 && ((1 << t) & 0x6c));
                    }),
                neighbours.end());

            if (neighbours.empty()) {
                continue;
            }

            // bump the magnet tile by +1.
            tc->setRawAndDisplayMagnitude(tc->displayedMagnitude + 1);
            pushStatTween(1, 1, &tile->mainQuad.posX, 0);

            // drain rate: -3 when only one neighbour qualifies, -1 when
            // two qualified. (binary's csel pattern matches sole = 3 / multi = 1.)
            int dec = (neighbours.size() == 1) ? 3 : 1;

            for (TileContent* n : neighbours) {
                int newMag = n->displayedMagnitude - dec;

                if (newMag <= 0) {
                    n->killAndFade();
                }
                else {
                    n->setRawAndDisplayMagnitude(newMag);
                    pushStatTween(1, -dec, &n->baseQuad.posX, 0);
                }
            }

            Game* g = getGame();

            if (g) {
                g->soundQueue.trigger(0x44);
            }
        }
        else if (contentType == 0xc) {
            // Clarity: each slot past slot 0 gains 1 magnitude per slot-
            // from-end, capped at total magnitude 99. slot 0 never fires
            // (the slotsFromEnd value gate covers slot 4 with gain=0).
            if (slot == 0) {
                continue;
            }

            int slotsFromEnd = 4 - slot;
            int magToCap     = 99 - tc->displayedMagnitude;
            int gain         = (slotsFromEnd <= magToCap) ? slotsFromEnd : magToCap;

            if (gain > 0) {
                tc->setRawAndDisplayMagnitude(tc->displayedMagnitude + gain);
                pushStatTween(1, gain, &tile->mainQuad.posX, 0);
            }
        }
    }

    // ---- page list walk ----
    // a few placed-snag types reach across to stamp decorations on
    // specific rack slots. uses boardOrigin for the action push since
    // these snags live on the scrolled board.
    for (TileObject* pageTile : pageList) {

        if (!pageTile) {
            continue;
        }

        SnagContent* sn = pageTile->getSnagIfAlive();

        if (!sn) {
            continue;
        }

        TileObject* target = nullptr;
        int         snagType = sn->type;

        if (snagType == 0x6a || snagType == 0x42) {
            target = rack[4];
        }
        else if (snagType == 0x2a) {
            target = rack[2];
        }

        if (target) {
            target->pushDecoration(0, 0, 0);
            TileObject* snParent = sn->tileParent;
            pushAction(0, boardOrigin, snParent);
        }
    }
}

// =============================================================================
// the in-game state machine: the big switch on `state` (+0x18) that lives
// at the bottom of FUN_100018ac8. cases 1..10 each have their own commit /
// transition logic; the default case (+ "fall through to default") just
// re-applies the trailing scroll-bounds clamp.
//
// state semantics (verified against binary):
//   1: idle: accept input, route via dispatchHexAndRackTouch /
//      tryConsumeXButton / onPointerReleasedDuringDrag / mid-grab anim.
//   2: drag-tracking: player has selected a rack tile, finger moving.
//   3: chain-bonus apply: runs once after a commit if flag1D set.
//   4: snag activation: runs once if !snagActivationSuppressed.
//   5: nemesis-interlock setup: scans page list for triggers.
//   6: hex-pickup commit: player tapping a placed tile.
//   7: post-resolve dispatch: refill rack, route to next state.
//   8: end-of-turn pipeline: applyEndOfTurnPipeline + post-turn top-off.
//   9: end-of-turn settle: gates on +0x8414 progress.
//  10: dim overlay: fades in / out for level transitions.
// =============================================================================
void GameBoard::tickInGameStateMachine(float dt, float touchInput) {
    // boolean: "any TileObject is still mid-animation", port of FUN_10001a940.
    // checks PlayerSystem character-token's spawn/move timers, then walks
    // the page list. per tile, FUN_100013818 walks trackedContent and
    // returns true if any tracked SnagContent has moveT < 1 or spawnT < 1.
    auto animPredicate = [&]() -> bool {
        float spawnT = playerSystem.spawnT;
        float moveT  = playerSystem.moveT;

        if (spawnT < 1.0f || moveT < 1.0f) {
            return true;
        }

        // walk page list; per tile, walk trackedContent (= FUN_100013818).
        for (TileObject* t : pageList) {

            if (!t) {
                continue;
            }

            for (SnagContent* sc : t->trackedContent) {

                if (sc && (sc->moveT < 1.0f || sc->spawnT < 1.0f)) {
                    return true;
                }
            }
        }

        return false;
    };

    // helper: state machine "fall to default" path = scroll bound clamp +
    // return without writing the new state.
    int newState = state;  // default: stay in current state
    bool writeNewState = false;
    bool jumpTo7Plus_8414 = false;  // = "LAB_10001958c" path: state-9-style direct commit
    bool jumpTo7AfterCommit = false;  // = "LAB_100019594" path: state-7

    switch (state) {

    case 1: {
        // dispatch input + back-button. when neither consumes the touch,
        // run release-drag cleanup + mid-grab mirror.
        bool dispatched = dispatchHexAndRackTouch(dt, touchInput);
        bool backFired  = false;

        if (!dispatched) {
            backFired = tryConsumeXButton();

            if (!backFired) {
                onPointerReleasedDuringDrag();
                animateMidGrabHexHighlight(dt);
            }
        }

        refreshDiscardButtonAvailability();  // FUN_100017b44

        // post-input transition logic.
        if (nemesis.eatTarget > 0) {
            jumpTo7Plus_8414 = true;
            break;
        }

        if (selectedRackSlot == -1) {

            if (hud.pendingDiscards.empty()) {
                // no staged discards + no rack tile selected -> stay in 1.
                break;
            }

            newState = 6;
            writeNewState = true;
        } else {
            newState = 2;
            writeNewState = true;
        }

        break;
    }

    case 2: {
        // drag-tracking. dispatch input -> if released, run release cleanup;
        // if still touching, advance the drag visualization.
        bool dispatched = dispatchHexAndRackTouch(dt, touchInput);

        if (!dispatched) {
            onPointerReleasedDuringDrag();
        }

        refreshDiscardButtonAvailability();

        if (selectedRackSlot == -1) {
            // touch released without a selection -> reset to state 1.
            // matches binary's `goto LAB_100019774` which is just
            // `uVar10 = 1; break;`, no animController.reset (only case 10
            // resets the controller before falling into the same label).
            newState = 1;
            writeNewState = true;
            break;
        }

        if (dispatched) {
            // touch was consumed and we're still selected -> stay in 2.
            break;
        }

        int navState = updateNavArrowAndConfirmDrag(dt);

        if (navState == 0) {
            animateMidGrabHexHighlight(dt);
            break;  // stay in state 2
        }

        // navState != 0 -> drag committed; advance to state 3.
        newState = 3;
        writeNewState = true;
        break;
    }

    case 3: {
        // chain-bonus apply. blocked on any tile mid-animation.
        if (animPredicate()) {
            break;  // stay in state 3 until animations settle
        }

        if (flag1D) {
            marchPageSnags();
        }

        newState = 4;
        writeNewState = true;
        break;
    }

    case 4: {
        if (animPredicate()) {
            break;
        }

        if (!snagActivationSuppressed) {
            tryActivateNewestPageSnag();
        }

        newState = 5;
        writeNewState = true;
        break;
    }

    case 5: {
        if (animPredicate()) {
            break;
        }

        armNemesisInterlockOnSpecialTiles();

        if (nemesis.eatTarget > 0) {
            jumpTo7Plus_8414 = true;
            break;
        }

        newState = 8;
        writeNewState = true;
        break;
    }

    case 6: {
        // hex-pickup commit. player has tapped a placed tile.
        bool dispatched = dispatchHexAndRackTouch(dt, touchInput);

        if (!dispatched) {
            applyHexPickupConsumeEffect(dt);
            animateMidGrabHexHighlight(dt);
        }

        if (!hud.pendingDiscards.empty()) {
            break;  // still resolving staged discards -> stay in 6
        }

        if (nemesis.eatTarget > 0) {
            jumpTo7Plus_8414 = true;
            break;
        }

        jumpTo7AfterCommit = true;
        break;
    }

    case 7: {
        // post-resolve dispatch.
        if (nemesis.eatStep != 0) {
            break;  // close-strike still active -> stay in 7
        }

        // pageList empty and nemesis.visible? -> game over, nemesis eats the player.
        if (pageList.empty() && nemesis.visible) {

            // binary clears these alongside the scoreRequested signal so the
            // state-7 path doesn't re-fire the eat cycle while the score
            // panel is opening. matches the FUN_10001925c branch tail.
            nemesis.eatTarget = 0;
            nemesis.eatActive = false;

            if (exitReached == 0 || playerDowned) {
                // death -> open ScorePanel via gb+0x01 (= scoreRequested
                // post-rename). do not use exitRequested here; that's the
                // PauseMenu "Main Menu" path that returns to title with no
                // score panel.
                scoreRequested = true;
                newState = 1;
                writeNewState = true;
            } else {
                Game* g = getGame();
                if (g) {
                    g->soundQueue.trigger(0x41);
                }

                newState = 10;
                writeNewState = true;
            }

            break;
        }

        // normal post-resolve: refill rack + apply chain bumps.
        dirty = true;
        refillRackPostCommit();
        applyChainStatBumpsToRack(dt);

        if (nemesis.eatTarget < 1) {
            // re-enter game state: state = 1, snagActivationSuppressed = 0,
            // flag1D = 0, combatEffectsSuppressed = 0.
            state = 1;
            snagActivationSuppressed = false;
            flag1D = false;
            combatEffectsSuppressed = false;
            // already in default-like exit; skip writeNewState (state was
            // written directly) but do bypass the auto-write at the end.
            break;
        }

        nemesis.eatTarget = 0;

        if (nemesis.eatActive) {
            newState = 8;
        } else {
            newState = 1;
        }

        writeNewState = true;
        break;
    }

    case 8: {
        // end-of-turn pipeline.
        if (playerDowned) {
            playerDowned = false;

            // FUN_10000d9ac(hud, 0): clears overlayState (death-heart gate)
            // while leaving overlayProgress intact, so tickDeathHeart fades
            // the heart sprite out instead of cutting it instantly.
            hud.resetOverlay(0);

            // FUN_10001dbe4(..., 20 - nemesisXP) - credits exactly enough XP
            // to fill the remaining bank, triggering an immediate level-up.
            // matches binary's `0x14 - +0x21A4` (= 0x14 - nemesis.nemesisXP).
            nemesisAdvance(20 - nemesis.nemesisXP);

            Game* g = getGame();
            if (g) {
                g->soundQueue.trigger(0x35);   // "revive" sound
            }

            applyOnDeathHpRefill();
        }

        if (exitReached == 0) {
            // normal end-of-turn (no level transition).
            applyEndOfTurnPipeline(dt);
            refillRackPostCommit();
            applyPostTurnTileEffects();
            applyChainStatBumpsToRack(dt);
            tryOpenEventChoicePanel();

            totalTurnCount += 1;
            levelTurnCount  += 1;
            flag1D = true;
            snagActivationSuppressed = false;
            combatEffectsSuppressed = false;

// DEBUG_FAST_CTRL: temporary +2 control per tile placed so we can iterate on the
// level-up flow without grinding control tiles. not in the binary. delete this
// block (the #define guard at game_board.h and the call here) when shipping.
#ifdef DEBUG_FAST_CTRL_ENABLED
            if (!playerDowned) {
                hud.advanceXPSlot(2, false);
            }
#endif
            // end DEBUG_FAST_CTRL

            // clear "any hint fired this frame"; binary clears it here.
            dialogPanel.anyHintFiredThisFrame = 0;

            dirty = true;

            // re-enter the state machine: write only state back to 1. the
            // flag bytes at 0x1C/0x1D/0x1E were already written above (snag
            // activation cleared, flag1D set to true, combat effects cleared)
            // and stay that way.
            state = 1;
            break;
        }

        // exitReached != 0 -> end-of-level path.
        Game* g = getGame();

        // achievement fan-out on level exit (= binary's FUN_10004db74). order
        // mirrors the binary: ALonelyRoad, WeHaveToGoBack, Breakthrough,
        // GottaGoFast, YouShallNotPass.
        {
            AchievementTracker& t = g->achievementTracker();
            TileObject* exitTile  = pageList.empty() ? nullptr : pageList.back();

            // "A Lonely Road": world >= 2 and nemesis never appeared this run.
            if (worldLevelIndex > 1 && !nemesis.visible) {
                t.increment(AchievementId::ALonelyRoad);
            }

            // "We Have to Go Back": 4+ rack slots hold a non-type-1 (= special)
            // snag. type 1 is the generic snag, anything else is "special".
            int specialHeld = 0;

            for (int i = 0; i < RACK_SLOT_COUNT; i++) {
                TileObject* rt = rack[i];

                if (rt != nullptr
                    && rt->getSnagIfAlive() != nullptr
                    && rt->getSnagType() != 1) {
                    specialHeld += 1;
                }
            }

            if (specialHeld >= 4) {
                t.increment(AchievementId::WeHaveToGoBack);
            }

            // "Breakthrough": unconditional. target 10 exits.
            t.increment(AchievementId::Breakthrough);

            // "Gotta Go Fast": world >= 2 and beaten in fewer than 111 tiles.
            if (worldLevelIndex > 1 && levelTurnCount < 111) {
                t.increment(AchievementId::GottaGoFast);
            }

            // "You Shall Not Pass": exit tile holds a Barricade (content type
            // 0xF). exit tile = newest page-list entry (= pageList.back()).
            if (exitTile != nullptr && exitTile->getContentType() == 0xF) {
                t.increment(AchievementId::YouShallNotPass);
            }
        }

        // FUN_10001dca0: pan the camera onto the tile just placed at the
        // exit hex (= pageList.back()). exitReached != 0 implies the page
        // list is non-empty.
        TileObject* newestTile = pageList.back();
        setNemesisPanTarget(newestTile->gridCol, newestTile->gridRow);

        // FUN_100056b98: starts the avatar's vanish animation (PlayerSystem.
        // update will now ramp characterPulseT 0 -> 1 over 0.7s, shrinking
        // the avatar). case 9 gates on characterPulseT >= 1.0.
        playerSystem.onLevelEnd();

        if (g) {
            g->soundQueue.trigger(0x40);
        }

        // discard all rack tiles with skipExtraEffects=true (the level is
        // ending; per-tile effects already applied).
        for (int i = 0; i < RACK_SLOT_COUNT; i++) {
            if (rack[i] != nullptr) {
                // FUN_10001dd14(this, i, 1): skipExtraEffects = true.
                discardRackTile(i, true);
            }
        }

        totalTurnCount += 1;
        newState = 9;
        writeNewState = true;
        break;
    }

    case 9: {
        // gate on the characterPulseT progress timer.
        float progress = playerSystem.characterPulseT;

        if (progress < 1.0f) {
            break;  // not settled yet -> stay in 9
        }

        // settled: prime nemesis.eatTarget = pageList.size(), fall through to
        // the FUN_10001c1a4 commit-tile path (= LAB_10001958c).
        nemesis.eatTarget = static_cast<int>(pageList.size());
        jumpTo7Plus_8414 = true;
        break;
    }

    case 10: {
        // dim overlay fade. rate is per-exitReached (= 1.0 or -1.0 select).
        constexpr float DIM_DURATION  = 0.7f;       // DAT_100059F60
        constexpr float DIM_PI        = 3.1415927f; // DAT_100059F64
        constexpr float DIM_ALPHA_END = 255.0f;     // DAT_100059F68

        // DAT_100059EF8 + exitReached*4: per-exitReached dim rate.
        // exitReached == 0 -> -1.0 (fade out / dim down).
        // exitReached != 0 -> +1.0 (fade in / dim up; level restart).
        constexpr float DIM_RATE_BY_FIELD3C[2] = { -1.0f, 1.0f };

        size_t idx = (exitReached != 0) ? 1u : 0u;
        float  step = (dt / DIM_DURATION) * DIM_RATE_BY_FIELD3C[idx];

        float t = dimProgress + step;

        if (t < 0.0f) {
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }

        dimProgress = t;

        // alpha = (0.5 - cos(t*PI)*0.5) * 255 + (1-eased) * 0.0
        float ease = 0.5f - std::cos(t * DIM_PI) * 0.5f;
        uint8_t alpha = static_cast<uint8_t>(static_cast<int>(
            ease * DIM_ALPHA_END + (1.0f - ease) * 0.0f));

        dimQuad.setAlpha(alpha);

        if (exitReached != 0) {
            // level-advance path: when fully dimmed, run initLevelContent.
            // it leaves dimProgress at 1.0; next case-10 frame sees
            // exitReached == 0 and starts the fade-out.
            if (dimProgress >= 1.0f) {
                initLevelContent();
            }
            break;
        }

        // exitReached == 0: regular fade-out. when fully faded, kick anim
        // controller to idle and transition to state 1.
        if (dimProgress > 0.0f) {
            break;
        }

        animController.reset();
        newState = 1;
        writeNewState = true;
        break;
    }

    default:
        // unknown state -> fall through to scroll clamp.
        break;
    }

    // LAB_10001958c: jump-7-plus-8414 path. fires when interlock counter
    // > 0 in any case that set it.
    if (jumpTo7Plus_8414) {
        // FUN_10001c1a4 = nemesisSpawnAtTrailTail. binary's LAB_10001958c
        // packs 0x3f800000 (= 1.0f) into the second-arg vector regs.
        nemesisSpawnAtTrailTail();
        newState = 7;
        writeNewState = true;
    } else if (jumpTo7AfterCommit) {
        // LAB_100019594: case-6 fall-through.
        newState = 7;
        writeNewState = true;
    }

    if (writeNewState) {
        state = newState;
    }

    // trailing scroll bound clamp (always runs, even on default path).
    // matches binary's switchD_10001925c_default.
    {
        float pX = positionX;

        if (pX <= scrollMinX) {
            pX = scrollMinX;
        }

        if (scrollMaxX <= pX) {
            pX = scrollMaxX;
        }

        positionX = pX;

        float pY = positionY;

        if (pY <= scrollMinY) {
            pY = scrollMinY;
        }

        if (scrollMaxY <= pY) {
            pY = scrollMaxY;
        }

        positionY = pY;
    }
}
//
// dual-pass walk over the page list:
//
// Pass 1: build a queue (cap 2) of TileObject pointers as we walk the page
// list. for each tile, if the queue isn't empty, dispatch FUN_10003de74 with
// the previous queue contents as args (= "this tile gets a chain bonus
// from each of the prior tiles in the queue"). when the queue would exceed
// 2, evict the oldest. type-13 tiles (FUN_1000133b0 == 0xD) reset
// the chain: clear the queue and start fresh.
//
// Pass 2 (only runs if Pass 1's queue ended empty): walks the page list a
// second time. for each pair of consecutive tiles where the older tile has
// alive snag of type 0xF and the older tile's stat at +0x490 > 1, dispatches
// FUN_10003de74 to apply the chain effect. plays sound 0x1F.
void GameBoard::marchPageSnags() {
    // queue caps at 2: binary's `if (2 < local_58 + 1)` evicts oldest
    // when count would exceed 2. so queue holds at most 2 elements:
    // queueFront (= newest pushed) and queueBack (= oldest still in
    // queue, when count == 2).
    //
    // dispatch + push only fires when queue is non-empty (binary's
    // `if (!bVar4)` gate). queue can only seed via the type-0xD reset
    // branch below.
    TileObject* queueFront = nullptr;
    TileObject* queueBack  = nullptr;
    int         queueCount = 0;

    // walk from oldest (= pageList.front()) toward newest (= pageList.back()).
    for (TileObject* t : pageList) {

        if (!t) {
            continue;
        }

        // queue-non-empty branch: dispatch + push.
        if (queueCount > 0) {
            SnagContent* sc = t->getSnagIfAlive();

            if (sc != nullptr) {
                // sendToward the queue's most recently pushed 0xD-content
                // tile (= queueFront). always fires when sc != null. when
                // the queue holds 2 (= an even-older 0xD seed survives in
                // queueBack), dispatch a second walk onto queueBack: the
                // snag splits between two anchor tiles.
                sc->sendToward(queueFront, false);

                if (queueCount > 1) {
                    sc->sendToward(queueBack, false);
                }
            }

            // push current onto the queue. shift old front to back; then
            // overwrite front. count grows up to 2; further pushes evict
            // the oldest (queueBack) silently by overwriting it.
            queueBack  = queueFront;
            queueFront = t;

            if (queueCount < 2) {
                queueCount++;
            }
        }

        // type-0xD reset: clear the queue entirely and re-seed with the
        // current tile. matches binary's FUN_100029320 (free entire queue)
        // + push current. always runs when content type is 0xD, regardless
        // of whether the !bVar4 branch above already pushed this tile;
        // the 0xD branch overwrites that result.
        int contentType = t->getContentType();

        if (contentType == 0xD) {
            queueFront = t;
            queueBack  = t;
            queueCount = 1;
        }
    }

    // Pass 2: only fires when Pass 1's queue ended empty.
    if (queueCount != 0) {
        return;
    }

    // binary walk: start at pageListHead->prev (= second-newest), step via
    // .prev down to oldest inclusive. so we visit: second-newest, third-
    // newest, ..., oldest. the newest tile is the snag's own parent and is
    // intentionally skipped.
    //
    // dispatch is from currTile's snag onto (1-back, 2-back) tiles.
    // 1-back is always set 1 step before 2-back so the variable shuffle
    // preserves a sliding window of (current, 1-back, 2-back) tiles.
    //
    // see the line-by-line trace in our memory's
    // feedback_dream_full_port_audit notes; the shifting is non-obvious.
    if (pageList.size() < 2) {
        return;
    }

    TileObject* tile_1back = nullptr;       // 1-back in sliding window
    TileObject* tile_2back = nullptr;       // 2-back in sliding window

    // reverse-iterate: skip newest (= back(), = snag's own parent), walk
    // second-newest -> oldest inclusive.
    auto rit = pageList.rbegin();
    ++rit;   // skip newest; binary starts at pageListHead->prev

    for (; rit != pageList.rend(); ++rit) {
        TileObject* currTile      = *rit;
        TileObject* newTile_2back = currTile;

        if (tile_2back != nullptr) {
            // dispatch logic only fires once we have at least one prior
            // tile (= second iter onward).
            SnagContent* curSnag = currTile ? currTile->getSnagIfAlive() : nullptr;
            bool skipDispatch = false;

            if (curSnag != nullptr) {

                if (curSnag->type == 0xF) {
                    int* chainStat = reinterpret_cast<int*>(&curSnag->consumedFlag);
                    int original = *chainStat;
                    int newStat  = 2;

                    if (original > 1) {
                        newStat = original - 1;
                    }

                    *chainStat = newStat;

                    if (original > 1) {
                        skipDispatch = true;  // = binary's "goto LAB_10001d3b0"
                    }
                }

                if (!skipDispatch) {
                    // first walk: optional "intermediate step" toward the
                    // 1-back tile, only when one exists in the sliding window.
                    if (tile_1back != nullptr) {
                        curSnag->sendToward(tile_1back, false);
                    }

                    // second walk: reparented final hop toward 2-back tile.
                    // reparent=true means the snag's owner moves with the
                    // animation: the snag actually changes parent on this
                    // hop, so the page-list back-pointer follows.
                    curSnag->sendToward(tile_2back, true);

                    Game* g = getGame();

                    if (g) {
                        g->soundQueue.trigger(0x1F);
                    }
                }
            }

            // LAB_10001d3b0 variable shuffle:
            //   newTile_2back = (1-back != 0 ? 1-back : 2-back)
            //   1-back        = currTile
            newTile_2back = (tile_1back != nullptr) ? tile_1back : tile_2back;
            tile_1back    = currTile;
        }

        // bottom of body: 2-back = newTile_2back.
        tile_2back = newTile_2back;
    }
}

// =============================================================================
// GameBoard::update, port of FUN_100018ac8.
//
// orchestrates the full per-frame update chain. flow is:
//   1. visibility early-out.
//   2. always-runs prologue: music post-fade dispatch, perk display tick,
//      detail-panel update, ambient pickup hint cascade.
//   3. dialog-pause early-out (+0x54B8 = DialogPanel.visible).
//   4. sub-screen guards: each panel that's currently active consumes
//      the entire frame and returns. five panels: encounter reward,
//      tile inspect / item upgrade, item display, score display, encounter.
//   5. steady-state: HUD update + one-shot dispatchers + floating-snag
//      rotation + per-frame system updates (playerSystem, statBars,
//      sub-collections, animController, hexMap) + score-fade tick +
//      detail-panel idle dismiss + nemesis update + per-page-tile walk +
//      rack alive-flag walk + global-tile-alpha sync + inertial pan +
//      cleanup-quad bob + discard-slide animation.
//   6. game state machine (cases 1..10).
//   7. scroll-bound clamp (always-runs; lives inside tickInGameStateMachine).
// =============================================================================
void GameBoard::update(float dt, float touchInput) {
    if (!visible) {
        return;
    }

    // ---- prologue (lines 1..18 of FUN_100018ac8) ----
    showNextAchievementBanner();
    achievementBanner.update(dt);
    detailPanel.update(dt);                  // FUN_10003f0e4
    tickAmbientPickupHinting(dt, touchInput); // FUN_10001980c

    // dialog-pause: when DialogPanel.visible is set (= +0x54B8), the
    // entire game freezes behind the modal. matches binary's
    // `if (param_3[0x54b8] != '\0') return;`.
    if (dialogPanel.visible) {
        return;
    }

    // ---- sub-screen guards (5 cascading early-returns) ----

    // 1. item-choice panel at +0xDEF0 (visible byte at +0xDEF8 = panel+0x08).
    if (itemChoicePanel.visible) {
        itemChoicePanel.update(dt, touchInput);

        // wait for the player to confirm a slot; selectedItem stays nullptr
        // until then.
        Item* selected = itemChoicePanel.selectedItem;

        if (selected == nullptr) {
            return;
        }

        // achievement item-equip fan-out (= binary's FUN_10004e168).
        {
            AchievementTracker& tracker = getGame()->achievementTracker();

            // "Like a Glove": item carries at least one rolled SpecialAbility.
            if (selected->abilities[0].abilityType != 0
                || selected->abilities[1].abilityType != 0) {
                tracker.increment(AchievementId::LikeAGlove);
            }

            // "I Want It All": item gives a bonus in all 3 stats.
            if (selected->atk != 0 && selected->def != 0 && selected->hp != 0) {
                tracker.increment(AchievementId::IWantItAll);
            }

            // "Power Without": item has 10+ in any stat.
            if (selected->atk >= 10 || selected->def >= 10 || selected->hp >= 10) {
                tracker.increment(AchievementId::PowerWithout);
            }
        }

        // ---- commit: install selected candidate into PlayerSystem ----
        // binary copies the selected Item (operator_new + Item copy ctor)
        // and pushes the copy into playerSystem.baseItems[type], deleting
        // the previously-held item via PlayerSystem::push. the panel still
        // owns the original selected Item (in slots[].candidateItem);
        // close() frees it below.
        Item* newItem = new Item(*selected);
        playerSystem.push(newItem);

        itemChoicePanel.close();

        // sync HUD stat numbers to the newly recomputed stats.
        hud.setMaxHealth(playerSystem.maxHealth);
        hud.setHealth(playerSystem.currentHealth, false);

        // Item SpecialAbility 18 ("+1 {X} per item upgrade") effect: if
        // any of the player's 3 base Items has this SpecialAbility, push
        // +1 XP into the HUD's marker bank. binary checks "ability exists"
        // (FUN_1000567b0 = baseItemSpecialAbilityValue) then advances XP by
        // a hardcoded 1 (= the ability's static magnitude).
        if (playerSystem.baseItemSpecialAbilityValue(0x12) >= 1) {
            hud.advanceXPSlot(1, false);
        }

        itemsFound += 1;
        dirty = true;
        return;
    }

    // 2. level-up panel at +0xC7B8 (visible byte at +0xC7C0 = panel+0x08).
    if (levelUpPanel.visible) {
        levelUpPanel.update(dt, touchInput);

        // wait for the player to lock in their picks. readyToCommit at
        // panel+0x1730 (= GameBoard+0xDEE8) is set by update() once 2
        // picks are chosen (or 1 + 1 from each category when perkLevel(0xE)
        // is 0).
        if (!levelUpPanel.readyToCommit) {
            return;
        }

        // commit drain, stat picks first (binary's switchD_100018cec_caseD_1
        // loop). returns 0 when no stat picks remain, otherwise yields the
        // stat-type code (2=ATK, 3=DEF, 6=HP) and writes the magnitude into
        // local. recomputeStats fires after each pick.
        {
            int val = 0;

            while (int code = levelUpPanel.getNextStatPick(&val)) {

                switch (code) {
                    case 2: playerSystem.baseATK += val; break;
                    case 3: playerSystem.baseDEF += val; break;
                    case 6: playerSystem.baseHP  += val; break;
                    default: break;   // unknown code; binary loops anyway
                }

                playerSystem.recomputeStats();
            }
        }

        // perk picks (binary's switchD_100018cec_caseD_0). drains each
        // picked Perk* and applies it via addOrUpgradePerk(perkType). the
        // panel's preview Perks stay in their slots until next open()
        // cleans them up, matching the binary's deferred-cleanup pattern.
        while (Perk* picked = levelUpPanel.getNextPerkPick()) {
            playerSystem.addOrUpgradePerk(picked->perkType);
        }

        // bookkeeping counter at +0x83A8 (= PlayerSystem.currentLevel).
        playerSystem.currentLevel += 1;
        levelUpPanel.close();

        // sync HUD numbers to the newly recomputed stats.
        hud.setMaxHealth(playerSystem.maxHealth);
        hud.setHealth(playerSystem.currentHealth, false);

        // SpecialAbility 6 grants "+1 control marker per level-up". if any
        // base item carries a non-zero ability-6 value, fire one
        // advanceCTRLSlot on the HUD's control bank.
        if (playerSystem.baseItemSpecialAbilityValue(6) > 0) {
            hud.advanceCTRLSlot(1, false);
        }

        levelsGained += 1;
        dirty = true;

        // achievement "Power Within" (= binary's FUN_10004e3b4 tail call).
        // fires when any base stat is now >= 10.
        if (static_cast<uint32_t>(playerSystem.baseATK) >= 10
            || static_cast<uint32_t>(playerSystem.baseDEF) >= 10
            || static_cast<uint32_t>(playerSystem.baseHP)  >= 10) {
            getGame()->achievementTracker().increment(AchievementId::PowerWithin);
        }

        return;
    }

    // 3. event-choice panel at +0xB2A8
    if (eventChoicePanel.visible) {
        eventChoicePanel.update(dt, touchInput);

        EventSlot* sourceEvent = eventChoicePanel.selectedEventOnConfirm;

        if (sourceEvent == nullptr) {
            return;
        }

        // Sudden perk (0x14) magnitude bonus: rolls a 0/1 starting-
        // charge bonus weighted by perk level.
        //   lvl 3: always +1
        //   lvl 2: 50% chance +1, else +0
        //   lvl 1: ~25% chance +1
        //   else:  +0
        int magnitudeBonus = 0;
        {
            int suddenLvl = playerSystem.perkLevel(0x14);

            if (suddenLvl >= 3) {
                magnitudeBonus = 1;

            } else if (suddenLvl == 2) {
                int roll = rngInt(0, 100, 3);
                magnitudeBonus = (roll < 0x33) ? 1 : 0;

            } else if (suddenLvl == 1) {
                int roll = rngInt(0, 100, 3);
                magnitudeBonus = (roll < 0x1a) ? 1 : 0;
            }
        }

        // clone the source EventSlot: new heap allocation + EventSlot::init
        // with (sourceEventType, sourceCurrentCharges + magnitudeBonus).
        // matches the binary's `operator_new(0xDD0)` + thunk_FUN_100029a28
        // call sequence.
        EventSlot* installed = new EventSlot();
        installed->init(sourceEvent->eventType,
                        sourceEvent->currentCharges + magnitudeBonus);
        hud.addEventSlot(installed);

        eventChoicePanel.close();
        dirty = true;

        // achievement events-held milestones (= binary's FUN_10004e270).
        // count > 1 fires SpoiltForChoice; count > 3 also fires NeverADullMoment.
        int eventsHeld = hud.countEventsHeld();
        AchievementTracker& tracker = getGame()->achievementTracker();

        if (eventsHeld > 1) {
            tracker.increment(AchievementId::SpoiltForChoice);

            if (eventsHeld > 3) {
                tracker.increment(AchievementId::NeverADullMoment);
            }
        }

        return;
    }

    // 4. pause menu at +0xF488.
    if (pauseMenu.visible) {
        pauseMenu.update(dt, touchInput);

        // copy each volume slider's linearValue into board.seVolume /
        // bgmVolume. Game::dispatchSounds reads seVolume as the SE gain
        // context, and Game::update reads bgmVolume -> MusicController.
        seVolume  = pauseMenu.volumeSliders[0].linearValue;
        bgmVolume = pauseMenu.volumeSliders[1].linearValue;

        // Main Menu byte (pauseMenu.exitRequest, +0xD9C) propagates even
        // while pauseMenu is still visible: the tab-0 release path doesn't
        // call panelHide on the parent, so exitRequested is set before the
        // re-check below. case 5 in Game::update closes gb (and pauseMenu
        // with it) on the next overlay-complete tick.
        if (pauseMenu.exitRequest != 0) {
            exitRequested = true;

            // saving settings is a deviation from the original binary, but a sensible one
            saveNewSettings = true;
        }

        // re-check visible: if the panel just hid itself this frame
        // (Forfeit-confirm path), fall through to the close-only side-
        // effects below. otherwise (still up: Tutorial toggle still in
        // flight, or Main Menu just flagged the exit) return.
        if (pauseMenu.visible) {
            return;
        }

        // panel just closed: apply close-only side-effects.
        char newTutorialFlag = pauseMenu.tutorialFlag;

        if (tutorialFlag != newTutorialFlag) {
            tutorialFlag = newTutorialFlag;

            if (newTutorialFlag != 0) {
                // reset the 24 hint "shown" markers so the ambient cascade
                // re-fires for the new mode.
                std::memset(dialogPanel.hintShown, 0, 24);
            }

            dirty = true;
        }

        // Forfeit byte (pauseMenu.scoreRequest, +0xD9D) propagates only
        // after pauseMenu closes: the Forfeit-confirm popup's own panelHide
        // is what closes pauseMenu, so this branch runs in the same frame
        // the popup commits.
        if (pauseMenu.scoreRequest != 0) {
            scoreRequested = true;
        }

        saveNewSettings = true;
        return;
    }

    // 5. UserStatsPanel at +0xA3F8 (= FUN_10000a594). when visible, consume
    // the frame and return early so gameplay input doesn't bleed through.
    if (userStatsPanel.visible) {
        userStatsPanel.update(dt);
        return;
    }

    // ---- steady-state per-frame chain ----

    hud.update(dt);                          // FUN_10000c18c

    // one-shot dispatchers: re-entry hooks for the HUD marker-bank fills
    // that finished last frame. the HUD marker tick raises hud.levelUpReady
    // (= GameBoard+0x7FD8) when the XP bank fills, and hud.itemChoiceReady
    // (= GameBoard+0x7FD9) when the CONTROL bank fills.
    if (hud.levelUpReady != 0) {
        hud.levelUpReady = 0;
        levelUpPanel.open(&playerSystem);
        return;
    }

    if (hud.itemChoiceReady != 0) {
        hud.itemChoiceReady = 0;
        itemChoicePanel.open(&playerSystem);
        return;
    }

    // snag 0x43 (Confusion): "While Held: Placed tile rotates continuously."
    // while a rack tile is selected for placement and a Confusion snag is held,
    // spin that tile so the player can't aim it. (gameBoardUpdate: FUN_10001282c
    // + FUN_100012ecc + fmodf + FUN_100012844.)
    if (selectedRackSlot != -1 &&
        findSnagInRack((int)SnagKind::Confusion) != nullptr) {
        TileObject* tile = rack[selectedRackSlot];

        // FUN_10001282c(tile, 1): kick the icon-fade anim if not already on.
        if (!tile->slideAnimActive) {
            tile->slideAnimActive = true;
            tile->iconFadeT       = 0.0f;
        }

        // advance the current rotation by 90 deg/sec (DAT_059f58), wrap at 360
        // (DAT_059f5c), and write both quads via setRotationDirect.
        float spun = std::fmod(tileCurrentRotationDegrees(tile) + dt * 90.0f,
                               360.0f);
        tile->setRotationDirect(spun);
    }

    // per-frame system updates. order matches binary line-for-line.
    playerSystem.update(dt);                // FUN_10005653c
    statBars.update(dt);                     // FUN_10003beec

    // stat-change tween per-frame tick (FUN_10002bee0). with nothing
    // animating, anyAnimating=false gives an instant early-out.
    tickStatTween(dt);

    // action queue per-frame tick (FUN_1000384cc). an empty
    // sentinel-aliased list walks as a no-op.
    tickActionQueue(dt);

    animController.update(dt);               // FUN_10003a010
    hexMap.update(dt);                       // FUN_10003b0c8

    tickExitArrowFade(dt);                   // FUN_10001a5a0
    tickDetailPanelIdleDismiss();            // FUN_10001a690
    updateNemesisAndCloseStrike(dt, touchInput);  // FUN_10001a780

    // per-page-tile update walk. binary uses raw dt (= fVar19) here, not
    // the hud-scaled dt that's used in the rack walk below.
    for (TileObject* t : pageList) {

        if (t) {
            t->update(dt);
        }
    }

    // rack-tile walk with "flipped upside-down" computation. setting
    // TileObject+0x244 (rotationAnimActive) to true triggers the tile's
    // 180 deg flip via update()'s rotationAnimT lerp. two snags drive this:
    //   - Change (0x39) in rack: always flip every rack tile.
    //   - Whimsy (0x38) on board: flip every other turn (even-numbered).
    // selected slot is excluded: the player's currently-grabbed tile
    // stays upright while flipped.
    bool flipRackTiles;
    SnagContent* changeSnag = findSnagInRack(0x39);

    if (changeSnag != nullptr) {
        flipRackTiles = true;
    } else if ((totalTurnCount & 1) == 0) {
        flipRackTiles = hasSnagInBoard(0x38);
    } else {
        flipRackTiles = false;
    }

    for (int i = 0; i < RACK_SLOT_COUNT; i++) {

        if (!rack[i]) {
            continue;
        }

        // animPredicate inline: when any tile's tracked SnagContent is
        // mid-animation, skip the flip-flag write (= preserve current
        // state until animations settle). matches FUN_10001a940's
        // per-rack-slot call. body is the same as the lambda inside
        // tickInGameStateMachine, kept inline to mirror the binary's
        // per-iteration call pattern.
        bool animating;
        {
            float spawnT = playerSystem.spawnT;
            float moveT  = playerSystem.moveT;

            if (spawnT < 1.0f || moveT < 1.0f) {
                animating = true;
            } else {
                animating = false;

                for (TileObject* t : pageList) {

                    if (!t) {
                        continue;
                    }

                    for (SnagContent* sc : t->trackedContent) {

                        if (sc && (sc->moveT < 1.0f || sc->spawnT < 1.0f)) {
                            animating = true;
                            break;
                        }
                    }

                    if (animating) {
                        break;
                    }
                }
            }
        }

        if (!animating) {
            // animPredicate returned 0 -> safe to update the flip flag.
            // selected slot stays upright; everyone else flips when the
            // rack-flip predicate is set.
            bool flipBit = flipRackTiles && (i != selectedRackSlot);
            rack[i]->rotationAnimActive = flipBit;  // = +0x244
        }

        rack[i]->update(dt);
    }

    // trailing internal updates (still steady-state, before state machine).
    syncGlobalTileAlpha();                   // FUN_10001a9b4
    tickInertialPanScroll(dt);               // FUN_10001aa84
    animateCleanupQuadBob(dt);               // FUN_10001ac04
    tickDiscardingTilesAnimation(dt);        // FUN_10001ad30

    // the big switch + scroll-bound clamp.
    tickInGameStateMachine(dt, touchInput);
}

// reconstructed from FUN_1000161fc
void GameBoard::initLevel(int characterIndex, uint32_t worldIndex,
                          const std::set<int>& snagFilter,
                          const std::set<int>& eventFilter) {
    float virtualHeight = Renderer::getVirtualHeight();

    // section 1: achievement-tracker session begin + perk display reset.
    // matches FUN_1000161fc's first two calls:
    //   FUN_10004d7ec(gamePtr + 0x42f8, 1);   -> achievementTracker.beginSession(1)
    //   FUN_10004f3ac(this + 0x9e18);         -> achievementBanner.reset()
    Game* gNow = getGame();

    if (gNow) {
        gNow->achievementTracker().beginSession(1);
    }

    achievementBanner.reset();

    // section 2: character / difficulty data.
    // matches the binary's opening writes in FUN_1000161fc:
    //   *(param_1 + 8)    = param_3              ; gb.worldIndex
    //   param_1[0xc]      = *(param_4 + 4)       ; gb.tutorialFlag = game.tutorialFlag
    //   *(param_1 + 0x10) = *(param_4 + 0xc)     ; gb.seVolume     = game.globalSeVolume
    //   *(param_1 + 0x14) = *(param_4 + 0x10)    ; gb.bgmVolume    = game.globalBgmVolume
    // where param_4 = &game.settingsMagic_ (= game+0x2E6DC). this is the
    // global-to-board seeding path that carries the user's saved settings
    // into the gameplay session.
    this->worldIndex = worldIndex;

    if (gNow) {
        this->tutorialFlag = gNow->tutorialFlag();
        this->seVolume     = gNow->globalSeVolume();
        this->bgmVolume    = gNow->globalBgmVolume();
    }

    // section 3: set gameplay flags (from decompilation, line by line)
    visible = true;
    exitRequested = false;
    scoreRequested = false;
    dirty = false;
    saveNewSettings = true;

    playerDowned = false;

    state = 0;
    // clear counters at +0x20 through +0x40. all 7 ints surface on the
    // post-run score panel (gb+0x20..gb+0x38).
    totalTurnCount       = 0;
    worldLevelIndex      = 0;
    snagsDefeated        = 0;
    specialSnagsDefeated = 0;
    levelsGained         = 0;
    itemsFound           = 0;
    eventsFired          = 0;
    exitReached          = 0;
    levelTurnCount         = 0;

    // section 4: clear old game data, reset the 24 tutorial hint "shown"
    // markers + the any-fired byte (binary memsets 0x19 = 25 bytes =
    // hintShown[24] + anyHintFiredThisFrame) at level start so hints re-fire.
    memset(dialogPanel.hintShown, 0, 24);
    dialogPanel.anyHintFiredThisFrame = 0;

    // free + clear the rack at +0x158. the binary calls a sub-object dtor
    // (thunk_FUN_10001220c -> operator_delete) on each non-null entry. our
    // TileObject has a real dtor, so `delete` does the same job. on a fresh
    // first-level startup these are all null already; this loop matters when
    // the player advances levels (Phase C).
    for (int i = 0; i < RACK_SLOT_COUNT; i++) {

        if (rack[i] != nullptr) {
            delete rack[i];
            rack[i] = nullptr;
        }
    }

    // clear tab visibility flags
    for (int i = 0; i < MAX_CURSOR_COUNT; i++) {
        tileCursorVisible[i] = false;
        tileCursorState[i] = false;
    }

    // section 4b: free every TileObject held by the placed-tile pagelist
    // (+0x1A8) and the discard-slide list (+0x96A0), then clear both lists.
    // mirrors FUN_1000161fc's two walks + the FUN_100029320 / FUN_1000292c4
    // tail clears. without this, the tiles + snags placed on the board last
    // run survive across initLevel and reappear on the new run's hex grid.
    for (TileObject* tile : pageList) {

        if (tile) {
            delete tile;
        }
    }

    pageList.clear();

    for (DiscardSlideBody& entry : discardSlide) {

        if (entry.tile) {
            delete entry.tile;
        }
    }

    discardSlide.clear();

    // section 4c: drop the per-level cosmetic-variant rotation state so
    // initLevelContent re-seeds 0..0xB and re-rolls tileVariant. mirrors
    // FUN_1000161fc's two collection clears at +0x128/+0x140.
    variantsUsed.clear();
    variantsRemaining.clear();

    // section 4d: animation-banner seed bookkeeping. matches FUN_1000161fc's
    // vector-clear + tree-free pair at +0x9DD0 / +0x9DE8.
    animBannerSeedHistory.clear();
    animBannerSeedPool.clear();

    // section 5: spawn the Nemesis at +0x9D0 (matches binary FUN_1000161fc).
    // placeOnHexGrid is not called here in the binary either: at a fresh
    // level start the body segments stay un-UV'd and invisible until the
    // Nemesis is summoned in play. the restore path (FUN_100016b18 /
    // restoreFromSnapshot) places it on resume. bg circle, center hex, XP
    // outline dots, and the level-number tint render via init() defaults.
    nemesis.setNemesisLevel(1);
    nemesis.setNemesisXP(0);

    // section 6: PlayerSystem reset + 3 starter baseItems.
    // FUN_1000562f4(this + 0x8270, characterIndex) clears slots and applies
    // the character portrait UV. then FUN_1000161fc spawns 3 baseItems via
    // operator_new(0x610) and pushes each, exactly mirrored here.
    playerSystem.reset(characterIndex);

    // matches FUN_1000161fc's three calls. parameters are
    // (type, atk, def, ctrl, filterMask):
    //   type 0 starter: ATK Item   atk=1, def=0, ctrl=0
    //   type 1 starter: CTRL Item  atk=0, def=0, ctrl=1
    //   type 2 starter: DEF Item   atk=0, def=1, ctrl=0
    // each push triggers recomputeStats; after the third push,
    // maxHealth = (baseHP + sumHP) * 5 = (1 + 1) * 5 = 10.
    {
        Item* atkStarter = new Item();
        atkStarter->init(&playerSystem, 0, 1, 0, 0, 0xFFFFFFFFu);
        playerSystem.push(atkStarter);

        Item* ctrlStarter = new Item();
        ctrlStarter->init(&playerSystem, 1, 0, 0, 1, 0xFFFFFFFFu);
        playerSystem.push(ctrlStarter);

        Item* defStarter = new Item();
        defStarter->init(&playerSystem, 2, 0, 1, 0, 0xFFFFFFFFu);
        playerSystem.push(defStarter);
    }

    // heal to full at level start (currentHealth = maxHealth).
    playerSystem.currentHealth = playerSystem.maxHealth;

    // HUD level config (mirrors FUN_1000161fc): read stat values from the
    // PlayerSystem fields at the binary's expected offsets +0x83AC..+0x83B8.
    // attack/defence are direct PlayerSystem fields; HP/MaxHP get computed
    // from CTRL aggregation in recomputeStats above.
    hud.setMaxHealth(playerSystem.maxHealth);
    hud.setHealth(playerSystem.currentHealth, 0);  // holdRatio=0 (snap)
    hud.setAttack(playerSystem.attack);
    hud.setDefence(playerSystem.defence);

    // remaining section 7 calls in the binary (FUN_1000161fc):
    //   FUN_10000d9ac(hud, 1)  -> resetOverlay(1)
    //   FUN_10000d778(hud)     -> clearEventSlots (drops all 4 event tray entries)
    //   FUN_10000d0c8(hud)     -> clearCTRLBank (zeros control marker count)
    //   FUN_10000d0b4(hud)     -> clearXPBank (zeros xp marker count)
    // without these, a new run inherits the previous run's event slots,
    // control gauge, and xp markers.
    hud.resetOverlay(1);
    hud.clearEventSlots();
    hud.clearCTRLBank();
    hud.clearXPBank();

    // section 8: reset visual state.
    // FUN_1000165e8 does not touch dimQuad alpha or dimProgress; those are
    // owned by case-10 of the state machine, which leaves dimProgress at 1.0
    // when it calls us, then fades it back to 0 on the next frame. resetting
    // either here would skip the fade-out (the "pop" symptom). the restore
    // path (FUN_100016b18 / restoreFromSnapshot) does reset both, since a
    // resume has no in-flight fade to preserve.

    // FUN_10002f72c(this + 0xF488): Menu::panelHide(pauseMenu, 1).
    // ensures the pause menu is hidden when a new level starts.
    pauseMenu.panelHide(true);

    // section 9: shop-pool filter copies. snag-type roller (rollSnagType)
    // and event candidate roller (EventChoicePanel::collectCandidates)
    // both consult these sets to gate which snags / events can spawn.
    excludedSnagTypes                  = snagFilter;
    eventChoicePanel.excludedHistory   = eventFilter;

    // section 10: position everything
    // FUN_1000165e8(param_1) - initLevelContent
    initLevelContent();

    SDL_Log("GameBoard::initLevel() complete (worldIndex=%d, character=%d, state=%d)",
            worldIndex, characterIndex, state);
}

// reconstructed from Ghidra FUN_1000165e8, full per-level content init.
//
// the binary's flow is dense: 31 distinct steps from "set dirty flag" through
// "sync music for level". our port keeps the same step ordering and matches
// each binary call with either (1) the corresponding ported helper / method,
// (2) an inline replication of trivial side-effects, or (3) a clearly-marked
// TODO with the FUN_X anchor for sub-systems we haven't typed yet.
//
// level 1 critical path (everything visible to the player on level 1
// entry): steps 7.20 (hardcoded reserve push) and 7.21 (5x populateRack)
// are the ones that fill the player's hand. the layout-variant roll at 7.3/7.4
// rotates gridLayout through 0..0xB each level via variantsRemaining (set) +
// variantsUsed (vector), picking with RNG stream 0 and re-seeding the set
// when it empties.
void GameBoard::initLevelContent() {
    float virtualHeight = Renderer::getVirtualHeight();

    // 7.1 state flags + counters
    dirty           = true;
    worldLevelIndex = worldLevelIndex + 1;
    exitReached         = 0;
    snagActivationSuppressed          = false;
    flag1D          = true;
    combatEffectsSuppressed          = false;
    levelTurnCount    = 0;

    // 7.2 seed pickup snag threshold (port of FUN_100017f28)
    seedPickupSnagThreshold();

    // 7.3 + 7.4 cosmetic variant rotation. binary picks one of 12 variants
    // (0..0xB) per level so consecutive levels don't reuse a sprite sheet.
    //
    // if variantsRemaining is empty, clear variantsUsed and re-seed the set
    // with 0..0xB (matches FUN_1000165e8's `if (*(this+0x150) == 0)` branch).
    // then RNG-pick a variant from the set via stream 0, erase it, set
    // tileVariant, append to variantsUsed.
    if (variantsRemaining.empty()) {
        variantsUsed.clear();

        for (int i = 0; i < 12; ++i) {
            variantsRemaining.insert(i);
        }
    }

    int idx = rngInt(0, static_cast<int>(variantsRemaining.size()) - 1, 0);
    auto it = std::next(variantsRemaining.begin(), idx);
    gridLayout = *it;
    variantsRemaining.erase(it);
    variantsUsed.push_back(gridLayout);

    // 7.5 reset rack-pickup + nav-drag state. binary writes 8 bytes at +0x198
    // covering both draggedRackSlot (+0x198) and selectedRackSlot (+0x19C)
    // at once, then 4 bytes at +0x1c0 = -1 (navDragState idle).
    draggedRackSlot  = -1;
    selectedRackSlot = -1;
    navDragState     = -1;

    // 7.6 board position layer: center-of-screen translate. these drive
    // GameBoard::draw's outer glTranslatef. also clear the drag /
    // inertial-fling active flags and park the pan animation at "idle"
    // (panProgress = 1.0).
    positionX = 0.5f;
    positionY = virtualHeight * 0.5f;
    panDragActive = false;
    panInertiaActive = false;
    panProgress = 1.0f;

    // 7.7 recompute scroll bounds (port of FUN_1000174c4).
    recomputeScrollBounds(0.0);

    // 7.8 reset the global tile-alpha ramp pair so the dim doesn't carry
    // across a level transition.
    tileAlphaMirror   = 0.0f;
    tileAlphaProgress = 0.0f;

    // 7.9 nemesis reset (FUN_10000996c, single visible-byte clear)
    nemesis.reset();

    // 7.10 clear nemesis eat-cycle state (last 16 bytes of NemesisRenderable).
    // matches the binary's `memset(GameBoard+0x43F0, 0, 0x10)`.
    nemesis.eatTarget = 0;
    nemesis.eatActive = false;
    nemesis.eatStep   = 0;
    nemesis.eatFired  = false;

    // 7.11 detailPanel.reset(1) / dialogPanel.reset(1).
    // both early-out unless their `visible` flag is set; at level start
    // visible == 0 from the GameBoard memset, so these calls are no-ops.
    detailPanel.reset(1);
    dialogPanel.reset(1);

    // 7.12 reset the discard-staging queue + selected Event pointer
    // (binary's FUN_100007df0 per-entry dtor is a no-op since DiscardEntry
    // is trivially destructible).
    hud.pendingDiscards.clear();
    hud.selectedEvent = nullptr;

    // 7.13 PlayerSystem secondaryInit (FUN_100056528) + clear inherited
    // `parent` pointer (the binary nulls it after secondaryInit).
    playerSystem.secondaryInit();

    // 7.14 place avatar at hex (0, 0). the binary calls
    //   stp s0,s1,[sp,#0x20];   // FUN_100012f04 returns both X (s0) and Y (s1)
    // and feeds the (X, Y) pair to MovableActor::setPosition.
    HexCellPos avatarHex = hexCellSnappedXY(0, 0);
    playerSystem.baseQuad.posX = avatarHex.x;
    playerSystem.baseQuad.posY = avatarHex.y;

    // 7.15 hide the StatBars row at level start (FUN_10003be88 early-outs
    // when !visible). show is gated by gameplay state (Phase C / combat).
    statBars.visible = false;

    // 7.16 stat-change tween mark-all-completed (FUN_10002c170). at level
    // start the list is empty so this is a no-op walk; on level transition
    // it abandons every in-flight "+N" tween without freeing the nodes.
    clearStatTween();

    // 7.17 tileWeightPool reset (FUN_10004d198). restores currentWeight
    // to baseWeight for each of the 5 entries seeded in GameBoard::create.
    resetTileWeightsToBase(tileWeightPool);

    // 7.18 action queue mark-all-completed (FUN_1000388cc). at level start
    // the list is empty so this is a no-op walk; on level transition it
    // abandons every in-flight burst without freeing the nodes.
    clearActionQueue();

    // 7.19 reset the gameplay-items vector live-count (+0x96D0). draw 8.2
    // (GameBoard::draw) walks gameplayItems[0..gameplayItemsLiveCount) each
    // frame; the vector itself keeps its allocation.
    gameplayItemsLiveCount = 0;

    // 7.20 free entire tile reserve queue. binary deletes each held tile
    // then clears the list. at level start the reserve is already empty,
    // so this is a no-op. on Phase C level transitions it actually frees.
    for (TileReserveEntry& entry : tileReserve) {

        if (entry.tile != nullptr) {
            delete entry.tile;
        }
    }

    tileReserve.clear();

    // 7.21 if worldLevelIndex == 1: hardcoded level-1 reserve queue contents.
    // 8 content tiles + 1 generic snag (kind=1). the player's first hand is
    // drawn from these via 5x populateRack below. binary-faithful branch:
    // for level 2+ the reserve carries over from the previous level (mid-
    // play pushReserveTile / pushReserveSnagTile calls), and rollRackTile
    // RNG-rolls a fresh tile if the queue is empty.
    int populateCount = 5;

    if (worldLevelIndex == 1) {
        pushReserveTile(3, 0xFFFFFFFF);
        pushReserveTile(3, 0xFFFFFFFF);
        pushReserveTile(6, 0xFFFFFFFF);
        pushReserveTile(2, 0xFFFFFFFF);
        pushReserveTile(2, 0xFFFFFFFF);
        pushReserveTile(3, 0xFFFFFFFF);
        pushReserveTile(2, 0xFFFFFFFF);
        pushReserveTile(5, 0xFFFFFFFF);
        pushReserveSnagTile(1, 0xFFFFFFFF);
    }

    // 7.22 5x populateRack, popping one tile from reserve into rack[0..4]
    // each. reserve count goes 9 -> 4; rack count goes 0 -> 5.
    for (int i = 0; i < populateCount; i++) {
        populateRack();
    }

    // 7.23 hexMap sentinel-list init (port of FUN_10003b204).
    hexMap.init();

    // 7.24 layout pass (port of FUN_100018408 -> FUN_1000175f0). RNG-rolls
    // a (col, row) jitter origin, writes it to exitCol/exitRow, then adds
    // the level-exit cell (kind=1) plus the 6 hex-neighbor key cells
    // (kind=2) to the hex map so HexMap::draw can render the goal markers.
    {
        // FUN_100018408: roll a jitter (col, row) within +/- range. range is
        // 0x32 (50) on level 1, 100 on later levels.
        int range = (worldLevelIndex == 1) ? 50 : 100;

        // RNG draws, stream 0 (cosmetic / common, since hex layout choice
        // is cosmetic). matches binary literal `0` arg to FUN_1000570ec.
        int rolledCol = rngInt(-range, range, 0);
        int rolledRow = rngInt(0, 100, 0);
        int signedRow = (rolledRow > 50) ? 1 : -1;

        // binary's clamp logic on (rolledCol, rolledRow):
        //   if (rolledCol < 1 || rolledRow < 51) and (rolledCol >= 0 || rolledRow > 50):
        //       rangeClamped = range - abs(rolledCol)
        //   else:
        //       if abs(rolledCol) == range: rangeClamped = rngInt(0, range, 0)
        //       else:                       rangeClamped = range  (unchanged)
        int absCol = (rolledCol < 0) ? -rolledCol : rolledCol;
        int rangeClamped = range;

        bool branchA = (rolledCol <  1 || rolledRow < 51) &&
                       (rolledCol >= 0 || rolledRow > 50);

        if (branchA) {
            rangeClamped = range - absCol;
        } else if (absCol == range) {
            rangeClamped = rngInt(0, range, 0);
        }

        int finalRow = rangeClamped * signedRow;

        // FUN_1000175f0: write the rolled level-exit grid position + place
        // the exit / key cells + configure exitTileIcon.
        placeExitAndKeys(rolledCol, finalRow);

        // FUN_100017794: keysRequired = clamp(worldLevelIndex + 1, 0, 4),
        // then size + position each lock icon below exitTileIcon. on level 1
        // this gives keysRequired = 2.
        setExitKeysRequired(worldLevelIndex + 1);

        // FUN_1000178fc: walk page list (empty at level start), seed
        // keysCollected, then flip lock UV + exitTileIcon UV. on level 1
        // both old/new keysCollected are 0, so no locked->unlocked chime.
        recomputeExitKeysCollected();
    }

    // 7.25 reset the exit-arrow visibility (+0x9C60) and fade timer
    // (+0x9C64). matches FUN_1000165e8's `*(this+0x9c60) = 0; *(this+0x9c64) = 0`.
    exitArrowVisible = false;
    exitArrowFade    = 0.0f;

    // 7.26 pop dream-snippet banner seed (= binary's FUN_100026e50).
    // draw-without-replacement bag: when the pool empties, history clears
    // and the pool refills 1..N-1. only runs past level 1; level 1 uses
    // the default seed 0 (which the snippet table puts as "It always
    // starts like this").
    int64_t bannerSeed = 0;

    if (worldLevelIndex != 1) {

        if (animBannerSeedPool.empty()) {
            animBannerSeedHistory.clear();
            const int snippetCount =
                static_cast<int>(DreamSnippets::table().size());

            for (int i = 1; i < snippetCount; i++) {
                animBannerSeedPool.insert(i);
            }
        }

        const int poolIdx =
            rngInt(0, static_cast<int>(animBannerSeedPool.size()) - 1, 0);
        auto it = std::next(animBannerSeedPool.begin(), poolIdx);
        bannerSeed = *it;
        animBannerSeedPool.erase(it);
        animBannerSeedHistory.push_back(bannerSeed);
    }

    // 7.27 dispatch to AnimationController (= binary's FUN_100039018).
    // anchor pair: X centered (0), Y biased upward by a quarter of the
    // virtual height plus a small HUD-state-dependent offset.
    {
        constexpr float kAnchorYNoEvents   = 0.03125f;    // DAT_100059ef0[1]
        constexpr float kAnchorYWithEvents = 0.1015625f;  // DAT_100059ef0[0]
        const float baseY = (hud.countEventsHeld() == 0)
                            ? kAnchorYNoEvents
                            : kAnchorYWithEvents;
        const float anchor[2] = {
            0.0f,
            baseY + Renderer::getVirtualHeight() * -0.25f,
        };
        animController.init(static_cast<uint32_t>(bannerSeed), anchor);
    }

    // 7.28 finalize display state (port of FUN_100017b44).
    // the binary checks several conditions and writes 0 or 1 to HUD+0x958
    // (= GameplayHUD::conditionalFlag). with pageCount == 0 the outer
    // condition fails, so the flag clears to 0.
    hud.conditionalFlag = false;

    // 7.29 HUD post-tile positioning (port of FUN_10000bab8). configures
    // the conditionalIcon at HUD+0x880 with its standard UV / size / pos.
    hud.setConditionalIcon(GameplayHUD::ConditionalIconState::Default);

    // 7.30 X-button refresh (port of FUN_100017c04). resets visible / pressed
    // flags, restores quad tint, then on a non-empty page list scans for a
    // dead-end target hex. with pageCount == 0 it exits early.
    tryShowXButton();

    // 7.31 achievement level-start fan-out (= binary's FUN_10004da20). pure
    // achievement dispatch, not music despite the name. branches on level
    // number and difficulty; AFreshStart fires unconditionally when the
    // player begins the level at 1 HP.
    {
        AchievementTracker& tracker = getGame()->achievementTracker();
        const int level = worldLevelIndex;
        const int diff  = (int)worldIndex;   // 0=easy, 1=normal, 2=hard

        if (level == 2) {
            tracker.increment(AchievementId::ItAlwaysStartsLikeThis);

            // "The Easy Way": reach world 2 on Hard with no damage taken
            // this run. damagedThisRun is cleared on new-run start, set
            // on first damage via setHP's notePlayerDamaged call, and
            // persists across world transitions within a run.
            if (diff == 2 && tracker.damagedThisRun == 0) {
                tracker.increment(AchievementId::TheEasyWay);
            }
        } else if (level == 3) {

            if (diff == 2) {
                tracker.increment(AchievementId::InTooDeep);
            } else if (diff == 1) {
                tracker.increment(AchievementId::IntoTheDark);
            }

            // "Scavenger Hunt": fires only when reaching world 3 on
            // normal / hard, with 5+ items found this run.
            if ((diff == 1 || diff == 2) && itemsFound >= 5) {
                tracker.increment(AchievementId::ScavengerHunt);
            }
        } else if (level == 5) {

            if (diff == 1) {
                tracker.increment(AchievementId::ThroughTheMirrorMaze);
            } else if (diff == 0) {
                tracker.increment(AchievementId::FirstSteps);
            }
        } else if (level == 7 && diff == 1) {
            tracker.increment(AchievementId::GazeIntoTheAbyss);
        }

        // "A Fresh Start": always checked, regardless of level / difficulty.
        if (playerSystem.currentHealth == 1) {
            tracker.increment(AchievementId::AFreshStart);
        }
    }

    SDL_Log("GameBoard::initLevelContent() complete (level=%d, reserve=%lld, tileVariant=%d)",
            worldLevelIndex, (long long)tileReserve.size(), gridLayout);
}

// ============================================================================
// Phase 3, tile generators + RNG helpers (ports of FUN_100017fb8, 10001809c,
// 100018180, 1000203d4, 100020450, 10001a8fc, 1000201e8, 1000268c8, 10001ffd4,
// 100020254). each is decompile-driven; deviations carry explicit TODOs.
// ============================================================================

// FUN_1000201e8, walk rack[0..4]; on the first slot whose snag type matches,
// return FUN_100013410 (= the SnagContent if alive, else null).
SnagContent* GameBoard::findSnagInRack(int snagType) {
    unsigned long uVar3 = 0;

    while (true) {
        TileObject* tile = rack[uVar3];

        if (tile != nullptr && tile->getSnagType() == snagType) {
            break;
        }

        uVar3 = uVar3 + 1;

        if (uVar3 > 4) {
            return nullptr;
        }
    }

    return rack[uVar3]->getSnagIfAlive();
}

// FUN_1000268c8, walk page list (sentinel at +0x1A8) forward via .next.
// matches when a tile's snagType (FUN_1000133f0) equals snagType, returns
// the matched tile's SnagContent (FUN_100013410, alive-gated). when the
// list is empty or no tile matches, returns null.
SnagContent* GameBoard::findSnagInPages(int snagType) {

    for (TileObject* tile : pageList) {

        if (tile && tile->getSnagType() == snagType) {
            return tile->getSnagIfAlive();
        }
    }

    return nullptr;
}

// FUN_10001a8fc, true if findSnagInRack or findSnagInPages hits.
bool GameBoard::hasSnagInBoard(int snagType) {

    if (findSnagInRack(snagType) != nullptr) {
        return true;
    }

    return findSnagInPages(snagType) != nullptr;
}

// FUN_100026750, first try rack, then page list. used wherever the binary
// needs the actual snag pointer (not just a boolean), to read its tileParent
// or stats.
SnagContent* GameBoard::findSnagInRackOrPage(int snagType) {
    SnagContent* snag = findSnagInRack(snagType);

    if (snag) {
        return snag;
    }

    return findSnagInPages(snagType);
}

// FUN_100020254, roll a content type 2..0x75 for a fresh rack tile. iterates
// the full type range, applies per-range filtering rules (skip / HUD-gated /
// include), excludes anything in excludedSnagTypes (+0x9E00), then
// RNG-picks from the survivors.
int GameBoard::rollSnagType() {
    std::vector<int> candidates;

    for (int type = 2; type <= 0x75; type++) {
        bool include = false;

        if (type < 0x1e) {
            // 2..0x1D: include all except 0x12.
            include = (type != 0x12);
        } else if (type < 0x48) {
            // 0x1E..0x47:
            //   0x22 -> skip
            //   0x1E or 0x25 -> HUD-gated
            //   else -> include
            if (type == 0x22) {
                include = false;
            } else if (type == 0x1e || type == 0x25) {
                include = (hud.countEventsHeld() != 0);
            } else {
                include = true;
            }
        } else if (type - 0x48 < 0x25) {
            // 0x48..0x6C: bitmask filter on (type - 0x48).
            uint64_t bit = 1ULL << ((type - 0x48) & 0x3f);

            if (bit & 0x1000000069ULL) {
                include = false;            // skip set: 0x48,0x4B,0x4D,0x4E,0x68
            } else if (bit & 0xc0000000ULL) {
                include = (hud.countEventsHeld() != 0);   // HUD-gated: 0x66,0x67
            } else {
                include = true;
            }
        } else {
            // 0x6D..0x75: include all.
            include = true;
        }

        // filter against the run's excluded-snag-types set (= binary's
        // FUN_100028178 against gb+0x9E00). empty until startRun seeds it.
        if (include && excludedSnagTypes.find(type) == excludedSnagTypes.end()) {
            candidates.push_back(type);
        }
    }

    // binary doesn't bounds-check candidates.empty(); falls through with empty
    // vector -> rngInt(0, -1, 4) -> undefined. in practice the candidate list
    // is large (~100 types) so it never empties out.
    if (candidates.empty()) {
        return 2;   // safety fallback for the impossible empty case.
    }

    int idx = rngInt(0, static_cast<int>(candidates.size()) - 1, 4);
    return candidates[idx];
}

namespace {

// FUN_100013c78, build a vector of grid-pool indices that pass two filters:
//   (1) outer:  exitCount < 3 (i.e., exitCount == 2)
//   (2) inner:  abs arc-distance between exits[0] and exits[1] <= tolerance.
//               score = (e1 - e0) if (e1 - e0) < 4 else (e1 - e0 - 6); abs(score).
// the binary stores the result in a global static vector; we use a local to
// avoid global state.
void buildGridIdxPool(int gridLayout, int tolerance, std::vector<int>& out) {
    out.clear();

    for (int i = 0; i < 24; i++) {
        const GridPoolEntry& e = kGridPoolTable[gridLayout][i];

        if (e.exitCount >= 3) {
            continue;
        }

        int e0 = e.exits[0];
        int e1 = e.exits[1];
        int lo = (e0 <= e1) ? e0 : e1;
        int hi = (e1 >= e0) ? e1 : e0;

        // score = max - (min + 6) when (max-min) >= 4, else max - min.
        int subtractFrom = (hi - lo < 4) ? lo : (lo + 6);
        int diff = hi - subtractFrom;
        int absDiff = (diff < 0) ? -diff : diff;

        if (absDiff <= tolerance) {
            out.push_back(i);
        }
    }
}

} // namespace

// FUN_1000203d4, roll a grid-idx 0..23 for a fresh tile. when snag kind 0x40
// (Ignorance) is on the board and the curve-bias filter has any survivors,
// pick one of those; otherwise uniform RNG over 0..23.
int GameBoard::rollContentGridIdx() {

    if (hasSnagInBoard(0x40)) {
        std::vector<int> filtered;
        buildGridIdxPool(gridLayout, 2, filtered);

        if (!filtered.empty()) {
            int idx = rngInt(0, static_cast<int>(filtered.size()) - 1, 4);
            return filtered[idx];
        }
    }

    return rngInt(0, 23, 4);
}

// FUN_100020450, roll a magnitude for a content tile. switch on contentType:
//   2 -> ATK tile, magnitude rolled from playerSystem.statRanges[0] (ATK).
//   3 -> DEF tile, magnitude rolled from playerSystem.statRanges[1] (DEF).
//   5 -> CTRL tile, magnitude is 1 (default) or 2 (perkLevel(0xf) raises the
//       odds: lvl 1 = 25%, lvl 2 = 50%, lvl 3 = 75%, lvl 4 = always 2).
//       with no perks owned this returns 1.
//   6 -> HP tile, magnitude rolled from playerSystem.statRanges[2] (CTRL Items
//       scale how much HP the tile heals).
//   default -> 1.
int GameBoard::rollContentMagnitude(uint32_t contentType) {

    switch (contentType) {
    case 2:
        return rngInt(playerSystem.statRanges[0].lo,
                      playerSystem.statRanges[0].hi, 4);

    case 3:
        return rngInt(playerSystem.statRanges[1].lo,
                      playerSystem.statRanges[1].hi, 4);

    case 5: {
        // content type 5 = Control ({C}) tile. perkLevel(0xf) returns the
        // level of the "Controlled" perk (perkType 0xf): each level raises
        // the chance of drawing a 2-{C} tile instead of 1 (perk_table.h
        // PERK_15_LEVELS = 25% / 50% / 75% / 100%). 0 = perk not owned.
        int controlledLevel = playerSystem.perkLevel(0xf);

        // FUN_100020450 case 5 inner switch. the binary's flag expansion
        // (bVar3 || bVar2 != bVar1) is `rng == T || rng < T`, i.e. rng <= T.
        switch (controlledLevel) {
        case 1: return (rngInt(0, 100, 4) <= 25) ? 2 : 1;
        case 2: return (rngInt(0, 100, 4) <= 50) ? 2 : 1;
        case 3: return (rngInt(0, 100, 4) <= 75) ? 2 : 1;
        case 4: return 2;
        default: return 1;
        }
    }

    case 6:
        return rngInt(playerSystem.statRanges[2].lo,
                      playerSystem.statRanges[2].hi, 4);

    default:
        return 1;
    }
}

// FUN_100017f28, seed pickupSnagThreshold from levelTurnCount * scale + RNG.
void GameBoard::seedPickupSnagThreshold() {
    constexpr float SCALE_DEFAULT     = -0.07999999821186066f;  // DAT_100059f3c
    constexpr float SCALE_FIRST_LEVEL = -0.11999999731779099f;  // DAT_100059f40

    float scale = (worldLevelIndex == 1) ? SCALE_FIRST_LEVEL : SCALE_DEFAULT;

    int base = std::clamp(static_cast<int>(static_cast<float>(levelTurnCount) * scale + 20.0f),
                          2, 0x14);
    int jitter = rngInt(-2, 2, 0);
    int delta = base + jitter;

    if (delta < 2) {
        delta = 1;
    }

    pickupSnagThreshold = delta + levelTurnCount;
}

// FUN_100017fb8, push a content tile onto the reserve queue (newest end).
TileObject* GameBoard::pushReserveTile(uint32_t contentType, uint32_t colorParam) {
    TileObject* tile = new TileObject();
    tile->init();

    int gridIdx = rollContentGridIdx();
    int magnitude = rollContentMagnitude(contentType);
    tile->setVisual(gridLayout, gridIdx, contentType, magnitude);

    // push-back (newest end).
    tileReserve.emplace_back(tile, colorParam);

    return tile;
}

// FUN_10001ffd4, pop a tile from the reserve queue (matching predicate),
// or roll a fresh one when the queue is exhausted / no candidate matches.
TileObject* GameBoard::rollRackTile(uint32_t flag) {

    // walk from front (oldest) toward back (newest).
    for (auto it = tileReserve.begin(); it != tileReserve.end(); ++it) {
        // pop predicate: signed colorParam < 0 and (flag&1 != 0 or snag not alive).
        bool colorNeg = static_cast<int32_t>(it->colorParam) < 0;
        bool flagBit  = (flag & 1) != 0;
        bool snagDead = (it->tile->getSnagIfAlive() == nullptr);

        if (colorNeg && (flagBit || snagDead)) {
            TileObject* tile = it->tile;
            tileReserve.erase(it);
            return tile;
        }
    }

    // reserve queue exhausted (or no eligible candidate). build a fresh tile.

    // FUN_1000201e8(param_1, 0x27): find rack snag of type 0x27.
    SnagContent* rackSnag27 = findSnagInRack(0x27);

    uint32_t uVar1;

    if (levelTurnCount < pickupSnagThreshold) {

        if (rackSnag27 == nullptr ||
            !rackSnag27->consumedFlag) {
            // FUN_1000268c8(param_1, 0x54): find page snag of type 0x54.
            SnagContent* pageSnag54 = findSnagInPages(0x54);

            if (pageSnag54 == nullptr) {
                uVar1 = static_cast<uint32_t>(weightedRollTileType(tileWeightPool, 4));

                if (uVar1 == 1 && flag != 1) {
                    // re-roll while we keep getting 1.
                    do {
                        uVar1 = static_cast<uint32_t>(weightedRollTileType(tileWeightPool, 4));
                    } while (uVar1 == 1);
                }
            } else {
                uVar1 = 2;
            }
        } else {
            uVar1 = 0;
            rackSnag27->consumedFlag = false;   // clear the consumed flag
        }
    } else {
        uVar1 = 1;
    }

    // allocate the fresh tile.
    TileObject* tile = new TileObject();
    tile->init();

    if (uVar1 == 1) {
        // snag-tile path. content type rolled from rollSnagType, with
        // a special-case branch for type 0x2c that pushes a type-8 reserve
        // tile as a side effect.
        int contentType;

        if (levelTurnCount < pickupSnagThreshold) {
            contentType = 1;
        } else {
            seedPickupSnagThreshold();
            contentType = rollSnagType();

            if (contentType == 0x2c) {
                pushReserveTile(8, 0xffffffff);
                contentType = 0x2c;
            }
        }

        int gridIdx = rollContentGridIdx();
        tile->setSnagVisual(gridLayout, gridIdx, static_cast<uint32_t>(contentType),
                            &playerSystem, totalTurnCount, levelTurnCount,
                            static_cast<int>(worldIndex));
    } else {
        // content-tile path.
        int gridIdx = rollContentGridIdx();
        int magnitude = rollContentMagnitude(uVar1);
        tile->setVisual(gridLayout, gridIdx, uVar1, magnitude);
    }

    return tile;
}

// FUN_100018180, fill the rack from slot 0; shifts existing tiles toward
// higher indices. one tile per call. handles snag-effect tile erasures
// (Blinding Light / Loneliness / Zeal) and the Parasite swap-in.
void GameBoard::populateRack() {
    constexpr float RACK_POS_Y_OFF_1     = -0.09843750f;  // DAT_100059f44
    constexpr float RACK_POS_Y_OFF_2     = -0.02031250f;  // DAT_100059f48
    constexpr float RACK_POS_X_BASE      = -0.17500000f;  // DAT_100059f4c
    constexpr float RACK_POS_X_STRIDE    =  0.10937500f;  // DAT_100059f50
    constexpr float RACK_SLIDE_STAGGER   =  0.05000000f;  // DAT_100059f54
    constexpr float RACK_X_PER_SLOT      =  0.1953125f;   // inline literal

    float virtualHeight = Renderer::getVirtualHeight();
    float rackY = virtualHeight + RACK_POS_Y_OFF_1 + RACK_POS_Y_OFF_2;

    // outer: find first empty slot.
    int slotIdx = 0;

    while (slotIdx < 5 && rack[slotIdx] != nullptr) {
        slotIdx++;
    }

    if (slotIdx >= 5) {
        return;   // rack already full, no-op
    }

    // inner: walk slotIdx down to 0. for slotIdx==0 populate; for slotIdx>0
    // shift. each iteration also calls setRackPosition with stagger.
    int stagger = 0;

    while (slotIdx >= 0) {

        if (slotIdx == 0) {
            // populate slot 0 with a fresh roll.
            bool darknessOnBoard = hasSnagInBoard(0x15);

            // pageList-non-empty conditional flag for rollRackTile.
            uint32_t flag = !pageList.empty() ? 1u : 0u;
            TileObject* fresh = rollRackTile(flag);
            rack[0] = fresh;

            fresh->setPosition(RACK_POS_X_BASE, rackY);

            // play snagSpecAppear (sound 0x22) when the fresh tile has an
            // alive snag of any kind != 1 (= not the generic Snag).
            SnagContent* aliveSnag = fresh->getSnagIfAlive();
            int snagType = fresh->getSnagType();

            if (aliveSnag != nullptr && snagType != 1) {
                Game* g = getGame();

                if (g) {
                    g->soundQueue.trigger(0x22);
                }
            }

            // Darkness on board -> push "?" decoration onto the new tile.
            if (darknessOnBoard) {
                fresh->pushDecoration(1, 0, 0);
            }

            // Blinding Light (snag 0x3a) on rack: blanks out HEALTH pickups
            // (content type 6). reroll the new tile until something else.
            if (fresh->getContentType() == 6 && findSnagInRack(0x3a) != nullptr) {
                fresh->erase();
            }

            // Loneliness (snag 0x45) on rack: blanks out generic-Snag tiles
            // (snag kind 1). reroll the new tile if it would draw one.
            if (fresh->getSnagType() == 1 && findSnagInRack(0x45) != nullptr) {
                fresh->erase();
            }

            // Zeal (snag 0x69) on rack: blanks out CONTROL pickups (content
            // type 5). reroll the new tile.
            if (fresh->getContentType() == 5 && findSnagInRack(0x69) != nullptr) {
                fresh->erase();
            }

            // Parasite (rack snag 0x48) or Infestation (board snag 0x47):
            // DEFENCE pickups (content type 3) become Parasite snag tiles.
            // reroll first, then transformToSnag(0x48) to overwrite the tile's
            // snagContent with a Parasite (kind 0x48).
            if (fresh->getContentType() == 3 &&
                (findSnagInRack(0x48) != nullptr || hasSnagInBoard(0x47))) {
                fresh->erase();
                fresh->transformToSnag(0x48, &playerSystem, totalTurnCount,
                                       levelTurnCount, static_cast<int>(worldIndex));
            }
        } else {
            // shift: slot[N] = slot[N-1]
            rack[slotIdx] = rack[slotIdx - 1];
        }

        // setRackPosition for the just-populated / shifted slot.
        float targetX = static_cast<float>(slotIdx) * RACK_X_PER_SLOT + RACK_POS_X_STRIDE;
        float targetPos[2] = { targetX, rackY };
        rack[slotIdx]->setRackPosition(static_cast<float>(stagger) * RACK_SLIDE_STAGGER,
                                       targetPos, true, false);

        slotIdx--;
        stagger++;
    }

    // play "tileDraw" sound (slot 17 = 0x11).
    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x11);
    }
}

// FUN_10001809c, push a snag tile onto the reserve queue. trailing
// FUN_100013410 read is preserved for byte-faithful call sequence; result
// discarded by the binary.
SnagContent* GameBoard::pushReserveSnagTile(uint32_t kind, uint32_t colorParam) {
    TileObject* tile = new TileObject();
    tile->init();

    int gridIdx = rollContentGridIdx();
    // setSnagVisual matches FUN_100012850. binary passes totalTurnCount (+0x20),
    // levelTurnCount (+0x40), worldIndex (+0x8) for the trailing 3 ints.
    tile->setSnagVisual(gridLayout, gridIdx, kind, &playerSystem,
                        totalTurnCount, levelTurnCount, static_cast<int>(worldIndex));

    tileReserve.emplace_back(tile, colorParam);

    // matches the binary's `b 0x100013410` tail call: return the new
    // snag's SnagContent pointer (or null if not alive, which shouldn't
    // happen for a freshly-created snag tile).
    return tile->getSnagIfAlive();
}

// ============================================================================
// GameBoard::dirtyXferSnapshot, port of FUN_1000269b8.
//
// pack the live GameBoard run-state into the slot-0 GameSnapshot at
// Game+0x2E348. called from Game::update's mid-run save-trigger gate
// (this->dirty != 0 && this->state == 1 && !eventChoicePanel.visible)
// when the player completes a save-worthy action. clears this->dirty
// on entry; the caller sets Game::saveSlot0Dirty so the save framework
// picks up the snapshot on the next flushDirty pass.
//
// the builder owns these binary helpers as anonymous-namespace statics:
//   FUN_100014d70 -> extractTileContent        (TileContent -> 2 ints)
//   FUN_10003e438 -> extractSnagContent        (SnagContent -> 4 or 6 ints)
//   FUN_100013dd8 -> extractTileToSnapshot     (TileObject -> 3 sub-blocks)
//   FUN_100056ba8 -> snapshotPlayerSystem      (PlayerSystem header + items)
//   FUN_100009974 -> snapshotNemesis           (5 nemesis fields)
//   FUN_10000d9c4 -> snapshotHud               (xp/control + eventTray)
//   FUN_10003b614 -> snapshotHexMap            (kind > 2 filter)
// ============================================================================

namespace {

// FUN_100014d70, TileContent (kind, magnitude) extract.
// writes out[0] = tc.type (+0x134), out[1] = tc.displayedMagnitude (+0x13C).
void extractTileContent(const TileContent& tc, uint32_t out[2]) {
    out[0] = static_cast<uint32_t>(tc.type);
    out[1] = static_cast<uint32_t>(tc.displayedMagnitude);
}

// FUN_10003e438, SnagContent (type, hp, atk, def, extras...) extract.
// always writes out[0..3]. out[4]/out[5] are conditionally written based
// on kind: kind 6 writes both as full int; kind 15 (0xF) writes out[4]
// as full int; kinds 17 (0x11) / 39 (0x27) write out[4] as a byte read.
// caller is responsible for zero-initing the buffer before the call so
// untouched slots stay 0.
//
// the "full int" reads cover the 4 bytes at sc+0x490 (= consumedFlag byte
// + pad491[3]). Reluctance (kind 0xF) writes 2 to this slot via 4-byte
// cast in the binary; Obsession (kind 6) writes 0x64 via an 8-byte
// combined store that also covers obsessionCount at +0x494.
void extractSnagContent(const SnagContent& sc, uint32_t out[6]) {
    out[0] = static_cast<uint32_t>(sc.type);
    out[1] = static_cast<uint32_t>(sc.hp);
    out[2] = static_cast<uint32_t>(sc.atk);
    out[3] = static_cast<uint32_t>(sc.def);

    const int kind = sc.type;

    if (kind == 6) {
        uint32_t consumedFull;
        std::memcpy(&consumedFull, &sc.consumedFlag, sizeof(consumedFull));
        out[4] = consumedFull;
        out[5] = static_cast<uint32_t>(sc.obsessionCount);
        return;
    }

    if (kind == 0xF) {
        uint32_t consumedFull;
        std::memcpy(&consumedFull, &sc.consumedFlag, sizeof(consumedFull));
        out[4] = consumedFull;
        return;
    }

    if (kind == 0x11 || kind == 0x27) {
        out[4] = static_cast<uint32_t>(sc.consumedFlag);
        return;
    }
    // all other kinds: out[4]/out[5] stay 0 (caller pre-zeroed).
}

// FUN_100013dd8, extract a TileObject into the 3-block snapshot layout:
//   tileFields3[0]  = tile.gridIdx       (+0xDC)
//   *tileMirror     = tile.mirror byte    (+0xE0)
//   tileFields3[2]  = tile.rotationStep  (+0xE4)
//   contentFields2  = extractTileContent(*tile.content)    if present, else {0,0}
//   snagFields6     = extractSnagContent(*tile.snagContent) if present, else 0s
//   decorationsOut  = (kind, value) of each suppressed==0 TileDecoration in the
//                     tile's decorList. nullable; placedTiles pass nullptr to
//                     skip the decoration walk.
//
// our typed access (gridIdx/mirror/rotationStep) lands at the same bytes as
// the binary's pointer arithmetic: tileFields3[0] = gridIdx u32, *tileMirror
// = mirror u8 (offset 4 in a u32-array view), tileFields3[2] = rotationStep
// u32. the destination in RackTileSnapshot / ReserveItemSnapshot has matching
// offsets.
void extractTileToSnapshot(TileObject& tile,
                           uint32_t tileFields3[3], bool& tileMirror,
                           uint32_t contentFields2[2],
                           uint32_t snagFields6[6],
                           std::vector<int64_t>* decorationsOut) {
    tileFields3[0] = static_cast<uint32_t>(tile.gridIdx);
    tileMirror     = tile.mirror;
    tileFields3[2] = static_cast<uint32_t>(tile.rotationStep);

    if (tile.content != nullptr && tile.content->visible) {
        extractTileContent(*tile.content, contentFields2);
    } else {
        contentFields2[0] = 0;
        // contentFields2[1] left untouched per binary (extraValue gated
        // behind contentType == 0; loader/encoder respect the gate too).
    }

    if (tile.snagContent != nullptr && tile.snagContent->visible) {
        extractSnagContent(*tile.snagContent, snagFields6);
    } else {
        snagFields6[0] = 0;
    }

    if (decorationsOut == nullptr) {
        return;
    }

    decorationsOut->clear();

    // walk tile.decorList from decorListNext (oldest) forward via .next,
    // ending at &tile.decorListPrev (= sentinel address). push each entry
    // whose `suppressed` byte is 0 (= actively hiding the tile content).
    const TileDecoration* sentinel =
        reinterpret_cast<const TileDecoration*>(&tile.decorListPrev);
    TileDecoration* node = tile.decorListNext;

    while (node != sentinel) {

        if (!node->suppressed) {
            const uint32_t kindU  = static_cast<uint32_t>(node->kind);
            const uint32_t valueU = static_cast<uint32_t>(node->value);
            const int64_t packed = static_cast<int64_t>(
                                       static_cast<uint64_t>(kindU) |
                                       (static_cast<uint64_t>(valueU) << 32));
            decorationsOut->push_back(packed);
        }

        node = node->next;
    }
}

// FUN_100033750, Item (subType, cosmeticNameIdx, atk, def, hp, [SPA pairs]).
//
// binary writes out[0..4] from item header, then loops 2x SPA slots
// (stride 0xE0) writing out[5..8] as (type, value) pairs. our typed
// access reads Item header fields + Item.abilities[0..1] directly.
void extractItemToSnapshot(const Item& item, StatBlockSnapshot& sb) {
    sb.itemSubType         = static_cast<uint32_t>(item.subType);
    sb.itemCosmeticNameIdx = static_cast<uint32_t>(item.cosmeticNameIdx);
    sb.itemAtk             = static_cast<uint32_t>(item.atk);
    sb.itemDef             = static_cast<uint32_t>(item.def);
    sb.itemHp              = static_cast<uint32_t>(item.hp);
    sb.spa0Type            = static_cast<uint32_t>(item.abilities[0].abilityType);
    sb.spa0Value           = static_cast<uint32_t>(item.abilities[0].abilityVal);
    sb.spa1Type            = static_cast<uint32_t>(item.abilities[1].abilityType);
    sb.spa1Value           = static_cast<uint32_t>(item.abilities[1].abilityVal);
}

// FUN_100056ba8, PlayerSystem snapshot. fills snap.{characterIndex,
// attack, defence, currentHealth, baseATK, baseDEF, baseHP},
// snap.statBlocks[0..2] from baseItems[0..2], and snap.perks from the
// perks vector. xpReceivedTotal / controlReceivedTotal are written by
// snapshotHud immediately after (binary calls in this order).
void snapshotPlayerSystem(const PlayerSystem& ps, GameSnapshot& snap) {
    snap.characterIndex       = static_cast<uint32_t>(ps.characterIndex);
    snap.xpReceivedTotal      = 0;   // overwritten by snapshotHud
    snap.controlReceivedTotal = 0;   // overwritten by snapshotHud
    snap.attack               = static_cast<uint32_t>(ps.attack);
    snap.defence              = static_cast<uint32_t>(ps.defence);
    snap.currentHealth        = static_cast<uint32_t>(ps.currentHealth);
    snap.baseATK              = static_cast<uint32_t>(ps.baseATK);
    snap.baseDEF              = static_cast<uint32_t>(ps.baseDEF);
    snap.baseHP               = static_cast<uint32_t>(ps.baseHP);

    for (int s = 0; s < 3; ++s) {
        const Item* item = ps.baseItems[s];

        if (item != nullptr) {
            extractItemToSnapshot(*item, snap.statBlocks[s]);
        } else {
            snap.statBlocks[s] = StatBlockSnapshot{};
        }
    }

    // binary doesn't null-check perks (FUN_1000423f8 derefs unconditionally);
    // we trust the same invariant.
    snap.perks.resize(ps.perks.size());

    for (size_t i = 0; i < ps.perks.size(); ++i) {
        const Perk* perk = ps.perks[i];
        const uint64_t packed =
            static_cast<uint64_t>(static_cast<uint32_t>(perk->perkType)) |
            (static_cast<uint64_t>(static_cast<uint32_t>(perk->perkLevel)) << 32);
        snap.perks[i] = static_cast<int64_t>(packed);
    }
}

// FUN_100009974, NemesisRenderable snapshot. 5 fields.
void snapshotNemesis(const NemesisRenderable& nem, GameSnapshot& snap) {
    snap.nemesisVisible = nem.visible;
    snap.nemesisGridCol = nem.nemesisGridCol;
    snap.nemesisGridRow = nem.nemesisGridRow;
    snap.nemesisLevel   = static_cast<uint32_t>(nem.nemesisLevel);
    snap.nemesisXP      = static_cast<uint32_t>(nem.nemesisXP);
}

// FUN_10000d9c4, GameplayHUD snapshot. overwrites the two zero slots
// snapshotPlayerSystem left at snap.xpReceivedTotal / .controlReceivedTotal,
// then fills snap.eventTraySnapshot from the non-null eventTray slots
// (FUN_10002a324 per entry = EventSlot.eventType + .currentCharges).
void snapshotHud(const GameplayHUD& hud, GameSnapshot& snap) {
    snap.xpReceivedTotal      = static_cast<uint32_t>(hud.xpReceivedTotal);
    snap.controlReceivedTotal = static_cast<uint32_t>(hud.controlReceivedTotal);

    int liveCount = 0;

    for (int i = 0; i < 4; ++i) {

        if (hud.eventTray[i].slotPtr != nullptr) {
            ++liveCount;
        }
    }

    snap.eventTraySnapshot.clear();
    snap.eventTraySnapshot.reserve(liveCount);

    for (int i = 0; i < 4; ++i) {
        const EventSlot* slot = hud.eventTray[i].slotPtr;

        if (slot == nullptr) {
            continue;
        }

        const uint64_t packed =
            static_cast<uint64_t>(static_cast<uint32_t>(slot->eventType)) |
            (static_cast<uint64_t>(static_cast<uint32_t>(slot->currentCharges)) << 32);
        snap.eventTraySnapshot.push_back(static_cast<int64_t>(packed));
    }
}

// FUN_10003b614, HexMap snapshot. filters to cells with kind > 2 (= the
// "interesting" map state); per cell pushes (col, row, kind, fadeTimer).
void snapshotHexMap(const HexMap& hexMap, std::vector<HexMapEntry>& out) {
    out.clear();

    for (const auto& [key, cell] : hexMap.cells) {

        if (static_cast<uint32_t>(cell.kind) <= 2) {
            continue;
        }

        HexMapEntry e{};
        e.col       = key.first;
        e.row       = key.second;
        e.kind      = static_cast<uint32_t>(cell.kind);
        e.fadeTimer = static_cast<uint32_t>(cell.fadeTimer);
        out.push_back(e);
    }
}

}  // anonymous namespace

void GameBoard::dirtyXferSnapshot(GameSnapshot& snap) {
    // 1. clear dirty trigger on entry, matches the binary's first store.
    this->dirty = false;

    // 2. 7-int counter block at +0x20..+0x3B (totalTurnCount through
    //    eventsFired). binary uses 4 overlapping 8-byte stores; we use
    //    direct field copies because our struct is typed.
    snap.totalTurnCount       = static_cast<uint32_t>(this->totalTurnCount);
    snap.worldLevelIndex      = static_cast<uint32_t>(this->worldLevelIndex);
    snap.snagsDefeated        = static_cast<uint32_t>(this->snagsDefeated);
    snap.specialSnagsDefeated = static_cast<uint32_t>(this->specialSnagsDefeated);
    snap.levelsGained         = static_cast<uint32_t>(this->levelsGained);
    snap.itemsFound           = static_cast<uint32_t>(this->itemsFound);
    snap.eventsFired          = static_cast<uint32_t>(this->eventsFired);

    // 3. worldIndex + tutorialFlag (single-byte writes in binary).
    snap.worldIndex   = this->worldIndex;
    snap.tutorialFlag = (this->tutorialFlag != 0);

    // 4. copy the 24 tutorial hint "shown" markers (dialogPanel.hintShown,
    //    = gb+0x63F8) into the snapshot.
    std::memcpy(snap.hintFlags, dialogPanel.hintShown, sizeof(snap.hintFlags));

    // 5. simple field copies.
    snap.gridLayout          = static_cast<uint32_t>(this->gridLayout);
    snap.exitCol             = this->exitCol;
    snap.exitRow             = this->exitRow;
    snap.keysRequired        = static_cast<uint32_t>(this->keysRequired);
    snap.levelTurnCount      = static_cast<uint32_t>(this->levelTurnCount);
    snap.pickupSnagThreshold = static_cast<uint32_t>(this->pickupSnagThreshold);

    // 6. vector clones; both are std::vector assignments. binary calls
    //    FUN_100028b48 / FUN_100028648 (vector::assign(begin, end)); the
    //    C++ operator= is equivalent.
    snap.variantsUsed          = this->variantsUsed;
    snap.animBannerSeedHistory = this->animBannerSeedHistory;

    // 7. 5 rack-tile snapshots. each rack[i] either is null (zero the
    //    landmark fields) or extracts via extractTileToSnapshot. binary
    //    writes 4 fields when null: contentType (+0x88), flagA (+0x8C),
    //    variant+extraFlag combined u64 (+0x90), snagType (+0x9C). our
    //    typed RackTileSnapshot zero-inits via value-init.
    for (int s = 0; s < 5; ++s) {
        RackTileSnapshot& r = snap.rackTiles[s];
        TileObject* tile = this->rack[s];

        if (tile == nullptr) {
            // landmark zero pattern: gridIdx, mirror, rotationStep,
            // contentType, snagKind, decorations vec.
            r.gridIdx      = 0;
            r.mirror       = false;
            // rotationStep + contentType form the u64 at rack+8 in the
            // binary; both get zeroed.
            r.rotationStep = 0;
            r.contentType  = 0;
            r.snagKind     = 0;
            r.decorations.clear();
            continue;
        }

        uint32_t tileFields[3] = { 0, 0, 0 };
        uint32_t contentFields[2] = { 0, 0 };
        uint32_t snagFields[6]    = { 0, 0, 0, 0, 0, 0 };

        extractTileToSnapshot(*tile, tileFields, r.mirror,
                              contentFields, snagFields,
                              &r.decorations);

        r.gridIdx            = tileFields[0];
        r.rotationStep       = tileFields[2];
        r.contentType        = contentFields[0];
        r.contentMagnitude   = static_cast<int32_t>(contentFields[1]);
        r.snagKind           = snagFields[0];
        r.snagHp             = snagFields[1];
        r.snagAtk            = snagFields[2];
        r.snagDef            = snagFields[3];
        r.snagConsumedFlag   = static_cast<int32_t>(snagFields[4]);
        r.snagObsessionCount = static_cast<int32_t>(snagFields[5]);
    }

    // 8. reserveItems list snapshot. walk the std::list from front
    //    (oldest) to back (newest). per entry: capture colorParam +
    //    extract the tile fields.
    snap.reserveItems.clear();
    snap.reserveItems.reserve(this->tileReserve.size());

    for (const TileReserveEntry& entry : this->tileReserve) {
        snap.reserveItems.emplace_back();
        ReserveItemSnapshot& e = snap.reserveItems.back();

        if (entry.tile != nullptr) {
            uint32_t tileFields[3]    = { 0, 0, 0 };
            uint32_t contentFields[2] = { 0, 0 };
            uint32_t snagFields[6]    = { 0, 0, 0, 0, 0, 0 };

            extractTileToSnapshot(*entry.tile, tileFields, e.mirror,
                                  contentFields, snagFields,
                                  /*decorationsOut=*/nullptr);

            e.gridIdx            = tileFields[0];
            e.rotationStep       = tileFields[2];
            e.contentType        = contentFields[0];
            e.contentMagnitude   = static_cast<int32_t>(contentFields[1]);
            e.snagKind           = snagFields[0];
            e.snagHp             = snagFields[1];
            e.snagAtk            = snagFields[2];
            e.snagDef            = snagFields[3];
            e.snagConsumedFlag   = static_cast<int32_t>(snagFields[4]);
            e.snagObsessionCount = static_cast<int32_t>(snagFields[5]);
        }

        // colorParam from entry: encoder will truncate to its low byte
        // on disk; we keep the full 32 bits in memory so a future codepath
        // that reads the un-saved form sees what the binary would.
        e.listSlotIndex = static_cast<int32_t>(entry.colorParam);
    }

    // 9. placedTiles list snapshot + placedContentMap + placedSnagMap fill.
    //    walk pageList front (oldest) -> back (newest). per tile: copy
    //    (col,row), extract tile fields, populate maps with current
    //    index as key when the extracted TileContent.type / SnagContent.type
    //    are non-zero.
    snap.placedTiles.clear();
    snap.placedTiles.resize(this->pageList.size());
    snap.placedContentMap.clear();
    snap.placedSnagMap.clear();

    {
        int idx = 0;

        for (TileObject* tile : this->pageList) {
            PlacedTileSnapshot& p = snap.placedTiles[static_cast<size_t>(idx)];
            p.col = tile->gridCol;
            p.row = tile->gridRow;

            uint32_t tileFields[3]    = { 0, 0, 0 };
            uint32_t contentFields[2] = { 0, 0 };
            uint32_t snagFields[6]    = { 0, 0, 0, 0, 0, 0 };

            extractTileToSnapshot(*tile, tileFields, p.mirror,
                                  contentFields, snagFields,
                                  /*decorationsOut=*/nullptr);

            p.gridIdx      = tileFields[0];
            p.rotationStep = tileFields[2];

            // TileContent populated -> placedContentMap[idx] = (type, magnitude).
            if (contentFields[0] != 0) {
                const uint64_t packed = static_cast<uint64_t>(contentFields[0]) |
                                        (static_cast<uint64_t>(contentFields[1]) << 32);
                snap.placedContentMap[idx] = packed;
            }

            // SnagContent populated -> placedSnagMap[idx] = PlacedSnagFields.
            if (snagFields[0] != 0) {
                PlacedSnagFields v{};
                v.kind   = snagFields[0];
                v.hp     = snagFields[1];
                v.atk    = snagFields[2];
                v.def    = snagFields[3];
                v.extra0 = snagFields[4];
                v.extra1 = snagFields[5];
                snap.placedSnagMap[idx] = v;
            }

            ++idx;
        }
    }

    // 10. PlayerSystem snapshot (header + 3 statBlocks + perks vector).
    snapshotPlayerSystem(this->playerSystem, snap);

    // 11. Nemesis snapshot (5 fields).
    snapshotNemesis(this->nemesis, snap);

    // 12. HUD snapshot (xp + control + eventTray vector; xp/control
    //     overwrite the two zero slots PlayerSystem left).
    snapshotHud(this->hud, snap);

    // 13. HexMap snapshot (filter: kind > 2).
    snapshotHexMap(this->hexMap, snap.hexMapVec);

    // 14. TileWeightPool clone (std::vector<TileWeightEntry> assign).
    snap.tileWeightPool = this->tileWeightPool;

    // 15. 5 RNG seeds, read the current LCG state of each stream.
    for (uint32_t i = 0; i < 5; ++i) {
        snap.rngSeeds[i] = rngGetSeed(i);
    }
}

// ============================================================================
// GameBoard::restoreFromSnapshot, port of FUN_100016b18 (the inverse of
// dirtyXferSnapshot). called from Game::update's case-1 transition when the
// player taps Start with hasSavedRun != 0. rebuilds the entire live GameBoard
// from the saved GameSnapshot, in the binary's section order:
//   1   tracker.beginSession(0) + achievement banner reset
//   2   basic gb byte/short fields + state machine = idle
//   3   24-byte hint-flags copy
//   4   7-int counter block + level fields
//   5   variantsRemaining = {0..11} minus variantsUsed
//   6   PlayerSystem (stats + 3 baseItems + perks), marshalled here
//   7   5 rack tiles (TileObject::setFromSnapshot)
//   8   placed-tile page list (+ content / snag from the side maps)
//   8b  avatar reposition onto the newest placed tile
//   9   scroll bounds + board-translate seeding
//   10  Nemesis (level / XP / hex placement + facing)
//   11  nemesis eat-cycle clear + detail / dialog panel resets
//   12  HUD (stats + marker banks + event tray)
//   13  HUD discard drain + StatBars hide + tween / action force-complete
//   14  TileWeightPool clone + 5 RNG stream seeds
//   15  discard-slide + reserve queue cleanup, reserve rebuild
//   16  HexMap restore
//   17  level-exit placement + dream-snippet bag + filters + chrome finalize
// ============================================================================

namespace {

// FUN_10003b720, clear gb.hexMap.cells and re-add one cell per entry from
// the snapshot's filtered list. equivalent to the binary's
// `FUN_10003b720(gb+0x96F0, snap+0x328)`.
void restoreHexMapFromSnapshot(HexMap& hexMap,
                               const std::vector<HexMapEntry>& entries) {
    hexMap.cells.clear();

    for (const HexMapEntry& e : entries) {
        hexMap.addCell(e.kind, e.col, e.row, e.fadeTimer);
    }
}

// FUN_100009998, nemesis rehydrate. always restores level/XP; only places
// onto the hex grid when snap.nemesisVisible is set, otherwise clears
// `visible`. facingDirSeed is the caller-computed direction (0..5, as a float
// value) derived from the relative positions of the nemesis and the oldest
// placed tile (see the orchestrator's seedNemesisFacing helper).
void restoreNemesisFromSnapshot(NemesisRenderable& nem,
                                const GameSnapshot& snap,
                                float facingDirSeed) {
    nem.setNemesisLevel(static_cast<int>(snap.nemesisLevel));
    nem.setNemesisXP   (static_cast<int>(snap.nemesisXP));

    if (snap.nemesisVisible) {
        const int64_t coords =
            static_cast<int64_t>(static_cast<uint32_t>(snap.nemesisGridCol)) |
            (static_cast<int64_t>(static_cast<uint32_t>(snap.nemesisGridRow)) << 32);
        nem.placeOnHexGrid(coords, facingDirSeed);
        return;
    }

    nem.visible = false;
}

// compute the integer facing direction (0..5) the Nemesis reloads with.
// placeOnHexGrid converts it to a rotation via (facing - 4) * angleStep, so it
// must match the binary exactly. from FUN_100016b18's inline cascade, which
// compares the oldest placed tile (gb+0x1b0 = pageList.front(), the end the
// Nemesis eats from first) against the saved Nemesis (col, row):
//   tileCol >  nemCol -> (tileRow > nemRow) ? 2 : 1
//   tileCol <  nemCol -> (tileRow < nemRow) ? 5 : 4
//   tileCol == nemCol -> (tileRow < nemRow) ? 0 : 3
// empty page list -> facing 0.
int seedNemesisFacing(const TileObject* oldestPlacedTile,
                      const GameSnapshot& snap) {

    if (oldestPlacedTile == nullptr) {
        return 0;
    }

    const int tileCol = oldestPlacedTile->gridCol;
    const int tileRow = oldestPlacedTile->gridRow;
    const int nemCol  = snap.nemesisGridCol;
    const int nemRow  = snap.nemesisGridRow;

    if (tileCol > nemCol) {
        return (tileRow > nemRow) ? 2 : 1;
    }

    if (tileCol < nemCol) {
        return (tileRow < nemRow) ? 5 : 4;
    }
    // tileCol == nemCol
    return (tileRow < nemRow) ? 0 : 3;
}

// FUN_10004d26c, std::vector<TileWeightEntry> assign. C++ operator=
// covers the same byte-copy + resize behavior.
void restoreTileWeightPoolFromSnapshot(TileWeightPool& dst,
                                       const TileWeightPool& src) {
    dst = src;
}

}  // anonymous namespace

void GameBoard::restoreFromSnapshot(const GameSnapshot& snap,
                                    const std::set<int>& snagFilter,
                                    const std::set<int>& eventFilter) {
    Game* gNow = getGame();

    // ---- 1. AchievementTracker.beginSession(0) + AchievementBanner reset ----
    //
    // matches FUN_100016b18's opening:
    //   FUN_10004d7ec(game + 0x42F8, 0)   // tracker.beginSession(0)
    //   FUN_10004f3ac(gb + 0x9E18)        // achievementBanner.reset()
    // sessionFlag=0 = "saved-game continue, not a fresh run" (preserves
    // damagedThisRun, etc.).

    if (gNow) {
        gNow->achievementTracker().beginSession(0);
    }

    achievementBanner.reset();

    // ---- 2. basic gb byte/short fields ----
    visible          = true;
    scoreRequested   = false;
    exitRequested    = false;
    dirty            = false;
    saveNewSettings  = false;

    worldIndex   = snap.worldIndex;
    tutorialFlag = snap.tutorialFlag ? 1 : 0;

    if (gNow) {
        // matches the binary's settings-struct param at +0xC/+0x10. our
        // initLevel reads the same source via the typed Game accessors;
        // no need for an inline settings pointer.
        seVolume  = gNow->globalSeVolume();
        bgmVolume = gNow->globalBgmVolume();
    }

    state                    = 1;          // game state machine = idle
    snagActivationSuppressed = false;      // gb+0x1C low byte = 0
    flag1D                   = true;       // gb+0x1D high byte = 1
    combatEffectsSuppressed  = false;      // gb+0x1E = 0

    // clear "any hint fired this frame" on restore.
    dialogPanel.anyHintFiredThisFrame = 0;

    // ---- 3. 24-byte hint "shown" markers copy ----
    std::memcpy(dialogPanel.hintShown, snap.hintFlags, sizeof(snap.hintFlags));

    // ---- 4. 7-int counter block at +0x20..+0x3B ----
    totalTurnCount       = static_cast<int>(snap.totalTurnCount);
    worldLevelIndex      = static_cast<int>(snap.worldLevelIndex);
    snagsDefeated        = static_cast<int>(snap.snagsDefeated);
    specialSnagsDefeated = static_cast<int>(snap.specialSnagsDefeated);
    levelsGained         = static_cast<int>(snap.levelsGained);
    itemsFound           = static_cast<int>(snap.itemsFound);
    eventsFired          = static_cast<int>(snap.eventsFired);
    exitReached          = 0;                // gb+0x3C = 0 (per-level state flag)
    levelTurnCount       = static_cast<int>(snap.levelTurnCount);
    pickupSnagThreshold  = static_cast<int>(snap.pickupSnagThreshold);
    gridLayout           = static_cast<int>(snap.gridLayout);

    // ---- 5. variantsRemaining = {0..11} minus variantsUsed ----
    //
    // binary clears variantsRemaining, re-inserts 0..11, then assigns
    // variantsUsed from snap and erases each entry from variantsRemaining.
    variantsRemaining.clear();

    for (int i = 0; i < 12; ++i) {
        variantsRemaining.insert(i);
    }

    variantsUsed = snap.variantsUsed;

    for (int v : variantsUsed) {
        variantsRemaining.erase(v);
    }

    // ---- 6. PlayerSystem restore (FUN_100056c90) ----
    //
    // pulled out before passing to PlayerSystem (keeps PlayerSystem save-agnostic):
    // reset the avatar's MovableActor anim timers, place it at the newest
    // placed tile's hex, reset() to clear items/perks + apply the portrait,
    // then seed the saved stats, rebuild the 3 baseItems via
    // Item::initExplicit + push, and append the saved perks.
    {
        PlayerSystem& ps = playerSystem;

        // newest placed tile (col, row): binary reads the last placedTiles
        // entry; (0, 0) when the run hasn't placed any tiles yet.
        int newestCol = 0;
        int newestRow = 0;

        if (!snap.placedTiles.empty()) {
            newestCol = snap.placedTiles.back().col;
            newestRow = snap.placedTiles.back().row;
        }

        // avatar MovableActor anim state: all 4 stage timers idle (1.0),
        // scale (1, 1), rotation 0, exitVanishing clear. (reset() below
        // re-applies scale/rotation/exitVanishing; the 4 timers are only
        // set here.)
        ps.spawnT         = 1.0f;
        ps.moveT          = 1.0f;
        ps.fadeT          = 1.0f;
        ps.scaleOutT      = 1.0f;
        ps.exitVanishing  = false;
        ps.baseQuad.scaleX   = 1.0f;
        ps.baseQuad.scaleY   = 1.0f;
        ps.baseQuad.rotation = 0.0f;
        ps.gridCol = newestCol;
        ps.gridRow = newestRow;

        // avatar position: mode-1 snapped hex X for (col, row); Y = the
        // case-1 seed (1.0, = FUN_100016b18's param_2). the layout pass
        // (section 17) repositions afterward.
        const HexCellPos avatarPos = hexCellSnappedXY(newestCol, newestRow);
        ps.setPosition(avatarPos.x, 1.0f);

        // reset() clears the 3 item slots + perk vector, applies the
        // portrait UV for characterIndex, and sets base stats to 1; the
        // saved values below override those.
        ps.reset((int)snap.characterIndex);

        ps.currentLevel = (int)snap.levelsGained;   // the run's level-up count
        ps.baseATK      = (int)snap.baseATK;
        ps.baseDEF      = (int)snap.baseDEF;
        ps.baseHP       = (int)snap.baseHP;

        // rebuild the 3 starter items (types 0/1/2) from the saved stat
        // blocks. push() recomputes stats against the baseATK/DEF/HP set
        // just above.
        for (int t = 0; t < 3; ++t) {
            const StatBlockSnapshot& sb = snap.statBlocks[t];
            Item* item = new Item();
            item->initExplicit(t, (int)sb.itemSubType, (int)sb.itemCosmeticNameIdx,
                               (int)sb.itemAtk, (int)sb.itemDef, (int)sb.itemHp,
                               (int)sb.spa0Type, (int)sb.spa0Value,
                               (int)sb.spa1Type, (int)sb.spa1Value);
            ps.push(item);
        }

        // saved current stats, set after the item pushes so they're the
        // final word (push()'s recomputeStats may otherwise clamp them).
        ps.attack        = (int)snap.attack;
        ps.defence       = (int)snap.defence;
        ps.currentHealth = (int)snap.currentHealth;

        // append each saved perk (raw push, no dedup; reset() emptied the
        // vector). entries are packed (perkType | perkLevel << 32).
        for (int64_t packed : snap.perks) {
            const int perkType  = (int)(packed & 0xFFFFFFFF);
            const int perkLevel = (int)(packed >> 32);
            Perk* perk = new Perk();
            perk->init(perkType, perkLevel);
            ps.perks.push_back(perk);
        }
    }

    // ---- 7. Rack tile rebuild (5 slots via TileObject::setFromSnapshot) ----
    //
    // matches FUN_100016b18's rack loop: clear playerDowned, then per slot
    // delete the old tile, allocate + init a fresh one, rebuild it from the
    // saved RackTileSnapshot fields, and set its rack position.
    playerDowned = false;   // gb+0x8418 = 0

    {
        const float virtualHeight = Renderer::getVirtualHeight();
        // rack Y = virtualHeight + DAT_100059f0c + DAT_100059f10.
        const float rackY = virtualHeight + (-0.0984375f) + (-0.0203125f);

        for (int i = 0; i < RACK_SLOT_COUNT; ++i) {
            const RackTileSnapshot& r = snap.rackTiles[i];

            if (rack[i] != nullptr) {
                delete rack[i];
            }

            TileObject* tile = new TileObject();
            tile->init();
            rack[i] = tile;

            const uint32_t snagFields[6] = {
                r.snagKind, r.snagHp, r.snagAtk, r.snagDef,
                (uint32_t)r.snagConsumedFlag, (uint32_t)r.snagObsessionCount,
            };

            tile->setFromSnapshot(gridLayout, (int)r.gridIdx, r.mirror ? 1 : 0,
                                  (int)r.rotationStep, r.contentType,
                                  r.contentMagnitude, snagFields,
                                  &r.decorations, &playerSystem);

            // rack column X = i * 0.1953125 + DAT_100059f08.
            tile->setPosition((float)i * 0.1953125f + 0.109375f, rackY);
        }
    }

    // -1 sentinel (= "no current/last drag", per FUN_100016b18). the 8-byte
    // store covers both adjacent int slots.
    draggedRackSlot = -1;
    selectedRackSlot = -1;

    // ---- 8. PageList (placed tiles) rebuild ----
    //
    // restore reuses the live GameBoard (the player returns from the main
    // menu), so the previous run's placed tiles are still in pageList and must
    // be torn down before the rebuild; otherwise the old trail and the
    // restored trail coexist as exact duplicates, which forces the X-button
    // and makes the Nemesis eat the trail twice. matches FUN_100016b18, which
    // walks gb+0x1b0 deleting each node->tile then FUN_100029320 clears it.
    for (TileObject* old : pageList) {

        if (old != nullptr) {
            delete old;
        }
    }

    pageList.clear();

    // per saved PlacedTileSnapshot (oldest->newest, keyed by index): resolve
    // content from placedContentMap[idx] and snag from placedSnagMap[idx]
    // (absent = none), build the tile, mark it committed, set its grid coord,
    // position it at its hex, and push it onto pageList.
    {
        int idx = 0;

        for (const PlacedTileSnapshot& p : snap.placedTiles) {
            TileObject* tile = new TileObject();
            tile->init();

            // content (placedContentMap[idx] -> packed type|mag<<32; absent = 0).
            uint32_t contentType = 0;
            int      contentMag  = 0;
            auto contentIt = snap.placedContentMap.find(idx);

            if (contentIt != snap.placedContentMap.end()) {
                contentType = (uint32_t)(contentIt->second & 0xFFFFFFFF);
                contentMag  = (int)(contentIt->second >> 32);
            }

            // snag (placedSnagMap[idx] -> 6 fields; absent = kind 0).
            uint32_t snagFields[6] = { 0, 0, 0, 0, 0, 0 };
            auto snagIt = snap.placedSnagMap.find(idx);

            if (snagIt != snap.placedSnagMap.end()) {
                const PlacedSnagFields& v = snagIt->second;
                snagFields[0] = v.kind;
                snagFields[1] = v.hp;
                snagFields[2] = v.atk;
                snagFields[3] = v.def;
                snagFields[4] = v.extra0;
                snagFields[5] = v.extra1;
            }

            tile->setFromSnapshot(gridLayout, (int)p.gridIdx, p.mirror ? 1 : 0,
                                  (int)p.rotationStep, contentType, contentMag,
                                  snagFields, /*decorations=*/nullptr,
                                  &playerSystem);

            tile->committed = true;             // tile+0xF0 = 1
            tile->setGridCoord(p.col, p.row);   // FUN_10001349c

            // position from FUN_100012f04(gridCol, gridRow, mode 0), which
            // returns both the hex X (s0) and hex Y (s1 = row * 0.1705); the
            // binary feeds both straight into setPosition. Ghidra drops s1 and
            // shows a stale rack-Y in its place; do not use rackY here, or
            // every placed tile lands on one row near the world origin.
            const HexCellPos placedPos =
                hexCellLinearXY(tile->gridCol, tile->gridRow);
            tile->setPosition(placedPos.x, placedPos.y);

            pageList.push_back(tile);
            ++idx;
        }
    }

    // ---- 8b. avatar reposition onto the newest placed tile ----
    //
    // FUN_100016b18 re-sets the avatar position after the page list is built:
    // onto the newest placed tile's mainQuad position, or (empty board) the
    // origin hex + the rack-Y placeholder. this overrides the placeholder
    // position section 6 seeded.
    {
        if (pageList.empty()) {
            // empty board: avatar at the snapped origin hex, both X and Y
            // from FUN_100012f04 mode-1 (the binary's `stp s0,s1`). not rackY.
            const HexCellPos origin = hexCellSnappedXY(0, 0);
            playerSystem.setPosition(origin.x, origin.y);
        } else {
            // non-empty: avatar snaps onto the newest placed tile's quad
            // position (tile+0xA8 / +0xAC), which section 8 just set.
            const TileObject* newest = pageList.back();
            playerSystem.setPosition(newest->mainQuad.posX,
                                     newest->mainQuad.posY);
        }
    }

    // ---- 9. scroll bounds + board-translate seeding ----
    navDragState = -1;                       // +0x1C0

    for (int i = 0; i < MAX_CURSOR_COUNT; ++i) {
        tileCursorVisible[i] = false;        // +0x6E0
        tileCursorState[i]   = false;        // +0x6E6
    }

    {
        // FUN_100057374: round(v * 640) / 640 pixel snap (same as the
        // hexCellSnappedXY lambda).
        auto snap640 = [](float v) -> float {
            return (v >= 0.0f) ? (float)(int)(v * 640.0f + 0.5f) / 640.0f
                               : (float)(int)(v * 640.0f - 0.5f) / 640.0f;
        };

        const float vh = Renderer::getVirtualHeight();

        if (pageList.empty()) {
            positionX = 0.5f;                // +0x97C
            positionY = vh * 0.5f;           // +0x980
        } else {
            // center the board on the newest placed tile: subtract its hex
            // position from the screen-center anchor, then pixel-snap. the
            // hex position is FUN_100012f04 mode-0, which returns both s0=X
            // and s1=Y (= row * 0.1705); the binary uses the dropped s1 here
            // for positionY, not rackY.
            const TileObject* newest = pageList.back();
            const HexCellPos newestPos =
                hexCellLinearXY(newest->gridCol, newest->gridRow);
            positionX = snap640(0.5f - newestPos.x);
            positionY = snap640(vh * 0.5f - newestPos.y);
        }
    }

    panDragActive = false;                    // drag / inertial-fling flags
    panInertiaActive = false;
    panProgress = 1.0f;                       // +0x9B0
    recomputeScrollBounds(0.0f);              // FUN_1000174c4 (ignores the arg)
    tileAlphaMirror   = 0.0f;                 // +0x9C4
    tileAlphaProgress = 0.0f;                 // +0x9C8

    // ---- 10. Nemesis restore (FUN_100009998) ----
    //
    // the Nemesis faces the oldest placed tile (= gb+0x1b0 = pageList.front(),
    // the end it eats from first), rebuilt in section 8. seedNemesisFacing
    // derives the 0..5 facing dir from the relative grid positions; empty page
    // list -> null -> facing 0.
    const TileObject* oldestPlaced =
        this->pageList.empty() ? nullptr : this->pageList.front();

    const int facingInt = seedNemesisFacing(oldestPlaced, snap);

    // placeOnHexGrid takes the facing as a plain float value and recovers it
    // with (int)facingDir (the binary stores the raw int facing at nemesis
    // +0x17a0, then scvtf's (facing - 4) into the rotation). pass (float)int,
    // exactly like the in-gameplay caller, not a reinterpreted bit pattern,
    // which reads back as 0 and pins every reload to one wrong rotation.
    restoreNemesisFromSnapshot(nemesis, snap, (float)facingInt);

    // ---- 11. nemesis eat-cycle clear + detail/dialog panel resets ----
    //
    // matches FUN_100016b18's inline post-Nemesis writes:
    //   gb+0x43F0..+0x43FC = 0     (the 16-byte nemesis eat-cycle block)
    //   FUN_1000404b0(gb+0x4408, 1) = detailPanel.reset(1)
    //   gb+0x54A8 = 0              (detailPanel internal field, before
    //                                dialog reset; reset() doesn't clear it)
    //   FUN_10004128c(gb+0x54B8, 1) = dialogPanel.reset(1)
    nemesis.eatTarget = 0;
    nemesis.eatActive = false;
    nemesis.eatStep   = 0;
    nemesis.eatFired  = false;

    detailPanel.reset(1);

    // explicit clear of detailPanel.touchHoldArea. reset() bails when
    // !visible, so we write the field unconditionally, matching
    // FUN_100016b18's ordering (reset, then field clear, then dialog reset).
    detailPanel.touchHoldArea = 0;

    dialogPanel.reset(1);

    // ---- 12. HUD restore (FUN_10000da70) ----
    //
    // runs after the PlayerSystem restore (section 6) so the player stats it
    // reads are the restored values. the HUD method owns its ~20 internal
    // field writes + marker-bank seeding + event-tray rebuild; the orchestrator
    // just unpacks the snapshot's player stats + XP/CONTROL totals + tray list.
    hud.restoreFromSnapshot(playerSystem.attack, playerSystem.defence,
                            playerSystem.maxHealth, playerSystem.currentHealth,
                            (int)snap.xpReceivedTotal,
                            (int)snap.controlReceivedTotal,
                            snap.eventTraySnapshot);

    // ---- 13. HUD pendingDiscards drain + selectedEvent clear + StatBars hide ----
    //
    // matches FUN_100016b18's inline writes between HUD restore and the
    // queue force-completes. binary's pendingDiscards drain pops one entry
    // per iter (stride 0xE0 = RemovalAnim size) with a FUN_100007df0 dtor
    // call that's a no-op since the std::list<RemovalAnim> erase already
    // calls the dtor for us; vector clear is the equivalent operation.
    hud.pendingDiscards.clear();
    hud.selectedEvent = nullptr;
    statBars.visible  = false;

    // ---- 13b. stat-tween + action queue force-complete ----
    //
    // FUN_10002c170(gb+0x9650) clears statTweenAnyAnim + bumps every live
    // statTween's animT to 1.0f so the next update tick pops them.
    // FUN_1000388cc(gb+0x9670) does the same for actionQueue entries.
    statTweenAnyAnim = false;

    for (TweenBody& entry : this->statTween) {
        entry.animT = 1.0f;
    }

    for (ActionBody& entry : this->actionQueue) {
        entry.animT = 1.0f;
    }

    // ---- 14. TileWeightPool restore + RNG seeds ----
    restoreTileWeightPoolFromSnapshot(this->tileWeightPool, snap.tileWeightPool);

    for (uint32_t i = 0; i < 5; ++i) {
        rngSeed(snap.rngSeeds[i], i);
    }

    // ---- 15a. discard-slide list cleanup (gb+0x96A0) ----
    //
    // FUN_100016b18's free loop walks each entry deleting the held
    // TileObject before clearing the list.
    for (DiscardSlideBody& entry : this->discardSlide) {

        if (entry.tile != nullptr) {
            delete entry.tile;
        }
    }

    this->discardSlide.clear();

    // gb+0x96D0 = gameplayItemsLiveCount = 0
    this->gameplayItemsLiveCount = 0;

    // ---- 15b. reserve queue cleanup (gb+0x96D8) ----
    //
    // same pattern: delete each held TileObject, then clear the list.
    for (TileReserveEntry& entry : this->tileReserve) {

        if (entry.tile != nullptr) {
            delete entry.tile;
        }
    }

    this->tileReserve.clear();

    // ---- 15c. reserve queue rebuild from snap.reserveItems ----
    //
    // matches FUN_100016b18's reserve loop: per saved ReserveItemSnapshot
    // allocate + init a fresh TileObject, rebuild it from the saved fields
    // (no decorations on reserve tiles), and push {tile, listSlotIndex}
    // onto the reserve queue. reserve tiles aren't positioned (they're
    // off-screen until populateRack pops them into a rack slot).
    for (const ReserveItemSnapshot& e : snap.reserveItems) {
        TileObject* tile = new TileObject();
        tile->init();

        const uint32_t snagFields[6] = {
            e.snagKind, e.snagHp, e.snagAtk, e.snagDef,
            (uint32_t)e.snagConsumedFlag, (uint32_t)e.snagObsessionCount,
        };

        tile->setFromSnapshot(gridLayout, (int)e.gridIdx, e.mirror ? 1 : 0,
                              (int)e.rotationStep, e.contentType,
                              e.contentMagnitude, snagFields,
                              /*decorations=*/nullptr, &playerSystem);

        tileReserve.push_back(TileReserveEntry(tile,
                                               (uint32_t)e.listSlotIndex));
    }

    // ---- 16. HexMap restore ----
    restoreHexMapFromSnapshot(this->hexMap, snap.hexMapVec);

    // ---- 17. level-exit placement + chrome finalize ----

    // 17a. exit + key cells (FUN_1000175f0) from the SAVED exit position,
    // then keysRequired (FUN_100017794) + keysCollected (FUN_1000178fc).
    placeExitAndKeys(snap.exitCol, snap.exitRow);
    setExitKeysRequired((int)snap.keysRequired);
    recomputeExitKeysCollected();

    // 17b. exit-arrow + dim-overlay reset. unlike initLevelContent (which
    // leaves the dim quad to case-10's cross-fade), FUN_100016b18 resets it
    // for a clean visual slate on the menu->game entry.
    exitArrowVisible = false;            // +0x9C60
    exitArrowFade    = 0.0f;             // +0x9C64
    dimQuad.setAlpha(0);                 // FUN_100008388(gb+0x9C68, 0)
    dimProgress      = 0.0f;             // +0x9D40

    // 17c. force the dream-snippet banner to its hidden (faded-out) state
    // (FUN_10003a5d8 = forceFadeOutComplete, not FUN_10003a408 = reset). reset()
    // starts track 1, which would re-animate any stale glyphs left over from
    // the snippet shown before exiting to the menu, replaying the level-intro
    // text on every resume. forceFadeOutComplete pins track2Timer to 1.0 so
    // draw() early-outs and the banner stays hidden.
    animController.forceFadeOutComplete();

    // 17d. dream-snippet bag restore. refill the pool with 1..N-1, restore
    // the saved "already shown" history, then remove each shown entry from
    // the pool, reproducing the draw-without-replacement bag state at save
    // time. (FUN_10002922c clear + FUN_1000390b0 count + FUN_100028648
    // history assign + FUN_1000285a8 per-entry erase.)
    animBannerSeedPool.clear();
    {
        const int snippetCount = static_cast<int>(DreamSnippets::table().size());

        for (int i = 1; i < snippetCount; i++) {
            animBannerSeedPool.insert(i);
        }
    }
    animBannerSeedHistory = snap.animBannerSeedHistory;

    for (int64_t shown : animBannerSeedHistory) {
        animBannerSeedPool.erase(shown);
    }

    // 17e. pause menu hidden (FUN_10002f72c).
    pauseMenu.panelHide(true);

    // 17f. shop-pool filter copies: gate which snags / events can spawn in
    // the restored run (FUN_100028eec / FUN_10000f780).
    excludedSnagTypes                = snagFilter;
    eventChoicePanel.excludedHistory = eventFilter;

    // 17g. final display state: discard-button availability (FUN_100017b44),
    // HUD conditional-icon re-anchor (FUN_10000bab8), X-button refresh
    // (FUN_100017c04).
    refreshDiscardButtonAvailability();
    hud.setConditionalIcon(GameplayHUD::ConditionalIconState::Default);
    tryShowXButton();
}