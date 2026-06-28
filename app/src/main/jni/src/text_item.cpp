#include "bmfont_table.h"
#include "text_item.h"
#include "renderer.h"   // bindTexture
#include <GLES/gl.h>
#include <cstring>

namespace {

// inline-tag UV/size constants. extracted from the iOS binary's __TEXT,__const
// at 0x10005a138..0x10005a29c (12 floats), consumed by FUN_10002fe60 (parseInlineTag)
// + FUN_100030204 (emitInlineIcon). values are pixel coordinates on the bitmap
// font sheet; converted to UV via TEXT_PIXEL_UV_SCALE during glyph placement.
//
// each {tag} character pulls a (uvU, uvV, uvWidth, uvHeight) tuple from these
// constants. {A}/{C}/{D}/{H} share TAG_DEFAULT_UV_SIZE (=46) as their uvHeight
// (and as C's uvWidth; C is a 46x46 square icon). {X} sits immediately right
// of C on the atlas at U=487 with a 38x47 footprint.
//
// per-tag UV bounds on the atlas (in pixels):
//   {A} = (186, 0)..(186+48, 0+46)   = ATK glyph
//   {C} = (440, 20)..(440+46, 20+46) = Control glyph
//   {D} = (287, 0)..(287+44, 0+46)   = DEF glyph
//   {H} = (235, 0)..(235+50, 0+46)   = HP glyph
//   {X} = (487, 20)..(487+38, 20+47) = XP glyph
constexpr float TAG_X_UV_U            = 487.0f;
constexpr float TAG_X_UV_WIDTH        = 38.0f;
constexpr float TAG_X_UV_HEIGHT       = 47.0f;
constexpr float TAG_C_UV_U            = 440.0f;
constexpr float TAG_DEFAULT_UV_SIZE   = 46.0f;
                                                       //   (uvHeight for A/C/D/H,
                                                       //    also uvWidth for C)
constexpr float TAG_H_UV_U            = 235.0f;
constexpr float TAG_H_UV_WIDTH        = 50.0f;
constexpr float TAG_D_UV_U            = 287.0f;
constexpr float TAG_D_UV_WIDTH        = 44.0f;
constexpr float TAG_A_UV_U            = 186.0f;
constexpr float TAG_A_UV_WIDTH        = 48.0f;
constexpr float TAG_ADVANCE_DIVISOR   = 640.0f;

// FUN_100014d84 helper constants, pixel-to-UV / pixel-to-display scaling.
// the bitmap font atlas is 1024 wide; the display reference width is 640.
constexpr float TEXT_PIXEL_UV_SCALE   = 0.0009765625f; // (= 1/1024)
constexpr float TEXT_PIXEL_SIZE_SCALE = 640.0f;

// emit a single inline-icon glyph into the TextItem at the next outline-tail
// slot. port of FUN_100030204:
//   - bump outline-tail count (outlineGlyphCount)
//   - compute slot index = vec.size - new outlineCount  (icons land at the
//     end of the glyph vector, draw()'s outline pass walks them in reverse)
//   - bump cursor by half-advance (centers the glyph)
//   - set Quad UV / size / pos via the FUN_100014d84 pixel-to-UV transform
//   - apply RGB white + current TextItem alpha
//   - bump cursor by half-advance again
void emitInlineIcon(float uvMinX, float uvMinY, float uvWidth, float uvHeight,
                    TextItem* item, float* cursorX) {
    float halfAdvance = ((uvWidth / TAG_ADVANCE_DIVISOR) * 10.0f) / 3.0f;
    item->outlineGlyphCount += 1;

    // place the icon at the trailing tail of the glyph vector (not at the
    // primary count). this is what draw()'s outline pass reads back from.
    int64_t idx = (int64_t)item->glyphVec.size() - item->outlineGlyphCount;
    *cursorX += halfAdvance;
    TileIcon& glyph = item->glyphVec[(size_t)idx];

    // FUN_100014d84 inlined: pixel -> UV/display coords.
    float u0 = uvMinX * TEXT_PIXEL_UV_SCALE;
    float v0 = uvMinY * TEXT_PIXEL_UV_SCALE;
    float u1 = (uvMinX + uvWidth)  * TEXT_PIXEL_UV_SCALE;
    float v1 = (uvMinY + uvHeight) * TEXT_PIXEL_UV_SCALE;
    glyph.quad.setTexCoords(u0, v0, u1, v1);

    float w = (uvWidth  * 10.0f) / TEXT_PIXEL_SIZE_SCALE;
    float h = (uvHeight * 10.0f) / TEXT_PIXEL_SIZE_SCALE;
    glyph.quad.setSize(w, h);

    glyph.quad.posX = *cursorX;
    glyph.quad.posY = (uvHeight * -2.0f) / TAG_ADVANCE_DIVISOR;

    glyph.quad.setColor(0xFF, 0xFF, 0xFF, item->alpha);

    // binary order: cursor advance then charBuffer stamp.
    *cursorX += halfAdvance;

    // mirror the byte-buffer push: stamp 0xFF at index `idx` if there's room.
    if ((size_t)idx < item->charBuffer.size()) {
        item->charBuffer[(size_t)idx] = (char)0xFF;
    }
}

// parse an inline tag like {A} / {C} / {D} / {H} / {X} starting at param_2
// (which points at the '{'). port of FUN_10002fe60:
//   - decrement glyphCount per char as we consume tag chars (compensating
//     for the per-char decrement the outer caller already did)
//   - on each known-letter, call emitInlineIcon with that letter's UV tuple
//   - terminate on '}' or end-of-string
void parseInlineTag(TextItem* item, const char* tagStart, float* cursorX) {
    const char* p = tagStart;

    // the binary loop pre-decrements glyphCount on every iteration (mirroring
    // the outer setString loop's per-char post-increment). we mirror that.
    while (true) {
        char c = *p;
        item->glyphCount -= 1;

        if (c == '\0' || c == '}') {
            return;
        }

        if (c >= 'A' && c < (char)0x7d) {
            switch (c) {
            case 'A':   // ATK glyph
                emitInlineIcon(TAG_A_UV_U, 0.0f, TAG_A_UV_WIDTH, TAG_DEFAULT_UV_SIZE,
                               item, cursorX);
                break;
            case 'C':   // Control glyph (square: uvWidth = uvHeight = 46)
                emitInlineIcon(TAG_C_UV_U, 20.0f, TAG_DEFAULT_UV_SIZE, TAG_DEFAULT_UV_SIZE,
                               item, cursorX);
                break;
            case 'D':   // DEF glyph
                emitInlineIcon(TAG_D_UV_U, 0.0f, TAG_D_UV_WIDTH, TAG_DEFAULT_UV_SIZE,
                               item, cursorX);
                break;
            case 'H':   // HP glyph
                emitInlineIcon(TAG_H_UV_U, 0.0f, TAG_H_UV_WIDTH, TAG_DEFAULT_UV_SIZE,
                               item, cursorX);
                break;
            case 'X':   // XP glyph (different uvHeight from the rest)
                emitInlineIcon(TAG_X_UV_U, 20.0f, TAG_X_UV_WIDTH, TAG_X_UV_HEIGHT,
                               item, cursorX);
                break;
            default:
                // unknown letter; skip silently. the binary's case statement
                // also has no default, so unknown letters within a tag fall
                // through to the next char without emitting anything.
                break;
            }
        }

        p++;
    }
}

}  // anonymous namespace

// FUN_10002fa08, basic init. the binary memsets bytes 0..0x5F, then sets:
//   scaleX = scaleY = 1.0
//   rotation = 0, rgba = white
//   renderedWidth = 0  (4 bytes only)
//   spaceMultiplier = 2.0
//
// fields the binary leaves untouched:
//   posX, posY, maxCharHeight, and the trailing padding bytes.
//   panels are responsible for setting posX/posY before draw(); maxCharHeight
//   gets reset to 0 on every setString call.
//
// in C++, the std::string + std::vector members are already
// in their empty-valid state via their default constructors; no memset needed
// (and writing memset over them would corrupt their internal control blocks).
// the trailing scalars that the binary's memset clears are
// explicitly zeroed below.
void TextItem::init() {
    // bytes 0x48..0x5F (matches binary memset of 0x60 stopping after these):
    glyphCount        = 0;
    outlineGlyphCount = 0;
    glyphTablePtr     = nullptr;

    // explicit field writes (matches binary's stp/str sequence post-memset):
    scaleX            = 1.0f;
    scaleY            = 1.0f;
    rotation          = 0.0f;
    rgba              = 0xFFFFFFFFu;
    renderedWidth     = 0.0f;
    spaceMultiplier   = 2.0f;
    // posX / posY / maxCharHeight / pad84 deliberately left untouched,
    // matching the binary. embedding-side ctors zero these via the
    // panel's overall init pattern.
}

// FUN_10002fa50, same as init() but stores the glyph table in glyphTablePtr.
// the binary memsets bytes 0..0x57 only, then sets:
//   glyphTablePtr = param_2
//   scaleX = scaleY = 1.0
//   rotation = 0, rgba = white
//   renderedWidth = maxCharHeight = 0  (8-byte store clears both)
//   spaceMultiplier = 2.0
//
// difference from init(): glyphTablePtr is set explicitly (not zeroed) and
// maxCharHeight is zeroed (the 8-byte store). posX/posY and the trailing
// padding still left untouched.
void TextItem::init(const BMFontTable* glyphTable) {
    glyphCount        = 0;
    outlineGlyphCount = 0;
    glyphTablePtr     = glyphTable;
    scaleX            = 1.0f;
    scaleY            = 1.0f;
    rotation          = 0.0f;
    rgba              = 0xFFFFFFFFu;
    renderedWidth     = 0.0f;
    maxCharHeight     = 0.0f;     // unique to this variant
    spaceMultiplier   = 2.0f;
}

// FUN_10002fae8, populate the glyph vector from a string.
//
// the binary trusts `s` is non-null (its `std::string::compare(this, s)` would
// crash on null). the null guards below are defensive, not in the binary, but
// cheap insurance against caller bugs in the panel-side wiring.
void TextItem::setString(const char* s, int len) {
    if (s == nullptr) {
        return;   // defensive, not in binary
    }

    // 1. early-out if the new string matches the stored one.
    if (storedText == s) {
        return;
    }

    // 2. determine length.
    size_t sLen = (len < 0) ? std::strlen(s) : (size_t)len;

    // 3. grow glyph vector to at least sLen elements. each new element is
    // a default-constructed TileIcon (a Quad plus 0x10 zero bytes).
    if (glyphVec.size() < sLen) {
        size_t prevCount = glyphVec.size();
        glyphVec.resize(sLen);
        // apply current color to the newly-created glyphs.
        for (size_t i = prevCount; i < sLen; i++) {
            glyphVec[i].quad.setColor(colorR, colorG, colorB, alpha);
        }
    }

    // 4. set glyphCount; reset outline state and rendered metrics.
    glyphCount        = (int64_t)sLen;
    outlineGlyphCount = 0;
    renderedWidth     = 0.0f;
    maxCharHeight     = 0.0f;

    // 5. walk input string char-by-char, emitting glyphs.
    float cursorX = 0.0f;

    if (sLen == 0) {
        // empty string; skip the walk, fall through to step 7.
    } else {
        const BMFontTable* table = glyphTablePtr;
        float perTableMul = table->perTableMul;

        size_t glyphIdx = 0;
        bool   inTag    = false;

        for (size_t k = 0; k < sLen; k++) {
            char c = s[k];

            if (inTag) {
                inTag = (c != '}');
                continue;
            }

            if (c == '{') {
                parseInlineTag(this, &s[k], &cursorX);
                inTag = true;
                continue;
            }

            // resolve the glyph entry. the binary sign-extends the char (ldrsb)
            // before indexing, so bytes >= 0x80 read before the entries array
            // (benign; high bytes are rarely rendered).
            const BMFontEntry& e = table->entries[(signed char)c];

            // diff check: if the char at glyphIdx differs from the last
            // setString call's result (or this is a fresh slot), re-set the
            // glyph's UV / size / offset. matches binary's avoid-redundant-
            // work optimization at FUN_10002fae8 line 10002fc88.
            bool needsRebuild = (glyphIdx >= charBuffer.size())
                             || (charBuffer[glyphIdx] != c);

            if (needsRebuild) {
                Quad& q = glyphVec[glyphIdx].quad;
                q.setTexCoords(e.uvU, e.uvV, e.uvU2, e.uvV2);
                q.setSize(e.sizeW, e.sizeH);
                q.addVertexOffset(e.xoffsetHalved, e.yoffsetCenter);
            }

            // unconditional per-char updates (matches binary's flow at fd30
            // onward, which always runs regardless of the diff check).
            float entryHeight = e.sizeH;
            if (maxCharHeight < entryHeight) {
                maxCharHeight = entryHeight;
            }

            float halfW = e.sizeW * 0.5f;
            cursorX += halfW;
            glyphVec[glyphIdx].quad.posX = cursorX;
            glyphVec[glyphIdx].quad.posY = 0.0f;

            float adv = e.xadvanceHalved;
            if (c == ' ') {
                adv *= spaceMultiplier;
            }

            cursorX += adv * perTableMul;

            // sync the char buffer at this index. matches the binary's
            // unconditional `charBuffer[i] = c` write at line 10002fd9c.
            if (charBuffer.size() <= glyphIdx) {
                charBuffer.push_back(c);
            } else {
                charBuffer[glyphIdx] = c;
            }

            glyphIdx++;
        }
    }

    // 6. store final cursor as rendered width.
    renderedWidth = cursorX;

    // 7. update storedText.
    storedText = s;
}

// FUN_100030014, bind texture, push transform, draw glyphs (forward primary
// + reverse outline tail), pop transform.
//
// the binary makes the gl* calls unconditionally (no scaleX!=1 / rotation!=0
// guards) and uses Z=0 for glScalef. for 2D rendering glScalef(x,y,0) is
// behaviorally identical to glScalef(x,y,1) because we never draw 3D geometry,
// but we mirror the binary exactly.
void TextItem::draw() {
    // gate: if both counts are zero, nothing to draw (matches binary's check
    // at the top of FUN_100030014).
    if (glyphCount == 0 && outlineGlyphCount == 0) {
        return;
    }

    glPushMatrix();
    glTranslatef(posX, posY, 0.0f);
    glScalef(scaleX, scaleY, 0.0f);
    glRotatef(rotation, 0.0f, 0.0f, 1.0f);

    // bind tex `glyphTablePtr->textureIndex + 1`. binary derefs without null
    // check; contract is that the panel-side init wrote a valid glyph table
    // here before draw could fire (panels stay hidden, so draw doesn't run
    // until a panel becomes visible, which only happens post-loadFonts).
    bindTexture((GLuint)(glyphTablePtr->textureIndex + 1));

    // primary pass: forward iter over the first `glyphCount` glyphs.
    for (int64_t i = 0; i < glyphCount; i++) {
        glyphVec[(size_t)i].quad.draw();
    }

    // outline pass: bind tex 9, iterate the trailing tail in reverse.
    if (outlineGlyphCount != 0) {
        bindTexture(9);

        for (int64_t i = 0; i < outlineGlyphCount; i++) {
            // binary walks via lVar1 = -1 then lVar1 += -1 with index =
            // (size - lVar1 - 1), equivalent to iterating from
            // (size - 1) downward `outlineGlyphCount` times.
            size_t idx = glyphVec.size() - 1 - (size_t)i;
            glyphVec[idx].quad.draw();
        }
    }

    glPopMatrix();
}

// FUN_100030144, apply current rgba to primary glyphs, current alpha to
// outline tail. used as the propagation step after rgba/alpha mutations.
void TextItem::applyColor() {
    // primary: full color apply.
    for (int64_t i = 0; i < glyphCount; i++) {
        glyphVec[(size_t)i].quad.setColor(colorR, colorG, colorB, alpha);
    }

    // outline tail: alpha-only.
    if (outlineGlyphCount != 0) {
        for (int64_t i = 0; i < outlineGlyphCount; i++) {
            size_t idx = glyphVec.size() - 1 - (size_t)i;
            glyphVec[idx].quad.setAlpha(alpha);
        }
    }
}

// FUN_1000301fc, write alpha byte, propagate.
void TextItem::setAlpha(uint8_t a) {
    alpha = a;
    applyColor();
}
