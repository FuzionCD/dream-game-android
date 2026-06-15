#pragma once

#include "quad.h"
#include <cstdint>

// reconstructed from Ghidra:
//   initVisuals: FUN_10004ad80
//   draw:        FUN_10004be04
//   update:      FUN_10004b574
//
// the TitleMenu is the visual foundation layer: title screen, hex grid,
// particle effects. it's embedded at game+0x4460, size ~0x16D50 bytes.
// drawn FIRST (lowest layer), uses texture 8 (sheet1.png).
//
// all quads in this struct are full 0xD8-byte Quads. previously we modeled
// these as Quad (0xC8) + 0x10 bytes of "extra" because the Quad ctor only
// _memcpy's the first 0xD0 bytes, but the trailing 0x10 bytes at +0xC8..+0xD7
// are actually the Quad's animMin/animMax target rect (see quad.h).
//
// all offsets verified against decompiled draw (FUN_10004be04) and
// update (FUN_10004b574).

// tile object size matches sizeof(Quad) now that the animation target rect
// is part of Quad itself (was previously TileIcon's "extra[0x10]")
#define BOARD_TILE_OBJ_SIZE 0xD8

// a tile object. TileIcon is just a Quad; what used to be
// TileIcon::extra[0x10] is now Quad::animMin* / animMax* at +0xC8.
// the nested `.quad` field is kept so existing access patterns like
// `obj.quad.posX` continue to compile. zero size overhead because the
// substruct sits at offset 0 and we don't add any other fields.
struct TileIcon {
    Quad quad;
};
static_assert(sizeof(TileIcon) == BOARD_TILE_OBJ_SIZE, "TileIcon must be 0xD8 bytes");

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
    //   hasProgress: shop indicator (binary: 0 < shop.keys)
    //   hasAnyScore: leaderboard + achievements indicators (binary:
    //                FUN_100037e88(leaderboardMenu) = any of the 3
    //                ScoreHistory lists non-empty)
    void initVisuals(bool hasProgress, bool hasAnyScore);

    // FUN_10004be04: draw all board elements
    void draw();

    // FUN_10004b574: update particle positions, rotation, fading, touch
    // gameData = pointer to the game struct (for reading touch state)
    // soundQueue = pointer to the game's sound queue (for triggering sounds)
    void update(float dt, bool interactable, uint8_t* gameData = nullptr,
                class SoundQueue* soundQueue = nullptr);

    // get the rotation pivot point (first tile's position)
    float pivotX() { return tileGrid[0].quad.posX; }
    float pivotY() { return tileGrid[0].quad.posY; }

    // --- struct layout, byte-exact from decompilation ---

    // +0x0000
    bool visible;                              // +0x0000
    bool initialized;                          // +0x0001
    uint8_t pad02[6];                          // +0x0002

    // +0x0008: fade overlay (drawn with no texture when fadeProgress < 1.0)
    TileIcon fadeOverlay;                        // +0x0008

    // +0x00E0: fade progress (0 to 1; < 1 means title screen / fading in)
    float fadeProgress;                        // +0x00E0
    uint8_t padE4[4];                          // +0x00E4

    // +0x00E8: 300 particle objects (the swirling purple nebula)
    TileIcon particles[SMOKE_PARTICLE_COUNT];    // +0x00E8 to +0xFE07

    // +0xFE08: tile grid (20 rows x 2 columns)
    // first tile's posX/posY (+0xFEB0/+0xFEB4) used as rotation pivot
    TileIcon tileGrid[BOARD_GRID_ROWS * BOARD_GRID_COLS]; // +0xFE08 to +0x11FC7

    // +0x11FC8
    float boardRotation;                       // +0x11FC8
    uint8_t pad11FCC[4];                       // +0x11FCC

    // +0x11FD0: cursor/selection mirror
    TileIcon cursorObj;                          // +0x11FD0

    // +0x120A8: 3 title screen elements (gear parts / glow)
    TileIcon logoPieces[BOARD_EXTRA_QUADS];       // +0x120A8 to +0x1232F

    // +0x12330
    float colorCycleTimer;                     // +0x12330
    uint8_t pad12334[4];                       // +0x12334

    // +0x12338: main overlay/status quad
    TileIcon mainOverlayObj;                    // +0x12338

    // +0x12410: first byte past mainOverlayObj.
    bool overlayClickFlag;                     // +0x12410 (set during touch, cleared on release)
    bool startButtonClicked;                   // +0x12411 (set on successful release, read by game update)
    uint8_t pad12412[6];                       // +0x12412

    // +0x12418: indicator pair 1
    TileIcon indicatorObj1a;                    // +0x12418
    TileIcon indicatorObj1b;                    // +0x124F0

    // +0x125C8
    bool indicatorGroup1;                      // +0x125C8
    bool indicatorHighlight1;                  // +0x125C9 (mirror during touch)
    bool indicatorClicked1;                    // +0x125CA (result flag, read by game update)
    uint8_t pad125CB[5];                       // +0x125CB

    // +0x125D0: indicator pair 2
    TileIcon leaderboardObjA;                    // +0x125D0
    TileIcon leaderboardObjB;                    // +0x126A8

    // +0x12780
    bool leaderboardEnabled;                      // +0x12780
    bool leaderboardHighlight;                  // +0x12781 (mirror during touch)
    bool leaderboardClicked;                    // +0x12782 (result flag, read by game update)
    uint8_t pad12783[5];                       // +0x12783

    // +0x12788: indicator pair 3
    TileIcon achievementsObjA;                    // +0x12788
    TileIcon achievementsObjB;                    // +0x12860

    // +0x12938
    bool achievementsEnabled;                      // +0x12938
    bool achievementsHighlight;                  // +0x12939 (mirror during touch)
    bool achievementsClicked;                    // +0x1293A (result flag, read by game update)
    uint8_t pad1293B[1];                       // +0x1293B

    // +0x1293C: particle/rotation speed (set in update)
    float particleSpeed;                       // +0x1293C
    float rotationSpeed;                       // +0x12940
    float titleAnimState;                      // +0x12944
    bool titleButtonPressed;                   // +0x12948
};
