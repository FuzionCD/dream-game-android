#pragma once

#include "tile_weight_pool.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

// ----------------------------------------------------------------------
// per-slot format version magic. each save blob's first 4 bytes hold one of
// these values; the slot loader rejects the blob if the magic doesn't match.
// bump the value when a slot's wire format changes so old saves get silently
// discarded instead of mis-parsed.
//
// iOS leaves all magics at 0 (memset default); there's no versioning at
// all in the original. our port writes a real constant so future format
// changes are safe.
// ----------------------------------------------------------------------
namespace SaveMagic {
constexpr int32_t SAV = 1;   // GameSnapshot
constexpr int32_t SET = 1;   // Settings
constexpr int32_t UNL = 1;   // PersistentUnlocks (shop)
constexpr int32_t SCO = 1;   // Leaderboard
constexpr int32_t ACH = 1;   // Achievements
}  // namespace SaveMagic

// ----------------------------------------------------------------------
// Save buffer types referenced by the Game struct.
//
// the iOS binary persists 5 NSData blobs to NSUserDefaults keyed by 3-char
// names: "sav", "set", "unl", "sco", "ach". each blob has a 4-byte version
// magic as its first field; at load time the loader compares against the
// game's in-memory expected magic and skips the blob on mismatch. our
// Android port writes the same blobs to internal-storage files instead, but
// keeps the binary-layout struct fields at the same offsets so the loader/
// encoder ports stay 1:1 with the Ghidra anchors.
//
// the FIVE blobs map to FIVE distinct buffer regions in the Game struct:
//   slot 0 ("sav"): GameSnapshot          at Game+0x2E348 (0x370 bytes)
//   slot 1 ("set"): inline settings       at Game+0x2E6DC (4-field group)
//   slot 2 ("unl"): PersistentUnlocks     at Game+0x2E710 (0x50  bytes)
//   slot 3 ("sco"): leaderboard xfer      at Game+0x2E780 (magic+pad+vector)
//   slot 4 ("ach"): AchievementSaveBuffer at Game+0x2E7C0 (0x88  bytes)
//
// loader / encoder / builder Ghidra anchors:
//   slot 0: FUN_10004762c / FUN_1000463a4    (builder: FUN_1000269b8)
//   slot 1: FUN_1000481f8 / inline in FUN_1000462ac
//   slot 2: FUN_100048278 / FUN_100046ffc
//   slot 3: FUN_1000483e8 / FUN_10004719c
//   slot 4: FUN_1000484e0 / FUN_1000472cc
// ----------------------------------------------------------------------

// ----------------------------------------------------------------------
// GameSnapshot sub-structs.
// ----------------------------------------------------------------------

// Per-rack tile snapshot (0x48 bytes, 5 contiguous in GameSnapshot).
// populated by FUN_100013dd8 (per-tile extractor) from each rack[i] in the
// builder; consumed by FUN_100016b18's rack-tile allocation pass.
struct RackTileSnapshot {
    uint32_t  gridIdx;             // +0x00  TileObject+0xDC (functional type 0..23)
    bool      mirror;              // +0x04  TileObject+0xE0 (50% RNG h-flip)
    uint8_t   pad005[3];           // +0x05
    uint32_t  rotationStep;        // +0x08  TileObject+0xE4 (0..5; rot = step*60)
    uint32_t  contentType;         // +0x0C  TileContent+0x134 (0..0x19); gates +0x10
    int32_t   contentMagnitude;    // +0x10  TileContent+0x13C (i16; only if contentType!=0)
    uint32_t  snagKind;            // +0x14  SnagContent+0x134 (0..0x76); gates the snag block
    uint32_t  snagHp;              // +0x18  SnagContent+0x13C (u16)
    uint32_t  snagAtk;             // +0x1C  SnagContent+0x140 (u16)
    uint32_t  snagDef;             // +0x20  SnagContent+0x144 (u16)
    int32_t   snagConsumedFlag;    // +0x24  SnagContent+0x490 (i16; only if snagHasExtras)
    int32_t   snagObsessionCount;  // +0x28  SnagContent+0x494 (i16; only if snagKind == 6)
    uint8_t   pad02C[4];           // +0x2C
    std::vector<int64_t> decorations;  // +0x30..+0x47, active decoration
                                       //                (kind:i32, value:i32) pairs from
                                       //                tile.decorList (filter:
                                       //                suppressed == 0). packed as
                                       //                low/high halves of int64.
};
static_assert(sizeof(RackTileSnapshot) == 0x48,
              "RackTileSnapshot must be 0x48 bytes (5-wide rack stride)");

// Per reserve-item snapshot (0x30 bytes per entry in reserveItems vector).
// reserveItem source = the doubly-linked tile-reserve list at gb+0x96D8;
// the prefix int comes from the list node itself, the rest from the
// embedded TileObject* via FUN_100013dd8 (no coords vec).
struct ReserveItemSnapshot {
    int32_t   listSlotIndex;       // +0x00  list node +0x18 (signed char source)
    uint32_t  gridIdx;             // +0x04  TileObject+0xDC
    bool      mirror;              // +0x08  TileObject+0xE0
    uint8_t   pad009[3];           // +0x09
    uint32_t  rotationStep;        // +0x0C  TileObject+0xE4
    uint32_t  contentType;         // +0x10  TileContent+0x134; gates +0x14
    int32_t   contentMagnitude;    // +0x14  TileContent+0x13C (i16)
    uint32_t  snagKind;            // +0x18  SnagContent+0x134; gates the snag block
    uint32_t  snagHp;              // +0x1C  SnagContent+0x13C
    uint32_t  snagAtk;             // +0x20  SnagContent+0x140
    uint32_t  snagDef;             // +0x24  SnagContent+0x144
    int32_t   snagConsumedFlag;    // +0x28  SnagContent+0x490 (i16; only if snagHasExtras)
    int32_t   snagObsessionCount;  // +0x2C  SnagContent+0x494 (i16; only if snagKind == 6)
};
static_assert(sizeof(ReserveItemSnapshot) == 0x30,
              "ReserveItemSnapshot must be 0x30 bytes "
              "(reserveItems stride)");

// Per placed-tile snapshot (0x14 bytes per entry in placedTiles vector).
// placedTile source = gb.placedTiles doubly-linked list at gb+0x1A8; the
// (col, row) prefix comes from TileObject+0xE8/+0xEC (committed coords),
// the rest from the embedded TileObject via FUN_100013dd8.
struct PlacedTileSnapshot {
    int32_t   col;                 // +0x00  TileObject+0xE8 (i16 in blob -> i32)
    int32_t   row;                 // +0x04  TileObject+0xEC (i16 in blob -> i32)
    uint32_t  gridIdx;             // +0x08  TileObject+0xDC
    bool      mirror;              // +0x0C  TileObject+0xE0
    uint8_t   pad00D[3];           // +0x0D
    uint32_t  rotationStep;        // +0x10  TileObject+0xE4
};
static_assert(sizeof(PlacedTileSnapshot) == 0x14,
              "PlacedTileSnapshot must be 0x14 bytes "
              "(placedTiles stride)");

// Per stat-block snapshot (0x24 bytes; 3 contiguous in GameSnapshot).
// one per PlayerSystem.baseItems[i]. extracted via FUN_100033750: 5 Item
// header ints, then 2 SpecialAbility (type, value) pairs from each Item's
// abilities[0..1] array. Item.type isn't stored (implicit by slot index).
struct StatBlockSnapshot {
    uint32_t  itemSubType;         // +0x00  Item+0x004
    uint32_t  itemCosmeticNameIdx; // +0x04  Item+0x014
    uint32_t  itemAtk;             // +0x08  Item+0x008
    uint32_t  itemDef;             // +0x0C  Item+0x00C
    uint32_t  itemHp;              // +0x10  Item+0x010
    uint32_t  spa0Type;            // +0x14  Item.abilities[0].abilityType
    uint32_t  spa0Value;           // +0x18  Item.abilities[0].abilityVal
    uint32_t  spa1Type;            // +0x1C  Item.abilities[1].abilityType
    uint32_t  spa1Value;           // +0x20  Item.abilities[1].abilityVal
};
static_assert(sizeof(StatBlockSnapshot) == 0x24,
              "StatBlockSnapshot must be 0x24 bytes (3-wide statBlocks stride)");

// Per hex-map vector entry (0x10 bytes in hexMapVec). HexMap source is
// gb.hexMap (std::map<Key, HexMapCell>); FUN_10003b614 filters in only
// entries with HexMapCell.kind > 2 and records (key.first, key.second,
// kind, fadeTimer).
struct HexMapEntry {
    int32_t   col;                 // +0x00  hexMap key.first (i32)
    int32_t   row;                 // +0x04  hexMap key.second (i32)
    uint32_t  kind;                // +0x08  HexMapCell.kind (+0x000)
    uint32_t  fadeTimer;           // +0x0C  HexMapCell.fadeTimer (+0x004)
};
static_assert(sizeof(HexMapEntry) == 0x10,
              "HexMapEntry must be 0x10 bytes (hexMapVec stride)");

// Per placedSnagMap value (24 bytes, the std::map value type). holds the snag
// stats for one placed tile that carries a SnagContent: the same 6-field
// block SnagContent::initExplicit consumes, {kind, hp, atk, def, extra0,
// extra1}. extra0/extra1 are only meaningful (and only present in the blob)
// when snagHasExtras(kind), the same gate as the rack/reserve snag blocks.
struct PlacedSnagFields {
    uint32_t  kind, hp, atk, def, extra0, extra1;
};
static_assert(sizeof(PlacedSnagFields) == 24,
              "PlacedSnagFields must be 24 bytes (placedSnagMap value stride)");

// ----------------------------------------------------------------------
// slot 0 GameSnapshot (Game+0x2E348..+0x2E6B8, 0x370 bytes).
//
// frozen mid-run state. populated by FUN_1000269b8 (the builder, called from
// GameBoard::update tail when the level ends) and consumed by FUN_100016b18
// (the restorer, called from Game::update case 1 when transitionTarget == 1,
// i.e. the Start button picks "Continue" because hasSavedRun != 0).
//
// field semantics derived from FUN_10004762c (loader) read sites and
// FUN_1000269b8 (builder) read sources. opaque-named fields are ones whose
// semantic the builder hasn't surfaced yet (will be pinned in H/G4 once the
// restorer port shows their destinations).
// ----------------------------------------------------------------------
struct GameSnapshot {
    int32_t   versionMagic;      // +0x000  slot 0 magic

    // 7-int run counter block. copied from gb+0x20..+0x3B by the builder;
    // restored to gb+0x20..+0x3B by FUN_100016b18. blob compresses to
    // u16/u8 per slot; zero-extended on read.
    uint32_t  totalTurnCount;        // +0x004 <- u16, gb+0x20
    uint32_t  worldLevelIndex;       // +0x008 <- u8 , gb+0x24
    uint32_t  snagsDefeated;         // +0x00C <- u16, gb+0x28
    uint32_t  specialSnagsDefeated;  // +0x010 <- u16, gb+0x2C
    uint32_t  levelsGained;          // +0x014 <- u8 , gb+0x30
    uint32_t  itemsFound;            // +0x018 <- u8 , gb+0x34
    uint32_t  eventsFired;           // +0x01C <- u16, gb+0x38

    uint32_t  worldIndex;            // +0x020 <- u8 , gb.worldIndex (+0x08)
    bool      tutorialFlag;          // +0x024 <- u8 == 1, gb.tutorialFlag (+0x0C)
    uint8_t   pad025[3];             // +0x025

    // 24-byte hint bitfield. populated from blob[14..17] (uint32) via
    // SIMD bit-expand iff tutorialFlag was 1 in the source blob; else
    // zeroed. each byte = 1 bit of the 24 low bits of the source uint.
    // builder source: gb+0x63F8..+0x640F (inside the HUD region).
    uint8_t   hintFlags[24];         // +0x028..+0x03F

    uint32_t  gridLayout;            // +0x040 <- u8 , gb.gridLayout (+0x120, cosmetic 0..11)
    int32_t   exitCol;               // +0x044 <- i16, gb.exitCol (+0x9708)
    int32_t   exitRow;               // +0x048 <- i16, gb.exitRow (+0x970C)
    uint32_t  keysRequired;          // +0x04C <- u8 , gb.keysRequired (+0x9B48)
    uint32_t  levelTurnCount;        // +0x050 <- u16, gb.levelTurnCount (+0x40)
    uint32_t  pickupSnagThreshold;   // +0x054 <- u16, gb.pickupSnagThreshold (+0x44)

    // vector head at +0x058, bytes read as ints. clone of gb.variantsUsed
    // (gb+0x128, std::vector<int>) via FUN_100028b48 (vector::assign).
    std::vector<int>  variantsUsed;  // +0x058..+0x06F

    // vector head at +0x070. clone of gb.animBannerSeedHistory
    // (gb+0x9DD0, std::vector<int64_t>) via FUN_100028648.
    std::vector<int64_t>  animBannerSeedHistory;  // +0x070..+0x087

    RackTileSnapshot  rackTiles[5];  // +0x088..+0x1EF

    std::vector<ReserveItemSnapshot>  reserveItems;  // +0x1F0..+0x207

    std::vector<PlacedTileSnapshot>   placedTiles;   // +0x208..+0x21F

    // per-placed-tile CONTENT side table at +0x220. key = the tile's index
    // in the page list (0-based, oldest first); value = packed
    // (TileContent.type | TileContent.displayedMagnitude << 32). nearly every
    // placed tile has content (its pickup magnitude), so this is mostly dense;
    // the only misses are blank tiles cleared by a snag effect / event
    // (contentType 0). PlacedTileSnapshot holds the geometry (col/row/gridIdx/
    // mirror/rotationStep); content is split out here because it's optional.
    std::map<int, uint64_t>  placedContentMap;            // +0x220..+0x237

    // per-placed-tile SNAG side table at +0x238. key = the tile's page-list
    // index; value = the snag's {kind, hp, atk, def, extra0, extra1} block.
    // sparse: only path tiles the player walked onto that still carry a live
    // SnagContent enemy get an entry.
    std::map<int, PlacedSnagFields>  placedSnagMap;       // +0x238..+0x24F

    // PlayerSystem + HUD-stat snapshot, 9 uints (0x24 bytes total).
    // builder fills slots 0/3..8 from PlayerSystem (FUN_100056ba8) and
    // slots 1/2 from GameplayHUD (FUN_10000d9c4 overwrites the zeroed
    // slots with hud.xpReceivedTotal / hud.controlReceivedTotal).
    uint32_t  characterIndex;        // +0x250 <- u8 , PlayerSystem+0x134
    uint32_t  xpReceivedTotal;       // +0x254 <- u8 , GameplayHUD+0x12E8
    uint32_t  controlReceivedTotal;  // +0x258 <- u8 , GameplayHUD+0x1BB8
    uint32_t  attack;                // +0x25C <- u16, PlayerSystem+0x144
    uint32_t  defence;               // +0x260 <- u16, PlayerSystem+0x148
    uint32_t  currentHealth;         // +0x264 <- u16, PlayerSystem+0x13C
    uint32_t  baseATK;               // +0x268 <- u8 , PlayerSystem+0x14C
    uint32_t  baseDEF;               // +0x26C <- u8 , PlayerSystem+0x150
    uint32_t  baseHP;                // +0x270 <- u8 , PlayerSystem+0x154

    StatBlockSnapshot statBlocks[3]; // +0x274..+0x32F

    // vector at +0x2E0, Perk snapshot. one int64 per PlayerSystem.perks
    // entry (+0x188); FUN_1000423f8 packs (perk.perkType, perk.perkLevel)
    // into the low/high 4 bytes of each int64 slot. restored via
    // Perk(type, level) ctor in FUN_100016b18's perk-rehydrate loop.
    std::vector<int64_t>  perks;     // +0x2E0..+0x2F7

    // 16-byte nemesis snapshot block. byte at +0x2F8 = nemesis.visible;
    // i32 pair at +0x2FC/+0x300 = nemesis.nemesisGridCol/Row (only meaningful
    // when nemesisVisible). builder via FUN_100009974.
    bool      nemesisVisible;        // +0x2F8 <- u8 == 1, NemesisRenderable+0x000
    uint8_t   pad2F9[3];             // +0x2F9
    int32_t   nemesisGridCol;        // +0x2FC <- i16, NemesisRenderable+0x1798
    int32_t   nemesisGridRow;        // +0x300 <- i16, NemesisRenderable+0x179C
    uint32_t  nemesisLevel;          // +0x304 <- u8 , NemesisRenderable+0x17D0
    uint32_t  nemesisXP;             // +0x308 <- u8 , NemesisRenderable+0x17D4
    uint8_t   pad30C[4];             // +0x30C

    // vector at +0x310, GameplayHUD eventTray snapshot. each entry =
    // (kind, slot-state) pair from non-null hud.eventTray[i] slots
    // (FUN_10002a324 fills each int64 entry).
    std::vector<int64_t>  eventTraySnapshot;  // +0x310..+0x327

    // vector at +0x328, filtered HexMap entries (only cells with
    // value+0x14 > 2). builder via FUN_10003b614.
    std::vector<HexMapEntry>  hexMapVec;  // +0x328..+0x33F

    // tile-weight pool clone (3-int TileWeightEntry, 12 bytes). cloned
    // from gb.tileWeightPool (gb+0x9688) via FUN_10004d1d8.
    TileWeightPool  tileWeightPool;  // +0x340..+0x357

    // 5 RNG stream seeds, one per random.cpp LCG stream. read via
    // FUN_1000570c4 (= DAT_10007e870 + idx*4). restored by FUN_10005708c
    // calls in FUN_100016b18.
    int32_t   rngSeeds[5];           // +0x358..+0x36B

    uint8_t   pad36C[4];             // +0x36C..+0x36F (struct trailing pad)
};
static_assert(sizeof(GameSnapshot) == 0x370,
              "GameSnapshot must span +0x2E348..+0x2E6B8 (0x370 bytes)");
static_assert(offsetof(GameSnapshot, totalTurnCount)       == 0x004);
static_assert(offsetof(GameSnapshot, worldIndex)           == 0x020);
static_assert(offsetof(GameSnapshot, tutorialFlag)         == 0x024);
static_assert(offsetof(GameSnapshot, hintFlags)            == 0x028);
static_assert(offsetof(GameSnapshot, gridLayout)           == 0x040);
static_assert(offsetof(GameSnapshot, levelTurnCount)       == 0x050);
static_assert(offsetof(GameSnapshot, pickupSnagThreshold)  == 0x054);
static_assert(offsetof(GameSnapshot, variantsUsed)         == 0x058);
static_assert(offsetof(GameSnapshot, animBannerSeedHistory)== 0x070);
static_assert(offsetof(GameSnapshot, rackTiles)            == 0x088);
static_assert(offsetof(GameSnapshot, reserveItems)         == 0x1F0);
static_assert(offsetof(GameSnapshot, placedTiles)          == 0x208);
static_assert(offsetof(GameSnapshot, placedContentMap)          == 0x220);
static_assert(offsetof(GameSnapshot, placedSnagMap)         == 0x238);
static_assert(offsetof(GameSnapshot, characterIndex)       == 0x250);
static_assert(offsetof(GameSnapshot, statBlocks)           == 0x274);
static_assert(offsetof(GameSnapshot, perks)                == 0x2E0);
static_assert(offsetof(GameSnapshot, nemesisVisible)       == 0x2F8);
static_assert(offsetof(GameSnapshot, eventTraySnapshot)    == 0x310);
static_assert(offsetof(GameSnapshot, hexMapVec)            == 0x328);
static_assert(offsetof(GameSnapshot, tileWeightPool)       == 0x340);
static_assert(offsetof(GameSnapshot, rngSeeds)             == 0x358);

// ----------------------------------------------------------------------
// slot 4 AchievementSaveBuffer (Game+0x2E7C0..+0x2E848, 0x88 bytes).
//
// mirror of AchievementTracker's persistent state. populated by
// AchievementTracker::dirtyXfer (FUN_10004e404) and consumed by
// AchievementTracker::restoreFromSave (FUN_10004e6ac).
//
// dirtyXfer only saves data still relevant to LOCKED achievements
// (counters with `states[idx] == 0`); the 3 unique-item sets are gated
// on states[22] / states[25] / states[26] respectively. once unlocked,
// that data is no longer needed and gets dropped on the next save.
//
// libc++ aarch64 sizes:
//   std::vector<int>:     24 bytes (begin / end / cap pointers)
//   std::map<int, int>:   24 bytes (__tree head)
//   std::set<int>:        24 bytes (__tree head)
// ----------------------------------------------------------------------
struct AchievementSaveBuffer {
    int32_t              versionMagic;      // +0x000  slot 4 magic
    uint8_t              pad004[4];         // +0x004
    std::vector<int>     states;            // +0x008..+0x01F  mirror of
                                            //   tracker.states[50]; dirtyXfer
                                            //   pushes all 50 ints regardless
                                            //   of lock state.
    std::map<int, int>   counters;          // +0x020..+0x037  sparse mirror
                                            //   of tracker.counters, filtered
                                            //   to (val > 0 AND states[key] == 0).
    std::set<int>        eventKinds;        // +0x038..+0x04F  mirror of
                                            //   tracker.eventKinds, saved
                                            //   only while states[22] == 0.
    std::set<int>        eventIds;          // +0x050..+0x067  mirror of
                                            //   tracker.eventIds, saved
                                            //   only while states[25] == 0.
    std::set<int>        snagKinds;         // +0x068..+0x07F  mirror of
                                            //   tracker.snagKinds, saved
                                            //   only while states[26] == 0.
    uint8_t              damagedThisRun;    // +0x080  mirror of
                                            //   tracker.damagedThisRun.
    uint8_t              pad081[7];         // +0x081..+0x087
};
static_assert(sizeof(AchievementSaveBuffer) == 0x88,
              "AchievementSaveBuffer must span +0x2E7C0..+0x2E848 (0x88 bytes)");
