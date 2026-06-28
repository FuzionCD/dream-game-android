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
// the decoration value stored in TileObject's std::list<DecorationValue> at
// TileObject.decorations. each tile holds zero or more decorations stacked in front
// of the hex face, each rendering a small overlay icon (and, for kind=2
// specifically, a numeric value rendered as digits via the embedded ColorTint).
//
// the list is sorted by `kind` ascending: pushDecoration walks until it finds a
// node with kind >= argument and inserts before it.
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
//        fade-in animation seeded by the alphaT progress flip.
//   2  numeric value indicator. UV (0.0, 0.1426)..(0.125, 0.252). size
//        (0.2, 0.109375). pos offset (0, -0.09375). stores `value`,
//        renders it as digits via the embedded ColorTint. plays sound 0x3D.

struct DecorationValue {
    int             kind;

    // sub-icon Quad. its posX/posY get mirrored from the parent tile's
    // mainQuad position by TileObject::setPosition.
    Quad            iconSubQuad;      // 0xD8 bytes; holds its own anim rect

    // alpha-fade animation timer (0..1). advances at rate 2/sec while the
    // decoration is on-screen. when suppressed flips on, the timer (which
    // is currently at 1.0) gets re-seeded by pushDecoration to start the
    // fade-out ramp. consumed by TileObject::update's decoration walk.
    float           alphaT;
    bool            suppressed;       // content-hide gate. when 0
                                      //   (not suppressed), the decoration is
                                      //   actively hiding: for kind=1
                                      //   (Darkness ?) the underlying
                                      //   TileContent / SnagContent sprite is
                                      //   not drawn. flip to 1 (effect
                                      //   suppressed) to reveal the content
                                      //   again with the alphaT fade-in. rack-
                                      //   flip-tile orientation lives on a
                                      //   separate field (flipRackTiles).
    int             value;            // (kind=2 only; the displayed
                                      //          numeric value)

    // ColorTint. used only for kind=2 decorations (the tint renders the numeric
    // value as small digit Quads). for kind=0 / kind=1 it's never rendered.
    ColorTint       colorTint;        // (0x38 bytes)

    // sub-position offset for the icon relative to the tile center. differs
    // per kind: kind=0 -> (0, 0); kind=1 -> (0.00625, 0.0078125); kind=2 ->
    // (0, -0.09375). read by TileObject::setPosition when propagating the
    // tile's pos into the iconSubQuad.
    float           subOffsetX;
    float           subOffsetY;
};
