#include "audio_engine.h"
#include "platform.h"
#include <SDL.h>
#include <SDL_mixer.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <vector>
#include <cstring>

// openal state (SFX)
static ALCdevice* sDevice = nullptr;
static ALCcontext* sContext = nullptr;

// 8 sources matching the original's alGenSources(8)
#define NUM_AL_SOURCES 8
static ALuint sSources[NUM_AL_SOURCES];

// loaded sound buffers
static std::vector<ALuint> sBuffers;

// sdl_mixer state (music only)
static Mix_Music* sCurrentMusic = nullptr;

// parse a WAV file header and extract PCM data for OpenAL
static bool parseWav(const unsigned char* fileData, size_t fileSize,
                     ALenum* outFormat, const unsigned char** outData,
                     ALsizei* outSize, ALsizei* outFreq) {

    if (fileSize < 44) {
        return false;
    }

    if (memcmp(fileData, "RIFF", 4) != 0 || memcmp(fileData + 8, "WAVE", 4) != 0) {
        return false;
    }

    const unsigned char* ptr = fileData + 12;
    const unsigned char* end = fileData + fileSize;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;

    while (ptr + 8 <= end) {
        uint32_t chunkSize;
        memcpy(&chunkSize, ptr + 4, 4);

        if (memcmp(ptr, "fmt ", 4) == 0 && ptr + 8 + chunkSize <= end) {
            memcpy(&channels, ptr + 10, 2);
            memcpy(&sampleRate, ptr + 12, 4);
            memcpy(&bitsPerSample, ptr + 22, 2);
        } else if (memcmp(ptr, "data", 4) == 0) {
            *outData = ptr + 8;
            *outSize = (ALsizei)chunkSize;
            *outFreq = (ALsizei)sampleRate;

            if (channels == 1 && bitsPerSample == 8) {
                *outFormat = AL_FORMAT_MONO8;
            } else if (channels == 1 && bitsPerSample == 16) {
                *outFormat = AL_FORMAT_MONO16;
            } else if (channels == 2 && bitsPerSample == 8) {
                *outFormat = AL_FORMAT_STEREO8;
            } else if (channels == 2 && bitsPerSample == 16) {
                *outFormat = AL_FORMAT_STEREO16;
            } else {
                return false;
            }

            return true;
        }

        ptr += 8 + chunkSize;

        if (chunkSize & 1) {
            ptr++;
        }
    }

    return false;
}

void AudioEngine::init() {
    // init OpenAL for sound effects
    sDevice = alcOpenDevice(nullptr);

    if (!sDevice) {
        SDL_Log("alcOpenDevice failed");
        return;
    }

    sContext = alcCreateContext(sDevice, nullptr);

    if (!sContext) {
        SDL_Log("alcCreateContext failed");
        alcCloseDevice(sDevice);
        sDevice = nullptr;
        return;
    }

    alcMakeContextCurrent(sContext);

    // generate 8 sources (matching original)
    alGenSources(NUM_AL_SOURCES, sSources);

    // init SDL_mixer for music only
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        SDL_Log("Mix_OpenAudio failed: %s", Mix_GetError());
    }

    SDL_Log("AudioEngine initialized (OpenAL SFX + SDL_mixer music)");
}

void AudioEngine::shutdown() {

    if (sContext) {
        alDeleteSources(NUM_AL_SOURCES, sSources);

        for (ALuint buf : sBuffers) {
            alDeleteBuffers(1, &buf);
        }

        sBuffers.clear();
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(sContext);
        sContext = nullptr;
    }

    if (sDevice) {
        alcCloseDevice(sDevice);
        sDevice = nullptr;
    }

    if (sCurrentMusic) {
        Mix_FreeMusic(sCurrentMusic);
        sCurrentMusic = nullptr;
    }

    Mix_CloseAudio();
}

int AudioEngine::loadSound(const char* filename) {

    if (!sContext) {
        return -1;
    }

    size_t fileSize = 0;
    unsigned char* fileData = Platform::loadAsset(filename, &fileSize);

    if (!fileData) {
        return -1;
    }

    ALenum format;
    const unsigned char* pcmData;
    ALsizei pcmSize, freq;

    if (!parseWav(fileData, fileSize, &format, &pcmData, &pcmSize, &freq)) {
        SDL_Log("Failed to parse WAV: %s", filename);
        SDL_free(fileData);
        return -1;
    }

    ALuint buffer;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, pcmData, pcmSize, freq);

    SDL_free(fileData);

    ALenum err = alGetError();

    if (err != AL_NO_ERROR) {
        SDL_Log("OpenAL error loading %s: 0x%X", filename, err);
        alDeleteBuffers(1, &buffer);
        return -1;
    }

    int handle = (int)sBuffers.size();
    sBuffers.push_back(buffer);
    return handle;
}

void AudioEngine::playSound(int handle, float gain, float pitch) {

    if (!sContext || handle < 0 || handle >= (int)sBuffers.size()) {
        return;
    }

    // find a stopped source (matching original's use of alGetSourcei)
    // fall back to source 0 if all are playing
    int sourceIdx = 0;

    for (int i = 0; i < NUM_AL_SOURCES; i++) {
        ALint state;
        alGetSourcei(sSources[i], AL_SOURCE_STATE, &state);

        if (state != AL_PLAYING) {
            sourceIdx = i;
            break;
        }
    }

    ALuint source = sSources[sourceIdx];

    alSourceStop(source);
    alSourcei(source, AL_BUFFER, (ALint)sBuffers[handle]);
    alSourcef(source, AL_GAIN, gain);
    alSourcef(source, AL_PITCH, pitch);
    alSourcei(source, AL_LOOPING, AL_FALSE);
    alSourcePlay(source);
}

// music (SDL_mixer)

void AudioEngine::playMusic(const char* filename, float volume) {

    if (sCurrentMusic) {
        Mix_FreeMusic(sCurrentMusic);
        sCurrentMusic = nullptr;
    }

    sCurrentMusic = Mix_LoadMUS(filename);

    if (!sCurrentMusic) {
        SDL_Log("Failed to load music: %s (%s)", filename, Mix_GetError());
        return;
    }

    // sdl_mixer music volume is 0-128
    Mix_VolumeMusic((int)(volume * 128.0f));
    // loop the track for the whole level; the controller swaps tracks on level /
    // screen changes (via resetFade), not when a track ends.
    Mix_PlayMusic(sCurrentMusic, -1);
}

void AudioEngine::stopMusic() {
    Mix_HaltMusic();
}

bool AudioEngine::isPlayingMusic() {
    return Mix_PlayingMusic() != 0;
}

void AudioEngine::setMusicVolume(float volume) {
    Mix_VolumeMusic((int)(volume * 128.0f));
}

// matching original's alcSuspendContext / alcProcessContext on app lifecycle
void AudioEngine::suspend() {

    if (sContext) {
        alcSuspendContext(sContext);
    }

    Mix_PauseMusic();
}

void AudioEngine::resume() {

    if (sContext) {
        alcProcessContext(sContext);
    }

    Mix_ResumeMusic();
}
