#pragma once

#include "quad.h"

// reconstructed from Ghidra:
//   table:   DAT_10007dbd8  (30 entries x 4 ints: x, y, w, h in pixel coords)
//   helper:  FUN_100056478  (apply portrait UV + size to a Quad)
//
// the 30-entry portrait table maps character indices 0..29 to sprite regions
// in sheet1.png (texture 8). used by the character-select screen (world.cpp)
// to render selectable portraits, and by PlayerSystem to set the in-game
// character token's sprite.

namespace {
    // pixel-space UV scale (texture is 1024 wide). DAT_10005a6e8.
    constexpr float UV_SCALE   = 1.0f / 1024.0f;
    // display-space size scale. DAT_10005a6ec.
    constexpr float SIZE_SCALE = 640.0f;

    struct PortraitEntry { int x, y, w, h; };

    // 30 character portraits, extracted from binary DAT_10007dbd8.
    constexpr PortraitEntry sPortraits[30] = {
            {  0, 935,  71, 89}, { 72, 935,  65, 89}, {138, 935,  78, 89},
            {217, 935,  78, 89}, {296, 935,  74, 89}, {371, 935,  82, 89},
            {454, 935,  79, 89}, {534, 935,  75, 89}, {610, 936,  72, 88},
            {683, 937,  77, 87}, {761, 934,  75, 90}, {837, 934,  71, 90},
            {909, 934,  80, 90}, {  0, 842,  73, 92}, { 74, 842,  78, 92},
            {153, 844,  74, 90}, {228, 844,  89, 90}, {318, 844,  84, 90},
            {403, 844,  83, 90}, {487, 844,  74, 90}, {562, 844,  87, 90},
            {650, 844,  81, 90}, {732, 844,  85, 89}, {818, 844,  80, 89},
            {899, 848,  93, 85}, {  0, 755,  79, 86}, { 80, 755,  81, 86},
            {162, 756,  77, 87}, {240, 756,  78, 87}, {319, 758,  85, 85}
    };
}

void setPortraitVisual(int characterIndex, Quad& q);
