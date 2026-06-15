#include "portrait_table.h"

// reconstructed from Ghidra FUN_100056478
void setPortraitVisual(int characterIndex, Quad& q) {

    if (characterIndex < 0 || characterIndex >= 30) {
        return;
    }

    const PortraitEntry& p = sPortraits[characterIndex];

    float u0 = (float)p.x          * UV_SCALE;
    float v0 = (float)p.y          * UV_SCALE;
    float u1 = (float)(p.x + p.w)  * UV_SCALE;
    float v1 = (float)(p.y + p.h)  * UV_SCALE;

    q.setTexCoords(u0, v0, u1, v1);
    q.setSize((float)p.w / SIZE_SCALE, (float)p.h / SIZE_SCALE);
}
