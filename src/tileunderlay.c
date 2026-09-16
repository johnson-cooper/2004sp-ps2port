#include <stdlib.h>

#include "allocator.h"
#include "tileunderlay.h"

TileUnderlay *tileunderlay_new(int southwestColor, int southeastColor, int northeastColor, int northwestColor, int textureId, int rgb, bool flat) {
#ifdef __PS2__
    // TileUnderlays are persistent scene terrain, never per-frame dynamic entity scaffolding. Keep
    // them in the same scene arena as the PS2 resident Ground nodes so increasing the terrain window
    // does not consume the already tiny libc heap during `Loading - please wait`.
    TileUnderlay *underlay = rs2_calloc(true, 1, sizeof(TileUnderlay));
#else
    TileUnderlay *underlay = calloc(1, sizeof(TileUnderlay));
#endif
    if (!underlay) {
        return NULL;
    }
    // underlay->flat = true;

    underlay->southwestColor = southwestColor;
    underlay->southeastColor = southeastColor;
    underlay->northeastColor = northeastColor;
    underlay->northwestColor = northwestColor;
    underlay->textureId = textureId;
    underlay->rgb = rgb;
    underlay->flat = flat;
    return underlay;
}
