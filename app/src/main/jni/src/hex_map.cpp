#include "hex_map.h"

// per-kind icon UV bounds at DAT_10007ae10, 0x30-byte stride table,
// bytes 0..0x10 = (uMinX, uMinY, width, height) as ints. only kinds
// 1..7 are populated (FUN_10003b404 gates `param_1 - 1 < 7`); kind 1's
// row is present but the binary special-cases kind 1 to use zero UV.
namespace {
struct KindIconUV { int uMinX; int uMinY; int width; int height; };
constexpr KindIconUV kKindIconUVTable[8] = {
    /* 0 */ {  0,   0,  0,  0 },   // sentinel, never indexed
    /* 1 */ {322, 270, 93, 49 },   // exit
    /* 2 */ {376, 530, 76, 70 },   // exit key
    /* 3 */ {308, 512, 66, 83 },   // snag-spawn marker (Fear's Threat token)
    /* 4 */ {454, 537, 70, 67 },   // CTRL tile placement hint
    /* 5 */ {494, 246, 69, 68 },   // 4 leaf clover icon (Good Luck)
    /* 6 */ {433, 454, 75, 75 },   // sword icon (Weakness?)
    /* 7 */ {644, 246, 73, 74 },   // dark blue spade icon (Talent)
};
}  // namespace

// FUN_10003b204, empty-init. binary self-aliases the begin pointer to the
// end-node sentinel and zeros the size; std::map's clear() does the
// equivalent.
void HexMap::init() {
    cells.clear();
}

// FUN_10003b00c. walk every cell, skipping the one at the player's current
// hex (where the avatar sits and would obscure the marker anyway). caller
// must have tex 9 (UI atlas) bound; the cells' icon/outline UVs are pixel
// coords on that atlas.
//
// per-cell draw:
//   - outline drawn while fadeT2 < 1 (= fading out via deactivateCell);
//     hides itself once HexMap::update ticks fadeT2 to 1.0.
//   - icon drawn when kind != 0 (live cell).
void HexMap::draw(int playerCol, int playerRow) {

    if (cells.empty()) {
        return;
    }

    for (auto& entry : cells) {
        const Key& key = entry.first;

        if (key.first == playerCol && key.second == playerRow) {
            continue;
        }

        HexMapCell& cell = entry.second;

        if (cell.fadeT2 < 1.0f) {
            cell.outline.draw();
        }

        if (cell.kind != 0) {
            cell.icon.draw();
        }
    }
}

// FUN_10003b0c8. animate each cell's fade timers and prune cells whose
// outline fade-out completes once kind has been zeroed by FUN_10003b364.
//
// per cell:
//   1. fadeT1 (icon fade-in): advance by dt*2 (clamped), set icon.alpha
//      = fadeT1 * 255. visible cells animate from invisible up to fully
//      drawn over ~0.5s.
//   2. fadeT2 (outline fade-out): advance by dt*2, set outline.alpha
//      = (1 - fadeT2) * 255. outline starts visible, fades away.
//   3. if fadeT2 >= 1.0 and kind == 0: erase the cell. cells whose kind
//      was zeroed by a prior `addCell(0, ...)` call (explicit removal)
//      finish their outline fade then drop out of the map.
//
// on an empty std::map the walk is a no-op.
void HexMap::update(float dt) {

    if (cells.empty()) {
        return;
    }

    constexpr float ALPHA_END = 255.0f;  // DAT_10005a280

    auto it = cells.begin();

    while (it != cells.end()) {
        HexMapCell& c = it->second;

        if (c.fadeT1 < 1.0f) {
            float t = c.fadeT1 + dt + dt;

            if (t > 1.0f) {
                t = 1.0f;
            }

            c.fadeT1 = t;
            uint8_t a = static_cast<uint8_t>(static_cast<int>(t * ALPHA_END));
            c.icon.setAlpha(a);
        }

        if (c.fadeT2 < 1.0f) {
            float t = c.fadeT2 + dt + dt;

            if (t > 1.0f) {
                t = 1.0f;
            }

            c.fadeT2 = t;
            uint8_t a = static_cast<uint8_t>(static_cast<int>((1.0f - t) * ALPHA_END));
            c.outline.setAlpha(a);
        }

        if (c.fadeT2 >= 1.0f && c.kind == 0) {
            it = cells.erase(it);   // FUN_10003bb84, unlink + delete
        } else {
            ++it;
        }
    }
}

// FUN_10003b238.
//   - cells with kind 1 (exit) and 2 (key) are locked once placed;
//     subsequent addCell calls at their hex are ignored.
//   - same-kind re-add just refreshes fadeTimer.
//   - different-kind re-add deactivates the old cell (kicks its outline
//     fade-out) then re-initialises in place.
//   - fresh cells start with fadeT2=1.0 (outline already-faded), fadeT1=0
//     (icon fades in over the next 0.5s via HexMap::update).
void HexMap::addCell(uint32_t kind, int gridCol, int gridRow, uint32_t fadeTimer) {
    constexpr float TEX_PIXEL_INV    = 1.0f / 1024.0f;     // DAT_100059ebc
    constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;      // DAT_100059ec0
    constexpr float CELL_POS_OFFSET  = 0.0015625f;         // DAT_10005a284
    constexpr float HEX_X_COL_STEP   =  0.196875f;         // DAT_100059e80
    constexpr float HEX_X_ROW_OFFSET = -0.098437503f;      // DAT_100059e84
    constexpr float HEX_Y_ROW_STEP   =  0.170498759f;      // DAT_100059e88

    HexMapCell* cell = nullptr;
    auto it = cells.find({ gridCol, gridRow });

    if (it == cells.end()) {
        // emplace a fresh cell. binary's FUN_10003b79c does the alloc +
        // tree insert; we delegate to std::map::emplace.
        auto inserted = cells.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(gridCol, gridRow),
                                      std::forward_as_tuple());
        cell = &inserted.first->second;
        cell->fadeT2 = 1.0f;   // matches puVar2[0x6f] = 0x3f800000;
    } else {
        cell = &it->second;

        // existing kind 1 (exit) or 2 (key) are locked. binary gate:
        // `if (1 < (existingKind - 1))` -> existingKind > 2 to proceed.
        // we negate: existingKind == 1 || 2 -> no-op return.
        if (cell->kind == 1 || cell->kind == 2) {
            return;
        }

        // same kind: refresh fadeTimer only.
        if ((uint32_t)cell->kind == kind) {
            cell->fadeTimer = (int)fadeTimer;
            return;
        }

        // kind changed: deactivate old visuals, then fall through to
        // re-init below (binary's `goto LAB_10003b2c8`).
        deactivateCell(*cell);
    }

    // common-init (LAB_10003b2c8). new kind / fadeTimer, restart icon
    // fade-in from 0.
    cell->kind      = (int)kind;
    cell->fadeTimer = (int)fadeTimer;
    cell->fadeT1    = 0.0f;

    // icon UV / size lookup. binary skips the table read for kind == 1
    // (zero UV stays from the local stack init); kinds 2..7 read from
    // the 0x30-byte stride table at DAT_10007ae10.
    int uvMinX = 0;
    int uvMinY = 0;
    int uvW    = 0;
    int uvH    = 0;

    if (kind != 1 && kind >= 2 && kind <= 7) {
        const KindIconUV& uv = kKindIconUVTable[kind];
        uvMinX = uv.uMinX;
        uvMinY = uv.uMinY;
        uvW    = uv.width;
        uvH    = uv.height;
    }

    // FUN_100014d84 inlined: pixel UV / 1024 -> atlas-fraction UV;
    // pixel size / 640 -> screen-fraction size (scale = 1.0 from caller).
    float u0 = (float)uvMinX            * TEX_PIXEL_INV;
    float v0 = (float)uvMinY            * TEX_PIXEL_INV;
    float u1 = (float)(uvMinX + uvW)    * TEX_PIXEL_INV;
    float v1 = (float)(uvMinY + uvH)    * TEX_PIXEL_INV;
    cell->icon.setTexCoords(u0, v0, u1, v1);
    cell->icon.setSize((float)uvW * SCREEN_PIXEL_INV,
                       (float)uvH * SCREEN_PIXEL_INV);

    // hex grid -> screen coordinates (FUN_100012f04 mode=0, linear).
    float hexX = (float)gridCol * HEX_X_COL_STEP +
                 (float)gridRow * HEX_X_ROW_OFFSET;
    float hexY = (float)gridRow * HEX_Y_ROW_STEP;
    cell->icon.posX = hexX + CELL_POS_OFFSET;
    cell->icon.posY = hexY + CELL_POS_OFFSET;

    cell->icon.setAlpha(0);   // start invisible; update() ramps to 255 over 0.5s
}

// FUN_10003b530. looks up cell.kind at (col, row); returns 0 when absent.
// the binary calls FUN_10003baf0 (std::map::find equivalent) and reads
// the cell's first int (kind) when found, else 0 from the end-iterator.
int HexMap::cellKindAt(int col, int row) const {
    auto it = cells.find({ col, row });

    if (it == cells.end()) {
        return 0;
    }

    return it->second.kind;
}

// FUN_10003b364. inner gate `kind != 0` is the binary's `if (*param_2 != 0)`
// early-out; on hit:
//   - kind / fadeTimer -> 0
//   - fadeT2 = clamp(1 - fadeT1, 0, 1), fadeT1 = 1.0 (kicks the outline
//     fade-out timer; HexMap::update will tick fadeT2 to 1.0 over 0.5s)
//   - outline copies icon's display state (so the fade-out matches the
//     last drawn icon's pos / scale / rotation)
//   - outline alpha = (1 - newFadeT2) * 255 (initial visibility before
//     the per-frame update reduces it).
void HexMap::deactivateCell(HexMapCell& cell) {
    constexpr float ALPHA_END = 255.0f;            // DAT_10005a288

    if (cell.kind == 0) {
        return;
    }

    cell.kind      = 0;
    cell.fadeTimer = 0;

    float newFadeT2 = 1.0f - cell.fadeT1;

    if (newFadeT2 < 0.0f) {
        newFadeT2 = 0.0f;
    }
    else if (newFadeT2 > 1.0f) {
        newFadeT2 = 1.0f;
    }

    cell.fadeT2 = newFadeT2;
    cell.fadeT1 = 1.0f;

    cell.outline.copyVisualState(cell.icon);
    cell.outline.setAlpha(static_cast<uint8_t>(
        static_cast<int>((1.0f - newFadeT2) * ALPHA_END)));
}

// FUN_10003b4a0. per-turn fade tick on every cell. behavior:
//   - fadeTimer == 2: decrement to 1 (still drawn, will fade next turn).
//   - fadeTimer == 1: deactivate (kicks the outline fade-out anim).
//   - other (0, 3, etc.): no change.
// the binary's tree walk uses an in-order traversal of the libc++ rb-tree;
// we just iterate the std::map. order doesn't matter, each cell is
// processed independently.
void HexMap::tickFade() {

    for (auto& entry : cells) {
        HexMapCell& cell = entry.second;

        if (cell.fadeTimer == 2) {
            cell.fadeTimer = 1;
        }
        else if (cell.fadeTimer == 1) {
            deactivateCell(cell);
        }
    }
}

// FUN_10003b458. outer gate (fadeTimer == 3) is the binary's
// `ldr w8, [x0, #0x2c]; cmp w8, #0x3`; only specific dynamic markers
// consume on visit. layout cells (fadeTimer=0) and snag-0x28 grow cells
// (fadeTimer=2) survive.
void HexMap::tryConsumeCellAt(int col, int row) {
    auto it = cells.find({ col, row });

    if (it == cells.end()) {
        return;
    }

    HexMapCell& cell = it->second;

    if (cell.fadeTimer != 3) {
        return;
    }

    deactivateCell(cell);
}
