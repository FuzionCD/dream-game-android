#pragma once

#include <SDL.h>
#include <GLES/gl.h>
#include <string>

// platform abstraction for file i/o and texture loading
namespace Platform {
    // load a file from the apk assets directory into a buffer
    // caller owns the returned pointer (free with SDL_free)
    // returns nullptr on failure, sets outSize to file length
    unsigned char* loadAsset(const char* filename, size_t* outSize);

    // load a png/jpg from assets and create an opengl texture
    // returns the gl texture id, or 0 on failure
    // sets outW/outH to the texture dimensions
    GLuint loadTexture(const char* filename, int* outW, int* outH);

    // get the writable save directory path
    // returns something like /data/data/com.fireflame.dream/files/
    std::string getSavePath();
}
