#include "platform.h"
#include <SDL.h>
#include <GLES/gl.h>

// stb_image for texture loading (replaces GLKTextureLoader)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

unsigned char* Platform::loadAsset(const char* filename, size_t* outSize) {
    SDL_RWops* rw = SDL_RWFromFile(filename, "rb");

    if (!rw) {
        SDL_Log("Failed to open asset: %s (%s)", filename, SDL_GetError());
        return nullptr;
    }

    Sint64 size = SDL_RWsize(rw);

    if (size <= 0) {
        SDL_Log("Asset has invalid size: %s", filename);
        SDL_RWclose(rw);
        return nullptr;
    }

    unsigned char* data = (unsigned char*)SDL_malloc((size_t)size);

    if (!data) {
        SDL_Log("Failed to allocate %lld bytes for asset: %s", (long long)size, filename);
        SDL_RWclose(rw);
        return nullptr;
    }

    size_t bytesRead = SDL_RWread(rw, data, 1, (size_t)size);
    SDL_RWclose(rw);

    if (bytesRead != (size_t)size) {
        SDL_Log("Short read on asset: %s (got %zu, expected %lld)", filename, bytesRead, (long long)size);
        SDL_free(data);
        return nullptr;
    }

    if (outSize) {
        *outSize = (size_t)size;
    }

    return data;
}

GLuint Platform::loadTexture(const char* filename, int* outW, int* outH) {
    // load through sdl_rwops so it works with android apk assets
    size_t fileSize = 0;
    unsigned char* fileData = loadAsset(filename, &fileSize);

    if (!fileData) {
        return 0;
    }

    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(fileData, (int)fileSize, &w, &h, &channels, 4);
    SDL_free(fileData);

    if (!pixels) {
        SDL_Log("stb_image failed to decode: %s", filename);
        return 0;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    stbi_image_free(pixels);

    if (outW) {
        *outW = w;
    }

    if (outH) {
        *outH = h;
    }

    SDL_Log("Loaded texture: %s (%dx%d, id=%u)", filename, w, h, tex);
    return tex;
}

std::string Platform::getSavePath() {
    char* path = SDL_GetPrefPath("FireFlame", "Dream");

    if (!path) {
        SDL_Log("SDL_GetPrefPath failed: %s", SDL_GetError());
        return "";
    }

    std::string result(path);
    SDL_free(path);
    return result;
}
