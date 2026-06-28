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
// embedded in the Game struct, drawn by the game's draw function
// when its visible flag is set.
//
// after the title screen transition (case 0), the board is hidden
// and the world map becomes visible. it shows a grid of available
// levels. when a level is selected (byte 2 flag set), the game
// update calls Level::generate to populate the menu.
//
// max tiles in the world grid. derived from binary layout: tiles[] starts at
// World.tiles and tileTypes[] starts at World.tileTypes. the 0x1950-byte span
// holds exactly 30 x sizeof(TileIcon) (= 30 x 0xD8). likewise tileTypes[]
// occupies 0x78 bytes = 30 ints.
#define WORLD_MAX_TILES 30

// difficulty button (from FUN_100055268)
// each button has two TileCollections (unselected/selected frame shapes)
// and two TileIcon quads (unselected/selected text labels)
struct DifficultyButton {
    TileCollection unselectedFrame; // unselected 3-part button frame
    TileCollection selectedFrame;   // selected 3-part button frame
    TileIcon unselectedText;        // unselected text label quad
    TileIcon selectedText;          // selected text label quad
};

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

    bool visible;
    bool active;
    bool characterSelected; // (set when a character is picked, read by game update)

    // background overlay quad
    TileIcon backgroundOverlay;

    // world tile grid (level selection tiles).
    // 30 x sizeof(TileIcon) = 30 x 0xD8 = 0x1950.
    TileIcon tiles[WORLD_MAX_TILES];

    // tile type IDs for each grid slot
    int tileTypes[WORLD_MAX_TILES];

    // tile count
    int tileCount;
    int selectedTile;      // (which tile is selected, -1 = none)

    // 3 difficulty buttons (Easy, Normal, Hard)
    DifficultyButton diffButtons[3];

    float fadeInProgress;  // (0 to 1, controls tile fade-in)
    float selAnimProgress; // (0 to 1, selection animation progress)
    float selStartX;       // (selected tile start X for animation)
    float selStartY;       // (selected tile start Y)
    float selTargetX;      // (target X = 0.5)
    float selTargetY;      // (target Y = virtualHeight * 0.5)

    // selected character type (portrait index from tileTypes[])
    // read by Level::generate from the Game struct.
    int selectedCharType;

    // difficulty/world index (0=easy, 1=normal, 2=hard)
    // read by Level::generate from the Game struct.
    uint32_t worldIndex;
};
