#pragma once

#include "datastruct/lrucache.h"
#include "model.h"
#include "packet.h"
#include "pathingentity.h"

typedef struct {
    PathingEntity pathing_entity;
    char name[USERNAME_LENGTH + 1];
    bool visible; // = false;
    int gender;
    int headicons;
    int appearances[12];
    int colors[5];
    int combatLevel;
    int64_t appearanceHashcode;
    int y;
    int locStartCycle;
    int locStopCycle;
    int locOffsetX;
    int locOffsetY;
    int locOffsetZ;
    Model *locModel;
    int minTileX;
    int minTileZ;
    int maxTileX;
    int maxTileZ;
    bool lowmem; // = false;
#ifdef __PS2__
    // Lightweight on-screen diagnosis for the first real-hardware appearance build.  This is
    // deliberately state only: it does not trigger a separate GS presentation.
    int ps2_model_state;
    int ps2_model_parts;
#endif
} PlayerEntity;

typedef struct {
    LruCache *modelCache; // = new LruCache(200);
} PlayerEntityData;

PlayerEntity *playerentity_new(void);
void playerentity_init_global(void);
void playerentity_free_global(void);
// Player appearance meshes are long-lived cache entries.  On PS2 they are heap-backed and
// explicitly released here so they cannot consume the per-scene bump arena indefinitely.
void playerentity_clear_model_cache(void);
void playerentity_read(PlayerEntity *entity, Packet *buf);
Model *playerentity_draw(PlayerEntity *entity, int loopCycle);
Model *playerentity_get_sequencedmodel(PlayerEntity *entity);
Model *playerentity_get_headmodel(PlayerEntity *entity);
bool playerentity_is_visible(PlayerEntity *entity);
