#include "score_panel.h"

#include "game.h"
#include "renderer.h"
#include "sound_queue.h"

#include <GLES/gl.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---- DAT constants extracted from binary (Dream_1.0.3, arm64 slice) -------
// each value verified via scripts/extract_phase3_data.py reading the file
// offset for the named DAT_xxx address.

static constexpr float kCtorBgWidthScale     = 1.1f;         // ctor: bgQuad.setSize x
static constexpr float kCtorBgHeightOffset   = 0.1f;         // DAT_100059dcc

static constexpr float kOpenTitleBaseYOffset = -0.273438f;   // DAT_100059dd0
static constexpr float kOpenRowStrideY       = 0.078125f;    // DAT_100059dd4
static constexpr float kOpenRowValueAnchorX  = 0.765625f;    // DAT_100059dd8
static constexpr float kOpenRowScoreAnchorX  = 0.9375f;      // inline in FUN_100010990
static constexpr float kOpenRowLabelX        = 0.0625f;      // inline
static constexpr float kOpenRowDefaultScale  = 0.06f;        // inline
static constexpr uint32_t kOpenRowDefaultRgba = 0xff141414u; // inline; dark-gray text
static constexpr float kOpenTitleAnimEntry   = 0.09f;        // DAT_100059ddc

static constexpr float kOpenKeyStrideDivisor = 640.0f;       // DAT_100059de4
static constexpr float kOpenKeyPopInStride   = 0.2f;         // DAT_100059de8

static constexpr float kUpdateAlphaMult      = 255.0f;       // DAT_100059dec
static constexpr float kUpdateKeyProgRate    = 0.3f;         // DAT_100059df0
static constexpr float kUpdatePi             = 3.14159274f;  // DAT_100059df4
static constexpr float kUpdateKeySinMul      = 0.8f;         // DAT_100059df8
static constexpr float kUpdateKeyScaleDenom  = 0.866025f;    // DAT_100059dfc
static constexpr float kUpdateKeyScaleThr1   = 0.2f;         // DAT_100059e00
static constexpr float kUpdateKeyScaleThr2   = 0.8f;         // DAT_100059e04
static constexpr float kUpdateKeyScaleOffset = -0.8f;        // DAT_100059e08
static constexpr float kUpdateKeyScaleMax    = 1.2f;         // DAT_100059e0c
static constexpr float kUpdateSecondaryOffset = -0.2f;       // DAT_100059e10
static constexpr float kUpdateSecondaryRate  = 0.6f;         // DAT_100059e14

static constexpr float kLayoutPanelYAbove    = -0.0125f;     // DAT_100059e18
static constexpr float kLayoutRank4XBump     = -0.003125f;   // DAT_100059e1c
static constexpr float kLayoutRank4YBump     = -0.015625f;   // DAT_100059e20
static constexpr float kLayoutRank23YBump    = -0.01875f;    // DAT_100059e24
static constexpr float kLayoutRank1YBump     = -0.00625f;    // DAT_100059e28

static constexpr float kDrawCosMult          = 3.14159274f;  // DAT_100059e2c
static constexpr float kDrawRowStretchMax    = 1.4f;         // DAT_100059e30

static constexpr float kInitialScoreDelay    = 2.5f;         // 0x40200000

// ---- ScorePanelKeyIcon (heap-allocated currency key icon, 0xF0) ----------
// one allocated per key earned, by ScorePanel::open via operator_new(0xF0).
// linked into the intrusive list at +0xE50/+0xE58 by manual prev/next
// manipulation. each icon owns a Quad for its key sprite plus a progress /
// popInTimer pair driven by update().

struct ScorePanelKeyIcon {
    ScorePanelKeyIcon* prev;        // +0x00
    ScorePanelKeyIcon* next;        // +0x08
    Quad               tileQuad;    // +0x10..+0xE7
    float              progress;    // +0xE8  scale-in animation 0..1
    float              popInTimer;  // +0xEC  per-icon stagger delay
};

static_assert(sizeof(ScorePanelKeyIcon) == 0xF0,
              "ScorePanelKeyIcon must be 0xF0 bytes, matching the binary's "
              "operator_new(0xF0) in FUN_100010990.");
static_assert(offsetof(ScorePanelKeyIcon, tileQuad)   == 0x10);
static_assert(offsetof(ScorePanelKeyIcon, progress)   == 0xE8);
static_assert(offsetof(ScorePanelKeyIcon, popInTimer) == 0xEC);

// ---- helpers ------------------------------------------------------------

static inline float clamp01(float v) {

    if (v < 0.0f) {
        return 0.0f;
    }

    if (v > 1.0f) {
        return 1.0f;
    }

    return v;
}

static inline ScorePanelKeyIcon* keyListSentinel(ScorePanel* panel) {
    return reinterpret_cast<ScorePanelKeyIcon*>(&panel->keysRow);
}

// FUN_100011c98, Y-axis stretch wrapper for one TextItem.
// only wraps in the matrix transform when scaleY > 1.0; at the pulse
// endpoints (1.0) push/pop is skipped entirely.
//   centerX = posX + renderedWidth * scaleX * 0.5
//   centerY = posY + maxCharHeight * scaleY * -0.5
//   glPushMatrix; glTranslate(cx, cy); glScalef(1, scaleY);
//   glTranslate(-cx, -cy); draw(); glPopMatrix
static void drawRowLabelStretched(float scaleY, TextItem& label) {

    if (scaleY <= 1.0f) {
        label.draw();
        return;
    }

    const float cx = label.posX + label.renderedWidth * label.scaleX * 0.5f;
    const float cy = label.posY + label.maxCharHeight * label.scaleY * -0.5f;

    glPushMatrix();
    glTranslatef(cx, cy, 0.0f);
    glScalef(1.0f, scaleY, 1.0f);
    glTranslatef(-cx, -cy, 0.0f);
    label.draw();
    glPopMatrix();
}

// FUN_100010764, ctor.
//
// inits the bg Quad + title AnimationController + 8 rows' 24 TextItems
// (via TextItem::init). centers the bg Quad on (0.5, virtualHeight*0.5)
// and sizes it to (1.1, virtualHeight + 0.1). leaves visible flags 0;
// opens later via open().
void ScorePanel::init() {
    visible          = false;
    secondaryVisible = false;
    closeRequested   = false;

    // per-row TextItems are non-trivial; init() is required, value-init of
    // the embedded struct is not enough.
    for (int r = 0; r < 8; r++) {
        rows[r].titleLabel.init();
        rows[r].valueLabel.init();
        rows[r].scoreLabel.init();
    }

    // panel.bgQuad: width 1.1, height vh + 0.1. centers on (0.5, vh*0.5).
    const float vh = Renderer::getVirtualHeight();
    bgQuad.setSize(kCtorBgWidthScale, vh + kCtorBgHeightOffset);
    bgQuad.posX = 0.5f;
    bgQuad.posY = vh * 0.5f;

    // per-vertex gradient: white at the top (vertices 0+1), pink/purple at
    // the bottom (vertex 2 = #E696F5, vertex 3 = #DC8CEB). FUN_100010764.
    // RGBA bytes packed little-endian as 0xAABBGGRR.
    bgQuad.vertices[0].color = 0xFFFFFFFFu;                          // white
    bgQuad.vertices[1].color = 0xFFFFFFFFu;                          // white
    bgQuad.vertices[2].color = (0xFFu << 24) | (0xF5u << 16)
                              | (0x96u << 8) | 0xE6u;               // #E696F5
    bgQuad.vertices[3].color = (0xFFu << 24) | (0xEBu << 16)
                              | (0x8Cu << 8) | 0xDCu;               // #DC8CEB

    titleAnchorX = 0.0f;
    titleAnchorY = 0.0f;
    chromeAlpha  = 0.0f;
    scoreDelay   = 0.0f;
    subProgress  = 0.0f;
    rowProgress  = 0;
    totalScore   = 0;

    // empty sentinel: head/tail point at the list head itself, count = 0.
    auto* sentinel = keyListSentinel(this);
    keysRow.head  = sentinel;
    keysRow.tail  = sentinel;
    keysRow.count = 0;
}

// FUN_100011d54, clear the key-icon linked list.
//
// drops each icon from the intrusive list and operator_delete's it. walks
// from list.tail to the sentinel.
void ScorePanel::clearKeysRow() {

    if (keysRow.count == 0) {
        return;
    }

    auto* sentinel = keyListSentinel(this);
    auto* node     = keysRow.tail;

    keysRow.head  = sentinel;
    keysRow.tail  = sentinel;
    keysRow.count = 0;

    while (node != sentinel) {
        ScorePanelKeyIcon* nextNode = node->next;
        delete node;
        node = nextNode;
    }
}

// helper: allocate + configure one key icon and append it at the head of the
// intrusive list. mirrors the per-iteration body of FUN_100010990's
// `iVar9 < param_3` loop.
//
// totalKeys is the open()'s `keysEarned` (= the binary's param_3, the full
// key-earned count for the run). shared across all pushes so every icon gets
// the same centering offset. iVar12 is the per-iteration X stride accumulator
// (advances by 60 per push).
static ScorePanelKeyIcon* pushKeyIcon(ScorePanel* panel, int totalKeys,
                                       int iVar12) {
    auto* node     = new ScorePanelKeyIcon();
    auto* sentinel = keyListSentinel(panel);

    // insert at head, manual prev/next chain mutation.
    auto* prevHead = panel->keysRow.head;
    node->prev     = prevHead;
    node->next     = sentinel;
    prevHead->next = node;
    panel->keysRow.head  = node;
    panel->keysRow.count += 1;

    // configure tile Quad. all icons share the same UV + size. UV via
    // FUN_1000081bc:
    //   uMin = (0.3544922, 0.66015625)
    //   uMax = (0.4169922, 0.73828125)
    node->tileQuad.setTexCoords(0.3544922f, 0.66015625f,
                                 0.4169922f, 0.73828125f);
    node->tileQuad.setSize(0.1f, 0.125f);

    // position: horizontal cascade. centering offset computed once per open():
    //   fVar15 = (totalKeys - 1) * 0.5 * (-60) / 640
    // and per iteration:
    //   posX = fVar15 + 0.5 + iVar12 / 640
    //   posY = (vh + panel.rows[7].titleLabel.posY) * 0.5
    // iVar12 advances by 60 per loop, spreading icons 60/640 = ~0.094 apart.
    const float vh = Renderer::getVirtualHeight();
    const float fVar15 = static_cast<float>(totalKeys - 1) * 0.5f
                         * (-60.0f) / kOpenKeyStrideDivisor;
    const float scoreRowY = panel->rows[7].titleLabel.posY;

    node->tileQuad.posX = fVar15 + 0.5f
                          + static_cast<float>(iVar12)
                          / kOpenKeyStrideDivisor;
    node->tileQuad.posY = (vh + scoreRowY) * 0.5f;
    node->tileQuad.setAlpha(0);
    node->tileQuad.snapToPixelGrid();

    node->progress   = 0.0f;
    node->popInTimer = static_cast<float>(panel->keysRow.count)
                       * kOpenKeyPopInStride;
    return node;
}

// FUN_100010990, open.
//
// the post-run results panel open. param order:
//   scoreCounts[0] = turns_taken
//   scoreCounts[1] = worlds_completed
//   scoreCounts[2] = snags_defeated
//   scoreCounts[3] = special_snags_defeated
//   scoreCounts[4] = levels_gained
//   scoreCounts[5] = items_found
//   scoreCounts[6] = events_activated
// keysEarned = the number of currency keys awarded for this run (used both
// for the bottom-row pop-in count and the per-icon stagger).
void ScorePanel::open(const int* scoreCounts, int keysEarned) {
    visible          = true;
    secondaryVisible = true;
    closeRequested   = false;
    chromeAlpha      = 0.0f;
    scoreDelay       = kInitialScoreDelay;
    subProgress      = 0.0f;
    rowProgress      = 0;

    // first-time-open path: if row[0].titleLabel is empty, init the per-row
    // glyph-table pointers (worldFont = game.bmfontTables_[2]) + default
    // colors/scales, then set the row title strings.
    if (rows[0].titleLabel.storedText.empty()) {
        Game* game = getGame();
        int* worldFont = (game != nullptr)
            ? game->bmfontTablePtr(2)
            : nullptr;

        for (int r = 0; r < 8; r++) {
            rows[r].titleLabel.glyphTablePtr = worldFont;
            rows[r].valueLabel.glyphTablePtr = worldFont;
            rows[r].scoreLabel.glyphTablePtr = worldFont;

            rows[r].titleLabel.scaleX = kOpenRowDefaultScale;
            rows[r].titleLabel.scaleY = kOpenRowDefaultScale;
            rows[r].valueLabel.scaleX = kOpenRowDefaultScale;
            rows[r].valueLabel.scaleY = kOpenRowDefaultScale;
            rows[r].scoreLabel.scaleX = kOpenRowDefaultScale;
            rows[r].scoreLabel.scaleY = kOpenRowDefaultScale;

            rows[r].titleLabel.rgba = kOpenRowDefaultRgba;
            rows[r].valueLabel.rgba = kOpenRowDefaultRgba;
            rows[r].scoreLabel.rgba = kOpenRowDefaultRgba;

            rows[r].titleLabel.applyColor();
            rows[r].valueLabel.applyColor();
            rows[r].scoreLabel.applyColor();
        }

        rows[0].titleLabel.setString("Turns Taken");
        rows[1].titleLabel.setString("Worlds Completed");
        rows[2].titleLabel.setString("Snags Defeated");
        rows[3].titleLabel.setString("Special Snags Defeated");
        rows[4].titleLabel.setString("Levels Gained");
        rows[5].titleLabel.setString("Items Found");
        rows[6].titleLabel.setString("Events Activated");
        rows[7].titleLabel.setString("Score");
    }

    // build the per-row count + scored-value arrays (binary's local_138 /
    // local_150 vectors). multipliers, FUN_100010990 inline:
    //   row 0: turns          -> raw=turns,  scored=turns
    //   row 1: worlds         -> raw=worlds-1, scored=sum(100, 200, 300, ...)
    //   row 2: snags          -> raw=snags,  scored=snags * 3
    //   row 3: special snags  -> raw=spec,   scored=spec * 10
    //   row 4: levels         -> raw=lvls,   scored=lvls * 5
    //   row 5: items          -> raw=items,  scored=items * 5
    //   row 6: events         -> raw=evt,    scored=evt * 2
    //   row 7: score          -> no raw,     scored=sum of rows 0..6
    int rawCounts[9];
    int scoredValues[9];

    // scored[0] = turns, not turns-1. the `-1` that looks like it belongs
    // here is really local_15c[2], which is row 1's worlds count.
    rawCounts[0]    = scoreCounts[0];
    scoredValues[0] = rawCounts[0];       // turns score = turns count * 1

    // worlds-completed display: raw column is `scoreCounts[1] - 1` (so a
    // player dying on level 1 sees "0 worlds completed"). the scored column
    // then accumulates 100 + 200 + ... over that count.
    rawCounts[1]    = scoreCounts[1] - 1;
    scoredValues[1] = 0;

    if (rawCounts[1] > 0) {
        int n     = 0;
        int accum = 0;
        int step  = 100;

        do {
            n     += 1;
            accum += step;
            scoredValues[1] = accum;
            step  += 100;
        } while (n < rawCounts[1]);
    }

    rawCounts[2]    = scoreCounts[2];
    scoredValues[2] = rawCounts[2] * 3;

    rawCounts[3]    = scoreCounts[3];
    scoredValues[3] = rawCounts[3] * 10;

    rawCounts[4]    = scoreCounts[4];
    scoredValues[4] = rawCounts[4] * 5;

    rawCounts[5]    = scoreCounts[5];
    scoredValues[5] = rawCounts[5] * 5;

    rawCounts[6]    = scoreCounts[6];
    scoredValues[6] = rawCounts[6] * 2;

    rawCounts[7] = 0;

    int total = 0;

    for (int i = 0; i < 7; i++) {
        total += scoredValues[i];
    }

    totalScore      = total;
    scoredValues[7] = total;

    // position rows + format value/score labels.
    const float vh    = Renderer::getVirtualHeight();
    const float baseY = vh * 0.5f + kOpenTitleBaseYOffset;
    char        buf[64];

    for (int r = 0; r < 8; r++) {
        const int spacing = (r == 7) ? 2 : 1;
        rows[r].titleLabel.posX = kOpenRowLabelX;
        rows[r].titleLabel.posY = baseY + (float)(spacing + r) * kOpenRowStrideY;

        if (r != 7) {
            std::snprintf(buf, sizeof(buf), "%d", rawCounts[r]);
            rows[r].valueLabel.setString(buf);
            rows[r].valueLabel.posX = kOpenRowValueAnchorX
                - rows[r].valueLabel.renderedWidth
                * rows[r].valueLabel.scaleX;
            rows[r].valueLabel.posY = rows[r].titleLabel.posY;
        }

        std::snprintf(buf, sizeof(buf), "%d", scoredValues[r]);
        rows[r].scoreLabel.setString(buf);
        rows[r].scoreLabel.posX = kOpenRowScoreAnchorX
            - rows[r].scoreLabel.renderedWidth
            * rows[r].scoreLabel.scaleX;
        rows[r].scoreLabel.posY = rows[r].titleLabel.posY;
    }

    // title position: centered horizontally, half-way between top + baseY.
    titleAnchorX = 0.5f;
    titleAnchorY = baseY * 0.5f;

    // title text "And then I wake up". colors from FUN_100010990's
    // stack-local byte writes:
    //   primary = (R=0x14, G=0x14, B=0x00, A=0xFF), dark olive
    //   outline = (R=0x50, G=0x32, B=0x00, A=0xFF), darker olive
    // both blended per-glyph by initFromLines via RNG stream 0.
    static const std::vector<std::string> kTitleLines = {
        "And then I wake up"
    };
    titleAnim.initFromLines(kOpenTitleAnimEntry,
                             kTitleLines,
                             titleAnchorX, titleAnchorY,
                             /*primary=*/0xFF001414u,
                             /*outline=*/0xFF003250u);
    titleAnim.reset();

    // clear + rebuild key-icon row with `keysEarned` entries.
    clearKeysRow();

    int iVar12 = 0;

    for (int i = 0; i < keysEarned; i++) {
        pushKeyIcon(this, keysEarned, iVar12);
        iVar12 += 0x3C;
    }

    // result rank Quads start cleared; setResultRankVisual(rank) sets them up
    // separately from FUN_100045410's gb+0x01 branch (= our scoreRequested).
    resultRankQuad.setTexCoords(0.0f, 0.0f, 0.0f, 0.0f);
    resultRankQuad.setSize(0.0f, 0.0f);
    resultPanelQuad.setTexCoords(0.0f, 0.0f, 0.0f, 0.0f);
    resultPanelQuad.setSize(0.0f, 0.0f);

    Game* game = getGame();

    if (game != nullptr) {
        game->soundQueue.trigger(0x31);
    }

    // run one frame of update to prime the chrome alpha + scoreDelay tick.
    update(0.0f);
}

// FUN_100011344, per-frame update.
//
// drives the cascading row animation + key-icon pop-in. on each frame:
//   1. chrome alpha ramps 0->1
//   2. title AnimationController updates
//   3. scoreDelay counts down (paused while game input state == press)
//   4. row-by-row cascade: each row's subProgress eases 0..1 via cosine,
//      then advances rowProgress to the next row
//   5. when rowProgress == 8 (final row reached), the per-icon pop-in
//      timers tick down then drive each icon's alpha + scale via sine
//   6. when fully open + game input state == drag (2), set closeRequested
void ScorePanel::update(float dt) {

    // 1. chrome alpha fade (only while < 1).
    if (chromeAlpha < 1.0f) {
        chromeAlpha = clamp01(chromeAlpha + dt);
        const float a = chromeAlpha * kUpdateAlphaMult;
        const auto  ab = static_cast<uint8_t>(static_cast<int>(a));
        bgQuad.setAlpha(ab);
    }

    // 2. title anim controller update.
    titleAnim.update(dt);

    // 3. scoreDelay countdown, blocks row cascade until elapsed.
    if (scoreDelay > 0.0f) {
        Game* game = getGame();

        if (game != nullptr && game->inputState() != 1) {
            scoreDelay -= dt;
            return;
        }

        scoreDelay = 0.0f;
    }

    // 4. row cascade.
    const int64_t currentRow = rowProgress;

    // key-icon path gated on (rowProgress == 8 and subProgress >= 1.0). while
    // subProgress < 1.0 at row 8, fall through to LAB_100011434 so row 7
    // (Score) gets its final fade-in. binary's compound condition
    // `(lVar4 == 8) && (subProgress < 1.0 == NAN(subProgress))` reduces to the
    // same thing.
    if (currentRow == 8 && subProgress >= 1.0f) {
        // 5. all rows shown, key-icon pop-in.
        if (keysRow.count != 0) {
            auto* sentinel = keyListSentinel(this);

            // animate while the head node (first popped icon) hasn't completed.
            if (keysRow.head != sentinel && keysRow.head->progress < 1.0f) {
                const float perIconRate = dt / kUpdateKeyProgRate;

                for (auto* node = keysRow.tail; node != sentinel;
                     node = node->next) {

                    if (node->popInTimer > 0.0f) {
                        node->popInTimer -= dt;
                        continue;
                    }

                    if (node->progress == 0.0f && dt > 0.0f) {
                        Game* game = getGame();

                        if (game != nullptr) {
                            game->soundQueue.trigger(0x33);
                        }
                    }

                    node->progress = clamp01(node->progress + perIconRate);

                    // alpha: cos arg is progress*pi. ramps 0 -> 1 over
                    // progress 0..1.
                    const float c = std::cos(node->progress * kUpdatePi);
                    const float ease = 0.5f - c * 0.5f;
                    const auto  a = static_cast<uint8_t>(
                        static_cast<int>(ease * kUpdateAlphaMult));
                    node->tileQuad.setAlpha(a);

                    // scale: sin arg is progress * 0.8 (radians, not *pi).
                    // settles at sin(0.8)/0.866 ~= 0.83 at progress=1, so the
                    // icon rests slightly under its atlas size. the 0.8 matters:
                    // a 2*pi arg would zero both alpha and scale at progress=1
                    // and the icons would vanish. writes Quad.scaleX/scaleY.
                    const float bump = std::sin(node->progress * kUpdateKeySinMul)
                                       / kUpdateKeyScaleDenom;
                    node->tileQuad.scaleX = bump;
                    node->tileQuad.scaleY = bump;
                }
                return;
            }
        }

        // 6. fully open + drag input -> request dismiss.
        Game* game = getGame();

        if (game != nullptr && game->inputState() == 2) {
            closeRequested = true;
        }

        return;
    }

    // row 0..7 still cascading.
    Game* game = getGame();

    if (currentRow != 0 && subProgress < 1.0f) {
        // continue advancing within current row -> fall to LAB_100011434.
    } else {
        // either row 0 init or previous row finished; advance.
        rowProgress = currentRow + 1;
        subProgress = 0.0f;
    }

    // LAB_100011434, common tail: drive subProgress + apply alpha + scale
    // to the current row's 3 labels. fires sound 0x32 once when a new row
    // starts animating in.
    if (subProgress == 0.0f && dt > 0.0f) {

        if (game != nullptr) {
            game->soundQueue.trigger(0x32);
        }
    }

    float newSub;

    if (game != nullptr && game->inputState() == 1) {
        // game touch-down snaps subProgress to 1.0 (skip this row's anim).
        subProgress = 1.0f;
        newSub      = 1.0f;
    } else {
        // clamp(2*dt + subProgress); ~31 frames per row at 60fps.
        subProgress = clamp01(2.0f * dt + subProgress);
        newSub      = subProgress;
    }

    const float ease = 0.5f - std::cos(newSub * kUpdatePi) * 0.5f;
    const auto  rowAlpha = static_cast<uint8_t>(
        static_cast<int>(ease * kUpdateAlphaMult));

    // apply alpha to the 3 columns of rows[rowProgress - 1].
    const int64_t idx = rowProgress - 1;

    if (idx >= 0 && idx < 8) {
        // binary calls FUN_1000301fc which is TextItem::setAlpha (writes the
        // alpha byte then applyColor).
        rows[idx].titleLabel.setAlpha(rowAlpha);
        rows[idx].valueLabel.setAlpha(rowAlpha);
        rows[idx].scoreLabel.setAlpha(rowAlpha);
    }

    // when the final row (Score) lands, scale the result quads via a
    // two-piece cosine ease:
    //   newSub < 0.2          -> outSize = 0 (invisible)
    //   newSub in [0.2, 0.8]  -> ramps 0 -> 1.2 over width 0.6 (the pop-in,
    //                            uses offset=-0.2, rate=0.6)
    //   newSub in [0.8, 1.0]  -> ramps 1.2 -> 1.0 over width 0.2 (overshoot
    //                            settle, uses offset=-0.8, rate=0.2)
    // FUN_100011344's branches: mid pairs +DAT_100059e10 / e14, upper pairs
    // +DAT_100059e08 / e00.
    if (rowProgress == 8) {
        float outSize;

        if (newSub < kUpdateKeyScaleThr1) {
            outSize = 0.0f;
        } else if (newSub < kUpdateKeyScaleThr2) {
            // mid segment: width 0.6, ramps up.
            const float t = std::cos(((newSub + kUpdateSecondaryOffset)
                                       / kUpdateSecondaryRate) * kUpdatePi);
            const float k = 0.5f - t * 0.5f;
            outSize = k * kUpdateKeyScaleMax;
        } else {
            // upper segment: width 0.2, settles back to 1.0.
            // outSize = k + (1-k)*1.2:
            //   at newSub=0.8 (k=0): outSize = 0 + 1*1.2 = 1.2 (peak)
            //   at newSub=1.0 (k=1): outSize = 1 + 0*1.2 = 1.0 (settled)
            // the leading `+ k` is load-bearing: drop it and the medal
            // collapses to 0 at the end.
            const float t = std::cos(((newSub + kUpdateKeyScaleOffset)
                                       / kUpdateKeyScaleThr1) * kUpdatePi);
            const float k = 0.5f - t * 0.5f;
            outSize = k + (1.0f - k) * kUpdateKeyScaleMax;
        }

        // writes Quad.scaleX/scaleY (+0xB8/+0xBC inside each Quad), not
        // animMinX/animMinY (+0xC8/+0xCC).
        resultRankQuad.scaleX  = outSize;
        resultRankQuad.scaleY  = outSize;
        resultPanelQuad.scaleX = outSize;
        resultPanelQuad.scaleY = outSize;
    }
}

// FUN_100011b78, draw.
//
// 1. bind gameplay texture (slot 0)
// 2. draw bg quad
// 3. draw the title animation controller
// 4. iterate animated-in rows, drawing each row's 3 labels (with extra fade
//    on the last row currently animating)
// 5. bind panel atlas (slot 8)
// 6. draw resultRankQuad + resultPanelQuad
// 7. iterate key-icon list (tail to head), draw each entry's Quad
void ScorePanel::draw() {

    if (!visible) {
        return;
    }

    bindTexture(0);
    bgQuad.draw();
    titleAnim.draw();

    // walk rows 0..rowProgress-1. each row's alpha was set by update(); the
    // last row (`i == rowProgress - 1`) also gets a Y-stretch pulse driven
    // by subProgress via FUN_100011c98:
    //   ease = 0.5 - cos(2 * subProgress * pi) * 0.5
    //   yScale = ease * 1.4 + (1 - ease) * 1.0
    // pulses 1.0 -> 1.4 -> 1.0 as the row cascades in. earlier rows pass
    // scaleY=1.0 which short-circuits to a plain draw().
    const int64_t shown = rowProgress;

    if (shown > 0) {
        const float subTwo = subProgress + subProgress;
        const float ease   = 0.5f - std::cos(subTwo * kDrawCosMult) * 0.5f;
        const float lastY  = ease * kDrawRowStretchMax + (1.0f - ease);

        for (int64_t i = 0; i < shown && i < 8; i++) {
            const float y = (i == shown - 1) ? lastY : 1.0f;
            drawRowLabelStretched(y, rows[i].titleLabel);
            drawRowLabelStretched(y, rows[i].valueLabel);
            drawRowLabelStretched(y, rows[i].scoreLabel);
        }
    }

    bindTexture(8);
    resultRankQuad.draw();
    resultPanelQuad.draw();

    // walk key icons tail -> head, drawing each.
    auto* sentinel = keyListSentinel(this);

    for (auto* node = keysRow.tail; node != sentinel; node = node->next) {
        node->tileQuad.draw();
    }
}

// FUN_100011730, pick result-rank visual layout based on rank (1..4).
//
// the rank int comes from FUN_100037d3c (per-difficulty stat-history insert)
// after open(). cases 1..4 pick distinct UV regions + sizes for the panel
// atlas's "rank visual" pair (resultRankQuad + resultPanelQuad). rank
// outside 1..4 clears the quads.
//
// exact in-game semantics aren't pinned down: the rank visual could be a
// medal stamp, a per-tier outcome graphic, or something else. ported
// byte-faithfully against the hardcoded UV/size literals.
void ScorePanel::setResultRankVisual(int rank) {
    // both result quads anchor to the Score row (rows[7]), not the title.
    // resultRankQuad.posX/posY are set first; resultPanelQuad picks them up
    // per-case with a small Y (and rank-4: X) bump.
    //   *(panel + 0xf10) = 0.5
    //   *(panel + 0xf14) = rows[7].titleLabel.posY + (-0.0125)
    resultRankQuad.posX = 0.5f;
    resultRankQuad.posY = rows[7].titleLabel.posY + kLayoutPanelYAbove;

    if (rank < 1 || rank > 4) {
        // no rank visual: clear scales so the update() pop-in starts from
        // zero. LAB_100011b44 still snaps the panel quad and zeros both pairs
        // of scaleX/Y; positions stay wherever they were.
        resultPanelQuad.snapToPixelGrid();
        resultRankQuad.scaleX  = 0.0f;
        resultRankQuad.scaleY  = 0.0f;
        resultPanelQuad.scaleX = 0.0f;
        resultPanelQuad.scaleY = 0.0f;
        return;
    }

    // all four cases share the rank-quad UV from the binary literal.
    // each case picks distinct sizes + a tinted color overlay. tints decoded
    // from FUN_100011730 disassembly (float-as-RGBA bit patterns):
    //   rank 1 = gold   #FFFF64
    //   rank 2 = silver #F5F5F5
    //   rank 3 = bronze #EBB432
    //   rank 4 = green  #32E632
    resultRankQuad.setTexCoords(0.5859375f, 0.0f, 0.7041016f, 0.103515625f);

    switch (rank) {
        case 1: {
            resultRankQuad.setSize(0.1890625f, 0.165625f);
            resultPanelQuad.setTexCoords(0.5986328f, 0.16796875f,
                                          0.6835938f, 0.23535156f);
            resultPanelQuad.setSize(0.1359375f, 0.1078125f);
            resultPanelQuad.posX = resultRankQuad.posX;
            resultPanelQuad.posY = resultRankQuad.posY + kLayoutRank1YBump;
            resultRankQuad.setColor(0xFF, 0xFF, 0x64, 0xFF);    // gold
            resultPanelQuad.setColor(0xFF, 0xFF, 0xFF, 0xFF);   // white
            break;
        }
        case 2: {
            resultRankQuad.setSize(0.15125f, 0.13250001f);
            resultPanelQuad.setTexCoords(0.66015625f, 0.10449219f,
                                          0.7041016f, 0.16796875f);
            resultPanelQuad.setSize(0.05625f, 0.08f);
            resultPanelQuad.posX = resultRankQuad.posX;
            resultPanelQuad.posY = resultRankQuad.posY + kLayoutRank23YBump;
            resultRankQuad.setColor(0xF5, 0xF5, 0xF5, 0xFF);    // silver
            resultPanelQuad.setColor(0xFF, 0xFF, 0xFF, 0xFF);   // white
            break;
        }
        case 3: {
            resultRankQuad.setSize(0.13234374f, 0.115937494f);
            resultPanelQuad.setTexCoords(0.6152344f, 0.10449219f,
                                          0.6591797f, 0.16796875f);
            resultPanelQuad.setSize(0.04921875f, 0.07f);
            resultPanelQuad.posX = resultRankQuad.posX;
            resultPanelQuad.posY = resultRankQuad.posY + kLayoutRank23YBump;
            resultRankQuad.setColor(0xEB, 0xB4, 0x32, 0xFF);    // bronze
            resultPanelQuad.setColor(0xFF, 0xD2, 0x64, 0xFF);   // light gold
            break;
        }
        case 4: {
            resultRankQuad.setSize(0.11343751f, 0.099375f);
            resultPanelQuad.setTexCoords(0.5673828f, 0.10449219f,
                                          0.6142578f, 0.16796875f);
            resultPanelQuad.setSize(0.045f, 0.06f);
            resultPanelQuad.posX = resultRankQuad.posX + kLayoutRank4XBump;
            resultPanelQuad.posY = resultRankQuad.posY + kLayoutRank4YBump;
            resultRankQuad.setColor(0x32, 0xE6, 0x32, 0xFF);    // green
            resultPanelQuad.setColor(0x64, 0xFA, 0x64, 0xFF);   // bright green
            break;
        }
    }

    resultPanelQuad.snapToPixelGrid();
    // FUN_100011730 tail clears Quad.scaleX + scaleY for each result quad
    // (not animMinX/animMinY), so the pop-in starts from zero.
    resultRankQuad.scaleX  = 0.0f;
    resultRankQuad.scaleY  = 0.0f;
    resultPanelQuad.scaleX = 0.0f;
    resultPanelQuad.scaleY = 0.0f;
}
