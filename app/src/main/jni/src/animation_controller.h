#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class TextItem;  // forward decl; children are TextItem* per the binary
class Quad;      // forward decl; forEachVisibleGlyph hands callback a Quad&

// reconstructed from Ghidra:
//   init  : FUN_100039018 + FUN_100039b14
//   draw  : FUN_100039f94
//   update: FUN_10003a010
//   reset : FUN_10003a408
//
// AnimationController lives at GameBoard.animController (0x88 bytes). it's the
// "transient overlay text" subsystem: picks an entry from the global
// animation table at DAT_10007dfe0 (lazy-built by FUN_1000390b0) and
// renders the entry's text glyphs as fading / sliding overlays on top of
// the gameplay screen. used for "NEW EVENT" / "LEVEL UP" / "NEW ITEM"
// style banner text.
//
// the controller has two independent animation tracks gated by separate
// enable flags + timer pairs:
//   - track 1: track1Enabled / track1Timer / track1RateDivisor / track1Extra
//   - track 2: track2Enabled / track2Timer / track2RateDivisor / track2Extra
//
// reset (FUN_10003a408) enables track 1 (sets track1Enabled) and disables
// track 2 (clears track2Enabled). update (FUN_10003a010) then dispatches: if
// track 2 is enabled it runs the "track 2 body" first (advances track2Timer);
// otherwise, if track 1 is enabled, it runs the "track 1 body" (advances
// track1Timer).
//
// the bodies are not symmetric:
//   - track 1 body uses cumulativeGlyphs for time normalization,
//     does not skip spaces, and consumes the per-glyph color array
//     glyphColors (built by initFromLines).
//   - track 2 body uses visibleGlyphs, skips spaces, and pulls
//     per-glyph color + scale data from the auxiliary table perGlyphData
//     (built by FUN_1000390b0).
// the score panel calls reset() and only ever runs track 1.
//
// draw() is gated on `track1.timer > 0 and track2.timer < 1`. before any
// reset both timers are 0 so draw early-outs; after reset, track 1's timer
// ramps 0->1 and draw fires once it ticks positive.

class AnimationController {
public:
    // FUN_100039b14: build TextItem children + per-glyph color array.
    // takes the text content directly rather than going through the binary's
    // global anim table (DAT_10007dfe0 / FUN_1000390b0). the score panel
    // passes a single string; in-game banner text passes multi-line entries.
    //
    // primaryRgba / outlineRgba are blended per-glyph using a deterministic
    // PRNG (FUN_1000571d0, RNG stream 0); gives each character a varied
    // tint between the two endpoints.
    void initFromLines(float fontScale,
                       const std::vector<std::string>& lines,
                       float anchorX, float anchorY,
                       uint32_t primaryRgba,
                       uint32_t outlineRgba);

    // FUN_100039018. dispatches to initFromLines with the 0xFFFFAAFA /
    // dream-snippet palette and the static 81-entry snippet
    // table (= DreamSnippets::table()). out-of-range entryIdx clamps to
    // 1, matching the binary's `if (table.size() <= idx) idx = 1` guard.
    // called from GameBoard::initLevelContent to display the level-start
    // dream snippet.
    void init(uint32_t entryIdx, const float* posPair);

    // FUN_100039ea4, thin wrapper around initFromLines for callers that
    // already have a single C string + a pair of RGBA colors. matches the
    // binary's call shape used by Shop's unlock-reveal name overlay
    // (FUN_1000525f4 at 0x100053434 / 0x100053540 / 0x1000535f0).
    void startText(float fontScale,
                   const char* text,
                   float anchorX, float anchorY,
                   uint32_t primaryRgba,
                   uint32_t outlineRgba);

    // dtor: deletes heap-allocated TextItem children.
    ~AnimationController();

    // FUN_100039f94, render. early-out when track1Timer <= 0 or
    // track2Timer >= 1. otherwise: glPushMatrix, translate to (posX, posY),
    // iterate the children TextItem array calling vtable[2] on each,
    // glPopMatrix.
    void draw();

    // FUN_10003a010, animation tick. dispatch order: when track 2 is
    // enabled, runs the "track 2 body" (advances track2Timer).
    // otherwise, when track 1 is enabled, runs the "track 1 body"
    // (advances track1Timer). both bodies walk each TextItem
    // child's glyphs and animate color / alpha / scale per cosine ease.
    void update(float dt);

    // FUN_10003a408. enables track 1, disables track 2, zeros both
    // timers, runs one update tick.
    void reset();

    // FUN_10003a430. begin the banner fade-out. snapshots each visible
    // glyph's current color / scale into perGlyphData, switches the
    // active track from 1 to 2, runs one update tick. called by the
    // post-pickup-commit hook (FUN_100023ac4) so the player's first
    // move clears any in-flight level-start snippet.
    void startFadeOut();

    // FUN_10003a5d8, force the banner to its fully-faded-out (hidden) state
    // in one shot: enable track 2 and pin track2Timer to 1.0. with track2Timer
    // >= 1.0 both draw() and update() early-out, so the banner renders nothing
    // and stays frozen. distinct from reset() (FUN_10003a408, which starts
    // track 1). called by restoreFromSnapshot so resuming a saved run does not
    // replay the level-start dream snippet that was already shown.
    void forceFadeOutComplete();

    // --- byte-exact struct fields (0x88 bytes total) ---
    float                  posX;
    float                  posY;
    // children: per-glyph TextItem pointers. binary's std::vector<T*>
    // layout is begin/end/cap pointers, 24 bytes total.
    std::vector<TextItem*> children;
    int64_t                childCount;          // (mirrors children.size())

    // per-glyph base color array. binary writes uint32 RGBA values into
    // it during init; consumed by update for the cos-eased fade.
    std::vector<uint32_t>  glyphColors;
    int64_t                cumulativeGlyphs;    // (sum across children)
    int64_t                visibleGlyphs;       // (= count of non-space chars)

    // track 1 state. enabled by reset() (FUN_10003a408
    // sets track1Enabled); its body runs when track 2 is disabled
    // and the body advances track1Timer.
    bool                   track1Enabled;
    float                  track1Timer;         // (0..1 progress)
    float                  track1RateDivisor;
    float                  track1Extra;

    // track 2 state. disabled by reset(); its body runs
    // first in the dispatch (when enabled) and advances track2Timer.
    // used by in-game banner text variants where both tracks
    // animate simultaneously.
    bool                   track2Enabled;
    float                  track2Timer;
    float                  track2RateDivisor;
    float                  track2Extra;

private:
    // shared walk used by startFadeOut (snapshot) and the track 2 body
    // (apply fade). visits every non-space glyph in textual order, passing
    // its 0-based visible index (non-space count, binary x23), its 0-based
    // global glyph index (counts spaces, binary x24), and a mutable Quad
    // reference. track 2 indexes perGlyphData by the visible index but
    // glyphColors by the global index.
    void forEachVisibleGlyph(
            const std::function<void(size_t, size_t, Quad&)>& fn);

public:
    // per-glyph snapshot used by the track 2 fade-out body. populated
    // by startFadeOut from each visible glyph's current state at the
    // moment the fade kicks in. one entry per visible (non-space) glyph.
    struct PerGlyphFade {
        uint32_t rgbaSnapshot;   // glyph color at fade-start
        float    scaleXSnapshot;
        float    scaleYSnapshot;
    };

    std::vector<PerGlyphFade> perGlyphData;  // (libc++ vector head)
};
