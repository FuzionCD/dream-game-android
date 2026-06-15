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
// PauseMenu is the in-game pause / settings popup at GameBoard+0xF488
// (0x1990 bytes). it's a Menu subclass with the following 5 interactable
// settings stacked vertically inside the panel, top to bottom:
//
//   tabs[0] -> "Main Menu" button. release sets GameBoard.scoreRequested = 1,
//             which triggers the case-5 overlay transition back to TitleMenu
//             without opening the post-run ScorePanel.
//   tabs[1] -> "Tutorial" toggle. release XORs the tutorialFlag byte at +0xD9E.
//             GameBoard's consumer clears the hint-state region at +0x63F8
//             when the value changes, so the ambient tutorial hints re-fire.
//   volumeSliders[0] -> SE-volume slider. drag updates linearValue in [0, 1],
//                 which GameBoard copies into seVolume (+0x10). consumed by
//                 Game::dispatchSounds as the per-frame SE gain context.
//   volumeSliders[1] -> BGM-volume slider. same shape; copied into bgmVolume
//                 (+0x14). consumed by Game::update via
//                 MusicController::setTargetVolume.
//   tabs[2] -> "Forfeit" button. release calls forfeitConfirmPanel.open(),
//             which pops up the "Forfeit Run?" Yes/No confirmation. when
//             the player confirms (button 0), exitRequest fires and the run
//             closes via the ScorePanel path.
//
// triggered by HUD publishedState == 2 (player tapped the menu icon on
// the gameplay HUD). FUN_10001ae10's case-2 branch calls
//   FUN_10002f688(gb+0x10, gb+0x14, &pauseMenu, gb+0xC)
// which Menu::panelShow + seeds the two slider values from current
// seVolume / bgmVolume + writes the tutorialFlag byte from GameBoard's
// own copy at +0xC.

// each tab is a Label (the 9-slice button frame) + a Quad (the tab's icon
// glyph). 3 tabs total, top to bottom: Main-Menu, Tutorial, Forfeit.
struct PauseMenuBtn {
    Label   label;      // +0x000..+0x097
    Quad    iconQuad;   // +0x098..+0x16F
};
static_assert(sizeof(PauseMenuBtn) == 0x170,
              "PauseMenuBtn must be exactly 0x170 bytes (Label + Quad).");

// each volume-slider row is 3 Quads (icon track + thumb-bg + thumb) + a
// linearValue float in [0, 1] that GameBoard reads every frame while
// the menu is open. row 0 = SE-volume, row 1 = BGM-volume.
struct PauseMenuInfoRow {
    Quad    quadA;            // +0x000..+0x0D7  full-width slider track + icon
    Quad    quadB;            // +0x0D8..+0x1AF  thumb background
    Quad    quadC;            // +0x1B0..+0x287  thumb foreground (tracks B)
    float   linearValue;      // +0x288  drag-position in [0, 1]. GameBoard
                              //          copies row[0] into seVolume and
                              //          row[1] into bgmVolume per frame.
    float   tailUnknown;      // +0x28C  unidentified second tail slot
};
static_assert(sizeof(PauseMenuInfoRow) == 0x290,
              "PauseMenuInfoRow must be exactly 0x290 bytes (3 Quads + 8B).");

// "Forfeit Run?" confirmation popup, embedded inside PauseMenu at +0xF50
// (= GameBoard+0x103D8). its own vtable lives at PTR_FUN_100074760.
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

    // 2 buttons at +0x420 (stride 0x170, same shape as PauseMenuBtn,
    // Label + iconQuad). iter 0 = "Yes" (Forfeit confirmed),
    // iter 1 = "No" (cancel).
    PauseMenuBtn    buttons[2];                       // +0x420..+0x6FF

    // selectedButton tracks the in-progress button press (-1 = none).
    // open() writes -1; press writes the button index; release reads it.
    int32_t         selectedButton;                   // +0x700

    // confirmResult: open() clears to 0. release on button 0 writes 1
    // (Forfeit confirmed); release on button 1 leaves 0 (cancelled).
    // PauseMenu::update reads this after the popup self-hides to decide
    // whether to flag exitRequest on the parent.
    uint8_t         confirmResult;                    // +0x704
    uint8_t         pad705[3];                        // +0x705..+0x707

    // 9-slice popup submenu BG
    Label           questionLabel;                    // +0x708..+0x79F

    // 3 small decorative icon Quads framing the popup.
    Quad            forfeitTitle;                            // +0x7A0..+0x877
    Quad            warnLine1;                            // +0x878..+0x94F
    Quad            warnLine2;                            // +0x950..+0xA27
};
static_assert(sizeof(ForfeitConfirmPanel) == 0xA28,
              "ForfeitConfirmPanel must be exactly 0xA28 bytes, embedding at "
              "PauseMenu+0xEA0 (= GameBoard+0x10328).");

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

    // 3 tabs at +0x420 (stride 0x170). FUN_10002e838's first loop inits
    // each one (Label + Quad). selectedTabIndex tracks which tab is
    // currently focused (-1 = none), reset by open().
    //   tabs[0] = Main Menu, tabs[1] = Tutorial toggle, tabs[2] = Forfeit.
    PauseMenuBtn    tabs[3];                          // +0x420..+0x86F

    int32_t         selectedTabIndex;                 // +0x870..+0x873
                                                      // (open writes -1)
    uint8_t         pad874[4];                        // +0x874..+0x877

    // 2 info rows at +0x878 (stride 0x290). FUN_10002e838's second loop
    // inits each row's 3 Quads + trailing state.
    //   volumeSliders[0] = SE-volume slider, volumeSliders[1] = BGM-volume slider.
    PauseMenuInfoRow volumeSliders[2];                     // +0x878..+0xD97

    // secondaryTabIndex tracks the in-progress slider drag (-1 = none).
    // open() writes -1; press on a row writes its index; drag reads it
    // to know which row to update; release re-checks and clears.
    int32_t         secondaryTabIndex;                // +0xD98..+0xD9B

    // 3 byte flags read by GameBoard's tick after the menu closes:
    //   +0xD9C -> board.exitRequested = 1     (Main Menu tap -> TitleMenu,
    //                                          no ScorePanel)
    //   +0xD9D -> board.scoreRequested = 1    (Forfeit confirmation tap ->
    //                                          ScorePanel for the run)
    //   +0xD9E -> board.tutorialFlag          (Tutorial on/off; open()'s
    //                                          param_4 seeds this with the
    //                                          current setting so the toggle
    //                                          widget starts on the right
    //                                          value)
    // note: the two byte names look swapped vs. their offsets, but they
    // match GameBoard's own +0x01 / +0x02 (scoreRequested at the lower
    // offset, exitRequested at the higher). naming is keyed off consumer
    // effect (ScorePanel vs. exit-to-title), not raw binary offset order.
    // open() clears both bytes to 0 and writes its byte arg into tutorialFlag.
    uint8_t         exitRequest;                      // +0xD9C
    uint8_t         scoreRequest;                     // +0xD9D
    char            tutorialFlag;                     // +0xD9E  matches
                                                      //         GameBoard's
                                                      //         own `char
                                                      //         tutorialFlag`
                                                      //         at +0x0C.
    uint8_t         padD9F[1];                        // +0xD9F

    // 2 standalone icon Quads for the Tutorial toggle: tutorialUnchecked is
    // drawn when tutorialFlag == 0, tutorialChecked when nonzero. UV + size
    // set in the ctor; positions land just to the right of tabs[1].label.
    Quad            tutorialUnchecked;                  // +0xDA0..+0xE77
    Quad            tutorialChecked;                  // +0xE78..+0xF4F

    // embedded "Forfeit Run?" confirmation popup at +0xF50
    // (= GameBoard+0x103D8).
    ForfeitConfirmPanel  forfeitConfirmPanel;         // +0xF50..+0x1977

    // trailing std::vector<TextItem> at +0x1978. open() clears + rebuilds
    // it per show; the binary's `lVar1 = -0x88` pop loop matches
    // libc++'s vector::pop_back + ~TextItem on each entry. zero-init from
    // GameBoard's memset gives begin=end=cap=nullptr which is the libc++
    // empty-vector representation, so no placement-new is needed.
    std::vector<TextItem>  trailingItems;             // +0x1978..+0x198F
};

static_assert(sizeof(PauseMenu) == 0x1990,
              "PauseMenu must be exactly 0x1990 bytes, matching the "
              "GameBoard+0xF488..+0x10E18 region per FUN_1000437a4's alloc.");
