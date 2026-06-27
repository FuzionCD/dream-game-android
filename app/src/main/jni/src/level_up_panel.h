#pragma once

#include "color_tint.h"
#include "label.h"
#include "menu.h"              // Menu base class (chrome + fade animation)
#include "perk.h"
#include "text_item.h"
#include "tile_content.h"      // TileContent (stat-slot icon)
#include "title_menu.h"        // TileIcon
#include <list>

class PlayerSystem;

// reconstructed from Ghidra:
//   open():            FUN_10002d278   (= setup 3 stat slots + 3 perk slots)
//   update / touch:    FUN_10002ce9c   (= per-frame animation + tap-pick)
//   draw():            (no single function; called via vtable[2] inherited)
//   setSlotSelected:   FUN_10002d0f8   (= tint slot's quads on select)
//   pickPerkSet:       FUN_10002d620   (= generate 3 random perkTypes)
//   getNextStatPick:   FUN_10002db34   (= drain stat picks from selected list)
//   getNextPerkPick:   FUN_10002dbe0   (= drain perk picks from selected list)
//   close / cleanup:   FUN_10002da0c   (= clears finalized byte)
//   show(byte):        FUN_100057ac8   (= visible + animTimer = 0 + audio cue)
//   hide(byte):        FUN_100057b1c   (= clear visible OR clear sub-flags)
//
// LevelUpPanel lives at GameBoard+0xC7B8 (size 0x1738 bytes). it's
// triggered when GameBoard+0x7FD8 (= HUD+0x1BC0) is set by the HUD's XP
// marker bank filling 10 markers (1 level = 1 cycle of 10). when open, the
// panel offers 3 stat picks (ATK / HP / DEF +N) and 3 perk picks
// (random perkTypes drawn from PerkPool by perkLevel(0xE) gating). the
// player taps 1 or 2 options; on commit, the selected picks drain back
// through getNextStatPick / getNextPerkPick into the gameBoardUpdate
// commit loop which mutates playerSystem (stat bumps + addOrUpgradePerk).
//
// the binary's internal layout is largely opaque to our port; most of
// the 0x1738 bytes are visual sub-objects (Quads / TileIcons / ColorTints /
// TextItems) whose binary stride doesn't cleanly factor. we type only
// the landmark fields that are read or written by gameplay code outside
// the panel itself, and access internal sub-fields via byte-offset
// helpers that match the binary 1:1.
//
// per-slot stride:
//   stat slot N (N in 0..2): starts at panel + 0x420 + N * 0x2E8
//   perk slot N (N in 0..2): starts at panel + 0xCD8 + N * 0x360
//
// the selected-picks linked list (+0x1718..+0x172F) holds 0x18-byte nodes
// of (prev, next, slotIdx) where slotIdx is 0/1/2 = stat slot or
// 3/4/5 = perk slot.
//
// the panel inherits from Menu (menu.h), the binary's shared base class
// providing the 0x420-byte chrome header (bgDim, frame9slice, titleQuad,
// closeBg, confirmButton + fade-state). subclass content (stat/perk slots,
// picks lists, etc.) starts at +0x420.

// ---- per-slot layout structs ----
//
// each stat slot is 0x2E8 bytes; each perk slot is 0x360 bytes. each
// slot contains two Label widgets (0x98 bytes each) plus the
// stat-specific TileIcon / ColorTints or the perk-specific Perk* +
// TextItems.

struct LevelUpStatSlot {
    TileContent  icon;            // +0x000..+0x177  stat icon (TileContent). init'd lightweight
                                  //                  via FUN_10001467c (no setType / no magnitude),
                                  //                  then setType(2|6|3) inside LevelUpPanel::init.
                                  //                  setPosition placed via label0 centroid each
                                  //                  ctor pass.
    int32_t      value;           // +0x178          stat magnitude (= +0x598 globally)
    uint8_t      pad17C[4];       // +0x17C..+0x17F
    Label        label0;          // +0x180..+0x217  primary hit-test / 9-slice frame (y=124)
    Label        label1;          // +0x218..+0x2AF  secondary 9-slice frame (y=123)
    ColorTint    numberTint;      // +0x2B0..+0x2E7  the "+N" number color tint
};
static_assert(sizeof(LevelUpStatSlot) == 0x2E8, "LevelUpStatSlot stride");

struct LevelUpPerkSlot {
    Perk*         perk;            // +0x000  preview Perk allocated by open()
    float         posX;            // +0x008
    float         posY;            // +0x00C
    Label  label0;          // +0x010..+0x0A7  primary hit-test / cost text
    Label  label1;          // +0x0A8..+0x13F  secondary text
    TextItem      nameText;        // +0x140..+0x1C7  perk name display
    TextItem      descText[3];     // +0x1C8..+0x35F  3 effect-description lines
};
static_assert(sizeof(LevelUpPerkSlot) == 0x360, "LevelUpPerkSlot stride");

class LevelUpPanel : public Menu {
public:

    // ---- public methods ----

    // FUN_10002cbbc, dtor. frees the 3 preview Perk objects the slots own
    // (open() heap-allocates them); the rest of the panel is value members.
    ~LevelUpPanel();

    // FUN_10002c230, MenuLevel ctor. runs once at GameBoard construction time.
    // calls Menu::init with title atlas (641, 906, 251, 48), then initializes
    // the 6 slots' sub-objects, sets up each stat icon's content type
    // (ATK/HP/DEF), and lays out each perk slot's 9-slice chrome + TextItem
    // glyph-table/scale wires. finishes with Menu::setSize + setAnchorY.
    void init();

    // FUN_10002d278, open the level-up panel. allocates 3 fresh Perk
    // objects for the perk slots (preview of result), generates a random
    // 3-perk set via pickPerkSet, sets up TextItems + Quads + ColorTints
    // for each slot, then primes the visibility flag.
    void open(PlayerSystem* playerSystem);

    // FUN_10002ce9c, per-frame update + touch dispatch. calls Menu::baseUpdate
    // first (fade animation + confirm-button tap), then early-out if not visible
    // or anim not settled. on tap: hit-test the 3 stat slots, then the 3 perk
    // slots. on hit: either toggle the existing pick (re-tap) or add it.
    // vtable[3] override.
    void update(float dt, float touchInput) override;

    // draws all 6 slot visuals.
    void draw();

    // FUN_10002d0f8, tint slot N (0..5) into "selected" (param_3 != 0)
    // or "unselected" (param_3 == 0) color state.
    void setSlotSelected(int slotIdx, bool selected);

    // FUN_10002db34, drain the next stat pick. returns 0 if no more
    // stat picks remain; otherwise removes the entry from the list and
    // writes the stat magnitude into *outVal, returning the stat-type
    // code: 2 (ATK), 3 (DEF), or 6 (HP).
    int getNextStatPick(int* outVal);

    // FUN_10002dbe0, drain the next perk pick. returns nullptr if no
    // perk picks remain; otherwise removes the entry from the list and
    // returns the Perk* pointer stored in the matching perk slot.
    Perk* getNextPerkPick();

    // FUN_10002da0c, close the panel after the gameBoardUpdate commit
    // loop has drained all picks. clears the "ready-to-commit" flag at
    // panel+0x1730 and hides via FUN_100057b1c(panel, 0).
    void close();

    // FUN_10002d620, fill outPerkTypes with 3 distinct perkTypes drawn
    // from the runtime perk pool, biased across cost tiers and excluding
    // perks already at max level. consumed by open() to populate the 3
    // perk slot previews.
    static void pickPerkSet(PlayerSystem* playerSystem, int outPerkTypes[3]);

    // vtable[4] override. fades chrome (via Menu::setAlpha) and also propagates
    // alpha onto every stat-slot icon/label/numberTint and every perk-slot
    // label/nameText/descText. note: confirm button + perk icons not touched.
    void setAlpha(uint8_t a) override;

    // vtable[5] override. fires when the player taps + releases over the
    // confirm button while readyByte is set. latches readyToCommit = 1,
    // signaling gameBoardUpdate to drain the picks and close the panel.
    void onConfirmTapped() override;

    // ---- byte-exact field landmarks ----
    //
    // Menu base class (+0x000..+0x41F) owns the panel chrome: vtable +
    // visible, anchorX/Y, fade state, bgDim Quad, frame9slice Label,
    // titleQuad Quad, closeBg Quad, confirmButton Quad, readyByte,
    // confirmPressed. see menu.h for full layout.

    // ---- stat slots (+0x420..+0xCD7) ----
    static constexpr int STAT_SLOT_COUNT = 3;
    LevelUpStatSlot statSlots[STAT_SLOT_COUNT];   // +0x420..+0xCD7

    // ---- perk slots (+0xCD8..+0x16F7) ----
    static constexpr int PERK_SLOT_COUNT = 3;
    LevelUpPerkSlot perkSlots[PERK_SLOT_COUNT];   // +0xCD8..+0x16F7

    // ---- tail (+0x16F8..+0x1737): two picks lists + flags ----
    //
    // the panel maintains two parallel std::list<int> instances of picked
    // slot indices. libc++ aarch64's list layout is (end_.prev, end_.next,
    // size) = exactly 0x18 bytes, matching the binary's per-list layout
    // 1:1. each list node is 0x18 bytes (prev, next, int + pad), also
    // matching libc++.
    //
    // slotIdx values: 0..2 = stat slots, 3..5 = perk slots.
    //
    //   picksList: the authoritative list of slots the player has tapped.
    //   update()'s touch handler push_back's here, runs a std::find dup
    //   check before adding.
    //
    //   drainList: a parallel mirror, kept in sync via assign() on every
    //   tap. consumed by getNextStatPick / getNextPerkPick during commit;
    //   draining picks does not touch picksList (it stays as-is until
    //   close()).

    uint8_t      cachedAlpha;          // +0x16F8  setAlpha (vtable[4]) writes
                                       // the eased alpha byte here every
                                       // fade frame; cleared to 0 at open
                                       // so the fade-in starts transparent.
    uint8_t      allowTwoFromCategory; // +0x16F9  set by open() from
                                       // perkLevel(0xE) (perk 0x0E:
                                       // "Can choose 2 stats or 2 perks").
                                       // 0 = pick one from each category;
                                       // 1 = pick any 2 (or 1 of each).
    uint8_t      pad16FA[6];           // +0x16FA..+0x16FF

    std::list<int> picksList;          // +0x1700..+0x1717
    std::list<int> drainList;          // +0x1718..+0x172F

    uint8_t      readyToCommit;        // +0x1730  set when player taps confirm; consumed by gameBoardUpdate
    uint8_t      pad1731[7];           // +0x1731..+0x1737
};

static_assert(sizeof(LevelUpPanel) == 0x1738,
              "LevelUpPanel must be exactly 0x1738 bytes (= GameBoard+0xDEF0 - GameBoard+0xC7B8)");
