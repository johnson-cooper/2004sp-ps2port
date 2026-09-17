#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "allocator.h"
#include "collisionmap.h"
#include "datastruct/linklist.h"
#include "defines.h"
#include "flotype.h"
#include "locentity.h"
#include "loctype.h"
#include "model.h"
#include "packet.h"
#include "pix3d.h"
#include "platform.h"
#include "seqtype.h"
#include "world.h"
#include "world3d.h"
#ifdef __PS2__
#include "client.h"
#endif

extern Pix3D _Pix3D;
extern FloTypeData _FloType;
extern SeqTypeData _SeqType;

WorldData _World = {.lowMemory = true};

const int ROTATION_WALL_TYPE[] = {1, 2, 4, 8};
const int ROTATION_WALL_CORNER_TYPE[] = {16, 32, 64, 128};
const int WALL_DECORATION_ROTATION_FORWARD_X[] = {1, 0, -1, 0};
const int WALL_DECORATION_ROTATION_FORWARD_Z[] = {0, -1, 0, 1};

void world_init_global(void) {
    _World.randomHueOffset = (int)(jrand() * 17.0) - 8;
    _World.randomLightnessOffset = (int)(jrand() * 33.0) - 16;
}

World *world_new(int maxTileX, int maxTileZ, int (*levelHeightmap)[104 + 1][104 + 1], int8_t (*levelTileFlags)[104][104]) {
    World *world = calloc(1, sizeof(World));
    world->maxTileX = maxTileX;
    world->maxTileZ = maxTileZ;
    world->levelHeightmap = levelHeightmap;
    world->levelTileFlags = levelTileFlags;
    world->levelTileUnderlayIds = calloc(4, sizeof(*world->levelTileUnderlayIds));
    world->levelTileOverlayIds = calloc(4, sizeof(*world->levelTileOverlayIds));
    world->levelTileOverlayShape = calloc(4, sizeof(*world->levelTileOverlayShape));
    world->levelTileOverlayRotation = calloc(4, sizeof(*world->levelTileOverlayRotation));
    world->levelOccludemap = calloc(4, sizeof(*world->levelOccludemap));
    world->levelShademap = calloc(4, sizeof(*world->levelShademap));
    world->levelLightmap = calloc(world->maxTileX + 1, sizeof(*world->levelLightmap));
    world->blendChroma = calloc(world->maxTileZ, sizeof(int));
    world->blendSaturation = calloc(world->maxTileZ, sizeof(int));
    world->blendLightness = calloc(world->maxTileZ, sizeof(int));
    world->blendLuminance = calloc(world->maxTileZ, sizeof(int));
    world->blendMagnitude = calloc(world->maxTileZ, sizeof(int));
    return world;
}

void world_free(World *world) {
    free(world->levelTileUnderlayIds);
    free(world->levelTileOverlayIds);
    free(world->levelTileOverlayShape);
    free(world->levelTileOverlayRotation);
    free(world->levelOccludemap);
    free(world->levelShademap);
    free(world->levelLightmap);
    free(world->blendChroma);
    free(world->blendSaturation);
    free(world->blendLightness);
    free(world->blendLuminance);
    free(world->blendMagnitude);
    free(world);
}

int perlinNoise(int x, int z) {
    int value = interpolatedNoise(x + 45365, z + 91923, 4) + ((interpolatedNoise(x + 10294, z + 37821, 2) - 128) >> 1) + ((interpolatedNoise(x, z, 1) - 128) >> 2) - 128;
    value = (int)((double)value * 0.3) + 35;
    if (value < 10) value = 10;
    else if (value > 60) value = 60;
    return value;
}

int interpolatedNoise(int x, int z, int scale) {
    int intX = x / scale;
    int fracX = x & scale - 1;
    int intZ = z / scale;
    int fracZ = z & scale - 1;
    int v1 = smoothNoise(intX, intZ);
    int v2 = smoothNoise(intX + 1, intZ);
    int v3 = smoothNoise(intX, intZ + 1);
    int v4 = smoothNoise(intX + 1, intZ + 1);
    int i1 = interpolate(v1, v2, fracX, scale);
    int i2 = interpolate(v3, v4, fracX, scale);
    return interpolate(i1, i2, fracZ, scale);
}

int interpolate(int a, int b, int x, int scale) {
    int f = (65536 - _Pix3D.cos_table[x * 1024 / scale]) >> 1;
    return (a * (65536 - f) >> 16) + (b * f >> 16);
}

int smoothNoise(int x, int y) {
    int corners = noise(x - 1, y - 1) + noise(x + 1, y - 1) + noise(x - 1, y + 1) + noise(x + 1, y + 1);
    int sides = noise(x - 1, y) + noise(x + 1, y) + noise(x, y - 1) + noise(x, y + 1);
    int center = noise(x, y);
    return corners / 16 + sides / 8 + center / 4;
}

int noise(int x, int y) {
    int n = x + y * 57;
    int n1 = n << 13 ^ n;
    int n2 = n1 * (n1 * n1 * 15731 + 789221) + 1376312589 & INT_MAX;
    return n2 >> 19 & 0xff;
}

void world_add_loc(int level, int x, int z, World3D *scene, int (*levelHeightmap)[104 + 1][104 + 1], LinkList *locs, CollisionMap *collision, int locId, int shape, int rotation, int trueLevel) {
    int heightSW = levelHeightmap[trueLevel][x][z];
    int heightSE = levelHeightmap[trueLevel][x + 1][z];
    int heightNE = levelHeightmap[trueLevel][x + 1][z + 1];
    int heightNW = levelHeightmap[trueLevel][x][z + 1];
    int y = (heightSW + heightSE + heightNW + heightNE) >> 2;
    LocType *loc = loctype_get(locId);
    int bitset = x + (z << 7) + (locId << 14) + 0x40000000;
    if (!loc->active) bitset += INT_MIN;
    int8_t info = (int8_t)((rotation << 6) + shape);
    Model *model1;
    int width;
    int offset;
    Model *model2;

    if (shape == GROUNDDECOR) {
        model1 = loctype_get_model(loc, GROUNDDECOR, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_grounddecoration(scene, model1, level, x, z, y, bitset, info);
        if (loc->blockwalk && loc->active) collisionmap_set_blocked(collision, x, z);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 3, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == CENTREPIECE_STRAIGHT || shape == CENTREPIECE_DIAGONAL) {
        model1 = loctype_get_model(loc, CENTREPIECE_STRAIGHT, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        if (model1) {
            int yaw = shape == CENTREPIECE_DIAGONAL ? 256 : 0;
            int height;
            if (rotation == 1 || rotation == 3) { width = loc->length; height = loc->width; }
            else { width = loc->width; height = loc->length; }
            world3d_add_loc(scene, level, x, z, y, model1, NULL, bitset, info, width, height, yaw);
        }
        if (loc->blockwalk) collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape >= ROOF_STRAIGHT) {
        model1 = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_loc(scene, level, x, z, y, model1, NULL, bitset, info, 1, 1, 0);
        if (loc->blockwalk) collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALL_STRAIGHT) {
        model1 = loctype_get_model(loc, WALL_STRAIGHT, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_TYPE[rotation], 0, model1, NULL, bitset, info);
        if (loc->blockwalk) collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALL_DIAGONALCORNER) {
        model1 = loctype_get_model(loc, WALL_DIAGONALCORNER, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_CORNER_TYPE[rotation], 0, model1, NULL, bitset, info);
        if (loc->blockwalk) collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALL_L) {
        int nextRotation = rotation + 1 & 0x3;
        Model *model3 = loctype_get_model(loc, WALL_L, rotation + 4, heightSW, heightSE, heightNE, heightNW, -1);
        model2 = loctype_get_model(loc, WALL_L, nextRotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_TYPE[rotation], ROTATION_WALL_TYPE[nextRotation], model3, model2, bitset, info);
        if (loc->blockwalk) collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALL_SQUARECORNER) {
        model1 = loctype_get_model(loc, WALL_SQUARECORNER, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_CORNER_TYPE[rotation], 0, model1, NULL, bitset, info);
        if (loc->blockwalk) collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALL_DIAGONAL) {
        model1 = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_loc(scene, level, x, z, y, model1, NULL, bitset, info, 1, 1, 0);
        if (loc->blockwalk) collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALLDECOR_STRAIGHT_NOOFFSET) {
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model1, info, rotation * 512, ROTATION_WALL_TYPE[rotation]);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALLDECOR_STRAIGHT_OFFSET) {
        offset = 16;
        width = world3d_get_wallbitset(scene, level, x, z);
        if (width > 0) offset = loctype_get(width >> 14 & 0x7fff)->wallwidth;
        model2 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, WALL_DECORATION_ROTATION_FORWARD_X[rotation] * offset, WALL_DECORATION_ROTATION_FORWARD_Z[rotation] * offset, bitset, model2, info, rotation * 512, ROTATION_WALL_TYPE[rotation]);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALLDECOR_DIAGONAL_OFFSET) {
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model1, info, rotation, 256);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALLDECOR_DIAGONAL_NOOFFSET) {
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model1, info, rotation, 512);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALLDECOR_DIAGONAL_BOTH) {
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model1, info, rotation, 768);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
    }
}

void clearLandscape(World *world, int startX, int startZ, int endX, int endZ) {
    int8_t waterOverlay = 0;
    for (int i = 0; i < _FloType.count; i++) {
        if (platform_strcasecmp(_FloType.instances[i]->name, "water") == 0) { waterOverlay = (int8_t)(i + 1); break; }
    }
    for (int z = startX; z < startX + endX; z++) {
        for (int x = startZ; x < startZ + endZ; x++) {
            if (x < 0 || x >= world->maxTileX || z < 0 || z >= world->maxTileZ) continue;
            world->levelTileOverlayIds[0][x][z] = waterOverlay;
            for (int level = 0; level < 4; level++) { world->levelHeightmap[level][x][z] = 0; world->levelTileFlags[level][x][z] = 0; }
        }
    }
}

void world_load_ground(World *world, int originX, int originZ, int xOffset, int zOffset, int8_t *src, int src_len) {
    Packet *buf = packet_new(src, src_len);
    for (int level = 0; level < 4; level++) {
        for (int x = 0; x < 64; x++) {
            for (int z = 0; z < 64; z++) {
                int stx = x + xOffset;
                int stz = z + zOffset;
                int opcode;
                if (stx >= 0 && stx < 104 && stz >= 0 && stz < 104) {
                    world->levelTileFlags[level][stx][stz] = 0;
                    while (true) {
                        opcode = g1(buf);
                        if (opcode == 0) {
                            if (level == 0) world->levelHeightmap[0][stx][stz] = -perlinNoise(stx + originX + 932731, stz + originZ + 556238) * 8;
                            else world->levelHeightmap[level][stx][stz] = world->levelHeightmap[level - 1][stx][stz] - 240;
                            break;
                        }
                        if (opcode == 1) {
                            int height = g1(buf); if (height == 1) height = 0;
                            if (level == 0) world->levelHeightmap[0][stx][stz] = -height * 8;
                            else world->levelHeightmap[level][stx][stz] = world->levelHeightmap[level - 1][stx][stz] - height * 8;
                            break;
                        }
                        if (opcode <= 49) { world->levelTileOverlayIds[level][stx][stz] = g1b(buf); world->levelTileOverlayShape[level][stx][stz] = (int8_t)((opcode - 2) >> 2); world->levelTileOverlayRotation[level][stx][stz] = (int8_t)(opcode - 2 & 0x3); }
                        else if (opcode <= 81) world->levelTileFlags[level][stx][stz] = (int8_t)(opcode - 49);
                        else world->levelTileUnderlayIds[level][stx][stz] = (int8_t)(opcode - 81);
                    }
                } else {
                    while (true) { opcode = g1(buf); if (opcode == 0) break; if (opcode == 1) { g1(buf); break; } if (opcode <= 49) g1(buf); }
                }
            }
        }
    }
    free(buf);
}

void world_load_locations(World *world, World3D *scene, LinkList *locs, CollisionMap **collision, int8_t *src, int src_len, int xOffset, int zOffset) {
#if defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY)
    Packet packet_storage = {0};
    packet_storage.data = src;
    packet_storage.length = src_len;
    Packet *buf = &packet_storage;
    int ps2_outer_guard = 0;
    int ps2_prev_pos = -1;
#else
    Packet *buf = packet_new(src, src_len);
#endif
    int locId = -1;
#ifdef __PS2__
    int ps2_loc_count = 0;
    int64_t ps2_t0 = rs2_now();
#ifdef PS2_LOC_DECODE_ONLY
    (void)ps2_t0;
#endif
    // Match loc placement to the same 80x80 local traversal bridge window as terrain. The previous
    // fixed 32x32 gate (32..63) left the outer traversable scene without walls/objects and made locs
    // disappear after region recentres even though terrain continued correctly. This is an isolated
    // correctness test; if model/arena pressure is too high on hardware, replace it with a moving
    // 32x32/48x48 loc window rather than keeping the larger resident model set permanently.
    const int ps2LocMinTile = PS2_TERRAIN_MIN_TILE;
    const int ps2LocMaxTile = PS2_TERRAIN_MAX_TILE;
#endif

    while (true) {
#if defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY)
        if (++ps2_outer_guard > 10000 || buf->pos == ps2_prev_pos) return;
        ps2_prev_pos = buf->pos;
#endif
#ifdef __PS2__
        if (buf->pos >= buf->length) { free(buf); return; }
#endif
        int deltaId = gsmarts(buf);
        if (deltaId == 0) {
#if !(defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY))
            free(buf);
#endif
            return;
        }
        locId += deltaId;
        int locPos = 0;
#if defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY)
        int ps2_inner_guard = 0;
#endif
        while (true) {
#if defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY)
            if (++ps2_inner_guard > 100000) return;
#endif
#ifdef __PS2__
            if (buf->pos >= buf->length) { free(buf); return; }
#endif
            int deltaPos = gsmarts(buf);
            if (deltaPos == 0) break;
            locPos += deltaPos - 1;
            int z = locPos & 0x3f;
            int x = locPos >> 6 & 0x3f;
            int level = locPos >> 12;
            int info = g1(buf);
            int shape = info >> 2;
            int rotation = info & 0x3;
            int stx = x + xOffset;
            int stz = z + zOffset;
#ifdef __PS2__
            if (level < 0 || level >= 4) continue;
#endif
#if defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY)
            (void)stx; (void)stz; (void)shape; (void)rotation;
#else
            if (stx > 0 && stz > 0 && stx < 103 && stz < 103
#ifdef __PS2__
                && stx >= ps2LocMinTile && stx < ps2LocMaxTile && stz >= ps2LocMinTile && stz < ps2LocMaxTile
#endif
            ) {
                int currentLevel = level;
                if ((world->levelTileFlags[1][stx][stz] & 0x2) == 2) currentLevel = level - 1;
                CollisionMap *collisionMap = currentLevel >= 0 ? collision[currentLevel] : NULL;
#ifdef __PS2__
                ps2_loc_count++;
                if (ps2_loc_count <= 5 || ps2_loc_count % 50 == 0) {
                    rs2_log("world_load_locations: loc #%d id=%d shape=%d rot=%d x=%d z=%d level=%d bump=%d/%d elapsed=%dms\n", ps2_loc_count, locId, shape, rotation, stx, stz, level, bump_allocator_used(), bump_allocator_capacity(), (int)(rs2_now() - ps2_t0));
                }
#endif
                world_add_loc2(world, level, stx, stz, scene, locs, collisionMap, locId, shape, rotation);
            }
#endif
        }
    }
}

void world_add_loc2(World *world, int level, int x, int z, World3D *scene, LinkList *locs, CollisionMap *collision, int locId, int shape, int rotation) {
    if (_World.lowMemory) {
        if ((world->levelTileFlags[level][x][z] & 0x10) != 0) return;
        if (world_get_drawlevel(world, level, x, z) != _World.levelBuilt) return;
    }
    int heightSW = world->levelHeightmap[level][x][z];
    int heightSE = world->levelHeightmap[level][x + 1][z];
    int heightNE = world->levelHeightmap[level][x + 1][z + 1];
    int heightNW = world->levelHeightmap[level][x][z + 1];
    int y = (heightSW + heightSE + heightNW + heightNE) >> 2;
    LocType *loc = loctype_get(locId);
    int bitset = x + (z << 7) + (locId << 14) + 0x40000000;
    if (!loc->active) bitset += INT_MIN;
    int8_t info = (int8_t)((rotation << 6) + shape);
    Model *model;
    int width;
    int offset;
    Model *model1;

    if (shape == GROUNDDECOR) {
        if (_World.lowMemory && !loc->active && !loc->forcedecor) return;
        model = loctype_get_model(loc, GROUNDDECOR, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_grounddecoration(scene, model, level, x, z, y, bitset, info);
        if (loc->blockwalk && loc->active && collision) collisionmap_set_blocked(collision, x, z);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 3, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == CENTREPIECE_STRAIGHT || shape == CENTREPIECE_DIAGONAL) {
        model = loctype_get_model(loc, CENTREPIECE_STRAIGHT, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        if (model) {
            int yaw = shape == CENTREPIECE_DIAGONAL ? 256 : 0;
            int height;
            if (rotation == 1 || rotation == 3) { width = loc->length; height = loc->width; }
            else { width = loc->width; height = loc->length; }
            if (world3d_add_loc(scene, level, x, z, y, model, NULL, bitset, info, width, height, yaw) && loc->shadow) {
                for (int dx = 0; dx <= width; dx++) for (int dz = 0; dz <= height; dz++) {
                    int shade = model->radius / 4; if (shade > 30) shade = 30;
                    if (shade > world->levelShademap[level][x + dx][z + dz]) world->levelShademap[level][x + dx][z + dz] = (int8_t)shade;
                }
            }
        }
        if (loc->blockwalk && collision) collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape >= ROOF_STRAIGHT) {
        model = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_loc(scene, level, x, z, y, model, NULL, bitset, info, 1, 1, 0);
        if (shape <= ROOF_FLAT && shape != ROOF_DIAGONAL_WITH_ROOFEDGE && level > 0) world->levelOccludemap[level][x][z] |= 0x924;
        if (loc->blockwalk && collision) collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALL_STRAIGHT) {
        model = loctype_get_model(loc, WALL_STRAIGHT, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_TYPE[rotation], 0, model, NULL, bitset, info);
        if (loc->blockwalk && collision) collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        if (loc->wallwidth != 16) world3d_set_walldecorationoffset(scene, level, x, z, loc->wallwidth);
    } else if (shape == WALL_DIAGONALCORNER || shape == WALL_SQUARECORNER) {
        model = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_CORNER_TYPE[rotation], 0, model, NULL, bitset, info);
        if (loc->blockwalk && collision) collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALL_L) {
        int nextRotation = rotation + 1 & 3;
        Model *model3 = loctype_get_model(loc, WALL_L, rotation + 4, heightSW, heightSE, heightNE, heightNW, -1);
        model1 = loctype_get_model(loc, WALL_L, nextRotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_TYPE[rotation], ROTATION_WALL_TYPE[nextRotation], model3, model1, bitset, info);
        if (loc->blockwalk && collision) collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        if (loc->wallwidth != 16) world3d_set_walldecorationoffset(scene, level, x, z, loc->wallwidth);
    } else if (shape == WALL_DIAGONAL) {
        model = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_loc(scene, level, x, z, y, model, NULL, bitset, info, 1, 1, 0);
        if (loc->blockwalk && collision) collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        if (loc->anim != -1 && loc->anim < _SeqType.count) linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
    } else if (shape == WALLDECOR_STRAIGHT_NOOFFSET) {
        model = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model, info, rotation * 512, ROTATION_WALL_TYPE[rotation]);
    } else if (shape == WALLDECOR_STRAIGHT_OFFSET) {
        offset = 16; width = world3d_get_wallbitset(scene, level, x, z); if (width > 0) offset = loctype_get(width >> 14 & 0x7fff)->wallwidth;
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, WALL_DECORATION_ROTATION_FORWARD_X[rotation] * offset, WALL_DECORATION_ROTATION_FORWARD_Z[rotation] * offset, bitset, model1, info, rotation * 512, ROTATION_WALL_TYPE[rotation]);
    } else if (shape == WALLDECOR_DIAGONAL_OFFSET || shape == WALLDECOR_DIAGONAL_NOOFFSET || shape == WALLDECOR_DIAGONAL_BOTH) {
        model = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        int angle = shape == WALLDECOR_DIAGONAL_OFFSET ? 256 : (shape == WALLDECOR_DIAGONAL_NOOFFSET ? 512 : 768);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model, info, rotation, angle);
    }
}

static FloType *world_flotype_get(int id, int level, int x, int z) {
    if (id > _FloType.count) { rs2_error("world_build: floType id %d out of range (count=%d) at level=%d x=%d z=%d\n", id, _FloType.count, level, x, z); return NULL; }
    return _FloType.instances[id - 1];
}

#ifdef __PS2__
void world_build(World *world, World3D *scene, CollisionMap **collision, Client *c) {
#else
void world_build(World *world, World3D *scene, CollisionMap **collision) {
#endif
    for (int level = 0; level < 4; level++) for (int x = 0; x < 104; x++) for (int z = 0; z < 104; z++) {
        if ((world->levelTileFlags[level][x][z] & 1) == 1) {
            int trueLevel = level; if ((world->levelTileFlags[1][x][z] & 2) == 2) trueLevel--;
            if (trueLevel >= 0) collisionmap_set_blocked(collision[trueLevel], x, z);
        }
    }
    _World.randomHueOffset += (int)(jrand() * 5.0) - 2; if (_World.randomHueOffset < -8) _World.randomHueOffset = -8; else if (_World.randomHueOffset > 8) _World.randomHueOffset = 8;
    _World.randomLightnessOffset += (int)(jrand() * 5.0) - 2; if (_World.randomLightnessOffset < -16) _World.randomLightnessOffset = -16; else if (_World.randomLightnessOffset > 16) _World.randomLightnessOffset = 16;

    for (int level = 0; level < 4; level++) {
        int8_t (*shademap)[105] = world->levelShademap[level];
        int lightAmbient = 96, lightAttenuation = 768; int8_t lightX = -50, lightY = -10, lightZ = -50;
#ifdef USE_FLOATS
        int lightMag = (int)sqrtf((float)(lightX * lightX + lightY * lightY + lightZ * lightZ));
#else
        int lightMag = (int)sqrt(lightX * lightX + lightY * lightY + lightZ * lightZ);
#endif
        int lightMagnitude = lightAttenuation * lightMag >> 8;
        for (int z = 1; z < world->maxTileZ - 1; z++) for (int x = 1; x < world->maxTileX - 1; x++) {
            int dx = world->levelHeightmap[level][x + 1][z] - world->levelHeightmap[level][x - 1][z];
            int dz = world->levelHeightmap[level][x][z + 1] - world->levelHeightmap[level][x][z - 1];
#ifdef USE_FLOATS
            int len = (int)sqrtf((float)(dx * dx + dz * dz + 65536));
#else
            int len = (int)sqrt(dx * dx + dz * dz + 65536);
#endif
            int normalX = (dx << 8) / len, normalY = 65536 / len, normalZ = (dz << 8) / len;
            int light = lightAmbient + (lightX * normalX + lightY * normalY + lightZ * normalZ) / lightMagnitude;
            int shade = (shademap[x - 1][z] >> 2) + (shademap[x + 1][z] >> 3) + (shademap[x][z - 1] >> 2) + (shademap[x][z + 1] >> 3) + (shademap[x][z] >> 1);
            world->levelLightmap[x][z] = light - shade;
        }
        for (int z = 0; z < world->maxTileZ; z++) { world->blendChroma[z] = world->blendSaturation[z] = world->blendLightness[z] = world->blendLuminance[z] = world->blendMagnitude[z] = 0; }
        for (int x0 = -5; x0 < world->maxTileX + 5; x0++) {
            for (int z0 = 0; z0 < world->maxTileZ; z0++) {
                int x1 = x0 + 5;
                if (x1 >= 0 && x1 < world->maxTileX) { int id = world->levelTileUnderlayIds[level][x1][z0] & 0xff; if (id > 0) { FloType *f = world_flotype_get(id, level, x1, z0); if (f) { world->blendChroma[z0] += f->chroma; world->blendSaturation[z0] += f->saturation; world->blendLightness[z0] += f->lightness; world->blendLuminance[z0] += f->luminance; world->blendMagnitude[z0]++; } } }
                int x2 = x0 - 5;
                if (x2 >= 0 && x2 < world->maxTileX) { int id = world->levelTileUnderlayIds[level][x2][z0] & 0xff; if (id > 0) { FloType *f = world_flotype_get(id, level, x2, z0); if (f) { world->blendChroma[z0] -= f->chroma; world->blendSaturation[z0] -= f->saturation; world->blendLightness[z0] -= f->lightness; world->blendLuminance[z0] -= f->luminance; world->blendMagnitude[z0]--; } } }
            }
            if (x0 >= 1 && x0 < world->maxTileX - 1) {
                int hueA=0,satA=0,lightA=0,lumA=0,magA=0;
                for (int z0=-5; z0<world->maxTileZ+5; z0++) {
                    int a=z0+5; if(a>=0&&a<world->maxTileZ){hueA+=world->blendChroma[a];satA+=world->blendSaturation[a];lightA+=world->blendLightness[a];lumA+=world->blendLuminance[a];magA+=world->blendMagnitude[a];}
                    int b=z0-5; if(b>=0&&b<world->maxTileZ){hueA-=world->blendChroma[b];satA-=world->blendSaturation[b];lightA-=world->blendLightness[b];lumA-=world->blendLuminance[b];magA-=world->blendMagnitude[b];}
                    if (z0>=1&&z0<world->maxTileZ-1
#ifdef __PS2__
                        && x0>=PS2_TERRAIN_MIN_TILE&&x0<PS2_TERRAIN_MAX_TILE&&z0>=PS2_TERRAIN_MIN_TILE&&z0<PS2_TERRAIN_MAX_TILE
#endif
                        && (!_World.lowMemory || ((world->levelTileFlags[level][x0][z0]&0x10)==0 && world_get_drawlevel(world,level,x0,z0)==_World.levelBuilt))) {
                        int underlayId=world->levelTileUnderlayIds[level][x0][z0]&0xff, overlayId=world->levelTileOverlayIds[level][x0][z0]&0xff;
                        if(underlayId>_FloType.count)underlayId=0; if(overlayId>_FloType.count)overlayId=0;
                        if(underlayId>0||overlayId>0){
                            int hSW=world->levelHeightmap[level][x0][z0],hSE=world->levelHeightmap[level][x0+1][z0],hNE=world->levelHeightmap[level][x0+1][z0+1],hNW=world->levelHeightmap[level][x0][z0+1];
                            int lSW=world->levelLightmap[x0][z0],lSE=world->levelLightmap[x0+1][z0],lNE=world->levelLightmap[x0+1][z0+1],lNW=world->levelLightmap[x0][z0+1];
                            int base=-1,tint=-1;
                            if(underlayId>0&&lumA!=0&&magA!=0){int hue=hueA*256/lumA,sat=satA/magA,li=lightA/magA;base=hsl24to16(hue,sat,li);int rh=(hue+_World.randomHueOffset)&0xff;li+=_World.randomLightnessOffset;if(li<0)li=0;else if(li>255)li=255;tint=hsl24to16(rh,sat,li);}
                            int shadeColor=base!=-1?_Pix3D.palette[mulHSL(tint,96)]:0;
                            if(overlayId==0) world3d_set_tile(scene,level,x0,z0,0,0,-1,hSW,hSE,hNE,hNW,mulHSL(base,lSW),mulHSL(base,lSE),mulHSL(base,lNE),mulHSL(base,lNW),0,0,0,0,shadeColor,0);
                            else {int shape=world->levelTileOverlayShape[level][x0][z0]+1;int8_t rot=world->levelTileOverlayRotation[level][x0][z0];FloType *flo=_FloType.instances[overlayId-1];int textureId=flo->texture,hsl,rgb;if(textureId>=0){
#if defined(__PS2__) && PS2_UNTEXTURED_TERRAIN
                                textureId=-1;hsl=base!=-1?base:hsl24to16(flo->hue,flo->saturation,flo->lightness);rgb=_Pix3D.palette[adjustLightness(hsl,96)];
#else
                                rgb=pix3d_get_average_texture_rgb(textureId);hsl=-1;
#endif
                                }else if(flo->rgb==MAGENTA){rgb=0;hsl=-2;textureId=-1;}else{hsl=hsl24to16(flo->hue,flo->saturation,flo->lightness);rgb=_Pix3D.palette[adjustLightness(flo->hsl,96)];}
                                world3d_set_tile(scene,level,x0,z0,shape,rot,textureId,hSW,hSE,hNE,hNW,mulHSL(base,lSW),mulHSL(base,lSE),mulHSL(base,lNE),mulHSL(base,lNW),adjustLightness(hsl,lSW),adjustLightness(hsl,lSE),adjustLightness(hsl,lNE),adjustLightness(hsl,lNW),shadeColor,rgb);
                            }
                        }
                    }
                }
            }
        }
        for(int stz=1;stz<world->maxTileZ-1;stz++)for(int stx=1;stx<world->maxTileX-1;stx++)world3d_set_drawlevel(scene,level,stx,stz,world_get_drawlevel(world,level,stx,stz));
    }
    if(!_World.fullbright) world3d_build_models(scene,64,768,-50,-10,-50);
    for(int x=0;x<world->maxTileX;x++)for(int z=0;z<world->maxTileZ;z++)if((world->levelTileFlags[1][x][z]&2)==2)world3d_set_bridge(scene,x,z);
    (void)c;
}

int world_get_drawlevel(World *world, int level, int stx, int stz) {
    if ((world->levelTileFlags[level][stx][stz] & 0x8) == 0) return level <= 0 || (world->levelTileFlags[1][stx][stz] & 0x2) == 0 ? level : level - 1;
    return 0;
}
