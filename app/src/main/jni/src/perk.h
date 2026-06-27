#pragma once

#include "color_tint.h"
#include "title_menu.h"   // for TileIcon
#include <cstdint>

// reconstructed from Ghidra:
//   constructor:    FUN_100041840
//   pool registrar: FUN_10004223c (called many times by FUN_100041aa4)
//   pool init:      FUN_100041aa4 (lazy: guarded by `DAT_10007e2d0 == DAT_10007e2d8`)
//   name lookup:    FUN_10004218c (returns DAT_10007e268[perkType*0x10])
//
// each Perk is a 0x1F0-byte object allocated via operator_new and pushed into
// PlayerSystem.perks (the std::vector<Perk*> at +0x188). a Perk represents
// a passive bonus the player has unlocked at some level; gameplay code
// queries `playerSystem.perkLevel(perkType)` to scale stat-tile magnitudes,
// open extra event slots, gate cell-predicate parity, etc.
//
// ---- runtime perk pool (DAT_10007e290..+0x300) ----
// FUN_100041aa4 registers ~50 entries via FUN_10004223c, each entry a
// (perkType, levelMax, unlockCost, line1, line2) tuple. perkTypes observed
// in the binary (extracted from FUN_100041aa4 by Claude in a prior session):
//
//   0x01  "Gain N {A} per turn while your {H} is less than half max {H}"  (levels 5/12/20)
//   0x02  "Gain N {A}{D} per discarded {H}"                                (levels 0/5/10)
//   0x03  "Gain N {A} per defeated snag"                                   (levels 3/10)
//   0x05  "Gain N {D} per turn while your {D} is less than your {A}"       (levels 2/8/12)
//   0x06  "Gain N {D} per placed {A} tile"                                 (levels 2/6/10/15)
//   0x07  "Draw {D} tile per discarded snag"                               (level 6)
//   0x09  "Refills your current {H} to N% when your {H} reaches 0"         (levels 3/6, "75%/100%")
//          consumed by GameBoard::applyOnDeathHpRefill via setHP.
//   0x0A  "Discarding with a {H} tile while full {H} won't advance Nemesis" (levels 0/1)
//   0x0B  "Gain N {H} per turn while holding at least 2 {H} tiles"          (levels 4/10/15/20)
//   0x0C  "Stats increase by N on level up"                                  (levels 8/15)
//   0x0D  "Lowest stat increases by an extra +N on level up"                (levels 4/12)
//   0x0E  "Can choose 2 stats or 2 perks on level up"                       (level 4)
//   0x0F  "P% chance of drawing a 2 {C} tile instead of 1 {C}"              (levels 2/6/10/15,
//          "25%/50%/75%/100%"). consumed by GameBoard::rollContentMagnitude
//          case 5 (the CTRL-tile magnitude path).
//   0x10  "{C} spots are closer together"                                   (level 1).
//          consumed by GameBoard::controlTileCellPredicate (parity bucket).
//   0x11  "Gain N {C} when defeating a non-zero {X} special snag"           (levels 6/14)
//   0x12  "Can hold N events at a time"                                     (levels 0/3/7/12,
//          "1/2/3/4 events"). consumed by the events-bar wrapper.
//   0x13  "Choose from 4 events; one event starts with 1 charge"            (level 4)
//   0x14  "P% chance for events to start with 1 charge"                     (levels 2/6/12,
//          "25%/50%/100%")
//   0x15  "P% chance of +2 to primary stat on item upgrade"                 (levels 4/8/12,
//          "25%/75%/100%")
//   0x16  "P% chance of N item abilities"                                   (levels 4/8/12).
//          consumed by Item ctor (FUN_10003040c) to roll how many
//          SpecialAbilities each new Item gets.
//   0x17  "Choose from 4 items; one item gets +1 to secondary stat"         (level 3).
//          consumed by FUN_100035218 (item-choice list builder).
//
// types 0x04 and 0x08 are absent from FUN_100041aa4: they're the DEF and
// HP stat-bump perks (alongside type 0 = ATK), handled by the level-up
// panel's stat slots rather than as choosable perks.
//
// all per-type data (name, category, icon UV, per-level descriptors) is
// transcribed in perk_table.h. Perk::init reads from there to populate
// warnLine1/warnLine2/tint; the binary's runtime FUN_100042444 + FUN_100041aa4
// initializer pair is replaced by that static table.
//
// Perk.perkLevel selects which entry from the per-type vector applies
// to this player's instance: higher levels = stronger bonus (e.g., perk
// 0x0F level 1 = "25% chance of 2 {C}", level 4 = "100% chance").

class Perk {
public:
    // FUN_100041840. fully ported now. populates warnLine1 (category glyph),
    // warnLine2 (per-perk glyph), and tint (numeric level overlay) from
    // perk_table.h. perkLevel clamping mirrors the binary: stat-bump
    // perks (perkType in {0, 4, 8}) skip the clamp; others get pinned
    // to [1, levelCount].
    void init(int perkType, int perkLevel);

    // FUN_10004218c. simple table lookup; returns PERK_TYPE_TABLE[perkType].name.
    const char* getName() const;

    // FUN_1000421a4. for stat-bump perkTypes (0, 4, 8), returns hardcoded
    // "Increases your {A/D/H} stat by 1" on line 0 and "" on line 1+.
    // for normal perks, returns PERK_TYPE_TABLE[perkType].levels[perkLevel-1].line0
    // / line1, or "" for line 2+. matches the binary's branch order.
    const char* getDescriptionLine(int lineIdx) const;

    // FUN_1000420bc. set alpha on warnLine1/warnLine2/tint, bind tex 12, push a
    // matrix translation to (posX, posY), draw warnLine2 (under) then warnLine1
    // (transparent-center frame on top), then the tint, and pop.
    //
    // icon order matters: warnLine2 = per-perk glyph (smaller, ~45px); warnLine1 =
    // per-category frame (larger, 74px, with transparent center). warnLine2
    // shows through the frame's hole.
    void drawAt(float posX, float posY, uint8_t alpha);

    // --- byte-exact struct fields ---
    int32_t  perkType;     // +0x000  index into the runtime pool (1..0x17)
    int32_t  perkLevel;    // +0x004  current level (1..N), 0 = not owned
    TileIcon icon1;        // +0x008..+0x0DF
    TileIcon icon2;        // +0x0E0..+0x1B7
    ColorTint tint;        // +0x1B8..+0x1EF
};
static_assert(sizeof(Perk) == 0x1F0,
              "Perk must be exactly 0x1F0 bytes (binary uses operator_new(0x1F0))");
