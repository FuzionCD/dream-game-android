package com.fireflame.dream;

import org.libsdl.app.SDLActivity;

public class DreamActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "SDL2_mixer",
            "main"
        };
    }
}
