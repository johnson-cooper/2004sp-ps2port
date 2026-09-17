#pragma once

#include <stdint.h>

#include "datastruct/linkable.h"
#include "decor.h"
#include "grounddecor.h"
#include "groundobject.h"
#include "location.h"
#include "tileoverlay.h"
#include "tileunderlay.h"
#include "wall.h"

typedef struct Ground Ground;
struct Ground {
    Linkable link;
#ifdef __PS2__
    // Ground is one of the most numerous persistent scene objects on the 32 MiB PS2.
    // Keep pointer fields together, then pack the small tile/order state at the tail.
    // These ranges are intrinsic to this scene format: x/z are 0..103, levels are
    // 0..3, locCount is 0..5, locSpan/locSpans/check/block/inverse are 4-bit masks,
    // and backWallTypes is an 8-bit wall mask.  The desktop layout remains unchanged.
    TileUnderlay *underlay;
    TileOverlay *overlay;
    Wall *wall;
    Decor *decor;
    GroundDecor *groundDecor;
    GroundObject *groundObj;
    Location *locs[5];
    Ground *bridge;

    uint8_t level;
    uint8_t x;
    uint8_t z;
    uint8_t occludeLevel;
    uint8_t locCount;
    uint8_t locSpan[5];
    uint8_t locSpans;
    uint8_t drawLevel;
    bool visible;
    bool update;
    bool containsLocs;
    uint8_t checkLocSpans;
    uint8_t blockLocSpans;
    uint8_t inverseBlockLocSpans;
    uint8_t backWallTypes;
#else
    int level;
    int x;
    int z;
    int occludeLevel;
    TileUnderlay *underlay;
    TileOverlay *overlay;
    Wall *wall;
    Decor *decor;
    GroundDecor *groundDecor;
    GroundObject *groundObj;
    int locCount;
    Location *locs[5];
    int locSpan[5];
    int locSpans;
    int drawLevel;
    bool visible;
    bool update;
    bool containsLocs;
    int checkLocSpans;
    int blockLocSpans;
    int inverseBlockLocSpans;
    int backWallTypes;
    Ground *bridge;
#endif
};

Ground *ground_new(int level, int x, int z);
void ground_free(Ground *ground);
