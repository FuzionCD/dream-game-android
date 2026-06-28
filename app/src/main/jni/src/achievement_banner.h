#pragma once

#include "label.h"
#include "quad.h"
#include "text_item.h"
#include "title_menu.h"   // TileIcon

#include <cstddef>
#include <cstdint>
#include <list>

// achievement-unlock banner card. lives at GameBoard.achievementBanner. AchievementTracker
// queues unlocks; GameBoard::update's tail pops the next idx and calls open();
// the card animates in, displays a few seconds, slides out.
//
// ctor FUN_10004efec / reset FUN_10004f3ac / open FUN_10004fd7c /
// update FUN_10004f490 / draw FUN_10004f3b8.
// helpers: FUN_10004fc60 panel-glow, FUN_10004f920 chrome reveal,
//          FUN_10004ffc8 / FUN_1000501a0 / FUN_100050090 pool add,
//          FUN_100050220 pool grow.
//
// draw pipeline: open() rebuilds the per-banner draw-pool by copying every
// panelFrame / dividerStrip / icon / title / description glyph into a
// 3-Quad DrawNode (source = template, foreground + outline = drawn).

struct AchievementBannerDrawNode {
    int32_t  textureId;          // bindTexture id (9 for chrome/icon)
    Quad     source;             // shape template; never drawn
    Quad     foreground;         // drawn
    Quad     outline;            // drawn
};

class AchievementBanner {
public:
    // FUN_10004efec (ctor body). add panelFrame / dividerStrip chrome glyphs,
    // set deco[0..1] UV + size, set title / description scale + pos + color.
    // C++ default ctors handle the RAII members + Quad / Label / TextItem
    // POD inits already; init() runs the FUN_10004efec tail that the binary's
    // in-place ctor does explicitly. called once from GameBoard::construct.
    void init();

    // FUN_10004f3ac. park resetTimer = -1 (idle). called from initLevel.
    void reset();

    // FUN_10004fd7c. populate icon/title/desc from ACHIEVEMENT_TABLE[idx],
    // rebuild draw-pool, start timers, fire unlock sound, prime via update(0).
    void open(uint32_t idx);

    // FUN_10004f490. per-frame anim tick (ascend / hold / descend).
    void update(float dt);

    // FUN_10004f3b8. resetTimer < 0 early-out; gl push + translate + walk pool.
    void draw();

    // ---- byte-exact field layout (verified via FUN_10004efec) ----

    float          posX;
    float          posY;             // written each frame
    float          baseY;            // target Y, set in open()

    Label          panelFrame;       // 9-slice card chrome
    Label          dividerStrip;     // secondary chrome detail
    Quad           icon;             // achievement icon
    Quad           deco[2];          // sparkle decorations
    Quad           panelGlow;        // animated glow streak across panel
    TextItem       title;
    TextItem       description;

    // draw-pool. open() resets drawCursor = 0 then re-appends; nodes are
    // reused across opens, list only grows.
    std::list<AchievementBannerDrawNode> drawPool;
    uint64_t       drawCursor;       // next-insert index

    // state machine.
    //   resetTimer  -1 idle / 0..1 active (open sets 0, reset sets -1)
    //   dismissTimer 3.0 ambient -> <0.3 tap-triggered -> 0 fires close sound
    //   dismissed   1 = sliding out
    float          resetTimer;
    float          dismissTimer;
    uint8_t        dismissed;
};
