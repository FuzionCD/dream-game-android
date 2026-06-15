#pragma once

// sound effects: OpenAL Soft (matches original iOS SoundEngine)
//   - 8 simultaneous sources (matching alGenSources(8))
//   - per-source gain and pitch control
//   - short sounds loaded entirely into AL buffers
//
// music: SDL_mixer (replaces AVAudioPlayer)
//   - MP3 streaming from disk
//   - volume control and crossfading

namespace AudioEngine {
    void init();
    void shutdown();

    // load a sound effect WAV from assets into an OpenAL buffer
    // returns a handle (buffer index) or -1 on failure
    int loadSound(const char* filename);

    // play a loaded sound effect on the next available source
    // gain: 0.0+ (values above 1.0 amplify, matching OpenAL AL_GAIN)
    // pitch: 0.5 to 2.0 (1.0 = normal speed, matching OpenAL AL_PITCH)
    void playSound(int handle, float gain, float pitch = 1.0f);

    // music (SDL_mixer, streamed from disk)
    void playMusic(const char* filename, float volume);
    void stopMusic();
    void setMusicVolume(float volume);

    // app lifecycle (matching original's alcSuspendContext / alcProcessContext)
    void suspend();
    void resume();
}
