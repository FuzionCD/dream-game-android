#pragma once

#include <cstdint>

// AUTO-EXTRACTED from the Dream binary's master achievement table at
// DAT_10007d408. 50 entries x 0x28 bytes each. layout matches the binary 1:1:
//   +0x00  iconX        (pixel column on the achievement-icon spritesheet,
//                        always a multiple of 85)
//   +0x04  iconY        (pixel row, multiple of 85)
//   +0x08  sortKey      (display order in the AchievementsMenu, 1..50,
//                        accessed via FUN_10004d698)
//   +0x0C  targetCount  (0 = one-shot; >0 = N-progress, the "M" in N/M,
//                        accessed via direct read at DAT_10007d414+idx*0x28)
//   +0x10  assetKey     (internal / save-key identifier, e.g. "world_2";
//                        used as the GameKit ID prefix on iOS;
//                        accessor: FUN_10004d668)
//   +0x18  title        (big display title shown on the tile, e.g.
//                        "It Always Starts Like This";
//                        accessor: FUN_10004d608)
//   +0x20  description  (flavor / how-to text, e.g. "Reach world 2 on any
//                        difficulty"; accessor: FUN_10004d638)
//
// icon spritesheet layout (verified from FUN_10004d5c4): icons are 84x84 px
// on a 85-px-stride grid (1 px padding between cells to avoid bilinear-filter
// bleed). 12 columns x 5 rows seen across the 50 entries.

// AchievementId: name -> idx for all 50 achievements. names are PascalCase
// forms of each title; idx values match ACHIEVEMENT_TABLE below. use these at
// every fire site so a "who fires Symbiosis?" audit is a simple grep and so
// each `increment(AchievementId::Xxx)` call is self-documenting.
enum class AchievementId : uint32_t {
    ItAlwaysStartsLikeThis    = 0,   // world_2:           reach world 2 on any difficulty
    IntoTheDark               = 1,   // world_3_normal:    reach world 3 on Normal
    ThroughTheMirrorMaze      = 2,   // world_5_normal:    reach world 5 on Normal
    GazeIntoTheAbyss          = 3,   // world_7_normal:    reach world 7 on Normal
    FirstSteps                = 4,   // world_5_easy:      reach world 5 on Easy
    InTooDeep                 = 5,   // world_3_hard:      reach world 3 on Hard
    Symbiosis                 = 6,   // nemesis_consume:   nemesis consumes a special snag
    ASportingChance           = 7,   // nemesis_5:         nemesis lvl 5 with player still lvl 0
    DontLookBack              = 8,   // nemesis_10:        nemesis lvl 10
    ALonelyRoad               = 9,   // nemesis_hidden:    exit world 2+ without nemesis appearing
    JustAFleshWound           = 10,  // health_0 (x20):    drop to 0 HP 20 times
    AFreshStart               = 11,  // health_1:          start a world with 1 HP
    DoubleEdged               = 12,  // health_trap:       place HP tile with trap of same value
    TheEasyWay                = 13,  // health_full:       reach world 2 on Hard with no damage taken
    Opportunist               = 14,  // snag_200 (x200):   defeat 200 snags
    SpringCleaning            = 15,  // snag_discard:      discard 5 snags at once
    KeepRunning               = 16,  // snag_chasing:      5+ snags chasing
    LikeAGlove                = 17,  // item_ability:      find item with special ability
    IWantItAll                = 18,  // item_1_1_1:        item with bonus in all 3 stats
    PowerWithout              = 19,  // item_10:           item with 10+ in any stat
    ScavengerHunt             = 20,  // item_5:            reach world 3 normal/hard having found 5 items
    Recurring                 = 21,  // event_100 (x100):  activate 100 events
    JackOfAllTrades           = 22,  // event_types (x5):  activate all 5 types of events (set-counter)
    SpoiltForChoice           = 23,  // event_2:           hold 2 events at once
    NeverADullMoment          = 24,  // event_4:           hold 4 events at once
    EverythingIsDifferentNow  = 25,  // event_50 (x50):    activate 50 different events (set-counter)
    GottaCatchEmAll           = 26,  // special_100 (x100):defeat 100 different special snags (set-counter)
    WeHaveToGoBack            = 27,  // special_escape:    escape world holding 4 special snags
    OurPowersCombined         = 28,  // special_merge:     merge two special snags together
    LikeTearsInTheRain        = 29,  // special_discard:   discard a special snag
    TooManyKeys               = 30,  // exit_6_keys:       collect all six keys to an exit
    Breakthrough              = 31,  // exit_10 (x10):     unlock 10 exits
    SomethingWicked           = 32,  // super_1:           defeat a 3-XP special snag
    BigGame                   = 33,  // super_5 (x5):      defeat five 3-XP special snags
    Lucid                     = 34,  // control_50 (x50):  place 50 control tiles of value 2
    LostControl               = 35,  // control_discard:   discard three value-2 control tiles at once
    ABeautifulGarden          = 36,  // surround_tile:     surround a tile with 6 others
    TheSoundOfSilence         = 37,  // discard_blanks:    discard 5 blank tiles at once
    EasyComeEasyGo            = 38,  // discard_250 (x250):discard 250 tiles
    EndOfTheLine              = 39,  // cant_place_discard:hold 5 unplaceable/undiscardable tiles
    FocusOnThePain            = 40,  // pain:              place 30+ pain tile and survive
    SpiceOfLife               = 41,  // tile_special_100:  place 100 special tiles (no atk/def/hp/control/snag)
    KeepItSafe                = 42,  // secret:            exhaust a Secret tile
    GottaGoFast               = 43,  // 111_tiles:         beat world 2+ in 111 tiles or less
    YouShallNotPass           = 44,  // barricade:         place a Barricade tile on the exit
    Serendipity               = 45,  // 4_xp:              create an XP drop of value 4+
    WaxOnWaxOff               = 46,  // 100_atk_def:       100+ ATK and DEF
    TheWagesOfTruth           = 47,  // honesty_merge:     merge Honesty with a normal snag
    SomeoneIUsedToKnow        = 48,  // doppelganger:      face yourself and win
    PowerWithin               = 49,  // stat_10:           reach 10+ in any base stat
};

struct AchievementInfo {
    int32_t     iconX;
    int32_t     iconY;
    uint32_t    sortKey;
    uint32_t    targetCount;
    const char* assetKey;
    const char* title;
    const char* description;
};

static_assert(sizeof(AchievementInfo) == 0x28,
              "AchievementInfo must be 0x28 bytes to match the binary's "
              "table stride at DAT_10007d408.");

// FUN_10004d5c4 hardcodes the icon display size; mirrored here so callers
// (the menu's tile-icon Quad setSize) can refer to it by name.
inline constexpr float ACHIEVEMENT_ICON_SIZE_PX = 84.0f;

inline constexpr AchievementInfo ACHIEVEMENT_TABLE[50] = {
    // {iconX, iconY, sortKey, target, assetKey,                title,                          description}
    /* 00 */ {  255,    0, 0x01,    0, "world_2",                "It Always Starts Like This",   "Reach world 2 on any difficulty" },
    /* 01 */ {    0,    0, 0x03,    0, "world_3_normal",         "Into the Dark",                "Reach world 3 on Normal difficulty" },
    /* 02 */ {   85,    0, 0x04,    0, "world_5_normal",         "Through the Mirror Maze",      "Reach world 5 on Normal difficulty" },
    /* 03 */ {  170,    0, 0x05,    0, "world_7_normal",         "Gaze Into the Abyss",          "Reach world 7 on Normal difficulty" },
    /* 04 */ {  340,    0, 0x02,    0, "world_5_easy",           "First Steps",                  "Reach world 5 on Easy difficulty" },
    /* 05 */ {  425,    0, 0x06,    0, "world_3_hard",           "In Too Deep",                  "Reach world 3 on Hard difficulty" },
    /* 06 */ {   85,  340, 0x08,    0, "nemesis_consume",        "Symbiosis",                    "Use Nemesis to consume a special snag" },
    /* 07 */ {  510,   85, 0x09,    0, "nemesis_5",              "A Sporting Chance",            "Get Nemesis to level 5 without leveling up yourself" },
    /* 08 */ {  595,   85, 0x0a,    0, "nemesis_10",             "Don't Look Back",              "Get Nemesis to level 10" },
    /* 09 */ {  425,   85, 0x0b,    0, "nemesis_hidden",         "A Lonely Road",                "Exit world 2+ without Nemesis appearing" },
    /* 10 */ {  850,    0, 0x16,   20, "health_0",               "Just a Flesh Wound",           "Get down to 0 health 20 times" },
    /* 11 */ {  935,   85, 0x13,    0, "health_1",               "A Fresh Start",                "Start a world with 1 health" },
    /* 12 */ {  850,   85, 0x15,    0, "health_trap",            "Double Edged",                 "Place a health tile with a trap of the same value" },
    /* 13 */ {    0,  170, 0x14,    0, "health_full",            "The Easy Way",                 "Reach world 2 on Hard without taking any damage" },
    /* 14 */ {  765,   85, 0x1b,  200, "snag_200",               "Opportunist",                  "Defeat 200 snags" },
    /* 15 */ {  340,  255, 0x19,    0, "snag_discard",           "Spring Cleaning",              "Discard 5 snags at the same time" },
    /* 16 */ {  425,  255, 0x1a,    0, "snag_chasing",           "Keep Running",                 "Have 5 or more snags chasing you" },
    /* 17 */ {  170,  170, 0x1d,    0, "item_ability",           "Like a Glove",                 "Find an item with a special ability" },
    /* 18 */ {  255,  170, 0x1e,    0, "item_1_1_1",             "I Want It All",                "Find an item with a bonus in all 3 stats" },
    /* 19 */ {  340,  170, 0x1f,    0, "item_10",                "Power Without",                "Find an item with a 10+ in any stat" },
    /* 20 */ {  425,  170, 0x20,    0, "item_5",                 "Scavenger Hunt",               "Reach world 3 on Normal or Hard having found 5 items" },
    /* 21 */ {  595,  170, 0x26,  100, "event_100",              "Recurring",                    "Activate 100 events" },
    /* 22 */ {  680,  170, 0x23,    5, "event_types",            "Jack of All Trades",           "Activate all 5 types of events" },
    /* 23 */ {  765,  170, 0x24,    0, "event_2",                "Spoilt for Choice",            "Hold 2 events at the same time" },
    /* 24 */ {  850,  170, 0x25,    0, "event_4",                "Never a Dull Moment",          "Hold 4 events at the same time" },
    /* 25 */ {  935,  170, 0x27,   50, "event_50",               "Everything Is Different Now",  "Activate 50 different events" },
    /* 26 */ {  935,  255, 0x2c,  100, "special_100",            "Gotta Catch 'Em All",          "Defeat 100 different special snags" },
    /* 27 */ {  680,  255, 0x2b,    0, "special_escape",         "We Have to Go Back",           "Escape a world while holding 4 special snags" },
    /* 28 */ {  765,  255, 0x29,    0, "special_merge",          "Our Powers Combined",          "Merge two special snags together" },
    /* 29 */ {  850,  255, 0x2a,    0, "special_discard",        "Like Tears in the Rain",       "Discard a special snag" },
    /* 30 */ {  255,   85, 0x0d,    0, "exit_6_keys",            "Too Many Keys",                "Collect all six keys to an exit" },
    /* 31 */ {  340,   85, 0x0e,   10, "exit_10",                "Breakthrough",                 "Unlock 10 exits" },
    /* 32 */ {  170,  255, 0x2f,    0, "super_1",                "Something Wicked",             "Defeat a special snag worth 3 XP" },
    /* 33 */ {   85,  255, 0x30,    5, "super_5",                "Big Game",                     "Defeat 5 special snags worth 3 XP each" },
    /* 34 */ {  680,    0, 0x11,   50, "control_50",             "Lucid",                        "Place 50 control tiles of value 2" },
    /* 35 */ {  765,    0, 0x10,    0, "control_discard",        "Lost Control",                 "Discard three control tiles of value 2 at the same time" },
    /* 36 */ {  510,    0, 0x07,    0, "surround_tile",          "A Beautiful Garden",           "Completely surround a tile with 6 other tiles" },
    /* 37 */ {  595,    0, 0x12,    0, "discard_blanks",         "The Sound of Silence",         "Discard 5 blank tiles at the same time" },
    /* 38 */ {  935,    0, 0x22,  250, "discard_250",            "Easy Come, Easy Go",           "Discard 250 tiles" },
    /* 39 */ {    0,   85, 0x32,    0, "cant_place_discard",     "End of the Line",              "Hold 5 tiles that can't be placed or discarded" },
    /* 40 */ {   85,   85, 0x21,    0, "pain",                   "Focus on the Pain",            "Place a 30+ pain tile and survive" },
    /* 41 */ {  170,   85, 0x2d,  100, "tile_special_100",       "Spice of Life",                "Place 100 tiles without attack, defence, health, control, snag" },
    /* 42 */ {  680,   85, 0x18,    0, "secret",                 "Keep It Safe",                 "Exhaust a Secret tile" },
    /* 43 */ {   85,  170, 0x28,    0, "111_tiles",              "Gotta Go Fast",                "Beat world 2+ using 111 tiles or less" },
    /* 44 */ {  510,  170, 0x0c,    0, "barricade",              "You Shall Not Pass",           "Place a Barricade tile on the exit" },
    /* 45 */ {    0,  255, 0x17,    0, "4_xp",                   "Serendipity",                  "Create an XP drop of value 4 or higher" },
    /* 46 */ {  255,  255, 0x2e,    0, "100_atk_def",            "Wax On, Wax Off",              "Have 100+ attack and defence" },
    /* 47 */ {  510,  255, 0x1c,    0, "honesty_merge",          "The Wages of Truth",           "Merge Honesty with a normal snag" },
    /* 48 */ {  595,  255, 0x0f,    0, "doppelganger",           "Someone I Used to Know",       "Face yourself and come out on top" },
    /* 49 */ {    0,  340, 0x31,    0, "stat_10",                "Power Within",                 "Reach 10+ in any of your stats" },
};
