#pragma once

#include "item.h"
#include "movable_actor.h"
#include "perk.h"
#include "quad.h"
#include <cstddef>
#include <cstdint>
#include <vector>

// reconstructed from Ghidra:
//   ctor FUN_100056138 (calls base FUN_100038b18), reset FUN_1000562f4,
//   draw vt[2] FUN_100038b88, update vt[3] FUN_10005653c (base FUN_100038bac),
//   push FUN_1000567fc, set-portrait FUN_100056478.
//
// the player entity, at GameBoard.playerSystem (0x1A8 bytes) contains the avatar Quad
// (the character icon that moves on the hex grid), the player's stats, the 3
// starter baseItems, and a vector of gained perks.
//
// inherits MovableActor (vtable PTR_FUN_100074660), the same 0x134-byte base
// TileContent and SnagContent use. inheriting rather than composing is what
// lets our derived fields land inside MovableActor's tail padding,
// matching the binary's layout. the base owns baseQuad, the visible flag, the
// 4 stage timers, and the movement queue.
//
// movement-queue node = 0x18 bytes (prev / next / data); data is the next
// (posX, posY) target. on moveT completion the front node's data is copied to
// moveFrom and the node is freed.

class PlayerSystem : public MovableActor {
public:
    // FUN_100038b18 base init + FUN_100056138 derived init. sets vtable,
    // visible=1, inits the character Quad, all 4 stage timers to 1.0 (no
    // animation pending), and the stat block. the base "parent" field
    // stays null and nothing reads it; code that needs the Game struct goes
    // through the getGame() global instead.
    void init();

    // FUN_1000562f4: per-level reset. deletes the 3 starter Item slots and
    // every Perk in the perks vector (then clears it), clears the character
    // index, applies the matching portrait UV via FUN_100056478, resets stat
    // counts to 1, and resets the character Quad's scale to (1, 1) / rotation
    // to 0.
    void reset(int characterIndex);

    // FUN_100038b88 (vtable[2]): early-out if !visible and fadeT >= 1.0,
    // otherwise draw the character Quad. that's it.
    void draw();

    // FUN_10005653c (vtable[3]): runs the base 4-stage update (FUN_100038bac):
    // spawn-in lerp, move-to-target lerp with queue pop, fade, scale-out. then
    // runs the character-token pulse (scale + 360deg spin over 0.7s) while
    // characterPulseT < 1.0.
    void update(float dt);

    // FUN_1000565f0 (vtable[6]) / FUN_100056624 (vtable[7]): ramp the avatar's
    // alpha during the fade-out / scale-in stages (the base no-ops leave alpha
    // untouched). onFade dims 255 to 0 as fadeT completes; onScaleOut ramps
    // 0 to 255 as the avatar scales in (revive / level start).
    void onFade(float fadeT);
    void onScaleOut(float scaleOutT);

    // FUN_1000567fc: store an Item* in the fixed slot keyed by the item's
    // `type` field (0..2), operator_delete-ing any prior occupant, then
    // recomputeStats(). Item is forward-declared in the param to dodge an
    // include cycle.
    void push(class Item* item);

    // FUN_100056528. clears exitVanishing and resets the avatar Quad's scale to
    // (1, 1) / rotation to 0. called from initLevelContent after reset() so the
    // avatar starts each level clean, with no leftover scale-out or spin from
    // the previous level's exit vanish.
    void secondaryInit();

    // FUN_100056b98. begins the exit-vanish: sets exitVanishing and rewinds
    // characterPulseT to 0; update() then shrinks the avatar's scale over 0.7s.
    // the level-exit transition (state 8) uses this to clear the player off the
    // board before the dim overlay takes over.
    void onLevelEnd();

    // FUN_1000568bc: re-aggregate sumATK / sumDEF / sumHP across the 3
    // starter Item slots, store min/max ranges into statRanges, and
    // recompute maxHealth = (baseHP + sumHP) * 5. clamps currentHealth
    // to maxHealth in case max went down.
    void recomputeStats();

    // stepToward (FUN_100038e10) is inherited from MovableActor.

    // FUN_100056658. post-combat ATK/DEF decay for the player, the mirror of
    // the snag's FUN_10003e2f8 (the caller FUN_100025dcc runs both back-to-back,
    // then writes the player via setATK/setDEF and the snag via setAtkDisplay/
    // setDefDisplay). branches on the snag's type (param_4):
    //   0x48 (Parasite)        playerDef = min(orig, (def>>1) + SPA_14)
    //   0x12 (Shred of Doubt)  playerAtk -= clamp(snag.atk, atk);
    //                          playerDef -= clamp(snag.def, def)
    //   default (snagDamage>0) playerAtk = min(orig, (2*atk+2)/3 + SPA_2);
    //                          playerDef = min(orig, (def>>1) + SPA_14)
    // SPA_2 / SPA_14 are passive item abilities (baseItemSpecialAbilityValue)
    // that soften the loss, armor for ATK / DEF; at 0 you get the bare halve /
    // damp. snagDamage = how much DEF the snag lost this round (playerAtk -
    // snagDef, clamped).
    void degradeStatsAfterCombat(uint32_t* atkInOut, uint32_t* defInOut,
                                 class SnagContent* snag, int snagDamage);

    // --- byte-exact struct fields ---
    // the player avatar's Quad / visible flag / 4-stage timers / movement
    // queue all live in the inherited MovableActor base.
    // note: when code reads/writes the avatar Quad, it does so as `baseQuad`
    // (the inherited member name).

    // ---- PlayerSystem-specific (0x134..0x1A7; lands in MovableActor's
    //      tail padding via the tail-padding optimization) ----
    int32_t characterIndex;        // (0..29; selects portrait UV)

    // player's current level (0-indexed). starts at 0 on new-game, +1 each time
    // the level-up panel commit drains its picks. only resets on new game.
    // the Item ctor (FUN_10003040c) reads it as the SpecialAbility magnitude
    // tier: `magnitude = rngInt(lo, lo + (currentLevel / 10) * step)`. every 10
    // levels bumps the tier, so items rolled later in a run come out stronger.
    int32_t currentLevel;          // (init 0)

    // player stats (read by GameplayHUD):
    int32_t currentHealth;         // (mirrored to GameplayHUD center number)
    int32_t maxHealth;             // (mirrored to GameplayHUD denominator)
    int32_t attack;                // (mirrored to GameplayHUD left tint)
    int32_t defence;               // (mirrored to GameplayHUD right tint)

    // base stat values (init 1 each, used as a floor when recomputeStats
    // takes min/max against summed Item contributions).
    int32_t baseATK;               // (init 1)
    int32_t baseDEF;               // (init 1)
    int32_t baseHP;                // (init 1)

    // per-stat (lo, hi) ranges computed by recomputeStats (FUN_1000568bc):
    //   [0] = ATK range  (min/max of baseATK,  sum of Item.atk fields)
    //   [1] = DEF range  (min/max of baseDEF,  sum of Item.def fields)
    //   [2] = HP   range (min/max of baseHP,  sum of Item.hp   fields)
    // these are read by gameplay code that needs to know "minimum guaranteed"
    // vs "maximum potential" stats given the current Item loadout.
    //
    // also read by FUN_100020450 to roll the magnitude for stat-bonus content
    // tiles. content type maps:
    //   type 2 -> ATK tile (magnitude from statRanges[0])
    //   type 3 -> DEF tile (magnitude from statRanges[1])
    //   type 6 -> HP tile  (magnitude from statRanges[2] - HP   Items scale
    //                       how much HP this tile heals when placed)
    // before any baseItems push all three ranges read [0, 0], because nothing
    // has been summed for this level yet.
    struct StatRange { int32_t lo; int32_t hi; };
    StatRange statRanges[3];       // (3 × 8 bytes)

    // 3 starter Item slots. each is a 0x610-byte Item object allocated via
    // operator_new during initLevel (binary's FUN_1000161fc). slot index
    // matches Item's `type` field (0 = atk, 1 = hp, 2 = def starter).
    Item* baseItems[3];

    // passive perks the player has unlocked. each entry is a 0x1F0-byte
    // Perk struct (perk.h) with a (perkType, perkLevel) lead-in. perks are
    // gained / leveled when the player levels up.
    std::vector<Perk*> perks;

    // FUN_1000569a0. walk perks and return the perkLevel of the first
    // entry whose perkType matches `queryType`; returns 0 when no match.
    // gameplay consumers use this to query the player's level for a
    // specific passive bonus. examples:
    //   perkLevel(0x09)  -> on-death HP refill tier (75% / 100%)
    //   perkLevel(0x0F)  -> 2-CTRL-tile chance tier (25% / 50% / 75% / 100%)
    //   perkLevel(0x10)  -> "{C} spots are closer together" (1 = on)
    //   (etc; see perk.h's runtime-pool comment for the full table.)
    int perkLevel(int queryType) const;

    // FUN_100056a24. add or upgrade a perk by type. the level-up panel commit
    // loop calls this once per perk pick. perkTypes 0/4/8 are the "stat-bump"
    // types; they also +1 the matching base stat (ATK / DEF / HP) and run
    // recomputeStats.
    void addOrUpgradePerk(int perkType);

    // FUN_1000567b0. walk the 3 base Items, scanning each one's 2 SpecialAbility
    // slots for abilityType == queryType and returning the first non-zero
    // abilityVal. the content-type switch in updateNavArrowAndConfirmDrag uses it
    // for case 3 (DEF tile, +ATK from SPA type 3) and case 5 ({C} tile, +HP from
    // SPA type 0xb). a matched slot with abilityVal == 0 counts as no-match.
    int baseItemSpecialAbilityValue(int queryType) const;

    // set to 1 when the character steps onto the exit tile at level end. update()
    // then plays a one-shot vanish (shrink to scale 0 + 360deg spin over 0.7s)
    // while the next level loads; the next reset() restores scale to 1.0 and
    // clears this flag, bringing the character back.
    bool exitVanishing;            // (init 0)
    float characterPulseT;         // (init 0; runs the spawn pulse on the
                                   //         first frame. also the state-9 timer:
                                   //         GameBoard's update case 9 reads it
                                   //         to gate the 9 -> 7 step.)
};