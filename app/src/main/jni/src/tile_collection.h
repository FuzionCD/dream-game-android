#pragma once

#include "title_menu.h"
#include <vector>
#include <cstdint>

// reconstructed from decompilation:
//   constructor: FUN_10004c014
//   push item:   FUN_10004c310
//   draw:        FUN_10004c09c
//   get width:   FUN_10004c61c
//   get left X:  FUN_10004c93c
//   set position: FUN_10004c4f8
//   hit test:    FUN_10004c8cc
//   set alpha:   FUN_10004c84c
//
// a collection of TileIcon objects used for composite UI elements.
// the original uses 5 parallel std::vectors to store per-item data.
// total struct size: 0x98 bytes.
//
// struct layout:
//   +0x00: vector<TileIcon>  items        (the drawn quads)
//   +0x18: vector<int>       modes        (per-item mode: 1=stretch, 2=fixed)
//   +0x30: vector<int64_t>   itemExtra    (per-item 8-byte data)
//   +0x48: vector<int>       displayParam (per-item display param)
//   +0x60: vector<int>       additional   (per-item additional data)
//   +0x78: int               cachedValue
//   +0x7C: float             scale        (texture size, typically 1024.0)
//   +0x80: float             (padding/unused)
//   +0x84: float             totalWidth   (cached total width)
//   +0x88: bool              flag88
//   +0x8C: float             posX
//   +0x90: float             posY
//   +0x94: bool              rotated      (draw with 90 degree rotation)

class TileCollection {
public:
    void init();
    void pushItem(float displayParam, float* pixelPos, float* pixelSize,
                  int mode, int extraData);
    void draw();
    float getWidth();
    float getLeftX();
    void setPosition(float leftX, float y);
    bool hitTest(float touchX, float touchY, float height);
    void setAlpha(uint8_t alpha);

    // FUN_10004c150: recalculate all item sizes and positions based on modes
    void recalculate();

    // 5 parallel vectors (matching the original's internal layout)
    std::vector<TileIcon> items;         // +0x00
    std::vector<int> modes;              // +0x18
    std::vector<int64_t> itemExtra;      // +0x30
    std::vector<int> displayParams;      // +0x48
    std::vector<int> additionalData;     // +0x60

    int stagingValue;    // +0x78 (temp slot: caller writes here, pushItem copies to additionalData)
    float scale;         // +0x7C (texture size, typically 1024.0)
    float pad80;         // +0x80
    float totalWidth;    // +0x84
    bool flag88;         // +0x88
    uint8_t pad89[3];    // +0x89
    float posX;          // +0x8C
    float posY;          // +0x90
    bool rotated;        // +0x94
    uint8_t pad95[3];    // +0x95
};
static_assert(sizeof(TileCollection) == 0x98, "TileCollection must be 0x98 bytes");
