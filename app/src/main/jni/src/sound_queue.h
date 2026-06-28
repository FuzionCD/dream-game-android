#pragma once

#include <cstdint>

// reconstructed from Ghidra:
//   update:  FUN_100035c98
//   trigger: FUN_100035ccc
//   getNext: FUN_100035cfc
//   getSoundId: FUN_100035ef4
//   getSoundGain: FUN_100035de4
//
// the sound queue is embedded in the game struct at offset 0x3880.
// it contains 82 slots (0x290 / 8), each 8 bytes:
//   byte 0: play flag (1 = play this frame)
//   bytes 4-7: delay timer (float, 50ms cooldown after trigger)
//
// game logic sets play flags via trigger(). after the update tick,
// the host reads flags and dispatches to the audio engine.

#define SOUND_QUEUE_SLOTS 82
#define SOUND_QUEUE_NONE 82

struct SoundQueueEntry {
    uint8_t playFlag;
    float delay;
};

class SoundQueue {
public:
    // zero-initialize
    void init();

    // FUN_100035c98: clear all play flags, decrement active delays
    void update(float dt);

    // FUN_100035ccc: trigger a sound by slot index (sets play flag if cooldown expired)
    void trigger(int slotIndex);

    // iterate triggered sounds this frame (for dispatching to audio engine)
    // returns slot index of next triggered sound, or SOUND_QUEUE_NONE if done
    // call repeatedly until it returns SOUND_QUEUE_NONE
    int getNextTriggered();

    // reset the iteration cursor
    void resetCursor();

private:
    SoundQueueEntry entries[SOUND_QUEUE_SLOTS];
    int cursor;
};
