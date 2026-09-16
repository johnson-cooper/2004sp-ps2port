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
    // Real-hardware isolation: keep PLAYER_INFO/NPC_INFO parsing, pathing,
    // scene insertion, collision, and all other gameplay state live, but do not
    // build/draw any dynamic entity mesh. The Lumbridge freeze has ranged from
    // ~T130 to >T2500 while the last packet cadence remains 123/87/123, so tick
    // lifetime is not a useful proxy for one specific packet or animation. If
    // this build still freezes, the remaining failure is upstream of dynamic
    // player/NPC/projectile/spotanim model construction and the renderer can be
    // removed from the active suspect set. world3d already treats a NULL model
    // as a legitimate unavailable entity and skips the draw/free path.
    (void)entity;
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
