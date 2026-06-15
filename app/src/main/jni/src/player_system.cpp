#include "player_system.h"
#include "game_board.h"   // GameBoard accessors used by Item drawing paths
#include "item.h"
#include "perk.h"
#include "snag_content.h"   // SnagContent.type / .atk / .def for combat scaling
#include "game.h"
#include "portrait_table.h"
#include <GLES/gl.h>
#include <cmath>
#include <cstring>

// reconstructed from Ghidra FUN_100038b18 (base init) + FUN_100056138 (derived init)
void PlayerSystem::init() {
    // MovableActor base. the binary passes a null parent and nothing reads it;
    // code that needs the Game struct uses getGame().
    initBase(nullptr);

    // base stat values are 1 (the floor recomputeStats clamps summed item
    // bonuses against); the live stats start at 0, health at 1.
    characterIndex = 0;
    currentLevel       = 0;
    currentHealth  = 1;
    maxHealth      = 1;
    attack         = 0;
    defence        = 0;
    baseATK        = 1;
    baseDEF        = 1;
    baseHP       = 1;

    // stat ranges stay empty until the first recomputeStats.
    for (int i = 0; i < 3; i++) {
        statRanges[i].lo = 0;
        statRanges[i].hi = 0;
    }
    baseItems[0] = nullptr;
    baseItems[1] = nullptr;
    baseItems[2] = nullptr;
    perks.clear();
    exitVanishing       = false;
    std::memset(pad1A1, 0, sizeof(pad1A1));
    characterPulseT = 0.0f;                          // 0 = pulse idle
}

// reconstructed from Ghidra FUN_1000562f4
void PlayerSystem::reset(int charIdx) {
    // free the 3 starter Item slots (plain delete; see push() for why that's
    // enough given Item's trivial teardown).
    for (int i = 0; i < 3; i++) {

        if (baseItems[i]) {
            delete baseItems[i];
            baseItems[i] = nullptr;
        }
    }

    // free every Perk and shrink the perks vector back to empty.
    for (Perk* p : perks) {

        if (p) {
            delete p;
        }
    }
    perks.clear();

    // clamp character index to [0, 30] (binary uses 0x1f as the inclusive max,
    // i.e. effectively [0..30]; we have 30 portraits so [0..29] is the valid
    // range; out-of-range indices fall through to setPortraitVisual's no-op).
    int clamped = (charIdx > 30) ? 0 : charIdx;
    characterIndex = clamped;

    // apply the matching portrait UV + size to the character Quad
    setPortraitVisual(clamped, baseQuad);

    // a fresh run resets the player to level 0 with bare base stats.
    currentLevel = 0;
    attack   = 1;
    defence  = 1;
    baseATK  = 1;
    baseDEF  = 1;
    baseHP = 1;

    exitVanishing = false;

    // clear any leftover exit-vanish transform on the character Quad.
    baseQuad.scaleX = 1.0f;
    baseQuad.scaleY = 1.0f;
    baseQuad.rotation = 0.0f;
}

// reconstructed from Ghidra FUN_100056528 (secondaryInit)
void PlayerSystem::secondaryInit() {
    // same clean slate as reset's tail: clear the exit-vanish flag + transform.
    exitVanishing     = false;
    baseQuad.scaleX   = 1.0f;
    baseQuad.scaleY   = 1.0f;
    baseQuad.rotation = 0.0f;
}

// reconstructed from Ghidra FUN_100056b98. begins the exit-vanish; update() then
// shrinks and spins the avatar away while the next level loads.
void PlayerSystem::onLevelEnd() {
    exitVanishing   = true;
    characterPulseT = 0.0f;
}

// reconstructed from Ghidra FUN_100038b88 (vtable[2], primary draw)
void PlayerSystem::draw() {
    // early-out if hidden and the fade has fully completed
    if (!visible && fadeT >= 1.0f) {
        return;
    }

    // texture binding is done by the caller (GameBoard::draw binds tex 8 = sheet1
    // before invoking PlayerSystem.draw, since portraits live on sheet1).
    baseQuad.draw();
}

// reconstructed from Ghidra FUN_1000567fc
void PlayerSystem::push(Item* item) {
    int slot = item->type;

    if (slot < 0 || slot > 2) {
        // bad slot, ignore
        return;
    }

    // free any existing Item in the slot. plain delete suffices: Item has no
    // user-defined dtor and its sub-objects own no heap resources yet.
    Item* existing = baseItems[slot];

    if (existing) {
        delete existing;
    }

    baseItems[slot] = item;
    recomputeStats();
}

// reconstructed from Ghidra FUN_1000568bc
void PlayerSystem::recomputeStats() {
    // sum atk/def/hp across all 3 starter slots
    int32_t sumATK  = 0;
    int32_t sumDEF  = 0;
    int32_t sumHP = 0;

    for (int i = 0; i < 3; i++) {
        Item* a = baseItems[i];

        if (a) {
            sumATK  += a->atk;
            sumDEF  += a->def;
            sumHP += a->hp;
        }
    }

    // store min/max ranges against the per-stat base values (baseATK / baseDEF / baseHP)
    auto applyRange = [](StatRange& r, int32_t sum, int32_t base) {
        r.lo = (sum < base) ? sum  : base;
        r.hi = (sum < base) ? base : sum;
    };
    applyRange(statRanges[0], sumATK,  baseATK);
    applyRange(statRanges[1], sumDEF,  baseDEF);
    applyRange(statRanges[2], sumHP, baseHP);

    // maxHealth = (baseHP + sumHP) * 5
    int32_t newMaxHP = (baseHP + sumHP) * 5;
    maxHealth = newMaxHP;

    // clamp currentHealth to <= maxHealth in case max dropped
    if (currentHealth > newMaxHP) {
        currentHealth = newMaxHP;
    }
}

// stepToward (FUN_100038e10) is inherited from MovableActor.

// reconstructed from Ghidra FUN_100056658. degrade the player's atk / def
// after a combat round. mirrors SnagContent::resolveCombatDelta on the
// snag side; both are called in sequence by FUN_100025dcc.
void PlayerSystem::degradeStatsAfterCombat(uint32_t* atkInOut,
                                           uint32_t* defInOut,
                                           SnagContent* snag,
                                           int snagDamage) {

    if (snag->type == 0x48) {
        // Parasite: skip atk damp, just halve def with SPA_14 mitigation.
        uint32_t origDef   = *defInOut;
        uint32_t halvedDef = origDef >> 1;
        *defInOut = halvedDef;

        int armor = baseItemSpecialAbilityValue(0xe);   // SPA_14
        uint32_t recovered = (uint32_t)armor + halvedDef;

        if (origDef <= recovered) {
            recovered = origDef;
        }

        *defInOut = recovered;
        return;
    }

    if (snag->type == 0x12) {
        // Shred of Doubt: subtract clamp(snag.atk, atk) and clamp(snag.def, def).
        uint32_t origAtk = *atkInOut;
        uint32_t loseA   = (uint32_t)snag->atk;

        if (origAtk <= loseA) {
            loseA = origAtk;
        }

        *atkInOut = origAtk - loseA;

        uint32_t origDef = *defInOut;
        uint32_t loseD   = (uint32_t)snag->def;

        if (origDef <= loseD) {
            loseD = origDef;
        }

        *defInOut = origDef - loseD;
        return;
    }

    // default: damp atk if connected, then halve def. SPA_2 / SPA_14
    // mitigate the damp / halve respectively.
    if (snagDamage > 0) {
        uint32_t origAtk   = *atkInOut;
        uint32_t dampedAtk = (origAtk * 2 + 2) / 3;
        *atkInOut = dampedAtk;

        int armor = baseItemSpecialAbilityValue(2);     // SPA_2
        uint32_t recovered = (uint32_t)armor + dampedAtk;

        if (origAtk <= recovered) {
            recovered = origAtk;
        }

        *atkInOut = recovered;
    }

    uint32_t origDef   = *defInOut;
    uint32_t halvedDef = origDef >> 1;
    *defInOut = halvedDef;

    int armorD = baseItemSpecialAbilityValue(0xe);      // SPA_14
    uint32_t recoveredD = (uint32_t)armorD + halvedDef;

    if (origDef <= recoveredD) {
        recoveredD = origDef;
    }

    *defInOut = recoveredD;
}

// reconstructed from Ghidra FUN_10005653c (vtable[3], derived update). runs
// MovableActor's 4-stage base update, then layers on the exit anim.
void PlayerSystem::update(float dt) {
    MovableActor::update(dt);

    // exit animation: a one-shot flourish (scale shrink + 360deg spin
    // over 0.7s) gated on exitVanishing, which onLevelEnd sets at level exit.
    // the gate is load-bearing: without it the anim runs at level start and
    // ends at scale 0, leaving the character invisible.
    constexpr float VANISH_DURATION = 0.7f;     // DAT_10005a6f0
    constexpr float VANISH_PI       = 3.1415927f;
    constexpr float VANISH_DEG      = 360.0f;   // DAT_10005a6f8

    if (exitVanishing && characterPulseT < 1.0f) {
        float t = characterPulseT + dt / VANISH_DURATION;
        if (t > 1.0f) {
            t = 1.0f;
        }
        characterPulseT = t;

        // cosine ease: u = 0.5 - cos(t*PI)*0.5 runs 0 -> 0.5 -> 1, so
        // scale = 1 - u starts at 1 and smoothly shrinks to 0.
        float u = 0.5f - std::cos(t * VANISH_PI) * 0.5f;
        float scale = 1.0f - u;
        baseQuad.scaleX = scale;
        baseQuad.scaleY = scale;
        // full 360deg spin over the duration
        baseQuad.rotation = t * VANISH_DEG;
    }
}

// FUN_1000565f0 (vtable[6]). dim the avatar to transparent as the fade-out
// stage completes: alpha = (1 - fadeT) * 255 (255 at fadeT=0, 0 at fadeT=1).
void PlayerSystem::onFade(float fadeT) {
    setAlpha((uint8_t)((1.0f - fadeT) * 255.0f));
}

// FUN_100056624 (vtable[7]). ramp the avatar up from transparent as it scales
// in / revives: alpha = scaleOutT * 255 (0 at scaleOutT=0, 255 at scaleOutT=1).
void PlayerSystem::onScaleOut(float scaleOutT) {
    setAlpha((uint8_t)(scaleOutT * 255.0f));
}

// reconstructed from Ghidra FUN_1000569a0.
int PlayerSystem::perkLevel(int queryType) const {

    for (const Perk* p : perks) {

        if (p && p->perkType == queryType) {
            return p->perkLevel;
        }
    }

    return 0;
}

// reconstructed from Ghidra FUN_100056a24.
//
// the level-up panel commit loop calls this with the chosen perkType. types
// 0/4/8 also bump a base stat (ATK / DEF / HP) and recomputeStats; then, for
// any type, the perks vector is walked, replacing an existing match at level+1
// or appending a fresh Perk(perkType, 1).
void PlayerSystem::addOrUpgradePerk(int perkType) {
    // stat-bump branch (perkType 0 = ATK, 4 = DEF, 8 = HP). any other type
    // skips straight to the find-or-create below.
    if (perkType == 0) {
        baseATK += 1;
        recomputeStats();
    } else if (perkType == 4) {
        baseDEF += 1;
        recomputeStats();
    } else if (perkType == 8) {
        baseHP += 1;
        recomputeStats();
    }

    // step 2: find-or-create. walk perks looking for an existing match.
    for (size_t i = 0; i < perks.size(); ++i) {
        Perk* existing = perks[i];

        if (existing != nullptr && existing->perkType == perkType) {
            int oldLevel = existing->perkLevel;
            delete existing;
            Perk* fresh = new Perk();
            fresh->init(perkType, oldLevel + 1);
            perks[i] = fresh;
            return;
        }
    }

    // no existing match: push a new perk at level 1.
    Perk* fresh = new Perk();
    fresh->init(perkType, 1);
    perks.push_back(fresh);
}

// reconstructed from Ghidra FUN_1000567b0 (FUN_1000334bc inlined). walks the 3
// base Items, scanning each one's 2 SpecialAbility slots for abilityType ==
// queryType and returning the first non-zero abilityVal. a matched slot with
// abilityVal == 0 counts as no-match, so we keep scanning the next Item.
int PlayerSystem::baseItemSpecialAbilityValue(int queryType) const {

    for (int i = 0; i < 3; i++) {
        const Item* item = baseItems[i];
        int v = 0;

        if (item) {

            for (int j = 0; j < 2; j++) {

                if (item->abilities[j].abilityType == queryType) {
                    v = item->abilities[j].abilityVal;
                    break;
                }
            }
        }

        if (v != 0) {
            return v;
        }
    }

    return 0;
}
