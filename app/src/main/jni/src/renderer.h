#pragma once

#include <GLES/gl.h>

// handles opengl es 1.x setup and per-frame state.
// reconstructed from FUN_100044764 (viewport/projection setup).

// FUN_10005734c. set the active GL_TEXTURE_2D binding, with a cached-id
// short-circuit so back-to-back binds of the same texture skip the GL call.
// kept as a free function (not Renderer::) so the ~20 callsites in draw
// code stay terse.
void bindTexture(GLuint textureId);

namespace Renderer {
    // FUN_100044764: sets up viewport, projection, blend mode, clears.
    // the game uses a virtual coordinate system:
    //   X: -scaleFactor to scaleFactor + 1.0  (typically 0 to 1 with no letterboxing)
    //   Y: 0 (top) to virtualHeight (bottom)
    //   virtualHeight = screen height / screen width (the aspect ratio)
    //
    // if the aspect ratio is below 1.32 (e.g. wide tablets), letterboxing
    // kicks in: scaleFactor > 0, and black bars fill the extra space.
    //
    // gameData is the raw game struct pointer, needed to set up letterbox quads.
    // pass nullptr if not yet available (projection still works, no letterbox).
    void init(int screenW, int screenH, void* gameData);

    void beginFrame();

    int getScreenWidth();
    int getScreenHeight();
    float getVirtualHeight();
    float getScaleFactor();
}
