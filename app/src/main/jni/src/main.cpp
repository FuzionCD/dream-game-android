#include <SDL.h>
#include <GLES/gl.h>

#include "platform.h"
#include "renderer.h"
#include "audio_engine.h"
#include "quad.h"
#include "game.h"
#include "game_board.h"
#include "save_system.h"

// must match the value in game.cpp
#ifndef NUM_KNOWN_SOUNDS
#define NUM_KNOWN_SOUNDS 5
#endif

// sdl requires this signature for the entry point
int main(int argc, char* argv[]) {

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // request opengl es 1.1 context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_Window* window = SDL_CreateWindow(
        "I Keep Having This Dream",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        0, 0,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN
    );

    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext glContext = SDL_GL_CreateContext(window);

    if (!glContext) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // get actual screen dimensions
    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    SDL_Log("Screen size: %d x %d", screenW, screenH);

    // set up initial gl state (pass nullptr for game data, we'll reinit after game creation)
    Renderer::init(screenW, screenH, nullptr);

    // init audio
    AudioEngine::init();

    Game* game = Game::create();

    if (!game) {
        SDL_Log("Failed to create game");
        SDL_Quit();
        return 1;
    }

    // running flag, declared early so the splash phases below can honor a quit
    // request that arrives during the asset load.
    bool running = true;

    // --- splash logo, drawn before the heavy asset load so the logo (not a
    //     black screen) covers it. the logo is loaded + faded in, then its
    //     texture is deleted so loadTextures() gets the game's expected GL ids
    //     (1..13, bound by literal id); the rendered frame stays on screen
    //     (it lives in the framebuffer, not the texture), so the logo holds
    //     through the whole load. when load finishes the main loop's first
    //     frame clears to black and the title fades in over it: a clean cut,
    //     no splash fade-out (the title already fades itself in).
    //     width-relative on black: width 0.814, centered at (0.5, 0.46).
    int splashW = 0, splashH = 0;
    GLuint splashTex = Platform::loadTexture("logo-1250.png", &splashW, &splashH);
    const bool haveSplash = (splashTex != 0 && splashW > 0);
    Quad splashLogo = Quad();

    if (haveSplash) {
        // match the title screen's logo cluster (curved title text + gear +
        // bullseye, title_menu.cpp construct): centered at x=0.5, bounding-box
        // center at y=0.613. placing the splash logo there means the title
        // fades in exactly behind it with no logo jump. height keeps logo-1250's
        // own aspect (no stretch).
        splashLogo.setSize(0.813f, 0.813f * (float)splashH / (float)splashW);
        splashLogo.setTexCoords(0.0f, 0.0f, 1.0f, 1.0f);
        splashLogo.posX = 0.5f;
        splashLogo.posY = 0.613f;
    }

    auto drawSplashLogo = [&](float alpha) {
        alpha = (alpha < 0.0f) ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
        splashLogo.setColor(255, 255, 255, (uint8_t)(alpha * 255.0f));
        Renderer::beginFrame();
        bindTexture(splashTex);
        splashLogo.draw();
        SDL_GL_SwapWindow(window);
    };

    // fade the logo in over 0.4s, pumping events so a quit still works.
    auto fadeInSplashLogo = [&]() {
        const float DURATION = 0.4f;
        float elapsed = 0.0f;
        Uint32 tick = SDL_GetTicks();

        while (running && elapsed < DURATION) {
            SDL_Event e;

            while (SDL_PollEvent(&e)) {

                if (e.type == SDL_QUIT) {
                    running = false;
                }
            }

            Uint32 now = SDL_GetTicks();
            elapsed += (now - tick) / 1000.0f;
            tick = now;
            drawSplashLogo(elapsed / DURATION);
            SDL_Delay(1);
        }
    };

    if (haveSplash) {
        fadeInSplashLogo();
        drawSplashLogo(1.0f);             // hold full opacity through the load
        glDeleteTextures(1, &splashTex);  // free GL id 1 for the game textures
    }

    if (!game->loadTextures()) {
        SDL_Log("Failed to load textures");
        game->destroy();
        SDL_Quit();
        return 1;
    }

    // reinit renderer now that we have the game struct, so the letterbox
    // quads can be set up
    Renderer::init(screenW, screenH, game);

    // init overlay with the actual virtual height from the renderer
    game->overlay().init(Renderer::getVirtualHeight());

    game->loadSounds();
    game->loadFonts();

    // load persistent state from internal storage. mirrors iOS view
    // viewDidLoad's NSUserDefaults read loop (at 0x10002aa10), which
    // runs after asset loading but before Game::init. each slot's loader
    // populates its matching Game region; first-launch / missing-file
    // cases are silent no-ops.
    SaveSystem::load(*game);

    game->init();

    // store the computed values in the game struct
    game->scaleFactor() = Renderer::getScaleFactor();
    game->setVirtualHeight(Renderer::getVirtualHeight());

    // the splash logo is still held on screen here. the main loop's first
    // frame clears to black (Renderer::beginFrame) and starts the title's own
    // fade-in, so the logo cuts straight to black with no splash fade-out.
    SDL_Log("Game initialized, entering main loop");

    // main loop
    Uint32 lastTick = SDL_GetTicks();

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {

            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_FINGERDOWN: {
                    game->touchBegan(event.tfinger.x, event.tfinger.y);
                    break;
                }

                case SDL_FINGERMOTION: {
                    game->touchMoved(event.tfinger.x, event.tfinger.y);
                    break;
                }

                case SDL_FINGERUP: {
                    game->touchEnded();
                    break;
                }

                case SDL_APP_WILLENTERBACKGROUND:
                    AudioEngine::suspend();
                    SDL_Log("App entering background");
                    break;

                case SDL_APP_DIDENTERFOREGROUND:
                    AudioEngine::resume();
                    SDL_Log("App entering foreground");
                    break;

                case SDL_KEYDOWN:
                    if (event.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                        // route the back button through the game's screen
                        // stack; it returns true only at the title screen
                        // (= quit the app).
                        if (game->handleBackPressed()) {
                            running = false;
                        }
                    }
                    break;
            }
        }

        // calculate delta time
        Uint32 now = SDL_GetTicks();
        float dt = (now - lastTick) / 1000.0f;
        lastTick = now;

        if (dt > 0.1f) {
            dt = 0.1f;
        }

        Renderer::beginFrame();
        game->update(dt);

        // mirror GameViewController::update's tail (0x10002af84): walk
        // the 5 save slots; if any has its dirty bit set, encode it and
        // write the resulting bytes to that slot's file. iOS calls
        // [m_prefs synchronize] once after the loop; on Android each
        // writeFile() is its own atomic rename, so no batched flush is
        // needed. Game Center upload (updateLeaderboardScores /
        // updateAchievements) has no Android consumer and is skipped.
        SaveSystem::flushDirty(*game);

        game->draw();

        SDL_GL_SwapWindow(window);
        SDL_Delay(1);
    }

    game->destroy();
    AudioEngine::shutdown();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
