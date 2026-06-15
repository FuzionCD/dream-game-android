#include "event_choice_panel.h"
#include "detail_panel.h"
#include "event_slot.h"
#include "event_table.h"
#include "game.h"
#include "game_board.h"
#include "label.h"      // for GlyphOffset
#include "renderer.h"
#include "player_system.h"
#include "random.h"     // rngInt, candidate-pick RNG (stream 3)
#include <GLES/gl.h>
#include <iterator>     // std::next, RB-tree position lookup for rng-pop

namespace {

// Quad::setColor packed-bytes shim, same helper used elsewhere.
void quadSetColorBytes(Quad& q, uint32_t packedRGBA) {
    uint8_t r = (uint8_t)(packedRGBA >>  0);
    uint8_t g = (uint8_t)(packedRGBA >>  8);
    uint8_t b = (uint8_t)(packedRGBA >> 16);
    uint8_t a = (uint8_t)(packedRGBA >> 24);
    q.setColor(r, g, b, a);
}

}   // namespace

// FUN_10000e128, EventChoicePanel::init.
//
// Menu base init with the event-choice title atlas (641, 808, 309, 48),
// then per-slot widget setup:
//   - labelDefault / labelSelected: 9-slice frame chrome (3 atlas strips
//     each: left edge, middle stretch, right edge). atlas regions:
//       default:  (488, 81), (508, 81), (509, 81), sizes 20x124 / 1x124 / 20x124
//       selected: (530, 81), (550, 81), (551, 81), sizes 20x123 / 1x123 / 20x123
//   - titleText: scale 0.085, points at game's panel glyph table
//   - descText[0..2]: scale 0.07, same glyph table
//   - iconQuad: bare TileIcon init (UV/size set per slot at open() time)
//   - slotPtr = nullptr (open() populates)
void EventChoicePanel::init(DetailPanel* heldItemRefIn) {

    Menu::init(0x281, 0x328, 0x135, 0x30);

    heldItemRef = heldItemRefIn;

    Game* g = getGame();
    int*  panelGlyphs = g ? g->bmfontTablePtr(0) : nullptr;

    // 9-slice atlas table: 3 strips per Label, default and selected variants.
    static constexpr float L0_UV_ORIGIN[3][2] = {
        { 488.0f, 81.0f }, { 508.0f, 81.0f }, { 509.0f, 81.0f },
    };
    static constexpr float L0_UV_SIZE[3][2] = {
        {  20.0f, 124.0f }, {   1.0f, 124.0f }, {  20.0f, 124.0f },
    };
    static constexpr float L1_UV_ORIGIN[3][2] = {
        { 530.0f, 81.0f }, { 550.0f, 81.0f }, { 551.0f, 81.0f },
    };
    static constexpr float L1_UV_SIZE[3][2] = {
        {  20.0f, 123.0f }, {   1.0f, 123.0f }, {  20.0f, 123.0f },
    };
    static constexpr uint32_t FRAME_MODES[3] = { 2, 1, 2 };

    for (int i = 0; i < MAX_SLOT_COUNT; ++i) {
        EventChoiceSlot& slot = slots[i];

        // initial state
        slot.slotPtr = nullptr;

        // Labels: empty init, then add the per-Label 9-slice glyphs.
        slot.labelDefault.init();
        slot.labelSelected.init();

        for (int gi = 0; gi < 3; ++gi) {
            GlyphOffset off = { 0.0f, 0.0f };
            slot.labelDefault.addGlyph(-1.0f, L0_UV_ORIGIN[gi], L0_UV_SIZE[gi],
                                       FRAME_MODES[gi], off);
        }

        for (int gi = 0; gi < 3; ++gi) {
            GlyphOffset off = { 0.0f, 0.0f };
            slot.labelSelected.addGlyph(-1.0f, L1_UV_ORIGIN[gi], L1_UV_SIZE[gi],
                                        FRAME_MODES[gi], off);
        }

        // TextItems: empty init, then point glyphTable + set scale.
        // title 0.085, descs 0.07.
        slot.titleText.init();
        slot.titleText.glyphTablePtr = panelGlyphs;
        slot.titleText.scaleX        = 0.085f;
        slot.titleText.scaleY        = 0.085f;

        for (int d = 0; d < 3; ++d) {
            slot.descText[d].init();
            slot.descText[d].glyphTablePtr = panelGlyphs;
            slot.descText[d].scaleX        = 0.07f;
            slot.descText[d].scaleY        = 0.07f;
        }

        // iconQuad: bare Quad init (UV / size assigned at open() time).
        slot.iconQuad = Quad();
    }

    // tail state
    selectedEventOnConfirm = nullptr;
    slotCount              = 0;
    selectedSlot           = -1;
    // excludedHistory is a std::set<int>, default-constructed empty by
    // GameBoard's create() path. binary's manual sentinel-byte writes at
    // +0x14F8 are equivalent (libc++ aarch64 layout matches).
}

// FUN_10000e6d4, EventChoicePanel::draw.
void EventChoicePanel::draw() {

    if (!visible) {
        return;
    }

    Menu::draw();

    glPushMatrix();
    glTranslatef(anchorX, anchorY, 0.0f);

    bindTexture(9);

    // first pass: per-slot chrome label (selected variant when this slot
    // is the active selection).
    for (int i = 0; i < slotCount; ++i) {
        EventChoiceSlot& slot = slots[i];
        Label& chrome = (i == selectedSlot) ? slot.labelSelected
                                            : slot.labelDefault;
        chrome.draw();
    }

    // second pass: each EventSlot's visual (mainQuad + eventFrame +
    // chargeSlots + chargeMarkers). EventSlot::draw rebinds tex 12.
    for (int i = 0; i < slotCount; ++i) {

        if (slots[i].slotPtr) {
            slots[i].slotPtr->draw();
        }
    }

    bindTexture(9);

    // third pass: per-slot icon Quad (separate from EventSlot's visuals,
    // a chrome decoration overlaid on the chosen-event card).
    for (int i = 0; i < slotCount; ++i) {
        slots[i].iconQuad.draw();
    }

    // fourth pass: per-slot text (title + 3 desc lines).
    for (int i = 0; i < slotCount; ++i) {
        EventChoiceSlot& slot = slots[i];
        slot.titleText.draw();

        for (int d = 0; d < 3; ++d) {
            slot.descText[d].draw();
        }
    }

    glPopMatrix();
}

// FUN_10000e834, EventChoicePanel::update.
//
// per-frame chrome update + tap dispatch. mirrors ItemChoicePanel::update
// almost exactly; same shape, different sub-widgets:
//   1. Menu::baseUpdate(dt): fade animation + confirm-button tick.
//   2. early-out if not visible or fade hasn't completed.
//   3. on inputState == 1 (tap): for each slot, hit-test EventSlot::
//      contains first; on hit, pop up detail panel with name + desc.
//      then hit-test labelDefault; on hit, select that slot (sound 5/7).
//   4. on inputState == 0 (release): tear down the detail-panel popup.
void EventChoicePanel::update(float dt, float touchInput) {
    (void)touchInput;
    baseUpdate(dt);

    if (!visible || animTimer0 < 1.0f) {
        return;
    }

    Game* g = getGame();

    if (!g) {
        return;
    }

    const int touchState = g->inputState();

    if (touchState == 1) {
        const float touchLocalX = g->touchX() - anchorX;
        const float touchLocalY = g->touchY() - anchorY;

        for (int i = 0; i < slotCount; ++i) {
            EventChoiceSlot& slot = slots[i];

            // EventSlot icon tap -> detail-panel popup. binary calls
            // FUN_100040124 (text + atlas-icon detail populator) with the
            // event's name, 2 desc lines, and a key-dependent atlas-icon
            // slot. not ported yet; we observe the touch but show no
            // card. wire up once FUN_100040124 lands.
            //
            // do not early-return here. the binary falls through to the
            // labelDefault hit test so an icon tap also selects the slot
            // (the icon sits inside the chrome frame, so the same touch
            // hits both bboxes and both actions fire).
            (void)(slot.slotPtr && slot.slotPtr->contains(touchLocalX, touchLocalY));

            // chrome label tap -> select this slot.
            if (slot.labelDefault.contains(touchLocalX, touchLocalY)) {

                if (selectedSlot >= 0 && selectedSlot != i) {
                    setSlotSelected(selectedSlot, false);
                }

                int sound = (selectedSlot == i) ? 7 : 5;
                g->soundQueue.trigger(sound);

                selectedSlot = i;
                setSlotSelected(i, true);
                readyByte = 1;
                return;
            }
        }

    } else if (touchState == 0) {
        heldItemRef->reset(0);
    }
}

// FUN_10000eb2c, EventChoicePanel::setSlotSelected.
//
// tints the slot's title + description text on selection. same color
// matrix as ItemChoicePanel::setSlotSelected:
//   titleText: white (unselected) / light-gray (selected)
//   descText:  faint magenta-tinted (unselected) / dimmer (selected)
// also tints the iconQuad behind the chrome via 0xC8C8C8 / 0xFFFFFFFF.
void EventChoicePanel::setSlotSelected(int slotIdx, bool selected) {
    EventChoiceSlot& slot = slots[slotIdx];

    // EventSlot visual: dimColor / resetColor based on selection.
    // when no slotPtr (shouldn't happen at this point), skip.
    if (slot.slotPtr) {

        if (selected) {
            slot.slotPtr->dimColor();
        } else {
            slot.slotPtr->resetColor();
        }
    }

    // iconQuad tint behind chrome
    const uint32_t iconRGBA = selected ? 0xFFC8C8C8u : 0xFFFFFFFFu;
    quadSetColorBytes(slot.iconQuad, iconRGBA);

    // titleText: white (unselected) / light-gray (selected)
    const uint8_t nameRGB = selected ? 200 : 255;
    slot.titleText.colorR = nameRGB;
    slot.titleText.colorG = nameRGB;
    slot.titleText.colorB = nameRGB;
    slot.titleText.alpha  = 0xFF;
    slot.titleText.applyColor();

    // descText: faint magenta-tinted (unselected) / dimmer (selected)
    const uint8_t descRB = selected ? 0x96 : 0xC8;
    const uint8_t descG  = selected ? 0x8C : 0xBE;

    for (int d = 0; d < 3; ++d) {
        TextItem& t = slot.descText[d];
        t.colorR = descRB;
        t.colorG = descG;
        t.colorB = descRB;
        t.alpha  = 0xFF;
        t.applyColor();
    }
}

// FUN_10000f31c, candidate roller.
//
// helper used only by EventChoicePanel::open. picks `count` fresh
// EventSlots (one per EventKey category) excluding the kinds already in
// the tray and any kinds previously dismissed via excludedHistory.
//
// when count == 4 (Perceptive perk owned), one of the 4 slots gets a
// +1 starting-charge bonus (the Sudden-perk slot). the bonus-slot index
// is picked via RNG stream 3 over [0, 3].
//
// the binary uses std::set<int> for both the excluded set and the
// per-category candidate set, with FUN_10000f800 (rng-pop via
// std::next iterator walk). we use the same primitives directly.
namespace {

int rngPopSet(std::set<int>& s, uint32_t stream) {
    int idx = rngInt(0, static_cast<int>(s.size()) - 1, stream);
    auto it = std::next(s.begin(), idx);
    int v = *it;
    s.erase(it);
    return v;
}

void rollCandidates(EventChoicePanel& panel,
                    const std::vector<EventSlot*>& sourceEvents,
                    int count,
                    std::vector<EventSlot*>& out) {

    std::set<int> excludedKinds;

    for (EventSlot* es : sourceEvents) {

        if (es) excludedKinds.insert(es->eventType);
    }

    for (int kind : panel.excludedHistory) {
        excludedKinds.insert(kind);
    }

    std::set<int> availableKeys;
    for (int k = 0; k < 5; ++k) {
        availableKeys.insert(k);
    }

    int magnitudeBonusIdx = -1;

    if (count == 4) {
        magnitudeBonusIdx = rngInt(0, 3, 3);
    }

    out.clear();

    for (int i = 0; i < count; ++i) {
        int key = rngPopSet(availableKeys, 3);

        std::set<int> kindsForKey;
        for (int kind = 0; kind < 81; ++kind) {

            if (static_cast<int>(EVENT_TABLE[kind].key) != key) continue;
            if (excludedKinds.count(kind))                    continue;

            kindsForKey.insert(kind);
        }

        if (kindsForKey.empty()) {
            // shouldn't happen with the default tables, but safety-net
            // for shrunken pools: skip this slot.
            continue;
        }

        int kind = rngPopSet(kindsForKey, 3);
        excludedKinds.insert(kind);   // don't re-roll same kind for later slots

        EventSlot* fresh = new EventSlot();
        fresh->init(kind, (i == magnitudeBonusIdx) ? 1 : 0);
        out.push_back(fresh);
    }
}

}   // anonymous namespace

// FUN_10000ec58, EventChoicePanel::open.
//
// see header for high-level contract. layout follows the binary line-by-
// line; DAT constants extracted from __DATA at runtime via parse_macho.
void EventChoicePanel::open(PlayerSystem& ps,
                            const std::vector<EventSlot*>& sourceEvents) {
    // chrome show: visible=1, animTimer0=0, readyByte cleared, sound 8.
    panelShow();

    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(9);
    }

    heldItemRef->reset(0);
    selectedSlot = -1;

    // Perceptive (perkType 0x13) grants a 4th candidate.
    slotCount = (ps.perkLevel(0x13) > 0) ? 4 : 3;

    // build candidates + populate text. while populating, track the
    // max rendered text width (renderedWidth * scaleX) across all slots'
    // title + 3 desc lines. the panel chrome label's width derives from
    // this max via snapMenuPixel + DAT_100059d6c.
    std::vector<EventSlot*> candidates;
    rollCandidates(*this, sourceEvents, slotCount, candidates);

    float maxTextWidth = 0.0f;

    for (int i = 0; i < slotCount; ++i) {
        EventChoiceSlot& slot = slots[i];

        // free the prior slot's EventSlot (if any); these are owned.
        if (slot.slotPtr) {
            delete slot.slotPtr;
            slot.slotPtr = nullptr;
        }

        if (i >= static_cast<int>(candidates.size())) {
            // candidate roll fell short, leave this slot empty.
            continue;
        }
        slot.slotPtr = candidates[i];

        slot.titleText.setString(slot.slotPtr->getName(), -1);
        float w = slot.titleText.renderedWidth * slot.titleText.scaleX;

        if (w > maxTextWidth) maxTextWidth = w;

        for (int d = 0; d < 3; ++d) {
            slot.descText[d].setString(slot.slotPtr->getDescriptionLine(d), -1);
            float dw = slot.descText[d].renderedWidth * slot.descText[d].scaleX;

            if (dw > maxTextWidth) maxTextWidth = dw;
        }
    }

    // pixel-snap the max width to the 1/640 grid.
    float snappedMax = snapMenuPixel(maxTextWidth);

    // per-slot layout constants. DAT_100059d6c..DAT_100059dac in
    // the binary; renamed by role here. all positions in screen-space
    // (0..1 horizontally, 0..virtualHeight vertically); ROW_STRIDE_PX +
    // DESC_LINE_STEP_PX + DESC_FIRST_LINE_Y_PX + PANEL_H_BASE_PX are
    // in pixel units before /UV_PIXEL_PER_UNIT.
    constexpr float UV_PIXEL_PER_UNIT       = 640.0f;       // (= 1 unit / 640 px)

    // every sub-element's position is relative to labelDefault's
    // (leftX, topY), the chrome's top-left. binary re-fetches both via
    // getLeftX (which returns leftX in s0 + topY in s1) at the start of
    // each sub-element setup. labelDefault.topY = baseY since the
    // setPosition call above used baseY as topY, so all the "_FROM_LABEL"
    // offsets are equivalent to "_FROM_BASE".
    constexpr float LABEL_CHROME_PADDING        = 0.26249999f;  // chrome W = textW + this
    constexpr float SLOT_ROW_Y_ORIGIN           = 0.05468750f;  // Y of first slot's row top
    constexpr float CHROME_LEFT_X               = 0.05625000f;  // labelDefault.posX
    constexpr float EVENT_SLOT_X_FROM_LABEL_TOP = 0.11875000f;  // DAT_100059d7c
    constexpr float SLOT_Y_FROM_LABEL_TOP       = 0.09843750f;  // DAT_100059d80
    constexpr float TITLE_Y_FROM_LABEL_TOP      = 0.06250000f;  // DAT_100059d84
    constexpr float ICON_X_FROM_LABEL_TOP       = 0.05156250f;  // DAT_100059d8c
    constexpr float ICON_Y_FROM_LABEL_TOP       = 0.03437500f + 0.00937500f;
                                                                // DAT_100059d90 + DAT_100059d98
    constexpr float TEXT_X_FROM_LABEL_TOP       = 0.234375f;    // titleText / descText X
                                                                //   offset from label.leftX

    // per-EventKey icon X-nudges (centers icon visually relative to its
    // chrome label given each key's iconQuad width).
    constexpr float ICON_X_NUDGE_EXPERIENCE = -0.01406250f;  // key 0
    constexpr float ICON_X_NUDGE_HEALTH     = -0.00781250f;  // key 1
    constexpr float ICON_X_NUDGE_ATTACK     = -0.00781250f;  // key 2 (same value as 1)
    constexpr float ICON_X_NUDGE_DEFENCE    = -0.00937500f;  // key 3
    constexpr float ICON_X_NUDGE_CONTROL    = -0.00468750f;  // key 4

    constexpr float ROW_STRIDE_PX           = 126.0f;        // per-slot Y stride (px)
    constexpr float DESC_LINE_STEP_PX       = 25.0f;         // per-desc-line Y step (px)
    constexpr float DESC_FIRST_LINE_Y_PX    = 72.0f;         // first descText Y offset (px)
    constexpr float PANEL_W_OVERHEAD        = 0.375f;        // setSize(W = textW + this)
    constexpr float PANEL_H_BASE_PX         = 69.0f;         // setSize(H = (n*ROW_STRIDE + this)/640)
    // binary hardcodes ANCHOR_Y_3_SLOTS = 0.35937500 / ANCHOR_Y_4_SLOTS = 0.29375
    // for iOS (virtualHeight = 1.5). Menu::setSizeAndCenterY derives the
    // anchor from the runtime virtualHeight so it works on Android's
    // ~2.2 virtualHeight too.

    float labelWidth = snappedMax + LABEL_CHROME_PADDING;

    for (int i = 0; i < slotCount; ++i) {
        EventChoiceSlot& slot = slots[i];

        // measure the glyph height for hit-test sizing. binary calls
        // measureGlyphRun(-1, -1) here and uses its heightTotal return
        // (the last glyph's natural V-span for the open-filter case)
        // as the height arg for setSize. that height ends up in
        // cachedSize1, which the contains bbox test reads via getWidth's
        // dual return, so passing the wrong height breaks the chrome's
        // touch target.
        float labelHeight =
            slot.labelDefault.measureGlyphRun(-1, -1).heightTotal;
        slot.labelDefault.setSize(labelWidth, labelHeight);
        float baseY = (float)i * ROW_STRIDE_PX / UV_PIXEL_PER_UNIT
                    + SLOT_ROW_Y_ORIGIN;
        slot.labelDefault.setPosition(CHROME_LEFT_X, baseY);

        const float labelW    = slot.labelDefault.getWidth();
        const float labelLeft = slot.labelDefault.getLeftX();
        const float labelTop  = slot.labelDefault.topY;   // = baseY since we just
                                                          //   setPosition'd labelDefault
                                                          //   with baseY.

        // binary uses labelDefault.cachedSize1 (= labelHeight) for
        // labelSelected too; getWidth's s1 return preserves it across
        // the call chain. our labelSelected has slightly different glyph
        // heights (123 vs 124 px) but reusing labelDefault's measured
        // value matches the binary exactly.
        slot.labelSelected.setSize(labelW, labelHeight);
        slot.labelSelected.setPosition(labelLeft, labelTop);

        if (slot.slotPtr) {
            float slotXY[2] = {
                labelLeft + EVENT_SLOT_X_FROM_LABEL_TOP,
                labelTop  + SLOT_Y_FROM_LABEL_TOP,
            };
            slot.slotPtr->setPosition(slotXY);
        }

        slot.titleText.posX = labelLeft + TEXT_X_FROM_LABEL_TOP;
        slot.titleText.posY = labelTop  + TITLE_Y_FROM_LABEL_TOP;

        // descText[d] offsets re-base from labelTop each iter; the
        // binary re-fetches topY via getLeftX inside the loop instead
        // of accumulating. result: lines are spaced by 25px (DESC_LINE_STEP_PX
        // increment of (d*25 + 72)/640), with the first line at +72px.
        for (int d = 0; d < 3; ++d) {
            slot.descText[d].posX = labelLeft + TEXT_X_FROM_LABEL_TOP;
            slot.descText[d].posY = labelTop
                                  + ((float)d * DESC_LINE_STEP_PX + DESC_FIRST_LINE_Y_PX)
                                      / UV_PIXEL_PER_UNIT;
        }

        // iconQuad UV + size + position by EventKey.
        if (slot.slotPtr) {
            uint32_t key = slot.slotPtr->getEventTypeKey();
            float iconNudge = 0.0f;

            switch (key) {
                case 0:   // Experience
                    slot.iconQuad.setTexCoords(0.47558594f, 0.01953125f,
                                               0.51269531f, 0.06542969f);
                    slot.iconQuad.setSize(0.059375f, 0.0734375f);
                    iconNudge = ICON_X_NUDGE_EXPERIENCE;
                    break;

                case 1:   // Health
                    slot.iconQuad.setTexCoords(0.42968750f, 0.01953125f,
                                               0.47460938f, 0.06445313f);
                    slot.iconQuad.setSize(0.071875f, 0.071875f);
                    iconNudge = ICON_X_NUDGE_HEALTH;
                    break;

                case 2:   // Attack
                    slot.iconQuad.setTexCoords(0.18164063f, 0.0f,
                                               0.22851563f, 0.04492188f);
                    slot.iconQuad.setSize(0.075f, 0.071875f);
                    iconNudge = ICON_X_NUDGE_ATTACK;
                    break;

                case 3:   // Defence
                    slot.iconQuad.setTexCoords(0.28027344f, 0.0f,
                                               0.32324219f, 0.04492188f);
                    slot.iconQuad.setSize(0.06875f, 0.071875f);
                    iconNudge = ICON_X_NUDGE_DEFENCE;
                    break;

                case 4:   // Control
                    slot.iconQuad.setTexCoords(0.22949219f, 0.0f,
                                               0.27832031f, 0.04492188f);
                    slot.iconQuad.setSize(0.078125f, 0.071875f);
                    iconNudge = ICON_X_NUDGE_CONTROL;
                    break;

                default:
                    iconNudge = 0.0f;
                    break;
            }
            slot.iconQuad.posX = labelLeft + ICON_X_FROM_LABEL_TOP + iconNudge;
            slot.iconQuad.posY = labelTop  + ICON_Y_FROM_LABEL_TOP;
        }

        setSlotSelected(i, false);
    }

    // panel chrome size + anchor + initial alpha.
    const float panelW = snappedMax + PANEL_W_OVERHEAD;
    const float panelH = ((float)slotCount * ROW_STRIDE_PX + PANEL_H_BASE_PX)
                       / UV_PIXEL_PER_UNIT;
    setSizeAndCenterY(panelW, panelH);
    setAlpha(0);   // vtable[3] in binary = setAlpha; fade-in animates back to 0xFF
}

// FUN_10000f66c, EventChoicePanel::close.
//
// hide the panel via Menu::panelHide, dismiss the detail-panel popup,
// and clear the install queue. per-slot EventSlots are not freed here;
// they persist until the next open() (or panel dtor) cleans them up.
void EventChoicePanel::close() {
    panelHide(false);
    heldItemRef->reset(0);
    selectedEventOnConfirm = nullptr;
}

// FUN_10000f6a0, EventChoicePanel::setAlpha.
//
// Menu::setAlpha first (chrome: bgDim half-alpha + title / closeBg /
// confirmButton), then per active slot propagate alpha onto:
//   labelDefault (Label::setAlpha)
//   labelSelected (Label::setAlpha)
//   EventSlot::setAlpha (mainQuad + eventFrame + chargeSlots + chargeMarkers)
//   iconQuad (Quad::setAlpha)
//   titleText (TextItem::setAlpha)
//   descText[0..2] (TextItem::setAlpha)
void EventChoicePanel::setAlpha(uint8_t a) {
    Menu::setAlpha(a);

    for (int i = 0; i < slotCount; ++i) {
        EventChoiceSlot& slot = slots[i];
        slot.labelDefault.setAlpha(a);
        slot.labelSelected.setAlpha(a);

        if (slot.slotPtr) {
            slot.slotPtr->setAlpha(a);
        }

        slot.iconQuad.setAlpha(a);
        slot.titleText.setAlpha(a);

        for (int d = 0; d < 3; ++d) {
            slot.descText[d].setAlpha(a);
        }
    }
}

// FUN_10000f768, EventChoicePanel::onConfirmTapped.
//
// writes the selected slot's EventSlot* into selectedEventOnConfirm.
// gameBoardUpdate polls this field on its next tick; when non-null,
// it allocates a fresh EventSlot via operator_new(0xDD0), copies
// (eventType, currentCharges + magnitude bonus from perk 0x14), calls
// hud.addEventSlot, then calls panel.close() to clear the queue.
void EventChoicePanel::onConfirmTapped() {

    if (selectedSlot < 0 || selectedSlot >= slotCount) {
        return;
    }

    selectedEventOnConfirm = slots[selectedSlot].slotPtr;
}
