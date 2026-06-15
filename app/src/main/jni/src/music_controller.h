#pragma once

#include <cstdint>

// reconstructed from Ghidra:
//   update:     FUN_100008514
//   resetFade:  FUN_100008568
//   setTrack:   FUN_10000860c
//   getVolume:  FUN_1000085a4
//   clearList:  FUN_100008894
//   getLayerCount: FUN_10000854c (always returns 16)
//   setVolume:  FUN_100008800
//   consumeFlag: FUN_100008874
//
// the original uses a linked list as a track stack. when menus open over
// the board, each layer pushes its music. closing a menu pops back to the
// previous track. we use a fixed-size array to replicate the stack behavior.
//
// ios flow:
//   1. syncMusic() calls resetFade() then pushes new tracks via setTrack()
//   2. resetFade() saves current volume, starts fading to zero, clears stack
//   3. setTrack() pushes a track onto the stack at a random position
//   4. each frame, update() advances the fade
//   5. applyToAudio() reads the top of the stack and the effective volume,
//      and applies changes to SDL_mixer
//   6. when fade completes, the new stack's top track plays at proper volume

#define MUSIC_TRACK_COUNT 16
#define MUSIC_STACK_MAX 16

class MusicController {
public:
    void init();

    // FUN_100008514: advance fade animation
    void update(float dt);

    // FUN_100008568: start fading out, then clear the track stack
    void resetFade();

    // FUN_10000860c: push a track onto the stack
    void setTrack(int trackIndex);

    // FUN_1000085a4: get the current effective volume (0-1)
    float getVolume();

    // FUN_100008800: set target volume with threshold check
    void setTargetVolume(float volume);

    // FUN_100008874: consume and clear the "changed" flag
    bool consumeChangeFlag();

    // apply current state to the audio engine (called once per frame)
    void applyToAudio();

    // get the currently active track index (-1 if none)
    int getCurrentTrack() const;

private:
    // track stack (replaces the original's linked list)
    int trackStack[MUSIC_STACK_MAX];
    int stackSize;

    float volume;           // target volume level (0-1)
    bool changeFlag;        // set when state changes
    bool fading;            // currently fading out
    float fadeStartVolume;  // volume at start of fade
    float fadeProgress;     // 0 to 1 during fade
    bool fadeComplete;      // true when fade-out finishes

    int lastAppliedTrack;   // track currently playing in SDL_mixer
};
