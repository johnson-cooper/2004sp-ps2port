#include <stdlib.h>

#include "allocator.h"
#include "defines.h"
#include "ground.h"
#include "tileoverlay.h"

static bool ground_is_ps2_resident_terrain(int x, int z) {
#ifdef __PS2__
    return x >= PS2_TERRAIN_MIN_TILE && x < PS2_TERRAIN_MAX_TILE &&
           z >= PS2_TERRAIN_MIN_TILE && z < PS2_TERRAIN_MAX_TILE;
#else
    (void)x;
    (void)z;
    return false;
#endif
}

Ground *ground_new(int level, int x, int z) {
#ifdef __PS2__
    // Terrain inside the deliberately materialized PS2 window lives for exactly one scene-arena
    // generation. Put those Ground nodes in the arena instead of consuming the tiny libc heap during
    // region entry. Dynamic entity scaffolding outside the resident window remains ordinary heap
    // memory so world3d_clear_temporarylocs() can reclaim it every frame.
    Ground *ground = rs2_calloc(ground_is_ps2_resident_terrain(x, z), 1, sizeof(Ground));
#else
    Ground *ground = calloc(1, sizeof(Ground));
#endif
    if (!ground) {
        return NULL;
    }
    ground->link = (Linkable){0};
    ground->occludeLevel = ground->level = level;
    ground->x = x;
    ground->z = z;
    return ground;
}

void ground_free(Ground *ground) {
    if (!ground) {
        return;
    }

    // link.next/link.prev are intrusive-list links to other objects; they are not allocations owned
    // by this Ground. Freeing them here can double-free/corrupt neighbouring queue nodes during a
    // scene reset. The owning list is responsible for unlinking; Ground owns only its attachments.
#ifdef __PS2__
    // PS2 TileUnderlays in the resident terrain window are scene-arena allocations as well; the arena
    // reset reclaims them after the scene/caches are torn down. Other attachments keep their existing
    // ownership until they are migrated individually.
    if (!ground_is_ps2_resident_terrain(ground->x, ground->z)) {
        free(ground->underlay);
    }
#else
    free(ground->underlay);
#endif
    if (ground->overlay) {
        tileoverlay_free(ground->overlay);
    }
    free(ground->wall);
    free(ground->decor);
    free(ground->groundDecor);
    free(ground->groundObj);
    for (int loc = 0; loc < 5; loc++) {
        ground->locs[loc] = NULL;
    }

    if (ground->bridge) {
        ground_free(ground->bridge);
    }
#ifdef __PS2__
    if (!ground_is_ps2_resident_terrain(ground->x, ground->z)) {
        free(ground);
    }
#else
    free(ground);
#endif
}
