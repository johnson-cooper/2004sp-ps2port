#include <stdio.h>
#include <stdlib.h>

#include "loctype.h"
#include "model.h"
#include "platform.h"
#ifdef __PS2__
// Only for the checkpoint below - ps2_crash_client (the exception handler's global, still valid to
// reuse here) and ps2_scene_checkpoint(). client.h doesn't include loctype.h (checked), no cycle.
#include "client.h"
#endif

LocTypeData _LocType = {0};

static void loctype_decode(LocType *loc, Packet *dat);

static LocType *loctype_new(void) {
    LocType *loc = calloc(1, sizeof(LocType));
    loc->index = -1;
    return loc;
}

void loctype_unpack(Jagfile *config) {
    _LocType.modelCacheStatic = lrucache_new(500);
#ifdef __PS2__
    // PS2 scene models live in a reset-only bump arena. Evicting a dynamic loc model does not
    // reclaim its arena bytes, so rebuilding an evicted variant later in the same dense scene
    // consumes the arena twice. 150 entries was small enough to thrash in large cities once the
    // full loc residency window was restored. 512 matches the existing PS2 side-cache strategy
    // while costing only a few KiB of BSS and no extra per-model heap allocations.
    _LocType.modelCacheDynamic = lrucache_new(512);
#else
    _LocType.modelCacheDynamic = lrucache_new(30*5);
#endif

    _LocType.dat = jagfile_to_packet(config, "loc.dat");
    Packet *idx = jagfile_to_packet(config, "loc.idx");

    _LocType.count = g2(idx);
    _LocType.offsets = calloc(_LocType.count, sizeof(int));

    int offset = 2;
    for (int id = 0; id < _LocType.count; id++) {
        _LocType.offsets[id] = offset;
        offset += g2(idx);
    }

    _LocType.cache = calloc(10, sizeof(LocType *));
    for (int id = 0; id < 10; id++) {
        _LocType.cache[id] = loctype_new();
    }

    packet_free(idx);
}

void loctype_free_global(void) {
    lrucache_free(_LocType.modelCacheStatic);
    lrucache_free(_LocType.modelCacheDynamic);
    free(_LocType.offsets);
    for (int i = 0; i < 10; i++) {
        free(_LocType.cache[i]);
    }
    free(_LocType.cache);
    packet_free(_LocType.dat);
}

LocType *loctype_get(int id) {
    for (int i = 0; i < 10; i++) {
        if (_LocType.cache[i]->index == id) {
            return _LocType.cache[i];
        }
    }

    _LocType.cachePos = (_LocType.cachePos + 1) % 10;
    LocType *loc = _LocType.cache[_LocType.cachePos];
    loc->index = id;
    loctype_reset(loc);
    if (id < 0 || id >= _LocType.count) {
        // see npctype_get()/objtype_get() for the same fix - a rev254 server can reference loc type
        // ids beyond what's in Client3's loaded loc.idx, and offsets[] is only _LocType.count entries.
        rs2_error("loctype_get: loc type id %d out of range (max %d), using defaults\n", id, _LocType.count - 1);
        return loc;
    }
    _LocType.dat->pos = _LocType.offsets[id];
    loctype_decode(loc, _LocType.dat);
    return loc;
}

void loctype_reset(LocType *loc) {
    free(loc->models);
    free(loc->shapes);
    free(loc->name);
    free(loc->desc);
    free(loc->recol_s);
    free(loc->recol_d);
    if (loc->op) {
        for (int i = 0; i < 5; i++) {
            free(loc->op[i]);
        }
        free(loc->op);
    }
    loc->models = NULL;
    loc->shapes = NULL;
    loc->name = NULL;
    loc->desc = NULL;
    loc->recol_s = NULL;
    loc->recol_d = NULL;
    loc->width = 1;
    loc->length = 1;
    loc->blockwalk = true;
    loc->blockrange = true;
    loc->active = false;
    loc->hillskew = false;
    loc->sharelight = false;
    loc->occlude = false;
    loc->anim = -1;
    loc->wallwidth = 16;
    loc->ambient = 0;
    loc->contrast = 0;
    loc->op = NULL;
    loc->animHasAlpha = false;
    loc->mapfunction = -1;
    loc->mapscene = -1;
    loc->mirror = false;
    loc->shadow = true;
    loc->resizex = 128;
    loc->resizey = 128;
    loc->resizez = 128;
    loc->forceapproach = 0;
    loc->offsetx = 0;
    loc->offsety = 0;
    loc->offsetz = 0;
    loc->forcedecor = false;
    loc->breakroutefinding = false;
    loc->raiseobject = -1; // sentinel: derived from blockwalk after decode if never explicitly set
}

static void loctype_decode(LocType *loc, Packet *dat) {
    int active = -1;

    while (true) {
        int code = g1(dat);
        if (code == 0) {
            break;
        }

        if (code == 1) {
            int count = g1(dat);
            loc->shapes_and_models_count = count;
            loc->shapes = calloc(count, sizeof(int));
            loc->models = calloc(count, sizeof(int));

            for (int i = 0; i < count; i++) {
                loc->models[i] = g2(dat);
                loc->shapes[i] = g1(dat);
            }
        } else if (code == 5) {
            int count = g1(dat);
            loc->shapes_and_models_count = count;
            loc->models = calloc(count, sizeof(int));
            loc->shapes = calloc(count, sizeof(int)); // rev254: no per-entry shape byte on the wire; TODO: verify CENTREPIECE_STRAIGHT default against real data

            for (int i = 0; i < count; i++) {
                loc->models[i] = g2(dat);
                loc->shapes[i] = CENTREPIECE_STRAIGHT;
            }
        } else if (code == 2) {
            loc->name = gjstr(dat);
        } else if (code == 3) {
            loc->desc = gjstr(dat);
        } else if (code == 14) {
            loc->width = g1(dat);
        } else if (code == 15) {
            loc->length = g1(dat);
        } else if (code == 17) {
            loc->blockwalk = false;
        } else if (code == 18) {
            loc->blockrange = false;
        } else if (code == 19) {
            active = g1(dat);

            if (active == 1) {
                loc->active = true;
            }
        } else if (code == 21) {
            loc->hillskew = true;
        } else if (code == 22) {
            loc->sharelight = true;
        } else if (code == 23) {
            loc->occlude = true;
        } else if (code == 24) {
            loc->anim = g2(dat);
            if (loc->anim == 65535) {
                loc->anim = -1;
            }
        } else if (code == 25) {
            loc->animHasAlpha = true;
        } else if (code == 28) {
            loc->wallwidth = g1(dat);
        } else if (code == 29) {
            loc->ambient = g1b(dat);
        } else if (code == 39) {
            loc->contrast = g1b(dat);
        } else if (code >= 30 && code < 39) {
            if (!loc->op) {
                loc->op = calloc(5, sizeof(char *));
            }

            loc->op[code - 30] = gjstr(dat);
            if (platform_strcasecmp(loc->op[code - 30], "hidden") == 0) {
                free(loc->op[code - 30]);
                loc->op[code - 30] = NULL;
            }
        } else if (code == 40) {
            int count = g1(dat);
            loc->recol_count = count;
            loc->recol_s = calloc(count, sizeof(int));
            loc->recol_d = calloc(count, sizeof(int));

            for (int i = 0; i < count; i++) {
                loc->recol_s[i] = g2(dat);
                loc->recol_d[i] = g2(dat);
            }
        } else if (code == 60) {
            loc->mapfunction = g2(dat);
        } else if (code == 62) {
            loc->mirror = true;
        } else if (code == 64) {
            loc->shadow = false;
        } else if (code == 65) {
            loc->resizex = g2(dat);
        } else if (code == 66) {
            loc->resizey = g2(dat);
        } else if (code == 67) {
            loc->resizez = g2(dat);
        } else if (code == 68) {
            loc->mapscene = g2(dat);
        } else if (code == 69) {
            loc->forceapproach = g1(dat);
        } else if (code == 70) {
            loc->offsetx = g2b(dat);
        } else if (code == 71) {
            loc->offsety = g2b(dat);
        } else if (code == 72) {
            loc->offsetz = g2b(dat);
        } else if (code == 73) {
            loc->forcedecor = true;
        } else if (code == 74) {
            loc->breakroutefinding = true;
        } else if (code == 75) {
            loc->raiseobject = g1(dat);
        } else {
            // see objtype_decode()/npctype_decode() for the same fix - an opcode this decoder
            // doesn't recognise (rev254 loc.dat can contain some) would otherwise desync dat->pos,
            // and g1/g2/g1b do no bounds checking, so that desync can read arbitrarily far past the
            // buffer and crash much later, far from this site. Stop decoding this loc instead.
            rs2_error("Error unrecognised loc config code: %d\n", code);
            return;
        }
    }

    if (!loc->shapes) {
        loc->shapes = calloc(1, sizeof(int)); // new int[0];
    }

    if (active == -1) {
        loc->active = loc->shapes_and_models_count > 0 && loc->shapes[0] == 10;

        if (loc->op) {
            loc->active = true;
        }
    }

    if (loc->breakroutefinding) {
        loc->blockwalk = false;
        loc->blockrange = false;
    }

    if (loc->raiseobject == -1) {
        loc->raiseobject = loc->blockwalk ? 1 : 0;
    }
}

Model *loctype_get_model(LocType *loc, int shape, int rotation, int heightmapSW, int heightmapSE, int heightmapNE, int heightmapNW, int transformId) {
#ifdef __PS2__
    const bool ps2_watch_loc_684 = loc->index == 684 && shape == CENTREPIECE_STRAIGHT;
    // 2026-09-14: next boundary down the call chain from world_add_loc2()'s own entry checkpoint -
    // see that comment for the full context (PS2_LOC_DECODE_ONLY proved the bug lives inside
    // world_add_loc2()'s subtree). Switched from "first 5 calls only" to "every 20th call" - this
    // function runs for every loc placement in the scene (easily hundreds), so the old gate went
    // silent almost immediately and whatever text was last drawn just sat there frozen regardless of
    // whether the game kept working or genuinely hung much later - indistinguishable from a real hang
    // without this. Periodic sampling keeps visibility across the whole scene build.
    static int ps2_lgm_call_num = 0;
    ps2_lgm_call_num++;
    bool ps2_lgm_log = ps2_lgm_call_num <= 10 || (ps2_lgm_call_num % 4) == 1;
    if (ps2_lgm_log) {
        char ps2_lgm_msg[64];
        snprintf(ps2_lgm_msg, sizeof(ps2_lgm_msg), "loctype_get_model ENTER call#%d shape=%d index=%d", ps2_lgm_call_num, shape, loc->index);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg);
    }
#endif
    int shapeIndex = -1;
    for (int i = 0; i < loc->shapes_and_models_count; i++) {
        if (loc->shapes[i] == shape) {
            shapeIndex = i;
            break;
        }
    }
#ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, "loc 684 model: shape search done");
    }
#endif
#ifdef __PS2__
    // 2026-09-14: freeze narrowed to somewhere between this function's ENTER checkpoint and the
    // "calling model_from_id" one - this is the shape-search loop's exit point, bounded by
    // loc->shapes_and_models_count. If this never fires, the loop itself is stuck (a corrupted,
    // huge/negative-wrapped shapes_and_models_count would explain it).
    if (ps2_lgm_log) {
        char ps2_lgm_msg0[80];
        snprintf(ps2_lgm_msg0, sizeof(ps2_lgm_msg0), "call#%d shape search done shapeIndex=%d count=%d", ps2_lgm_call_num,
                 shapeIndex, loc->shapes_and_models_count);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg0);
    }
#endif

    if (shapeIndex == -1) {
        return NULL;
    }

    int64_t bitset = ((int64_t)loc->index << 6) + ((int64_t)shapeIndex << 3) + rotation + ((int64_t)(transformId + 1) << 32);
    if (_LocType.reset) {
        bitset = 0L;
    }

    Model *cached = (Model *)lrucache_get(_LocType.modelCacheDynamic, bitset);
#ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, cached ? "loc 684 model: dynamic cache hit" : "loc 684 model: dynamic cache miss");
    }
#endif
#ifdef __PS2__
    // Bounds the other prime suspect: an LRU-cache lookup that never returns (a corrupted internal
    // linked list with a cycle would explain a hang here with no crash).
    if (ps2_lgm_log) {
        char ps2_lgm_msg1[80];
        snprintf(ps2_lgm_msg1, sizeof(ps2_lgm_msg1), "call#%d dynamic cache lookup done cached=%s hillskew=%d",
                 ps2_lgm_call_num, cached ? "hit" : "miss", loc->hillskew);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg1);
    }
#endif
    if (cached) {
        if (_LocType.reset) {
            return cached;
        }

        if (loc->hillskew || loc->sharelight) {
            cached = model_copy_faces(cached, loc->hillskew, loc->sharelight, transformId == -1);
#ifdef __PS2__
            // model_copy_faces() can now genuinely return NULL under real arena exhaustion (see
            // model.c's OOM sweep) - without this, cached->vertex_count below would dereference NULL.
            if (!cached) {
                rs2_error("loctype_get_model: model_copy_faces returned NULL - out of memory\n");
                return NULL;
            }
#endif
        }

        if (loc->hillskew) {
            int groundY = (heightmapSW + heightmapSE + heightmapNE + heightmapNW) / 4;

            for (int i = 0; i < cached->vertex_count; i++) {
                int x = cached->vertices_x[i];
                int z = cached->vertices_z[i];

                int heightS = heightmapSW + (heightmapSE - heightmapSW) * (x + 64) / 128;
                int heightN = heightmapNW + (heightmapNE - heightmapNW) * (x + 64) / 128;
                int y = heightS + (heightN - heightS) * (z + 64) / 128;

                cached->vertices_y[i] += y - groundY;
            }

            model_calculate_bounds_y(cached);
        }

        return cached;
    }

    if (shapeIndex >= loc->shapes_and_models_count) {
        return NULL;
    }

    int modelId = loc->models[shapeIndex];
    if (modelId == -1) {
        return NULL;
    }

    bool flipped = loc->mirror ^ (rotation > 3);
    if (flipped) {
        modelId += 65536;
    }

    Model *model = (Model *)lrucache_get(_LocType.modelCacheStatic, modelId);
#ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, model ? "loc 684 model: static cache hit" : "loc 684 model: static cache miss");
    }
#endif
#ifdef __PS2__
    if (ps2_lgm_log) {
        char ps2_lgm_msg1b[80];
        snprintf(ps2_lgm_msg1b, sizeof(ps2_lgm_msg1b), "call#%d static cache lookup done model=%s modelId=%d",
                 ps2_lgm_call_num, model ? "hit" : "miss", modelId);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg1b);
    }
#endif
    if (!model) {
#ifdef __PS2__
        if (ps2_watch_loc_684) {
            ps2_scene_checkpoint(ps2_crash_client, "loc 684 model: before model_from_id");
        }
        if (ps2_lgm_log) {
            char ps2_lgm_msg2[64];
            snprintf(ps2_lgm_msg2, sizeof(ps2_lgm_msg2), "call#%d calling model_from_id(%d)", ps2_lgm_call_num,
                     modelId & 0xffff);
            ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg2);
        }
#endif
        model = model_from_id(modelId & 0xffff, true);
#ifdef __PS2__
        if (ps2_watch_loc_684) {
            ps2_scene_checkpoint(ps2_crash_client, "loc 684 model: model_from_id returned");
        }
        if (ps2_lgm_log) {
            char ps2_lgm_msg3[64];
            snprintf(ps2_lgm_msg3, sizeof(ps2_lgm_msg3), "call#%d model_from_id returned %s", ps2_lgm_call_num,
                     model ? "non-NULL" : "NULL");
            ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg3);
        }
#endif
        // model_from_id() can genuinely return NULL - not just for a missing ondemand.zip entry
        // (rare here; that path degrades to an empty-but-non-NULL Model when _Model.metadata is also
        // unset, as it is on PS2), but for a real allocation failure inside it (rs2_calloc/malloc) -
        // and this exact call site runs during scene construction, a point this project has directly
        // measured running with as little as a few hundred bytes of EE heap free (see the model/
        // interface unpacking memory log). Without this guard, model_rotate_y180(NULL) and
        // &model->link below would dereference a NULL pointer - a real, previously unhandled crash/
        // hang risk under the exact low-memory conditions this build is known to run close to.
        if (!model) {
            rs2_error("loctype_get_model: model_from_id(%d) returned NULL - out of memory or missing model\n",
                      modelId & 0xffff);
            return NULL;
        }
        if (flipped) {
            model_rotate_y180(model);
        }
        lrucache_put(_LocType.modelCacheStatic, modelId, &model->link);
    }

    bool scaled = loc->resizex != 128 || loc->resizey != 128 || loc->resizez != 128;
    bool translated = loc->offsetx != 0 || loc->offsety != 0 || loc->offsetz != 0;

    Model *modified = model_share_colored(model, !loc->recol_s, !loc->animHasAlpha, rotation == 0 && transformId == -1 && !scaled && !translated, true);
#ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, "loc 684 model: share_colored returned");
    }
#endif
#ifdef __PS2__
    // 2026-09-14: freeze narrowed to AFTER the static-cache-hit checkpoint (model_from_id() was
    // never even called - this was a cache hit) - so it's somewhere in this function's remaining
    // ~30 lines: model_share_colored() itself, the conditional transform/rotate/recolor/scale/
    // translate steps, or model_calculate_normals(). Bisecting with two checkpoints: this one right
    // after model_share_colored(), and one after model_calculate_normals() covering everything
    // between.
    if (ps2_lgm_log) {
        char ps2_lgm_msg4[112];
        snprintf(ps2_lgm_msg4, sizeof(ps2_lgm_msg4),
                 "call#%d share_colored done transformId=%d rotation=%d recol_count=%d scaled=%d translated=%d",
                 ps2_lgm_call_num, transformId, rotation, loc->recol_s ? loc->recol_count : 0, scaled, translated);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg4);
    }
    // model_share_colored() can now genuinely return NULL under real arena exhaustion (see model.c's
    // OOM sweep) - without this, every step below (transform/rotate/recolor/scale/translate,
    // model_calculate_normals()) would dereference NULL.
    if (!modified) {
        rs2_error("loctype_get_model: model_share_colored returned NULL - out of memory\n");
        return NULL;
    }
#endif
    if (transformId != -1) {
        model_create_label_references(modified, false);
        model_apply_transform(modified, transformId);
        model_free_label_references(modified);
        modified->label_faces = NULL;
        modified->label_vertices = NULL;
    }

    while (rotation-- > 0) {
        model_rotate_y90(modified);
    }
#ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, "loc 684 model: rotation done");
    }
#endif
#ifdef __PS2__
    // 2026-09-14: single, minimal addition - learning from last round's mistake of adding 4 at once.
    // This test's data has transformId=-1 (transform block skipped) and rotation=1 (this loop runs
    // exactly once, guaranteed) - model_rotate_y90() is a real, first-order suspect since it's the
    // only thing proven to actually execute between share_colored and this point. One checkpoint,
    // not several.
    if (ps2_lgm_log) {
        char ps2_lgm_msg_rot[48];
        snprintf(ps2_lgm_msg_rot, sizeof(ps2_lgm_msg_rot), "call#%d rotate loop done", ps2_lgm_call_num);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg_rot);
    }
#endif

    if (loc->recol_s) {
        for (int i = 0; i < loc->recol_count; i++) {
            model_recolor(modified, loc->recol_s[i], loc->recol_d[i]);
        }
    }

    if (scaled) {
        model_scale(modified, loc->resizex, loc->resizey, loc->resizez);
    }

    if (translated) {
        model_translate(modified, loc->offsety, loc->offsetx, loc->offsetz);
    }
#ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, "loc 684 model: transforms done");
    }
#endif
#ifdef __PS2__
    // 2026-09-14: "rotate loop done" fired (confirmed model_rotate_y90() completes), narrowing the
    // remaining candidates to model_scale()/model_translate() (both here) or model_calculate_normals()
    // below. One more single checkpoint, same minimal-addition discipline as last round.
    if (ps2_lgm_log) {
        char ps2_lgm_msg_st[48];
        snprintf(ps2_lgm_msg_st, sizeof(ps2_lgm_msg_st), "call#%d scale/translate done", ps2_lgm_call_num);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg_st);
    }
#endif

    // 2026-09-14: history for this function's checkpoints - 4 were once added here all at once
    // (after transform/rotate/recolor/scale-translate individually), which produced a real,
    // reproducible false-freeze (this project's third confirmed instance this session of stacked
    // ps2_scene_checkpoint() I/O itself becoming the cause of a freeze, not just a way to observe
    // one). rs2_log() has since been removed from ps2_scene_checkpoint() entirely (real, unconditional
    // USB/BDM I/O with zero confirmed value all session), and checkpoints are now added one at a
    // time, confirmed on hardware before adding the next - current density is deliberate, not an
    // oversight.
    model_calculate_normals(modified, loc->ambient + 64, loc->contrast * 5 + 768, -50, -10, -50, !loc->sharelight, true);
#ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, "loc 684 model: normals done");
    }
#endif
#ifdef __PS2__
    if (ps2_lgm_log) {
        char ps2_lgm_msg5[48];
        snprintf(ps2_lgm_msg5, sizeof(ps2_lgm_msg5), "call#%d calculate_normals done", ps2_lgm_call_num);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg5);
    }
#endif

    if (loc->blockwalk) {
        modified->obj_raise = modified->max_y;
    }

    lrucache_put(_LocType.modelCacheDynamic, bitset, &modified->link);
#ifdef __PS2__
    if (ps2_watch_loc_684) {
        ps2_scene_checkpoint(ps2_crash_client, "loc 684 model: dynamic cache put done");
    }
#endif

    if (loc->hillskew || loc->sharelight) {
        modified = model_copy_faces(modified, loc->hillskew, loc->sharelight, transformId == -1);
#ifdef __PS2__
        // 2026-09-14: second, previously-unguarded model_copy_faces() call site in this function (the
        // first, inside the dynamic-cache-hit branch above, was already guarded) - can genuinely return
        // NULL under real arena exhaustion, and the hillskew loop right below dereferences
        // modified->vertex_count unconditionally.
        if (!modified) {
            rs2_error("loctype_get_model: model_copy_faces returned NULL - out of memory\n");
            return NULL;
        }
#endif
    }

    if (loc->hillskew) {
        int groundY = (heightmapSW + heightmapSE + heightmapNE + heightmapNW) / 4;

        for (int i = 0; i < modified->vertex_count; i++) {
            int x = modified->vertices_x[i];
            int z = modified->vertices_z[i];

            int heightS = heightmapSW + (heightmapSE - heightmapSW) * (x + 64) / 128;
            int heightN = heightmapNW + (heightmapNE - heightmapNW) * (x + 64) / 128;
            int y = heightS + (heightN - heightS) * (z + 64) / 128;

            modified->vertices_y[i] += y - groundY;
        }

        model_calculate_bounds_y(modified);
    }
#ifdef __PS2__
    if (ps2_lgm_log) {
        char ps2_lgm_msg_done[48];
        snprintf(ps2_lgm_msg_done, sizeof(ps2_lgm_msg_done), "call#%d loctype_get_model done", ps2_lgm_call_num);
        ps2_scene_checkpoint(ps2_crash_client, ps2_lgm_msg_done);
    }
#endif
    return modified;
}
