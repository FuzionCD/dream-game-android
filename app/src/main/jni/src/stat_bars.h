#pragma once

#include "quad.h"
#include <cstddef>
#include <cstdint>

// reconstructed from Ghidra:
//   constructor: FUN_10003bc20
//   draw:        FUN_10003be88
//   update:      FUN_10003beec
//
// StatBars lives at GameBoard.statBars (0x1230 bytes). it's the row of 20
// stat-bar segment Quads that animate in along the bottom of the gameplay
// HUD (the "ATK / DEF / HP" tier bar visualization). draws in tex 9 / ui1.png.
//
// the binary's update walks 20 slots of stride 0xE8 (a Quad plus per-slot
// scratch). each slot's scratch holds (animT, targetX, targetY, alphaTarget)
// plus a few uncharacterized floats. the update advances animT, then writes the
// quad's posX/posY/scale/alpha as a quadratic-ease scale-in animation.

// a Quad (0xD8) plus 0x10 of per-slot animation state. Quad owns the
// animation target rect internally.
struct StatBarSlot {
    Quad    quad;          // Quad init'd by FUN_10003bc20 (0xD8)
    float   targetX;       // translate-to X (scaled by animT * animT)
    float   targetY;       // translate-to Y
    float   alphaTarget;   // alpha endpoint for the fade-in
    float   animT;         // 0..1 animation timer (advances each update)
};

class StatBars {
public:
    // FUN_10003bc20, clears visible byte + default-constructs all 20 Quads.
    void init();

    // FUN_10003be88, early-out if !visible; otherwise translate to (posX, posY)
    // and dispatch each slot's Quad::draw via vtable. caller binds tex 9 first.
    void draw();

    // FUN_10003beec, animation tick. advances animT for every slot, writes
    // the resulting posX/posY/scale/alpha onto each Quad. clears `visible`
    // once all slots reach animT == 1.0 (the row has finished settling).
    void update(float dt);

    // FUN_10003bc94, burst 20 icon particles outward from `tilePos`
    // (= committed tile center) when the player consumes a tile. each
    // particle is a sub-quad UV'd to the tile's content-type icon
    // (looked up by FUN_100014980 from `contentType`); per-slot delay
    // staggers their fly-out so the burst reads as an explosion.
    // called by the post-tile-commit dispatcher in
    // updateNavArrowAndConfirmDrag's second switch.
    void spawnIconBurst(const float* tilePos, int contentType);

    // --- byte-exact struct fields ---

    bool    visible;
    float   posX;                  // draw translate X
    float   posY;                  // draw translate Y

    StatBarSlot slots[20];         // (20 * 0xE8 = 0x1220)
};
