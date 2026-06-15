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
// HexMap lives at GameBoard+0x96F0 (0x18 bytes, head only). it's the main
// hex grid map: a sorted associative container of "cells" keyed by hex grid
// coordinates. each cell tracks a small piece of visualization state for
// one hex position the character has visited or is near (a marker dot
// plus a fade-in animation).
//
// the binary's collection at +0x96F0 has the EXACT 24-byte layout of a
// libc++ std::map<K, V> with empty (= EBO-compressed) Compare and
// Allocator: { begin_node, end_node.left, size }. the per-cell tree-node
// is libc++'s standard __tree_node layout, so we can use std::map<> here
// directly and inherit its empty / find / insert / iterate operations.
//
// the broader visual subsystem extends past +0x96F0: the indicator Quad
// at GameBoard+0x9710 and a 4-entry button-quad array at +0x97E8 belong
// to GameBoard's outer drawing flow, not to this 0x18-byte map head.

// per-cell payload (0x1C0 bytes). embedded inside libc++'s std::map node;
// total node size = 0x20 (tree-node header) + 0x8 (pair key) + 0x1C0 (cell)
// = 0x1E8, matching FUN_10003ba04's operator_new(0x1E8) call.
//
// layout derived from FUN_10003b238 (addCell) writes + FUN_10003b0c8 reads
// (where plVar5 is the tree-node base, value at node+0x28 = cell+0):
//   +0x000  int   kind         (== 0 means erase-on-fade-complete; non-zero
//                               cells stay around even after fading in)
//   +0x004  int   fadeTimer    (lifetime mode + countdown. set by addCell from
//                               param_4. observed values:
//                                 0 = persistent (no tick, no auto-consume)
//                                 2 = fade-pending; HexMap::tickFade decrements
//                                     2 -> 1 -> deactivate over 2 turns
//                                 3 = consume-on-visit sentinel (tryConsumeCellAt)
//                                 1 = transient end-stage of the fade chain)
//   +0x008  Quad  icon         (drawn when kind != 0; the visual marker the
//                               player sees on visited / scoutable hexes)
//   +0x0E0  Quad  outline      (drawn during fade-in; faded out as fadeT2
//                               completes)
//   +0x1B8  float fadeT1       (0..1 icon fade-in timer, rate 2/s)
//   +0x1BC  float fadeT2       (0..1 outline fade-out timer, rate 2/s)
//
// the trailing 0x10 of each Quad (icon @ +0xD0, outline @ +0x1A8) holds
// the Quad's animation target rect (animMinX/Y, animMaxX/Y; see quad.h).
// HexMapCell doesn't drive that animation, but the bytes still live inside
// Quad's footprint.
#include "quad.h"  // pulled in for the embedded Quad fields below
struct HexMapCell {
    int     kind;             // +0x000
    int     fadeTimer;        // +0x004  addCell stores param_4 here; see header comment for value semantics
    Quad    icon;             // +0x008..+0x0DF (0xD8, owns anim rect at +0xD0)
    Quad    outline;          // +0x0E0..+0x1B7 (0xD8, owns anim rect at +0x1A8)
    float   fadeT1;           // +0x1B8  icon fade-in (0..1, rate 2/s)
    float   fadeT2;           // +0x1BC  outline fade-out (0..1, rate 2/s)
};
static_assert(sizeof(HexMapCell) == 0x1C0,
              "HexMapCell must be exactly 0x1C0 bytes (= operator_new(0x1E8) "
              "minus tree-node header 0x20 minus std::pair<int,int> key 0x8)");

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

static_assert(sizeof(std::map<HexMap::Key, HexMapCell>) == 0x18,
              "libc++ std::map head must be exactly 24 bytes (begin/end/size). "
              "if a future libc++ version breaks this, HexMap needs a manual "
              "raw-pointer triple instead.");
static_assert(sizeof(HexMap) == 0x18,
              "HexMap head must be exactly 0x18 bytes (3 pointers)");
