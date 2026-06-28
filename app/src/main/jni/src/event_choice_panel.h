#pragma once

#include "event_slot.h"
#include "label.h"
#include "menu.h"
#include "quad.h"
#include "text_item.h"
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

class DetailPanel;
class PlayerSystem;

// reconstructed from Ghidra:
//   init / ctor:        FUN_10000e128   (Menu::init + per-slot widget setup)
//   dtor:               FUN_10000e5a4
//   draw     vtable[2]: FUN_10000e6d4
//   update   vtable[3]: FUN_10000e834
//   setAlpha vtable[4]: FUN_10000f6a0
//   onConfirmTapped[5]: FUN_10000f768
//   setSlotSelected:    FUN_10000eb2c
//   close:              FUN_10000f66c
//
// EventChoicePanel lives at GameBoard.eventChoicePanel (size 0x1510 bytes). it's a
// Menu-derived popup that presents 1..4 candidate Events to the player at
// pick-time (typically Eventful-perk reward, Spirit Drain snag, etc.).
// when the player taps confirm, onConfirmTapped writes the selected slot's
// EventSlot* into `selectedEventOnConfirm`;
// gameBoardUpdate then polls that field, clones the source event via
// operator_new(0xDD0) + EventSlot::init, calls hud.addEventSlot, and
// calls panel.close() to clear the queue.
//
// per-slot stride 0x430 bytes, structurally identical to ItemChoiceSlot:
// EventSlot* slotPtr (owning ptr; freed in panel dtor)
// Label   labelDefault   (atlas (488, 81) chrome)
// Label   labelSelected  (atlas (530, 81) chrome)
// TextItem titleText      (slot.event.getName())
// TextItem descText[0]    (slot.event.getDescriptionLine(0))
// TextItem descText[1]
// TextItem descText[2]    (always "", wide-margin slack)
// Quad     iconQuad       (per-Event icon, drawn under labelSelected)
// total: 0x430 bytes.

class EventChoiceSlot {
public:
    // ---- per-slot layout ----
    EventSlot* slotPtr;            // owning ptr; null = empty slot
    Label      labelDefault;       // "unselected" chrome frame
    Label      labelSelected;      // "selected" chrome frame
    TextItem   titleText;
    TextItem   descText[3];        // (slot 2 always empty)
    Quad       iconQuad;
};

class EventChoicePanel : public Menu {
public:
    // ---- public methods ----

    // FUN_10000ec58. open the event-choice panel. called by GameBoard's
    // end-of-turn pipeline when the player's eventTray has free slots and
    // their Eventful perk grants them more events. rolls 3 (or 4 with the
    // Perceptive perk) random EventKinds, one per EventKey category,
    // excluding kinds already on the tray and kinds dismissed previously
    // (excludedHistory). sets up each slot's title / desc text + icon
    // + chrome label, sizes the panel, and fades in.
    //   sourceEvents: current tray events; their EventKinds are excluded
    //                 from the candidate roll.
    void open(class PlayerSystem& ps,
              const std::vector<EventSlot*>& sourceEvents);

    // FUN_10000e128. panel construction. runs once at GameBoard create.
    // calls Menu::init with the event-choice title atlas (0x281, 0x328,
    // 0x135, 0x30), stores `heldItemRef` for detail-panel popups, then
    // initializes each of the 4 slots' embedded widgets: two Labels +
    // 4 TextItems (title + 3 desc lines) + 1 Quad icon. adds 9-slice
    // glyph data to each Label (6 atlas regions per label), points
    // each TextItem's glyphTablePtr at game's panel-font glyph table,
    // and zero-inits the slotPtr on every slot.
    void init(DetailPanel* heldItemRef);

    // FUN_10000e6d4. draws panel chrome via Menu::draw, then per slot:
    // labelDefault (or labelSelected if this slot is selected),
    // EventSlot::draw (which renders mainQuad + eventFrame + chargeSlots
    // + chargeMarkers per the slot's current state), iconQuad, titleText,
    // descText[0..2]. tex 9 bind for chrome, EventSlot::draw rebinds
    // to tex 12 (items1).
    void draw();

    // FUN_10000e834. per-frame update + touch dispatch. on tap: hit-test
    // EventSlot::contains on each candidate (the slot's actual EventSlot
    // visual is the touch target; labelDefault wraps it). on hit, pops
    // up the detail panel with the event's name + description via
    // FUN_100040124 (mode 1, content-icon variant). on tap of the
    // labelDefault chrome frame, selects that slot (sound 5/7 + dim).
    // on release outside any slot, dismisses the detail-panel popup.
    void update(float dt, float touchInput) override;

    // FUN_10000eb2c. tints slot N's chrome into "selected" / "unselected"
    // state. same color scheme as ItemChoicePanel::setSlotSelected:
    //   titleText: white (unsel) / light-gray (sel)
    //   descText:  faint-pink white (unsel) / faint-pink gray (sel)
    // plus a 0xC8C8C8 / 0xFFFFFFFF tint on the iconQuad behind it.
    void setSlotSelected(int slotIdx, bool selected);

    // FUN_10000f66c. close the panel after confirm: Menu::panelHide,
    // dismiss the detail-panel popup, and clear selectedEventOnConfirm.
    // does not free per-slot EventSlots; those persist until the
    // next open() (or panel dtor) cleans them up.
    void close();

    // vtable[4] override (FUN_10000f6a0). Menu::setAlpha first, then per
    // active slot: labelDefault.setAlpha, labelSelected.setAlpha, the
    // EventSlot's per-Quad setAlpha (mainQuad + eventFrame + chargeSlots
    // + chargeMarkers), iconQuad.setAlpha, titleText.setAlpha, descText
    // [0..2].setAlpha.
    void setAlpha(uint8_t a) override;

    // vtable[5] override (FUN_10000f768). writes the selected slot's
    // EventSlot* into selectedEventOnConfirm. polled by gameBoardUpdate
    // to trigger the EventSlot install path.
    void onConfirmTapped() override;

    // ---- byte-exact field landmarks ----

    // Menu base owns the panel chrome.

    // pointer back to GameBoard::detailPanel; used by update()'s tap path
    // to pop up the event-detail card (event name + descriptions).
    DetailPanel* heldItemRef;

    static constexpr int MAX_SLOT_COUNT = 4;
    EventChoiceSlot slots[MAX_SLOT_COUNT];

    // ---- tail: panel state ----

    // when the player taps confirm with an event selected, the binary
    // writes the selected slot's EventSlot* here. gameBoardUpdate polls
    // this field; when non-null, it allocates a fresh EventSlot, copies
    // (eventType, currentCharges + Eventful-perk magnitude bonus) from
    // the source pointer, calls hud.addEventSlot, then close() clears
    // this field back to null.
    EventSlot* selectedEventOnConfirm;

    int32_t slotCount;                         // (1..4 active candidates)
    int32_t selectedSlot;                      // (-1 = no selection)

    // "rejected-history" set: EventKinds the panel has shown but the
    // player has dismissed. each open() folds the current tray + this
    // history into the exclusion set used by candidate-roll. binary uses
    // a std::set<int> (libc++ aarch64 layout: 16-byte sentinel
    // + 8-byte size = 0x18 bytes, matches exactly).
    //
    // vestigial: the binary reads this set when rolling candidates (folding it
    // into the exclusion set) but no code path ever inserts into it, so it
    // stays empty. our port matches: declared and read, never populated.
    std::set<int> excludedHistory;
};
