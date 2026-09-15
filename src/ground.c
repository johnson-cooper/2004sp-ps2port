#include <stdlib.h>

#include "ground.h"
#include "tileoverlay.h"

Ground *ground_new(int level, int x, int z) {
    Ground *ground = calloc(1, sizeof(Ground));
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
    free(ground->underlay);
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
    free(ground);
}
