#pragma once

#include "quad.h"
#include <cstdint>

// reconstructed from Ghidra:
//   initVisuals: FUN_10004ad80
//   draw:        FUN_10004be04
//   update:      FUN_10004b574
//
// the TitleMenu is the visual foundation layer: title screen, hex grid,
// particle effects. it's embedded in Game, size ~0x16D50 bytes.
// drawn FIRST (lowest layer), uses texture 8 (sheet1.png).
//
// all quads in this struct are full 0xD8-byte Quads. the Quad ctor only
// _memcpy's the first 0xD0 bytes, but the trailing 0x10 bytes are the
// Quad's animMin/animMax target rect (see quad.h).
//
// layout verified against decompiled draw (FUN_10004be04) and
// update (FUN_10004b574).

// a tile object. TileIcon is just a Quad; the animation-target rect
// (Quad::animMin* / animMax*) is part of Quad itself.
// the nested `.quad` field is kept so existing access patterns like
// `obj.quad.posX` continue to compile. zero size overhead because the
// substruct sits at offset 0 and we don't add any other fields.
struct TileIcon {
    Quad quad;
};

#define SMOKE_PARTICLE_COUNT 300
#define BOARD_GRID_ROWS 20
#define BOARD_GRID_COLS 2
#define BOARD_EXTRA_QUADS 3

class TitleMenu {
public:
    // FUN_10004a774 (via thunk_FUN_10004ad7c): construct all Quads with proper
    // UVs, sizes, positions. called from the game constructor (FUN_1000437a4),
    // separately from initVisuals which is called from Game::init (FUN_100045250).
    void construct();

    // FUN_10004ad80: set up visuals (particles, grid, overlays). args
    // gate the title-menu indicator buttons:
    //   hasKeys: shop indicator (binary: 0 < shop.keys)
    //   hasAnyScore: leaderboard + achievements indicators (binary:
    //                FUN_100037e88(leaderboardMenu) = any of the 3
    //                ScoreHistory lists non-empty)
    void initVisuals(bool hasKeys, bool hasAnyScore);

    // FUN_10004be04: draw all board elements
    void draw();

    // FUN_10004b574: update particle positions, rotation, fading, touch
    // soundQueue = pointer to the game's sound queue (for triggering sounds).
    // touch state is read via getGame(); touch handling runs only when
    // interactable and getGame() is available.
    void update(float dt, bool interactable,
                class SoundQueue* soundQueue = nullptr);

    // get the rotation pivot point (first tile's position)
    float pivotX() { return tileGrid[0].quad.posX; }
    float pivotY() { return tileGrid[0].quad.posY; }

    // --- struct layout, byte-exact from decompilation ---

    bool visible;
    bool initialized;

    // fade overlay (drawn with no texture when fadeProgress < 1.0)
    TileIcon fadeOverlay;

    // fade progress (0 to 1; < 1 means title screen / fading in)
    float fadeProgress;

    // 300 particle objects (the swirling purple nebula)
    TileIcon particles[SMOKE_PARTICLE_COUNT];

    // tile grid (20 rows x 2 columns)
    // first tile's posX/posY used as rotation pivot
    TileIcon tileGrid[BOARD_GRID_ROWS * BOARD_GRID_COLS];

    float boardRotation;

    // cursor/selection mirror
    TileIcon cursorObj;

    // 3 title screen elements (gear parts / glow)
    TileIcon logoPieces[BOARD_EXTRA_QUADS];

    float colorCycleTimer;

    // main overlay/status quad
    TileIcon mainOverlayObj;

    // first byte past mainOverlayObj.
    bool overlayClickFlag;                     // (set during touch, cleared on release)
    bool startButtonClicked;                   // (set on successful release, read by game update)

    // indicator pair 1
    TileIcon shopObjA;
    TileIcon shopObjB;

    bool shopEnabled;
    bool shopHighlight;                  // (mirror during touch)
    bool shopClicked;                    // (result flag, read by game update)

    // indicator pair 2
    TileIcon leaderboardObjA;
    TileIcon leaderboardObjB;

    bool leaderboardEnabled;
    bool leaderboardHighlight;                  // (mirror during touch)
    bool leaderboardClicked;                    // (result flag, read by game update)

    // indicator pair 3
    TileIcon achievementsObjA;
    TileIcon achievementsObjB;

    bool achievementsEnabled;
    bool achievementsHighlight;                  // (mirror during touch)
    bool achievementsClicked;                    // (result flag, read by game update)

    // particle/rotation speed (set in update)
    float particleSpeed;
    float rotationSpeed;
    float titleAnimState;
    bool titleButtonPressed;
};
