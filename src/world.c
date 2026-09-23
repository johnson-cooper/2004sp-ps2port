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
// Only needed for ps2_scene_checkpoint() - client.h doesn't include world.h (checked), so this can't
// create a cycle.
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
    if (value < 10) {
        value = 10;
    } else if (value > 60) {
        value = 60;
    }
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
    if (!loc->active) {
        bitset += INT_MIN;
    }

    int8_t info = (int8_t)((rotation << 6) + shape);
    Model *model1;
    int width;
    int offset;
    Model *model2;

    if (shape == GROUNDDECOR) {
        model1 = loctype_get_model(loc, GROUNDDECOR, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_grounddecoration(scene, model1, level, x, z, y, bitset, info);

        if (loc->blockwalk && loc->active) {
            collisionmap_set_blocked(collision, x, z);
        }

        // loc->anim is decoded straight off the network/cache (loctype.c) as a raw 16-bit id with no
        // range check against _SeqType.count - the same class of bug already fixed for seqId at every
        // ANIM-mask call site in client.c, just missed here since this file indexes it directly
        // instead of going through a helper. A real rev254 config can reference a seq id beyond what
        // this build's seq.dat actually loaded (confirmed happening for other id spaces already this
        // session - npctype/objtype/loctype's own offset lookups needed the same fix). All 26 uses of
        // _SeqType.instances[loc->anim] in this file (both world_add_loc() and world_add_loc2()) now
        // carry this bound.
        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 3, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == CENTREPIECE_STRAIGHT || shape == CENTREPIECE_DIAGONAL) {
        model1 = loctype_get_model(loc, CENTREPIECE_STRAIGHT, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        if (model1) {
            int yaw = 0;
            if (shape == CENTREPIECE_DIAGONAL) {
                yaw += 256;
            }

            int height;
            if (rotation == 1 || rotation == 3) {
                width = loc->length;
                height = loc->width;
            } else {
                width = loc->width;
                height = loc->length;
            }

            world3d_add_loc(scene, level, x, z, y, model1, NULL, bitset, info, width, height, yaw);
        }

        if (loc->blockwalk) {
            collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape >= ROOF_STRAIGHT) {
        model1 = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_loc(scene, level, x, z, y, model1, NULL, bitset, info, 1, 1, 0);

        if (loc->blockwalk) {
            collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALL_STRAIGHT) {
        model1 = loctype_get_model(loc, WALL_STRAIGHT, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_TYPE[rotation], 0, model1, NULL, bitset, info);

        if (loc->blockwalk) {
            collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALL_DIAGONALCORNER) {
        model1 = loctype_get_model(loc, WALL_DIAGONALCORNER, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_CORNER_TYPE[rotation], 0, model1, NULL, bitset, info);

        if (loc->blockwalk) {
            collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALL_L) {
        int nextRotation = rotation + 1 & 0x3;
        Model *model3 = loctype_get_model(loc, WALL_L, rotation + 4, heightSW, heightSE, heightNE, heightNW, -1);
        model2 = loctype_get_model(loc, WALL_L, nextRotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_TYPE[rotation], ROTATION_WALL_TYPE[nextRotation], model3, model2, bitset, info);

        if (loc->blockwalk) {
            collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALL_SQUARECORNER) {
        model1 = loctype_get_model(loc, WALL_SQUARECORNER, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_CORNER_TYPE[rotation], 0, model1, NULL, bitset, info);

        if (loc->blockwalk) {
            collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALL_DIAGONAL) {
        model1 = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_loc(scene, level, x, z, y, model1, NULL, bitset, info, 1, 1, 0);

        if (loc->blockwalk) {
            collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_STRAIGHT_NOOFFSET) {
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model1, info, rotation * 512, ROTATION_WALL_TYPE[rotation]);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_STRAIGHT_OFFSET) {
        offset = 16;
        width = world3d_get_wallbitset(scene, level, x, z);

        if (width > 0) {
            offset = loctype_get(width >> 14 & 0x7fff)->wallwidth;
        }

        model2 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, WALL_DECORATION_ROTATION_FORWARD_X[rotation] * offset, WALL_DECORATION_ROTATION_FORWARD_Z[rotation] * offset, bitset, model2, info, rotation * 512, ROTATION_WALL_TYPE[rotation]);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_DIAGONAL_OFFSET) {
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model1, info, rotation, 256);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_DIAGONAL_NOOFFSET) {
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model1, info, rotation, 512);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_DIAGONAL_BOTH) {
        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model1, info, rotation, 768);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    }
}

void clearLandscape(World *world, int startX, int startZ, int endX, int endZ) {
    int8_t waterOverlay = 0;
    for (int i = 0; i < _FloType.count; i++) {
        if (platform_strcasecmp(_FloType.instances[i]->name, "water") == 0) {
            waterOverlay = (int8_t)(i + 1);
            break;
        }
    }

    for (int z = startX; z < startX + endX; z++) {
        for (int x = startZ; x < startZ + endZ; x++) {
            if (x < 0 || x >= world->maxTileX || z < 0 || z >= world->maxTileZ) {
                continue;
            }

            world->levelTileOverlayIds[0][x][z] = waterOverlay;

            for (int level = 0; level < 4; level++) {
                world->levelHeightmap[level][x][z] = 0;
                world->levelTileFlags[level][x][z] = 0;
            }
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
                            if (level == 0) {
                                world->levelHeightmap[0][stx][stz] = -perlinNoise(stx + originX + 932731, stz + originZ + 556238) * 8;
                            } else {
                                world->levelHeightmap[level][stx][stz] = world->levelHeightmap[level - 1][stx][stz] - 240;
                            }
                            break;
                        }

                        if (opcode == 1) {
                            int height = g1(buf);
                            if (height == 1) {
                                height = 0;
                            }
                            if (level == 0) {
                                world->levelHeightmap[0][stx][stz] = -height * 8;
                            } else {
                                world->levelHeightmap[level][stx][stz] = world->levelHeightmap[level - 1][stx][stz] - height * 8;
                            }
                            break;
                        }

                        if (opcode <= 49) {
                            world->levelTileOverlayIds[level][stx][stz] = g1b(buf);
                            world->levelTileOverlayShape[level][stx][stz] = (int8_t)((opcode - 2) >> 2);
                            world->levelTileOverlayRotation[level][stx][stz] = (int8_t)(opcode - 2 & 0x3);
                        } else if (opcode <= 81) {
                            world->levelTileFlags[level][stx][stz] = (int8_t)(opcode - 49);
                        } else {
                            world->levelTileUnderlayIds[level][stx][stz] = (int8_t)(opcode - 81);
                        }
                    }
                } else {
                    while (true) {
                        opcode = g1(buf);
                        if (opcode == 0) {
                            break;
                        }

                        if (opcode == 1) {
                            g1(buf);
                            break;
                        }

                        if (opcode <= 49) {
                            g1(buf);
                        }
                    }
                }
            }
        }
    }
    free(buf);
}

void world_load_locations(World *world, World3D *scene, LinkList *locs, CollisionMap **collision, int8_t *src, int src_len, int xOffset, int zOffset) {
#if defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY)
    // 2026-09-14: temporary decode-only diagnostic (PS2_LOC_DECODE_ONLY, set in ps2.yaml - remove
    // once this question is answered, don't leave enabled). The real-hardware hang has been narrowed
    // to somewhere inside this function's first call (mapsquare 49_49) - every fix downstream of
    // here (loc->anim bounds, model_from_id() NULL, the GS-sync removal above) changed nothing, so
    // this isolates the one remaining question: can real hardware parse a loc stream to completion
    // when it does absolutely nothing with the decoded locations? Three changes, all reverted
    // together when this flag is undefined: (1) a stack Packet instead of packet_new()'s heap
    // allocation, matching packet_new()'s own field-by-field init exactly - packet_new() has never
    // been proven safe at this specific call site under this project's own measured tight-heap
    // conditions; (2) world_add_loc2() (and everything downstream of it - LocType/Model/World3D/
    // collision/caches) is skipped entirely, this function only parses; (3) hard forward-progress
    // guards (iteration caps + a buf->pos stall check) so a genuinely stuck decode returns via a
    // single one-shot log instead of hanging indistinguishably from a real fault. Deliberately zero
    // per-iteration I/O - this needs to be as close to a silent, pure parser as possible so nothing
    // about the test itself can be mistaken for the bug being tested.
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
    // The per-mapsquare checkpoint in client_build_scene() narrowed a real-hardware hang to
    // *inside* the very first call to this function (mapsquare 49_49, the first of 9 squares,
    // right after a fresh bump_allocator_reset()) - so it's not simple arena exhaustion building
    // up across squares. This function streams an unknown number of loc placements per square with
    // no visibility into how many it gets through before dying - a per-N-placements heartbeat
    // (reusing ps2_boot_progress(), the same font-less raw-GS progress indicator model_unpack()
    // uses, since this function has no PixMap/Client access to draw real text) tells us whether it
    // dies on the first placement (points at something wrong with this square's specific data or
    // the very first loc type/model it references) or gets deep into the square first (points at
    // per-instance cost, e.g. bump allocator fill-up within a single dense square).
    int ps2_loc_count = 0;
    int64_t ps2_t0 = rs2_now();
#ifdef PS2_LOC_DECODE_ONLY
    (void)ps2_t0; // only consumed by the throttled progress log, which decode-only mode skips
#endif
#endif

#ifdef __PS2__
    // Match static location placement to the same bounded local traversal window as terrain. The
    // previous fixed 32x32 block (32..63) left most of the now-traversable 80x80 scene without
    // walls/objects and made them disappear after normal scene recentres. If this proves too costly,
    // replace it with a moving/recycled loc window rather than expanding residency further.
    const int ps2LocMinTile = PS2_TERRAIN_MIN_TILE;
    const int ps2LocMaxTile = PS2_TERRAIN_MAX_TILE;
#endif

    while (true) {
#if defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY)
        if (++ps2_outer_guard > 10000 || buf->pos == ps2_prev_pos) {
            rs2_error("world_load_locations: DECODE-ONLY guard tripped (outer_guard=%d pos=%d prev_pos=%d) - no "
                      "forward progress or excessive iterations, aborting\n",
                      ps2_outer_guard, buf->pos, ps2_prev_pos);
            return;
        }
        ps2_prev_pos = buf->pos;
#endif
#ifdef __PS2__
        // gsmarts()/g1() do no bounds checking at all (same known hazard already documented at
        // loctype_decode()'s unrecognised-opcode path: an unrecognised/desynced decode "can read
        // arbitrarily far past the buffer and crash much later, far from this site") - a truncated
        // or subtly corrupt loc stream for this one mapsquare would spin this loop reading
        // whatever heap bytes happen to follow `buf->data` forever, never naturally hitting a 0
        // terminator, which looks exactly like a hang and gives zero indication anything is wrong.
        // This turns that into a clean, logged, recoverable failure instead of silent unbounded
        // reads - it does not change behavior at all for a well-formed stream.
        if (buf->pos >= buf->length) {
            rs2_error("world_load_locations: decode ran past end of buffer (pos=%d len=%d) after %d placements - "
                      "truncated or corrupt loc data, aborting this mapsquare\n",
                      buf->pos, buf->length, ps2_loc_count);
#if !(defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY))
            free(buf);
#endif
            return;
        }
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
            if (++ps2_inner_guard > 100000) {
                rs2_error("world_load_locations: DECODE-ONLY inner guard tripped (inner_guard=%d) - aborting\n",
                          ps2_inner_guard);
                return;
            }
#endif
#ifdef __PS2__
            if (buf->pos >= buf->length) {
                rs2_error("world_load_locations: decode ran past end of buffer (pos=%d len=%d) mid-loc (id=%d) after "
                          "%d placements - truncated or corrupt loc data, aborting this mapsquare\n",
                          buf->pos, buf->length, locId, ps2_loc_count);
#if !(defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY))
                free(buf);
#endif
                return;
            }
#endif
            int deltaPos = gsmarts(buf);
            if (deltaPos == 0) {
                break;
            }

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
            // `level` is masked to 3 bits by the wire format's own encoding (locPos >> 12 leaves no
            // higher bits once locPos itself is well-formed) but nothing here actually enforces
            // that - a desynced/corrupt stream can produce a `locPos` with garbage above bit 12, and
            // `level`/`currentLevel` then indexes world->levelHeightmap/levelTileFlags/collision[]
            // directly with ZERO bounds checking (only stx/stz get checked below). Those are fixed
            // 4-level arrays (see client.c's `calloc(4, ...)` for levelHeightmap/levelTileFlags) -
            // an out-of-range level is a genuine out-of-bounds heap read/write, not just wrong game
            // behavior, and exactly the kind of thing that corrupts silently on a forgiving desktop
            // allocator but faults immediately on PS2's much smaller, tighter heap.
            if (level < 0 || level >= 4) {
                rs2_error("world_load_locations: loc #%d id=%d has out-of-range level %d (locPos=%d) - skipping "
                          "this placement, likely stream desync\n",
                          ps2_loc_count + 1, locId, level, locPos);
                continue;
            }
#endif

#if defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY)
            // Decode-only: intentionally do NOT touch world->levelTileFlags/collision/world_add_loc2()
            // here - this build's only job is to prove the parse itself reaches the end of the
            // stream (or a guard trip) cleanly, with zero placement/model/collision side effects.
            (void)stx;
            (void)stz;
            (void)shape;
            (void)rotation;
#else
            if (stx > 0 && stz > 0 && stx < 104 - 1 && stz < 104 - 1
#ifdef __PS2__
                && stx >= ps2LocMinTile && stx < ps2LocMaxTile && stz >= ps2LocMinTile && stz < ps2LocMaxTile
#endif
            ) {
                int currentLevel = level;
                if ((world->levelTileFlags[1][stx][stz] & 0x2) == 2) {
                    currentLevel = level - 1;
                }

                CollisionMap *collisionMap = NULL;
                if (currentLevel >= 0) {
                    collisionMap = collision[currentLevel];
                }

#ifdef __PS2__
                // 2026-09-14: ps2_boot_progress(), which used to fire alongside every throttled log
                // line here, does a real gsKit_queue_exec()+gsKit_sync_flip() - a full GS sync, not a
                // cheap draw. A dense mapsquare (confirmed up to ~950 locs in real testing) could hit
                // this ~100 times (5 unthrottled + one per 10 after that) in a single square, and a
                // full scene rebuilds several squares - this is the exact same "the diagnostic costs
                // more than what it measures" trap already proven out once this session with the
                // bzip heartbeat (see thirdparty/bzip.c's history), just not yet caught here. Removed
                // the GS sync entirely and widened the log throttle (10 -> 50) so this loop's own
                // instrumentation can no longer plausibly be mistaken for - or itself cause - a
                // real-hardware stall.
                ps2_loc_count++;
                if (ps2_loc_count <= 5 || ps2_loc_count % 50 == 0) {
                    rs2_log("world_load_locations: loc #%d id=%d shape=%d rot=%d x=%d z=%d level=%d bump=%d/%d elapsed=%dms\n",
                            ps2_loc_count, locId, shape, rotation, stx, stz, level, bump_allocator_used(), bump_allocator_capacity(),
                            (int)(rs2_now() - ps2_t0));
                }
#endif
                world_add_loc2(world, level, stx, stz, scene, locs, collisionMap, locId, shape, rotation);
            }
#endif
        }
    }
#if !(defined(__PS2__) && defined(PS2_LOC_DECODE_ONLY))
    free(buf);
#endif
}

void world_add_loc2(World *world, int level, int x, int z, World3D *scene, LinkList *locs, CollisionMap *collision, int locId, int shape, int rotation) {
#ifdef __PS2__
    const bool ps2_watch_loc_684 = locId == 684 && shape == CENTREPIECE_STRAIGHT && level == 0 && x == 13 && z == 2;
    // 2026-09-14: PS2_LOC_DECODE_ONLY just proved decoding, Packet handling, and world_build() are
    // all innocent - full loc decode + world_build() completed cleanly on real hardware when this
    // function alone was skipped. This is the next, never-before-instrumented boundary: a one-shot
    // (first 5 calls only, via ps2_crash_client - the same global the disabled exception handler was
    // going to use, reused here since it's already proven valid for the whole program lifetime and
    // this function has no Client* of its own to thread through) entry checkpoint, so we can see
    // exactly which loc (id/shape/level/x/z) this dies on, or confirms it's never even entered.
    // This path can run hundreds of times per square.  Keep the marker sparse enough that its GS
    // updates cannot become the hang being investigated: show the first three placements, then one
    // every hundredth placement.
    static int ps2_wal2_call_num = 0;
    ps2_wal2_call_num++;
    if (ps2_wal2_call_num <= 3 || (ps2_wal2_call_num % 100) == 0) {
        char ps2_wal2_msg[80];
        snprintf(ps2_wal2_msg, sizeof(ps2_wal2_msg), "world_add_loc2 ENTER call#%d locId=%d shape=%d level=%d x=%d z=%d",
                 ps2_wal2_call_num, locId, shape, level, x, z);
        ps2_scene_checkpoint(ps2_crash_client, ps2_wal2_msg);
    }
#endif
    if (_World.lowMemory) {
        if ((world->levelTileFlags[level][x][z] & 0x10) != 0) {
            return;
        }

        if (world_get_drawlevel(world, level, x, z) != _World.levelBuilt) {
            return;
        }
    }

    int heightSW = world->levelHeightmap[level][x][z];
    int heightSE = world->levelHeightmap[level][x + 1][z];
    int heightNE = world->levelHeightmap[level][x + 1][z + 1];
    int heightNW = world->levelHeightmap[level][x][z + 1];
    int y = (heightSW + heightSE + heightNW + heightNE) >> 2;

 #ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, "loc 684: before loctype_get");
    }
 #endif
    LocType *loc = loctype_get(locId);
 #ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, "loc 684: loctype_get returned");
    }
 #endif
    int bitset = x + (z << 7) + (locId << 14) + 0x40000000;
    if (!loc->active) {
        bitset += INT_MIN;
    }

    int8_t info = (int8_t)((rotation << 6) + shape);
    Model *model;
    int width;
    int offset;
    Model *model1;

    if (shape == GROUNDDECOR) {
        if (_World.lowMemory && !loc->active && !loc->forcedecor) {
            return;
        }

        model = loctype_get_model(loc, GROUNDDECOR, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_grounddecoration(scene, model, level, x, z, y, bitset, info);

        if (loc->blockwalk && loc->active && collision) {
            collisionmap_set_blocked(collision, x, z);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 3, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == CENTREPIECE_STRAIGHT || shape == CENTREPIECE_DIAGONAL) {
 #ifdef __PS2__
        if (ps2_watch_loc_684) {
            ps2_scene_checkpoint(ps2_crash_client, "loc 684: before model lookup");
        }
 #endif
        model = loctype_get_model(loc, CENTREPIECE_STRAIGHT, rotation, heightSW, heightSE, heightNE, heightNW, -1);
 #ifdef __PS2__
        if (ps2_watch_loc_684) {
            ps2_scene_checkpoint(ps2_crash_client, "loc 684: model lookup returned");
        }
 #endif

        if (model) {
            int yaw = 0;
            if (shape == CENTREPIECE_DIAGONAL) {
                yaw += 256;
            }

            int height;
            if (rotation == 1 || rotation == 3) {
                width = loc->length;
                height = loc->width;
            } else {
                width = loc->width;
                height = loc->length;
            }

 #ifdef __PS2__
            if (ps2_watch_loc_684) {
                ps2_phase_checkpoint(ps2_crash_client, "loc 684: before scene insert");
            }
 #endif
            if (world3d_add_loc(scene, level, x, z, y, model, NULL, bitset, info, width, height, yaw) && loc->shadow) {
                for (int dx = 0; dx <= width; dx++) {
                    for (int dz = 0; dz <= height; dz++) {
                        int shade = model->radius / 4;
                        if (shade > 30) {
                            shade = 30;
                        }

                        if (shade > world->levelShademap[level][x + dx][z + dz]) {
                            world->levelShademap[level][x + dx][z + dz] = (int8_t)shade;
                        }
                    }
                }
            }
        }

        if (loc->blockwalk && collision) {
            collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape >= ROOF_STRAIGHT) {
        model = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_loc(scene, level, x, z, y, model, NULL, bitset, info, 1, 1, 0);

        if (shape >= ROOF_STRAIGHT && shape <= ROOF_FLAT && shape != ROOF_DIAGONAL_WITH_ROOFEDGE && level > 0) {
            world->levelOccludemap[level][x][z] |= 0x924;
        }

        if (loc->blockwalk && collision) {
            collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALL_STRAIGHT) {
        model = loctype_get_model(loc, WALL_STRAIGHT, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_TYPE[rotation], 0, model, NULL, bitset, info);

        if (rotation == 0) {
            if (loc->shadow) {
                world->levelShademap[level][x][z] = 50;
                world->levelShademap[level][x][z + 1] = 50;
            }

            if (loc->occlude) {
                world->levelOccludemap[level][x][z] |= 0x249;
            }
        } else if (rotation == 1) {
            if (loc->shadow) {
                world->levelShademap[level][x][z + 1] = 50;
                world->levelShademap[level][x + 1][z + 1] = 50;
            }

            if (loc->occlude) {
                world->levelOccludemap[level][x][z + 1] |= 0x492;
            }
        } else if (rotation == 2) {
            if (loc->shadow) {
                world->levelShademap[level][x + 1][z] = 50;
                world->levelShademap[level][x + 1][z + 1] = 50;
            }

            if (loc->occlude) {
                world->levelOccludemap[level][x + 1][z] |= 0x249;
            }
        } else if (rotation == 3) {
            if (loc->shadow) {
                world->levelShademap[level][x][z] = 50;
                world->levelShademap[level][x + 1][z] = 50;
            }

            if (loc->occlude) {
                world->levelOccludemap[level][x][z] |= 0x492;
            }
        }

        if (loc->blockwalk && collision) {
            collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        }

        if (loc->wallwidth != 16) {
            world3d_set_walldecorationoffset(scene, level, x, z, loc->wallwidth);
        }
    } else if (shape == WALL_DIAGONALCORNER) {
        model = loctype_get_model(loc, WALL_DIAGONALCORNER, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_CORNER_TYPE[rotation], 0, model, NULL, bitset, info);

        if (loc->shadow) {
            if (rotation == 0) {
                world->levelShademap[level][x][z + 1] = 50;
            } else if (rotation == 1) {
                world->levelShademap[level][x + 1][z + 1] = 50;
            } else if (rotation == 2) {
                world->levelShademap[level][x + 1][z] = 50;
            } else if (rotation == 3) {
                world->levelShademap[level][x][z] = 50;
            }
        }

        if (loc->blockwalk && collision) {
            collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALL_L) {
        int nextRotation = rotation + 1 & 0x3;
        Model *model3 = loctype_get_model(loc, WALL_L, rotation + 4, heightSW, heightSE, heightNE, heightNW, -1);
        model1 = loctype_get_model(loc, WALL_L, nextRotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_TYPE[rotation], ROTATION_WALL_TYPE[nextRotation], model3, model1, bitset, info);

        if (loc->occlude) {
            if (rotation == 0) {
                world->levelOccludemap[level][x][z] |= 0x109;
                world->levelOccludemap[level][x][z + 1] |= 0x492;
            } else if (rotation == 1) {
                world->levelOccludemap[level][x][z + 1] |= 0x492;
                world->levelOccludemap[level][x + 1][z] |= 0x249;
            } else if (rotation == 2) {
                world->levelOccludemap[level][x + 1][z] |= 0x249;
                world->levelOccludemap[level][x][z] |= 0x492;
            } else if (rotation == 3) {
                world->levelOccludemap[level][x][z] |= 0x492;
                world->levelOccludemap[level][x][z] |= 0x249;
            }
        }

        if (loc->blockwalk && collision) {
            collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        }

        if (loc->wallwidth != 16) {
            world3d_set_walldecorationoffset(scene, level, x, z, loc->wallwidth);
        }
    } else if (shape == WALL_SQUARECORNER) {
        model = loctype_get_model(loc, WALL_SQUARECORNER, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_wall(scene, level, x, z, y, ROTATION_WALL_CORNER_TYPE[rotation], 0, model, NULL, bitset, info);

        if (loc->shadow) {
            if (rotation == 0) {
                world->levelShademap[level][x][z + 1] = 50;
            } else if (rotation == 1) {
                world->levelShademap[level][x + 1][z + 1] = 50;
            } else if (rotation == 2) {
                world->levelShademap[level][x + 1][z] = 50;
            } else if (rotation == 3) {
                world->levelShademap[level][x][z] = 50;
            }
        }

        if (loc->blockwalk && collision) {
            collisionmap_add_wall(collision, x, z, shape, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 0, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALL_DIAGONAL) {
        model = loctype_get_model(loc, shape, rotation, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_add_loc(scene, level, x, z, y, model, NULL, bitset, info, 1, 1, 0);

        if (loc->blockwalk && collision) {
            collisionmap_add_loc(collision, x, z, loc->width, loc->length, rotation, loc->blockrange);
        }

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 2, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_STRAIGHT_NOOFFSET) {
        model = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model, info, rotation * 512, ROTATION_WALL_TYPE[rotation]);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_STRAIGHT_OFFSET) {
        offset = 16;
        width = world3d_get_wallbitset(scene, level, x, z);
        if (width > 0) {
            offset = loctype_get(width >> 14 & 0x7fff)->wallwidth;
        }

        model1 = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, WALL_DECORATION_ROTATION_FORWARD_X[rotation] * offset, WALL_DECORATION_ROTATION_FORWARD_Z[rotation] * offset, bitset, model1, info, rotation * 512, ROTATION_WALL_TYPE[rotation]);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_DIAGONAL_OFFSET) {
        model = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model, info, rotation, 256);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_DIAGONAL_NOOFFSET) {
        model = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model, info, rotation, 512);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    } else if (shape == WALLDECOR_DIAGONAL_BOTH) {
        model = loctype_get_model(loc, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightSW, heightSE, heightNE, heightNW, -1);
        world3d_set_walldecoration(scene, level, x, z, y, 0, 0, bitset, model, info, rotation, 768);

        if (loc->anim != -1 && loc->anim < _SeqType.count) {
            linklist_add_tail(locs, &locentity_new(locId, level, 1, x, z, _SeqType.instances[loc->anim], true)->link);
        }
    }
#ifdef __PS2__
    // 2026-09-14: closes the last unbisected gap - loctype_get_model() now provably completes
    // end-to-end (its own "done" checkpoint fired), but world_add_loc2() itself had no exit marker,
    // so whatever it does with the returned model (world3d_add_*/collisionmap_*/linklist_add_tail,
    // depending on which shape branch this loc took) was invisible. Same sampling as the entry
    // checkpoint, so this only fires for the same sampled calls.
    if (ps2_wal2_call_num <= 10 || (ps2_wal2_call_num % 4) == 1) {
        char ps2_wal2_msg_done[48];
        snprintf(ps2_wal2_msg_done, sizeof(ps2_wal2_msg_done), "world_add_loc2 done call#%d", ps2_wal2_call_num);
        ps2_scene_checkpoint(ps2_crash_client, ps2_wal2_msg_done);
    }
#endif
}

// `underlayId`/`overlayId` come straight off decompressed map data (`& 0xff`, so 0-255) and were
// used to index `_FloType.instances[id - 1]` with no check against `_FloType.count` (the real number
// of FloTypes loaded from flo.dat, normally well under 255). A corrupt/desynced map square - or any
// future flo.dat that legitimately has fewer entries - reads a garbage/OOB pointer here, and every
// downstream field access (`flu->chroma`, `flu->luminance`, ...) is then a wild-pointer dereference,
// not just a bad game-logic value. This is exactly the kind of bug that can differ between PCSX2 and
// real hardware: same corrupt id, but what happens to live in memory just past `_FloType.instances[]`
// differs, and a wild read/deref that a forgiving desktop/emulated heap tolerates can fault immediately
// on the real EE.
static FloType *world_flotype_get(int id, int level, int x, int z) {
    if (id > _FloType.count) {
        rs2_error("world_build: floType id %d out of range (count=%d) at level=%d x=%d z=%d - treating as absent, "
                  "likely corrupt/desynced map data\n",
                  id, _FloType.count, level, x, z);
        return NULL;
    }
    return _FloType.instances[id - 1];
}

#ifdef __PS2__
void world_build(World *world, World3D *scene, CollisionMap **collision, Client *c) {
    char ps2_stage_label[48];
    ps2_scene_checkpoint(c, "WORLD BUILD ENTER");
    ps2_phase_checkpoint(c, "WORLD BUILD ENTER");
#else
void world_build(World *world, World3D *scene, CollisionMap **collision) {
#endif
    for (int level = 0; level < 4; level++) {
        for (int x = 0; x < 104; x++) {
            for (int z = 0; z < 104; z++) {
                // solid
                if ((world->levelTileFlags[level][x][z] & 0x1) == 1) {
                    int trueLevel = level;

                    // bridge
                    if ((world->levelTileFlags[1][x][z] & 0x2) == 2) {
                        trueLevel--;
                    }

                    if (trueLevel >= 0) {
                        collisionmap_set_blocked(collision[trueLevel], x, z);
                    }
                }
            }
        }
    }
#ifdef __PS2__
    ps2_scene_checkpoint(c, "collision flags done");
    ps2_phase_checkpoint(c, "collision flags done");
#endif

    _World.randomHueOffset += (int)(jrand() * 5.0) - 2;
    if (_World.randomHueOffset < -8) {
        _World.randomHueOffset = -8;
    } else if (_World.randomHueOffset > 8) {
        _World.randomHueOffset = 8;
    }

    _World.randomLightnessOffset += (int)(jrand() * 5.0) - 2;
    if (_World.randomLightnessOffset < -16) {
        _World.randomLightnessOffset = -16;
    } else if (_World.randomLightnessOffset > 16) {
        _World.randomLightnessOffset = 16;
    }

    for (int level = 0; level < 4; level++) {
#ifdef __PS2__
        snprintf(ps2_stage_label, sizeof(ps2_stage_label), "level %d lighting begin", level);
        ps2_scene_checkpoint(c, ps2_stage_label);
#endif
        int8_t (*shademap)[104 + 1] = world->levelShademap[level];
        int8_t lightAmbient = 96;
        int lightAttenuation = 768;
        int8_t lightX = -50;
        int8_t lightY = -10;
        int8_t lightZ = -50;
#ifdef USE_FLOATS
        // 2026-09-14: PS2's EE FPU is single-precision only (per the original C client author) - this
        // loop calls sqrt() up to 104*104 times PER LEVEL (4 levels every scene build), tens of
        // thousands of double-precision sqrt calls that would otherwise be software-emulated at real
        // cost instead of hardware-accelerated. Almost certainly the single biggest contributor to
        // this whole session's real-hardware-only "hangs" that never reproduced on PCSX2/desktop
        // (native double FPU support there) - not a crash, just severe, otherwise-invisible slowness.
        int lightMag = (int)sqrtf((float)(lightX * lightX + lightY * lightY + lightZ * lightZ));
#else
        int lightMag = (int)sqrt(lightX * lightX + lightY * lightY + lightZ * lightZ);
#endif
        int lightMagnitude = lightAttenuation * lightMag >> 8;

        for (int z = 1; z < world->maxTileZ - 1; z++) {
            for (int x = 1; x < world->maxTileX - 1; x++) {
                int dx = world->levelHeightmap[level][x + 1][z] - world->levelHeightmap[level][x - 1][z];
                int dz = world->levelHeightmap[level][x][z + 1] - world->levelHeightmap[level][x][z - 1];
#ifdef USE_FLOATS
                int len = (int)sqrtf((float)(dx * dx + dz * dz + 65536));
#else
                int len = (int)sqrt(dx * dx + dz * dz + 65536);
#endif
                int normalX = (dx << 8) / len;
                int normalY = 65536 / len;
                int normalZ = (dz << 8) / len;
                int light = lightAmbient + (lightX * normalX + lightY * normalY + lightZ * normalZ) / lightMagnitude;
                int shade = (shademap[x - 1][z] >> 2) + (shademap[x + 1][z] >> 3) + (shademap[x][z - 1] >> 2) + (shademap[x][z + 1] >> 3) + (shademap[x][z] >> 1);
                world->levelLightmap[x][z] = light - shade;
            }
        }

#ifdef __PS2__
        snprintf(ps2_stage_label, sizeof(ps2_stage_label), "level %d lighting done", level);
        ps2_scene_checkpoint(c, ps2_stage_label);
        snprintf(ps2_stage_label, sizeof(ps2_stage_label), "level %d landscape begin", level);
        ps2_scene_checkpoint(c, ps2_stage_label);
#endif

        for (int z = 0; z < world->maxTileZ; z++) {
            world->blendChroma[z] = 0;
            world->blendSaturation[z] = 0;
            world->blendLightness[z] = 0;
            world->blendLuminance[z] = 0;
            world->blendMagnitude[z] = 0;
        }

        for (int x0 = -5; x0 < world->maxTileX + 5; x0++) {
#ifdef __PS2__
            // Sparse per-row breadcrumb, not per-tile: this loop runs maxTileX+10 (~114) times per
            // level, each iteration doing an O(maxTileZ) window-blend pass - cheap enough to log every
            // 16 rows without the I/O itself becoming a confound (see world_load_locations' own
            // throttling note for why unconditional per-iteration logging was rejected elsewhere in
            // this file).
            if (x0 >= 0 && x0 % 16 == 0) {
                snprintf(ps2_stage_label, sizeof(ps2_stage_label), "land L=%d X=%d", level, x0);
                rs2_log("%s\n", ps2_stage_label);
                ps2_boot_progress((x0 * 100) / (world->maxTileX + 10));
            }
#endif
            for (int z0 = 0; z0 < world->maxTileZ; z0++) {
                int x1 = x0 + 5;
                int debugMag;
                (void)debugMag;

                if (x1 >= 0 && x1 < world->maxTileX) {
                    int underlayId = world->levelTileUnderlayIds[level][x1][z0] & 0xff;

                    if (underlayId > 0) {
                        FloType *flu = world_flotype_get(underlayId, level, x1, z0);
                        if (flu) {
                            world->blendChroma[z0] += flu->chroma;
                            world->blendSaturation[z0] += flu->saturation;
                            world->blendLightness[z0] += flu->lightness;
                            world->blendLuminance[z0] += flu->luminance;
                            debugMag = world->blendMagnitude[z0]++;
                        }
                    }
                }

                int x2 = x0 - 5;
                if (x2 >= 0 && x2 < world->maxTileX) {
                    int underlayId = world->levelTileUnderlayIds[level][x2][z0] & 0xff;

                    if (underlayId > 0) {
                        // Must mirror the exact same validity decision the entering-window add made for
                        // this tile (world_flotype_get is a pure function of stable map data), or the
                        // sliding-window sum desyncs permanently: skipping only the subtract (while the
                        // add above went through) would leave a phantom contribution in the window
                        // forever; skipping only the add would later underflow when this same tile leaves.
                        FloType *flu = world_flotype_get(underlayId, level, x2, z0);
                        if (flu) {
                            world->blendChroma[z0] -= flu->chroma;
                            world->blendSaturation[z0] -= flu->saturation;
                            world->blendLightness[z0] -= flu->lightness;
                            world->blendLuminance[z0] -= flu->luminance;
                            debugMag = world->blendMagnitude[z0]--;
                        }
                    }
                }
            }

            if (x0 >= 1 && x0 < world->maxTileX - 1) {
                int hueAccumulator = 0;
                int saturationAccumulator = 0;
                int lightnessAccumulator = 0;
                int luminanceAccumulator = 0;
                int magnitudeAccumulator = 0;

                for (int z0 = -5; z0 < world->maxTileZ + 5; z0++) {
                    int dz1 = z0 + 5;
                    if (dz1 >= 0 && dz1 < world->maxTileZ) {
                        hueAccumulator += world->blendChroma[dz1];
                        saturationAccumulator += world->blendSaturation[dz1];
                        lightnessAccumulator += world->blendLightness[dz1];
                        luminanceAccumulator += world->blendLuminance[dz1];
                        magnitudeAccumulator += world->blendMagnitude[dz1];
                    }

                    int dz2 = z0 - 5;
                    if (dz2 >= 0 && dz2 < world->maxTileZ) {
                        hueAccumulator -= world->blendChroma[dz2];
                        saturationAccumulator -= world->blendSaturation[dz2];
                        lightnessAccumulator -= world->blendLightness[dz2];
                        luminanceAccumulator -= world->blendLuminance[dz2];
                        magnitudeAccumulator -= world->blendMagnitude[dz2];
                    }

                    if (z0 >= 1 && z0 < world->maxTileZ - 1
#ifdef __PS2__
                        // Only materialise Ground nodes around the local player.
                        // Height/collision arrays retain the full 104x104 map;
                        // the outer terrain is intentionally non-resident until
                        // a normal scene rebuild recentres this window.
                        && x0 >= PS2_TERRAIN_MIN_TILE && x0 < PS2_TERRAIN_MAX_TILE && z0 >= PS2_TERRAIN_MIN_TILE && z0 < PS2_TERRAIN_MAX_TILE
#endif
                        && (!_World.lowMemory || ((world->levelTileFlags[level][x0][z0] & 0x10) == 0 && world_get_drawlevel(world, level, x0, z0) == _World.levelBuilt))) {
                        int underlayId = world->levelTileUnderlayIds[level][x0][z0] & 0xff;
                        int overlayId = world->levelTileOverlayIds[level][x0][z0] & 0xff;

                        // Clamp out-of-range ids to "absent" right at the read, before any of the several
                        // downstream branches below can index `_FloType.instances[id - 1]` directly
                        // (the occlude check and the overlay tile-build both do) - see world_flotype_get's
                        // comment above for why an unclamped id here is a wild-pointer risk, not just a
                        // cosmetic one.
                        if (underlayId > _FloType.count) {
                            rs2_error("world_build: underlay id %d out of range (count=%d) at level=%d x=%d z=%d - "
                                      "treating tile as having no underlay, likely corrupt/desynced map data\n",
                                      underlayId, _FloType.count, level, x0, z0);
                            underlayId = 0;
                        }
                        if (overlayId > _FloType.count) {
                            rs2_error("world_build: overlay id %d out of range (count=%d) at level=%d x=%d z=%d - "
                                      "treating tile as having no overlay, likely corrupt/desynced map data\n",
                                      overlayId, _FloType.count, level, x0, z0);
                            overlayId = 0;
                        }

                        if (underlayId > 0 || overlayId > 0) {
                            int heightSW = world->levelHeightmap[level][x0][z0];
                            int heightSE = world->levelHeightmap[level][x0 + 1][z0];
                            int heightNE = world->levelHeightmap[level][x0 + 1][z0 + 1];
                            int heightNW = world->levelHeightmap[level][x0][z0 + 1];

                            int lightSW = world->levelLightmap[x0][z0];
                            int lightSE = world->levelLightmap[x0 + 1][z0];
                            int lightNE = world->levelLightmap[x0 + 1][z0 + 1];
                            int lightNW = world->levelLightmap[x0][z0 + 1];

                            int baseColor = -1;
                            int tintColor = -1;

                            if (underlayId > 0) {
                                int hue = hueAccumulator * 256 / luminanceAccumulator;
                                int saturation = saturationAccumulator / magnitudeAccumulator;
                                int lightness = lightnessAccumulator / magnitudeAccumulator;
                                baseColor = hsl24to16(hue, saturation, lightness);
                                int randomHue = hue + _World.randomHueOffset & 0xff;
                                lightness += _World.randomLightnessOffset;
                                if (lightness < 0) {
                                    lightness = 0;
                                } else if (lightness > 255) {
                                    lightness = 255;
                                }
                                tintColor = hsl24to16(randomHue, saturation, lightness);
                            }

                            if (level > 0) {
                                bool occludes = underlayId != 0 || world->levelTileOverlayShape[level][x0][z0] == 0;

                                if (overlayId > 0 && !_FloType.instances[overlayId - 1]->occlude) {
                                    occludes = false;
                                }

                                // occludes && flat
                                if (occludes && heightSW == heightSE && heightSW == heightNE && heightSW == heightNW) {
                                    world->levelOccludemap[level][x0][z0] |= 0x924;
                                }
                            }

                            int shadeColor = 0;
                            if (baseColor != -1) {
                                shadeColor = _Pix3D.palette[mulHSL(tintColor, 96)];
                            }

                            if (overlayId == 0) {
                                world3d_set_tile(scene, level, x0, z0, 0, 0, -1, heightSW, heightSE, heightNE, heightNW, mulHSL(baseColor, lightSW), mulHSL(baseColor, lightSE), mulHSL(baseColor, lightNE), mulHSL(baseColor, lightNW), 0, 0, 0, 0, shadeColor, 0);
                            } else {
                                int shape = world->levelTileOverlayShape[level][x0][z0] + 1;
                                int8_t rotation = world->levelTileOverlayRotation[level][x0][z0];
                                FloType *flo = _FloType.instances[overlayId - 1];
                                int textureId = flo->texture;
                                int hsl;
                                int rgb;

                                if (textureId >= 0) {
#if defined(__PS2__) && PS2_UNTEXTURED_TERRAIN
                                    // Keep the per-vertex light gradient, but use the existing
                                    // underlay/floor colour instead of a sampled terrain texture.
                                    // Texture assets remain available for characters and UI icons.
                                    textureId = -1;
                                    hsl = baseColor != -1 ? baseColor : hsl24to16(flo->hue, flo->saturation, flo->lightness);
                                    rgb = _Pix3D.palette[adjustLightness(hsl, 96)];
#else
                                    rgb = pix3d_get_average_texture_rgb(textureId);
                                    hsl = -1;
#endif
                                } else if (flo->rgb == MAGENTA) {
                                    rgb = 0;
                                    hsl = -2;
                                    textureId = -1;
                                } else {
                                    hsl = hsl24to16(flo->hue, flo->saturation, flo->lightness);
                                    rgb = _Pix3D.palette[adjustLightness(flo->hsl, 96)];
                                }

                                world3d_set_tile(scene, level, x0, z0, shape, rotation, textureId, heightSW, heightSE, heightNE, heightNW, mulHSL(baseColor, lightSW), mulHSL(baseColor, lightSE), mulHSL(baseColor, lightNE), mulHSL(baseColor, lightNW), adjustLightness(hsl, lightSW), adjustLightness(hsl, lightSE), adjustLightness(hsl, lightNE), adjustLightness(hsl, lightNW), shadeColor, rgb);
                            }
                        }
                    }
                }
            }
        }

#ifdef __PS2__
        snprintf(ps2_stage_label, sizeof(ps2_stage_label), "level %d landscape done", level);
        ps2_scene_checkpoint(c, ps2_stage_label);
        snprintf(ps2_stage_label, sizeof(ps2_stage_label), "level %d drawlevels begin", level);
        ps2_scene_checkpoint(c, ps2_stage_label);
#endif

        for (int stz = 1; stz < world->maxTileZ - 1; stz++) {
            for (int stx = 1; stx < world->maxTileX - 1; stx++) {
                world3d_set_drawlevel(scene, level, stx, stz, world_get_drawlevel(world, level, stx, stz));
            }
        }
#ifdef __PS2__
        snprintf(ps2_stage_label, sizeof(ps2_stage_label), "level %d drawlevels done", level);
        ps2_scene_checkpoint(c, ps2_stage_label);
#endif
    }

#ifdef __PS2__
    ps2_scene_checkpoint(c, "models begin");
    ps2_phase_checkpoint(c, "models begin");
#endif
    if (!_World.fullbright) {
        world3d_build_models(scene, 64, 768, -50, -10, -50);
    }
#ifdef __PS2__
    ps2_scene_checkpoint(c, "models done");
    ps2_phase_checkpoint(c, "models done");
    ps2_scene_checkpoint(c, "bridges begin");
    ps2_phase_checkpoint(c, "bridges begin");
#endif

    for (int x = 0; x < world->maxTileX; x++) {
        for (int z = 0; z < world->maxTileZ; z++) {
            if ((world->levelTileFlags[1][x][z] & 0x2) == 2) {
                world3d_set_bridge(scene, x, z);
            }
        }
    }
#ifdef __PS2__
    ps2_scene_checkpoint(c, "bridges done");
    ps2_phase_checkpoint(c, "bridges done");
#endif

    if (!_World.fullbright) {
        int wall0 = 0x1; // world->flag is set by walls with rotation 0 or 2
        int wall1 = 0x2; // world->flag is set by walls with rotation 1 or 3
        int floor = 0x4; // world->flag is set by floors which are flat

        for (int topLevel = 0; topLevel < 4; topLevel++) {
            if (topLevel > 0) {
                wall0 <<= 0x3;
                wall1 <<= 0x3;
                floor <<= 0x3;
            }
#ifdef __PS2__
            snprintf(ps2_stage_label, sizeof(ps2_stage_label), "occluders topLevel %d begin", topLevel);
            ps2_scene_checkpoint(c, ps2_stage_label);
#endif

            for (int level = 0; level <= topLevel; level++) {
                for (int tileZ = 0; tileZ <= world->maxTileZ; tileZ++) {
                    for (int tileX = 0; tileX <= world->maxTileX; tileX++) {
                        if ((world->levelOccludemap[level][tileX][tileZ] & wall0) != 0) {
                            int minTileZ = tileZ;
                            int maxTileZ = tileZ;
                            int minLevel = level;
                            int maxLevel = level;

                            while (minTileZ > 0 && (world->levelOccludemap[level][tileX][minTileZ - 1] & wall0) != 0) {
                                minTileZ--;
                            }

                            while (maxTileZ < world->maxTileZ && (world->levelOccludemap[level][tileX][maxTileZ + 1] & wall0) != 0) {
                                maxTileZ++;
                            }

                            while (minLevel > 0) {
                                for (int z = minTileZ; z <= maxTileZ; z++) {
                                    if ((world->levelOccludemap[minLevel - 1][tileX][z] & wall0) == 0) {
                                        goto min_level_found;
                                    }
                                }
                                minLevel--;
                            }
                        min_level_found:

                            while (maxLevel < topLevel) {
                                for (int z = minTileZ; z <= maxTileZ; z++) {
                                    if ((world->levelOccludemap[maxLevel + 1][tileX][z] & wall0) == 0) {
                                        goto max_level_found;
                                    }
                                }
                                maxLevel++;
                            }
                        max_level_found:;

                            int area = (maxLevel + 1 - minLevel) * (maxTileZ + 1 - minTileZ);
                            if (area >= 8) {
                                int minY = world->levelHeightmap[maxLevel][tileX][minTileZ] - 240;
                                int maxX = world->levelHeightmap[minLevel][tileX][minTileZ];

                                world3d_add_occluder(topLevel, 1, tileX * 128, minY, minTileZ * 128, tileX * 128, maxX, maxTileZ * 128 + 128);

                                for (int l = minLevel; l <= maxLevel; l++) {
                                    for (int z = minTileZ; z <= maxTileZ; z++) {
                                        world->levelOccludemap[l][tileX][z] &= ~wall0;
                                    }
                                }
                            }
                        }

                        if ((world->levelOccludemap[level][tileX][tileZ] & wall1) != 0) {
                            int minTileX = tileX;
                            int maxTileX = tileX;
                            int minLevel = level;
                            int maxLevel = level;

                            while (minTileX > 0 && (world->levelOccludemap[level][minTileX - 1][tileZ] & wall1) != 0) {
                                minTileX--;
                            }

                            while (maxTileX < world->maxTileX && (world->levelOccludemap[level][maxTileX + 1][tileZ] & wall1) != 0) {
                                maxTileX++;
                            }

                            while (minLevel > 0) {
                                for (int x = minTileX; x <= maxTileX; x++) {
                                    if ((world->levelOccludemap[minLevel - 1][x][tileZ] & wall1) == 0) {
                                        goto min_level2_found;
                                    }
                                }
                                minLevel--;
                            }
                        min_level2_found:

                            while (maxLevel < topLevel) {
                                for (int x = minTileX; x <= maxTileX; x++) {
                                    if ((world->levelOccludemap[maxLevel + 1][x][tileZ] & wall1) == 0) {
                                        goto max_level2_found;
                                    }
                                }
                                maxLevel++;
                            }
                        max_level2_found:;

                            int area = (maxLevel + 1 - minLevel) * (maxTileX + 1 - minTileX);
                            if (area >= 8) {
                                int minY = world->levelHeightmap[maxLevel][minTileX][tileZ] - 240;
                                int maxY = world->levelHeightmap[minLevel][minTileX][tileZ];

                                world3d_add_occluder(topLevel, 2, minTileX * 128, minY, tileZ * 128, maxTileX * 128 + 128, maxY, tileZ * 128);

                                for (int l = minLevel; l <= maxLevel; l++) {
                                    for (int x = minTileX; x <= maxTileX; x++) {
                                        world->levelOccludemap[l][x][tileZ] &= ~wall1;
                                    }
                                }
                            }
                        }
                        if ((world->levelOccludemap[level][tileX][tileZ] & floor) != 0) {
                            int minTileX = tileX;
                            int maxTileX = tileX;
                            int minTileZ = tileZ;
                            int maxTileZ = tileZ;

                            while (minTileZ > 0 && (world->levelOccludemap[level][tileX][minTileZ - 1] & floor) != 0) {
                                minTileZ--;
                            }

                            while (maxTileZ < world->maxTileZ && (world->levelOccludemap[level][tileX][maxTileZ + 1] & floor) != 0) {
                                maxTileZ++;
                            }

                            while (minTileX > 0) {
                                for (int z = minTileZ; z <= maxTileZ; z++) {
                                    if ((world->levelOccludemap[level][minTileX - 1][z] & floor) == 0) {
                                        goto min_tile_xz_found;
                                    }
                                }
                                minTileX--;
                            }
                        min_tile_xz_found:

                            while (maxTileX < world->maxTileX) {
                                for (int z = minTileZ; z <= maxTileZ; z++) {
                                    if ((world->levelOccludemap[level][maxTileX + 1][z] & floor) == 0) {
                                        goto max_tile_xz_found;
                                    }
                                }
                                maxTileX++;
                            }
                        max_tile_xz_found:

                            if ((maxTileX + 1 - minTileX) * (maxTileZ + 1 - minTileZ) >= 4) {
                                int y = world->levelHeightmap[level][minTileX][minTileZ];

                                world3d_add_occluder(topLevel, 4, minTileX * 128, y, minTileZ * 128, maxTileX * 128 + 128, y, maxTileZ * 128 + 128);

                                for (int x = minTileX; x <= maxTileX; x++) {
                                    for (int z = minTileZ; z <= maxTileZ; z++) {
                                        world->levelOccludemap[level][x][z] &= ~floor;
                                    }
                                }
                            }
                        }
                    }
                }
            }
#ifdef __PS2__
            snprintf(ps2_stage_label, sizeof(ps2_stage_label), "occluders topLevel %d done", topLevel);
            ps2_scene_checkpoint(c, ps2_stage_label);
#endif
        }
    }
#ifdef __PS2__
    ps2_scene_checkpoint(c, "WORLD BUILD DONE");
    ps2_phase_checkpoint(c, "WORLD BUILD DONE");
#endif
}

int world_get_drawlevel(World *world, int level, int stx, int stz) {
    if ((world->levelTileFlags[level][stx][stz] & 0x8) == 0) {
        return level <= 0 || (world->levelTileFlags[1][stx][stz] & 0x2) == 0 ? level : level - 1;
    } else {
        return 0;
    }
}
