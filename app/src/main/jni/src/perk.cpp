#include "perk.h"
#include "perk_table.h"
#include "renderer.h"   // bindTexture
#include <GLES/gl.h>

// ---- helpers ----
//
// FUN_100014d84, pixel-rect on the 1024px atlas: sets a Quad's UV (via
// setTexCoords) and its display size (= atlasW * scale / 640, atlasH * scale / 640).
// reproduced inline here to avoid pulling in a separate helper header.
static void setPixelRect(Quad& q, float atlasX, float atlasY,
                         float atlasW, float atlasH, float displayScale) {
    constexpr float ATLAS_TO_UV     = 1.0f / 1024.0f;   // DAT_100059ebc
    constexpr float PIXEL_TO_SCREEN = 1.0f / 640.0f;    // DAT_100059ec0

    q.setTexCoords(
        atlasX           * ATLAS_TO_UV,  atlasY           * ATLAS_TO_UV,
        (atlasX + atlasW) * ATLAS_TO_UV, (atlasY + atlasH) * ATLAS_TO_UV);
    q.setSize(atlasW * displayScale * PIXEL_TO_SCREEN,
              atlasH * displayScale * PIXEL_TO_SCREEN);
}

// FUN_100041840, Perk::init.
//
// constructs the per-instance visual state for a (perkType, perkLevel) pair.
// pulls per-type data from PERK_TYPE_TABLE and per-category icon UVs from
// CATEGORY_ICON_UV.
//
// perkLevel clamping (matches binary mask 0x111 = bits 0, 4, 8):
//   - perkTypes 0, 4, 8 (stat-bumps): skip clamping entirely.
//   - others: clamp to [1, levelCount] so callers can pass "currentLevel+1"
//     even when already at max.
void Perk::init(int t, int l) {
    perkType  = t;
    perkLevel = l;
    icon1     = TileIcon();
    icon2     = TileIcon();
    tint.init();

    // perkLevel clamp, skipped for stat-bump perkTypes 0, 4, 8.
    bool isStatBump = (t == 0) || (t == 4) || (t == 8);

    if (!isStatBump) {
        int levelCount = PERK_TYPE_TABLE[t].levelCount;

        if (l < 1) {
            perkLevel = 1;
        } else if (l > levelCount) {
            perkLevel = levelCount;
        }
    }

    const PerkTypeEntry& entry = PERK_TYPE_TABLE[t];
    const CategoryIconUV& catUV = CATEGORY_ICON_UV[entry.category];

    // DAT constants from FUN_100041840.
    constexpr float PX_TO_UNIT  = 1.0f / 640.0f;   // DAT_10005a460 = 640.0
    constexpr float ICON1_POS   = 0.0046875f;      // DAT_10005a464
    constexpr float TINT_X_OFF  = -0.028125f;      // DAT_10005a468
    constexpr float TINT_Y_OFF  = -0.025f;         // DAT_10005a46c

    // warnLine2 = per-perk glyph. set UV + size from atlas pixel coords, then
    // nudge posX/posY by the per-perk sub-pixel offsets divided by 640.
    setPixelRect(icon2.quad,
                 (float)entry.icon2_atlasX, (float)entry.icon2_atlasY,
                 (float)entry.icon2_atlasW, (float)entry.icon2_atlasH,
                 1.0f);
    icon2.quad.posX = (float)entry.icon2_offsetXpx * PX_TO_UNIT;
    icon2.quad.posY = (float)entry.icon2_offsetYpx * PX_TO_UNIT;
    icon2.quad.snapToPixelGrid();

    // warnLine1 = per-category glyph. set UV + size from category table, then
    // pin posX/posY to the fixed 3-px offset (DAT_10005a464).
    setPixelRect(icon1.quad,
                 (float)catUV.atlasX, (float)catUV.atlasY,
                 (float)catUV.atlasW, (float)catUV.atlasH,
                 1.0f);
    icon1.quad.posX = ICON1_POS;
    icon1.quad.posY = ICON1_POS;

    // tint centered horizontally on warnLine1, with the small TINT_X_OFF /
    // TINT_Y_OFF nudges added.
    float tintX = icon1.quad.width  * 0.5f + ICON1_POS + TINT_X_OFF;
    float tintY = icon1.quad.height * 0.5f + ICON1_POS + TINT_Y_OFF;
    tint.setPosition(tintX, tintY, 1);
    tint.setNumber(perkLevel, 0, 1);
}

// FUN_10004218c, getName.
const char* Perk::getName() const {
    return PERK_TYPE_TABLE[perkType].name;
}

// FUN_1000421a4, getDescriptionLine.
//
// hardcoded stat-bump strings, plus a table lookup for normal perks.
// branch order matches the binary: check perkType 8 / 4 / 0 first, then
// fall through to the table.
const char* Perk::getDescriptionLine(int lineIdx) const {

    if (perkType == 8) {

        if (lineIdx != 0) {
            return "";
        }

        return "Increases your {H} stat by 1";
    }

    if (perkType == 4) {

        if (lineIdx != 0) {
            return "";
        }

        return "Increases your {D} stat by 1";
    }

    if (perkType == 0) {

        if (lineIdx != 0) {
            return "";
        }

        return "Increases your {A} stat by 1";
    }

    // normal perk: only line 0 and 1 are valid. line 2+ returns "".
    if (lineIdx > 1) {
        return "";
    }

    const PerkTypeEntry& entry = PERK_TYPE_TABLE[perkType];
    int levelIdx = perkLevel - 1;

    if (levelIdx < 0 || levelIdx >= entry.levelCount) {
        return "";
    }

    const PerkLevelEntry& lvl = entry.levels[levelIdx];

    return (lineIdx == 0) ? lvl.line0 : lvl.line1;
}

// FUN_1000420bc, Perk::drawAt.
//
// alpha-set on all three sub-objects, then bind texture 12 (the perk-icon
// atlas), push matrix + translate to (posX, posY), draw warnLine2 (per-perk
// glyph), draw warnLine1 (per-category frame with transparent center on top),
// draw tint (level number), pop matrix.
void Perk::drawAt(float posX, float posY, uint8_t alpha) {
    icon2.quad.setAlpha(alpha);
    icon1.quad.setAlpha(alpha);
    tint.setAlpha(alpha);

    bindTexture(12);

    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    icon2.quad.draw();
    icon1.quad.draw();
    tint.draw();

    glPopMatrix();
}
