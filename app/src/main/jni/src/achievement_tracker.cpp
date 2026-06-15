#include "achievement_tracker.h"
#include "achievement_table.h"
#include "event_slot.h"

#include <algorithm>   // std::min

// FUN_10004d6c4, isLocked.
bool AchievementTracker::isLocked(uint32_t idx) const {

    if (idx > 49) {
        return true;
    }

    return states[idx] == 0;
}

// FUN_10004d6e8, hasBeenShown.
bool AchievementTracker::hasBeenShown(uint32_t idx) const {

    if (idx > 49) {
        return false;
    }

    return states[idx] == 2;
}

// FUN_10004d770, getCounter.
//
// 3-way dispatch: idx 22 / 25 / 26 read the size of the linked unique-item
// std::set (eventKinds / eventIds / snagKinds). everything else falls back
// to the std::map counter lookup (returns 0 when not inserted yet).
int AchievementTracker::getCounter(uint32_t idx) {

    if (idx == 0x1a) {
        return static_cast<int>(snagKinds.size());
    }

    if (idx == 0x19) {
        return static_cast<int>(eventIds.size());
    }

    if (idx == 0x16) {
        return static_cast<int>(eventKinds.size());
    }

    auto it = counters.find(static_cast<int>(idx));

    if (it == counters.end()) {
        return 0;
    }

    return it->second;
}

// FUN_10004d70c, getProgress.
//
// returns 1 when achievement has a target (= is progress-based). when 1,
// writes current counter to *currentOut and target to *maxOut (either
// pointer may be null; matches binary's null-checks).
int AchievementTracker::getProgress(uint32_t idx, int* currentOut, int* maxOut) {

    if (idx >= 50) {
        return 0;
    }

    const int target = static_cast<int>(ACHIEVEMENT_TABLE[idx].targetCount);

    if (target <= 0) {
        return 0;
    }

    if (maxOut != nullptr) {
        *maxOut = target;
    }

    if (currentOut != nullptr) {
        *currentOut = getCounter(idx);
    }

    return 1;
}

// FUN_10004d82c, markShown.
//
// only transitions 1 -> 2. silently no-ops if state is 0 (locked) or 2
// (already shown).
void AchievementTracker::markShown(uint32_t idx) {

    if (idx >= 50) {
        return;
    }

    if (states[idx] != 1) {
        return;
    }

    states[idx] = 2;
    dirty       = 1;
}

// FUN_10004d858, increment.
//
// dispatches an achievement event:
//   1. if already unlocked or shown, no-op
//   2. for non-special idx, bump map counter
//   3. for all idx, re-read counter via getCounter; when target met,
//      mark unlocked, push idx onto both notification lists
//
// the 3 special idx values (22, 25, 26) have dedicated counters that are
// written from outside this method; increment only does their unlock
// check on the cached counter value.
void AchievementTracker::increment(uint32_t idx) {

    if (idx >= 50) {
        return;
    }

    if (states[idx] != 0) {
        return;
    }

    // bitmask 0x6400000 = bits 22 | 25 | 26, the 3 dedicated-counter idx
    // values that skip the map insert.
    const bool isSpecial = (idx <= 26)
                         && ((1u << (idx & 0x1f)) & 0x6400000u) != 0;

    if (!isSpecial) {
        counters[static_cast<int>(idx)] += 1;
    }

    // unlock check is literal `target <= counter`. for one-shot achievements
    // (target == 0), the map bump above lifts counter from 0 to 1, so 0 <= 1
    // fires the unlock on the first increment() call. for the 3 special idx
    // (22/25/26), increment() does not bump their dedicated counter slot;
    // the slot starts at 0 and only crosses target when an external writer
    // raises it, so increment() acts as the unlock-poll for those.
    const int current = getCounter(idx);
    const int target  = static_cast<int>(ACHIEVEMENT_TABLE[idx].targetCount);

    if (target <= current) {
        states[idx] = 1;
        pendingNotifications.push_back(static_cast<int>(idx));
        newlyUnlocked.push_back(static_cast<int>(idx));
    }

    dirty = 1;
}

// FUN_10004d7ec, beginSession.
//
// clears any banner-pending notifications carried over from the previous
// session, then on a brand-new run (sessionFlag != 0) resets the
// damagedThisRun flag. the states[13] (TheEasyWay) check is an
// optimization: once unlocked we never re-fire so don't bother churning
// the flag. only FUN_1000161fc (start-new-run-from-level-select) passes
// sessionFlag=1; in-engine world advance and save/load both pass 0, which
// is what gives damagedThisRun true per-run lifetime.
void AchievementTracker::beginSession(int sessionFlag) {
    pendingNotifications.clear();

    if (sessionFlag != 0 && states[13] == 0) {
        damagedThisRun = 0;
        dirty          = 1;
    }
}

// FUN_10004e200, noteEventActivated.
void AchievementTracker::noteEventActivated(EventSlot& evt) {
    increment(AchievementId::Recurring);
    eventKinds.insert(static_cast<int>(evt.getEventTypeKey()));
    increment(AchievementId::JackOfAllTrades);
    eventIds.insert(evt.eventType);
    increment(AchievementId::EverythingIsDifferentNow);
}

// FUN_10004e3e4, notePlayerDamaged.
//
// once-set damage flag. early-out when already 1 (no-op) or once
// TheEasyWay (states[13]) is unlocked (no point updating either flag).
void AchievementTracker::notePlayerDamaged() {

    if (damagedThisRun == 0 && states[13] == 0) {
        damagedThisRun = 1;
        dirty          = 1;
    }
}

// FUN_10004d968, popNextUnlock.
//
// pops the front of pendingNotifications and returns its idx. returns 50
// (= sentinel "no more pending") when the list is empty. consumer is the
// GameBoard banner popup driver (FUN_10004fd7c).
uint32_t AchievementTracker::popNextUnlock() {

    if (pendingNotifications.empty()) {
        return 50;
    }

    const uint32_t idx = static_cast<uint32_t>(pendingNotifications.front());
    pendingNotifications.pop_front();
    return idx;
}

// event-hook fan-out lives at the call sites; see achievement_tracker.h.

// FUN_10004e404, dirtyXfer.
//
// stages persistable tracker state into the save buffer. clears dirty,
// then pushes:
//   - all 50 states[]                                             (unfiltered)
//   - counters[] entries where (val > 0 && states[key] == 0)      (filtered)
//   - eventKinds  if states[22] (Recurring) still locked
//   - eventIds    if states[25] (JackOfAllTrades) still locked
//   - snagKinds   if states[26] (EverythingIsDifferentNow) still locked
//   - damagedThisRun                                              (direct)
//
// binary opens by recalculating snap.states.end to match begin (= clear),
// then push_back's 50 entries one at a time. libc++ vector::assign over a
// 50-int range does the equivalent in one call.
void AchievementTracker::dirtyXfer(AchievementSaveBuffer& snap) {
    dirty = 0;

    snap.states.assign(states, states + 50);

    snap.counters.clear();

    for (const auto& [key, val] : counters) {

        if (val > 0 && key >= 0 && key < 50 && states[key] == 0) {
            snap.counters[key] = val;
        }
    }

    snap.eventKinds.clear();

    if (states[22] == 0) {
        snap.eventKinds = eventKinds;
    }

    snap.eventIds.clear();

    if (states[25] == 0) {
        snap.eventIds = eventIds;
    }

    snap.snagKinds.clear();

    if (states[26] == 0) {
        snap.snagKinds = snagKinds;
    }

    snap.damagedThisRun = damagedThisRun;
}

// FUN_10004e6ac, restoreFromSave.
//
// inverse of dirtyXfer. copies snap.states into tracker.states[] up to
// the 50-entry cap, then assigns each container from snap. damagedThisRun
// is a direct byte copy. self-assign guards in the binary are redundant
// when src and dst are at distinct addresses (always true for tracker
// vs. save buffer); std::map / std::set operator= handles aliasing
// internally anyway.
void AchievementTracker::restoreFromSave(const AchievementSaveBuffer& snap) {
    const size_t cap = std::min(snap.states.size(), static_cast<size_t>(50));

    for (size_t i = 0; i < cap; ++i) {
        states[i] = snap.states[i];
    }

    counters       = snap.counters;
    eventKinds     = snap.eventKinds;
    eventIds       = snap.eventIds;
    snagKinds      = snap.snagKinds;
    damagedThisRun = snap.damagedThisRun;
}
