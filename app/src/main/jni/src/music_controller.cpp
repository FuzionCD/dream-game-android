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
    lastAppliedTrack = -1;

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

// reconstructed from FUN_10000860c
// pushes a track onto the stack at a random position.
// the original uses a linked list and inserts at a random index
// determined by FUN_1000570ec(0, listSize, 0). we replicate
// the random insertion into our array-based stack.
void MusicController::setTrack(int trackIndex) {

    if (trackIndex < 0 || trackIndex >= MUSIC_TRACK_COUNT) {
        return;
    }

    if (stackSize >= MUSIC_STACK_MAX) {
        return;
    }

    if (stackSize == 0) {
        // empty stack: just push
        trackStack[0] = trackIndex;
        stackSize = 1;
    } else {
        // insert at a random position (FUN_10000860c, stream 0, cosmetic)
        int insertPos = rngInt(0, stackSize, 0);

        // shift elements to make room
        for (int i = stackSize; i > insertPos; i--) {
            trackStack[i] = trackStack[i - 1];
        }

        trackStack[insertPos] = trackIndex;
        stackSize++;
    }

    changeFlag = true;
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

    // volume squared * per-track multiplier for the top-of-stack track.
    int topTrack = trackStack[0];

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
        return trackStack[0];
    }

    return -1;
}

// apply the current music state to SDL_mixer.
// in the ios version, GameViewController does this each frame after
// calling the game's update. it reads getVolume() and the current track,
// then calls SoundEngine methods accordingly.
void MusicController::applyToAudio() {

    if (!changeFlag) {
        return;
    }

    changeFlag = false;

    int topTrack = getCurrentTrack();
    float vol = getVolume();

    // if the track changed (and we're not mid-fade), switch the music
    if (topTrack != lastAppliedTrack && !fading) {

        if (topTrack >= 0 && topTrack < MUSIC_TRACK_COUNT) {
            AudioEngine::playMusic(sMusicFiles[topTrack], vol);
            SDL_Log("MusicController: playing track %d (%s) vol %.2f",
                    topTrack, sMusicFiles[topTrack], vol);
        } else {
            AudioEngine::stopMusic();
            SDL_Log("MusicController: stopped music");
        }

        lastAppliedTrack = topTrack;
    } else {
        // just update volume (handles fading)
        AudioEngine::setMusicVolume(vol);
    }

    // if fade just completed, start the new track
    if (fadeComplete) {
        fadeComplete = false;

        if (topTrack >= 0 && topTrack < MUSIC_TRACK_COUNT) {
            vol = getVolume();
            AudioEngine::playMusic(sMusicFiles[topTrack], vol);
            SDL_Log("MusicController: fade complete, now playing track %d vol %.2f", topTrack, vol);
        }

        lastAppliedTrack = topTrack;
    }
}
