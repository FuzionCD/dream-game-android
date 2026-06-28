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
//   slot 0 ("sav"): GameSnapshot          at Game.gameSnapshot_ (0x370 bytes)
//   slot 1 ("set"): inline settings       at Game.settingsMagic_ (4-field group)
//   slot 2 ("unl"): PersistentUnlocks     at Game.shopSaveBuffer_ (0x50  bytes)
//   slot 3 ("sco"): leaderboard xfer      at Game.leaderboardSaveMagic_ (magic+pad+vector)
//   slot 4 ("ach"): AchievementSaveBuffer at Game.achievementSaveBuffer_ (0x88  bytes)
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
    uint32_t  gridIdx;             // TileObject.gridIdx (functional type 0..23)
    bool      mirror;              // TileObject.mirror (50% RNG h-flip)
    uint32_t  rotationStep;        // TileObject.rotationStep (0..5; rot = step*60)
    uint32_t  contentType;         // TileContent.type (0..0x19); gates contentMagnitude
    int32_t   contentMagnitude;    // TileContent.displayedMagnitude (i16; only if contentType!=0)
    uint32_t  snagKind;            // SnagContent.type (0..0x76); gates the snag block
    uint32_t  snagHp;              // SnagContent.hp (u16)
    uint32_t  snagAtk;             // SnagContent.atk (u16)
    uint32_t  snagDef;             // SnagContent.def (u16)
    int32_t   snagConsumedFlag;    // SnagContent.consumedFlag (i16; only if snagHasExtras)
    int32_t   snagObsessionCount;  // SnagContent.obsessionCount (i16; only if snagKind == 6)
    std::vector<int64_t> decorations;  // active decoration
                                       //                (kind:i32, value:i32) pairs from
                                       //                tile.decorList (filter:
                                       //                suppressed == 0). packed as
                                       //                low/high halves of int64.
};

// Per reserve-item snapshot (0x30 bytes per entry in reserveItems vector).
// reserveItem source = the doubly-linked tile-reserve list at GameBoard.tileReserve;
// the prefix int comes from the list node itself, the rest from the
// embedded TileObject* via FUN_100013dd8 (no coords vec).
struct ReserveItemSnapshot {
    int32_t   listSlotIndex;       // TileReserveEntry.drawCountdown (signed char source)
    uint32_t  gridIdx;             // TileObject.gridIdx
    bool      mirror;              // TileObject.mirror
    uint32_t  rotationStep;        // TileObject.rotationStep
    uint32_t  contentType;         // TileContent.type; gates contentMagnitude
    int32_t   contentMagnitude;    // TileContent.displayedMagnitude (i16)
    uint32_t  snagKind;            // SnagContent.type; gates the snag block
    uint32_t  snagHp;              // SnagContent.hp
    uint32_t  snagAtk;             // SnagContent.atk
    uint32_t  snagDef;             // SnagContent.def
    int32_t   snagConsumedFlag;    // SnagContent.consumedFlag (i16; only if snagHasExtras)
    int32_t   snagObsessionCount;  // SnagContent.obsessionCount (i16; only if snagKind == 6)
};

// Per placed-tile snapshot (0x14 bytes per entry in placedTiles vector).
// placedTile source = gb.placedTiles doubly-linked list at GameBoard.pageList; the
// (col, row) prefix comes from TileObject.gridCol/gridRow (committed coords),
// the rest from the embedded TileObject via FUN_100013dd8.
struct PlacedTileSnapshot {
    int32_t   col;                 // TileObject.gridCol (i16 in blob -> i32)
    int32_t   row;                 // TileObject.gridRow (i16 in blob -> i32)
    uint32_t  gridIdx;             // TileObject.gridIdx
    bool      mirror;              // TileObject.mirror
    uint32_t  rotationStep;        // TileObject.rotationStep
};

// Per stat-block snapshot (0x24 bytes; 3 contiguous in GameSnapshot).
// one per PlayerSystem.baseItems[i]. extracted via FUN_100033750: 5 Item
// header ints, then 2 SpecialAbility (type, value) pairs from each Item's
// abilities[0..1] array. Item.type isn't stored (implicit by slot index).
struct StatBlockSnapshot {
    uint32_t  itemSubType;         // Item.subType
    uint32_t  itemCosmeticNameIdx; // Item.cosmeticNameIdx
    uint32_t  itemAtk;             // Item.atk
    uint32_t  itemDef;             // Item.def
    uint32_t  itemHp;              // Item.hp
    uint32_t  spa0Type;            // Item.abilities[0].abilityType
    uint32_t  spa0Value;           // Item.abilities[0].abilityVal
    uint32_t  spa1Type;            // Item.abilities[1].abilityType
    uint32_t  spa1Value;           // Item.abilities[1].abilityVal
};

// Per hex-map vector entry (0x10 bytes in hexMapVec). HexMap source is
// gb.hexMap (std::map<Key, HexMapCell>); FUN_10003b614 filters in only
// entries with HexMapCell.kind > 2 and records (key.first, key.second,
// kind, fadeTimer).
struct HexMapEntry {
    int32_t   col;                 // hexMap key.first (i32)
    int32_t   row;                 // hexMap key.second (i32)
    uint32_t  kind;                // HexMapCell.kind
    uint32_t  fadeTimer;           // HexMapCell.fadeTimer
};

// Per placedSnagMap value (24 bytes, the std::map value type). holds the snag
// stats for one placed tile that carries a SnagContent: the same 6-field
// block SnagContent::initExplicit consumes, {kind, hp, atk, def, extra0,
// extra1}. extra0/extra1 are only meaningful (and only present in the blob)
// when snagHasExtras(kind), the same gate as the rack/reserve snag blocks.
struct PlacedSnagFields {
    uint32_t  kind, hp, atk, def, extra0, extra1;
};

// ----------------------------------------------------------------------
// slot 0 GameSnapshot (Game.gameSnapshot_, 0x370 bytes).
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
    int32_t   versionMagic;      // slot 0 magic

    // 7-int run counter block. copied from GameBoard.totalTurnCount through eventsFired by the builder;
    // restored to GameBoard.totalTurnCount through eventsFired by FUN_100016b18. blob compresses to
    // u16/u8 per slot; zero-extended on read.
    uint32_t  totalTurnCount;        // <- u16, GameBoard.totalTurnCount
    uint32_t  worldLevelIndex;       // <- u8 , GameBoard.worldLevelIndex
    uint32_t  snagsDefeated;         // <- u16, GameBoard.snagsDefeated
    uint32_t  specialSnagsDefeated;  // <- u16, GameBoard.specialSnagsDefeated
    uint32_t  levelsGained;          // <- u8 , GameBoard.levelsGained
    uint32_t  itemsFound;            // <- u8 , GameBoard.itemsFound
    uint32_t  eventsFired;           // <- u16, GameBoard.eventsFired

    uint32_t  worldIndex;            // <- u8 , gb.worldIndex
    bool      tutorialFlag;          // <- u8 == 1, gb.tutorialFlag

    // 24-byte hint bitfield. populated from blob[14..17] (uint32) via
    // SIMD bit-expand iff tutorialFlag was 1 in the source blob; else
    // zeroed. each byte = 1 bit of the 24 low bits of the source uint.
    // builder source: the hintFlags region inside the GameBoard HUD area.
    uint8_t   hintFlags[24];

    uint32_t  gridLayout;            // <- u8 , gb.gridLayout (cosmetic 0..11)
    int32_t   exitCol;               // <- i16, gb.exitGridCol
    int32_t   exitRow;               // <- i16, gb.exitGridRow
    uint32_t  keysRequired;          // <- u8 , gb.keysRequired
    uint32_t  levelTurnCount;        // <- u16, gb.levelTurnCount
    uint32_t  pickupSnagThreshold;   // <- u16, gb.pickupSnagThreshold

    // bytes read as ints. clone of gb.variantsUsed via FUN_100028b48 (vector::assign).
    std::vector<int>  variantsUsed;

    // clone of gb.animBannerSeedHistory via FUN_100028648.
    std::vector<int64_t>  animBannerSeedHistory;

    RackTileSnapshot  rackTiles[5];

    std::vector<ReserveItemSnapshot>  reserveItems;

    std::vector<PlacedTileSnapshot>   placedTiles;

    // per-placed-tile CONTENT side table. key = the tile's index
    // in the page list (0-based, oldest first); value = packed
    // (TileContent.type | TileContent.displayedMagnitude << 32). nearly every
    // placed tile has content (its pickup magnitude), so this is mostly dense;
    // the only misses are blank tiles cleared by a snag effect / event
    // (contentType 0). PlacedTileSnapshot holds the geometry (col/row/gridIdx/
    // mirror/rotationStep); content is split out here because it's optional.
    std::map<int, uint64_t>  placedContentMap;

    // per-placed-tile SNAG side table. key = the tile's page-list
    // index; value = the snag's {kind, hp, atk, def, extra0, extra1} block.
    // sparse: only path tiles the player walked onto that still carry a live
    // SnagContent enemy get an entry.
    std::map<int, PlacedSnagFields>  placedSnagMap;

    // PlayerSystem + HUD-stat snapshot, 9 uints (0x24 bytes total).
    // builder fills slots 0/3..8 from PlayerSystem (FUN_100056ba8) and
    // slots 1/2 from GameplayHUD (FUN_10000d9c4 overwrites the zeroed
    // slots with hud.xpReceivedTotal / hud.controlReceivedTotal).
    uint32_t  characterIndex;        // <- u8 , PlayerSystem.characterIndex
    uint32_t  xpReceivedTotal;       // <- u8 , GameplayHUD.xpReceivedTotal
    uint32_t  controlReceivedTotal;  // <- u8 , GameplayHUD.controlReceivedTotal
    uint32_t  attack;                // <- u16, PlayerSystem.attack
    uint32_t  defence;               // <- u16, PlayerSystem.defence
    uint32_t  currentHealth;         // <- u16, PlayerSystem.currentHealth
    uint32_t  baseATK;               // <- u8 , PlayerSystem.baseATK
    uint32_t  baseDEF;               // <- u8 , PlayerSystem.baseDEF
    uint32_t  baseHP;                // <- u8 , PlayerSystem.baseHP

    StatBlockSnapshot statBlocks[3];

    // Perk snapshot. one int64 per PlayerSystem.perks
    // entry; FUN_1000423f8 packs (perk.perkType, perk.perkLevel)
    // into the low/high 4 bytes of each int64 slot. restored via
    // Perk(type, level) ctor in FUN_100016b18's perk-rehydrate loop.
    std::vector<int64_t>  perks;

    // 16-byte nemesis snapshot block. holds nemesis.visible, then the
    // nemesis.nemesisGridCol/Row i32 pair (only meaningful
    // when nemesisVisible). builder via FUN_100009974.
    bool      nemesisVisible;        // <- u8 == 1, NemesisRenderable.visible
    int32_t   nemesisGridCol;        // <- i16, NemesisRenderable.nemesisGridCol
    int32_t   nemesisGridRow;        // <- i16, NemesisRenderable.nemesisGridRow
    uint32_t  nemesisLevel;          // <- u8 , NemesisRenderable.nemesisLevel
    uint32_t  nemesisXP;             // <- u8 , NemesisRenderable.nemesisXP

    // GameplayHUD eventTray snapshot. each entry =
    // (kind, slot-state) pair from non-null hud.eventTray[i] slots
    // (FUN_10002a324 fills each int64 entry).
    std::vector<int64_t>  eventTraySnapshot;

    // filtered HexMap entries (only cells with
    // kind > 2). builder via FUN_10003b614.
    std::vector<HexMapEntry>  hexMapVec;

    // tile-weight pool clone (3-int TileWeightEntry, 12 bytes). cloned
    // from gb.tileWeightPool (GameBoard.tileWeightPool) via FUN_10004d1d8.
    TileWeightPool  tileWeightPool;

    // 5 RNG stream seeds, one per random.cpp LCG stream. read via
    // FUN_1000570c4 (= DAT_10007e870 + idx*4). restored by FUN_10005708c
    // calls in FUN_100016b18.
    int32_t   rngSeeds[5];

};

// ----------------------------------------------------------------------
// slot 4 AchievementSaveBuffer (Game.achievementSaveBuffer_, 0x88 bytes).
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
    int32_t              versionMagic;      // slot 4 magic
    std::vector<int>     states;            // mirror of
                                            //   tracker.states[50]; dirtyXfer
                                            //   pushes all 50 ints regardless
                                            //   of lock state.
    std::map<int, int>   counters;          // sparse mirror
                                            //   of tracker.counters, filtered
                                            //   to (val > 0 AND states[key] == 0).
    std::set<int>        eventKinds;        // mirror of
                                            //   tracker.eventKinds, saved
                                            //   only while states[22] == 0.
    std::set<int>        eventIds;          // mirror of
                                            //   tracker.eventIds, saved
                                            //   only while states[25] == 0.
    std::set<int>        snagKinds;         // mirror of
                                            //   tracker.snagKinds, saved
                                            //   only while states[26] == 0.
    uint8_t              damagedThisRun;    // mirror of
                                            //   tracker.damagedThisRun.
};
