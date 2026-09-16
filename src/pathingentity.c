#include "pathingentity.h"
#include "npcentity.h"
#include "playerentity.h"
#include "seqtype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "entity.h"

extern SeqTypeData _SeqType;

static bool pathingentity_seq_valid(int seq_id) {
    return seq_id >= 0 && seq_id < _SeqType.count && _SeqType.instances && _SeqType.instances[seq_id];
}

static void pathingentity_cancel_interruptible_primary_seq(PathingEntity *entity) {
    if (entity->primarySeqId == -1) {
        return;
    }

    // PLAYER_INFO/NPC_INFO movement reaches this path directly from the live
    // packet stream. Client3 historically trusted primarySeqId here and indexed
    // _SeqType.instances[] before checking that the rev254 id exists locally.
    // A stale/unsupported id must be discarded rather than turning an ordinary
    // movement update into an EE out-of-bounds read.
    if (!pathingentity_seq_valid(entity->primarySeqId)) {
        entity->primarySeqId = -1;
        return;
    }

    if (_SeqType.instances[entity->primarySeqId]->priority <= 1) {
        entity->primarySeqId = -1;
    }
}

PathingEntity pathingentity_new(const char *type) {
    PathingEntity entity = {0};
    entity.entity = (Entity){(Linkable){0}, type},
    entity.seqStretches = false;
    entity.size = 1,
    entity.seqStandId = -1,
    entity.seqTurnId = -1,
    entity.seqWalkId = -1,
    entity.seqTurnAroundId = -1,
    entity.seqTurnLeftId = -1,
    entity.seqTurnRightId = -1,
    entity.seqRunId = -1,
    entity.turnRate = 32,
    entity.chatTimer = 100,
    entity.combatCycle = -1000,
    entity.combatCycle2 = -1000,
    entity.targetId = -1,
    entity.secondarySeqId = -1,
    entity.primarySeqId = -1,
    entity.spotanimId = -1,
    entity.lastMask = -1,
    entity.lastMaskCycle = -1,
    entity.lastFaceX = -1,
    entity.lastFaceZ = -1;
    return entity;
}

void pathingentity_teleport(PathingEntity *entity, bool jump, int x, int z) {
    pathingentity_cancel_interruptible_primary_seq(entity);

    if (!jump) {
        int dx = x - entity->pathTileX[0];
        int dz = z - entity->pathTileZ[0];

        if (dx >= -8 && dx <= 8 && dz >= -8 && dz <= 8) {
            if (entity->pathLength < 9) {
                entity->pathLength++;
            }

            for (int i = entity->pathLength; i > 0; i--) {
                entity->pathTileX[i] = entity->pathTileX[i - 1];
                entity->pathTileZ[i] = entity->pathTileZ[i - 1];
                entity->pathRunning[i] = entity->pathRunning[i - 1];
            }

            entity->pathTileX[0] = x;
            entity->pathTileZ[0] = z;
            entity->pathRunning[0] = false;
            return;
        }
    }

    entity->pathLength = 0;
    entity->seqTrigger = 0;
    entity->pathTileX[0] = x;
    entity->pathTileZ[0] = z;
    entity->x = entity->pathTileX[0] * 128 + entity->size * 64;
    entity->z = entity->pathTileZ[0] * 128 + entity->size * 64;
}

void pathingentity_movealongroute(PathingEntity *entity, bool running, int direction) {
    int nextX = entity->pathTileX[0];
    int nextZ = entity->pathTileZ[0];

    if (direction == 0) {
        nextX--;
        nextZ++;
    } else if (direction == 1) {
        nextZ++;
    } else if (direction == 2) {
        nextX++;
        nextZ++;
    } else if (direction == 3) {
        nextX--;
    } else if (direction == 4) {
        nextX++;
    } else if (direction == 5) {
        nextX--;
        nextZ--;
    } else if (direction == 6) {
        nextZ--;
    } else if (direction == 7) {
        nextX++;
        nextZ--;
    }

    pathingentity_cancel_interruptible_primary_seq(entity);

    if (entity->pathLength < 9) {
        entity->pathLength++;
    }

    for (int i = entity->pathLength; i > 0; i--) {
        entity->pathTileX[i] = entity->pathTileX[i - 1];
        entity->pathTileZ[i] = entity->pathTileZ[i - 1];
        entity->pathRunning[i] = entity->pathRunning[i - 1];
    }

    entity->pathTileX[0] = nextX;
    entity->pathTileZ[0] = nextZ;
    entity->pathRunning[0] = running;
}

bool pathingentity_is_visible(PathingEntity *entity) {
    if (strcmp(entity->entity.type, "player") == 0) {
        return playerentity_is_visible((PlayerEntity *)entity);
    }
    if (strcmp(entity->entity.type, "npc") == 0) {
        return npcentity_is_visible((NpcEntity *)entity);
    }
    return false;
}
