#pragma once

#include "quad.h"
#include "title_menu.h"   // for TileIcon
#include "color_tint.h"
#include <cstdint>

// reconstructed from Ghidra:
//   constructor:        FUN_1000088f0
//   reset:              FUN_10000996c
//   setNemesisLevel:    FUN_100008f64
//   setNemesisXP:       FUN_100008fec
//   placeOnHexGrid:     FUN_100008dc0
//   placeWrap:          FUN_100009998
//   update:             FUN_100009094
//   drawSegments:       FUN_1000096fc
//   drawFrame:          FUN_100009790
//
// NemesisRenderable lives at GameBoard.nemesis (0x3A30 bytes). it's the visual
// for the Nemesis enemy: an instant-kill antagonist that follows the player
// each turn, consuming the path behind them. parts of this object:
//   - 25 hex segments (the Nemesis's body, made of pieces). animated by the
//     "reform" cycle in update(): on each move the segments swirl out and
//     re-form to face the new direction.
//   - a center hex piece (the Nemesis's "core")
//   - a bg circle (the visual frame around the Nemesis)
//   - 20 outline dots in a circle: the Nemesis's XP track. each lit dot = 1 XP.
//   - 20 fill dots in a circle: triggered during a level-up cascade when XP
//     caps out (also fires the cascade SFX).
//   - 2 ColorTints layered on top: tintLevelNumber renders the Nemesis's
//     current level in the center; tintLevelUpFlash flashes during cascade.
//
// each "segment slot" is 0xE0 bytes: a Quad (0xD8, includes its animation
// target rect) plus 0x08 bytes of segment-specific scratch (animation
// phase + per-segment random scale endpoint).

struct NemesisSegment {
    Quad quad;             // (0xD8, owns its anim rect)
    uint8_t scratch[0x08]; // (used by update for reform phase)
};

class NemesisRenderable {
public:
    // FUN_1000088f0: zero state + init all quads with their UVs and place the
    // 20 XP dots in a circle around (center.x, center.y). segments stay un-UV'd
    // until placeOnHexGrid runs.
    void init();

    // FUN_10000996c: just clears the visibility byte.
    void reset();

    // FUN_100008f64: rebuilds the two ColorTints to display the given level
    // number, plus mirrors the count into nemesisLevel and cascadeLevelMirror.
    // shown in the center of the Nemesis sprite.
    void setNemesisLevel(int level);

    // FUN_100008fec: sets XP count [0..20]. first N outline dots get full
    // alpha (filled-in look), remainder get dim (outline only). all fill dots
    // are alpha 0 here; the level-up cascade animates them in separately.
    void setNemesisXP(int count);

    // FUN_100008dc0: per-level setup. takes the Nemesis's hex grid coords
    // and facing direction (0..5; index 4 = 0deg, each step = 60deg).
    // sets up the 25 body segments and seeds the reform animation state.
    void placeOnHexGrid(int64_t hexGridConfig, float facingDir);

    // FUN_100009094: per-frame animation. drives the reform cycle, position
    // transition (when the Nemesis moves), bg fade-in, and level-up cascade.
    void update(float dt);

    // FUN_1000096fc: draws the 25 body segments + center quad with rotation
    // applied (rotation tracks the path being consumed).
    void drawSegments();

    // FUN_100009790: draws the bg circle, optional fill dots (cascade),
    // outline dots, and the two ColorTints. no rotation.
    void drawFrame();

    // FUN_1000098d0: add `amount` to the XP track. nemesisXP accumulates in
    // [0, 20); every overflow of 20 bumps nemesisLevel by one. amount also
    // accumulates into pendingXP (a separate "still to animate" buffer that
    // the level-up cascade reads). if the cascade was idle (fillTimer >= 1)
    // when amount arrives, restart it (fillTimer = 0, fillFlag = false).
    // no-op when amount <= 0.
    void creditXP(int amount);

    // FUN_100009874: start a move-to-cell animation. seeds (gridCol,
    // gridRow, facingDir), backs up (posX, posY) -> (posStartX, posStartY),
    // computes (posTargetX, posTargetY) from the destination cell, resets
    // posTransitionT = 0, writes transitionSpeed = durationScale.
    void beginMoveTo(float durationScale, int32_t col, int32_t row, int facing);

    // --- byte-exact struct fields ---

    bool visible;

    NemesisSegment segments[25]; // (the Nemesis's body)

    TileIcon centerQuad;         // (the core)
    TileIcon bgQuad;             // (frame around the body)

    // hex grid coordinates of the Nemesis on the level map. placeOnHexGrid
    // computes posX from these via FUN_100012f04. read by the page-list
    // occupancy check (FUN_100020880) and several Nemesis-flow helpers.
    int32_t nemesisGridCol;
    int32_t nemesisGridRow;

    // facing direction (0..5; FUN_100016b18 picks based on player vs Nemesis
    // grid position). update() re-reads this when a transition completes to
    // recompute rotation: (int)facingDir - 4) * 60 degrees.
    float facingDir;

    float posX;
    float posY;
    float rotation;

    // animation timers, all 0..1:
    //   reformT: per-move re-formation of the body segments
    //   posTransitionT: lerps posX/Y from start to target during a move
    //   bgFadeT: bg circle / outline fade-in when the Nemesis first appears
    float reformT;
    float posTransitionT;
    float bgFadeT;

    float posStartX;
    float posStartY;
    float posTargetX;
    float posTargetY;

    float transitionSpeed;       // (multiplier on transition rates)
    int32_t nemesisLevel;        // (mirror of setNemesisLevel for tint 1)
    int32_t nemesisXP;           // (clamped 0..20, mirror for tint 2)

    ColorTint tintLevelNumber;   // (level digits in center)
    ColorTint tintLevelUpFlash;  // (cascade flash overlay)

    TileIcon outlineDots[20];    // (XP track, dim outline)
    TileIcon fillDots[20];       // (level-up cascade fill)

    // level-up cascade state
    int32_t cascadeLevelMirror;  // (mirror of nemesisLevel for fill tint)
    int32_t cascadeActiveIndex;  // (cycles 0..19 during cascade)
    int32_t pendingXP;           // (creditXP adds here; fade-in consumes)
    float fillTimer;             // (init 1.0; <1.0 means cascade running)
    bool fillFlag;               // (init 0; toggles cascade direction)

    // Nemesis "eat" cycle state. when the player is downed (HP=0), Nemesis
    // starts consuming trail tiles from the oldest end. these fields drive
    // that cycle from death through sated revive:
    //   eatTarget:  total tiles to consume this cycle (= clamp of nemesisLevel
    //               against page-XP-tile count). set in setHP's death path.
    //   eatActive:  feeding cycle in flight. true from death to revive.
    //   eatStep:    per-batch step countdown. FUN_100020aac decrements.
    //               when 0 + eatActive -> state-7 routes to state-8 (revive).
    //   eatFired:   per-tick "already fired this state-7 tick" guard.
    //               cleared at the start of each step; set on advance fire.
    // names map to the game's audio vocabulary (sound 0x28/0x29/0x2A =
    // "eat content / snag / XP"), so these read as "the eating cycle."
    int32_t eatTarget;
    bool    eatActive;
    int32_t eatStep;
    bool    eatFired;
};

// constants from FUN_1000088f0 (constructor) and FUN_100009094 (update),
// extracted from binary __DATA section. the binary stores some values twice
// (PI, 180, -90, 60) at adjacent DAT addresses; we dedupe to one constant each.
namespace NemesisRenderableConstants {
    // shared math constants (DAT_100059bec/bf4/bf8/bf0 and DAT_100059c14/c20/c28)
    inline constexpr float DEG_360              = 360.0f;
    inline constexpr float DEG_180              = 180.0f;
    inline constexpr float DEG_POS90            =  90.0f;
    inline constexpr float DEG_NEG90            = -90.0f;
    inline constexpr float PI                   = 3.1415927f;

    // XP-dot ring layout
    inline constexpr float DOT_CIRCLE_RADIUS    = 0.07968750f;// DAT_100059bfc

    // rotation rates: 60 used both as init "deg per segment-index step" and
    // update "deg/s spin rate" (DAT_100059c00 and DAT_100059c0c).
    inline constexpr float ROT_DEG_PER_STEP     = 60.0f;

    // body-segment positioning + animation
    inline constexpr float SEG_SPACING          = 0.04f;       // DAT_100059c04
    inline constexpr float SEG_PHASE_HALF       = 0.3f;        // DAT_100059c08
    inline constexpr float SEG_AMPL_X           = -0.196875f;  // DAT_100059c10
    inline constexpr float SEG_AMPL_X_OFFSET    = -0.046875f;  // DAT_100059c18
    inline constexpr float SEG_PHASE_RATE       = 0.05f;       // DAT_100059c1c
    inline constexpr float SEG_RADIUS_X         = 0.110938f;   // DAT_100059c2c
    inline constexpr float SEG_RADIUS_Y         = 0.15f;       // DAT_100059c30

    // alpha / scale (cascade animation)
    inline constexpr float ALPHA_FULL           = 255.0f;      // DAT_100059c34
    inline constexpr float ALPHA_DIM            =  80.0f;      // DAT_100059c38
    inline constexpr float FILL_SCALE_BIG       =   1.8f;      // DAT_100059c3c
}
