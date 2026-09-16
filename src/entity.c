#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datastruct/hashtable.h"
#include "datastruct/linkable.h"
#include "entity.h"
#include "npcentity.h"
#include "platform.h"
#include "playerentity.h"
#include "projectileentity.h"
#include "spotanimentity.h"

#ifdef __PS2__
extern NpcTypeData _NpcType;
#endif

Entity *entity_new(const char *type) {
    Entity *entity = calloc(1, sizeof(Entity));
    entity->link = (Linkable){0};
    entity->type = type;
    return entity;
}

void entity_draw_free(Entity *entity, Model *m, int loopCycle) {
    if (strcmp(entity->type, "player") == 0) {
        PlayerEntity *player = ((PlayerEntity *)entity);
        if (!player->lowmem) {
            if ((player->pathing_entity.spotanimId != -1 && player->pathing_entity.spotanimFrame != -1) || (player->locModel && loopCycle >= player->locStartCycle && loopCycle < player->locStopCycle)) {
                model_free(m);
            } else {
                model_free_share_alpha(m, true);
            }
        }
    } else if (strcmp(entity->type, "npc") == 0) {
        NpcEntity *npc = ((NpcEntity *)entity);
        if (npc->pathing_entity.spotanimId == -1 || npc->pathing_entity.spotanimFrame == -1) {
            model_free_share_alpha(m, !npc->type->animHasAlpha);
        } else {
            model_free(m);
        }
    } else if (strcmp(entity->type, "spotanim") == 0) {
        SpotAnimEntity *spotanim = ((SpotAnimEntity *)entity);
        model_free_share_colored(m, true, !spotanim->type->animHasAlpha, false);
    } else if (strcmp(entity->type, "projectile") == 0) {
        ProjectileEntity *projectile = ((ProjectileEntity *)entity);
        model_free_calculate_normals(m);
        model_free_share_colored(m, true, !projectile->spotanim->animHasAlpha, false);
    }
}

Model *entity_draw(Entity *entity, int loopCycle) {
#ifdef __PS2__
    // Real-hardware bisection: the NPC cache-only diagnostic still freezes even
    // with no temporary clone, animation, spotanim, or rasterization. Its only
    // per-frame cache operation was lrucache_get(), which also unlinks/relinks the
    // cached Model in the LRU history list on every visible NPC lookup.
    //
    // Preserve the exact same persistent hash table/cache entries, but make cache
    // hits read-only with hashtable_get(). A miss still goes through the normal
    // NPC helper once so cache population remains unchanged.
    //
    // Stable => repeated NPC LRU history mutation is the trigger.
    // Freeze => even read-only access to the persistent NPC cache is sufficient,
    // strongly implicating cache-entry lifetime/arena ownership rather than LRU
    // recency bookkeeping.
    if (strcmp(entity->type, "npc") == 0) {
        NpcEntity *npc = (NpcEntity *)entity;
        if (npc->type && npc->type->models_count > 0) {
            Model *cached = (Model *)hashtable_get(_NpcType.modelCache->hashtable, npc->type->index);
            if (!cached) {
                Model *tmp = npctype_get_sequencedmodel(npc->type, -1, -1, NULL);
                if (tmp) {
                    model_free_share_alpha(tmp, !npc->type->animHasAlpha);
                }
            }
        }
    }
    (void)loopCycle;
    return NULL;
#else
    Model *model = NULL;
    if (strcmp(entity->type, "player") == 0) {
        model = playerentity_draw((PlayerEntity *)entity, loopCycle);
    } else if (strcmp(entity->type, "npc") == 0) {
        model = npcentity_draw((NpcEntity *)entity, loopCycle);
    } else if (strcmp(entity->type, "spotanim") == 0) {
        model = spotanimentity_draw((SpotAnimEntity *)entity, loopCycle);
    } else if (strcmp(entity->type, "projectile") == 0) {
        model = projectileentity_draw((ProjectileEntity *)entity, loopCycle);
    }
    return model;
#endif
}
