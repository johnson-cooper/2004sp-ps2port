#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datastruct/linkable.h"
#include "entity.h"
#include "npcentity.h"
#include "platform.h"
#include "playerentity.h"
#include "projectileentity.h"
#include "spotanimentity.h"

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
    // Real-hardware bisection: NPC-only rendering reproduced the Lumbridge
    // freeze, while all-dynamic-disabled and player-only each survived 10+
    // minutes in the same position. Exercise the complete NPC model build,
    // animation, spotanim composition, and matching free path here, but return
    // NULL so world3d never rasterizes the resulting mesh.
    //
    // Freeze => fault is in NPC model build/animation/free/cache ownership.
    // Stable => those paths are safe and the failure is in drawing an NPC model
    // through world3d/model rasterization (or state consumed only by that draw).
    if (strcmp(entity->type, "npc") == 0) {
        Model *model = npcentity_draw((NpcEntity *)entity, loopCycle);
        if (model) {
            entity_draw_free(entity, model, loopCycle);
        }
    }
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
