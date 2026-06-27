#include "music_controller.h"
#include "audio_engine.h"
#include "random.h"
#include <SDL.h>
#include <cmath>

// per-track volume multipliers (from DAT_100076668 in binary, stride 0x10)
static float sTrackVolumeMultiplier[MUSIC_TRACK_COUNT] = {
    0.7f, 0.45f, 0.45f, 0.45f,
    0.45f, 0.45f, 0.45f, 0.45f,
    0.45f, 0.45f, 0.45f, 0.45f,
    0.45f, 0.45f, 0.45f, 0.45f
};

// music filenames
static const char* sMusicFiles[MUSIC_TRACK_COUNT] = {
    "music00.mp3", "music01.mp3", "music02.mp3", "music03.mp3",
    "music04.mp3", "music05.mp3", "music06.mp3", "music07.mp3",
    "music08.mp3", "music09.mp3", "music10.mp3", "music11.mp3",
    "music12.mp3", "music13.mp3", "music14.mp3", "music15.mp3"
};

void MusicController::init() {
    stackSize = 0;
    volume = 0.0f;
    changeFlag = false;
    fading = false;
    fadeStartVolume = 0.0f;
    fadeProgress = 0.0f;
    fadeComplete = false;

    for (int i = 0; i < MUSIC_STACK_MAX; i++) {
        trackStack[i] = -1;
    }
}

// reconstructed from FUN_100008514
void MusicController::update(float dt) {

    if (!fading) {
        return;
    }

    changeFlag = true;
    fadeProgress += dt;

    if (fadeProgress >= 1.0f) {
        fadeProgress = 0.0f;
        fading = false;
        fadeComplete = true;
    }
}

// reconstructed from FUN_100008568
// saves current volume, starts fading to zero, then clears the track stack.
// the new tracks will be pushed via setTrack() after this call.
void MusicController::resetFade() {

    if (stackSize > 0) {
        fadeStartVolume = getVolume();
        fading = true;
        fadeProgress = 0.0f;
    }

    // clear the track stack (FUN_100008894)
    stackSize = 0;
    fadeComplete = false;
}

// insert trackIndex at array position pos, shifting [pos, stackSize) up by one.
void MusicController::insertTrackAt(int pos, int trackIndex) {

    for (int i = stackSize; i > pos; --i) {
        trackStack[i] = trackStack[i - 1];
    }

    trackStack[pos] = trackIndex;
    stackSize++;
}

// reconstructed from FUN_10000860c. insert a track near the front of the
// playlist. two-stage RNG (stream 0): an empty list or roll1 == 0 inserts at
// the front; otherwise roll2 picks how many steps back from the tail to walk,
// and the track goes just before that node.
void MusicController::setTrack(int trackIndex) {

    if (trackIndex < 0 || trackIndex >= MUSIC_TRACK_COUNT) {
        return;
    }

    // the binary's linked list is unbounded; our array caps at MUSIC_STACK_MAX.
    if (stackSize >= MUSIC_STACK_MAX) {
        return;
    }

    int pos = 0;

    if (stackSize > 0) {
        int roll1 = rngInt(0, stackSize, 0);

        if (roll1 != 0) {
            int roll2 = rngInt(0, stackSize - 1, 0);
            pos = (stackSize - 1) - roll2;
        }
    }

    insertTrackAt(pos, trackIndex);
}

// fade out the current track and queue a random gameplay track (1..N-1) in its
// place. the fade-out completing is what swaps to the new track (applyToAudio),
// giving a crossfade on each level change.
void MusicController::crossfadeToRandomGameplayTrack() {
    resetFade();
    setTrack(rngInt(1, MUSIC_TRACK_COUNT - 1, 0));
}

// reconstructed from FUN_1000086e0. pop the playing track (tail), recycle it
// into the front half so it won't replay soon, and return the new tail to play
// next. returns -1 when the list is empty or the volume is zero.
int MusicController::advanceTrack() {

    if (stackSize == 0 || volume <= 0.0f) {
        return -1;
    }

    // pop the tail (the track that just finished).
    int track = trackStack[stackSize - 1];
    stackSize--;

    // recycle it. fewer than 2 entries or roll1 == 0 goes to the front; else
    // roll2 in [count/2, count-1] walks back from the tail, keeping the track
    // out of the back half so it isn't picked again immediately.
    int pos = 0;

    if (stackSize >= 2) {
        int roll1 = rngInt(0, stackSize, 0);

        if (roll1 != 0) {
            int roll2 = rngInt(stackSize / 2, stackSize - 1, 0);
            pos = (stackSize - 1) - roll2;
        }
    }

    insertTrackAt(pos, track);

    // the new tail is the next track to play.
    return trackStack[stackSize - 1];
}

// reconstructed from FUN_1000085a4
float MusicController::getVolume() {

    if (fading) {
        // lerp startVolume down to 0 over the fade.
        return fadeStartVolume * (1.0f - fadeProgress);
    }

    if (stackSize == 0 || volume <= 0.0f) {
        return 0.0f;
    }

    // volume squared * per-track multiplier for the playing (tail) track.
    int topTrack = trackStack[stackSize - 1];

    if (topTrack < 0 || topTrack >= MUSIC_TRACK_COUNT) {
        return 0.0f;
    }

    return volume * volume * sTrackVolumeMultiplier[topTrack];
}

// reconstructed from FUN_100008800
void MusicController::setTargetVolume(float vol) {

    if (vol < 0.0f) {
        vol = 0.0f;
    }

    if (vol > 1.0f) {
        vol = 1.0f;
    }

    // only update if the change exceeds 0.01, or if this crosses the zero
    // boundary (one side zero, the other not).
    float diff = fabsf(vol - volume);

    if (diff > 0.01f || (vol > 0.0f) != (volume > 0.0f)) {
        volume = vol;
        changeFlag = true;
    }
}

// reconstructed from FUN_100008874
bool MusicController::consumeChangeFlag() {
    bool was = changeFlag;
    changeFlag = false;
    return was;
}

int MusicController::getCurrentTrack() const {

    if (stackSize > 0) {
        return trackStack[stackSize - 1];
    }

    return -1;
}

// per-frame music driver (the GameViewController::update music block in iOS).
// the playing track loops, so during a level this only pushes volume changes;
// it switches tracks when a fade-out completes (a level / screen change) or when
// nothing is playing yet (startup, or recovering from a muted gap).
void MusicController::applyToAudio() {

    // FUN_100008884: consume the fade-complete flag. a completed fade forces a
    // switch even though SDL may still report the old track as playing.
    bool fadeJustCompleted = fadeComplete;
    fadeComplete = false;

    if (!fadeJustCompleted && AudioEngine::isPlayingMusic()) {
        // still playing: just push volume changes (handles the fade ramp).
        if (consumeChangeFlag()) {
            AudioEngine::setMusicVolume(getVolume());
        }

        return;
    }

    // fade completed, or nothing playing: switch to the queued track.
    int track = advanceTrack();

    if (track < 0) {
        // empty list or muted: the binary calls stopMusic each frame here (no
        // log, since muted gameplay would otherwise flood it every frame).
        AudioEngine::stopMusic();
    } else {
        AudioEngine::playMusic(sMusicFiles[track], getVolume());
        SDL_Log("MusicController: playing track %d (%s)", track, sMusicFiles[track]);
    }
}
