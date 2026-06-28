#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <set>

#include "achievement_table.h"  // AchievementId enum
#include "save_buffers.h"       // AchievementSaveBuffer

class EventSlot;

// reconstructed from Ghidra:
//   isLocked:      FUN_10004d6c4  (true iff states[idx] == 0; also true
//                                  for any OOB idx)
//   hasBeenShown:  FUN_10004d6e8
//   getProgress:   FUN_10004d70c
//   getCounter:    FUN_10004d770
//   markShown:     FUN_10004d82c
//   increment:     FUN_10004d858   -> core event hook (see [[event-hooks]])
//   beginSession:  FUN_10004d7ec
//
// AchievementTracker lives at Game.achievementTracker_. earlier phases scaffolded this
// as "AudioState" because the one ported method (beginSession, then
// mis-named setMusicInGameMode) is invoked from initLevel(1) and looked
// superficially like a music-mode toggle. nothing about this struct is
// audio; the SoundQueue dispatcher is the real audio
// fan-in.
//
// per-achievement state machine (states[idx]):
//   0 = LOCKED (still in progress)
//   1 = unlocked, banner not-yet-shown to the user
//   2 = unlocked AND the banner has been shown
//
// counters split:
//   - 47 of the 50 achievements: counter lives in the counters std::map
//   - 3 unique-item std::set counters for idx 22 / 25 / 26 (eventKinds /
//     eventIds / snagKinds). these aren't "counters" in the increment sense;
//     they're sets whose size() is the progress count. e.g. idx 22 =
//     "Activate all 5 types of events" -> eventKinds.size() of 5 unlocks.
//     bitmask 0x6400000 in increment() routes idx 22/25/26 away from
//     the map; the size-as-count progress instead grows when something
//     external inserts into the sets via onEventActivated / onSnagEaten.
//
// dual notification lists:
//   pendingNotifications, drained when the banner shows the
//                                   unlock (achievements menu's update
//                                   calls markShown when a tile enters
//                                   the viewport)
//   newlyUnlocked        xfer queue piped to GameKit's
//                                   reportAchievement on iOS; on
//                                   Android nothing consumes it but
//                                   we keep the writes for fidelity
class AchievementTracker {
public:
    // ---- methods (the 7 binary entry points) ----

    // FUN_10004d6c4. true when states[idx] == 0 (still locked). also
    // returns true for any idx > 49 (out-of-range is treated as locked).
    bool isLocked(uint32_t idx) const;

    // FUN_10004d6e8. true iff states[idx] == 2 (unlocked AND the banner
    // notification has been shown). returns false for any OOB idx.
    bool hasBeenShown(uint32_t idx) const;

    // FUN_10004d70c. returns 1 when the achievement has a non-zero
    // targetCount (i.e. progress-based, N/M); when 1 ALSO writes the
    // current counter to *currentOut and the target to *maxOut (either
    // pointer may be null). returns 0 for one-shot achievements (target
    // 0) or OOB idx.
    int getProgress(uint32_t idx, int* currentOut, int* maxOut);

    // FUN_10004d770. returns the counter value for idx. for the 3
    // "unique-item set" idx values (22 / 25 / 26), returns the
    // corresponding set's size(). for everything else falls back to a
    // std::map lookup (returns 0 when not yet inserted).
    int getCounter(uint32_t idx);

    // FUN_10004d82c. transitions states[idx] from 1 -> 2 (unlocked-not-
    // shown to unlocked-shown). marks dirty. called by AchievementsMenu's
    // update when a pending-notification tile scrolls into view.
    void markShown(uint32_t idx);

    // FUN_10004d858. the core event-increment dispatcher.
    //   1. if states[idx] != 0, no-op (already unlocked or shown)
    //   2. for non-special idx (not 22 / 25 / 26): bump the std::map
    //      counter for this idx by 1 (insert with value 1 if absent)
    //   3. for all idx: read current counter via getCounter; if it
    //      meets/exceeds targetCount, transition states[idx] = 1 and
    //      push idx onto both notification lists
    //   4. mark dirty
    // call sites are scattered across game logic (snag death, level
    // end, item pickup, etc.) via ~40 trampolines in
    // FUN_10004da20..FUN_10004e3b4, F7.6-H scope.
    void increment(uint32_t idx);

    // strongly-typed overload. external fire sites use this so the enum
    // name carries the semantics; both this and the raw-uint32_t form
    // hit the same FUN_10004d858 logic.
    void increment(AchievementId id) { increment(static_cast<uint32_t>(id)); }

    // FUN_10004d7ec. clears pendingNotifications via list::clear. if
    // sessionFlag != 0 AND TheEasyWay (states[13]) is still locked, also
    // clears damagedThisRun and marks dirty. the states[13] gate is an
    // optimization: once TheEasyWay is unlocked it can never re-fire, so
    // there's no point churning the damage flag anymore. sessionFlag=1
    // is passed only by the start-new-run path (FUN_1000161fc); the
    // in-engine world-to-world advance and the save-load path both pass
    // 0, which is what makes damagedThisRun a true per-run flag (persists
    // across worlds within a run, persists across save/load).
    void beginSession(int sessionFlag);

    // FUN_10004d968. pops the front node of pendingNotifications and
    // returns its idx. returns 50 (= sentinel "no more pending") if the
    // list is empty. called from GameBoard::update each frame to feed
    // achievement-unlock banners to the popup display.
    uint32_t popNextUnlock();

    // FUN_10004e200. fan-out on every Event activation.
    void noteEventActivated(EventSlot& evt);

    // FUN_10004e3e4. set damagedThisRun = 1 if it isn't already, AND
    // only if TheEasyWay (states[13]) is still locked (optimization: no
    // point churning the dirty byte once that achievement can't fire).
    // called from GameBoard::setHP's damage path.
    void notePlayerDamaged();

    // FUN_10004e404. push persistable tracker state into the save buffer.
    // clears `dirty`. states[] copy is unfiltered (all 50). counters /
    // eventKinds / eventIds / snagKinds are pruned to "still relevant"
    // entries: counters drops zero-value or already-unlocked rows; the 3
    // sets drop entirely once their gating achievement (idx 22 / 25 / 26)
    // unlocks. damagedThisRun copied directly. fired from Game::update
    // when this->dirty is set.
    void dirtyXfer(AchievementSaveBuffer& snap);

    // FUN_10004e6ac. pull saved state back into the tracker on game
    // init. mirrors dirtyXfer: states[] up to 50, then containers, then
    // damagedThisRun. called from Game::init after SaveSystem::load.
    void restoreFromSave(const AchievementSaveBuffer& snap);

    // ---- event-hook fan-out ----
    //
    // the binary has ~18 dispatcher functions (FUN_10004da20..FUN_10004e3b4)
    // that translate gameplay events into specific achievement increments.
    // we don't port these as methods; each call site holds a small inline
    // conditional that calls `increment(AchievementId::X)` directly. that
    // keeps the gating logic visible next to the event source and lets a
    // `grep AchievementId::` audit list every fire site.

    // ---- byte-exact field layout ----

    uint8_t            dirty;                  // consumed by GameKit xfer (iOS)
    int32_t            states[50];             // per-achievement state machine
                                               //                 (0=locked, 1=banner-pending, 2=shown)
    std::map<int, int> counters;               // sparse counter store for the
                                               //                 47 non-special achievements

    // 3 std::set<int> objects, each 24 bytes (begin_ptr + end_node + size).
    // each set's size() is the achievement progress count for the linked
    // idx; getCounter() returns set.size() for these 3 idx values.
    //
    //   eventKinds : idx 22 (target 5)  = "Activate all 5 types of events"
    //                                     inserted on onEventActivated:
    //                                       insert(event.kind)
    //   eventIds   : idx 25 (target 50) = "Activate 50 different events"
    //                                     inserted on onEventActivated:
    //                                       insert(event.id)
    //   snagKinds  : idx 26 (target 100)= "Defeat 100 different special snags"
    //                                     inserted on onSpecialSnagEaten:
    //                                       insert(snag.kind)
    std::set<int>      eventKinds;
    std::set<int>      eventIds;
    std::set<int>      snagKinds;

    uint8_t            damagedThisRun;             // set 1 on first damage taken; cleared
                                                   //          by beginSession on new-run start.

    std::list<int>     pendingNotifications;   // banner-pending queue
    std::list<int>     newlyUnlocked;          // GameKit xfer queue (iOS)
};
