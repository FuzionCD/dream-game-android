#pragma once

#include <cstdint>

// AUTO-EXTRACTED from the Dream binary's hex-map cell table at DAT_10007ae10.
// 8 entries x 0x30 bytes each (4 UV ints + 4 string pointers). consumed by
// the HexMap visualization + the DetailPanel HexMap-cell populator:
//   FUN_10003b404, lookup UV (uPx, vPx, uExtPx, vExtPx) by kind
//   FUN_10003b568, lookup name string by kind
//   FUN_10003b59c, lookup desc N (0..2) by kind
//
// kind 0 is a sentinel (no cell); kinds 1..7 map onto the cell types the
// player can step onto. order matches the binary's HexMapCell.kind field.

struct HexMapCellInfo {
    int         uPx, vPx;       // pixel coords on the hex-map atlas
    int         uExtPx, vExtPx; // pixel dimensions
    const char* name;
    const char* desc1;
    const char* desc2;
    const char* desc3;
};

inline constexpr HexMapCellInfo kHexMapTable[8] = {
    /*0x0*/ {  0,   0,  0,  0, "",          "",                                "",                                "" },
    /*0x1 Exit       */ {322, 270, 93, 49, "Exit",
                          "Unlock all the locks and",
                          "place a tile here to move on.", ""},
    /*0x2 Exit Key   */ {376, 530, 76, 70, "Exit Key",
                          "Place a tile on this key to",
                          "unlock one of the exit locks.", ""},
    /*0x3 Threat     */ {308, 512, 66, 83, "Threat",
                          "Placing a tile on this token will",
                          "cause you to draw a normal snag.", ""},
    /*0x4 Control    */ {454, 537, 70, 67, "Control",
                          "Placing a {C} tile on this token will",
                          "give you {C} equal to that tile's value.", ""},
    /*0x5 Good Luck  */ {494, 246, 69, 68, "Good Luck",
                          "Placing a tile on this",
                          "token will give you 1 {X}", ""},
    /*0x6 Weakness   */ {433, 454, 75, 75, "Weakness",
                          "Snags placed on this token",
                          "will lose half of their {A}", ""},
    /*0x7 Talent     */ {644, 246, 73, 74, "Talent",
                          "{A} {H} {D} tiles placed on this token",
                          "give double their value.", ""},
};

// FUN_10003b404, read (uPx, vPx, uExtPx, vExtPx) for a hex-map cell kind.
// silently returns without writing when kind is out of range (binary's
// `param_1 - 1 < 7` guard, i.e. 1..7).
inline void lookupHexMapCellUVPx(uint32_t kind, float* uvOriginPx, float* uvSizePx) {
    if (kind - 1 < 7) {
        const HexMapCellInfo& e = kHexMapTable[kind];
        uvOriginPx[0] = (float)e.uPx;
        uvOriginPx[1] = (float)e.vPx;
        uvSizePx[0]   = (float)e.uExtPx;
        uvSizePx[1]   = (float)e.vExtPx;
    }
}

// FUN_10003b568, name for a hex-map cell kind, or "" when out of range.
inline const char* hexMapCellName(uint32_t kind) {
    if (kind - 1 < 7) {
        return kHexMapTable[kind].name;
    }
    return "";
}

// FUN_10003b59c, desc line N (0..2) for a hex-map cell kind.
inline const char* hexMapCellDesc(uint32_t kind, int n) {
    if (kind - 1 >= 7) {
        return "";
    }
    const HexMapCellInfo& e = kHexMapTable[kind];
    switch (n) {
        case 0:  return e.desc1;
        case 1:  return e.desc2;
        case 2:  return e.desc3;
        default: return "";
    }
}
