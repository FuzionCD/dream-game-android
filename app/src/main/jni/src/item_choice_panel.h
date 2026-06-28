#pragma once

#include "item.h"
#include "label.h"
#include "menu.h"
#include "text_item.h"
#include <cstdint>
#include <vector>

class DetailPanel;
class PlayerSystem;

// reconstructed from Ghidra:
//   open():            FUN_100034d60   (= setup 1..4 item-comparison slots)
//   update / touch:    FUN_100034b30   (per-frame; tween + tap-pick)
//   setSlotSelected:   FUN_100034cc8   (= tint slot's quads on select)
//   pickItemSet:       FUN_100035218   (= generate 1..4 candidate Items)
//   close / cleanup:   FUN_10003568c   (deletes slot[].heldItem + candidateItem)
//
// ItemChoicePanel lives at GameBoard.itemChoicePanel (size 0x1598 bytes). it's
// triggered when the HUD's CTRL marker bank fills 10 markers. when open,
// the panel offers 1..4
// "current vs offered" item comparison slots. the player taps a slot to
// pick the offered item; on confirm, the selected item is cloned into
// playerSystem.baseItems[type] (replacing the held item of the same type).
//
// the binary's internal layout is largely opaque; most of the 0x1598
// bytes are per-slot Labels / TextItems / chrome whose exact field
// breakdown isn't fully reverse-engineered. we type only the landmark
// fields that are read or written by gameplay code outside the panel
// itself, and access internal sub-fields via byte-offset helpers that
// match the binary 1:1.
//
// per-slot stride: 0x458 bytes. slot count: 1..4 (4 when perk 0x17 is owned;
// otherwise 1..3 depending on RNG inside pickItemSet).
//
// each slot has two display halves:
//   - "held"      side: a copy of the player's current item of
//                                  the same type as the candidate (for
//                                  side-by-side comparison).
//   - "candidate" side: the offered item.
//
// the slot owns both Item* pointers; both are allocated via operator_new
// (the candidate is constructed by pickItemSet, the held by open() via
// the Item copy ctor) and freed during close() / next open()'s prologue.

// ---- per-slot layout ----
//
// fields verified against FUN_100034d60 (open) field accesses; trailing
// regions inside each half are likely a Quad icon + chrome we haven't
// fully decoded yet.

class ItemChoiceSlot {
public:

    // ---- side A: HELD item (player's current item of this type) ----
    Item*    heldItem;             // owning ptr; freed at next open()'s prologue
    float    heldPosX;             // computed in open() from heldLabel layout
    float    heldPosY;
    Label    heldLabel;            // 6-glyph hit-test frame, atlas (70, 677)
    Quad     heldDividerQuad;      // separator art between held & candidate,
                                   //                  atlas UV (0.226, 0.576)..+(0.072, 0.211)

    // ---- side B: CANDIDATE item (the offered upgrade) ----
    Item*    candidateItem;        // owning ptr; allocated by pickItemSet
    float    candidatePosX;        // computed from candidateLabel center
    float    candidatePosY;
    Label    candidateLabel;       // 3-glyph hit-test frame, atlas (488, 81)
    Label    candidateBgLabel;     // 3-glyph chrome frame, atlas (530, 81)
    TextItem candidateNameText;    // candidate's getName() text
    TextItem descText[2];          // candidate's 2 description lines
};

class ItemChoicePanel : public Menu {
public:

    // ---- public methods ----

    // FUN_100033fe0, panel construction. runs once at GameBoard creation.
    // calls Menu::init with the item-choice title atlas (641, 857, 269, 48),
    // stores `heldItemRef` so update()'s held-tap can pop up the detail
    // panel for the held item, then initializes the 4 slots' embedded
    // widgets: Label::init on heldLabel / candidateLabel / candidateBgLabel,
    // TextItem::init on candidateNameText + descText[0..1] (with their
    // glyphTablePtr pointed at game's panel-font glyph table). adds the
    // 9-slice glyph data to each Label, sizes/positions all sub-widgets
    // per slot, and configures the heldDividerQuad UVs.
    void init(DetailPanel* heldItemRef);

    // FUN_100034d60. opens the panel: plays the show-sound + show
    // animation, builds the candidate list via pickItemSet, deletes any
    // prior slot.heldItem / candidateItem, allocates a fresh held-item
    // clone per slot, lays out per-slot Labels + TextItems, and kicks
    // vtable[3] for the first frame's update. trigger: gameBoardUpdate
    // when the HUD CTRL bank fills.
    void open(PlayerSystem* playerSystem);

    // FUN_100034b30. per-frame update + touch dispatch. on tap: hit-test
    // the candidate side of each slot; on confirm: latch selectedItem.
    // gameBoardUpdate consumes selectedItem in the commit branch.
    void update(float dt, float touchInput) override;

    // FUN_1000349ac. draws panel chrome via Menu::draw, then per slot:
    // heldLabel + heldDividerQuad, candidateBgLabel (selected) OR
    // candidateLabel (unselected), heldItem icon stack, candidateItem
    // icon stack, candidateNameText, descText[0..1].
    void draw();

    // FUN_10003568c. close the panel after commit: delete each slot's
    // heldItem + candidateItem, clear selectedItem, hide via Menu::panelHide.
    void close();

    // FUN_100034cc8. tints slot N's chrome into "selected" / "unselected"
    // state. called whenever lastSelectedSlot changes.
    void setSlotSelected(int slotIdx, bool selected);

    // FUN_100035218. fills outCandidates with 1..4 fresh Item*s drawn from
    // playerSystem's stat blocks. each candidate is a new Item (operator_new
    // + Item::init) with stats upgraded from the held item of that type +
    // perk 0x15 ability count rolls; subType is filtered to avoid matching
    // the held item's subType. the candidate vector size depends on
    // perk 0x17 + RNG.
    static void pickItemSet(PlayerSystem* playerSystem,
                            std::vector<Item*>& outCandidates);

    // vtable[4] override. propagates alpha onto each active slot's labels
    // + textitems + chrome quads, in addition to the Menu base chrome.
    void setAlpha(uint8_t a) override;

    // vtable[5] override. fires when the player taps + releases over the
    // confirm button. latches a "ready to commit" flag the gameBoardUpdate
    // commit branch reads.
    void onConfirmTapped() override;

    // ---- byte-exact field landmarks ----
    //
    // Menu base class owns the panel chrome: vtable +
    // visible, anchorX/Y, fade state, bgDim Quad, frame9slice Label,
    // titleQuad Quad, closeBg Quad, confirmButton Quad, readyByte,
    // confirmPressed. see menu.h for full layout.

    // pointer back to GameBoard::detailPanel (= the tile-inspect / Item-card
    // tooltip widget at GameBoard.detailPanel). open()/close()/update() call
    // heldItemRef->reset(0) to dismiss any prior tooltip when entering /
    // leaving the item-choice flow, and update()'s held-side tap pops up
    // the detail panel showing the held item card.
    DetailPanel* heldItemRef;

    static constexpr int MAX_SLOT_COUNT = 4;
    ItemChoiceSlot   slots[MAX_SLOT_COUNT];

    // ---- tail: panel state ----

    int32_t          slotCount;                // active slot count (1..4)
    int32_t          lastSelectedSlot;         // -1 = no selection;
                                               //          slot index when user taps
    Item*            selectedItem;             // pointer into slots[].candidateItem
                                               //          consumed by gameBoardUpdate commit branch
};
