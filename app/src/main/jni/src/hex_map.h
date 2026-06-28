#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

// reconstructed from Ghidra:
//   init    : FUN_10003b204
//   draw    : FUN_10003b00c
//   update  : FUN_10003b0c8
//   addCell : FUN_10003b238
//   loaderHook (Phase C): FUN_10003b720
//
// HexMap lives at GameBoard.hexMap (0x18 bytes, head only). it's the main
// hex grid map: a sorted associative container of "cells" keyed by hex grid
// coordinates. each cell tracks a small piece of visualization state for
// one hex position the character has visited or is near (a marker dot
// plus a fade-in animation).
//
// the broader visual subsystem extends beyond this map head: the indicator
// Quad at GameBoard.exitTileIcon and a 4-entry button-quad array belong
// to GameBoard's outer drawing flow, not to this 0x18-byte map head.

// per-cell payload (0x1C0 bytes). embedded inside libc++'s std::map node;
// total node size = 0x1E8 (tree-node header 0x20, pair key 0x8, cell 0x1C0),
// matching FUN_10003ba04's operator_new(0x1E8) call.
//
// layout derived from FUN_10003b238 (addCell) writes + FUN_10003b0c8 reads:
// int   kind         (== 0 means erase-on-fade-complete; non-zero
//                               cells stay around even after fading in)
// int   fadeTimer    (lifetime mode + countdown. set by addCell from
//                               param_4. observed values:
//                                 0 = persistent (no tick, no auto-consume)
//                                 2 = fade-pending; HexMap::tickFade decrements
//                                     2 -> 1 -> deactivate over 2 turns
//                                 3 = consume-on-visit sentinel (tryConsumeCellAt)
//                                 1 = transient end-stage of the fade chain)
// Quad  icon         (drawn when kind != 0; the visual marker the
//                               player sees on visited / scoutable hexes)
// Quad  outline      (drawn during fade-in; faded out as fadeT2
//                               completes)
// float fadeT1       (0..1 icon fade-in timer, rate 2/s)
// float fadeT2       (0..1 outline fade-out timer, rate 2/s)
//
// the trailing 0x10 of each Quad holds the Quad's animation target rect
// (animMinX/Y, animMaxX/Y; see quad.h). HexMapCell doesn't drive that
// animation, but the bytes still live inside Quad's footprint.
#include "quad.h"  // pulled in for the embedded Quad fields below
struct HexMapCell {
    int     kind;
    int     fadeTimer;        // addCell stores param_4 here; see header comment for value semantics
    Quad    icon;             // (0xD8, owns its anim rect)
    Quad    outline;          // (0xD8, owns its anim rect)
    float   fadeT1;           // icon fade-in (0..1, rate 2/s)
    float   fadeT2;           // outline fade-out (0..1, rate 2/s)
};

class HexMap {
public:
    using Key  = std::pair<int, int>;     // (tileVariant, gridIdx)
    using Map  = std::map<Key, HexMapCell>;

    // FUN_10003b204. empty-init; clears the tree.
    void init();

    // FUN_10003b00c. draw all cells whose key != (playerCol, playerRow).
    // an empty map is a no-op.
    void draw(int playerCol, int playerRow);

    // FUN_10003b0c8. animate per-cell fadeIn timers, dropping cells whose
    // timer reaches 1 and kind == 0. an empty map is a no-op.
    void update(float dt);

    // FUN_10003b238. insert-or-update a cell at (tileVariant, gridIdx).
    void addCell(uint32_t kind, int gridCol, int gridRow, uint32_t fadeTimer);

    // FUN_10003b4a0. per-turn fade tick. walks every cell, decrementing
    // fadeTimer 2 -> 1 -> deactivate. cells with fadeTimer 0 (persistent) or
    // 3 (consume-on-visit) are unaffected. invoked by GameBoard::
    // applyEndOfTurnPipeline.
    void tickFade();

    // FUN_10003b530. return the cell kind stored at (col, row), or 0 when
    // no cell exists at that key. used by the post-commit content dispatch
    // (kind == 7 -> double the magnitude) and the control-tile predicate
    // (FUN_1000245cc, which short-circuits true on kind == 4).
    int cellKindAt(int col, int row) const;

    // FUN_10003b458. try to "consume" the cell at (col, row): only fires
    // when cell.fadeTimer == 3 (binary disasm: `ldr w8, [x0, #0x2c]; cmp
    // w8, #0x3`). on hit, calls FUN_10003b364 which clears the cell's kind
    // / fadeTimer and kicks an outline fade-out. cells from the layout
    // pass (FUN_1000175f0) and snag-0x28 grow (FUN_100024cc8 Section 2)
    // both pass fadeTimer=0 / 2, so they are not consumed by this.
    void tryConsumeCellAt(int col, int row);

    // --- byte-exact field. libc++'s std::map<std::pair<int,int>, T> with
    // default (= empty) std::less<...> + std::allocator<...> compresses the
    // comparator + allocator into 0 bytes via EBO, leaving exactly the
    // 24-byte tree head (begin_node + end_node.left + size).
    Map cells;

private:
    // FUN_10003b364. clear a cell's kind / fadeTimer, kick its outline
    // fade-out (fadeT2 = 1 - fadeT1, fadeT1 = 1.0), copy icon's display
    // state onto outline, set outline alpha = (1 - newFadeT2) * 255.
    // shared by tryConsumeCellAt, tickFade, and addCell's "kind changed" branch.
    void deactivateCell(HexMapCell& cell);
};
