#pragma once

#include "color_tint.h"
#include "quad.h"
#include "title_menu.h"   // for TileIcon
#include <cstdint>
#include <string>

class PlayerSystem;

// reconstructed from Ghidra:
//   constructor:  FUN_10003040c
//   push to PS:   FUN_1000567fc (writes item into PlayerSystem.baseItems[type])
//
// each Item is a 0x610-byte object allocated via operator_new and pushed
// into one of PlayerSystem's 3 fixed slots (slot index = `type`, 0..2) or its
// dynamic vector. types in the binary's level-loader path:
//   type 0 (atk=1, def=0, hp=0): Attack item
//   type 1 (atk=0, def=0, hp=1): HP item
//   type 2 (atk=0, def=1, hp=0): Defence item
//
// special-ability slots: each Item carries up to 2 SpecialAbility entries
// (rolled randomly at construction). each entry is a (abilityType, abilityVal)
// pair plus an icon Quad. count is gated by perk 0x16; magnitude scales with
// PlayerSystem.currentLevel. the 18-entry pool of names + value ranges lives
// in SPECIAL_ABILITY_POOL (item_data_table.h, mirroring DAT_100079da0).

struct SpecialAbility {
    int32_t abilityType;    // +0x00 (RNG-selected stat type in the binary;
                            //        index into the DAT_100079da0 pool)
    int32_t abilityVal;     // +0x04 (RNG-selected magnitude shown in "+%d ...")
    Quad    iconQuad;       // +0x08..+0xDF (0xD8; owns anim rect at +0xD0;
                            //               UV (0.519, 0.200)..(0.558, 0.239),
                            //               size 0.0625; shared icon set by
                            //               FUN_100032a74 for both slots)
};
static_assert(sizeof(SpecialAbility) == 0xE0, "SpecialAbility must be exactly 0xE0 bytes");

class Item {
public:
    // FUN_10003040c. ctor:
    //   1. set type/atk/def/hp, zero header.
    //   2. init 4 icon TileIcons + 3 ColorTints + 2 SpecialAbility iconQuads.
    //   3. roll subType (cosmetic flavor) by `rngInt(0, 55|44, 2)`, re-rolling
    //      if it hits `filterMask`.
    //   4. roll 0..2 SpecialAbilities (count gated by perk 0x16 chance) from
    //      the type's 6-entry sub-pool in special_ability_table.h. each rolled
    //      ability gets a magnitude scaled by parent.currentLevel.
    //   5. sprintf the rolled magnitude into the SpecialAbility name template
    //      and assign into descLine[i].
    //   6. set up icon/tint UVs via the post-init setup helper.
    // skipped vs binary: subType cosmetic name pool (DAT_10007e220); items
    // get the "Strange Object" fallback from getName until that pool lands.
    void init(PlayerSystem* parent, int type, int atk, int def, int hp,
              uint32_t filterMask);

    // FUN_100032fc4, explicit-values ctor used by the saved-game restore
    // path (FUN_100056c90). unlike init() it does not roll subType / cosmetic
    // name / SpecialAbilities; it takes the exact values captured at save
    // time (the Item analog of SnagContent::initExplicit). the caller unpacks
    // the saved StatBlockSnapshot into these scalars, so Item stays
    // independent of the save module. descLine[i] is rebuilt by sprintf'ing
    // each non-zero ability's magnitude into its name template, exactly as
    // init() does for rolled abilities.
    void initExplicit(int type, int subType, int cosmeticNameIdx,
                      int atk, int def, int hp,
                      int spa0Type, int spa0Val, int spa1Type, int spa1Val);

    // FUN_100032d84. copy ctor, clones header (type/subType/atk/def/hp/
    // cosmeticNameIdx), descLine[0..1], and abilities[].abilityType/Val. the
    // iconQuads inside abilities[] get their 0xD0 trailing bytes memcpy'd
    // (everything past the vtable pointer), then postInitVisuals re-derives
    // all of the icon/tint UVs from type/subType. used in two places:
    //   1. ItemChoicePanel::open clones each held item into the slot for
    //      side-by-side display vs the offered candidate.
    //   2. the commit path clones the selected candidate into PlayerSystem's
    //      held-items slot.
    // doesn't take a parent; postInitVisuals doesn't reference one.
    Item(const Item& src);

    // not needed yet; cleanup is handled by std::string's own dtor + Quad's
    // trivial destruct (Quad has a virtual default dtor).
    Item() = default;

    // FUN_100033440. binary looks up
    //   pool[type][subType][cosmeticNameIdx]
    // and falls through to "Strange Object" when the inner vector is empty.
    // our port returns the fallback until the cosmetic name pool is ported.
    const char* getName() const;

    // FUN_100033484. returns descLine[lineIdx]. line 2+ -> empty string.
    const char* getDescriptionLine(int lineIdx) const;

    // FUN_1000332d4, visual draw. icon1 on tex 11 (items1.png); icon2 / 3 /
    // 4 + tint1 / 2 / 3 on tex 9 (ui1.png), each pair gated on the matching
    // atk / def / hp being non-zero (only stat badges with a value get shown).
    // when `showAbilities != 0`, the two SpecialAbility iconQuads are
    // additionally drawn under a glPushMatrix + glTranslatef to the caller-
    // supplied anchor. abilities skip when the slot's abilityType == 0.
    void drawAt(int showAbilities, const float* anchor);

    // FUN_1000333a8. propagates alpha onto every sub-Quad and tint:
    // icon1..4, tint1..3, abilities[0..1].iconQuad, unconditionally
    // (no abilityType != 0 check; the binary writes the alpha byte to
    // every iconQuad, and the draw path is what gates ability rendering).
    // used by ItemChoicePanel::setAlpha so a single panel-wide fade
    // cascades through all the Item's sub-visuals.
    void setAlpha(uint8_t alpha);

    // FUN_100032a74. sets up the 4 icon TileIcons + 3 ColorTints + 2
    // SpecialAbility iconQuads with their per-stat UVs, positions, and tint
    // colors. icon1 uses the per-(type, subType) atlas rect from ICON_RECTS;
    // icon2/3/4 and tints use shared values across all items. called at the
    // tail of init(); also re-callable when stats change so the displayed
    // numbers update.
    void postInitVisuals();

    // --- byte-exact struct fields ---

    // ---- header (0x000..0x047) ----
    int32_t type;             // +0x000  (0=ATK, 1=HP, 2=DEF; confirms via per-type
                              //          stat-block init in starter Items at level-load)
    int32_t subType;          // +0x004  (cosmetic flavor; init -1, rolled in init via
                              //          rngInt(0, 55, 2) for type 0/1; rngInt(0, 44, 2)
                              //          for type 2)
    int32_t atk;              // +0x008
    int32_t def;              // +0x00C
    int32_t hp;               // +0x010  HP-capacity boost; sums into PlayerSystem.baseHP
    int32_t cosmeticNameIdx;  // +0x014  index into the subType's cosmetic name vector
                              //         (DAT_10007e220 + type*0x18 -> vector<vector<char*>>).
                              //         binary's Item::getName looks up
                              //         pool[type][subType][cosmeticNameIdx]; returns
                              //         "Strange Object" when out of range.

    // two description lines, one per rolled SpecialAbility. Item::init
    // sprintf's the SpecialAbility name template (with magnitude) into a
    // scratch buffer then assigns into these. binary's getDescriptionLine
    // (FUN_100033484) returns descLine[lineIdx] (line 2+ = "").
    std::string descLine[2];  // +0x018..+0x047 (libc++ std::string = 0x18 each)

    // ---- 4 icon TileIcons (0xD8 each) ----
    // the binary inits these via thunk_FUN_100007d78 at offsets +0x48, +0x120,
    // +0x1F8, +0x2D0. UVs/sizes/positions are set by gameplay code (item-spawn
    // helpers we haven't ported); they stay at default 0..1 UV until then.
    TileIcon icon1;           // +0x048..+0x11F
    TileIcon icon2;           // +0x120..+0x1F7
    TileIcon icon3;           // +0x1F8..+0x2CF
    TileIcon icon4;           // +0x2D0..+0x3A7

    // ---- 3 ColorTints (0x38 each) ----
    ColorTint tint1;          // +0x3A8..+0x3DF
    ColorTint tint2;          // +0x3E0..+0x417
    ColorTint tint3;          // +0x418..+0x44F

    // ---- 2 SpecialAbility slots (0xE0 each) ----
    // populated at construction by an RNG draw from the per-type pool at
    // DAT_100079da0 (entries 1..6 for type 0, 7..12 for type 1, 13..18 for
    // type 2). number rolled depends on item tier (0..2 special abilities).
    SpecialAbility abilities[2];  // +0x450..+0x60F
};
static_assert(sizeof(Item) == 0x610, "Item must be exactly 0x610 bytes");
