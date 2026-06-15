#pragma once

#include "quad.h"
#include <cstdint>
#include <list>

// Payload type for the MovableActor move queue at +0x110. each queued entry
// is the target screen position (posX, posY) of one step in a multi-step
// movement. listed in chronological order: front() = next step, back() =
// final destination.
struct MoveTarget {
    float posX;   // +0x00
    float posY;   // +0x04

    MoveTarget() : posX(0.0f), posY(0.0f) {}
    MoveTarget(float x, float y) : posX(x), posY(y) {}
};
static_assert(sizeof(MoveTarget) == 0x8,
              "MoveTarget must be 8 bytes (matches binary's 8-byte node payload)");
static_assert(sizeof(std::list<MoveTarget>) == 0x18,
              "std::list head must be 24 bytes (libc++ aarch64)");

// reconstructed from Ghidra:
//   ctor:           FUN_100038b18 (called by PlayerSystem, TileContent,
//                    SnagContent, and any other "movable actor" subclass)
//   update vtable:  FUN_100038bac (the 4-stage state-machine update)
//   draw vtable[2]: FUN_100038b88 (early-out if hidden+faded, else draw Quad)
//
// MovableActor is the shared 0x134-byte base class for the game's animatable
// entities: anything that has a Quad sprite, lives at a position, can spawn
// in / move along a path / fade / scale-out, and tracks its own movement
// queue. Subclasses extend this with their own state starting at +0x134.
//
// vtable in the binary: PTR_FUN_100074660 (overridden per subclass to e.g.
// PTR_FUN_1000743e0 for TileContent or PTR_FUN_1000746e0 for SnagContent).
// We don't dispatch through the vtable in our port; methods are called
// directly, so the vtable field is kept as a placeholder for byte-exact
// layout but holds nullptr.
//
// MovableActor base vtable @ DAT_100074660 (12 entries, 8 bytes each):
//   [0] 0x100038edc, dtor (delete + free)
//   [1] 0x100038f20, placement dtor (call dtor only, no free)
//   [2] 0x100038b88, draw (early-out !visible+faded, else baseQuad.draw).
//                    ported as MovableActor::draw / inherited everywhere.
//   [3] 0x100038bac, update (4-stage state machine = MovableActor::update).
//                    overridden by SnagContent (= 0x10003da34).
//   [4] 0x100038ec4, setPosition (writes baseQuad.posX/Y from a float pair).
//                    overridden by TileContent (0x100014a40), SnagContent
//                    (0x10003dc38 = SnagContent::setPosition, ported).
//   [5] 0x100038f64, TBD, base. overridden by TileContent (0x100014b24),
//                    SnagContent (0x10003dd80).
//   [6] null,        fade-stage hook. overridden by TileContent
//                    (0x100014b84 = the squish/scale-down + ColorTint scale)
//                    and SnagContent (0x10003de0c = call vtable[5] with alpha
//                    = (1-fadeT) * DAT_a344).
//   [7] null,        scale-out-stage hook. overridden by TileContent
//                    (0x100014bf8) and SnagContent (0x10003de40).
//   [8] 0x10,        appears to be size info / type tag, not a function.
//   [9] 0x10005a250, pointer into __TEXT,__const (likely a string).
//   [10] 0x10007af90, pointer into __DATA, possibly RTTI.
//   [11] 0x10007b0e0, same shape, possibly type-name string.
//
// when porting a future vfunc, decompile the address above + add it as a
// real method. for the squish on tile consume the vtable[6] hook is the
// pivotal one; its absence is why placed tiles vanish without a fade.
//
// Subclasses use MovableActor in two ways:
//   1. inherit publicly for non-trivial subclasses (PlayerSystem, TileContent,
//      SnagContent) so they get the layout and the initBase() helper.
//   2. compose / mirror manually if a class predates the refactor (left as-is
//      for now).
//
// Shared base layout:
//   +0x000  vtable                (8 bytes)
//   +0x008  visible               (1 byte) + 7 bytes pad
//   +0x010  baseQuad              (0xD8 bytes, the rendered sprite; the
//                                   trailing 0x10 holds the Quad's animation
//                                   target rect, see quad.h)
//   +0x0E8  gridCol               (4 bytes, placed-on-board hex column,
//                                   cached from the parent TileObject's
//                                   +0xE8/+0xEC by initBase via 8-byte copy
//                                   from `*parent_ptr`. then re-synced by
//                                   FUN_10001349c at every page-commit so
//                                   sub-actors track their parent's hex coord.
//                                   initBase copies the pointed-to 8 bytes,
//                                   not the pointer itself.)
//   +0x0EC  gridRow               (4 bytes, paired with gridCol above)
//   +0x0F0  spawnT                (4 bytes, init 1.0 = "no spawn anim pending")
//   +0x0F4  spawnFromX            (4 bytes)
//   +0x0F8  spawnFromY            (4 bytes)
//   +0x0FC  spawnToX              (4 bytes)
//   +0x100  spawnToY              (4 bytes)
//   +0x104  moveT                 (4 bytes, init 1.0 = "no move anim pending")
//   +0x108  moveFromX        (4 bytes)
//   +0x10C  moveFromY        (4 bytes)
//   +0x110  moveQueue             (24 bytes, std::list<MoveTarget> head:
//                                   sentinel.prev / sentinel.next / size)
//   +0x128  moveStepRate          (4 bytes, pace multiplier on the
//                                   move-anim's per-frame moveT advance.
//                                   stepToward writes (float)moveQueue.size()
//                                   here; the move-anim reads it as
//                                   `(dt / 0.3) * moveStepRate`.
//                                   larger = faster animation.)
//   +0x12C  fadeT                 (4 bytes, init 1.0)
//   +0x130  scaleOutT             (4 bytes, init 1.0)
//
// Total: 0x134 bytes.

class MovableActor {
public:
    virtual ~MovableActor() = default;

    // FUN_100038b18: zero the struct, set visible=1, init baseQuad,
    // copy the parent's packed (col, row) pair into gridCol/gridRow (8-byte
    // dereference of parentCoordPtr; null = leave coords at 0), init the
    // 4 stage timers to 1.0, set up the movement queue sentinel. Subclasses
    // call this from their own init() and then populate their derived state.
    void initBase(const void* parentCoordPtr);

    // FUN_100038e8c: kill / re-roll. clears visible and resets fadeT to 0,
    // which kicks off the fade-out animation in update(); each frame the
    // fade stage advances fadeT and dispatches onFade for the visual.
    void killAndFade();

    // FUN_100038ea4: respawn / revive. sets visible to 1 and scaleOutT to
    // -2.0 (so the scale-out animation runs in reverse for a re-spawn pop).
    // each frame the scale-out stage advances scaleOutT and dispatches
    // onScaleOut for the visual.
    void revive();

    // FUN_100038db4. kicks a one-shot spawn-in animation that lurches the
    // sprite toward a hex direction (0..5 = neighbor directions).
    //   spawnT      = 0
    //   spawnFrom   = current (posX, posY)
    //   spawnTo     = spawnFrom + 0.07 * (cos(angle), sin(angle))
    //   angle       = (direction - 1) * 60 degrees (in radians)
    // used by FUN_100025dcc on snag death: the player and the dying snag
    // both bump in opposite directions to visualize the combat hit.
    void triggerBumpAnim(int direction);

    // FUN_100038e10. pushes a (posPair, gridPair) target onto the move queue.
    // kicks the move animation (moveT = 0, moveFrom = current pos), pushes
    // a MoveTarget onto the queue back, updates gridCol/Row from gridPair,
    // and writes moveStepRate = (float)moveQueue.size().
    // posPair: pointer to (posX, posY) float pair (target screen coords).
    // gridPair: pointer to (gridCol, gridRow) int pair (target hex coords).
    void stepToward(const float* posPair, const int* gridPair);

    // FUN_100038bac. base 4-stage state-machine update. spawn-in lerp
    // (spawnT), eased move (moveT), fade-out (fadeT), scale-out (scaleOutT).
    // each stage advances its own timer and returns when the stage is
    // mid-animation; only the active stage runs per-frame. virtual so derived
    // classes (SnagContent) can override to add stage-completion hooks (e.g.
    // SnagContent's move-completion reattaches the snag to target tile's
    // snagContent slot; that's how "chase" actually rewires the world).
    // TileObject dispatches update on its content / snagContent /
    // trackedContent[*], mirroring FUN_1000124ac's `(*vtable[3])(this)`.
    virtual void update(float dt);

    // FUN_100038ec4, vtable[4] base. writes baseQuad.posX/Y, snaps to the
    // pixel grid via FUN_1000573a8. derived overrides (TileContent's vtable
    // [4] = FUN_100014a40, SnagContent's = FUN_10003dc38) extend this to
    // propagate the position onto child sub-quads. dispatched per-frame
    // from MovableActor::update's spawn-in and move-anim stages so children
    // follow the parent during animation.
    virtual void setPosition(float x, float y);

    // FUN_100038f64, vtable[5] base. propagates an alpha byte onto the
    // embedded baseQuad's 4 vertices. derived overrides extend it to also
    // dim the icon / stat displays / ColorTints (TileContent's vtable[5]
    // = FUN_100014b24; SnagContent's = FUN_10003dd80). PlayerSystem
    // inherits this base, so the player avatar dims via this same hook.
    virtual void setAlpha(uint8_t alpha);

    // vtable[6] / vtable[7] hooks: derived classes apply per-frame visual
    // fade (squish, alpha-down) keyed off fadeT / scaleOutT. base no-op
    // matches MovableActor's null vtable[6] / vtable[7] entries.
    virtual void onFade(float fadeT)        { (void)fadeT; }
    virtual void onScaleOut(float scaleOutT) { (void)scaleOutT; }

    // ---- byte-exact base class layout ----
    // implicit C++ vtable pointer occupies +0x00..+0x07 (the binary's
    // vtable slot). visible follows at +0x008; the rest of the layout
    // matches the binary byte-for-byte.
    bool visible;                  // +0x008
    uint8_t pad009[7];             // +0x009..+0x00F
    Quad baseQuad;                 // +0x010..+0x0E7 (0xD8, owns anim rect at +0xD8)
    int gridCol;                   // +0x0E8 (cached from parent at initBase, re-synced at commit)
    int gridRow;                   // +0x0EC
    float spawnT;                  // +0x0F0
    float spawnFromX;              // +0x0F4
    float spawnFromY;              // +0x0F8
    float spawnToX;                // +0x0FC
    float spawnToY;                // +0x100
    float moveT;                   // +0x104
    float moveFromX;               // +0x108
    float moveFromY;               // +0x10C
    // pending-move queue (std::list head, 24 bytes total). populated by
    // stepToward() with target (posX, posY) entries; the move stage drains
    // it from the front each time animT hits 1.0.
    std::list<MoveTarget> moveQueue;  // +0x110..+0x127
    float moveStepRate;            // +0x128
    float fadeT;                   // +0x12C
    float scaleOutT;               // +0x130
};

// the binary's MovableActor stores 0x134 bytes of fields, but C++ rounds the
// type's `sizeof` up to 0x138 (multiple of 8) because the largest member
// alignment is 8 (void*, int64_t). this matches the binary; `sizeof` is the
// same on iOS clang. inheritance later compresses derived classes back: the
// C++ tail-padding rule lets a subclass's first data member occupy the base's
// trailing 4 bytes, so e.g. PlayerSystem's characterIndex lands at +0x134
// (not +0x138). composition does not get this optimization.
static_assert(sizeof(MovableActor) == 0x138,
              "MovableActor type size must be 0x138 (0x134 useful fields + 4 bytes trailing alignment padding)");
