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
// vector<TileIcon>  items        (the drawn quads)
// vector<int>       modes        (per-item mode: 1=stretch, 2=fixed)
// vector<int64_t>   itemExtra    (per-item 8-byte data)
// vector<int>       displayParam (per-item display param)
// vector<int>       additional   (per-item additional data)
// int               cachedValue
// float             scale        (texture size, typically 1024.0)
// float             totalWidth   (cached total width)
// float             posX
// float             posY
// bool              rotated      (draw with 90 degree rotation)

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
    std::vector<TileIcon> items;
    std::vector<int> modes;
    std::vector<int64_t> itemExtra;
    std::vector<int> displayParams;
    std::vector<int> additionalData;

    int stagingValue;    // (temp slot: caller writes here, pushItem copies to additionalData)
    float scale;         // (texture size, typically 1024.0)
    float totalWidth;
    float posX;
    float posY;
    bool rotated;
};
