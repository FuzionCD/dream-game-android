#include "achievement_banner.h"

#include "achievement_table.h"
#include "game.h"
#include "renderer.h"         // bindTexture, getVirtualHeight

#include <GLES/gl.h>
#include <algorithm>
#include <cmath>

// constants pulled from the binary's .rodata.
namespace {

constexpr float kAscendDuration = 0.8f;          // DAT_10005a550
constexpr float kAscendPhaseEnd = 0.6f;          // DAT_10005a554
constexpr float kAscendOffset   = -0.6f;         // DAT_10005a558
constexpr float kAscendDivisor  = 0.4f;          // DAT_10005a55c
constexpr float kPi             = 3.1415927f;    // DAT_10005a560
constexpr float kPosYAmpA       = -0.03125f;     // DAT_10005a564
constexpr float kShiftYA        = -0.015625f;    // DAT_10005a568
constexpr float kShiftYB        = 0.015625f;     // DAT_10005a56c
constexpr float kShiftYC        = 0.03125f;      // DAT_10005a570
constexpr float kDecoOffset     = -0.4f;         // DAT_10005a574
constexpr float kDecoDivisor    = 0.6f;          // DAT_10005a578
constexpr float kDecoBiasX      = 0.0953125f;    // DAT_10005a57c
constexpr float kScreenRefW     = 640.0f;        // DAT_10005a580
constexpr float kDecoBiasY      = -0.0625f;      // DAT_10005a584
constexpr float kGlowAlphaLo    = 0.15f;         // DAT_10005a588
constexpr float kGlowAlphaHi    = 0.35f;         // DAT_10005a58c
constexpr float kGlowAlphaScale = 240.0f;        // DAT_10005a590
constexpr float kTapArmTimer    = 0.3f;          // DAT_10005a594
constexpr float kGlowGlyphW     = 64.0f;         // DAT_10005a59c
constexpr float kGlowSheetU     = 960.0f;        // DAT_10005a5a4
constexpr float kGlowSheetV     = 474.0f;        // DAT_10005a5a8
constexpr float kGlowPosA       = -32.0f;        // DAT_10005a5ac
constexpr float kGlowPosB       = 32.0f;         // DAT_10005a5b0
constexpr float kGlowPosNorm    = 640.0f;        // DAT_10005a5b4
constexpr float kGlowPosScale   = 0.707107f;     // DAT_10005a5b8 (= sqrt(2)/2)
constexpr float kDividerPosX    = 0.146875f;     // DAT_10005a5bc
constexpr float kDividerPosY    = 0.0421875f;    // DAT_10005a5c0
constexpr float kPanelExtraX    = 0.0046875f;    // DAT_10005a5c4
constexpr float kDecoBiasArr[2] = { -1.0f, 1.0f };  // UNK_10005a5c8 / 5cc
constexpr float kDecoSpeedArr[2] = { 1.0f, 1.3f };  // UNK_10005a5d0 / 5d4

// FUN_1000570d4, clamp(v, lo, hi).
inline float clampRange(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

// FUN_10004efec, ctor tail. the Quad / Label / TextItem default ctors already
// ran via the C++ chain; this adds chrome glyphs, deco UV/size, and text
// styling that the binary writes explicitly after the sub-ctors.
void AchievementBanner::init() {
    // 0. POD zero-init. value-init already zeroed these, but keep the explicit
    //    writes for fidelity.
    posX         = 0.0f;
    posY         = 0.0f;
    baseY        = 0.0f;
    drawCursor   = 0;
    dismissTimer = 0.0f;
    dismissed    = 0;

    // 1. icon Quad position. open() rewrites only the icon's UV and size, not
    //    its posX/Y, so the icon stays at this inset relative to banner.posX/Y.
    //    panelGlow.posX/Y in FUN_10004fc60 anchors off icon.posX/Y too.
    icon.posX = 62.0f / 640.0f;          // = 0.096875
    icon.posY = 62.0f / 640.0f;

    // 2. Label::init on both chrome Labels (FUN_10004c014). load-bearing:
    //    Label::init sets scale = 1024.0, which addGlyph divides by to convert
    //    pixel UVs to 0..1 UVs. skip it and scale stays 0, so every UV becomes
    //    infinity, every glyph position becomes NaN, and the chrome falls apart.
    panelFrame.init();
    dividerStrip.init();

    // 3. TextItem POD init for title + description (FUN_10002fa08, run after the
    //    C++ ctor). sets scaleX/Y = 1.0, rgba = 0xFFFFFFFF, spaceMultiplier = 2.0
    //    (load-bearing: without it all spaces collapse).
    title.init();
    description.init();

    // 4. panel frame 9-slice chrome: 3 glyphs (left corner 94x126, 1-px
    //    stretch, right corner 56x126). UVs are pixel coords on ui1.png.
    {
        const GlyphOffset zero = { 0.0f, 0.0f };
        float uvOriginPx[2];
        float uvSizePx[2];

        uvOriginPx[0] = 672.0f; uvOriginPx[1] = 484.0f;
        uvSizePx[0]   = 94.0f;  uvSizePx[1]   = 126.0f;
        panelFrame.addGlyph(-1.0f, uvOriginPx, uvSizePx, 2, zero);

        uvOriginPx[0] = 766.0f; uvOriginPx[1] = 484.0f;
        uvSizePx[0]   = 1.0f;   uvSizePx[1]   = 126.0f;
        panelFrame.addGlyph(-1.0f, uvOriginPx, uvSizePx, 1, zero);

        uvOriginPx[0] = 767.0f; uvOriginPx[1] = 484.0f;
        uvSizePx[0]   = 56.0f;  uvSizePx[1]   = 126.0f;
        panelFrame.addGlyph(-1.0f, uvOriginPx, uvSizePx, 2, zero);
    }

    // 5. divider strip: 2 glyphs (corner 27x33, 1-px stretch).
    {
        const GlyphOffset zero = { 0.0f, 0.0f };
        float uvOriginPx[2];
        float uvSizePx[2];

        uvOriginPx[0] = 872.0f; uvOriginPx[1] = 957.0f;
        uvSizePx[0]   = 27.0f;  uvSizePx[1]   = 33.0f;
        dividerStrip.addGlyph(-1.0f, uvOriginPx, uvSizePx, 2, zero);

        uvOriginPx[0] = 899.0f; uvOriginPx[1] = 957.0f;
        uvSizePx[0]   = 1.0f;   uvSizePx[1]   = 33.0f;
        dividerStrip.addGlyph(-1.0f, uvOriginPx, uvSizePx, 1, zero);
    }

    // 6. sparkle deco quads: both share UV (0.5830078, 0.5185547) .. (0.6552734,
    //    0.5908203) on the 1024-stride atlas + size 0.115625 x 0.115625. deco[0]
    //    additionally has its top-row + bottom-row U coords swapped via
    //    FUN_100008430 (horizontal mirror).
    for (int i = 0; i < 2; ++i) {
        deco[i].setTexCoords(0.5830078f, 0.5185547f,
                             0.6552734f, 0.5908203f);
        deco[i].setSize(0.115625f, 0.115625f);

        if (i == 0) {
            // FUN_100008430. swaps vertices[0].u <-> vertices[1].u and
            // vertices[2].u <-> vertices[3].u for a horizontal-mirror flip.
            std::swap(deco[i].vertices[0].u, deco[i].vertices[1].u);
            std::swap(deco[i].vertices[2].u, deco[i].vertices[3].u);
        }
    }

    // 7. title text styling.
    title.scaleX = 0.078f;
    title.scaleY = 0.078f;
    title.posX   = 0.1875f;
    title.posY   = 0.08125f;
    title.rgba   = 0xff003232u;          // alpha=ff, BGR = 32, 32, 00
    title.applyColor();

    // 8. description text styling.
    description.scaleX = 0.067f;
    description.scaleY = 0.067f;
    description.posX   = 0.16875f;
    description.posY   = 0.1375f;
    description.rgba   = 0xff146464u;
    description.applyColor();

    // 9. park banner idle. matches FUN_10004f3ac.
    resetTimer = -1.0f;
}

// FUN_10004f3ac. park the banner idle.
void AchievementBanner::reset() {
    resetTimer = -1.0f;
}

// ---------------------------------------------------------------------------
// draw-pool helpers.
// ---------------------------------------------------------------------------

// FUN_100050220, append one zero-initialized DrawNode to the pool.
static void bannerGrowDrawPool(AchievementBanner* b) {
    b->drawPool.emplace_back();
}

// FUN_100050090, append-or-overwrite the (drawCursor + 1)-th node with a
// transformed copy of `src`. lazily grows the pool.
static void bannerAddQuadGlyph(AchievementBanner* b, int textureId,
                               const Quad& src,
                               float offX, float offY,
                               float scaleX, float scaleY) {
    b->drawCursor += 1;

    if (b->drawCursor > b->drawPool.size()) {
        bannerGrowDrawPool(b);
    }

    auto it = b->drawPool.begin();
    std::advance(it, b->drawCursor - 1);

    AchievementBannerDrawNode& node = *it;
    node.textureId = textureId;

    node.source.copyVisualState(src);
    node.source.posX   = src.posX   * scaleX + offX;
    node.source.posY   = src.posY   * scaleY + offY;
    node.source.scaleX = node.source.scaleX * scaleX;
    node.source.scaleY = node.source.scaleY * scaleY;

    node.foreground.copyVisualState(node.source);
    node.outline.copyVisualState(node.source);
}

// FUN_10004ffc8, append every glyph of `src` Label with identity transform.
static void bannerAddLabelGlyphs(AchievementBanner* b, int textureId,
                                 Label& src) {
    for (const TileIcon& g : src.glyphs) {
        bannerAddQuadGlyph(b, textureId, g.quad, 0.0f, 0.0f, 1.0f, 1.0f);
    }
}

// FUN_1000501a0, append every glyph of `src` TextItem, transformed by
// (src.posX/Y, src.scaleX/Y), with texId = glyphTablePtr->textureIndex + 1.
static void bannerAddTextItemGlyphs(AchievementBanner* b, TextItem& src) {
    int textureId = src.glyphTablePtr->textureIndex + 1;

    for (int64_t i = 0; i < src.glyphCount; ++i) {
        bannerAddQuadGlyph(b, textureId, src.glyphVec[i].quad,
                           src.posX, src.posY, src.scaleX, src.scaleY);
    }
}

// ---------------------------------------------------------------------------
// FUN_10004fc60, animate the panel-glow Quad each frame. param_1 selects
// the cosine ease point, param_2 is the wipe width factor (0..1).
// ---------------------------------------------------------------------------
static void animatePanelGlow(AchievementBanner* b,
                             float param1, float param2) {
    float invP2  = 1.0f - param2;
    float w64    = invP2 * kGlowGlyphW;
    float ease   = 0.5f - std::cos(param1 * kPi) * 0.5f;

    float uPx = w64 * ease + (1.0f - ease) * 0.0f + kGlowSheetU;
    float vPx = kGlowSheetV;
    float wPx = param2 * kGlowGlyphW;
    float hPx = kGlowGlyphW;

    constexpr float kTexInv    = 1.0f / 1024.0f;
    constexpr float kScreenInv = 1.0f / 640.0f;

    b->panelGlow.setTexCoords(uPx           * kTexInv,
                              vPx           * kTexInv,
                              (uPx + wPx)   * kTexInv,
                              (vPx + hPx)   * kTexInv);
    b->panelGlow.setSize(wPx * kScreenInv, hPx * kScreenInv);
    b->panelGlow.rotation = 45.0f;

    float diag = ((invP2 * kGlowPosB * ease + invP2 * kGlowPosA * (1.0f - ease))
                  / kGlowPosNorm) * kGlowPosScale;
    b->panelGlow.posX = b->icon.posX + diag;
    b->panelGlow.posY = b->icon.posY + diag;
}

// FUN_10004f920, per-frame chrome reveal. walks the draw-pool, vertically
// clipping each node's foreground bottom edge to the reveal anchorY, then
// extending the outline below the foreground over `halfExtent`. outline
// vertex alphas taper from origAlpha at anchorY to 0 at anchorY + halfExtent.
static void layoutPanelChrome(AchievementBanner* b,
                              float anchorY, float halfExtent) {
    if (b->drawPool.empty() || b->drawCursor == 0) {
        return;
    }

    constexpr float k255 = 255.0f;   // DAT_10005a598
    int64_t remaining = static_cast<int64_t>(b->drawCursor);

    for (AchievementBannerDrawNode& node : b->drawPool) {
        if (remaining-- == 0) {
            break;
        }

        const Quad& src = node.source;
        Quad&       fg  = node.foreground;
        Quad&       ol  = node.outline;

        float topEdge   = src.posY + src.vertices[0].y * src.scaleY;
        float vExtent   = src.scaleY * src.height;
        float uvVExtent = src.vertices[2].v - src.vertices[0].v;

        // ---- foreground bottom edge clipped to [topEdge, topEdge + fgFill]
        //      where fgFill = how much of the glyph sits above anchorY.
        float fgFill      = clampRange(anchorY - topEdge, 0.0f, vExtent);
        float fgFillDy    = (src.scaleY != 0.0f) ? (fgFill / src.scaleY) : 0.0f;
        float fgFillDv    = (vExtent   > 0.0f)   ? (uvVExtent * (fgFill / vExtent)) : 0.0f;

        fg.vertices[2].x = src.vertices[0].x;
        fg.vertices[2].y = src.vertices[0].y + fgFillDy;
        fg.vertices[3].x = src.vertices[1].x;
        fg.vertices[3].y = src.vertices[1].y + fgFillDy;
        fg.vertices[2].u = src.vertices[0].u;
        fg.vertices[2].v = src.vertices[0].v + fgFillDv;
        fg.vertices[3].u = src.vertices[1].u;
        fg.vertices[3].v = src.vertices[1].v + fgFillDv;

        // ---- outline top edge = foreground bottom edge; outline bottom edge
        //      extends a further olFill into the sweep window [anchorY ..
        //      anchorY + halfExtent], clipped to whatever vertical remains.
        float olCeiling = (vExtent - fgFill) < halfExtent
                          ? (vExtent - fgFill) : halfExtent;
        float olFill    = clampRange((anchorY + halfExtent) - topEdge, 0.0f, olCeiling);
        float olFillDy  = (src.scaleY != 0.0f) ? (olFill / src.scaleY) : 0.0f;
        float olFillDv  = (vExtent   > 0.0f)   ? (uvVExtent * (olFill / vExtent)) : 0.0f;

        ol.vertices[0].x = fg.vertices[2].x;
        ol.vertices[0].y = fg.vertices[2].y;
        ol.vertices[1].x = fg.vertices[3].x;
        ol.vertices[1].y = fg.vertices[3].y;
        ol.vertices[2].x = fg.vertices[2].x;
        ol.vertices[2].y = fg.vertices[2].y + olFillDy;
        ol.vertices[3].x = fg.vertices[3].x;
        ol.vertices[3].y = fg.vertices[3].y + olFillDy;
        ol.vertices[0].u = fg.vertices[2].u;
        ol.vertices[0].v = fg.vertices[2].v;
        ol.vertices[1].u = fg.vertices[3].u;
        ol.vertices[1].v = fg.vertices[3].v;
        ol.vertices[2].u = fg.vertices[2].u;
        ol.vertices[2].v = fg.vertices[2].v + olFillDv;
        ol.vertices[3].u = fg.vertices[3].u;
        ol.vertices[3].v = fg.vertices[3].v + olFillDv;

        // ---- per-vertex outline alpha taper:
        //      outAlpha = (origAlpha / 255.0) * float((int)((1 - frac) * 255.0))
        //      the inner int-cast quantizes to 1/255 steps.
        auto bake = [&](uint8_t srcAlpha, float frac) -> uint8_t {
            float fade  = clampRange(frac, 0.0f, 1.0f);
            int   step  = static_cast<int>((1.0f - fade) * k255);
            float ratio = (float)srcAlpha / k255;
            return static_cast<uint8_t>(static_cast<int>(ratio * (float)step));
        };

        // fade fractions for the outline's top + bottom vertex rows. for the
        // bottom, the sample point sits at topEdge + (foreground extent) +
        // (outline extent), i.e. the actual bottom Y of the outline.
        float topFade = (halfExtent != 0.0f) ? (topEdge - anchorY) / halfExtent : 0.0f;
        float botFade = (halfExtent != 0.0f)
                        ? ((topEdge + fgFill + olFill) - anchorY) / halfExtent
                        : 0.0f;

        uint8_t a0 = static_cast<uint8_t>((src.vertices[0].color >> 24) & 0xFFu);
        uint8_t a1 = static_cast<uint8_t>((src.vertices[1].color >> 24) & 0xFFu);
        uint8_t a2 = static_cast<uint8_t>((src.vertices[2].color >> 24) & 0xFFu);
        uint8_t a3 = static_cast<uint8_t>((src.vertices[3].color >> 24) & 0xFFu);

        ol.vertices[0].color = (ol.vertices[0].color & 0x00FFFFFFu) | (uint32_t(bake(a0, topFade)) << 24);
        ol.vertices[1].color = (ol.vertices[1].color & 0x00FFFFFFu) | (uint32_t(bake(a1, topFade)) << 24);
        ol.vertices[2].color = (ol.vertices[2].color & 0x00FFFFFFu) | (uint32_t(bake(a2, botFade)) << 24);
        ol.vertices[3].color = (ol.vertices[3].color & 0x00FFFFFFu) | (uint32_t(bake(a3, botFade)) << 24);
    }
}

// ---------------------------------------------------------------------------
// FUN_10004fd7c, open(idx). populate icon / title / desc from
// ACHIEVEMENT_TABLE, rebuild draw-pool, prime via update(0).
// ---------------------------------------------------------------------------
void AchievementBanner::open(uint32_t idx) {
    if (idx >= 50) {
        return;
    }

    const AchievementInfo& info = ACHIEVEMENT_TABLE[idx];
    Game*                  game = getGame();

    // 1. icon Quad UV + size from ACHIEVEMENT_TABLE entry (84x84 px on
    //    1024-stride atlas; size in screen-norm via /640).
    constexpr float kTexInv    = 1.0f / 1024.0f;
    constexpr float kScreenInv = 1.0f / 640.0f;
    float ix = static_cast<float>(info.iconX);
    float iy = static_cast<float>(info.iconY);

    icon.setTexCoords( ix                                * kTexInv,
                       iy                                * kTexInv,
                      (ix + ACHIEVEMENT_ICON_SIZE_PX)    * kTexInv,
                      (iy + ACHIEVEMENT_ICON_SIZE_PX)    * kTexInv);
    icon.setSize(ACHIEVEMENT_ICON_SIZE_PX * kScreenInv,
                 ACHIEVEMENT_ICON_SIZE_PX * kScreenInv);

    // 2. title + description from ACHIEVEMENT_TABLE; both bound to the dialog
    //    font (= game+0x1220 = bmfontTablePtr(1)).
    title.glyphTablePtr       = game->bmfontTablePtr(1);
    title.setString(info.title, -1);
    description.glyphTablePtr = game->bmfontTablePtr(1);
    description.setString(info.description, -1);

    // 3. panelFrame.setSize. width = max(titleW+0.25, descW+0.234375);
    //    height = paired-return heightTotal from measureGlyphRun (Ghidra
    //    drops s1, but the asm preserves it across the bl into setSize).
    float titleW = title.renderedWidth       * title.scaleX       + 0.25f;
    float descW  = description.renderedWidth * description.scaleX + 0.234375f;
    float panelW = (descW <= titleW) ? titleW : descW;
    float panelH = panelFrame.measureGlyphRun(-1, -1).heightTotal;
    panelFrame.setSize(panelW, panelH);

    // 4. dividerStrip. width = panelFrame.getWidth() - 0.1875; height = paired
    //    return heightTotal (again s1 preserved across the bl into setSize).
    float stripW = panelFrame.getWidth() - 0.1875f;
    float stripH = dividerStrip.measureGlyphRun(-1, -1).heightTotal;
    dividerStrip.setSize(stripW, stripH);
    dividerStrip.setPosition(kDividerPosX, kDividerPosY);

    // glyph[0] uniform alpha 100; glyph[1] left-100 / right-0 horizontal fade.
    // quad vertex order is TL/TR/BL/BR, so verts 0+2 are the left column = 100,
    // verts 1+3 the right column = 0.
    if (!dividerStrip.glyphs.empty()) {
        dividerStrip.glyphs[0].quad.setAlpha(100);

        if (dividerStrip.glyphs.size() >= 2) {
            Quad& g1 = dividerStrip.glyphs[1].quad;
            g1.vertices[0].color = (g1.vertices[0].color & 0x00FFFFFFu) | (uint32_t(100) << 24);
            g1.vertices[1].color = (g1.vertices[1].color & 0x00FFFFFFu) | (uint32_t(0)   << 24);
            g1.vertices[2].color = (g1.vertices[2].color & 0x00FFFFFFu) | (uint32_t(100) << 24);
            g1.vertices[3].color = (g1.vertices[3].color & 0x00FFFFFFu) | (uint32_t(0)   << 24);
        }
    }

    // 5. posX centers the panel; posY anchored at virtualHeight - 0.4375.
    posX  = (0.5f - panelFrame.getWidth() * 0.5f) + kPanelExtraX;
    float anchorY = Renderer::getVirtualHeight() - 0.4375f;
    posY  = anchorY;
    baseY = anchorY;

    // 6. rebuild draw-pool. order matches binary:
    //      panelFrame -> dividerStrip -> icon -> title -> description.
    drawCursor = 0;
    bannerAddLabelGlyphs(this, 9, panelFrame);
    bannerAddLabelGlyphs(this, 9, dividerStrip);
    bannerAddQuadGlyph(this, 0xD, icon, 0.0f, 0.0f, 1.0f, 1.0f);
    bannerAddTextItemGlyphs(this, title);
    bannerAddTextItemGlyphs(this, description);

    // 7. start the timers + fire unlock sound (audioState slot 0x51).
    resetTimer   = 0.0f;
    dismissTimer = 3.0f;
    dismissed    = 0;
    game->soundQueue.trigger(0x51);

    // 8. priming tick.
    update(0.0f);
}

// ---------------------------------------------------------------------------
// FUN_10004f490, per-frame anim tick. resetTimer < 0 = idle, early-out.
// ---------------------------------------------------------------------------
void AchievementBanner::update(float dt) {
    if (resetTimer < 0.0f) {
        return;
    }

    // 1. advance resetTimer (asc = dt/0.8, desc = -2*dt) and clamp to [0..1].
    float step = dismissed ? (-2.0f * dt) : (dt / kAscendDuration);
    resetTimer = clampRange(resetTimer + step, 0.0f, 1.0f);

    // 2. main panel posY. branch at resetTimer >= kAscendPhaseEnd (= 0.6).
    //    paired-return heightTotal from panelFrame.measureGlyphRun is used
    //    in the ascending sub-branch.
    float panelHeight = panelFrame.measureGlyphRun(-1, -1).heightTotal;

    if (resetTimer >= kAscendPhaseEnd) {
        float arg  = ((resetTimer + kAscendOffset) / kAscendDivisor) * kPi;
        float ease = 0.5f - std::cos(arg) * 0.5f;
        posY = baseY + ease * 0.0f + (1.0f - ease) * kPosYAmpA;
    } else {
        float arg  = (resetTimer / kAscendPhaseEnd) * kPi;
        float ease = 0.5f - std::cos(arg) * 0.5f;
        posY = baseY + ease * kPosYAmpA + (1.0f - ease) * panelHeight;
    }

    // 3. panel-frame chrome reveal (per-glyph vertex animation). FUN_10004f920
    //    consumes panelHeight + a secondary cosine on (2t) to derive the
    //    sweep parameters.
    float chromeT2     = clampRange(resetTimer / kAscendPhaseEnd, 0.0f, 1.0f);
    float chromeEase1  = 0.5f - std::cos(chromeT2 * kPi) * 0.5f;
    float chromeEase2  = 0.5f - std::cos((2.0f * chromeT2) * kPi) * 0.5f;
    float anchorY      = panelHeight * chromeEase1 + (1.0f - chromeEase1) * kShiftYA;
    float sweepHE      = chromeEase2 * kShiftYC + (1.0f - chromeEase2) * kShiftYB;
    layoutPanelChrome(this, anchorY, sweepHE);

    // 4. 2 sparkle deco Quads (loop runs lVar8 = 0 then 0xd8, exits at 0x1b0).
    for (int i = 0; i < 2; ++i) {
        float decoArg = clampRange((resetTimer + kDecoOffset) / kDecoDivisor,
                                0.0f, 1.0f);
        uint8_t alpha = (decoArg > 0.0f) ? 0xFFu : 0x00u;
        deco[i].setAlpha(alpha);

        size_t bIdx     = (i == 0) ? 1u : 0u;
        float  bias     = kDecoBiasArr[bIdx];
        float  speedArg = clampRange(kDecoSpeedArr[bIdx] * decoArg, 0.0f, 1.0f);
        float  ease     = 0.5f - std::cos(speedArg * kPi) * 0.5f;
        float  inv      = 1.0f - ease;
        deco[i].posX    = bias * kShiftYC + kDecoBiasX
                          + ((bias * -20.0f) / kScreenRefW) * inv;
        deco[i].posY    = inv * kDecoBiasY + 0.1875f;
    }

    // 5. ascend-only: panel-glow drive + glow-quad alpha lerp.
    if (!dismissed) {
        float wipeT     = clampRange((resetTimer + kAscendOffset) / kAscendDivisor,
                                  0.0f, 1.0f);
        float ease2     = 0.5f - std::cos((2.0f * wipeT) * kPi) * 0.5f;
        float alphaArg  = ease2 * kGlowAlphaHi + (1.0f - ease2) * kGlowAlphaLo;
        animatePanelGlow(this, wipeT, alphaArg);

        uint8_t aByte = static_cast<uint8_t>(
            static_cast<int>(ease2 * kGlowAlphaScale + (1.0f - ease2) * 0.0f));
        panelGlow.setAlpha(aByte);
    }

    // 6. tap-to-dismiss-early + auto-dismiss countdown. only runs once the
    //    banner has fully ascended (resetTimer >= 1.0). dismissTimer counts
    //    down from 3.0; while it's > 0.3 (= ambient phase), a panel-bbox
    //    touch snaps it down to 0.3 (= short pre-dismiss flash). once it
    //    reaches 0, dismissed = 1 + fire close sound.
    if (resetTimer >= 1.0f) {
        Game* g = getGame();

        if (dismissTimer > kTapArmTimer && g && g->inputState() == 1) {
            float touchX = g->touchX() - posX;
            float touchY = g->touchY() - posY;

            if (panelFrame.contains(touchX, touchY)) {
                dismissTimer = kTapArmTimer;
            }
        }

        dismissTimer -= dt;

        if (dismissTimer <= 0.0f) {
            dismissed = 1;

            if (g) {
                g->soundQueue.trigger(4);
            }
        }
    }

    // 7. settled at 0 + descending -> park idle.
    if (dismissed && resetTimer <= 0.0f) {
        resetTimer = -1.0f;
    }
}

// ---------------------------------------------------------------------------
// FUN_10004f3b8, draw. resetTimer < 0 = idle early-out.
// ---------------------------------------------------------------------------
void AchievementBanner::draw() {
    if (resetTimer < 0.0f) {
        return;
    }

    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);
    bindTexture(9);

    // 2 sparkle deco Quads (loop in binary writes lVar1 = 0, 0xd8, exits at
    // 0x1b0; two iterations).
    deco[0].draw();
    deco[1].draw();

    // walk draw-pool, drawing each node's foreground + outline pair under
    // its associated texture.
    int64_t remaining = static_cast<int64_t>(drawCursor);

    for (AchievementBannerDrawNode& node : drawPool) {
        if (remaining-- == 0) {
            break;
        }

        bindTexture(static_cast<uint32_t>(node.textureId));
        node.foreground.draw();
        node.outline.draw();
    }

    bindTexture(9);
    panelGlow.draw();

    glPopMatrix();
}
