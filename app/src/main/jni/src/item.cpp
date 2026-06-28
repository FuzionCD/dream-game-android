#include "item.h"
#include "item_table.h"
#include "player_system.h"
#include "random.h"
#include "renderer.h"     // bindTexture
#include <GLES/gl.h>
#include <cstdio>         // sprintf

// pixel-rect helper: atlas pixel coords on 1024x1024 -> UV + display size.
// matches the binary's FUN_100014d84.
static void setPixelRect(Quad& q, float atlasX, float atlasY,
                         float atlasW, float atlasH, float displayScale) {
    constexpr float ATLAS_TO_UV     = 1.0f / 1024.0f;
    constexpr float PIXEL_TO_SCREEN = 1.0f / 640.0f;

    q.setTexCoords(
        atlasX           * ATLAS_TO_UV,  atlasY           * ATLAS_TO_UV,
        (atlasX + atlasW) * ATLAS_TO_UV, (atlasY + atlasH) * ATLAS_TO_UV);
    q.setSize(atlasW * displayScale * PIXEL_TO_SCREEN,
              atlasH * displayScale * PIXEL_TO_SCREEN);
}

// FUN_100032a74. builds the item's visual: warnLine1 (the per-type silhouette),
// warnLine2/3/4 (ATK/DEF/HP number backdrops), 3 ColorTints (the stat numbers),
// and the 2 SpecialAbility icon quads.
void Item::postInitVisuals() {

    // warnLine1: the per-type silhouette. FUN_10003326c looks up its atlas rect
    // for (type, subType) in ICON_RECTS, then nudges it 1 px right and 5 px up
    // (at the 640 reference width).
    constexpr float ICON1_X_NUDGE = 1.0f / 640.0f;   //  0.0015625
    constexpr float ICON1_Y_NUDGE = -1.0f / 128.0f;  // -0.0078125
    const IconRect& rect = ICON_RECTS[type][subType];
    setPixelRect(icon1.quad,
                 (float)rect.atlasX, (float)rect.atlasY,
                 (float)rect.atlasW, (float)rect.atlasH,
                 1.0f);
    icon1.quad.posX = ICON1_X_NUDGE;
    icon1.quad.posY = ICON1_Y_NUDGE;
    icon1.quad.snapToPixelGrid();

    // warnLine2/3/4: the ATK / DEF / HP number-panel backdrops. each samples a
    // different per-stat atlas region (atlasW 48/44/50, atlasY 0, atlasH 46),
    // sampled 1:1 with screen px on the 640 reference width.
    setPixelRect(icon2.quad, 186.0f, 0.0f, 48.0f, 46.0f, 1.0f);
    icon2.quad.posX = -0.0625f;
    icon2.quad.posY =  0.078125f;

    setPixelRect(icon3.quad, 287.0f, 0.0f, 44.0f, 46.0f, 1.0f);
    icon3.quad.posX = 0.0625f;
    icon3.quad.posY = 0.078125f;

    setPixelRect(icon4.quad, 235.0f, 0.0f, 50.0f, 46.0f, 1.0f);
    icon4.quad.posX = 0.0f;
    icon4.quad.posY = 0.078125f;

    // the 3 stat-number tints. colors are 0xAARRGGBB but setColor takes (r,g,b):
    // atk 0xC8FFFF (yellow-white), def 0xFFE6C8 (off-white pink), hp 0x64FFFF
    // (cyan-ish). position offsets are all multiples of 1/640 (1 px on the 640
    // reference): atk shifts 2 px in X (its "+" prefix eats a glyph slot), def
    // and hp inset 1 px on both axes, and hp's Y nudge grows to 2 px for 2-digit
    // values (10..99) to keep the number centered on its backdrop.
    constexpr float TINT_INSET_2PX = -0.003125f;    // DAT_10005a168 = -2/640
    constexpr float TINT_INSET_1PX = -0.0015625f;   // DAT_10005a16c = -1/640
    constexpr float HP_Y_NUDGE_1DIGIT = 0.0015625f; // DAT_10005a178 = 1/640
    constexpr float HP_Y_NUDGE_2DIGIT = 0.003125f;  // DAT_10005a17c = 2/640

    tint1.setColor(0xFF, 0xFF, 0xC8);
    tint1.setNumber(atk, 0, 1);
    tint1.setPosition(icon2.quad.posX + TINT_INSET_2PX, icon2.quad.posY, 1);

    tint2.setColor(0xC8, 0xE6, 0xFF);
    tint2.setNumber(def, 0, 1);
    tint2.setPosition(icon3.quad.posX + TINT_INSET_1PX,
                      icon3.quad.posY + TINT_INSET_1PX, 1);

    tint3.setColor(0xFF, 0xFF, 0x64);
    tint3.setNumber(hp, 0, 1);
    const float hpYNudge = (hp >= 10) ? HP_Y_NUDGE_2DIGIT : HP_Y_NUDGE_1DIGIT;
    tint3.setPosition(icon4.quad.posX + TINT_INSET_1PX,
                      icon4.quad.posY - hpYNudge, 1);

    // the 2 SpecialAbility icon quads, sharing one atlas rect (531, 205, 40x40).
    // each slot drops 30 px below the previous, starting 50 px above the slot
    // origin (just under the stat-number row).
    constexpr float SPECIAL_ABILITY_POS_X       = -49.0f / 640.0f;  // -0.0765625, 0xbd9ccccd
    constexpr float SPECIAL_ABILITY_Y_STRIDE_PX = 30.0f;
    constexpr float SPECIAL_ABILITY_Y_BASE_PX   = -50.0f;
    constexpr float PIXEL_TO_SCREEN_REF         = 640.0f;     // DAT_10005a170

    for (int i = 0; i < 2; ++i) {
        Quad& q = abilities[i].iconQuad;
        setPixelRect(q, 531.0f, 205.0f, 40.0f, 40.0f, 1.0f);
        q.posX = SPECIAL_ABILITY_POS_X;
        q.posY = ((float)i * SPECIAL_ABILITY_Y_STRIDE_PX
                  + SPECIAL_ABILITY_Y_BASE_PX)
                 / PIXEL_TO_SCREEN_REF;
    }
}

// FUN_100032d84, Item copy ctor. copies the header scalars, default-constructs
// the sub-objects, copies the two descLine strings, copies each ability's
// (type, val) plus its iconQuad bytes, then re-derives every visual via
// postInitVisuals. the iconQuad copy below is promptly overwritten by
// postInitVisuals, so it's pure clone-pattern ceremony, but we keep it for
// fidelity.
Item::Item(const Item& src) {
    // (1)..(2) header copy
    type            = src.type;
    subType         = src.subType;
    atk             = src.atk;
    def             = src.def;
    hp              = src.hp;
    cosmeticNameIdx = src.cosmeticNameIdx;

    // C++ already default-constructed warnLine1..4 and the iconQuads. the tints
    // still need an explicit init() to reset their scalars (color to white
    // opaque, scale to 1.0) to a known state before postInitVisuals layers
    // setColor/setNumber on top, matching the binary's explicit ColorTint::init.
    tint1.init();
    tint2.init();
    tint3.init();
    // (4) skipped; our cosmetic name pool is constexpr at compile time.

    // (5) copy descLine via std::string operator=.
    descLine[0] = src.descLine[0];
    descLine[1] = src.descLine[1];

    // (6) abilities[i] header + iconQuad copy.
    for (int i = 0; i < 2; ++i) {
        abilities[i].abilityType = src.abilities[i].abilityType;
        abilities[i].abilityVal  = src.abilities[i].abilityVal;
        // copy the icon quad's data members; the implicit copy-assignment
        // leaves the vtable pointer intact, the same effect as the binary's
        // 0xD0-byte copy past the vtable.
        abilities[i].iconQuad = src.abilities[i].iconQuad;
    }

    // (7) re-derive visual setup.
    postInitVisuals();
}

// FUN_10003040c, Item::init (full port).
//
// per-type sub-type roll uses the rngInt range:
//   type 0 (ATK), type 1 (HP): rngInt(0, 55, 2)
//   type 2 (DEF):              rngInt(0, 44, 2)
//
// SpecialAbility count is gated by perk 0x16 ("Chance of N item abilities"):
//   perk level 1: 50% chance of 1 ability, 0% of 2
//   perk level 2: 25% chance of 2 abilities, else 50% chance of 1
//   perk level 3: 75% chance of 2 abilities, else 1 guaranteed
//   (else: 0 abilities)
//
// each rolled ability's magnitude scales with parent.currentLevel via
//   magnitude = rngInt(lo, lo + (currentLevel / 10) * hi_step, 2)
void Item::init(PlayerSystem* parent, int t, int a, int d, int h,
                uint32_t filterMask) {
    // init() rolls the cosmetic / SpecialAbility values from RNG + player
    // progress, then delegates the structural / sub-object / description /
    // visual build to initExplicit() (= the binary's FUN_100032fc4). this
    // mirrors the binary's two-function split (FUN_10003040c rolls,
    // FUN_100032fc4 builds) and keeps the build logic in one place. the
    // sub-object setup consumes no RNG, so moving it into initExplicit
    // (after the rolls) leaves the stream-2 consumption order unchanged.

    // subType roll: loop the RNG until it lands a value that isn't -1 or the
    // filterMask. type 0/1 use [0, 55], type 2 uses [0, 44]. the -1 guard is the
    // binary's; our bounded rngInt can't actually return -1, but we mirror it.
    const int subTypeMax = (t == 2) ? 44 : 55;
    int rolledSubType;

    do {
        rolledSubType = rngInt(0, subTypeMax, 2);
    } while (rolledSubType == -1 || (uint32_t)rolledSubType == filterMask);

    // ---- cosmeticNameIdx roll ----
    // binary picks a name slot within the subType's inner vector. if the
    // inner vector is empty, cosmeticNameIdx stays 0 and getName falls
    // through to "Strange Object". every entry in the current data has
    // count >= 4, so the fallback never actually fires.
    int rolledNameIdx = 0;

    if (rolledSubType >= 0 && rolledSubType < COSMETIC_NAMES_COUNT[t]) {
        int nameCount = COSMETIC_NAMES[t][rolledSubType].count;

        if (nameCount > 0) {
            rolledNameIdx = rngInt(0, nameCount - 1, 2);
        }
    }

    // ---- SpecialAbility count roll (gated by perk 0x16) ----
    int abilityPerkLvl = parent ? parent->perkLevel(0x16) : 0;
    int abilityCount   = 0;

    if (abilityPerkLvl > 0) {
        int roll = rngInt(0, 100, 2);

        if (abilityPerkLvl == 3) {
            abilityCount = (roll < 0x4B) ? 2 : 1;
        } else if (abilityPerkLvl == 2) {

            if (roll < 0x19) {
                abilityCount = 2;
            } else if (roll < 0x4B) {
                abilityCount = 1;
            }
        } else if (abilityPerkLvl == 1) {

            if (roll < 0x32) {
                abilityCount = 1;
            }
        }
    }

    // ---- SpecialAbility (type, magnitude) rolls ----
    int spaType[2] = { 0, 0 };
    int spaVal[2]  = { 0, 0 };

    if (abilityCount > 0) {
        // build a 6-entry pool of available SpecialAbility indices for this
        // type; pop without replacement so the two rolled abilities are distinct.
        const SpecialAbilitySubPool& subPool = SPECIAL_ABILITY_SUBPOOL[t];
        int available[6];

        for (int i = 0; i < subPool.count; ++i) {
            available[i] = subPool.firstIdx + i;
        }

        int availableCount = subPool.count;
        const int currentLevel = parent ? parent->currentLevel : 0;

        for (int i = 0; i < abilityCount && availableCount > 0; ++i) {
            // the binary's pool is a sorted std::set (FUN_100033dac inserts,
            // FUN_100033814 picks the k-th smallest remaining via in-order walk
            // and erases it preserving order). shift-down on removal keeps
            // available[] sorted-contiguous so rngInt(0, availableCount-1, 2)
            // selects the same k-th smallest element as the set walk.
            int idx = rngInt(0, availableCount - 1, 2);
            int abilityType = available[idx];

            for (int j = idx; j < availableCount - 1; ++j) {
                available[j] = available[j + 1];
            }

            availableCount -= 1;

            const SpecialAbilityEntry& entry = SPECIAL_ABILITY_POOL[abilityType];
            int magnitude = rngInt(
                entry.lo,
                entry.lo + (currentLevel / 10) * entry.hiStep,
                2);

            spaType[i] = abilityType;
            spaVal[i]  = magnitude;
        }
    }

    // ---- delegate the structural / description / visual build ----
    // initExplicit owns the header writes, sub-object init, ability-slot
    // assignment, the per-ability name sprintf (using each spaVal magnitude),
    // the "No special abilities" default, and postInitVisuals.
    initExplicit(t, rolledSubType, rolledNameIdx, a, d, h,
                 spaType[0], spaVal[0], spaType[1], spaVal[1]);
}

// FUN_100032fc4, Item::initExplicit (explicit-values builder). the
// saved-game-restore analog of init(): same structural + visual build, but the
// header / SpecialAbility values come from the caller instead of RNG rolls (the
// Item analog of SnagContent::initExplicit). the caller unpacks the saved
// StatBlockSnapshot into these scalars, so Item stays independent of the save
// module.
void Item::initExplicit(int t, int subType_, int cosmeticNameIdx_,
                        int a, int d, int h,
                        int spa0Type, int spa0Val, int spa1Type, int spa1Val) {
    // ---- header ----
    type            = t;
    subType         = subType_;
    atk             = a;
    def             = d;
    hp              = h;
    cosmeticNameIdx = cosmeticNameIdx_;

    descLine[0].clear();
    descLine[1].clear();

    // ---- sub-objects: 4 TileIcons, 3 ColorTints, 2 SpecialAbility iconQuads ----
    icon1 = TileIcon();
    icon2 = TileIcon();
    icon3 = TileIcon();
    icon4 = TileIcon();
    tint1.init();
    tint2.init();
    tint3.init();

    for (int i = 0; i < 2; ++i) {
        abilities[i].iconQuad = Quad();
    }

    // ---- SpecialAbility slots (supplied, not rolled) ----
    abilities[0].abilityType = spa0Type;
    abilities[0].abilityVal  = spa0Val;
    abilities[1].abilityType = spa1Type;
    abilities[1].abilityVal  = spa1Val;

    // ---- description lines ----
    // binary: when the first ability slot is empty, descLine[0] gets the
    // default string; otherwise each non-zero ability's magnitude is
    // sprintf'd into its name template (same path as init()).
    if (abilities[0].abilityType == 0) {
        descLine[0] = "No special abilities";
    } else {

        for (int i = 0; i < 2; ++i) {

            if (abilities[i].abilityType != 0) {
                const SpecialAbilityEntry& entry =
                    SPECIAL_ABILITY_POOL[abilities[i].abilityType];
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), entry.name,
                              abilities[i].abilityVal);
                descLine[i] = buffer;
            }
        }
    }

    // ---- visual setup ----
    postInitVisuals();
}

// FUN_100033440, Item::getName. looks up
// COSMETIC_NAMES[type][subType].names[cosmeticNameIdx], falling through to
// "Strange Object" on an empty row. every current entry has count >= 4, so the
// fallback is purely defensive.
const char* Item::getName() const {

    if (type < 0 || type > 2) {
        return "Strange Object";
    }

    if (subType < 0 || subType >= COSMETIC_NAMES_COUNT[type]) {
        return "Strange Object";
    }

    const CosmeticNameRow& row = COSMETIC_NAMES[type][subType];

    if (row.count == 0) {
        return "Strange Object";
    }

    int idx = cosmeticNameIdx;

    if (idx < 0 || idx >= row.count) {
        idx = 0;
    }

    return row.names[idx];
}

// FUN_100033484, Item::getDescriptionLine.
const char* Item::getDescriptionLine(int lineIdx) const {

    if (lineIdx < 0 || lineIdx > 1) {
        return "";
    }

    return descLine[lineIdx].c_str();
}

// FUN_1000333a8, Item::setAlpha. pushes the alpha byte onto every sub-Quad and
// tint, with no abilityType gate: the binary writes both ability iconQuads even
// when the slot is unused. drawAt gates what actually renders; setAlpha just
// keeps the stored alpha coherent.
void Item::setAlpha(uint8_t alpha) {
    icon1.quad.setAlpha(alpha);
    icon2.quad.setAlpha(alpha);
    icon3.quad.setAlpha(alpha);
    icon4.quad.setAlpha(alpha);
    tint1.setAlpha(alpha);
    tint2.setAlpha(alpha);
    tint3.setAlpha(alpha);
    abilities[0].iconQuad.setAlpha(alpha);
    abilities[1].iconQuad.setAlpha(alpha);
}

// Item::drawAt (FUN_1000332d4). draws the main icon (icon1) under tex 11, then
// the three stat-badge icon+tint pairs (icon2/icon4/icon3 with tint1/tint3/
// tint2) under tex 9, each gated on a non-zero stat so a fresh atk=def=hp=0
// item shows only its silhouette. when showAbilities is set, the two
// SpecialAbility quads draw under a matrix translated to `anchor`, each gated on
// abilityType != 0. alpha isn't touched here; setAlpha is called separately on
// panel transitions.
void Item::drawAt(int showAbilities, const float* anchor) {
    bindTexture(11);
    icon1.quad.draw();

    bindTexture(9);

    if (atk != 0) {
        icon2.quad.draw();
        tint1.draw();
    }

    if (hp != 0) {
        icon4.quad.draw();
        tint3.draw();
    }

    if (def != 0) {
        icon3.quad.draw();
        tint2.draw();
    }

    if (showAbilities != 0) {
        glPushMatrix();
        glTranslatef(anchor[0], anchor[1], 0.0f);

        for (int i = 0; i < 2; ++i) {

            if (abilities[i].abilityType != 0) {
                abilities[i].iconQuad.draw();
            }
        }

        glPopMatrix();
    }
}
