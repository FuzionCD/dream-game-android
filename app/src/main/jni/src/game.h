#pragma once

#include <GLES/gl.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "achievement_tracker.h"
#include "achievements_menu.h"
#include "bmfont_table.h"
#include "leaderboard_menu.h"
#include "music_controller.h"
#include "overlay.h"
#include "quad.h"
#include "save_buffers.h"
#include "score_history.h"
#include "score_panel.h"
#include "shop_menu.h"
#include "sound_queue.h"
#include "title_menu.h"
#include "world.h"

class GameBoard;

// the game's monolithic state struct, reconstructed from Ghidra decompilation.
// total size: 0x2E860 bytes (190,560 bytes).
//
// in the original binary this is one contiguous allocation. our port models
// the layout as a sequence of typed C++ fields with explicit `uint8_t`
// padding arrays bridging the unclassified gaps, the same pattern GameBoard
// already uses. landmark `static_assert(offsetof(...) == ...)` lines guard
// against drift when sub-systems gain or lose fields.
//
// auxiliary subsystems (SoundQueue, MusicController, Overlay) that aren't
// part of the original binary struct live OUTSIDE the typed-fields block in
// the public section. they were extracted for cleaner code and don't need
// fixed offsets.
//
// all binary offsets documented in REVERSE_ENGINEERING_NOTES.md.

#define GAME_STRUCT_SIZE 0x2E860

// texture id mapping: the ios version loads textures via GLKTextureLoader
// which assigns sequential GL texture ids starting from 1.
// we replicate this by loading in the same order and storing the GL ids.
#define GAME_NUM_TEXTURES 13

class Game {
public:
    // allocate and zero-initialize the game struct
    static Game* create();

    // clean up and free
    void destroy();

    // FUN_100045250: seed rng, init subsystems, set initial state
    void init();

    // FUN_100045410: main update tick
    void update(float dt);

    // FUN_100046000: draw all subsystems
    void draw();

    // set the virtual screen height (computed from aspect ratio by renderer)
    void setVirtualHeight(float vh);

    // FUN_10004612c: touch began (normalized coords 0-1)
    void touchBegan(float normX, float normY);

    // FUN_100046168: touch moved (normalized coords 0-1)
    void touchMoved(float normX, float normY);

    // FUN_10004619c: touch ended
    void touchEnded();

    // Android back button. no binary equivalent (iOS has no back button).
    // routes the press to the frontmost screen: in-game delegates to
    // GameBoard::handleBackPressed; sub-screens (shop / leaderboard /
    // achievements) request their close-to-title; character select returns
    // to title; the title screen quits. returns true only when the app
    // should quit.
    [[nodiscard]] bool handleBackPressed();

    // load all game textures in the correct order
    bool loadTextures();

    // load all known sound effects
    bool loadSounds();

    // load all 3 BMFont glyph tables (font / fontClean / fontWorld) into the
    // embedded BMFontTable instances at +0x10 / +0x1220 / +0x2430.
    // mirrors the iOS viewDidLoad font-load loop (FUN_1000461b4).
    bool loadFonts();

    // dispatch triggered sounds from the queue to the audio engine
    void dispatchSounds();

    // FUN_10004535c. resetFade then pick the active music track based on
    // which top-level subsystem is visible:
    //   - scorePanel.visible -> no track (silent during post-run)
    //   - titleMenu.visible  -> setTrack(0)
    //   - boardPtr.visible   -> push tracks 1..N-1 (gameplay layered stack)
    // followed by applyToAudio. called by every transitionTarget execute
    // path that swaps visible subsystems.
    void syncMusic();

    // ---- public accessors (forwards into the private typed binary fields) ----

    int&        gameState()     { return gameState_; }
    int&        inputState()    { return inputState_; }
    float&      touchX()        { return touchX_; }
    float&      touchY()        { return touchY_; }
    float&      scaleFactor()   { return scaleFactor_; }
    Quad&       letterboxQuad1() { return letterboxQuad1_; }
    Quad&       letterboxQuad2() { return letterboxQuad2_; }

    // the 5 per-slot save scratch buffers sit at non-contiguous offsets, so
    // they're separate members; this maps a slot index to its buffer.
    std::vector<uint8_t>& saveScratch(int slot) {
        switch (slot) {
            case 0:  return saveSlot0Scratch_;
            case 1:  return saveSlot1Scratch_;
            case 2:  return saveSlot2Scratch_;
            case 3:  return saveSlot3Scratch_;
            default: return saveSlot4Scratch_;
        }
    }

    // achievement state at +0x42F8. not audio: the only ported method,
    // beginSession, is called from initLevel and looks like a music-mode
    // toggle but isn't.
    AchievementTracker& achievementTracker() { return achievementTracker_; }

    // title-screen subsystem at +0x4460 (embedded inline; the binary's
    // GameBoard typename here is `TitleMenu` since it owns the title +
    // character + difficulty UI before the gameplay GameBoard takes over).
    TitleMenu&  titleMenu()         { return titleMenu_; }

    // world / character-select subsystem at +0x16DB0.
    World&      world()         { return world_; }

    // pointer to the gameplay GameBoard at +0x19120 (separately allocated
    // via operator_new; only the pointer lives here).
    GameBoard*& boardPtr()      { return boardPtr_; }

    // pointer to the post-run score panel at +0x19128 (heap-allocated via
    // operator_new(0x1018) in Game::create).
    ScorePanel*& scorePanel()   { return scorePanel_; }

    // between-runs shop backend at +0x19130. holds the persistent keys
    // balance + the 3-row unlock UI.
    Shop&            shop()             { return shop_; }

    // leaderboard viewer header at +0x1CB18. T=8 set site reads its
    // closeRequested flag. full per-difficulty rank UI is ported.
    LeaderboardMenu& leaderboardMenu()  { return leaderboardMenu_; }

    // per-difficulty stat-history at game+0x234C0 (= LeaderboardMenu+0x69A8).
    // FUN_100037d3c inserts each run on game-over.
    ScoreHistory&    scoreHistory()     { return scoreHistory_; }

    // achievements page at +0x23508. T=9 set site reads its closeRequested
    // flag.
    AchievementsMenu& achievementsMenu(){ return achievementsMenu_; }

    // post-overlay transition target at +0x2E170. set by every transition
    // initiator (titleMenu's startButton / indicators / etc., PauseMenu's
    // Main-Menu and Forfeit-confirm tails, ScorePanel close). read once per
    // frame in Game::update's switch when the overlay finishes opening.
    int& transitionTarget() { return transitionTarget_; }

    // screen-transition curtain at Game+0x2E178 (drives the fade/curtain
    // between title / board / score). the binary's in-struct overlay.
    Overlay& overlay() { return overlay_; }

    // global SE / BGM volume settings (overridden by GameBoard's own
    // seVolume / bgmVolume when gameplay is active).
    float& globalSeVolume()  { return globalSeVolume_; }
    float& globalBgmVolume() { return globalBgmVolume_; }

    // ===== persistence accessors =====
    // see save_buffers.h for the 5-slot architecture.

    // slot 0 ("sav"): mid-run GameSnapshot
    GameSnapshot& gameSnapshot()       { return gameSnapshot_; }
    uint32_t& saveSlot0Dirty()         { return saveSlot0Dirty_; }
    uint64_t& hasSavedRun()            { return hasSavedRun_; }

    // slot 1 ("set"): settings group (tutorial, difficulty, volumes)
    bool& saveSlot1Dirty()             { return saveSlot1Dirty_; }
    int32_t& settingsMagic()           { return settingsMagic_; }
    bool& tutorialFlag()               { return tutorialFlag_; }

    // slot 2 ("unl"): shop unlocks save buffer. fed by Shop::dirtyXfer
    // (FUN_100054298) when shop.dirty is set after a run, consumed by
    // Shop::restoreFromSave (FUN_100054338) on game-init.
    PersistentUnlocks& shopSaveBuffer() { return shopSaveBuffer_; }
    bool& shopSaveDirty()               { return shopSaveDirty_; }

    // slot 3 ("sco"): leaderboard XferEntry list
    int32_t& leaderboardSaveMagic()    { return leaderboardSaveMagic_; }
    std::vector<LeaderboardMenu::XferEntry>& leaderboardSaveBuffer() {
        return leaderboardSaveBuffer_;
    }
    bool& leaderboardSaveDirty()       { return leaderboardSaveDirty_; }

    // slot 4 ("ach"): AchievementTracker state mirror
    AchievementSaveBuffer& achievementSaveBuffer() {
        return achievementSaveBuffer_;
    }
    bool& saveSlot4Dirty()             { return saveSlot4Dirty_; }

    // cached worldIndex (= difficulty 0/1/2) written by Level::generate
    // tail and read by case 0 (transitionTarget=title->character-select)
    // as the World::generate arg. lets the back-to-character-select
    // transition preserve which difficulty was last played.
    int& stashedDifficulty() { return stashedDifficulty_; }

    // typed accessors for the 3 BMFont glyph tables.
    BMFontTable& bmfontTable(int idx) { return bmfontTables_[idx]; }

    // glyph-table pointer for handing to TextItem (stored in its glyphTablePtr).
    const BMFontTable* bmfontTablePtr(int idx) {
        return &bmfontTables_[idx];
    }

private:
    // ===== binary-faithful Game struct layout =====
    //
    // total: 0x2E860 bytes. mirrors the iOS allocation byte-for-byte.
    // every named field has a binary-confirmed offset. uint8_t padding
    // arrays use compile-time-computed sizes so any drift in sub-class
    // sizes (TitleMenu / World / AchievementTracker / Quad) surfaces
    // as a negative-array-size compile error.

    // ---- core state (head) ----
    int          gameState_;             // +0x00000
    int          inputState_;            // +0x00004
    float        touchX_;                // +0x00008
    float        touchY_;                // +0x0000C

    // ---- font glyph tables ----
    // 3 BMFont tables populated by loadFonts():
    //   [0] panel font   (font.fnt)       used by DetailPanel
    //   [1] dialog font  (fontClean.fnt)  used by DialogPanel
    //   [2] world font   (fontWorld.fnt)  used by world / level-select
    BMFontTable  bmfontTables_[3];       // +0x00010..+0x03640

    uint8_t      _gap_3640_to_36A0[0x60];

    // ---- animation timing constants (Game::init writes from FUN_100045250) ----
    float        animTiming_005a_;       // +0x036A0  init = 0.005f
    float        animTiming_035_;        // +0x036A4  init = 0.035f
    float        animTiming_08a_;        // +0x036A8  init = 0.08f
    float        animTiming_08b_;        // +0x036AC  init = 0.08f

    uint8_t      _gap_36B0_to_36C8[0x18];

    float        scaleFactor_;           // +0x036C8

    uint8_t      _gap_36CC_to_36D0[0x36D0 - 0x36CC];

    // ---- letterbox quads (renderer.cpp owns init/draw) ----
    // each Quad is 0xD8 bytes (including its trailing animation target rect,
    // which letterbox quads never use). at 0xD8, letterboxQuad1_ runs
    // +0x036D0..+0x037A8 exactly, so the two quads sit back-to-back with
    // no gap between them.
    Quad         letterboxQuad1_;        // +0x036D0..+0x037A7
    Quad         letterboxQuad2_;        // +0x037A8..+0x03880 (= +0x37A8 + 0xD8)

    uint8_t      _gap_3880_to_42F8[0x42F8 - (0x37A8 + sizeof(Quad))];

    // ---- achievement state ----
    // 0x168 bytes; exactly fills the slot from +0x42F8 to +0x4460 with no gap.
    AchievementTracker  achievementTracker_;     // +0x042F8

    // ---- title menu (title screen + character + difficulty UI) ----
    TitleMenu    titleMenu_;             // +0x04460
    uint8_t      _gap_titleMenu_to_world[0x16DB0 - (0x4460 + sizeof(TitleMenu))];

    // ---- world / character-select state ----
    // World is sized exactly 0x2370 bytes (asserted in world.h), filling the
    // allotted slot from +0x16DB0 to +0x19120 with no gap. if World ever
    // changes size, the `offsetof(Game, boardPtr_) == 0x19120` assert below
    // catches the drift.
    World        world_;                 // +0x16DB0..+0x19120

    // ---- gameplay GameBoard pointer (allocated separately) ----
    GameBoard*   boardPtr_;              // +0x19120

    // ---- post-run score panel pointer (allocated separately) ----
    // heap-allocated via operator_new(0x1018) in Game::create. holds the
    // post-run "And then I wake up" panel state. opens via FUN_100010990
    // (= ScorePanel::open) when the player dies or completes a run.
    ScorePanel*  scorePanel_;            // +0x19128

    // ---- between-runs shop backend ----
    // persistent keys balance + the 3-row unlock UI state. fills exactly the
    // gap from +0x19130 up to the next subsystem at +0x1CB18.
    Shop             shop_;                  // +0x19130..+0x1CB18 (0x39E8 bytes)

    // ---- leaderboard viewer (header region) ----
    // visible / closeRequested / dirty flags + the per-difficulty TextItem
    // rows. ends EXACTLY where ScoreHistory begins.
    LeaderboardMenu  leaderboardMenu_;       // +0x1CB18..+0x234C0 (0x69A8 bytes)

    // ---- per-difficulty stat-history lists ----
    // 3 * std::list<Entry> at the binary's exact +0x234C0 offset (=
    // LeaderboardMenu+0x69A8). FUN_100037d3c inserts here on every run end
    // and returns the entry's 1-based rank for ScorePanel::setResultRankVisual.
    // semantically embedded inside LeaderboardMenu but kept as a top-level
    // Game field for direct access from the exitRequested bridge.
    ScoreHistory     scoreHistory_;          // +0x234C0..+0x23508 (0x48 bytes)

    // ---- achievements page ----
    // 50-tile unlock grid + flags. fills the remaining gap to
    // transitionTarget at +0x2E170.
    AchievementsMenu achievementsMenu_;      // +0x23508..+0x2E170 (0xAC68 bytes)

    // FUN_100045410's `param_2[0xb85c]` reads drive the title->gameplay
    // transition gate. set to 0 by Game::create's memset and to a target
    // state code by various screens (game.cpp:836, 912).
    int          transitionTarget_;      // +0x2E170

    uint8_t      _gap_2E174_to_2E178[0x2E178 - (0x2E170 + sizeof(int))];

    // screen-transition curtain (FUN_10001070c/61c/05ec/0740) at its exact
    // binary offset. the transition driver FUN_100045410 reads visible (+0x0),
    // opening (+0x1B8 = game+0x2E330), and progress (+0x1BC = game+0x2E334)
    // here every frame. access via overlay().
    Overlay      overlay_;               // +0x2E178..+0x2E340 (0x1C8)

    // ===== persistence region =====
    // slots 0..4 (sav / set / unl / sco / ach) each carry a dirty byte +
    // versioned buffer + 0x18-byte scratch vector for the encoded blob.
    // see save_buffers.h for the full architecture. dirty-byte / scratch
    // / magic-word offsets are dictated by the iOS slot dispatchers
    // (FUN_10004621c / FUN_1000475b8) and the per-slot encoders.

    // slot 0 ("sav"). dirty is uint32: 0 = clean, 1 = encode, 2 = delete.
    uint32_t     saveSlot0Dirty_;        // +0x2E340, slot 0 dirty (FUN_10004621c case 0)
    uint8_t      _pad_2E344[4];          // +0x2E344..+0x2E347

    GameSnapshot gameSnapshot_;          // +0x2E348..+0x2E6B7, mid-run state

    // slot 0 scratch buffer. holds the encoded blob between encode and disk
    // write. libc++ std::vector head is 0x18 bytes; storage heap-allocated.
    std::vector<uint8_t> saveSlot0Scratch_;   // +0x2E6B8..+0x2E6CF

    // saved-run presence flag at game+0x2E6D0. binary stores the raw
    // NSData byteCount of the "sav" blob here (uint64 to mirror the
    // slot-0 loader's `*(ulong*)(game+0x2E6D0) = length;` write), but
    // the only read site (start-button branch) treats it as a boolean
    // gate: non-zero => saved run present => transitionTarget = 1.
    uint64_t     hasSavedRun_;           // +0x2E6D0

    // slot 1 ("set"). settings group: tutorial flag + cached difficulty +
    // global volumes. consumed by FUN_1000481f8 / FUN_1000462ac case 1.
    bool         saveSlot1Dirty_;        // +0x2E6D8
    uint8_t      _pad_2E6D9[3];          // +0x2E6D9..+0x2E6DB
    int32_t      settingsMagic_;         // +0x2E6DC, slot 1 magic
    bool         tutorialFlag_;          // +0x2E6E0, PauseMenu tutorial toggle byte
    uint8_t      _pad_2E6E1[3];          // +0x2E6E1..+0x2E6E3

    // post-Level::generate stash of the selected difficulty (written at
    // game+0x2E6E4). consumed by case 0 of the transitionTarget switch as
    // the worldIndex arg to World::generate.
    int          stashedDifficulty_;     // +0x2E6E4

    // global volume settings used when no GameBoard is active. when the
    // gameplay GameBoard is visible, its own seVolume / bgmVolume
    // (+0x10 / +0x14) override these.
    //   globalSeVolume:  Game::dispatchSounds reads this as the SE gain
    //                     context (binary: FUN_100035cfc's pfVar1 fallback).
    //   globalBgmVolume: Game::update reads this each frame for
    //                     MusicController::setTargetVolume (binary:
    //                     FUN_100045410's pfVar6 fallback before
    //                     FUN_100008800).
    float        globalSeVolume_;        // +0x2E6E8
    float        globalBgmVolume_;       // +0x2E6EC

    // slot 1 scratch buffer (11-byte encoded blob lives here).
    std::vector<uint8_t> saveSlot1Scratch_;   // +0x2E6F0..+0x2E707

    // slot 2 ("unl"). shop unlocks save xfer state. dirty byte set to 1
    // just BEFORE Shop::dirtyXfer fires; buffer at +0x2E710 holds the
    // persistent unlocks vectors (magic + keys + 3 unlock vectors).
    bool         shopSaveDirty_;         // +0x2E708, slot 2 dirty
    uint8_t      _pad_2E709[7];          // +0x2E709..+0x2E70F
    PersistentUnlocks shopSaveBuffer_;   // +0x2E710..+0x2E75F

    // slot 2 scratch buffer.
    std::vector<uint8_t> saveSlot2Scratch_;   // +0x2E760..+0x2E777

    // slot 3 ("sco"). leaderboard xfer state. dirty byte set immediately
    // before LeaderboardMenu::dirtyXfer; the trailing vector is filled
    // with XferEntry copies of every ScoreHistory entry across all 3
    // lists. staged both for local persistence and Game Center upload
    // (the upload side has no Android consumer).
    bool         leaderboardSaveDirty_;  // +0x2E778
    uint8_t      _pad_2E779[7];          // +0x2E779..+0x2E77F
    int32_t      leaderboardSaveMagic_;  // +0x2E780, slot 3 magic
    uint8_t      _pad_2E784[4];          // +0x2E784..+0x2E787
    std::vector<LeaderboardMenu::XferEntry> leaderboardSaveBuffer_;
                                         // +0x2E788..+0x2E79F

    // slot 3 scratch buffer.
    std::vector<uint8_t> saveSlot3Scratch_;   // +0x2E7A0..+0x2E7B7

    // slot 4 ("ach"). AchievementTracker save mirror. dirty byte set when
    // the tracker's state mutates; buffer at +0x2E7C0 holds the persisted
    // achievement state. loader / encoder defer field-naming to task H/F.
    bool         saveSlot4Dirty_;        // +0x2E7B8
    uint8_t      _pad_2E7B9[7];          // +0x2E7B9..+0x2E7BF
    AchievementSaveBuffer achievementSaveBuffer_;   // +0x2E7C0..+0x2E847

    // slot 4 scratch buffer (last; runs out to the Game struct end at
    // +0x2E860).
    std::vector<uint8_t> saveSlot4Scratch_;   // +0x2E848..+0x2E85F

    // friend struct that runs the layout asserts. defined at namespace
    // scope after the class is complete; offsetof needs a complete type
    // and access to private members.
    friend struct GameLayoutCheck;

public:
    // ---- public utility members (NOT part of the binary struct) ----
    // textures and the utility subsystems trail the binary region; they're
    // extracted for cleaner code and don't need fixed offsets.

    // the texture ids loaded for this game.
    GLuint           textures[GAME_NUM_TEXTURES];

    // utility subsystems.
    SoundQueue       soundQueue;
    MusicController  musicController;
};

// ===== layout invariants =====
// any field-add/remove that breaks the binary layout fails at compile
// time. fix by adjusting the sub-class size, NOT by editing these asserts.
//
// run inside a friend struct so offsetof has private-member access AND
// the Game class is complete (offsetof requires both).
struct GameLayoutCheck {
    static_assert(offsetof(Game, gameState_)         == 0x00000,
                  "gameState_ must be the first binary field of Game");
    static_assert(offsetof(Game, bmfontTables_)      == 0x00010);
    static_assert(offsetof(Game, animTiming_005a_)   == 0x036A0);
    static_assert(offsetof(Game, scaleFactor_)       == 0x036C8);
    static_assert(offsetof(Game, letterboxQuad1_)    == 0x036D0);
    static_assert(offsetof(Game, letterboxQuad2_)    == 0x037A8);
    static_assert(offsetof(Game, achievementTracker_) == 0x042F8);
    static_assert(offsetof(Game, titleMenu_)         == 0x04460);
    static_assert(offsetof(Game, world_)             == 0x16DB0);
    static_assert(offsetof(Game, boardPtr_)          == 0x19120);
    static_assert(offsetof(Game, scorePanel_)        == 0x19128);
    static_assert(offsetof(Game, shop_)              == 0x19130);
    static_assert(offsetof(Game, leaderboardMenu_)   == 0x1CB18);
    static_assert(offsetof(Game, scoreHistory_)      == 0x234C0);
    static_assert(offsetof(Game, achievementsMenu_)  == 0x23508);
    static_assert(offsetof(Game, transitionTarget_)  == 0x2E170);
    static_assert(offsetof(Game, overlay_)           == 0x2E178);
    static_assert(offsetof(Game, saveSlot0Dirty_)    == 0x2E340);
    static_assert(offsetof(Game, gameSnapshot_)      == 0x2E348);
    static_assert(offsetof(Game, saveSlot0Scratch_)  == 0x2E6B8);
    static_assert(offsetof(Game, hasSavedRun_)       == 0x2E6D0);
    static_assert(offsetof(Game, saveSlot1Dirty_)    == 0x2E6D8);
    static_assert(offsetof(Game, settingsMagic_)     == 0x2E6DC);
    static_assert(offsetof(Game, tutorialFlag_)      == 0x2E6E0);
    static_assert(offsetof(Game, stashedDifficulty_) == 0x2E6E4);
    static_assert(offsetof(Game, globalSeVolume_)    == 0x2E6E8);
    static_assert(offsetof(Game, globalBgmVolume_)   == 0x2E6EC);
    static_assert(offsetof(Game, saveSlot1Scratch_)  == 0x2E6F0);
    static_assert(offsetof(Game, shopSaveDirty_)     == 0x2E708);
    static_assert(offsetof(Game, shopSaveBuffer_)    == 0x2E710);
    static_assert(offsetof(Game, saveSlot2Scratch_)  == 0x2E760);
    static_assert(offsetof(Game, leaderboardSaveDirty_)  == 0x2E778);
    static_assert(offsetof(Game, leaderboardSaveMagic_)  == 0x2E780);
    static_assert(offsetof(Game, leaderboardSaveBuffer_) == 0x2E788);
    static_assert(offsetof(Game, saveSlot3Scratch_)  == 0x2E7A0);
    static_assert(offsetof(Game, saveSlot4Dirty_)    == 0x2E7B8);
    static_assert(offsetof(Game, achievementSaveBuffer_) == 0x2E7C0);
    static_assert(offsetof(Game, saveSlot4Scratch_)  == 0x2E848);

    // canonical end-of-binary-data invariant: the binary-fields region
    // (gameState_ through saveSlot4Scratch_ inclusive) totals
    // GAME_STRUCT_SIZE. catches ANY drift in sub-class size or padding.
    static_assert(offsetof(Game, saveSlot4Scratch_)
                  + sizeof(Game::saveSlot4Scratch_)
                  == GAME_STRUCT_SIZE,
                  "binary-layout fields must total exactly GAME_STRUCT_SIZE bytes");
};


// bindTexture moved to renderer.h; it's a pure GL state-tracking wrapper
// with no Game-state dependency.

// global game-singleton accessor, mirrors the binary's FUN_100043798 which
// reads DAT_10007e868. lets subsystems (PlayerSystem, panels, etc.) reach
// the Game struct without storing their own back-pointer.
Game* getGame();
