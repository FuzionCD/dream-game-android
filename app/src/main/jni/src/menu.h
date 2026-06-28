#pragma once

#include "label.h"
#include "quad.h"
#include <cstdint>

// reconstructed from Ghidra:
//   init:        FUN_100057424   (Menu ctor)
//   draw:        FUN_100057878   (chrome only: bgDim + frame9slice + title + close)
//   baseUpdate:  FUN_1000578f8   (fade animation + confirm-button tap state machine)
//   panelShow:   FUN_100057ac8   (visible=1, animTimer0=0, sound 8)
//   panelHide:   FUN_100057b1c   (visible=0 / secondaryVisible=0 + sound 8)
//   setAlpha:    FUN_100057b58   (bgDim half + chrome full)
//   setSize:     FUN_100057b98   (frame9slice + title/close/confirm positioning)
//   setAnchorY:  FUN_100057c54   (pixel-snapped Y anchor)
//
// binary's typeinfo: this is the `Menu` base class with siblings `MenuLevel`,
// `MenuSettings`, `MenuItems`, `MenuForfeit`. each subclass owns its own
// content area; the 0x420-byte Menu header owns the shared
// chrome and fade state.
//
// virtual hooks (vtable[3..5] in binary):
//   - update(dt, touchInput)   vtable[3]. subclass per-frame logic.
//   - setAlpha(a)              vtable[4]. subclass fades its content; base
//                                impl fades the chrome.
//   - onConfirmTapped()        vtable[5]. fires when player taps + releases
//                                over the confirm button while readyByte set.

// scalar pixel-snap helper (binary FUN_100057374). matches Quad::snapToPixelGrid's
// grid (round to nearest 1/640). exposed to Menu subclasses for content layout
// math that needs the same snap as the chrome positioning.
float snapMenuPixel(float v);

class Menu {
public:

    // FUN_100057424. shared chrome setup. titleAtlasX/Y/W/H are the atlas-pixel
    // coordinates of the per-subclass title bar (e.g. (641, 906, 251, 48) for
    // MenuLevel). bgDim, frame9slice, closeBg, confirmButton get their fixed
    // setup; titleQuad's UV/size derive from the caller's args.
    void init(int titleAtlasX, int titleAtlasY, int titleAtlasW, int titleAtlasH);

    // FUN_100057878. base chrome draw; subclasses call this then draw their
    // own slot content. binds tex 0 for bgDim, then under a glPushMatrix
    // translated by (anchorX, anchorY), draws frame9slice + titleQuad + (when
    // readyByte is set) closeBg + confirmButton.
    void draw();

    // FUN_1000578f8. shared per-frame logic: drives the cosine-eased fade
    // animation and the confirm-button tap state machine. dispatches setAlpha
    // virtually (so subclass fades its content too). subclass update()
    // overrides typically call this before their own per-frame work.
    void baseUpdate(float dt);

    // FUN_100057b98. sizes frame9slice + positions title/close/confirm
    // relative to the (w, h) panel rect, then snaps each Quad's anchor to
    // the 1/640 pixel grid. writes anchorX = snap(0.5 - w*0.5).
    void setSize(float panelWidth, float panelHeight);

    // FUN_100057c54. writes pixel-snapped anchorY.
    void setAnchorY(float y);

    // Android adapter for the iOS binary's hardcoded anchor-Y constants.
    // iOS targets a 4:3 aspect (virtualHeight ~= 1.5) so the per-subclass
    // anchor Y values land the panel vertically centered there; on
    // Android (~9:19.5, virtualHeight ~= 2.2) those same constants push
    // the panel up against the top. derive the anchor from the runtime
    // virtualHeight so the panel stays vertically centered on any
    // aspect ratio: anchorY = (virtualHeight - panelHeight) / 2.
    //
    // also calls setSize(panelWidth, panelHeight); most subclasses pair
    // the two calls anyway. for the rare case where a subclass needs an
    // off-center placement, call setSize + setAnchorY directly.
    void setSizeAndCenterY(float panelWidth, float panelHeight);

    // FUN_100057ac8. visible flag set, fade timer reset, readyByte cleared,
    // confirmButton restored to full white, sound 8 fires.
    void panelShow();

    // FUN_100057b1c. `justClearVisible` mirrors the binary's two-arg variants:
    //   true  -> just clear visible (used for the deferred-close animation tail)
    //   false -> full close (clear secondaryVisible + animTimer0 + readyByte,
    //                        play sound 8).
    void panelHide(bool justClearVisible);

    // ---- virtual hooks (vtable[3..5] in binary) ----

    // vtable[3]. subclass per-frame logic. default does nothing.
    virtual void update(float dt, float touchInput) { (void)dt; (void)touchInput; }

    // vtable[4]. base impl fades bgDim (half) + frame9slice + titleQuad to `a`.
    // subclasses override to also fade their slot content.
    virtual void setAlpha(uint8_t a);

    // vtable[5]. fires when player taps + releases over the confirm button.
    // default does nothing.
    virtual void onConfirmTapped() {}

    virtual ~Menu() = default;

    // ---- byte-exact field layout (Menu == 0x420 bytes) ----
    //
    // vtable pointer (8 bytes, implicit from virtual methods,
    // matches the binary's vtable field).

    bool         visible;          // =1 while panel is on screen
    float        anchorX;          // panel screen position X (glTranslatef)
    float        anchorY;          // panel screen position Y
    bool         secondaryVisible; // fade direction (1 = open/opening,
                                   //                        0 = closing)
    float        animTimer0;       // fade progress 0..1

    // bgDim Quad, fullscreen dark backdrop. setSize(1.0, virtualHeight),
    // setColor(0xff000000). drawn first (under tex 0) before the chrome push.
    Quad         bgDim;            // (0xD8 bytes; trailing 0x10
                                   //                  holds the animation target
                                   //                  rect, see quad.h)

    // frame9slice, chrome border. Label with 9 addGlyph slots
    // arranged as a 3x3 stretchable border on atlas pixels (72..170, 351..449).
    Label        frame9slice;

    // titleQuad, per-subclass title bar. UV+size set from init()'s
    // (x, y, w, h) atlas-pixel literals via the pixel-rect helper.
    Quad         titleQuad;

    // closeBg Quad, round backdrop behind confirmButton. drawn only
    // when readyByte is set.
    Quad         closeBg;

    // confirmButton Quad. hit-tested by baseUpdate against
    // touch coords when readyByte is set.
    Quad         confirmButton;

    bool         readyByte;        // =1 once subclass enables commit;
                                   //         confirm button accepts taps.
                                   //         also gates closeBg + confirm draw.
    bool         confirmPressed;   // =1 while touch is held over
                                   //         confirm button; latched so a
                                   //         tap-and-drag-off doesn't fire
                                   //         vtable[5].
};
