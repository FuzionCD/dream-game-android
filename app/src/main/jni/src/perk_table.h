#pragma once

// reconstructed from Ghidra:
//   FUN_100042444, global static initializer (cxa_atexit registers this) that
//                  populates the DAT_10007e268 + perkType * 0x40 table with
//                  per-perk (name, category, warnLine2 UV, flags) at startup.
//   FUN_100041aa4, lazy initializer that calls FUN_10004223c for each
//                  (perkType, level) entry, populating per-perk std::vector
//                  of level descriptors at DAT_10007e290 + perkType * 0x40 + 0x28.
//
// our port replaces both with the static tables below; the data is constant
// at startup so no runtime "init" pass is needed.
//
// per-perk table (24 entries, perkType 0..23):
//   - perkTypes 0/4/8 are the stat-bump perks (ATK/DEF/HP). they're not in
//     FUN_100041aa4's registration list and have an empty `levels` vector;
//     their descriptions are hardcoded inside Perk::getDescriptionLine.
//   - perkTypes 1..23 (excluding 4, 8) are the real perk choices the level-up
//     panel rolls between.
//
// warnLine1 vs warnLine2 (per FUN_100041840):
//   - warnLine1 = the per-category glyph at DAT_10007d348 + category * 0x10.
//     atlas y=950, 74x74, columns at 0/75/150/.../450 px. these are the 7
//     stat-class icons (events / generic-stat / CTRL / HP / DEF / ATK / items
//     in category order 0..6).
//   - warnLine2 = the per-perk glyph above the category icon. each entry's
//     icon2_atlasX/Y/W/H taken from the per-perk table.
//
// pattern matches snag_table.h: inline constexpr in the header so the
// storage lives in one TU and callers can read the table without an extra
// .cpp file.

struct PerkLevelEntry {
    int         unlockLevel;   // FUN_10004223c param_3, XP level required to unlock
    const char* line0;
    const char* line1;
};

struct PerkTypeEntry {
    const char* name;
    int         category;        // 0..6, indexes into CATEGORY_ICON_UV[]
    int         icon2_atlasX;
    int         icon2_atlasY;
    int         icon2_atlasW;
    int         icon2_atlasH;
    int         icon2_offsetXpx;   // FUN_100041840 reads these as flag1/flag2; divided
    int         icon2_offsetYpx;   // by 640 to produce warnLine2.posX/posY sub-pixel nudges.
    const PerkLevelEntry* levels;
    int         levelCount;
};

struct CategoryIconUV {
    int atlasX;
    int atlasY;
    int atlasW;
    int atlasH;
};

// DAT_10007d348 + category * 0x10. all at atlas y=950, 74x74 each.
inline constexpr CategoryIconUV CATEGORY_ICON_UV[7] = {
    {   0, 950, 74, 74 },   // 0 = events
    {  75, 950, 74, 74 },   // 1 = generic stat-bump
    { 150, 950, 74, 74 },   // 2 = CTRL
    { 225, 950, 74, 74 },   // 3 = HP
    { 300, 950, 74, 74 },   // 4 = DEF
    { 375, 950, 74, 74 },   // 5 = ATK
    { 450, 950, 74, 74 },   // 6 = items
};

// ---- per-perk level descriptor arrays ----
//
// each array transcribes one perkType's FUN_10004223c call sequence from
// FUN_100041aa4. perkType is 1-indexed (level 1 -> entry 0), matching the
// binary's `perkLevel * 0x18 - 0x10 + lineIdx * 8` offset math in
// FUN_1000421a4.

inline constexpr PerkLevelEntry PERK_1_LEVELS[] = {
    { 5,  "Gain 1 {A} per turn while your", "{H} is less than half max {H}" },
    { 12, "Gain 2 {A} per turn while your", "{H} is less than half max {H}" },
    { 20, "Gain 3 {A} per turn while your", "{H} is less than half max {H}" },
};
inline constexpr PerkLevelEntry PERK_2_LEVELS[] = {
    { 0,  "Gain 1 {A} {D} per discarded {H}", "" },
    { 5,  "Gain 2 {A} {D} per discarded {H}", "" },
    { 10, "Gain 3 {A} {D} per discarded {H}", "" },
};
inline constexpr PerkLevelEntry PERK_3_LEVELS[] = {
    { 3,  "Gain 2 {A} per defeated snag", "" },
    { 10, "Gain 5 {A} per defeated snag", "" },
};
inline constexpr PerkLevelEntry PERK_5_LEVELS[] = {
    { 2,  "Gain 1 {D} per turn while your", "{D} is less than your {A}" },
    { 8,  "Gain 2 {D} per turn while your", "{D} is less than your {A}" },
    { 12, "Gain 3 {D} per turn while your", "{D} is less than your {A}" },
};
inline constexpr PerkLevelEntry PERK_6_LEVELS[] = {
    { 2,  "Gain 1 {D} per placed {A} tile", "" },
    { 6,  "Gain 2 {D} per placed {A} tile", "" },
    { 10, "Gain 3 {D} per placed {A} tile", "" },
    { 15, "Gain 4 {D} per placed {A} tile", "" },
};
inline constexpr PerkLevelEntry PERK_7_LEVELS[] = {
    { 6,  "Draw {D} tile per discarded snag", "" },
};
inline constexpr PerkLevelEntry PERK_9_LEVELS[] = {
    { 3,  "Refills your current {H} to 75%",  "when your {H} reaches 0" },
    { 6,  "Refills your current {H} to 100%", "when your {H} reaches 0" },
};
inline constexpr PerkLevelEntry PERK_10_LEVELS[] = {
    { 0,  "Discarding with a {H} tile while", "full {H} won't advance Nemesis" },
    { 1,  "Discarding with a {H} tile",       "won't advance Nemesis" },
};
inline constexpr PerkLevelEntry PERK_11_LEVELS[] = {
    { 4,  "Gain 1 {H} per turn while", "holding at least 2 {H} tiles" },
    { 10, "Gain 2 {H} per turn while", "holding at least 2 {H} tiles" },
    { 15, "Gain 3 {H} per turn while", "holding at least 2 {H} tiles" },
    { 20, "Gain 4 {H} per turn while", "holding at least 2 {H} tiles" },
};
inline constexpr PerkLevelEntry PERK_12_LEVELS[] = {
    { 8,  "Stats increase by 2 on level up", "" },
    { 15, "Stats increase by 3 on level up", "" },
};
inline constexpr PerkLevelEntry PERK_13_LEVELS[] = {
    { 4,  "Lowest stat increases by an", "extra +1 on level up" },
    { 12, "Lowest stat increases by an", "extra +2 on level up" },
};
inline constexpr PerkLevelEntry PERK_14_LEVELS[] = {
    { 4,  "Can choose 2 stats or 2 perks", "on level up" },
};
inline constexpr PerkLevelEntry PERK_15_LEVELS[] = {
    { 2,  "25% chance of drawing a",  "2 {C} tile instead of 1 {C}" },
    { 6,  "50% chance of drawing a",  "2 {C} tile instead of 1 {C}" },
    { 10, "75% chance of drawing a",  "2 {C} tile instead of 1 {C}" },
    { 15, "100% chance of drawing a", "2 {C} tile instead of 1 {C}" },
};
inline constexpr PerkLevelEntry PERK_16_LEVELS[] = {
    { 1,  "{C} spots are closer together", "" },
};
inline constexpr PerkLevelEntry PERK_17_LEVELS[] = {
    { 6,  "Gain 1 {C} when defeating a", "non-zero {X} special snag" },
    { 14, "Gain 2 {C} when defeating a", "non-zero {X} special snag" },
};
inline constexpr PerkLevelEntry PERK_18_LEVELS[] = {
    { 0,  "Can hold 1 event at a time",  "" },
    { 3,  "Can hold 2 events at a time", "" },
    { 7,  "Can hold 3 events at a time", "" },
    { 12, "Can hold 4 events at a time", "" },
};
inline constexpr PerkLevelEntry PERK_19_LEVELS[] = {
    { 4,  "Choose from 4 events", "One event starts with 1 charge" },
};
inline constexpr PerkLevelEntry PERK_20_LEVELS[] = {
    { 2,  "25% chance for events to",  "start with 1 charge" },
    { 6,  "50% chance for events to",  "start with 1 charge" },
    { 12, "100% chance for events to", "start with 1 charge" },
};
inline constexpr PerkLevelEntry PERK_21_LEVELS[] = {
    { 4,  "25% chance of +2 to primary",  "stat on item upgrade" },
    { 8,  "75% chance of +2 to primary",  "stat on item upgrade" },
    { 12, "100% chance of +2 to primary", "stat on item upgrade" },
};
inline constexpr PerkLevelEntry PERK_22_LEVELS[] = {
    { 4,  "50% chance of 1 item ability",  "" },
    { 8,  "75% chance of 1 item ability",  "25% chance of 2 item abilities" },
    { 12, "100% chance of 1 item ability", "75% chance of 2 item abilities" },
};
inline constexpr PerkLevelEntry PERK_23_LEVELS[] = {
    { 3,  "Choose from 4 items. One item", "gets +1 to secondary stat" },
};

// ---- top-level per-perk table ----
//
// each row mirrors one DAT_10007e268 + perkType * 0x40 entry from
// FUN_100042444. category and warnLine2 UV are the iOS atlas-pixel coordinates.
// flag1/flag2 (icon2_offsetXpx/Ypx) stay opaque pending a use-site that needs them.

inline constexpr PerkTypeEntry PERK_TYPE_TABLE[24] = {
    // perkType 0: stat-bump ATK (no level entries; description hardcoded)
    { "Strong",        5, 0x2c7, 0x3d4, 0x2b, 0x2c, 1, 1, nullptr,         0 },
    { "Dire",          5, 0x39f, 0x374, 0x30, 0x2f, 1, 2, PERK_1_LEVELS,   3 },
    { "Daring",        5, 0x37b, 0x3a4, 0x2d, 0x2c, 1, 1, PERK_2_LEVELS,   3 },
    { "Confident",     5, 0x33f, 0x373, 0x2d, 0x2e, 1, 2, PERK_3_LEVELS,   2 },

    // perkType 4: stat-bump DEF
    { "Tough",         4, 0x29c, 0x3d4, 0x2a, 0x2c, 1, 1, nullptr,         0 },
    { "Balanced",      4, 0x36d, 0x371, 0x31, 0x2f, 1, 1, PERK_5_LEVELS,   3 },
    { "Happy",         4, 0x2e2, 0x377, 0x2c, 0x2f, 1, 1, PERK_6_LEVELS,   4 },
    { "Cautious",      4, 0x34d, 0x3a2, 0x2d, 0x2e, 2, 1, PERK_7_LEVELS,   1 },

    // perkType 8: stat-bump HP
    { "Resilient",     3, 0x26e, 0x3d8, 0x2d, 0x28, 2, 2, nullptr,         0 },
    { "Refreshing",    3, 0x31e, 0x3a2, 0x2e, 0x2e, 1, 1, PERK_9_LEVELS,   2 },
    { "Measured",      3, 0x3d0, 0x37b, 0x30, 0x2f, 1, 1, PERK_10_LEVELS,  2 },
    { "Calm",          3, 0x30f, 0x371, 0x2f, 0x30, 1, 2, PERK_11_LEVELS,  4 },

    { "Experienced",   1, 0x3a9, 0x3a4, 0x26, 0x2c, 2, 1, PERK_12_LEVELS,  2 },
    { "Capable",       1, 0x290, 0x3ad, 0x2e, 0x26, 1, 2, PERK_13_LEVELS,  2 },
    { "Focused",       1, 0x321, 0x3d1, 0x2d, 0x2f, 1, 2, PERK_14_LEVELS,  1 },

    { "Controlled",    2, 0x2f3, 0x3d4, 0x2d, 0x2c, 2, 1, PERK_15_LEVELS,  4 },
    { "Patient",       2, 0x2bf, 0x3aa, 0x30, 0x29, 2, 2, PERK_16_LEVELS,  1 },
    { "Introspective", 2, 0x260, 0x3a6, 0x2f, 0x31, 2, 2, PERK_17_LEVELS,  2 },

    { "Eventful",      0, 0x3d9, 0x3d1, 0x27, 0x2f, 2, 2, PERK_18_LEVELS,  4 },
    { "Perceptive",    0, 0x3d0, 0x3ab, 0x30, 0x25, 2, 2, PERK_19_LEVELS,  1 },
    { "Sudden",        0, 0x2f0, 0x3a7, 0x2d, 0x2c, 2, 1, PERK_20_LEVELS,  3 },

    { "Potent",        6, 0x3a9, 0x3d1, 0x2f, 0x2f, 1, 2, PERK_21_LEVELS,  3 },
    { "Rewarding",     6, 0x37f, 0x3d1, 0x29, 0x2f, 2, 1, PERK_22_LEVELS,  3 },
    { "Flexible",      6, 0x34f, 0x3d1, 0x2f, 0x2f, 2, 1, PERK_23_LEVELS,  1 },
};

constexpr int PERK_TYPE_COUNT = 24;
