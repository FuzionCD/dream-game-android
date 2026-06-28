#include "snag_content.h"
#include "player_system.h"
#include "tile_object.h"       // TileObject (sendToward target + trackedContent)
#include "snag_table.h"        // kSnagTable (tier lookup)
#include "renderer.h"          // bindTexture
#include "game_board.h"        // GameBoard::pushStatTween for stat-change tween
#include "portrait_table.h"    // setPortraitVisual for the Doppelganger path
#include "random.h"            // rngInt / rngFloat, binary's MINSTD-style LCG
#include <GLES/gl.h>
#include <algorithm>           // std::find, std::max
#include <cmath>
#include <cstring>

// ----------------------------------------------------------------------------
// constants extracted from the binary's __TEXT,__const.
// ----------------------------------------------------------------------------
namespace {

// FUN_10003d244 (computeBaseStat), exp2f-based scaling.
// formula:
//   base = exp2(snags / SNAGS_EXP_DIV) * SNAGS_EXP_MUL + SNAGS_EXP_ADD
//   base *= (exp2(items / ITEMS_EXP_DIV) / 3 + ITEMS_EXP_ADD)
//   if jitter: base *= randf(JITTER_LO, JITTER_HI)
//   if world == 2: base *= WORLD2_MUL          (1.3x, harder world)
//   if world == 0: base *= WORLD0_MUL          (0.6x, easier world)
//   if applyTier (kind != 1):
//       floor = (world == 2) ? 3.0 : 2.0
//       base = max(base, floor)
//       base *= jitter ? randf(1.0, JITTER_HI) : DEFAULT_TIER_MUL
constexpr float SNAGS_EXP_DIV    = 170.0f;        // DAT_10005a2e8
constexpr float SNAGS_EXP_MUL    = 1.7f;          // DAT_10005a2ec
constexpr float SNAGS_EXP_ADD    = -0.7f;         // DAT_10005a2f0
constexpr float ITEMS_EXP_DIV    = 80.0f;         // DAT_10005a2f4
constexpr float ITEMS_EXP_ADD    = 0.65f;         // DAT_10005a2f8
constexpr float JITTER_LO        = 0.8f;          // DAT_10005a2fc
constexpr float JITTER_HI        = 1.2f;          // DAT_10005a300
constexpr float WORLD2_MUL       = 1.3f;          // DAT_10005a304
constexpr float WORLD0_MUL       = 0.6f;          // DAT_10005a308
constexpr float DEFAULT_TIER_MUL = 1.1f;          // DAT_10005a30c

// kind=1 RNG-rolled stat-distribution patterns.
constexpr float K1_PATTERN0_ATK_MUL  = 0.7f;      // DAT_10005a2e0
constexpr float K1_PATTERN1_HP_MUL   = 0.9f;      // DAT_10005a2dc
constexpr float K1_PATTERN23_ATK_MUL = 1.2f;      // DAT_10005a2d8
constexpr float K1_PATTERN3_HP_MUL   = 1.1f;      // DAT_10005a2e4
constexpr float K1_HALF              = 1.5f;      // inline constant, no DAT

// FUN_100014d84 (sprite-UV setter).
constexpr float TEX_PIXEL_INV    = 1.0f / 1024.0f; // DAT_100059ebc
constexpr float SCREEN_PIXEL_INV = 1.0f / 640.0f;  // DAT_100059ec0

// stat-display layout offsets (used by setAtkDisplay / setDefDisplay / setHpDisplay).
constexpr float ATK_TINT_X_OFFSET = -0.003125f;     // DAT_10005a310
constexpr float DEF_TINT_OFFSET   = -0.001562500023f; // DAT_10005a314 (also reused for hp X)
constexpr float HP_TINT_X_OFFSET  = -0.001562500023f; // DAT_10005a318
constexpr float HP_TINT_Y_LOW     = 0.001562500023f;  // DAT_10005a358 (hp <= 9)
constexpr float HP_TINT_Y_HIGH    = 0.003125f;        // DAT_10005a35c (hp > 9)

// ColorTint colors (extracted from the FUN_10003ccc8 disassembly). the binary
// applies them in the order it inits the tints (hpTint first, then atkTint,
// then defTint). bytes shift out as R / G / B / A on a
// little-endian target.
//   HP tint:  0xff64ffff -> R=ff G=ff B=64 A=ff (yellow)
//   ATK tint: 0xffc8ffff -> R=ff G=ff B=c8 A=ff (light yellow)
//   DEF tint: 0xffffe6c8 -> R=c8 G=e6 B=ff A=ff (light blue)
constexpr uint32_t HP_TINT_COLOR  = 0xff64ffff;
constexpr uint32_t ATK_TINT_COLOR = 0xffc8ffff;
constexpr uint32_t DEF_TINT_COLOR = 0xffffe6c8;

// stream indices used by FUN_10003d244 / FUN_10003ccc8 in the binary.
// matches the literal `4` passed to FUN_1000570ec / FUN_1000571d0 in those
// functions; they all share stream 4. when the saved-game load path
// (FUN_100016b18) is ported it will seed each stream from the
// GameSnapshot's per-level seed table.
constexpr uint32_t SNAG_RNG_STREAM = 4;

// FUN_10003d244, port of the exp2f-driven stat baseline calculation.
//   snags = snags-defeated counter (player progress)
//   items = items-found counter (player progress)
//   world = current worldIndex (0 = first/easier world, 2 = harder)
//   applyTier = bool (1 if kind != 1, 0 if kind == 1)
//   jitter   = bool (always 1 in the binary's call from FUN_10003ccc8)
float computeBaseStat(int snags, int items, int world, int applyTier, int jitter) {
    float fSnags = std::exp2f((float)snags / SNAGS_EXP_DIV);
    float base   = fSnags * SNAGS_EXP_MUL + SNAGS_EXP_ADD;

    float fItems = std::exp2f((float)items / ITEMS_EXP_DIV);
    base = base * (fItems / 3.0f + ITEMS_EXP_ADD);

    if (jitter != 0) {
        base *= rngFloat(JITTER_LO, JITTER_HI, SNAG_RNG_STREAM);
    }

    if (world == 2) {
        base *= WORLD2_MUL;
    } else if (world == 0) {
        base *= WORLD0_MUL;
    }

    if (applyTier != 0) {
        float floor = (world == 2) ? 3.0f : 2.0f;
        if (base < floor) {
            base = floor;
        }
        float scale = jitter ? rngFloat(1.0f, JITTER_HI, SNAG_RNG_STREAM) : DEFAULT_TIER_MUL;
        base *= scale;
    }

    return base;
}

}  // anonymous namespace

// FUN_100014d84, apply pixel UV + size to a Quad. inputs are pixel coords.
void applySpriteUV(Quad& q, int u, int v, int w, int h, float scale) {
    constexpr float kTexPxInv    = 1.0f / 1024.0f; // DAT_100059ebc
    constexpr float kScreenPxInv = 1.0f / 640.0f;  // DAT_100059ec0

    float u0 = (float)u                  * kTexPxInv;
    float v0 = (float)v                  * kTexPxInv;
    float u1 = (float)(u + w)            * kTexPxInv;
    float v1 = (float)(v + h)            * kTexPxInv;
    q.setTexCoords(u0, v0, u1, v1);

    float sw = ((float)w * scale) * kScreenPxInv;
    float sh = ((float)h * scale) * kScreenPxInv;
    q.setSize(sw, sh);
}

// ----------------------------------------------------------------------------
// SnagContent::init, port of FUN_10003ccc8.
// ----------------------------------------------------------------------------
void SnagContent::init(SnagKind kind_e, TileObject* tileObj, PlayerSystem* player,
                       int levelTurnCount, int pickupsFound, int worldIndex) {
    const uint32_t kind = static_cast<uint32_t>(kind_e);

    // init() computes the snag's stats + per-kind extras from player progress,
    // then delegates the structural / sprite / display build to initExplicit
    // (= FUN_10003d614). this mirrors the binary's two-function split:
    // FUN_10003ccc8 (this) computes the values; FUN_10003d614 builds the snag.
    // the structural setup, sprite, display-visual, and stat assignment all
    // live in initExplicit so both code paths share them.

    // ---- bestiary lookup (stat multipliers for non-kind-1 snags) ----
    const SnagInfo& info = snagInfo(kind);

    // ---- stat baseline ----
    int applyTier = (kind != 1) ? 1 : 0;
    float baseStat = computeBaseStat(levelTurnCount, pickupsFound, worldIndex, applyTier, 1);

    // ---- per-kind stat distribution ----
    int rolledAtk;
    int rolledDef;
    int rolledHp;

    if (kind == 1) {
        // FUN_10003ccc8's kind=1 branch: pick one of 4 stat distribution patterns.
        // each pattern produces a different (atk, def, hp) shape on top of baseStat.
        int pattern = rngInt(0, 3, SNAG_RNG_STREAM);
        float fAtk;
        float fDef;
        float hpScale;

        if (pattern == 0) {
            // pattern 0: strong atk, weak def, normal hp
            fAtk    = baseStat * K1_HALF;                // 1.5
            fDef    = baseStat * K1_PATTERN0_ATK_MUL;    // 0.7
            hpScale = 1.0f;                              // hp = baseStat
        } else if (pattern == 1) {
            // pattern 1: normal atk, doubled def, slightly less hp
            fAtk    = baseStat;                          // 1x
            fDef    = baseStat + baseStat;               // 2x
            hpScale = K1_PATTERN1_HP_MUL;                // 0.9
        } else if (pattern == 2) {
            // pattern 2: normal atk, 1.2x def, 1.5x hp
            fAtk    = baseStat;                          // 1x
            fDef    = baseStat * K1_PATTERN23_ATK_MUL;   // 1.2
            hpScale = K1_HALF;                           // 1.5
        } else {
            // pattern 3: 1.2x atk, 1.5x def, 1.1x hp
            fAtk    = baseStat * K1_PATTERN23_ATK_MUL;   // 1.2
            fDef    = baseStat * K1_HALF;                // 1.5
            hpScale = K1_PATTERN3_HP_MUL;                // 1.1
        }

        rolledAtk = std::max((int)fAtk, 1);
        rolledDef = std::max((int)fDef, 1);
        rolledHp  = (int)(baseStat * hpScale);
    } else {
        // every other kind: read multipliers directly from the snag table.
        rolledAtk = (int)(baseStat * info.atkMult);
        rolledDef = (int)(baseStat * info.defMult);
        rolledHp  = (int)(baseStat * info.hpMult);
    }

    // ---- per-kind post-processing (matches FUN_10003ccc8's switch after
    //      stat rolling). two flavors of work happen here:
    //        - stat modifications (Doppelganger / Infestation / Grief /
    //          Honesty / Attrition) adjust rolledAtk/Def/Hp in place;
    //        - extra-value computation (Obsession / Reluctance / Doubt /
    //          Despair) sets the consumedFlag / obsessionCount scratch values, which we
    //          pass to initExplicit instead of writing directly.
    //      initExplicit applies the extras + does the Doppelganger portrait;
    //      the Indecision (0x24) spawn-time decoration is handled after the
    //      delegate call since initExplicit doesn't carry it.
    uint32_t extra0 = 0;
    uint32_t extra1 = 0;

    switch (kind) {
        case (uint32_t)SnagKind::Obsession:    // 0x06: low4 = 0x64, high4 = 2
            extra0 = 0x64;
            extra1 = 2;
            break;

        case (uint32_t)SnagKind::Reluctance:   // 0x0f
            extra0 = 2;
            break;

        case (uint32_t)SnagKind::Doubt:        // 0x11
            extra0 = 0;   // consumedFlag = (extra0 == 1) -> false
            break;

        case (uint32_t)SnagKind::Doppelganger: // 0x19
            // copy the player's atk / def, overrides the rolled values
            // entirely. initExplicit applies the portrait UV.
            if (player) {
                rolledAtk = player->attack;
                rolledDef = player->defence;
            }
            break;

        case (uint32_t)SnagKind::Despair:      // 0x27
            extra0 = 1;   // consumedFlag = (extra0 == 1) -> true
            break;

        case (uint32_t)SnagKind::Infestation: { // 0x47
            // FUN_10005722c is a clamp(value, lo, hi). the binary computes
            // (totalTurnCount / 35) and clamps to [5..15], stored as def.
            int v = levelTurnCount / 35;
            if (v < 5)  { v = 5; }
            if (v > 15) { v = 15; }
            rolledDef = v;
            break;
        }

        case (uint32_t)SnagKind::Grief:        // 0x52
            if (rolledDef < 4) {
                rolledDef = 3;
            }
            break;

        case (uint32_t)SnagKind::Honesty:      // 0x6c, passive helper snag
            rolledAtk = 0;
            rolledDef = 0;
            rolledHp  = 1;
            break;

        case (uint32_t)SnagKind::Attrition: {  // 0x70
            // hp clamped to [5..10], atk forced >= 2.
            int hpClamped = rolledHp;
            if (hpClamped < 5)  { hpClamped = 5; }
            if (hpClamped > 10) { hpClamped = 10; }
            rolledHp = hpClamped;
            if (rolledAtk < 2) {
                rolledAtk = 2;
            }
            break;
        }

        default:
            break;
    }

    // ---- final stat clamping (binary: max(rolled, 0); max(rolledHp, 1)) ----
    // FUN_10003ccc8 clamps before the +1 transient; FUN_10003d614 does not
    // (its inputs are pre-clamped saved values), so the clamp stays here.
    if (rolledAtk < 0) { rolledAtk = 0; }
    if (rolledDef < 0) { rolledDef = 0; }
    if (rolledHp  < 2) { rolledHp  = 1; }    // max(rolledHp, 1)

    // ---- delegate the structural / sprite / display / stat-assign build ----
    initExplicit(kind, rolledHp, rolledAtk, rolledDef, extra0, extra1,
                 tileObj, player);

    // ---- Indecision (0x24) spawn-time self-decoration ----
    // binary FUN_10003ccc8's per-kind switch ends with FUN_100013870(tileObj,
    // 0, 0, 1) for Indecision, a kind-0 decoration stamped on the snag's own
    // parent tile at spawn. distinct from the per-turn Indecision effect in
    // applyEndOfTurnPipeline (which decorates a random rack tile each turn).
    if (kind == (uint32_t)SnagKind::Indecision && tileObj != nullptr) {
        tileObj->pushDecoration(0, 0, 1);
    }
}

// ----------------------------------------------------------------------------
// SnagContent::initExplicit, port of FUN_10003d614.
//
// builds the snag's full structural / sprite / display state from final stat
// values supplied by the caller, instead of computing them from player
// progress like init() does. init() delegates here after its own stat +
// extra computation; the saved-game restore path (FUN_100016b18 ->
// FUN_100013f04) calls it with the values captured at save time.
// ----------------------------------------------------------------------------
void SnagContent::initExplicit(uint32_t kind, int hp_, int atk_, int def_,
                               uint32_t extra0, uint32_t extra1,
                               TileObject* tileObj, PlayerSystem* player) {
    // ---- structural setup (identical to init's) ----
    initBase(nullptr);

    hp  = 0;
    atk = 0;
    def = 0;

    hpDisplay  = TileIcon();
    atkDisplay = TileIcon();
    defDisplay = TileIcon();

    hpTint.init();
    atkTint.init();
    defTint.init();

    atkDefSwapT = 1.0f;
    tileParent   = tileObj;
    targetTile   = nullptr;
    consumedFlag = false;

    type = (int)kind;

    // ---- baseQuad sprite UV + texture ----
    // FUN_10003d614 calls FUN_10003dedc (refreshBaseSprite) unconditionally,
    // then re-applies the Doppelganger portrait below. for Doppelganger the
    // refreshBaseSprite UV round-trip is overwritten by the portrait, so the
    // end state (tex 8 + portrait UV) matches init()'s inline path.
    refreshBaseSprite();

    // ---- per-kind extra values ----
    // mirrors FUN_10003d614's per-kind switch: Obsession / Reluctance write
    // the consumedFlag scratch (and Obsession also obsessionCount); Doubt /
    // Despair set consumedFlag = (extra0 == 1); Doppelganger re-pulls the
    // portrait. every other kind ignores extra0 / extra1.
    switch (kind) {
        case (uint32_t)SnagKind::Obsession:    // 0x06
            consumedFlag   = (uint8_t)extra0;
            obsessionCount = (int)extra1;
            break;

        case (uint32_t)SnagKind::Reluctance:   // 0x0f
            consumedFlag = (uint8_t)extra0;
            break;

        case (uint32_t)SnagKind::Doppelganger: // 0x19
            if (player) {
                setPortraitVisual(player->characterIndex, baseQuad);
            }
            break;

        case (uint32_t)SnagKind::Doubt:        // 0x11
        case (uint32_t)SnagKind::Despair:      // 0x27
            consumedFlag = (extra0 == 1) ? 1 : 0;
            break;

        default:
            break;
    }

    // ---- sub-Quad UV / size for the 3 stat displays (identical to init) ----
    hpDisplay.quad.setTexCoords(0.22949219f, 0.0f, 0.27832031f, 0.04492188f);
    hpDisplay.quad.setSize(0.078125f, 0.071875f);

    atkDisplay.quad.setTexCoords(0.18164063f, 0.0f, 0.22851563f, 0.04492188f);
    atkDisplay.quad.setSize(0.075f, 0.071875f);

    defDisplay.quad.setTexCoords(0.28027344f, 0.0f, 0.32324219f, 0.04492188f);
    defDisplay.quad.setSize(0.06875f, 0.071875f);

    // ---- ColorTint colors (identical to init) ----
    hpTint.setColor ((uint8_t)(HP_TINT_COLOR  >> 0), (uint8_t)(HP_TINT_COLOR  >> 8), (uint8_t)(HP_TINT_COLOR  >> 16));
    hpTint.setAlpha ((uint8_t)(HP_TINT_COLOR  >> 24));
    atkTint.setColor((uint8_t)(ATK_TINT_COLOR >> 0), (uint8_t)(ATK_TINT_COLOR >> 8), (uint8_t)(ATK_TINT_COLOR >> 16));
    atkTint.setAlpha((uint8_t)(ATK_TINT_COLOR >> 24));
    defTint.setColor((uint8_t)(DEF_TINT_COLOR >> 0), (uint8_t)(DEF_TINT_COLOR >> 8), (uint8_t)(DEF_TINT_COLOR >> 16));
    defTint.setAlpha((uint8_t)(DEF_TINT_COLOR >> 24));

    // ---- stat assignment with the +1 transient trick ----
    // binary sets each stat to value+1 so the setXDisplay change-detector
    // (current != new) fires and rebuilds the ColorTint digit display, then
    // setXDisplay corrects the stored value back to the real one.
    atk = atk_ + 1;
    def = def_ + 1;
    hp  = hp_  + 1;

    setAtkDisplay(atk_);
    setDefDisplay(def_);
    setHpDisplay (hp_);
}

// ----------------------------------------------------------------------------
// stat display setters, port of FUN_10003d3a4 / FUN_10003d468 / FUN_10003d530.
// each pushes a numeric value into the corresponding ColorTint (which renders
// it as digit Quads via the bitmap font) and snaps the tint to the right
// position relative to the sub-Quad. the binary also takes an optional
// "animation list" and "fade pos" pair we don't expose here; they're for
// stat-change pop-up animations during combat (Phase C work).
// ----------------------------------------------------------------------------

// FUN_10003d3a4, atk display.
void SnagContent::setAtkDisplay(int value, GameBoard* tweenBoard,
                                const float* posOffset) {
    int oldValue = atk;

    if (oldValue == value) {
        return;
    }

    atk = value;
    atkTint.setNumber(value, 0, 1);
    // atk tint sits at atkDisplay.posX + ATK_TINT_X_OFFSET (-0.003125),
    // same Y as atkDisplay (no Y offset).
    atkTint.setPosition(atkDisplay.quad.posX + ATK_TINT_X_OFFSET,
                        atkDisplay.quad.posY,
                        1);

    // optional floating "+N" / "-N" delta tween anchored on the atk tint.
    if (tweenBoard && posOffset) {
        float source[2] = {
            atkTint.posX + posOffset[0],
            atkTint.posY + posOffset[1],
        };
        tweenBoard->pushStatTween(/*textStyle*/ 0, value - oldValue, source,
                                  /*direction*/ 0);
    }
}

// FUN_10003d468, def display.
void SnagContent::setDefDisplay(int value, GameBoard* tweenBoard,
                                const float* posOffset) {
    int oldValue = def;

    if (oldValue == value) {
        return;
    }

    def = value;
    defTint.setNumber(value, 0, 1);
    // def tint sits at defDisplay.posX/posY + DEF_TINT_OFFSET (-0.001563) on
    // both axes (single offset reused for X and Y in the binary).
    defTint.setPosition(defDisplay.quad.posX + DEF_TINT_OFFSET,
                        defDisplay.quad.posY + DEF_TINT_OFFSET,
                        1);

    if (tweenBoard && posOffset) {
        float source[2] = {
            defTint.posX + posOffset[0],
            defTint.posY + posOffset[1],
        };
        tweenBoard->pushStatTween(/*textStyle*/ 0, value - oldValue, source,
                                  /*direction*/ 0);
    }
}

// FUN_10003d530, hp display.
void SnagContent::setHpDisplay(int value, GameBoard* tweenBoard,
                               const float* posOffset) {
    int oldValue = hp;

    if (oldValue == value) {
        return;
    }

    hp = value;
    hpTint.setNumber(value, 0, 1);
    // hp's Y offset depends on whether the value is single- or double-digit
    // (matches the binary's `csel` on `9 < hp`).
    float yOffset = (value > 9) ? HP_TINT_Y_HIGH : HP_TINT_Y_LOW;
    hpTint.setPosition(hpDisplay.quad.posX + HP_TINT_X_OFFSET,
                       hpDisplay.quad.posY - yOffset,
                       1);

    if (tweenBoard && posOffset) {
        float source[2] = {
            hpTint.posX + posOffset[0],
            hpTint.posY + posOffset[1],
        };
        tweenBoard->pushStatTween(/*textStyle*/ 0, value - oldValue, source,
                                  /*direction*/ 0);
    }
}

// ----------------------------------------------------------------------------
// FUN_10003dc38 (vtable[4]): set the snag's position. lays out the 3 stat-
// display sub-Quads (HP centered below, ATK bottom-left, DEF bottom-right)
// and pulls each ColorTint to its display's anchor with a small offset.
//
// constants from binary __TEXT,__const at 0x10005a32c..a344:
//   a32c = +0.0859    (HP display Y offset relative to snag posY)
//   a330 = -0.0672    (ATK display X offset)
//   a334 = +0.0469    (ATK + DEF display Y offset)
//   a338 = +0.0672    (DEF display X offset)
//   a33c = -0.0016    (HP tint X + DEF tint X+Y offset)
//   a340 = -0.0031    (ATK tint X offset)
//
// when called with `flag & 1` set, the binary skips the sub-Quad / tint
// layout step. only base position is written. we mirror that.
// ----------------------------------------------------------------------------
namespace {
constexpr float HP_DISP_DY  =  0.0859375f;  // DAT_10005a32c
constexpr float ATK_DISP_DX = -0.06718750f; // DAT_10005a330
constexpr float STAT_DISP_DY = 0.046875f;   // DAT_10005a334 (atk + def share Y)
constexpr float DEF_DISP_DX =  0.06718750f; // DAT_10005a338
constexpr float HP_TINT_DX  = -0.00156250f; // DAT_10005a33c (also DEF tint Y offset)
}

void SnagContent::setPosition(float x, float y, int skipLayout) {
    // base sprite at the requested position. the binary passes flag 0 to the
    // base call, so the main quad always snaps regardless of skipLayout.
    MovableActor::setPosition(x, y, 0);

    // spawn-in (skipLayout&1) leaves the stat displays where they are; they get
    // laid out on the settle/move pass once the flag clears.
    if ((skipLayout & 1) != 0) {
        return;
    }

    // 3 stat-display sub-Quads, positioned around the snag (from the unsnapped
    // x, y, matching the binary's read of param_2 rather than the snapped quad).
    hpDisplay.quad.posX  = x;
    hpDisplay.quad.posY  = y + HP_DISP_DY;
    atkDisplay.quad.posX = x + ATK_DISP_DX;
    atkDisplay.quad.posY = y + STAT_DISP_DY;
    defDisplay.quad.posX = x + DEF_DISP_DX;
    defDisplay.quad.posY = y + STAT_DISP_DY;

    // 3 ColorTints (digit displays), pulled to each sub-Quad's anchor.

    // HP tint: X = snag.posX + a33c (-0.00156); Y = HP_disp.posY - hpYOffset.
    float hpYOff = (hp > 9) ? HP_TINT_Y_HIGH : HP_TINT_Y_LOW;
    hpTint.setPosition(x + HP_TINT_DX, hpDisplay.quad.posY - hpYOff, 1);

    // ATK tint: X = atkDisplay.posX + a340 (-0.003); Y = atkDisplay.posY.
    atkTint.setPosition(atkDisplay.quad.posX + ATK_TINT_X_OFFSET,
                        atkDisplay.quad.posY,
                        1);

    // DEF tint: X+Y = defDisplay.posX/posY + a33c (-0.00156).
    defTint.setPosition(defDisplay.quad.posX + HP_TINT_DX,
                        defDisplay.quad.posY + HP_TINT_DX,
                        1);
}

// reconstructed from Ghidra FUN_10003da34 (vtable[3] override).
void SnagContent::update(float dt) {

    MovableActor::update(dt);

    // atkDefSwapT < 1 means a Mania ATK/DEF visual swap animation in flight.
    // cosine-eased lerp of both tints from swapped anchor to natural anchor,
    // with a sin-wave Y bounce shared between them so they arc across each
    // other.
    if (atkDefSwapT < 1.0f) {
        constexpr float SLIDE_PI       = 3.1415927f;   // DAT_10005a31c
        constexpr float SLIDE_Y_BOUNCE = 0.046875f;    // DAT_10005a320

        float t = atkDefSwapT + dt + dt;

        if (t > 1.0f) {
            t = 1.0f;
        }
        atkDefSwapT = t;

        float yBounce = (0.5f - std::cos((t + t) * SLIDE_PI) * 0.5f) *
                        SLIDE_Y_BOUNCE;
        float lerp    = 0.5f - std::cos(t * SLIDE_PI) * 0.5f;
        // pixel-snap only on the final-frame setPosition; keeps the
        // mid-animation lerps smooth.
        int   posMode = (atkDefSwapT < 1.0f) ? 0 : 1;

        // each tint slides from the opposite display's natural anchor (=
        // where swapAtkDefDisplay parked it) to its own display's natural
        // anchor.
        float atkSwapX    = defDisplay.quad.posX + DEF_TINT_OFFSET;
        float atkSwapY    = defDisplay.quad.posY + DEF_TINT_OFFSET;
        float atkNaturalX = atkDisplay.quad.posX + ATK_TINT_X_OFFSET;
        float atkNaturalY = atkDisplay.quad.posY;
        atkTint.setPosition(atkNaturalX * lerp + atkSwapX * (1.0f - lerp),
                            yBounce + atkNaturalY * lerp + atkSwapY * (1.0f - lerp),
                            posMode);

        float defSwapX    = atkDisplay.quad.posX + ATK_TINT_X_OFFSET;
        float defSwapY    = atkDisplay.quad.posY;
        float defNaturalX = defDisplay.quad.posX + DEF_TINT_OFFSET;
        float defNaturalY = defDisplay.quad.posY + DEF_TINT_OFFSET;
        defTint.setPosition(defNaturalX * lerp + defSwapX * (1.0f - lerp),
                            yBounce + defNaturalY * lerp + defSwapY * (1.0f - lerp),
                            posMode);
    }

    // chase reattachment. without this, every sendToward permanently
    // strands the snag and combat queries silently return null forever.
    if (targetTile != nullptr && moveT >= 1.0f && spawnT >= 1.0f) {
        TileObject* newParent = targetTile;
        TileObject* oldParent = tileParent;

        if (oldParent) {
            auto& vec = oldParent->trackedContent;
            vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
        }

        tileParent = newParent;
        targetTile = nullptr;

        newParent->attachSnag(this);
    }
}

// FUN_10003d9a8 (vtable[8]), full snag draw. mirrors the binary's exact
// 8-step sequence including the redundant inner FUN_100038b88 base-draw guard.
void SnagContent::drawFull(bool drawTints) {

    // outer guard from FUN_10003d9a8.
    if (!visible && fadeT >= 1.0f) {
        return;
    }

    bindTexture(static_cast<GLuint>(spriteTextureIdx));

    // FUN_100038b88 dispatch (MovableActor base draw): re-checks the same
    // guard then draws baseQuad. redundant given the outer guard but kept
    // for byte-for-byte parity with the binary's call sequence.
    if (visible || fadeT < 1.0f) {
        baseQuad.draw();
    }

    bindTexture(9);
    hpDisplay.quad.draw();
    atkDisplay.quad.draw();
    defDisplay.quad.draw();

    if (drawTints) {
        hpTint.draw();
        atkTint.draw();
        defTint.draw();
    }
}

// reconstructed from Ghidra FUN_10003dd80.
//
// propagates a single alpha byte across every visible sub-element. caller
// (onFade / onScaleOut / syncGlobalTileAlpha) drives the alpha curve;
// this just fans it out.
void SnagContent::setAlpha(uint8_t alpha) {
    baseQuad.setAlpha(alpha);
    hpDisplay.quad.setAlpha(alpha);
    atkDisplay.quad.setAlpha(alpha);
    defDisplay.quad.setAlpha(alpha);
    hpTint.setAlpha(alpha);
    atkTint.setAlpha(alpha);
    defTint.setAlpha(alpha);
}

// reconstructed from Ghidra FUN_10003de0c.
//
// fade-out alpha curve: alpha = (1 - fadeT) * 255. at fadeT=0 the snag
// is fully opaque; at fadeT=1 it's fully transparent.
void SnagContent::onFade(float fadeT) {
    constexpr float ALPHA_SCALE = 255.0f;  // DAT_10005a344

    int     raw = (int)((1.0f - fadeT) * ALPHA_SCALE);
    uint8_t a   = (uint8_t)(raw & 0xff);
    setAlpha(a);
}

// reconstructed from Ghidra FUN_10003de40.
//
// fade-in alpha curve (used on revive): alpha = scaleOutT * 255. as the
// scale-out timer advances 0 -> 1 the snag fades back in.
void SnagContent::onScaleOut(float scaleOutT) {
    constexpr float ALPHA_SCALE = 255.0f;  // DAT_10005a348

    int     raw = (int)(scaleOutT * ALPHA_SCALE);
    uint8_t a   = (uint8_t)(raw & 0xff);
    setAlpha(a);
}

// reconstructed from Ghidra FUN_10003de74. snag walks (move-queue style)
// toward `target`, severing visibility from its previous parent tile.
void SnagContent::sendToward(TileObject* target, bool reparent) {

    if (!target) {
        return;
    }

    // push (target.posXY, target.gridColRow) onto the move queue. base
    // class method (FUN_100038e10), resets moveT = 0.
    stepToward(&target->mainQuad.posX, &target->gridCol);

    targetTile = target;

    TileObject* oldParent = tileParent;

    // defensive null guard; the binary derefs oldParent unconditionally, but
    // it's non-null on every real call path (snag came from a live tile).
    if (oldParent) {
        oldParent->snagContent = nullptr;
    }

    if (reparent && oldParent) {
        // FUN_100013778: remove this snag from old parent's trackedContent.
        auto& oldVec = oldParent->trackedContent;
        oldVec.erase(std::remove(oldVec.begin(), oldVec.end(), this),
                     oldVec.end());

        // FUN_100012ad8: push back into target's trackedContent if absent.
        auto& tgtVec = target->trackedContent;

        if (std::find(tgtVec.begin(), tgtVec.end(), this) == tgtVec.end()) {
            tgtVec.push_back(this);
        }
    }
}

// reconstructed from Ghidra FUN_10003ddec. clamped HP-loss helper.
void SnagContent::deductHpClamped(int amount, GameBoard* tweenBoard,
                                  const float* posOffset) {

    if (amount <= 0) {
        return;
    }

    int loss = (amount <= hp) ? amount : hp;
    setHpDisplay(hp - loss, tweenBoard, posOffset);
}

// reconstructed from Ghidra FUN_10003e078. one-line tier lookup.
int SnagContent::tier() const {
    return SNAG_TABLE[type].tier;
}

// reconstructed from Ghidra FUN_10003e24c. visual ATK/DEF swap.
void SnagContent::swapAtkDefDisplay() {
    constexpr float ATK_TINT_SWAP_OFFSET = -0.0015625f;  // DAT_10005a34c (atk on both axes)
    constexpr float DEF_TINT_SWAP_X      = -0.003125f;   // DAT_10005a350 (def X only)

    int oldAtk = atk;

    // step 1: re-render each tint with the swapped stat number. setAtkDisplay
    // / setDefDisplay each early-out when value matches existing; binary
    // matches that behavior so when atk == def the tints don't redraw.
    setAtkDisplay(def);
    setDefDisplay(oldAtk);

    // step 2: swap each tint to the other display's home position so the
    // slide animation in update() runs from the opposite side. atkTint goes
    // to defDisplay's pos + DAT_10005a34c on both axes; defTint goes to
    // atkDisplay's posX + DAT_10005a350 with raw atkDisplay.posY (no Y offset).
    atkTint.setPosition(defDisplay.quad.posX + ATK_TINT_SWAP_OFFSET,
                        defDisplay.quad.posY + ATK_TINT_SWAP_OFFSET,
                        1);
    defTint.setPosition(atkDisplay.quad.posX + DEF_TINT_SWAP_X,
                        atkDisplay.quad.posY,
                        1);

    // step 3: start the slide animation timer (consumed in vtable[3] update).
    atkDefSwapT = 0.0f;
}

// reconstructed from Ghidra FUN_10003e2f8. per-snag-type combat scaling.
//   posDelta (param_5) = positive when the player has the position advantage;
//   defDelta (param_6) = the snag's incoming def loss (input value).
//
// note on SPA_X (= baseItemSpecialAbilityValue from PlayerSystem): SPA_2 is
// the "ATK piercing" stat (subtracted from atk delta), SPA_9 / SPA_10 are
// the type-1 piercing pair, etc.
void SnagContent::resolveCombatDelta(PlayerSystem* player,
                                     uint32_t* atkInOut,
                                     uint32_t* defInOut,
                                     int posDelta, uint32_t defDelta) {

    if (type == 0x12) {
        // Shred of Doubt ("Doesn't lose {A} or {D} when attacking"): the early
        // return skips the atk/def scaling other snags get, so it keeps full
        // stats. this is the effect, not a gap (binary returns early for 0x12).
        return;
    }

    if (type == 1) {
        // Snag (kind=1, the start tile / generic): full pierce + halve path.
        if (posDelta > 0) {
            *atkInOut = (*atkInOut * 2 + 2) / 3;
        }

        *defInOut = *defInOut >> 1;

        uint32_t pierceA = (uint32_t)player->baseItemSpecialAbilityValue(9);

        if ((int)pierceA > (int)*atkInOut) {
            pierceA = *atkInOut;
        }

        *atkInOut -= pierceA;

        uint32_t halvedDef = *defInOut;
        uint32_t pierceD   = (uint32_t)player->baseItemSpecialAbilityValue(10);

        if ((int)pierceD > (int)halvedDef) {
            pierceD = halvedDef;
        }

        *defInOut = halvedDef - pierceD;
        return;
    }

    if (type == 0x20) {
        // Stubbornness: atk damped if posDelta>0; def clamped to >= 0.
        if (posDelta > 0) {
            *atkInOut = (*atkInOut * 2 + 2) / 3;
        }

        if ((int)defDelta < 1) {
            defDelta = 0;
        }

        *defInOut = defDelta;
        return;
    }

    if (type == 7) {
        // Judgement: def halved only, atk untouched. the binary jumps straight
        // to the def>>1 at 0x10003e3a0, skipping the posDelta/atk damp.
        *defInOut = *defInOut >> 1;
        return;
    }

    // default: atk damped if posDelta>0; def halved.
    if (posDelta > 0) {
        *atkInOut = (*atkInOut * 2 + 2) / 3;
    }

    *defInOut = *defInOut >> 1;
}

// FUN_10003dedc.
void SnagContent::refreshBaseSprite(Quad* target, int* outTexIdx) {
    Quad& tgt   = target    ? *target    : baseQuad;
    int&  texId = outTexIdx ? *outTexIdx : spriteTextureIdx;

    if (type == (int)SnagKind::Doppelganger) {
        texId = 8;

        // copy UV (vert0 + vert3 corners) and size off this snag's own
        // baseQuad. when target is baseQuad this is a round-trip that
        // re-aligns TR / BL vertices and rebuilds vertex positions as
        // +/-0.5 * size around the origin. when target is a panel's icon
        // Quad it propagates the snag's current portrait UV onto the icon.
        tgt.setTexCoords(baseQuad.vertices[0].u, baseQuad.vertices[0].v,
                         baseQuad.vertices[3].u, baseQuad.vertices[3].v);
        tgt.setSize(baseQuad.width, baseQuad.height);
        return;
    }

    // non-Doppelganger path (= FUN_10003e0a8).
    texId = 10;
    const SnagInfo& info = snagInfo((uint32_t)type);
    applySpriteUV(tgt, info.spriteU, info.spriteV, info.spriteW, info.spriteH);

    if (type == (int)SnagKind::Change) {
        tgt.addVertexOffset(-0.0046875f, -0.003125f);
    }
}

// FUN_10003df70.
void SnagContent::mergeFrom(SnagContent& incoming) {
    setAtkDisplay(atk + incoming.atk);
    setDefDisplay(def + incoming.def);
    setHpDisplay (hp  + incoming.hp);

    // generic snag (type 1) absorbing a special-typed snag: the survivor
    // takes on the absorbed snag's identity. for any other type pairing
    // the merge is stats-only.
    if (type == 1 && incoming.type != 1) {
        type = incoming.type;
        refreshBaseSprite();

        // 8-byte copy covers consumedFlag + obsessionCount, the
        // two per-type scratch fields. matches the binary's combined store.
        consumedFlag   = incoming.consumedFlag;
        obsessionCount = incoming.obsessionCount;

        // Doppelganger absorption: copy the absorbed snag's baseQuad
        // visual state (vertices + posX/Y + width/height + scale + rotation)
        // so the survivor is visually indistinguishable from the original.
        if (incoming.type == (int)SnagKind::Doppelganger) {
            baseQuad.copyVisualState(incoming.baseQuad);
        }
    }
}
