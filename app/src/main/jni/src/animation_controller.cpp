#include "animation_controller.h"

#include "dream_snippet_table.h"
#include "game.h"
#include "random.h"
#include "sound_queue.h"
#include "text_item.h"

#include <GLES/gl.h>
#include <cmath>

// constants extracted from binary (Dream_1.0.3, arm64 slice). verified via
// scripts/extract_phase3_data.py.
static constexpr float kInitLineStrideMul     = 0.078125f;   // DAT_10005a260
static constexpr float kInitTrack1RateMult    = 0.06f;       // DAT_10005a264
static constexpr float kInitTrack2RateMult    = 0.02f;       // DAT_10005a268
static constexpr float kBlendCosPi            = 3.14159274f; // DAT_10005a26c

static constexpr float kTrack1CosPi           = 3.14159274f; // DAT_10005a270
static constexpr float kTrack1AlphaMult       = 255.0f;      // DAT_10005a274
static constexpr float kTrack1SinMul          = 0.8f;        // DAT_10005a278
static constexpr float kTrack1ScaleDenom      = 0.587785f;   // DAT_10005a27c

// FUN_10003a808, eased per-glyph color blend.
//   progress    = 0..1 (advances with the track timer).
//   primary     = RGBA at progress = 0.
//   outline     = RGBA at progress = 1.
//   easeCos     = when nonzero (and easeDoubled = 0) applies a cosine ease
//                 to progress.
//   easeDoubled = when nonzero, doubles progress then applies a cosine ease,
//                 producing a U-curve over 0..1 (returns to primary at
//                 progress=1, not outline). track 1 body calls with (1, 1);
//                 init's per-glyph randomized blend calls with (0, 0).
static uint32_t blendRgba(float progress, uint32_t primary, uint32_t outline,
                          int easeCos, int easeDoubled) {

    if (easeDoubled != 0) {
        progress = progress + progress;
        const float c = std::cos(progress * kBlendCosPi);
        progress = 0.5f - c * 0.5f;
    } else if (easeCos != 0) {
        const float c = std::cos(progress * kBlendCosPi);
        progress = 0.5f - c * 0.5f;
    }

    const float invProgress = 1.0f - progress;

    auto pr = static_cast<uint8_t>((primary       ) & 0xFFu);
    auto pg = static_cast<uint8_t>((primary >> 8  ) & 0xFFu);
    auto pb = static_cast<uint8_t>((primary >> 16 ) & 0xFFu);
    auto pa = static_cast<uint8_t>((primary >> 24 ) & 0xFFu);

    auto or_ = static_cast<uint8_t>((outline       ) & 0xFFu);
    auto og = static_cast<uint8_t>((outline >> 8  ) & 0xFFu);
    auto ob = static_cast<uint8_t>((outline >> 16 ) & 0xFFu);
    auto oa = static_cast<uint8_t>((outline >> 24 ) & 0xFFu);

    uint32_t r = static_cast<uint32_t>(static_cast<int>(pr * invProgress)
                + static_cast<int>(or_ * progress)) & 0xFFu;
    uint32_t g = static_cast<uint32_t>(static_cast<int>(pg * invProgress)
                + static_cast<int>(og * progress)) & 0xFFu;
    uint32_t b = static_cast<uint32_t>(static_cast<int>(pb * invProgress)
                + static_cast<int>(ob * progress)) & 0xFFu;
    uint32_t a = static_cast<uint32_t>(static_cast<int>(pa * invProgress)
                + static_cast<int>(oa * progress)) & 0xFFu;

    return (a << 24) | (b << 16) | (g << 8) | r;
}

// FUN_100039b14, full init body.
void AnimationController::initFromLines(
        float fontScale,
        const std::vector<std::string>& lines,
        float anchorX, float anchorY,
        uint32_t primaryRgba,
        uint32_t outlineRgba) {

    // 1. zero core state: position, both track enables, both track timers.
    posX          = 0.0f;
    posY          = 0.0f;
    track1Enabled = false;
    track2Enabled = false;
    track1Timer   = 0.0f;
    track2Timer   = 0.0f;
    childCount    = 0;

    if (lines.empty()) {
        cumulativeGlyphs = 0;
        visibleGlyphs    = 0;
        glyphColors.clear();
        return;
    }

    // 2. Build / reuse TextItem children, one per line. binary lazy-grows
    // the children vector via operator_new(0x88) calls.
    Game*        game      = getGame();
    BMFontTable* worldFont = (game != nullptr)
                              ? &game->bmfontTable(2)
                              : nullptr;

    int64_t nonSpaceLineCount = 0;

    for (size_t i = 0; i < lines.size(); i++) {

        if (children.size() <= static_cast<size_t>(childCount)) {
            auto* item = new TextItem();
            item->init(reinterpret_cast<int*>(worldFont));
            children.push_back(item);
        }

        TextItem* child = children[static_cast<size_t>(childCount)];
        child->scaleX = fontScale;
        child->scaleY = fontScale;
        child->setString(lines[i].c_str(), -1);
        childCount += 1;

        // count lines that aren't all-whitespace; they contribute to the
        // vertical layout. matches FUN_100039f38.
        bool allSpaces = true;

        for (char c : lines[i]) {

            if (c != ' ') {
                allSpaces = false;
                break;
            }
        }

        if (!allSpaces) {
            nonSpaceLineCount += 1;
        }
    }

    if (nonSpaceLineCount == 0 || childCount == 0) {
        cumulativeGlyphs = 0;
        visibleGlyphs    = 0;
        glyphColors.clear();
        return;
    }

    // 3. Position non-space lines in a centered vertical column.
    {
        const TextItem* first      = children[0];
        const float     firstScale = first->scaleY;
        const float     firstMaxH  = first->maxCharHeight;
        const float     colHeight  = (static_cast<float>(nonSpaceLineCount) - 1.0f)
                                     * kInitLineStrideMul;

        int64_t lineIdx = 0;

        for (int64_t i = 0; i < childCount; i++) {
            TextItem* child = children[static_cast<size_t>(i)];

            bool allSpaces = true;

            for (size_t j = 0; j < child->storedText.size(); j++) {

                if (child->storedText[j] != ' ') {
                    allSpaces = false;
                    break;
                }
            }

            if (allSpaces) {
                continue;
            }

            child->posX = anchorX
                + child->renderedWidth * child->scaleX * -0.5f;
            child->posY = static_cast<float>(lineIdx) * kInitLineStrideMul
                + (anchorY - (colHeight + firstMaxH * firstScale) * 0.5f)
                + first->maxCharHeight * first->scaleY;
            lineIdx += 1;
        }
    }

    // 4. Walk every glyph: count total + count non-space.
    cumulativeGlyphs = 0;
    visibleGlyphs    = 0;

    for (int64_t i = 0; i < childCount; i++) {
        TextItem* child = children[static_cast<size_t>(i)];
        cumulativeGlyphs += child->glyphCount;

        for (size_t j = 0; j < child->storedText.size(); j++) {

            if (child->storedText[j] != ' ') {
                visibleGlyphs += 1;
            }
        }
    }

    // 5. resize glyphColors to cumulativeGlyphs. fill with random blends of
    // primaryRgba -> outlineRgba via RNG stream 0.
    glyphColors.resize(static_cast<size_t>(cumulativeGlyphs));

    for (int64_t i = 0; i < cumulativeGlyphs; i++) {
        const float rnd = rngFloat(0.0f, 1.0f, /*stream=*/0);
        glyphColors[static_cast<size_t>(i)] =
            blendRgba(rnd, primaryRgba, outlineRgba,
                       /*easeCos=*/0, /*easeDoubled=*/0);
    }

    // 6. per-track timing. matches binary's FUN_100039b14 tail:
    //   track1RateDivisor = cumulativeGlyphs * 0.06
    //   track1Extra       = 10.0 / cumulativeGlyphs
    //   track2RateDivisor = visibleGlyphs    * 0.02
    //   track2Extra       = 10.0 / visibleGlyphs
    // track 1's body normalizes time by cumulativeGlyphs, so its divisor /
    // extra must key off cumulativeGlyphs (track 2 off visibleGlyphs).
    track1RateDivisor = static_cast<float>(cumulativeGlyphs) * kInitTrack1RateMult;
    track1Extra       = 10.0f / static_cast<float>(cumulativeGlyphs);
    track2RateDivisor = static_cast<float>(visibleGlyphs)    * kInitTrack2RateMult;
    track2Extra       = 10.0f / static_cast<float>(visibleGlyphs);
}

// FUN_100039018.
void AnimationController::init(uint32_t entryIdx, const float* posPair) {
    const auto& snippets = DreamSnippets::table();

    if (entryIdx >= snippets.size()) {
        entryIdx = 1;
    }

    constexpr float    kFontScale  = 0.07f;        // DAT_10005a25c
    constexpr uint32_t kPrimaryRgba = 0xFFFFAAFA;  // bytes fa aa ff ff
    constexpr uint32_t kOutlineRgba = 0xFFDC3CC8;
    initFromLines(kFontScale, snippets[entryIdx],
                  posPair[0], posPair[1],
                  kPrimaryRgba, kOutlineRgba);
}

// FUN_100039ea4, thin wrapper: wraps the C string in a 1-element list and
// calls initFromLines (FUN_100039b14).
void AnimationController::startText(
        float fontScale,
        const char* text,
        float anchorX, float anchorY,
        uint32_t primaryRgba,
        uint32_t outlineRgba) {
    std::vector<std::string> lines = { text ? std::string(text) : std::string() };
    initFromLines(fontScale, lines, anchorX, anchorY, primaryRgba, outlineRgba);
}

// FUN_100039f94, render. early-out when track1Timer <= 0 (track 1 hasn't
// started progressing) or track2Timer >= 1 (track 2 has completed).
void AnimationController::draw() {

    if (track1Timer <= 0.0f || track2Timer >= 1.0f) {
        return;
    }

    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);

    for (int64_t i = 0; i < childCount && i < (int64_t)children.size(); ++i) {
        TextItem* child = children[(size_t)i];

        if (child) {
            child->draw();
        }
    }

    glPopMatrix();
}

// FUN_10003a010, per-frame animation tick.
//
// outer gate: skip if either track has reached its terminal (timer >= 1)
// while still enabled.
//
// dispatch order: when track 2 is enabled, run the "track 2 body" (advances
// track2Timer). otherwise, when track 1 is enabled, run the "track 1 body"
// (advances track1Timer).
//
//   TRACK 1 body (score-panel title path):
//     advances track1Timer by dt / track1RateDivisor. per glyph index i in
//     [0, cumulativeGlyphs):
//       u    = clamp((i / cumulative - (timer*(extra+1) - extra)) / extra)
//       invU = 1 - u
//       color = blendRgba(invU, glyphColors[i], 0xff6464ff, eased=true)
//       alpha = (0.5 - cos(invU * pi) * 0.5) * 255
//       scaleY = sin(invU * pi * 0.8) / 0.587785
//       scaleX = scaleY + (1 - scaleY) * 0.5
//     fires sound 0x36 when a non-space glyph's alpha crosses 100. note
//     the 0.8 in the sin argument: at invU=1 the scale settles at
//     sin(0.8*pi)/0.587785 ~= 1.0 rather than zeroing out.
//
//   TRACK 2 body (banner-text fade-out):
//     advances track2Timer by dt / track2RateDivisor. walks every visible
//     (non-space) glyph, lerping scale snapshot -> 1.0 on a global eased
//     progress, blending color snapshot -> glyphColors[i], and cosf-easing
//     alpha from snapshot to 0 with a per-glyph stagger so letters fade
//     out back-to-front. perGlyphData holds the snapshot captured at
//     startFadeOut.
void AnimationController::update(float dt) {

    bool track1Live = !track1Enabled || track1Timer < 1.0f;
    bool track2Live = !track2Enabled || track2Timer < 1.0f;

    if (!(track1Live && track2Live)) {
        return;
    }

    if (track2Enabled) {
        // TRACK 2 body, banner fade-out. advances track2Timer, then per
        // visible glyph: lerp scale toward 1.0, cosf-ease alpha toward 0,
        // blend color toward the "raw" glyphColors entry. perGlyphData
        // holds the snapshot taken at startFadeOut.
        float t2 = track2Timer + dt / track2RateDivisor;

        if (t2 > 1.0f) {
            t2 = 1.0f;
        }

        if (t2 < 0.0f) {
            t2 = 0.0f;
        }

        track2Timer = t2;

        // color + scale share a single global eased progress 0..1 that
        // saturates within track2Extra. alpha is staggered per-glyph so
        // letters fade out one at a time rather than all together.
        const float clampedEased =
            std::min(1.0f, std::max(0.0f, t2 / track2Extra));
        const float timerLeadingEdge = t2 * (track2Extra + 1.0f) - track2Extra;
        const float visibleGlyphsF   = static_cast<float>(visibleGlyphs);

        forEachVisibleGlyph(
            [this, clampedEased, timerLeadingEdge, visibleGlyphsF]
            (size_t visibleIdx, Quad& glyphQuad) {

                if (visibleIdx >= perGlyphData.size()) {
                    return;
                }

                const PerGlyphFade& snap = perGlyphData[visibleIdx];

                // per-glyph fade progress: starts at 1.0 (visible) and
                // drops to 0.0 as the fade wave sweeps over this glyph's
                // index.
                const float uPerGlyph = std::min(1.0f, std::max(0.0f,
                    ((float)visibleIdx / visibleGlyphsF - timerLeadingEdge)
                    / track2Extra));
                const float alphaFade =
                    0.5f - std::cos((1.0f - uPerGlyph) * kBlendCosPi) * 0.5f;

                // color: blend snapshot -> glyphColors[visibleIdx]. at fade
                // end alpha is 0 anyway so the color choice is moot.
                if (visibleIdx < glyphColors.size()) {
                    const uint32_t blended = blendRgba(
                        clampedEased,
                        snap.rgbaSnapshot,
                        glyphColors[visibleIdx],
                        false, false);
                    glyphQuad.vertices[0].color = blended;
                    glyphQuad.vertices[1].color = blended;
                    glyphQuad.vertices[2].color = blended;
                    glyphQuad.vertices[3].color = blended;
                }

                // alpha: cosf-ease from snapshot's alpha byte to 0,
                // gated by the per-glyph fade progress.
                const uint8_t alphaByte = static_cast<uint8_t>(snap.rgbaSnapshot >> 24);
                const float   newAlphaF = (float)alphaByte * (1.0f - alphaFade);
                glyphQuad.setAlpha(static_cast<uint8_t>(newAlphaF));

                // scale: lerp snapshot -> 1.0 by clampedEased.
                glyphQuad.scaleX = clampedEased + (1.0f - clampedEased) * snap.scaleXSnapshot;
                glyphQuad.scaleY = clampedEased + (1.0f - clampedEased) * snap.scaleYSnapshot;
            });

        return;
    }

    if (!track1Enabled) {
        return;
    }

    // TRACK 1 body. advance track1Timer.
    float t = track1Timer + dt / track1RateDivisor;

    if (t > 1.0f) {
        t = 1.0f;
    }

    if (t < 0.0f) {
        t = 0.0f;
    }

    track1Timer = t;

    if (childCount == 0) {
        return;
    }

    int64_t glyphIdx = 0;
    Game*   game     = getGame();

    for (int64_t childI = 0; childI < childCount; childI++) {
        TextItem* child = children[static_cast<size_t>(childI)];

        if (child->glyphCount == 0) {
            continue;
        }

        for (int64_t glyphJ = 0; glyphJ < child->glyphCount; glyphJ++) {
            const float position = static_cast<float>(glyphIdx)
                                   / static_cast<float>(cumulativeGlyphs);
            const float ramp     = t * (track1Extra + 1.0f) - track1Extra;
            float       u        = (position - ramp) / track1Extra;

            if (u < 0.0f) {
                u = 0.0f;
            }

            if (u > 1.0f) {
                u = 1.0f;
            }

            const float invU = 1.0f - u;
            Quad& glyphQuad = child->glyphVec[static_cast<size_t>(glyphJ)].quad;

            const uint8_t oldAlpha = static_cast<uint8_t>(
                (glyphQuad.vertices[0].color >> 24) & 0xFFu);

            // doubled-cos ease (binary calls with mov w2,#1; mov w3,#1).
            // U-curves invU 0..1 so the result returns to glyphColors[i]
            // (the olive blend) at both endpoints and flashes through the
            // outline (red) at the midpoint.
            const uint32_t newColor = blendRgba(invU,
                glyphColors[static_cast<size_t>(glyphIdx)],
                0xff6464ffu, /*easeCos=*/1, /*easeDoubled=*/1);

            for (int v = 0; v < 4; v++) {
                glyphQuad.vertices[v].color = newColor;
            }

            const float c    = std::cos(invU * kTrack1CosPi);
            const float ease = 0.5f - c * 0.5f;
            const auto  a    = static_cast<uint8_t>(
                static_cast<int>(ease * kTrack1AlphaMult));
            glyphQuad.setAlpha(a);

            if (game != nullptr
                && glyphJ < (int64_t)child->storedText.size()
                && child->storedText[(size_t)glyphJ] != ' '
                && oldAlpha < 0x65
                && a > 0x64) {
                game->soundQueue.trigger(0x36);
            }

            // scale arg is invU * pi * 0.8, not invU * pi. the 0.8 factor
            // (DAT_10005a278) means the sin curve peaks past the animation's
            // mid-point and settles to ~1.0 at invU=1, so the title rests at
            // full size instead of vanishing.
            const float s = std::sin(invU * kTrack1CosPi * kTrack1SinMul)
                            / kTrack1ScaleDenom;
            glyphQuad.scaleX = s + (1.0f - s) * 0.5f;
            glyphQuad.scaleY = s;

            glyphIdx += 1;
        }
    }
}

// FUN_10003a408. if track 1 is already enabled, no-op. otherwise enables
// track 1, disables track 2, zeros both timers, runs one update tick.
void AnimationController::reset() {

    if (track1Enabled) {
        return;
    }

    track1Enabled = true;
    track2Enabled = false;
    track1Timer   = 0.0f;
    track2Timer   = 0.0f;
    update(0.0f);
}

void AnimationController::forEachVisibleGlyph(
        const std::function<void(size_t, Quad&)>& fn) {
    size_t visibleIdx = 0;

    for (int64_t lineIdx = 0;
         lineIdx < childCount && lineIdx < (int64_t)children.size();
         ++lineIdx) {
        TextItem* child = children[(size_t)lineIdx];

        if (!child) {
            continue;
        }

        for (int64_t g = 0; g < child->glyphCount; ++g) {
            const char ch = (g < (int64_t)child->storedText.size())
                            ? child->storedText[(size_t)g]
                            : ' ';

            if (ch == ' ') {
                continue;
            }

            fn(visibleIdx, child->glyphVec[(size_t)g].quad);
            visibleIdx++;
        }
    }
}

// FUN_10003a5d8, pin the banner to "fully faded out". sets track2Enabled and
// track2Timer = 1.0; draw() then early-outs on `track2Timer >= 1.0` and update()
// early-outs on `!track2Live`, leaving the banner hidden and frozen.
void AnimationController::forceFadeOutComplete() {
    track2Enabled = true;
    track2Timer   = 1.0f;
}

// FUN_10003a430.
void AnimationController::startFadeOut() {

    if (track2Enabled) {
        return;
    }

    track1Enabled = false;
    track2Enabled = true;
    track2Timer   = 0.0f;

    perGlyphData.clear();

    forEachVisibleGlyph([this](size_t /*visibleIdx*/, Quad& glyphQuad) {
        perGlyphData.push_back({
            glyphQuad.vertices[0].color,
            glyphQuad.scaleX,
            glyphQuad.scaleY,
        });
    });

    update(0.0f);
}

AnimationController::~AnimationController() {

    for (TextItem* child : children) {
        delete child;
    }

    children.clear();
}
