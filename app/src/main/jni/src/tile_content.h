#pragma once

#include "color_tint.h"
#include "movable_actor.h"
#include <cstdint>

// reconstructed from Ghidra:
//   ctor:           FUN_100014708
//   setType:        FUN_1000147d4 (sets +0x134, picks per-type UV/offset for
//                    the sub-icon Quad)
//   setMagnitude:   FUN_100014c6c (sets +0x13C, pushes the magnitude into the
//                    embedded ColorTint at +0x140 which renders it as digits)
//   getValues:      FUN_100014d70 (reads +0x134/+0x13C into a 2-int output)
//
// TileContent is the "what's on this hex tile" object, 0x178 bytes, owned by
// TileObject at +0x200. extends the same MovableActor base class as
// PlayerSystem and SnagContent (PTR_FUN_100074660 vtable, override at
// PTR_FUN_1000743e0).
//
// TileContent vtable @ DAT_1000743e0; see movable_actor.h for the slot
// roles + base values; only the per-class differences are noted here:
//   [2] 0x1000149d0, TileContent's specific draw (with sub-icon overlay
//                    + the embedded ColorTint's digit display).
//   [4] 0x100014a40, setPosition: baseQuad.posX/Y + propagation to the
//                    embedded ColorTint at +0x140.
//   [5] 0x100014b24, TBD; called from SnagContent's vtable[6] with
//                    (this, alpha); likely a setColor / setAlpha analog.
//   [6] 0x100014b84, fade-stage hook. the squish: writes baseQuad.scaleX
//                    /scaleY = cos-eased(fadeT) and mirrors the same scale
//                    onto the embedded ColorTint. without this the placed
//                    tile vanishes without a fade.
//   [7] 0x100014bf8, scale-out-stage hook (mirrors vtable[6] for scaleOutT).
//
// `magnitude` is the visible integer rendered on the tile by the embedded
// ColorTint's digit display:
//   - content type 2 (ATK tile): magnitude = stat-boost amount, rolled in
//                                  FUN_100020450 from player's ATK range
//                                  (statRanges[0]).
//   - content type 3 (DEF tile): magnitude = stat-boost amount from DEF range
//                                  (statRanges[1]).
//   - content type 5 (CTRL tile): magnitude = 1 (default) or 2, scaled by
//                                  perkType 0xf level: 1 = 25%, 2 = 50%,
//                                  3 = 75%, 4 = 100% chance of rolling 2.
//                                  drives the HUD's CTRL marker advance.
//   - content type 6 (HP tile):  magnitude = stat-boost amount from HP range
//                                  (statRanges[2]). HP Items scale how much
//                                  HP this tile heals when placed.
//   - other types:               magnitude = 1.
//
// Layout split:
//   - 0x000..0x12F  MovableActor base (vtable, visible, embedded sub-Quad,
//                    parent, 4-stage state-machine timers, movement queue)
//   - 0x130..0x177  TileContent-specific:
//                    +0x134  int type            content kind 0..0x19
//                    +0x138  int rawMagnitude    the rolled value, set by ctor
//                    +0x13C  int displayedMag    last magnitude pushed to
//                                                ColorTint (change detection)
//                    +0x140  ColorTint           renders digits + tints sub-Quad

// FUN_100014980. read the per-content-type icon UV / size pixel coords
// out of the table at DAT_1000787e8 (= kContentTypeUVTable in tile_content.cpp).
// writes 4 floats: uvOriginPx[2] = (uPx, vPx), uvSizePx[2] = (uExtPx, vExtPx).
// silently returns without writing when contentType >= 26 (binary's
// `param_1 < 0x1a` guard). consumed by StatBars::spawnIconBurst.
void lookupContentIconUVPx(int contentType, float* uvOriginPx, float* uvSizePx);

// the MovableActor's `baseQuad` IS the on-tile content icon for TileContent
// (the binary's per-type setup writes the icon UV/offset onto the Quad at
// +0x010, which inherits as MovableActor::baseQuad).
class TileContent : public MovableActor {
public:
    // FUN_100014708: zero the struct, run MovableActor::initBase, override
    // the vtable, init the ColorTint, store type/magnitude, register with the
    // global audio dispatcher (game+0x42f8). takes the parent pointer
    // (typically TileObject+0xE8, which ends up null in our usage).
    void init(uint32_t type, int magnitude, void* parent);

    // FUN_10001467c: lightweight init variant. runs MovableActor::initBase
    // with a null parent, zeroes type/rawMagnitude/displayedMagnitude (binary
    // writes them all as plain zero, not a -1 sentinel), inits the ColorTint.
    // does not call setType/setMagnitude and does not register with the audio
    // dispatcher. used by LevelUpPanel::init for the stat-slot icons (caller
    // calls setType explicitly after).
    void initDefault();

    // FUN_1000147d4: re-pick the sub-Quad's UV + offset based on type. pulls
    // from a 26-entry per-type lookup table at DAT_1000787e8 (stride 0x50).
    // each entry packs (uPx, vPx, uExtPx, vExtPx, offXPx, offYPx): pixel
    // coords on tiles{1..4}.png. bounded to type < 0x1A.
    void setType(uint32_t type);

    // FUN_100014c6c: push magnitude into the embedded ColorTint (which renders
    // it as visible digit Quads), with a no-op early-out if unchanged.
    void setMagnitude(int magnitude);

    // FUN_100014870: write `magnitude` to both rawMagnitude (+0x138) and
    // displayedMagnitude (via setMagnitude). also fires the Serendipity
    // achievement check (= binary's FUN_10004e36c) for XP drops grown to
    // value 4+. callers that grow / shrink a tile's stat bonus use this;
    // setMagnitude alone only updates the rendered digits and leaves
    // rawMagnitude stale.
    void setRawAndDisplayMagnitude(int magnitude);

    // FUN_1000149d0, TileContent's vtable[2] override. early-outs on
    // !visible and fadeT >= 1.0; otherwise binds tex 9 (ui1.png), draws
    // baseQuad, and conditionally draws the colorTint when the per-type
    // drawMagnitudeTint gate is set.
    void draw();

    // FUN_100014a40, TileContent's vtable[4] override. applies a small
    // per-class position offset (DAT_100059ec4 = 0.0015625 / DAT_100059ec8
    // = 0.003125), writes the adjusted (x, y) to baseQuad, then repositions
    // the embedded colorTint based on the per-type 1-digit / 2-digit
    // magnitude offset (FUN_100014a98).
    void setPosition(float x, float y) override;

    // FUN_100014b24, TileContent's vtable[5] override. propagates alpha
    // onto baseQuad and the embedded magnitude ColorTint at +0x140 so the
    // digits dim alongside the icon.
    void setAlpha(uint8_t alpha) override;

    // FUN_100014b84, TileContent's vtable[6] override. cos-eased squish:
    // baseQuad + colorTint scaleX from 1.0 -> 0.7, scaleY from 1.0 -> 0.0
    // over fadeT, mirroring the same scale onto the embedded ColorTint
    // so the magnitude digits collapse with the icon.
    void onFade(float fadeT) override;

    // FUN_100014bf8, TileContent's vtable[7] override. inverse of onFade
    // (used on revive): scaleX 0.7 -> 1.0, scaleY 0.0 -> 1.0 over scaleOutT.
    void onScaleOut(float scaleOutT) override;

private:
    // FUN_100014a98. pick the per-type magnitude-offset slot (1-digit vs
    // 2-digit form, gated on displayedMagnitude > 9) and reposition the
    // embedded ColorTint relative to baseQuad. shared by setPosition and
    // setMagnitude so the tint anchor stays correct whenever either input
    // changes.
    void repositionTintForMagnitude();

public:
    // ---- TileContent-specific (0x134..0x177; comes after MovableActor) ----
    int       type;                // +0x134  content kind (0..0x19)
    int       rawMagnitude;        // +0x138
    int       displayedMagnitude;  // +0x13C
    ColorTint colorTint;           // +0x140..+0x177
};

static_assert(sizeof(TileContent) == 0x178,
              "TileContent must be exactly 0x178 bytes");
