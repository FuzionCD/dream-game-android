#pragma once

#include <cstddef>
#include <cstdint>

class Game;

// ----------------------------------------------------------------------
// SaveSystem
//
// Android-side persistence layer that replaces the iOS NSUserDefaults
// read / synchronize round-trip. on iOS the 5 save slots are NSData blobs
// keyed by 3-char names ("sav", "set", "unl", "sco", "ach") inside
// `[NSUserDefaults standardUserDefaults]`. on Android we write the same
// blobs to 5 small files under `SDL_AndroidGetInternalStoragePath()`:
//
//   /data/data/<package>/files/sav.bin     (slot 0: mid-run snapshot)
//   /data/data/<package>/files/set.bin     (slot 1: settings)
//   /data/data/<package>/files/unl.bin     (slot 2: shop unlocks)
//   /data/data/<package>/files/sco.bin     (slot 3: leaderboard ranks)
//   /data/data/<package>/files/ach.bin     (slot 4: achievement state)
//
// the per-slot loader / encoder bodies are 1:1 ports of the iOS dispatcher
// (FUN_10004621c / FUN_1000475b8 / FUN_1000462ac). this header just exposes
// the lifecycle hooks; each slot's body is filled by tasks H/C..H/G.
//
// lifecycle:
//   1. main.cpp -> SaveSystem::load(game)   before Game::init (mirrors
//      GameViewController::viewDidLoad's loader loop). reads each file,
//      dispatches its bytes through the slot loader so the matching Game
//      region (and its magic word) gets populated.
//   2. Game::update tail -> SaveSystem::flushDirty(game). per-slot dirty bit
//      check; if set, run the slot's encoder, write the resulting bytes to
//      that slot's file. mirrors GameViewController::update's slot loop +
//      synchronize call.
// ----------------------------------------------------------------------

namespace SaveSystem {

// the 5 save-slot identifiers. order matches the iOS dispatcher's switch
// statement; the slot number is the case label.
enum SlotId : int {
    SAV = 0,   // mid-run GameSnapshot
    SET = 1,   // settings
    UNL = 2,   // shop unlocks
    SCO = 3,   // leaderboard ranks
    ACH = 4,   // achievement state
    NUM_SLOTS = 5,
};

// load all 5 slot files from internal storage and dispatch each blob into
// its matching Game region. mirrors viewDidLoad's NSUserDefaults read loop
// (at 0x10002aa10 in the binary). called once at startup before Game::init.
// missing files are silent no-ops; corrupt / wrong-magic blobs get
// silently rejected by the slot loaders.
void load(Game& game);

// scan all 5 slot dirty bits; for each dirty slot, run the encoder and
// write the resulting bytes to the matching file. mirrors GameViewController
// ::update's tail loop (at 0x10002af84 in the binary). called from Game::
// update's tail, once per frame.
void flushDirty(Game& game);

}  // namespace SaveSystem
