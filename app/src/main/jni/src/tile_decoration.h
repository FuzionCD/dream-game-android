#pragma once

#include "color_tint.h"
#include "quad.h"
#include <cstddef>
#include <cstdint>

// reconstructed from Ghidra:
//   alloc + link helper:  FUN_1000140b8
//   push (sorted insert): FUN_100013870
//   draw (per-tile pass): inlined inside FUN_100012278
//   dtor:                 FUN_100007df0 (a no-op in the binary)
//
// 0x140-byte node added to TileObject's decoration list at TileObject+0x228.
// each tile holds zero or more decorations stacked in front of the hex face,
// each rendering a small overlay icon (and, for kind=2 specifically, a
// numeric value rendered as digits via the embedded ColorTint).
//
// the decoration list is sentinel-style and sorted by `kind` ascending:
// FUN_100013870 walks until it finds a node with kind >= argument, inserts
// before it, and updates the tile's stored count.
//
// kind values observed in the binary:
//   0  generic indicator. UV depends on whether the tile's snagContent is
//        alive and the param_4 flag. alive: (0.3877, 0.000)..(0.4977, 0.249);
//        dead: (0.2754, 0.000)..(0.3879, 0.249). size (0.1781, 0.2031). plays
//        sound 0x3C.
//   1  "Darkness obscured" indicator (the "?" symbol on top of a tile).
//        UV (0.5000, 0.0000)..(0.65875, 0.15...) on the UI atlas. size
//        (0.11875, 0.1515625). pos offset (0.00625, 0.0078125). silent.
//        added to newly-drawn rack tiles when snag kind 0x15 ("Darkness") is
//        active on the board: populateRack pushes a kind=1 decoration onto
//        the slot-0 tile when `hasSnagInBoard(0x15)` returns true, hiding the
//        tile's contents from the player for a turn (the Darkness snag's
//        gameplay effect: players see "?" instead of knowing what they drew).
//        FUN_1000123c4 (the content-draw helper inside FUN_100012278) walks
//        the decoration list and, if it finds a kind=1 node with
//        `suppressed == 0`, returns early without drawing the underlying
//        TileContent / SnagContent sprite, leaving only the question mark
//        visible. setting `suppressed = 1` (e.g. by re-pushing kind=1 on
//        a tile that already has one) reveals the content again with a
//        fade-in animation seeded by the +0xF0 progress flip.
//   2  numeric value indicator. UV (0.0, 0.1426)..(0.125, 0.252). size
//        (0.2, 0.109375). pos offset (0, -0.09375). stores `value` at +0xF8,
//        renders it as digits via the embedded ColorTint. plays sound 0x3D.

struct TileDecoration {
    // intrusive doubly-linked list: prev at +0, next at +8. matches the
    // libc++ std::list node header (prev/next pointers), but the rest of
    // the body is 0x130 bytes of decoration-specific state, too large
    // for the std::list value-stored idiom we use elsewhere. the list
    // is therefore walked by hand via prev/next instead of being typed
    // as std::list<TileDecoration>.
    // confirmed by FUN_1000124ac's walk (`plVar5 = (long *)plVar5[1]`
    // advances forward via byte 8) and FUN_1000140b8's insertion writes.
    TileDecoration* prev;             // +0x000  (= sentinel.prev when self)
    TileDecoration* next;             // +0x008  (forward iteration pointer)
    int             kind;             // +0x010
    uint8_t         pad014[4];        // +0x014

    // sub-icon Quad. its posX/posY at +0x0C0/+0x0C4 (Quad+0xA8 inside the
    // node) get mirrored from the parent tile's mainQuad position by
    // TileObject::setPosition.
    Quad            iconSubQuad;      // +0x018..+0x0EF (0xD8; owns anim rect at +0xE0)

    // alpha-fade animation timer (0..1). advances at rate 2/sec while the
    // decoration is on-screen. when suppressed flips on, the timer (which
    // is currently at 1.0) gets re-seeded by FUN_100013870 to start the
    // fade-out ramp. consumed by TileObject::update's decoration walk
    // (port of FUN_1000124ac's plVar5 + 0x1e read).
    float           alphaT;           // +0x0F0
    bool            suppressed;       // +0x0F4  content-hide gate. when 0
                                      //   (not suppressed), the decoration is
                                      //   actively hiding: for kind=1
                                      //   (Darkness ?) the underlying
                                      //   TileContent / SnagContent sprite is
                                      //   not drawn (FUN_1000123c4 returns
                                      //   early). flip to 1 (effect suppressed)
                                      //   via FUN_100013bfc to reveal the
                                      //   content again with the alphaT
                                      //   fade-in. rack-flip-tile orientation
                                      //   lives on a separate field
                                      //   (flipRackTiles), not here.
    uint8_t         pad0F5[3];        // +0x0F5
    int             value;            // +0x0F8  (kind=2 only; the displayed
                                      //          numeric value)
    uint8_t         pad0FC[4];        // +0x0FC

    // ColorTint at +0x100. used only for kind=2 decorations (the tint renders
    // the numeric value as small digit Quads). for kind=0 / kind=1 the tint
    // exists in memory but is never rendered.
    ColorTint       colorTint;        // +0x100..+0x137  (0x38 bytes)

    // sub-position offset for the icon relative to the tile center. differs
    // per kind: kind=0 -> (0, 0); kind=1 -> (0.00625, 0.0078125); kind=2 ->
    // (0, -0.09375). read by TileObject::setPosition when propagating the
    // tile's pos into the iconSubQuad.
    float           subOffsetX;       // +0x138
    float           subOffsetY;       // +0x13C
};

static_assert(sizeof(TileDecoration) == 0x140,
              "TileDecoration must be exactly 0x140 bytes (binary uses "
              "operator_new(0x140) in FUN_1000140b8)");
