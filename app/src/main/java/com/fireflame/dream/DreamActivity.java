package com.fireflame.dream;

import android.os.Build;
import android.os.Bundle;
import android.view.KeyEvent;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // applicable to SDK 33+
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerBackCallback();
        }
    }

    private void registerBackCallback() {
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
            OnBackInvokedDispatcher.PRIORITY_DEFAULT,
            new OnBackInvokedCallback() {

                @Override
                public void onBackInvoked() {
                    // pass through a real back keypress whenever SDL eats one
                    // so the game doesn't close
                    SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_BACK);
                    SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_BACK);
                }
            });
    }
}
