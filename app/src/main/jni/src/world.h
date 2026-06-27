#pragma once

#include "quad.h"
#include "title_menu.h"
#include "tile_collection.h"
#include <cstdint>
#include <set>

// reconstructed from Ghidra:
//   generate: FUN_100055970
//   draw:     FUN_100056048
//   update:   FUN_100055b94
//
// the world subsystem manages the world/level selection map.
// embedded at game+0x16DB0, drawn by the game's draw function
// when visible (checked at game+0x16DB0 byte 0).
//
// after the title screen transition (case 0), the board is hidden
// and the world map becomes visible. it shows a grid of available
// levels. when a level is selected (byte 2 flag set), the game
// update calls Level::generate to populate the menu.
//
// max tiles in the world grid. derived from binary layout: tiles[] starts at
// World+0x00E0 and tileTypes[] starts at World+0x1A30. the 0x1950-byte span
// holds exactly 30 x sizeof(TileIcon) (= 30 x 0xD8). likewise tileTypes[]
// occupies 0x1AA8 - 0x1A30 = 0x78 bytes = 30 ints.
#define WORLD_MAX_TILES 30

// difficulty button (from FUN_100055268)
// each button has two TileCollections (unselected/selected frame shapes)
// and two TileIcon quads (unselected/selected text labels)
struct DifficultyButton {
    TileCollection unselectedFrame; // +0x000: unselected 3-part button frame
    TileCollection selectedFrame;   // +0x098: selected 3-part button frame
    TileIcon unselectedText;        // +0x130: unselected text label quad
    TileIcon selectedText;          // +0x208: selected text label quad
};
static_assert(sizeof(DifficultyButton) == 0x2E0, "DifficultyButton must be 0x2E0 bytes");

class World {
public:
    // FUN_100055268: construct the world (tiles, buttons, etc.)
    void construct();

    // FUN_100055970: generate the world map grid. binary takes
    // (this, worldIndex, std::set<int>* tileTypeSet) where the set
    // holds the IDs of unlocked face portraits (= {0..29} - shop.facePool).
    // tile count derives from the set's size; tiles get populated by
    // rng-popping IDs from the set.
    void generate(uint32_t worldIndex, std::set<int>& tileTypeSet);

    // FUN_100056048: draw the world map
    void draw();

    // FUN_100055b94: update the world map (handles selection). touch state is
    // read via getGame(); touch handling runs only when getGame() is available.
    void update(float dt, class SoundQueue* soundQueue = nullptr);

    // back-button fade-out (no binary equivalent): the reverse of update()'s
    // fade-in ramp. fades the char-select content out to black at the same
    // rate; returns true once fully faded. Game::update drives this during
    // the character-select -> title back transition.
    bool tickFadeOut(float dt);

    // apply a uniform alpha to every tile + difficulty button. shared by the
    // fade-in (update) and the fade-out (tickFadeOut).
    void applyContentAlpha(uint8_t alpha);

    // --- struct layout from decompilation ---

    bool visible;          // +0x0000 (byte 0)
    bool active;           // +0x0001 (byte 1)
    bool characterSelected; // +0x0002 (byte 2, set when a character is picked, read by game update)
    uint8_t pad03[5];      // +0x0003

    // +0x0008: background overlay quad
    TileIcon backgroundOverlay;  // +0x0008

    // +0x00E0: world tile grid (level selection tiles).
    // 30 x sizeof(TileIcon) = 30 x 0xD8 = 0x1950, ending at +0x1A30.
    TileIcon tiles[WORLD_MAX_TILES];  // +0x00E0..+0x1A30

    // +0x1A30: tile type IDs for each grid slot
    int tileTypes[WORLD_MAX_TILES];  // +0x1A30..+0x1AA8

    // +0x1AA8: tile count
    int tileCount;         // +0x1AA8
    int selectedTile;      // +0x1AAC (which tile is selected, -1 = none)

    // +0x1AB0: 3 difficulty buttons (Easy, Normal, Hard)
    DifficultyButton diffButtons[3];  // +0x1AB0, stride 0x2E0, total 0x8A0

    // +0x2350
    float fadeInProgress;  // +0x2350 (0 to 1, controls tile fade-in)
    float selAnimProgress; // +0x2354 (0 to 1, selection animation progress)
    float selStartX;       // +0x2358 (selected tile start X for animation)
    float selStartY;       // +0x235C (selected tile start Y)
    float selTargetX;      // +0x2360 (target X = 0.5)
    float selTargetY;      // +0x2364 (target Y = virtualHeight * 0.5)

    // +0x2368: selected character type (portrait index from tileTypes[])
    // this is at game+0x19118, read by Level::generate as param_2[0x6446]
    int selectedCharType;  // +0x2368

    // +0x236C: difficulty/world index (0=easy, 1=normal, 2=hard)
    // this is at game+0x1911C, read by Level::generate as param_2[0x6447]
    uint32_t worldIndex;   // +0x236C
};

static_assert(sizeof(World) == 0x2370, "World object must be exactly 0x2370 bytes.");
