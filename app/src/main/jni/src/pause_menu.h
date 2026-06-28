#pragma once

#include "label.h"
#include "menu.h"
#include "quad.h"
#include "text_item.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// reconstructed from Ghidra:
//   PauseMenu ctor:           FUN_10002e838
//   PauseMenu open:           FUN_10002f688
//   PauseMenu vtable:         PTR_FUN_1000745a0 -> 6 entries
//   panelHide-on-level:       FUN_10002f72c  (= Menu::panelHide(this, 1))
//   ForfeitConfirmPanel ctor: FUN_100042cf4
//   ForfeitConfirmPanel open: FUN_1000435d0
//   ForfeitConfirmPanel vt:   PTR_FUN_100074760
//
// PauseMenu is the in-game pause / settings popup at GameBoard.pauseMenu
// (0x1990 bytes). it's a Menu subclass with the following 5 interactable
// settings stacked vertically inside the panel, top to bottom:
//
//   tabs[0] -> "Main Menu" button. release sets GameBoard.scoreRequested = 1,
//             which triggers the case-5 overlay transition back to TitleMenu
//             without opening the post-run ScorePanel.
//   tabs[1] -> "Tutorial" toggle. release XORs the tutorialFlag byte.
//             GameBoard's consumer clears the hint-state region
//             when the value changes, so the ambient tutorial hints re-fire.
//   volumeSliders[0] -> SE-volume slider. drag updates linearValue in [0, 1],
//                 which GameBoard copies into seVolume. consumed by
//                 Game::dispatchSounds as the per-frame SE gain context.
//   volumeSliders[1] -> BGM-volume slider. same shape; copied into bgmVolume
//                 & consumed by Game::update via MusicController::setTargetVolume.
//   tabs[2] -> "Forfeit" button. release calls forfeitConfirmPanel.open(),
//             which pops up the "Forfeit Run?" Yes/No confirmation. when
//             the player confirms (button 0), exitRequest fires and the run
//             closes via the ScorePanel path.
//
// triggered by HUD publishedState == 2 (player tapped the menu icon on
// the gameplay HUD). FUN_10001ae10's case-2 branch calls
//   FUN_10002f688(GameBoard.seVolume, GameBoard.bgmVolume, &pauseMenu, GameBoard.tutorialFlag)
// which Menu::panelShow + seeds the two slider values from current
// seVolume / bgmVolume + writes the tutorialFlag byte from GameBoard's
// own copy.

// each tab is a Label (the 9-slice button frame) + a Quad (the tab's icon
// glyph). 3 tabs total, top to bottom: Main-Menu, Tutorial, Forfeit.
struct PauseMenuBtn {
    Label   label;
    Quad    iconQuad;
};

// each volume-slider row is 3 Quads (icon track + thumb-bg + thumb) + a
// linearValue float in [0, 1] that GameBoard reads every frame while
// the menu is open. row 0 = SE-volume, row 1 = BGM-volume.
struct PauseMenuInfoRow {
    Quad    quadA;            // full-width slider track + icon
    Quad    quadB;            // thumb background
    Quad    quadC;            // thumb foreground (tracks B)
    float   linearValue;      // drag-position in [0, 1]. GameBoard
                              //          copies row[0] into seVolume and
                              //          row[1] into bgmVolume per frame.
    float   tailUnknown;      // unidentified second tail slot
};

// "Forfeit Run?" confirmation popup, embedded inside PauseMenu. its own
// vtable lives at PTR_FUN_100074760.
//
// 2 buttons (Yes / No), the question label, and 3 small decorative icons.
// the player's tap on "Yes" sets confirmResult = 1, which the parent
// PauseMenu sees on close and re-flags into board.exitRequested so the
// run ends via the ScorePanel path.
class ForfeitConfirmPanel : public Menu {
public:
    // FUN_100042cf4. Menu chrome + the 9-slice question label + 3 decorative
    // icons + the 2 Yes/No buttons (each a Label + iconQuad).
    void init();

    // FUN_1000435d0. clears selectedButton (-1) + confirmResult (=0), then
    // calls panelShow + primes one update tick at dt=0.
    void open();

    // FUN_100043328. chrome + buttons + question + 3 icons under the
    // panel's translate context. non-virtual; called directly by parent's
    // draw().
    void draw();

    // vtable[3], FUN_1000433c4. press hit-tests buttons; release sets
    // confirmResult = (selectedButton == 0 ? 1 : 0), then panelHide.
    void update(float dt, float touchInput) override;

    // vtable[4]. chrome fade + button labels/icons + question label +
    // 3 icons fade.
    void setAlpha(uint8_t a) override;

    // vtable[5]. empty in the binary; button taps handled in update().
    void onConfirmTapped() override {}

    // ---- byte-exact field layout (0xA28 bytes including Menu base) ----

    // 2 buttons (stride 0x170, same shape as PauseMenuBtn,
    // Label + iconQuad). iter 0 = "Yes" (Forfeit confirmed),
    // iter 1 = "No" (cancel).
    PauseMenuBtn    buttons[2];

    // selectedButton tracks the in-progress button press (-1 = none).
    // open() writes -1; press writes the button index; release reads it.
    int32_t         selectedButton;

    // confirmResult: open() clears to 0. release on button 0 writes 1
    // (Forfeit confirmed); release on button 1 leaves 0 (cancelled).
    // PauseMenu::update reads this after the popup self-hides to decide
    // whether to flag exitRequest on the parent.
    uint8_t         confirmResult;

    // 9-slice popup submenu BG
    Label           questionLabel;

    // 3 small decorative icon Quads framing the popup.
    Quad            forfeitTitle;
    Quad            warnLine1;
    Quad            warnLine2;
};

class PauseMenu : public Menu {
public:
    // FUN_10002e838, PauseMenu construction.
    // sets up Menu chrome, the 3 tabs (Label + icon Quad each), the 2 info
    // rows (3 Quads + scale feed), 2 standalone Tutorial-toggle Quads, and
    // chains to ForfeitConfirmPanel::init(). called once at GameBoard
    // construction time.
    void init();

    // FUN_10002f688. opens the pause menu seeded with current seVolume /
    // bgmVolume slider values + the GameBoard's tutorialFlag byte for the
    // Tutorial toggle. clears scoreRequest / exitRequest, resets selected
    // tab + row indices to -1, hides the Forfeit popup, drops any leftover
    // trailing-vector entries, and primes one update tick.
    void open(float seVolumeSeed, float bgmVolumeSeed, char tutorialModeSeed);

    // FUN_10002f0cc. render the menu chrome + tabs + (one of two)
    // standalone Tutorial icon + 2 volume slider rows + Forfeit popup +
    // trailing items. non-virtual override; called directly via PauseMenu*.
    void draw();

    // vtable[3], FUN_10002f1d0. drives the fade animation (via
    // Menu::baseUpdate) and the touch state machine: press hit-tests
    // tabs / rows, drag slides info rows, release fires tab actions.
    // when the Forfeit popup is open, delegates to its update + closes
    // the parent menu when the user confirms.
    void update(float dt, float touchInput) override;

    // vtable[4], FUN_10002f734. fades chrome + all 3 tab labels + their
    // icon Quads + both standalone Tutorial Quads + all 6 slider-row Quads.
    void setAlpha(uint8_t a) override;

    // vtable[5], FUN_10002fa04. empty in the binary; confirm-button
    // taps are handled inside update() instead.
    void onConfirmTapped() override {}

    // ---- byte-exact field layout (0x1990 bytes including Menu base) ----

    // 3 tabs (stride 0x170). FUN_10002e838's first loop inits
    // each one (Label + Quad). selectedTabIndex tracks which tab is
    // currently focused (-1 = none), reset by open().
    //   tabs[0] = Main Menu, tabs[1] = Tutorial toggle, tabs[2] = Forfeit.
    PauseMenuBtn    tabs[3];

    int32_t         selectedTabIndex;
                                                      // (open writes -1)

    // 2 info rows (stride 0x290). FUN_10002e838's second loop
    // inits each row's 3 Quads + trailing state.
    //   volumeSliders[0] = SE-volume slider, volumeSliders[1] = BGM-volume slider.
    PauseMenuInfoRow volumeSliders[2];

    // secondaryTabIndex tracks the in-progress slider drag (-1 = none).
    // open() writes -1; press on a row writes its index; drag reads it
    // to know which row to update; release re-checks and clears.
    int32_t         secondaryTabIndex;

    // 3 byte flags read by GameBoard's tick after the menu closes:
    // - board.exitRequested = 1     (Main Menu tap -> TitleMenu,
    //                                  no ScorePanel)
    // - board.scoreRequested = 1    (Forfeit confirmation tap ->
    //                                  ScorePanel for the run)
    // - board.tutorialFlag          (Tutorial on/off; open()'s
    //                                  param_4 seeds this with the
    //                                  current setting so the toggle
    //                                  widget starts on the right
    //                                  value)
    // note: the two byte names look swapped relative to GameBoard's own
    // pair (where scoreRequested comes before exitRequested). naming is
    // keyed off consumer effect (ScorePanel vs. exit-to-title), not raw
    // declaration order.
    // open() clears both bytes to 0 and writes its byte arg into tutorialFlag.
    uint8_t         exitRequest;
    uint8_t         scoreRequest;
    char            tutorialFlag;                     // matches
                                                      //         GameBoard's
                                                      //         own `char
                                                      //         tutorialFlag`.

    // 2 standalone icon Quads for the Tutorial toggle: tutorialUnchecked is
    // drawn when tutorialFlag == 0, tutorialChecked when nonzero. UV + size
    // set in the ctor; positions land just to the right of tabs[1].label.
    Quad            tutorialUnchecked;
    Quad            tutorialChecked;

    // embedded "Forfeit Run?" confirmation popup.
    ForfeitConfirmPanel  forfeitConfirmPanel;

    // trailing std::vector<TextItem>. open() clears + rebuilds
    // it per show; the binary's `lVar1 = -0x88` pop loop matches
    // libc++'s vector::pop_back + ~TextItem on each entry. zero-init from
    // GameBoard's memset gives begin=end=cap=nullptr which is the libc++
    // empty-vector representation, so no placement-new is needed.
    std::vector<TextItem>  trailingItems;
};
