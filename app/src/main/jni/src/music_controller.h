#pragma once

#include <cstdint>

// reconstructed from Ghidra:
//   update:     FUN_100008514
//   resetFade:  FUN_100008568
//   setTrack:   FUN_10000860c
//   advanceTrack: FUN_1000086e0
//   getVolume:  FUN_1000085a4
//   clearList:  FUN_100008894
//   setVolume:  FUN_100008800
//   consumeChangeFlag:   FUN_100008874
//   consumeFadeComplete: FUN_100008884
//
// the playlist is a fixed-size array modeling the original's linked list
// (index 0 = front, stackSize-1 = tail = the currently playing track).
// setTrack (FUN_10000860c) inserts near the front at a random position and
// advanceTrack (FUN_1000086e0) pops/recycles the tail; these are faithful ports
// of the binary's primitives.
//
// a track loops for the whole level, so the controller only changes tracks when
// a fade-out completes. those fades are kicked on level changes (each new level
// crossfades to a fresh random track via GameBoard::initLevelContent ->
// crossfadeToRandomGameplayTrack) and on screen changes (Game::syncMusic:
// title -> track 0; gameplay -> one random track; score panel -> silence).
//
// flow:
//   1. resetFade() saves the current volume, starts fading to zero, clears list.
//   2. setTrack() queues the next track.
//   3. each frame, update() advances the fade and applyToAudio() drives playback:
//      while a track plays it only pushes volume changes; once the fade-out
//      completes it switches to the queued track.

#define MUSIC_TRACK_COUNT 16
#define MUSIC_STACK_MAX 16

class MusicController {
public:
    void init();

    // FUN_100008514: advance fade animation
    void update(float dt);

    // FUN_100008568: start fading out, then clear the track stack
    void resetFade();

    // FUN_10000860c: insert a track into the playlist at a random position
    void setTrack(int trackIndex);

    // fade out the current track and queue a random gameplay track (1..N-1) in
    // its place: the crossfade used on each level change. composes resetFade +
    // setTrack, the same idiom Game::syncMusic uses for screen changes.
    void crossfadeToRandomGameplayTrack();

    // FUN_1000085a4: get the current effective volume (0-1)
    float getVolume();

    // FUN_100008800: set target volume with threshold check
    void setTargetVolume(float volume);

    // FUN_100008874: consume and clear the "changed" flag
    bool consumeChangeFlag();

    // apply current state to the audio engine (called once per frame)
    void applyToAudio();

    // get the currently playing track index (the tail; -1 if none)
    int getCurrentTrack() const;

private:
    // FUN_1000086e0: pop the playing track (tail), recycle it into the front
    // half, and return the new tail to play next (-1 if the list is empty or
    // the volume is zero).
    int advanceTrack();

    // insert a track at array index pos, shifting [pos, stackSize) up by one.
    void insertTrackAt(int pos, int trackIndex);

    // playlist (models the original's linked list; index 0 = front, the
    // highest index = tail = currently playing track).
    int trackStack[MUSIC_STACK_MAX];
    int stackSize;

    float volume;           // target volume level (0-1)
    bool changeFlag;        // set when state changes
    bool fading;            // currently fading out
    float fadeStartVolume;  // volume at start of fade
    float fadeProgress;     // 0 to 1 during fade
    bool fadeComplete;      // true when fade-out finishes
};
