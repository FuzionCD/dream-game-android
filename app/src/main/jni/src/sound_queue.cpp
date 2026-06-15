#include "sound_queue.h"
#include <cstring>
#include <SDL.h>

void SoundQueue::init() {
    memset(entries, 0, sizeof(entries));
    cursor = 0;
}

// reconstructed from FUN_100035c98
void SoundQueue::update(float dt) {
    cursor = 0;

    for (int i = 0; i < SOUND_QUEUE_SLOTS; i++) {
        entries[i].playFlag = 0;

        if (entries[i].delay > 0.0f) {
            entries[i].delay -= dt;
        }
    }
}

// reconstructed from FUN_100035ccc
void SoundQueue::trigger(int slotIndex) {

    if (slotIndex < 0 || slotIndex >= SOUND_QUEUE_SLOTS) {
        return;
    }

    if (entries[slotIndex].delay <= 0.0f) {
        entries[slotIndex].playFlag = 1;
        entries[slotIndex].delay = 0.05f;
    }
}

// reconstructed from FUN_100035cfc
// the original's volume check is handled by Game::dispatchSounds() before calling this
int SoundQueue::getNextTriggered() {

    while (cursor < SOUND_QUEUE_SLOTS) {
        int idx = cursor;
        cursor++;

        if (entries[idx].playFlag) {
            return idx;
        }
    }

    return SOUND_QUEUE_NONE;
}

void SoundQueue::resetCursor() {
    cursor = 0;
}
