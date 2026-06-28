#include "save_system.h"

#include "game.h"

#include <SDL.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ----------------------------------------------------------------------
// per-slot loader / encoder helpers.
//
// each pair mirrors one Ghidra-anchored function. the slot dispatcher
// table fans out by SlotId to the matching loader/encoder helper.
//
// loader signature: (game, blobBytes, blobLength) -> void
//   reads the blob's first 4 bytes as the expected magic; if it matches
//   game.<slot>Magic, copies fields into the matching region.
//
// encoder signature: (game, &outLength) -> const uint8_t*
//   clears the slot's dirty bit, packs fields into the slot's scratch
//   buffer (a std::vector<uint8_t> at a fixed offset), sets *outLength
//   to the encoded byte count, returns the scratch buffer's data
//   pointer. returns nullptr when no blob should be written (slot 0's
//   "delete the file" case).
// ----------------------------------------------------------------------

// ===== slot 0 ("sav"), FUN_10004762c / FUN_1000463a4, task H/G =====
//
// densely-packed variable-length encoding. high-level layout:
//   magic byte, then 7 counter fields (u16/u8 mix) ... worldIndex (u8) ...
//   tutorialFlag (u8 == 1), optional 4-byte hint bitfield expanded to
//   24 bytes of 0/1 ... 6 header fields ... vector(int byte entries),
//   vector(byte->int64 entries) ... 5 rack tile snapshots (each
//   variable-length: 4 unconditional bytes + optional 2-byte extra +
//   1-byte snagType + optional 6 bytes + optional 4 bytes + inner
//   vector of (col:u8, row:i16) triples) ... placedTiles vector with
//   the same variable-length per-entry shape ... placedContentMap (key u16,
//   value 5 bytes packed) ... placedSnagMap (key u16, value 9 or 13
//   bytes depending on snagHasExtras() gate) ... PlayerSystem 12-byte
//   header ... 3 stat blocks (5 bytes + up to 4 bytes ability pairs
//   with a 1-byte sentinel) ... vector of (byte, byte) ... 16-byte
//   block ... vector of (byte, byte) ... hexMapVec (6 bytes each) ...
//   tileWeightPool (5 bytes each) ... 5*int32 rngSeeds.

// FUN_10003e4ac. returns 1 for snagTypes {6, 15, 17, 39}, else 0.
// inlines a 64-bit bitmask lookup. used by the loader / encoder /
// builder / restorer to gate the "snag has 2 extra i16's" fields.
static bool snagHasExtras(uint32_t snagType) {

    if (snagType < 6 || (snagType - 6) >= 0x22) {
        return false;
    }

    constexpr uint64_t mask = 0x200000a01ull;
    return ((mask >> (snagType - 6)) & 1) != 0;
}

namespace {

// little-endian primitive readers. cursor advances by element size.
inline uint8_t readU8(const uint8_t* blob, size_t& cur) {
    return blob[cur++];
}

inline uint16_t readU16(const uint8_t* blob, size_t& cur) {
    uint16_t v;
    std::memcpy(&v, blob + cur, sizeof(v));
    cur += sizeof(v);
    return v;
}

inline int16_t readI16(const uint8_t* blob, size_t& cur) {
    int16_t v;
    std::memcpy(&v, blob + cur, sizeof(v));
    cur += sizeof(v);
    return v;
}

inline uint32_t readU32(const uint8_t* blob, size_t& cur) {
    uint32_t v;
    std::memcpy(&v, blob + cur, sizeof(v));
    cur += sizeof(v);
    return v;
}

// little-endian primitive writers. cursor advances by element size.
inline void writeU8(uint8_t* dst, size_t& cur, uint8_t v) {
    dst[cur++] = v;
}

inline void writeI16(uint8_t* dst, size_t& cur, int16_t v) {
    std::memcpy(dst + cur, &v, sizeof(v));
    cur += sizeof(v);
}

inline void writeU16(uint8_t* dst, size_t& cur, uint16_t v) {
    std::memcpy(dst + cur, &v, sizeof(v));
    cur += sizeof(v);
}

inline void writeU32(uint8_t* dst, size_t& cur, uint32_t v) {
    std::memcpy(dst + cur, &v, sizeof(v));
    cur += sizeof(v);
}

}  // namespace

// FUN_10004762c. magic-gate, hasSavedRun set, then scatter the densely-
// packed blob into game.gameSnapshot. on magic mismatch, hasSavedRun
// gets cleared (= start-button picks T=0 next launch instead of T=1).
static void loadSavedGame(Game& game, const uint8_t* blob, size_t length) {
    // our scratch resizes dynamically, so the binary's "scratch capacity
    // < blob length -> reject" gate has no equivalent here; keep
    // hasSavedRun set unconditionally for matching.
    game.hasSavedRun() = length;

    if (blob == nullptr || length == 0) {
        return;
    }

    GameSnapshot& snap = game.gameSnapshot();
    const uint32_t blobMagic = static_cast<uint32_t>(blob[0]);
    const uint32_t memMagic  = static_cast<uint32_t>(snap.versionMagic);

    if (blobMagic != memMagic) {
        SDL_Log("DEBUG_SAVE: loadSavedGame rejected -- magic 0x%X != 0x%X",
                blobMagic, memMagic);
        game.hasSavedRun() = 0;
        return;
    }

    size_t cur = 1;
    snap.totalTurnCount       = readU16(blob, cur);
    snap.worldLevelIndex      = readU8 (blob, cur);
    snap.snagsDefeated        = readU16(blob, cur);
    snap.specialSnagsDefeated = readU16(blob, cur);
    snap.levelsGained         = readU8 (blob, cur);
    snap.itemsFound           = readU8 (blob, cur);
    snap.eventsFired          = readU16(blob, cur);
    snap.worldIndex           = readU8 (blob, cur);
    snap.tutorialFlag         = (readU8(blob, cur) == 1);

    std::memset(snap.hintFlags, 0, sizeof(snap.hintFlags));

    if (snap.tutorialFlag) {
        const uint32_t bits = readU32(blob, cur);

        for (int i = 0; i < 24; ++i) {
            snap.hintFlags[i] = static_cast<uint8_t>((bits >> i) & 1);
        }
    }

    snap.gridLayout           = readU8 (blob, cur);
    snap.exitCol              = readI16(blob, cur);
    snap.exitRow              = readI16(blob, cur);
    snap.keysRequired         = readU8 (blob, cur);
    snap.levelTurnCount       = readU16(blob, cur);
    snap.pickupSnagThreshold  = readU16(blob, cur);

    // vector<int>, bytes -> ints. clone of gb.variantsUsed.
    {
        const uint8_t n = readU8(blob, cur);
        snap.variantsUsed.resize(n);

        for (uint8_t i = 0; i < n; ++i) {
            snap.variantsUsed[i] = readU8(blob, cur);
        }
    }

    // vector<int64>, bytes -> int64. clone of gb.animBannerSeedHistory.
    {
        const uint8_t n = readU8(blob, cur);
        snap.animBannerSeedHistory.resize(n);

        for (uint8_t i = 0; i < n; ++i) {
            snap.animBannerSeedHistory[i] = readU8(blob, cur);
        }
    }

    // 5 rack-tile snapshots.
    for (int slot = 0; slot < 5; ++slot) {
        RackTileSnapshot& r = snap.rackTiles[slot];
        r.gridIdx          = readU8(blob, cur);
        r.mirror           = (readU8(blob, cur) == 1);
        r.rotationStep     = readU8(blob, cur);
        r.contentType      = readU8(blob, cur);
        r.contentMagnitude = (r.contentType != 0) ? readI16(blob, cur) : 0;
        r.snagKind         = readU8(blob, cur);

        if (r.snagKind != 0) {
            r.snagHp  = readU16(blob, cur);
            r.snagAtk = readU16(blob, cur);
            r.snagDef = readU16(blob, cur);

            if (snagHasExtras(r.snagKind)) {
                r.snagConsumedFlag   = readI16(blob, cur);
                r.snagObsessionCount = readI16(blob, cur);
            }
        }

        // inner decorations vector: 3 bytes per entry (kind:u8, value:i16),
        // stored as packed int64 (low 4 = kind, high 4 = value).
        const uint8_t n = readU8(blob, cur);
        r.decorations.resize(n);

        for (uint8_t i = 0; i < n; ++i) {
            const uint8_t kind  = readU8(blob, cur);
            const int16_t value = readI16(blob, cur);
            r.decorations[i] = static_cast<uint64_t>(kind) |
                               (static_cast<uint64_t>(static_cast<uint32_t>(
                                    static_cast<int32_t>(value))) << 32);
        }
    }

    // reserveItems vector, variable-length per entry, same shape as
    // a rack-slot snapshot minus the inner coords vec.
    {
        const uint16_t n = readU16(blob, cur);
        snap.reserveItems.resize(n);

        for (uint16_t i = 0; i < n; ++i) {
            ReserveItemSnapshot& e = snap.reserveItems[i];
            e.listSlotIndex    = static_cast<int32_t>(
                                      static_cast<int8_t>(readU8(blob, cur)));
            e.gridIdx          = readU8(blob, cur);
            e.mirror           = (readU8(blob, cur) == 1);
            e.rotationStep     = readU8(blob, cur);
            e.contentType      = readU8(blob, cur);
            e.contentMagnitude = (e.contentType != 0) ? readI16(blob, cur) : 0;
            e.snagKind         = readU8(blob, cur);

            if (e.snagKind != 0) {
                e.snagHp  = readU16(blob, cur);
                e.snagAtk = readU16(blob, cur);
                e.snagDef = readU16(blob, cur);

                if (snagHasExtras(e.snagKind)) {
                    e.snagConsumedFlag   = readI16(blob, cur);
                    e.snagObsessionCount = readI16(blob, cur);
                }
            }
        }
    }

    // placedTiles vector, fixed 7 bytes per entry.
    {
        const uint16_t n = readU16(blob, cur);
        snap.placedTiles.resize(n);

        for (uint16_t i = 0; i < n; ++i) {
            PlacedTileSnapshot& p = snap.placedTiles[i];
            p.col          = readI16(blob, cur);
            p.row          = readI16(blob, cur);
            p.gridIdx      = readU8 (blob, cur);
            p.mirror       = (readU8(blob, cur) == 1);
            p.rotationStep = readU8 (blob, cur);
        }
    }

    // placedContentMap: key (u16), then (u8 + i16) -> packed uint64 value.
    snap.placedContentMap.clear();
    {
        const uint16_t n = readU16(blob, cur);

        for (uint16_t i = 0; i < n; ++i) {
            const uint16_t key  = readU16(blob, cur);
            const uint8_t  vlo  = readU8 (blob, cur);
            const int16_t  vhi  = readI16(blob, cur);
            snap.placedContentMap[key] = static_cast<uint64_t>(vlo) |
                                    (static_cast<uint64_t>(static_cast<uint32_t>(
                                        static_cast<int32_t>(vhi))) << 32);
        }
    }

    // placedSnagMap: key (placed-tile index, u16), value = snag
    // {kind, hp, atk, def} + the 2 extras (conditional on snagHasExtras(kind)).
    snap.placedSnagMap.clear();
    {
        const uint16_t n = readU16(blob, cur);

        for (uint16_t i = 0; i < n; ++i) {
            const uint16_t key = readU16(blob, cur);
            PlacedSnagFields v{};
            v.kind = readU8 (blob, cur);
            v.hp   = readU16(blob, cur);
            v.atk  = readU16(blob, cur);
            v.def  = readU16(blob, cur);

            if (snagHasExtras(v.kind)) {
                v.extra0 = static_cast<uint32_t>(readI16(blob, cur));
                v.extra1 = static_cast<uint32_t>(readI16(blob, cur));
            }

            snap.placedSnagMap[key] = v;
        }
    }

    // PlayerSystem + HUD-stat 9-uint header (12 bytes encoded).
    snap.characterIndex        = readU8 (blob, cur);
    snap.xpReceivedTotal       = readU8 (blob, cur);
    snap.controlReceivedTotal  = readU8 (blob, cur);
    snap.attack                = readU16(blob, cur);
    snap.defence               = readU16(blob, cur);
    snap.currentHealth         = readU16(blob, cur);
    snap.baseATK               = readU8 (blob, cur);
    snap.baseDEF               = readU8 (blob, cur);
    snap.baseHP                = readU8 (blob, cur);

    // 3 stat blocks: 5 Item header ints + up to 2 SpecialAbility pairs
    // terminated by a 0-byte sentinel.
    for (int s = 0; s < 3; ++s) {
        StatBlockSnapshot& b = snap.statBlocks[s];
        b.itemSubType         = readU8(blob, cur);
        b.itemCosmeticNameIdx = readU8(blob, cur);
        b.itemAtk             = readU8(blob, cur);
        b.itemDef             = readU8(blob, cur);
        b.itemHp              = readU8(blob, cur);
        b.spa0Type            = 0;
        b.spa0Value           = 0;
        b.spa1Type            = 0;
        b.spa1Value           = 0;

        uint32_t* spaSlots[2][2] = {
            { &b.spa0Type, &b.spa0Value },
            { &b.spa1Type, &b.spa1Value },
        };

        for (int slot = 0; slot < 2; ++slot) {
            const uint8_t t = readU8(blob, cur);

            if (t == 0) {
                break;  // sentinel: stop reading SpecialAbility slots
            }

            *spaSlots[slot][0] = t;
            *spaSlots[slot][1] = readU8(blob, cur);
        }
    }

    // perks: (perkType:u8, perkLevel:u8) pairs packed into int64 entries
    // (low 32 bits = perkType, high 32 bits = perkLevel).
    {
        const uint8_t n = readU8(blob, cur);
        snap.perks.resize(n);

        for (uint8_t i = 0; i < n; ++i) {
            const uint8_t perkType  = readU8(blob, cur);
            const uint8_t perkLevel = readU8(blob, cur);
            snap.perks[i] = static_cast<uint64_t>(perkType) |
                            (static_cast<uint64_t>(perkLevel) << 32);
        }
    }

    // Nemesis block: visible-byte gate, then level/XP u8 pair
    // (unconditional), then grid (col, row) i16 pair (only when visible).
    snap.nemesisVisible = (readU8(blob, cur) == 1);
    snap.nemesisLevel   = readU8(blob, cur);
    snap.nemesisXP      = readU8(blob, cur);

    if (snap.nemesisVisible) {
        snap.nemesisGridCol = readI16(blob, cur);
        snap.nemesisGridRow = readI16(blob, cur);
    }
    else {
        snap.nemesisGridCol = 0;
        snap.nemesisGridRow = 0;
    }

    // eventTraySnapshot: (kind:u8, slot-state:u8) pairs packed as int64
    // (FUN_10002a324 fills each entry from hud.eventTray[i]).
    {
        const uint8_t n = readU8(blob, cur);
        snap.eventTraySnapshot.resize(n);

        for (uint8_t i = 0; i < n; ++i) {
            const uint8_t lo = readU8(blob, cur);
            const uint8_t hi = readU8(blob, cur);
            snap.eventTraySnapshot[i] = static_cast<uint64_t>(lo) |
                                        (static_cast<uint64_t>(hi) << 32);
        }
    }

    // hexMapVec: 6 bytes per entry (2 i16 + 2 u8).
    {
        const uint16_t n = readU16(blob, cur);
        snap.hexMapVec.resize(n);

        for (uint16_t i = 0; i < n; ++i) {
            HexMapEntry& h = snap.hexMapVec[i];
            h.col  = readI16(blob, cur);
            h.row  = readI16(blob, cur);
            h.kind = readU8 (blob, cur);
            h.fadeTimer = readU8 (blob, cur);
        }
    }

    // tileWeightPool: 5 bytes per entry (u8 typeId, u16 base, u16 cur).
    {
        const uint8_t n = readU8(blob, cur);
        snap.tileWeightPool.resize(n);

        for (uint8_t i = 0; i < n; ++i) {
            TileWeightEntry& w = snap.tileWeightPool[i];
            w.typeId         = readU8 (blob, cur);
            w.baseWeight     = readU16(blob, cur);
            w.currentWeight  = readU16(blob, cur);
        }
    }

    // rngSeeds[5]: direct 20-byte copy.
    for (int i = 0; i < 5; ++i) {
        snap.rngSeeds[i] = static_cast<int32_t>(readU32(blob, cur));
    }

    SDL_Log("DEBUG_SAVE: loadSavedGame OK -- %zu bytes consumed (of %zu)",
            cur, length);
}

// FUN_1000463a4. handles two cases:
//   dirty == 2: "delete the saved run", clear hasSavedRun, clear dirty,
//               delete the file directly (framework doesn't infer deletion
//               from a zero-length return), return null.
//   else      : clear dirty, pack the GameSnapshot into the slot's
//               scratch vector, set hasSavedRun to the
//               encoded length, return data ptr + length.
//
// note: only the encode path runs the full pack; the delete path is a
// shortcut that bypasses encoding entirely.
//
// forward declarations for the file-delete helpers (defined in the
// anonymous namespace below). called from the dirty==2 fast path.
namespace { std::string slotPath(SaveSystem::SlotId slot); void deleteFile(const std::string& path); }

static const uint8_t* encodeSavedGame(Game& game, size_t* outLength) {

    if (game.saveSlot0Dirty() == 2) {
        // delete-the-save case: zero hasSavedRun + dirty, delete the
        // file directly, return nothing for the framework to write.
        game.hasSavedRun() = 0;
        game.saveSlot0Dirty() = 0;
        deleteFile(slotPath(SaveSystem::SAV));
        *outLength = 0;
        return nullptr;
    }

    game.saveSlot0Dirty() = 0;
    const GameSnapshot& snap = game.gameSnapshot();

    // worst-case scratch size: a variable-length blob can't be predicted
    // exactly without a pre-pass. reserve generously (1 MB covers any
    // plausible save: hexMapVec is u16-counted with 6 bytes/entry =
    // 384 KB absolute max, plus headers), then resize down after
    // encode. the slot's scratch buffer is libc++
    // std::vector<uint8_t>.
    auto& scratch = game.saveScratch(0);
    scratch.resize(1024 * 1024);
    uint8_t* dst = scratch.data();
    size_t cur = 0;

    writeU8 (dst, cur, static_cast<uint8_t>(snap.versionMagic & 0xFF));
    writeU16(dst, cur, static_cast<uint16_t>(snap.totalTurnCount));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.worldLevelIndex));
    writeU16(dst, cur, static_cast<uint16_t>(snap.snagsDefeated));
    writeU16(dst, cur, static_cast<uint16_t>(snap.specialSnagsDefeated));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.levelsGained));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.itemsFound));
    writeU16(dst, cur, static_cast<uint16_t>(snap.eventsFired));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.worldIndex));
    writeU8 (dst, cur, snap.tutorialFlag ? 1 : 0);

    if (snap.tutorialFlag) {
        // re-pack hintFlags[24] into a 24-bit field at the low end of a
        // uint32. matches the loader's NEON ushl + cmeq expand inverse.
        uint32_t bits = 0;

        for (int i = 0; i < 24; ++i) {
            if (snap.hintFlags[i]) {
                bits |= (1u << i);
            }
        }

        writeU32(dst, cur, bits);
    }

    writeU8 (dst, cur, static_cast<uint8_t> (snap.gridLayout));
    writeI16(dst, cur, static_cast<int16_t> (snap.exitCol));
    writeI16(dst, cur, static_cast<int16_t> (snap.exitRow));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.keysRequired));
    writeU16(dst, cur, static_cast<uint16_t>(snap.levelTurnCount));
    writeU16(dst, cur, static_cast<uint16_t>(snap.pickupSnagThreshold));

    writeU8(dst, cur, static_cast<uint8_t>(snap.variantsUsed.size()));

    for (int v : snap.variantsUsed) {
        writeU8(dst, cur, static_cast<uint8_t>(v));
    }

    writeU8(dst, cur, static_cast<uint8_t>(snap.animBannerSeedHistory.size()));

    for (int64_t v : snap.animBannerSeedHistory) {
        writeU8(dst, cur, static_cast<uint8_t>(v));
    }

    for (int slot = 0; slot < 5; ++slot) {
        const RackTileSnapshot& r = snap.rackTiles[slot];
        writeU8(dst, cur, static_cast<uint8_t>(r.gridIdx));
        writeU8(dst, cur, r.mirror ? 1 : 0);
        writeU8(dst, cur, static_cast<uint8_t>(r.rotationStep));
        writeU8(dst, cur, static_cast<uint8_t>(r.contentType));

        if (r.contentType != 0) {
            writeI16(dst, cur, static_cast<int16_t>(r.contentMagnitude));
        }

        writeU8(dst, cur, static_cast<uint8_t>(r.snagKind));

        if (r.snagKind != 0) {
            writeU16(dst, cur, static_cast<uint16_t>(r.snagHp));
            writeU16(dst, cur, static_cast<uint16_t>(r.snagAtk));
            writeU16(dst, cur, static_cast<uint16_t>(r.snagDef));

            if (snagHasExtras(r.snagKind)) {
                writeI16(dst, cur, static_cast<int16_t>(r.snagConsumedFlag));
                writeI16(dst, cur, static_cast<int16_t>(r.snagObsessionCount));
            }
        }

        writeU8(dst, cur, static_cast<uint8_t>(r.decorations.size()));

        for (uint64_t packed : r.decorations) {
            const uint8_t  kind  = static_cast<uint8_t>(packed & 0xFF);
            const int16_t  value = static_cast<int16_t>(
                                       static_cast<int32_t>(packed >> 32));
            writeU8 (dst, cur, kind);
            writeI16(dst, cur, value);
        }
    }

    writeU16(dst, cur, static_cast<uint16_t>(snap.reserveItems.size()));

    for (const ReserveItemSnapshot& e : snap.reserveItems) {
        writeU8(dst, cur, static_cast<uint8_t>(e.listSlotIndex));
        writeU8(dst, cur, static_cast<uint8_t>(e.gridIdx));
        writeU8(dst, cur, e.mirror ? 1 : 0);
        writeU8(dst, cur, static_cast<uint8_t>(e.rotationStep));
        writeU8(dst, cur, static_cast<uint8_t>(e.contentType));

        if (e.contentType != 0) {
            writeI16(dst, cur, static_cast<int16_t>(e.contentMagnitude));
        }

        writeU8(dst, cur, static_cast<uint8_t>(e.snagKind));

        if (e.snagKind != 0) {
            writeU16(dst, cur, static_cast<uint16_t>(e.snagHp));
            writeU16(dst, cur, static_cast<uint16_t>(e.snagAtk));
            writeU16(dst, cur, static_cast<uint16_t>(e.snagDef));

            if (snagHasExtras(e.snagKind)) {
                writeI16(dst, cur, static_cast<int16_t>(e.snagConsumedFlag));
                writeI16(dst, cur, static_cast<int16_t>(e.snagObsessionCount));
            }
        }
    }

    writeU16(dst, cur, static_cast<uint16_t>(snap.placedTiles.size()));

    for (const PlacedTileSnapshot& p : snap.placedTiles) {
        writeI16(dst, cur, static_cast<int16_t>(p.col));
        writeI16(dst, cur, static_cast<int16_t>(p.row));
        writeU8 (dst, cur, static_cast<uint8_t>(p.gridIdx));
        writeU8 (dst, cur, p.mirror ? 1 : 0);
        writeU8 (dst, cur, static_cast<uint8_t>(p.rotationStep));
    }

    writeU16(dst, cur, static_cast<uint16_t>(snap.placedContentMap.size()));

    for (const auto& [key, packed] : snap.placedContentMap) {
        writeU16(dst, cur, static_cast<uint16_t>(key));
        writeU8 (dst, cur, static_cast<uint8_t> (packed & 0xFF));
        writeI16(dst, cur, static_cast<int16_t>(
                                static_cast<int32_t>(packed >> 32)));
    }

    writeU16(dst, cur, static_cast<uint16_t>(snap.placedSnagMap.size()));

    for (const auto& [key, v] : snap.placedSnagMap) {
        writeU16(dst, cur, static_cast<uint16_t>(key));
        writeU8 (dst, cur, static_cast<uint8_t> (v.kind));
        writeU16(dst, cur, static_cast<uint16_t>(v.hp));
        writeU16(dst, cur, static_cast<uint16_t>(v.atk));
        writeU16(dst, cur, static_cast<uint16_t>(v.def));

        if (snagHasExtras(v.kind)) {
            writeI16(dst, cur, static_cast<int16_t>(v.extra0));
            writeI16(dst, cur, static_cast<int16_t>(v.extra1));
        }
    }

    writeU8 (dst, cur, static_cast<uint8_t> (snap.characterIndex));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.xpReceivedTotal));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.controlReceivedTotal));
    writeU16(dst, cur, static_cast<uint16_t>(snap.attack));
    writeU16(dst, cur, static_cast<uint16_t>(snap.defence));
    writeU16(dst, cur, static_cast<uint16_t>(snap.currentHealth));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.baseATK));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.baseDEF));
    writeU8 (dst, cur, static_cast<uint8_t> (snap.baseHP));

    for (int s = 0; s < 3; ++s) {
        const StatBlockSnapshot& b = snap.statBlocks[s];
        writeU8(dst, cur, static_cast<uint8_t>(b.itemSubType));
        writeU8(dst, cur, static_cast<uint8_t>(b.itemCosmeticNameIdx));
        writeU8(dst, cur, static_cast<uint8_t>(b.itemAtk));
        writeU8(dst, cur, static_cast<uint8_t>(b.itemDef));
        writeU8(dst, cur, static_cast<uint8_t>(b.itemHp));

        // emit each non-zero SpecialAbility pair, then a 0 sentinel if
        // fewer than 2 were emitted (matches loader's break-on-zero gate).
        int emitted = 0;
        const uint32_t types[2]  = { b.spa0Type,  b.spa1Type  };
        const uint32_t values[2] = { b.spa0Value, b.spa1Value };

        for (int slot = 0; slot < 2; ++slot) {

            if (types[slot] == 0) {
                break;
            }

            writeU8(dst, cur, static_cast<uint8_t>(types[slot]));
            writeU8(dst, cur, static_cast<uint8_t>(values[slot]));
            ++emitted;
        }

        if (emitted < 2) {
            writeU8(dst, cur, 0);  // sentinel
        }
    }

    writeU8(dst, cur, static_cast<uint8_t>(snap.perks.size()));

    for (uint64_t v : snap.perks) {
        writeU8(dst, cur, static_cast<uint8_t>(v & 0xFF));         // perkType
        writeU8(dst, cur, static_cast<uint8_t>((v >> 32) & 0xFF)); // perkLevel
    }

    writeU8(dst, cur, snap.nemesisVisible ? 1 : 0);
    writeU8(dst, cur, static_cast<uint8_t>(snap.nemesisLevel));
    writeU8(dst, cur, static_cast<uint8_t>(snap.nemesisXP));

    if (snap.nemesisVisible) {
        writeI16(dst, cur, static_cast<int16_t>(snap.nemesisGridCol));
        writeI16(dst, cur, static_cast<int16_t>(snap.nemesisGridRow));
    }

    writeU8(dst, cur, static_cast<uint8_t>(snap.eventTraySnapshot.size()));

    for (uint64_t v : snap.eventTraySnapshot) {
        writeU8(dst, cur, static_cast<uint8_t>(v & 0xFF));
        writeU8(dst, cur, static_cast<uint8_t>((v >> 32) & 0xFF));
    }

    writeU16(dst, cur, static_cast<uint16_t>(snap.hexMapVec.size()));

    for (const HexMapEntry& h : snap.hexMapVec) {
        writeI16(dst, cur, static_cast<int16_t>(h.col));
        writeI16(dst, cur, static_cast<int16_t>(h.row));
        writeU8 (dst, cur, static_cast<uint8_t>(h.kind));
        writeU8 (dst, cur, static_cast<uint8_t>(h.fadeTimer));
    }

    writeU8(dst, cur, static_cast<uint8_t>(snap.tileWeightPool.size()));

    for (const TileWeightEntry& w : snap.tileWeightPool) {
        writeU8 (dst, cur, static_cast<uint8_t> (w.typeId));
        writeU16(dst, cur, static_cast<uint16_t>(w.baseWeight));
        writeU16(dst, cur, static_cast<uint16_t>(w.currentWeight));
    }

    for (int i = 0; i < 5; ++i) {
        writeU32(dst, cur, static_cast<uint32_t>(snap.rngSeeds[i]));
    }

    // shrink scratch to the exact encoded size so the file write picks
    // up the right byte count.
    scratch.resize(cur);

    // hasSavedRun now holds the length so the next launch's start-button
    // gate (which reads hasSavedRun) sees a populated save.
    game.hasSavedRun() = cur;

    *outLength = cur;

    SDL_Log("DEBUG_SAVE: encodeSavedGame -- %zu bytes packed", cur);
    return scratch.data();
}

// ===== slot 1 ("set"), FUN_1000481f8 / inline FUN_1000462ac, task H/C =====
//
// blob layout (11 bytes total):
//   [0]    version magic           (low byte of game.settingsMagic_)
//   [1]    tutorialFlag            (0 or 1)
//   [2]    stashedDifficulty       (low byte; in-memory is uint32)
//   [3..6] globalSeVolume          (float, native byte order)
//   [7..10]globalBgmVolume         (float, native byte order)

// FUN_1000481f8. read blob, compare blob[0] to settingsMagic; if match,
// scatter fields into game. mismatch / null blob -> silent reject.
static void loadSettings(Game& game, const uint8_t* blob, size_t length) {

    if (blob == nullptr || length == 0) {
        return;
    }

    // both sides zero-extend to uint32, so the comparison is effectively
    // (blob[0] == low byte of settingsMagic).
    const uint32_t blobMagic = static_cast<uint32_t>(blob[0]);
    const uint32_t memMagic  = static_cast<uint32_t>(game.settingsMagic());

    if (blobMagic != memMagic) {
        SDL_Log("DEBUG_SAVE: loadSettings rejected -- magic 0x%X != 0x%X",
                blobMagic, memMagic);
        return;
    }

    game.tutorialFlag()      = (blob[1] == 1);
    game.stashedDifficulty() = static_cast<int>(blob[2]);
    std::memcpy(&game.globalSeVolume(),  blob + 3, sizeof(float));
    std::memcpy(&game.globalBgmVolume(), blob + 7, sizeof(float));

    SDL_Log("DEBUG_SAVE: loadSettings OK -- tutorial=%d diff=%d se=%.3f bgm=%.3f",
            (int)game.tutorialFlag(), game.stashedDifficulty(),
            game.globalSeVolume(), game.globalBgmVolume());
}

// inline FUN_1000462ac case 1. clear slot-1 dirty, pack 11 bytes into
// the slot's scratch vector, return data ptr + length.
static const uint8_t* encodeSettings(Game& game, size_t* outLength) {
    // clear dirty before encoding so a re-entry mid-loop is a no-op.
    game.saveSlot1Dirty() = false;

    // resize scratch to the fixed 11-byte blob size. binary's scratch is
    // a heap buffer it pre-allocates; libc++ vector::resize gives us the
    // same end result (data ptr stable for the duration of this call).
    auto& scratch = game.saveScratch(1);
    scratch.resize(11);
    uint8_t* dst = scratch.data();

    dst[0] = static_cast<uint8_t>(game.settingsMagic() & 0xFF);
    dst[1] = game.tutorialFlag() ? 1 : 0;
    dst[2] = static_cast<uint8_t>(game.stashedDifficulty() & 0xFF);
    std::memcpy(dst + 3, &game.globalSeVolume(),  sizeof(float));
    std::memcpy(dst + 7, &game.globalBgmVolume(), sizeof(float));

    *outLength = 11;

    SDL_Log("DEBUG_SAVE: encodeSettings -- tutorial=%d diff=%d se=%.3f bgm=%.3f",
            (int)game.tutorialFlag(), game.stashedDifficulty(),
            game.globalSeVolume(), game.globalBgmVolume());
    return dst;
}

// ===== slot 2 ("unl"), FUN_100048278 / FUN_100046ffc, task H/D =====
//
// blob layout (4 + faceCount + snagCount + eventCount bytes):
//   [0]                                          version magic (low byte
//                                                of shopSaveBuffer.versionMagic)
//   [1]                                          keys (clamped to [0, 20])
//   [2]                                          faceCount
//   [3 .. 2 + faceCount]                         face IDs (1 byte each)
//   [3 + faceCount]                              snagCount
//   [4 + faceCount .. 3 + faceCount + snagCount] snag IDs
//   [4 + faceCount + snagCount]                  eventCount
//   [5 + faceCount + snagCount .. ...]           event IDs

// FUN_100048278. magic-gate, then variable-length scatter into
// game.shopSaveBuffer. mismatch / null blob -> silent reject.
static void loadUnlocks(Game& game, const uint8_t* blob, size_t length) {

    if (blob == nullptr || length == 0) {
        return;
    }

    const uint32_t blobMagic = static_cast<uint32_t>(blob[0]);
    const uint32_t memMagic  = static_cast<uint32_t>(
                                  game.shopSaveBuffer().versionMagic);

    if (blobMagic != memMagic) {
        SDL_Log("DEBUG_SAVE: loadUnlocks rejected -- magic 0x%X != 0x%X",
                blobMagic, memMagic);
        return;
    }

    PersistentUnlocks& snap = game.shopSaveBuffer();
    snap.keys = static_cast<int32_t>(blob[1]);

    const uint8_t faceCount = blob[2];
    snap.faceUnlocks.resize(faceCount);

    for (uint8_t i = 0; i < faceCount; ++i) {
        snap.faceUnlocks[i] = blob[3 + i];
    }

    const size_t snagPos = 3 + faceCount;
    const uint8_t snagCount = blob[snagPos];
    snap.snagUnlocks.resize(snagCount);

    for (uint8_t i = 0; i < snagCount; ++i) {
        snap.snagUnlocks[i] = blob[snagPos + 1 + i];
    }

    const size_t eventPos = snagPos + 1 + snagCount;
    const uint8_t eventCount = blob[eventPos];
    snap.eventUnlocks.resize(eventCount);

    for (uint8_t i = 0; i < eventCount; ++i) {
        snap.eventUnlocks[i] = blob[eventPos + 1 + i];
    }

    SDL_Log("DEBUG_SAVE: loadUnlocks OK -- keys=%d face=%zu snag=%zu event=%zu",
            snap.keys, snap.faceUnlocks.size(), snap.snagUnlocks.size(),
            snap.eventUnlocks.size());
}

// FUN_100046ffc. clear shopSaveDirty, pack variable-length blob into the
// slot's scratch vector, return data ptr + length.
static const uint8_t* encodeUnlocks(Game& game, size_t* outLength) {
    game.shopSaveDirty() = false;

    const PersistentUnlocks& snap = game.shopSaveBuffer();
    const size_t totalSize = 3 + snap.faceUnlocks.size()
                           + 1 + snap.snagUnlocks.size()
                           + 1 + snap.eventUnlocks.size();

    auto& scratch = game.saveScratch(2);
    scratch.resize(totalSize);
    uint8_t* dst = scratch.data();

    dst[0] = static_cast<uint8_t>(snap.versionMagic & 0xFF);
    dst[1] = static_cast<uint8_t>(snap.keys & 0xFF);
    dst[2] = static_cast<uint8_t>(snap.faceUnlocks.size());

    for (size_t i = 0; i < snap.faceUnlocks.size(); ++i) {
        dst[3 + i] = static_cast<uint8_t>(snap.faceUnlocks[i]);
    }

    size_t cur = 3 + snap.faceUnlocks.size();
    dst[cur] = static_cast<uint8_t>(snap.snagUnlocks.size());

    for (size_t i = 0; i < snap.snagUnlocks.size(); ++i) {
        dst[cur + 1 + i] = static_cast<uint8_t>(snap.snagUnlocks[i]);
    }

    cur += 1 + snap.snagUnlocks.size();
    dst[cur] = static_cast<uint8_t>(snap.eventUnlocks.size());

    for (size_t i = 0; i < snap.eventUnlocks.size(); ++i) {
        dst[cur + 1 + i] = static_cast<uint8_t>(snap.eventUnlocks[i]);
    }

    *outLength = totalSize;

    SDL_Log("DEBUG_SAVE: encodeUnlocks -- keys=%d face=%zu snag=%zu event=%zu",
            snap.keys, snap.faceUnlocks.size(), snap.snagUnlocks.size(),
            snap.eventUnlocks.size());
    return dst;
}

// ===== slot 3 ("sco"), FUN_1000483e8 / FUN_10004719c, task H/E =====
//
// blob layout (2 + 11 * entryCount bytes):
//   [0]              version magic (low byte of leaderboardSaveMagic_)
//   [1]              entryCount
//   per entry (11 bytes), starting at byte 2:
//     [0]  worldIndex      (byte, source int32 truncated)
//     [1]  characterIndex  (byte)
//     [2]  levels          (byte)
//     [3]  items           (byte)
//     [4]  worlds          (byte)
//     [5..6] turns         (int16, source int32 truncated)
//     [7..10] score        (int32, full)
//
// the binary computes count via `((end - begin) >> 2) * -0x49` cast to char,
// which is the modular inverse trick to compute count without dividing by
// the 28-byte stride. our port uses vector::size() directly.

// FUN_1000483e8. magic-gate, then variable-length scatter into
// game.leaderboardSaveBuffer.
static void loadScoreRecords(Game& game, const uint8_t* blob, size_t length) {

    if (blob == nullptr || length == 0) {
        return;
    }

    const uint32_t blobMagic = static_cast<uint32_t>(blob[0]);
    const uint32_t memMagic  = static_cast<uint32_t>(game.leaderboardSaveMagic());

    if (blobMagic != memMagic) {
        SDL_Log("DEBUG_SAVE: loadScoreRecords rejected -- magic 0x%X != 0x%X",
                blobMagic, memMagic);
        return;
    }

    auto& buf = game.leaderboardSaveBuffer();
    const uint8_t count = blob[1];
    buf.resize(count);

    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t* e = blob + 2 + (i * 11);
        LeaderboardMenu::XferEntry& dst = buf[i];
        dst.worldIndex     = e[0];
        dst.characterIndex = e[1];
        dst.levels         = e[2];
        dst.items          = e[3];
        dst.worlds         = e[4];
        std::memcpy(&dst.turns, e + 5, sizeof(int16_t));
        std::memcpy(&dst.score, e + 7, sizeof(int32_t));
        // binary loads `turns` as a ushort into a uint slot (zero-extend).
        // our memcpy fills only the low 2 bytes; the upper 2 stay at the
        // freshly-resized vector's value-init zero, giving the same result.
    }

    SDL_Log("DEBUG_SAVE: loadScoreRecords OK -- %u entries", count);
}

// FUN_10004719c. clear leaderboardSaveDirty, pack variable-length blob
// into the slot's scratch vector.
static const uint8_t* encodeScoreRecords(Game& game, size_t* outLength) {
    game.leaderboardSaveDirty() = false;

    const auto& buf = game.leaderboardSaveBuffer();
    const size_t count = buf.size();
    const size_t totalSize = 2 + (count * 11);

    auto& scratch = game.saveScratch(3);
    scratch.resize(totalSize);
    uint8_t* dst = scratch.data();

    dst[0] = static_cast<uint8_t>(game.leaderboardSaveMagic() & 0xFF);
    dst[1] = static_cast<uint8_t>(count);

    for (size_t i = 0; i < count; ++i) {
        uint8_t* e = dst + 2 + (i * 11);
        const LeaderboardMenu::XferEntry& src = buf[i];
        e[0] = static_cast<uint8_t>(src.worldIndex     & 0xFF);
        e[1] = static_cast<uint8_t>(src.characterIndex & 0xFF);
        e[2] = static_cast<uint8_t>(src.levels         & 0xFF);
        e[3] = static_cast<uint8_t>(src.items          & 0xFF);
        e[4] = static_cast<uint8_t>(src.worlds         & 0xFF);
        const int16_t turns16 = static_cast<int16_t>(src.turns);
        std::memcpy(e + 5, &turns16,    sizeof(int16_t));
        std::memcpy(e + 7, &src.score,  sizeof(int32_t));
    }

    *outLength = totalSize;

    SDL_Log("DEBUG_SAVE: encodeScoreRecords -- %zu entries", count);
    return dst;
}

// ===== slot 4 ("ach"), FUN_1000484e0 / FUN_1000472cc, task H/F =====
//
// blob layout (variable length):
//   [0]                                    version magic
//   [1]                                    states count (typ. 50)
//   [2 .. 1 + statesCount]                 states bytes (1 per entry)
//   next byte                              counters count
//   per entry (2 bytes)                    key, value
//   next byte                              eventKinds count
//   per entry (1 byte)                     key
//   next byte                              eventIds count
//   per entry (1 byte)                     key
//   next byte                              snagKinds count
//   per entry (1 byte)                     key
//   last byte                              damagedThisRun

// FUN_1000484e0. magic-gate, then scatter into game.achievementSaveBuffer.
static void loadAchievements(Game& game, const uint8_t* blob, size_t length) {

    if (blob == nullptr || length == 0) {
        return;
    }

    const uint32_t blobMagic = static_cast<uint32_t>(blob[0]);
    const uint32_t memMagic  = static_cast<uint32_t>(
                                  game.achievementSaveBuffer().versionMagic);

    if (blobMagic != memMagic) {
        SDL_Log("DEBUG_SAVE: loadAchievements rejected -- magic 0x%X != 0x%X",
                blobMagic, memMagic);
        return;
    }

    AchievementSaveBuffer& snap = game.achievementSaveBuffer();

    const uint8_t statesCount = blob[1];
    snap.states.resize(statesCount);

    for (uint8_t i = 0; i < statesCount; ++i) {
        snap.states[i] = blob[2 + i];
    }

    size_t cur = 2 + statesCount;
    const uint8_t countersCount = blob[cur];
    snap.counters.clear();

    for (uint8_t i = 0; i < countersCount; ++i) {
        const int k = blob[cur + 1 + i * 2];
        const int v = blob[cur + 1 + i * 2 + 1];
        snap.counters[k] = v;
    }

    cur += 1 + countersCount * 2;
    const uint8_t eventKindsCount = blob[cur];
    snap.eventKinds.clear();

    for (uint8_t i = 0; i < eventKindsCount; ++i) {
        snap.eventKinds.insert(blob[cur + 1 + i]);
    }

    cur += 1 + eventKindsCount;
    const uint8_t eventIdsCount = blob[cur];
    snap.eventIds.clear();

    for (uint8_t i = 0; i < eventIdsCount; ++i) {
        snap.eventIds.insert(blob[cur + 1 + i]);
    }

    cur += 1 + eventIdsCount;
    const uint8_t snagKindsCount = blob[cur];
    snap.snagKinds.clear();

    for (uint8_t i = 0; i < snagKindsCount; ++i) {
        snap.snagKinds.insert(blob[cur + 1 + i]);
    }

    cur += 1 + snagKindsCount;
    snap.damagedThisRun = (blob[cur] == 1) ? 1 : 0;

    SDL_Log("DEBUG_SAVE: loadAchievements OK -- states=%u counters=%u "
            "ek=%u ei=%u sk=%u dmg=%d",
            statesCount, countersCount, eventKindsCount, eventIdsCount,
            snagKindsCount, snap.damagedThisRun);
}

// FUN_1000472cc. clear saveSlot4Dirty, pack variable-length blob into
// the slot's scratch vector.
static const uint8_t* encodeAchievements(Game& game, size_t* outLength) {
    game.saveSlot4Dirty() = false;

    const AchievementSaveBuffer& snap = game.achievementSaveBuffer();

    const size_t totalSize = 1                                  // magic
                           + 1 + snap.states.size()             // states
                           + 1 + snap.counters.size() * 2       // counters
                           + 1 + snap.eventKinds.size()         // eventKinds
                           + 1 + snap.eventIds.size()           // eventIds
                           + 1 + snap.snagKinds.size()          // snagKinds
                           + 1;                                 // damagedThisRun

    auto& scratch = game.saveScratch(4);
    scratch.resize(totalSize);
    uint8_t* dst = scratch.data();

    dst[0] = static_cast<uint8_t>(snap.versionMagic & 0xFF);
    dst[1] = static_cast<uint8_t>(snap.states.size());

    for (size_t i = 0; i < snap.states.size(); ++i) {
        dst[2 + i] = static_cast<uint8_t>(snap.states[i] & 0xFF);
    }

    size_t cur = 2 + snap.states.size();
    dst[cur] = static_cast<uint8_t>(snap.counters.size());
    {
        size_t i = 0;

        for (const auto& [k, v] : snap.counters) {
            dst[cur + 1 + i * 2]     = static_cast<uint8_t>(k & 0xFF);
            dst[cur + 1 + i * 2 + 1] = static_cast<uint8_t>(v & 0xFF);
            ++i;
        }
    }

    cur += 1 + snap.counters.size() * 2;
    dst[cur] = static_cast<uint8_t>(snap.eventKinds.size());
    {
        size_t i = 0;

        for (int k : snap.eventKinds) {
            dst[cur + 1 + i] = static_cast<uint8_t>(k & 0xFF);
            ++i;
        }
    }

    cur += 1 + snap.eventKinds.size();
    dst[cur] = static_cast<uint8_t>(snap.eventIds.size());
    {
        size_t i = 0;

        for (int k : snap.eventIds) {
            dst[cur + 1 + i] = static_cast<uint8_t>(k & 0xFF);
            ++i;
        }
    }

    cur += 1 + snap.eventIds.size();
    dst[cur] = static_cast<uint8_t>(snap.snagKinds.size());
    {
        size_t i = 0;

        for (int k : snap.snagKinds) {
            dst[cur + 1 + i] = static_cast<uint8_t>(k & 0xFF);
            ++i;
        }
    }

    cur += 1 + snap.snagKinds.size();
    dst[cur] = snap.damagedThisRun;

    *outLength = totalSize;

    SDL_Log("DEBUG_SAVE: encodeAchievements -- states=%zu counters=%zu "
            "ek=%zu ei=%zu sk=%zu dmg=%d",
            snap.states.size(), snap.counters.size(), snap.eventKinds.size(),
            snap.eventIds.size(), snap.snagKinds.size(), snap.damagedThisRun);
    return dst;
}

// ----------------------------------------------------------------------
// dispatchers: 1:1 ports of the iOS binary's slot-fanout functions.
// ----------------------------------------------------------------------

namespace {

// per-slot file basenames. matches the iOS NSUserDefaults keys
// "sav" / "set" / "unl" / "sco" / "ach".
constexpr const char* kSlotBasenames[SaveSystem::NUM_SLOTS] = {
    "sav.bin",
    "set.bin",
    "unl.bin",
    "sco.bin",
    "ach.bin",
};

// FUN_10004621c. return whether slot `slot` has a pending write.
bool isSlotDirty(Game& game, SaveSystem::SlotId slot) {

    switch (slot) {
        case SaveSystem::SAV: return game.saveSlot0Dirty() != 0;
        case SaveSystem::SET: return game.saveSlot1Dirty();
        case SaveSystem::UNL: return game.shopSaveDirty();
        case SaveSystem::SCO: return game.leaderboardSaveDirty();
        case SaveSystem::ACH: return game.saveSlot4Dirty();
        default:              return false;
    }
}

// FUN_1000475b8. dispatch a loaded blob into the matching slot loader.
void dispatchLoad(Game& game, SaveSystem::SlotId slot,
                  const uint8_t* bytes, size_t length) {

    switch (slot) {
        case SaveSystem::SAV:
            loadSavedGame(game, bytes, length); break;
        case SaveSystem::SET:
            loadSettings(game, bytes, length); break;
        case SaveSystem::UNL:
            loadUnlocks(game, bytes, length); break;
        case SaveSystem::SCO:
            loadScoreRecords(game, bytes, length); break;
        case SaveSystem::ACH:
            loadAchievements(game, bytes, length); break;
        default:              break;
    }
}

// FUN_1000462ac. clear slot dirty, encode, return scratch ptr + length
// via outLength. nullptr return = "delete the file" (slot 0's dirty == 2
// path; see FUN_1000463a4).
const uint8_t* dispatchEncode(Game& game, SaveSystem::SlotId slot,
                              size_t* outLength) {
    *outLength = 0;

    switch (slot) {
        case SaveSystem::SAV: return encodeSavedGame(game, outLength);
        case SaveSystem::SET: return encodeSettings(game, outLength);
        case SaveSystem::UNL: return encodeUnlocks(game, outLength);
        case SaveSystem::SCO: return encodeScoreRecords(game, outLength);
        case SaveSystem::ACH: return encodeAchievements(game, outLength);
        default:              return nullptr;
    }
}

// ----- file I/O helpers -----

// returns the absolute path for slot `slot`'s save file.
// uses SDL's per-app internal-storage accessor; on Android that resolves
// to /data/data/<package>/files/.
std::string slotPath(SaveSystem::SlotId slot) {
    const char* base = SDL_AndroidGetInternalStoragePath();

    if (base == nullptr) {
        // fallback for non-Android targets / SDL test rigs: cwd.
        return std::string(kSlotBasenames[slot]);
    }

    std::string p(base);
    p += '/';
    p += kSlotBasenames[slot];
    return p;
}

// read the entire contents of `path` into `out`. returns false if the file
// is missing (silent, first-launch case) or unreadable.
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");

    if (f == nullptr) {
        // missing file = first launch / never saved. not an error.
        return false;
    }

    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }

    const long size = std::ftell(f);

    if (size < 0) {
        std::fclose(f);
        return false;
    }

    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }

    out.resize(static_cast<size_t>(size));
    const size_t read = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);

    if (read != out.size()) {
        SDL_Log("DEBUG_SAVE: short read on %s (%zu of %zu bytes)",
                path.c_str(), read, out.size());
        return false;
    }
    return true;
}

// atomic-ish write: write to "<path>.tmp" then rename. avoids leaving a
// partially-written blob on disk if the process dies mid-write.
bool writeFile(const std::string& path, const uint8_t* bytes, size_t length) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");

    if (f == nullptr) {
        SDL_Log("DEBUG_SAVE: open(%s, wb) failed: %s",
                tmp.c_str(), std::strerror(errno));
        return false;
    }

    const size_t written = std::fwrite(bytes, 1, length, f);
    std::fclose(f);

    if (written != length) {
        SDL_Log("DEBUG_SAVE: short write on %s (%zu of %zu bytes)",
                tmp.c_str(), written, length);
        std::remove(tmp.c_str());
        return false;
    }

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        SDL_Log("DEBUG_SAVE: rename(%s -> %s) failed: %s",
                tmp.c_str(), path.c_str(), std::strerror(errno));
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// delete a slot's file (slot 0's dirty == 2 = "delete the save" case).
void deleteFile(const std::string& path) {

    if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
        SDL_Log("DEBUG_SAVE: remove(%s) failed: %s",
                path.c_str(), std::strerror(errno));
    }
}

}  // namespace

// ----------------------------------------------------------------------
// SaveSystem public API.
// ----------------------------------------------------------------------

void SaveSystem::load(Game& game) {
    SDL_Log("DEBUG_SAVE: SaveSystem::load(): reading 5 slot files");

    for (int s = 0; s < NUM_SLOTS; ++s) {
        const auto slot = static_cast<SlotId>(s);
        const std::string path = slotPath(slot);

        std::vector<uint8_t> blob;
        const bool ok = readFile(path, blob);

        if (!ok || blob.empty()) {
            SDL_Log("DEBUG_SAVE: slot %d (%s) -- no file",
                    s, kSlotBasenames[s]);
            continue;
        }

        SDL_Log("DEBUG_SAVE: slot %d (%s) -- loaded %zu bytes",
                s, kSlotBasenames[s], blob.size());
        dispatchLoad(game, slot, blob.data(), blob.size());
    }
}

void SaveSystem::flushDirty(Game& game) {

    for (int s = 0; s < NUM_SLOTS; ++s) {
        const auto slot = static_cast<SlotId>(s);

        if (!isSlotDirty(game, slot)) {
            continue;
        }

        size_t length = 0;
        const uint8_t* bytes = dispatchEncode(game, slot, &length);

        if (bytes == nullptr || length == 0) {
            // encoder cleared the dirty bit but produced nothing to
            // write (stub, or a real encoder that determined no save
            // is needed). slot-0's "delete the saved run" case calls
            // deleteFile from inside its own encoder; the framework
            // does not infer deletion from a zero-length return.
            continue;
        }

        const std::string path = slotPath(slot);

        if (writeFile(path, bytes, length)) {
            SDL_Log("DEBUG_SAVE: slot %d (%s) -- wrote %zu bytes",
                    s, kSlotBasenames[s], length);
        }
    }
}
