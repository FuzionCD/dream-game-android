#pragma once

#include "color_tint.h"
#include "movable_actor.h"
#include "snag_table.h"
#include "title_menu.h"   // for TileIcon
#include <cstdint>

// reconstructed from Ghidra:
//   ctor:           FUN_10003ccc8
//   sprite UV:      FUN_10003e0a8 (reads sprite ints from snag table) +
//                    FUN_100014d84 (applies as UV/size on baseQuad)
//   doppelganger:   FUN_10003dedc (kind=0x19 takes a special path that copies
//                    the player's portrait UV onto baseQuad)
//   stat baseline:  FUN_10003d244 (exp2f-based scaling from snags / items /
//                    worldIndex; output drives the per-kind multipliers)
//   stat displays:  FUN_10003d3a4 (atk display), FUN_10003d468 (def display),
//                    FUN_10003d530 (hp display)
//   getValues:      FUN_10003e438 (reads +0x134 type into output)
//
// SnagContent is the 0x498-byte object that holds an enemy snag's fight state,
// owned by TileObject at +0x208. the recognisable atk / def / hp 3-stat
// fight-card pattern lives here: 3 TileIcons (one quad per stat) + 3 ColorTints
// (each rendering its stat as digits via the bitmap font).
//
// SnagContent vtable @ DAT_1000746e0; see movable_actor.h for the slot
// roles + base values. only the per-class differences are noted here:
//   [3] 0x10003da34, overrides update (adds snag-specific per-frame logic
//                    on top of the inherited 4-stage MovableActor body).
//   [4] 0x10003dc38, SnagContent::setPosition (lays out the 3 stat-display
//                    sub-Quads + 3 ColorTints around the snag sprite using
//                    DAT_10005a32c..a344 offsets).
//   [5] 0x10003dd80, setAlpha; called from vtable[6].
//   [6] 0x10003de0c, fade-stage hook. dispatches vtable[5] with alpha =
//                    (1 - fadeT) * DAT_10005a344, fading the snag toward
//                    transparent during fade-out (no scale-down, distinct
//                    from TileContent's squish).
//   [7] 0x10003de40, scale-out-stage hook.
//   [8] 0x10003d9a8, drawFull: binds spriteTextureIdx, draws baseQuad + 3
//                    sub-quads + 3 stat tints when drawTints is true.
//
// `kind` is one of the 119 entries in the snag bestiary (see snag_table.h).
// kind=0 is the sentinel "None"; kinds 1..0x76 are the 118 named snags.
// the vast majority of kinds use the table's stat multipliers as-is; eleven
// kinds (1, 6, 0x0f, 0x11, 0x19, 0x24, 0x27, 0x47, 0x52, 0x6c, 0x70) take
// per-kind branches in init(); see snag_table.h's trailing comment block.
//
// SnagContent is also reused by TileObject::transformToSnag (FUN_1000135d0)
// when an effect turns a tile into a snag (e.g. Parasite via Infestation, kind
// 0x48): the same struct, allocated with the effect's snag kind. unrelated to
// Item.SpecialAbility (item.h).

class GameBoard;
class TileObject;
class PlayerSystem;

// FUN_100014d84.
void applySpriteUV(Quad& q, int u, int v, int w, int h, float scale = 1.0f);

class SnagContent : public MovableActor {
public:
    // FUN_10003ccc8: zero the struct, run MovableActor::initBase, init the
    // 3 sub-Quads + 3 ColorTints, look up sprite UV / stat multipliers in the
    // snag bestiary table, run player-progress-driven stat scaling, run the
    // per-kind switch for the special-cased kinds, register stat displays.
    //
    // `tileObj` is the owning TileObject (back-ref stored at +0x480).
    // `totalTurnCount` (= GameBoard+0x20) / `levelTurnCount` (+0x40) /
    // `worldIndex` (+0x8) feed the stat-scaling formula (FUN_10003d244).
    void init(SnagKind kind, TileObject* tileObj, PlayerSystem* player,
              int levelTurnCount, int pickupsFound, int worldIndex);

    // raw uint32_t overload for callers from binary code paths where the kind
    // arrives as an int (e.g. FUN_1000135d0 passes 0x48 directly when an effect
    // transforms a tile into a snag). simply forwards to the SnagKind version.
    void init(uint32_t kind, TileObject* tileObj, PlayerSystem* player,
              int levelTurnCount, int pickupsFound, int worldIndex) {
        init(static_cast<SnagKind>(kind), tileObj, player,
             levelTurnCount, pickupsFound, worldIndex);
    }

    // FUN_10003d614, explicit-stat snag builder. takes the final (hp, atk,
    // def) + per-kind extra values directly instead of computing them from
    // player progress, then does the full structural / sprite / display
    // build. init() delegates here after computing its stats + extras; the
    // saved-game restore path (FUN_100016b18 -> FUN_100013f04) calls it with
    // the values captured at save time.
    //
    // extra0 / extra1 are the per-kind scratch values written to +0x490 /
    // +0x494: Obsession uses both (0x64 / 2); Reluctance uses extra0 (2);
    // Doubt / Despair set consumedFlag = (extra0 == 1). other kinds ignore
    // them. player is needed for the Doppelganger (0x19) portrait pull.
    void initExplicit(uint32_t kind, int hp, int atk, int def,
                      uint32_t extra0, uint32_t extra1,
                      TileObject* tileObj, PlayerSystem* player);

    // FUN_10003d3a4 / FUN_10003d468 / FUN_10003d530, stat display setters.
    // each pushes the new value into the corresponding ColorTint (rendering
    // it as digits) and snaps the tint's position relative to the sub-Quad.
    //
    // optional `tweenBoard` + `posOffset`: when both are non-null, also push
    // a floating "+N" / "-N" stat-change tween onto the board's stat-tween
    // queue (textStyle 0). source pos = own tint's posX/posY + posOffset.
    // matches the binary's `if (param_3 != 0) { ... FUN_10002c00c(...) }`
    // gate; combat-resolution callers (FUN_10001df54, Chunk D end-of-turn)
    // pass non-null queue + offset to surface the delta visually.
    void setAtkDisplay(int value, GameBoard* tweenBoard = nullptr,
                       const float* posOffset = nullptr);
    void setDefDisplay(int value, GameBoard* tweenBoard = nullptr,
                       const float* posOffset = nullptr);
    void setHpDisplay(int value, GameBoard* tweenBoard = nullptr,
                      const float* posOffset = nullptr);

    // FUN_10003dc38 (vtable[4] override), set the snag's position. lays out
    // the 3 stat-display sub-Quads + 3 ColorTints around the snag sprite using
    // the per-stat offsets from DAT_10005a32c..a344. called by TileObject::
    // setPosition when the snag's parent tile moves, and by MovableActor::
    // update each frame during sendToward / triggerBumpAnim animation.
    void setPosition(float x, float y) override;

    // FUN_10003da34 (vtable[3] override), snag-specific per-frame logic on
    // top of MovableActor's 4-stage base body. two extras:
    //   1. scale478 swap-slide animation tick (Mania visual swap).
    //   2. move-completion reattachment: when targetTile is set AND both
    //      moveT and spawnT are settled, this swaps the snag's tileParent
    //      to targetTile and attaches it to the new tile's snagContent slot
    //      via TileObject::attachSnag. this is the hook that makes a chased
    //      snag re-enter the world's logical model; without it, the snag
    //      ends up visually animating but logically detached from every
    //      tile, and combat queries (getSnagIfAlive) never find it again.
    void update(float dt) override;

    // FUN_10003d9a8 (vtable[8]), full snag draw: bind spriteTextureIdx,
    // draw baseQuad (the snag sprite), bind tex 9 for UI atlas, draw the 3
    // stat-display sub-Quads (HP / ATK / DEF), then if drawTints is true draw
    // the 3 ColorTints (which render the stat numbers as digits).
    void drawFull(bool drawTints);

    // FUN_10003dd80, vtable[5] override. propagates an alpha byte across
    // every visible sub-element: baseQuad (sprite), the 3 stat displays
    // (hp / atk / def TileIcons), and the 3 stat ColorTints. consumed by
    // onFade / onScaleOut to drive the snag's fade-in/-out as a single
    // transparency change rather than the per-axis squish TileContent
    // uses, and by syncGlobalTileAlpha to dim the snag during drag mode.
    void setAlpha(uint8_t alpha) override;

    // FUN_10003de0c, vtable[6] override. fade-out alpha curve:
    // alpha = (1 - fadeT) * 255 (full -> transparent).
    void onFade(float fadeT) override;

    // FUN_10003de40, vtable[7] override. fade-in alpha curve (used on
    // revive): alpha = scaleOutT * 255 (transparent -> full).
    void onScaleOut(float scaleOutT) override;

    // FUN_10003de74, kick the snag's move animation toward `target`.
    // pushes (target.posXY, target.gridColRow) onto the move queue, sets
    // +0x488 = target (the "associated tile" back-ref), and clears the
    // original parent's snagContent slot so the snag is no longer rendered
    // by its old tile. when `reparent` is true: also moves the SnagContent*
    // entry in the trackedContent vector from old parent to target. the
    // pre-commit page-walk in FUN_100025238 passes reparent=0; the
    // post-death tail walk passes reparent=0 too (the kill-and-fade after
    // sweeps the visual).
    void sendToward(class TileObject* target, bool reparent);

    // FUN_10003ddec, clamped HP-loss helper: hp -= clamp(amount, 0, hp)
    // and refresh the HP display (with stat-tween at sourceXY+offset).
    // amount <= 0 is a no-op. used by FUN_100025dcc as the snag's "take
    // damage" path during combat resolution.
    void deductHpClamped(int amount, GameBoard* tweenBoard,
                         const float* posOffset);

    // FUN_10003e24c, visual ATK/DEF swap on a placed snag. updates atk
    // display to show old def, def display to show old atk, then swaps
    // each ColorTint to the other display's home position and resets
    // scale478 = 0 to kick the slide animation in vtable[3] update.
    // called by snag 5 (Mania) page-walk effect once per turn.
    void swapAtkDefDisplay();

    // FUN_10003e078, return SnagInfo[type].tier. per-snag-type difficulty
    // class: 0 = spawn-only, 1 = common, 2 = special, 3 = boss/elite.
    int tier() const;

    // FUN_10003e2f8, compute the snag's post-combat (atk, def) values.
    // mutates *snagAtkInOut and *snagDefInOut; the caller pushes both into
    // setAtkDisplay / setDefDisplay so the snag's stat display animates the
    // delta. branches on snag type:
    //   1    if playerDamage>0: atk' = (2*atk+2)/3; def' = def>>1;
    //        then atk' -= clamp(SPA_9, 0, atk'); def' = (def>>1) - clamp(SPA_10, 0, def>>1)
    //   7    same as default (atk damp + def halve)
    //   0x12 (Shred of Doubt)  no-op (returns immediately)
    //   0x20 (Stubbornness)    atk damped; def' = max(0, snagDamage)
    //   default                if playerDamage>0: atk' = (2*atk+2)/3; def' = def >> 1
    //
    // playerDamage = how much HP the snag knocked off the player this round
    // (= snag.atk - playerDef, can be < 1 when blocked). counterintuitive
    // gameplay rule preserved from the binary: when the snag scores a hit
    // (playerDamage > 0), its own displayed atk discharges to ~2/3, so the
    // snag becomes weaker on subsequent rounds for spending its attack.
    // snagDamage = how much def the snag loses (= playerAtk - snagDef,
    // clamped per snag type by the caller); only used directly for
    // type 0x20 where it overrides the def-halve rule.
    void resolveCombatDelta(class PlayerSystem* player,
                            uint32_t* snagAtkInOut,
                            uint32_t* snagDefInOut,
                            int playerDamage, uint32_t snagDamage);

    // FUN_10003dedc, re-derive UV / size + texture index from the current
    // `type`. Doppelganger (0x19) goes to tex 8 with a UV/size copy off
    // this snag's own baseQuad (round-trip when target == baseQuad);
    // every other kind goes to tex 10 with the SNAG_TABLE pixel UV.
    // default args write through to baseQuad / spriteTextureIdx and are
    // what mergeFrom uses. DetailPanel re-uses the same logic against its
    // own icon Quad by passing explicit target / outTexIdx.
    void refreshBaseSprite(Quad* target = nullptr, int* outTexIdx = nullptr);

    // FUN_10003df70, absorb `incoming` into this snag. sums atk/def/hp via
    // the display setters; if this is Honesty (type 1) and incoming isn't,
    // takes on incoming's type, refreshes the sprite, and copies the +0x490
    // scratch (consumedFlag + obsessionCount). Doppelganger absorptions
    // additionally copy incoming's full baseQuad visual state so the
    // surviving snag becomes a visual clone. caller still owns incoming and
    // is responsible for deleting it (and for firing the
    // OurPowersCombined / TheWagesOfTruth achievement check, which mirrors
    // the binary's FUN_10004e2c0 placement before this body).
    void mergeFrom(SnagContent& incoming);

    // ---- SnagContent-specific (0x134..0x497; comes after MovableActor) ----
    int       type;                // +0x134
    int       spriteTextureIdx;    // +0x138  (texture used for baseQuad, set
                                   //          by FUN_10003dedc to 8 for the
                                   //          Doppelganger, 10 for all other
                                   //          snags. consumed by SnagContent's
                                   //          vtable[8] draw FUN_10003d9a8.)
    int       hp;                  // +0x13C
    int       atk;                 // +0x140
    int       def;                 // +0x144
    // sub-Quad order in the binary is HP / ATK / DEF (read FUN_10003d3a4 / 468 /
    // 530: atk uses sub-Quad at +0x220, def at +0x2F8, hp at +0x148). same shuffle
    // for the 3 ColorTints. naming the fields by what they actually hold keeps
    // the stat-display setters straightforward.
    TileIcon  hpDisplay;           // +0x148..+0x21F
    TileIcon  atkDisplay;          // +0x220..+0x2F7
    TileIcon  defDisplay;          // +0x2F8..+0x3CF
    ColorTint hpTint;              // +0x3D0..+0x407   (color 0xff64ffff yellow)
    ColorTint atkTint;             // +0x408..+0x43F   (color 0xffc8ffff light yellow)
    ColorTint defTint;             // +0x440..+0x477   (color 0xffffe6c8 light blue)
    float     scale478;            // +0x478
    uint8_t   pad47C[4];           // +0x47C..+0x47F
    TileObject*     tileParent;    // +0x480
    // +0x488: "associated tile" back-ref. distinct from tileParent: this
    // is the target tile of an in-flight move animation (set by sendToward).
    // tileParent stays at the snag's current owner; targetTile is where the
    // snag is walking toward.
    TileObject*     targetTile;    // +0x488
    // +0x490: per-snag-type scratch byte. Doubt/Despair use 1-byte writes
    // (0/1); Reluctance writes 2 via a 4-byte cast; Obsession seeds 0x64
    // via an 8-byte combined store that also writes obsessionCount at
    // +0x494. all observed values fit in a byte. existing port call sites
    // read it as bool (truthy/falsy) which works because non-zero always
    // means "consumed."
    uint8_t   consumedFlag;        // +0x490
    uint8_t   pad491[3];           // +0x491..+0x493  (3-byte gap before next int)
    int32_t   obsessionCount;      // +0x494          (only used by Obsession)
};

static_assert(sizeof(SnagContent) == 0x498,
              "SnagContent must be exactly 0x498 bytes");
