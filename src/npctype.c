#include <stdio.h>
#include <stdlib.h>

#include "allocator.h"
#include "datastruct/doublylinklist.h"
#include "datastruct/linkable.h"
#include "datastruct/lrucache.h"
#include "jagfile.h"
#include "model.h"
#include "npctype.h"
#include "packet.h"
#include "platform.h"

NpcTypeData _NpcType = {0};

static void npctype_decode(NpcType *npc, Packet *dat);

#ifdef __PS2__
static void npctype_free_cached_model(Model *model) {
    if (!model) {
        return;
    }
    // NPC cache entries now have the same lifetime rule as PS2 player appearances:
    // every allocation owned by a persistent cached model is heap-backed and can be
    // reclaimed explicitly. Label-reference arrays are not owned by model_free().
    model_free_label_references(model);
    model_free(model);
}

static void npctype_clear_model_cache_ps2(void) {
    if (!_NpcType.modelCache) {
        return;
    }
    DoublyLinkable *node;
    while ((node = doublylinklist_pop(_NpcType.modelCache->history)) != NULL) {
        linkable_unlink(&node->link);
        npctype_free_cached_model((Model *)node);
    }
    _NpcType.modelCache->available = _NpcType.modelCache->capacity;
}

static void npctype_cache_model_ps2(int64_t key, Model *model) {
    LruCache *cache = _NpcType.modelCache;
    if (cache->available == 0) {
        DoublyLinkable *node = doublylinklist_pop(cache->history);
        if (node) {
            linkable_unlink(&node->link);
            npctype_free_cached_model((Model *)node);
            cache->available++;
        }
    }
    lrucache_put(cache, key, &model->link);
}
#endif

static void npctype_free(NpcType *npc) {
    if (!npc) return;
    free(npc->name);
    free(npc->desc);
    free(npc->models);
    free(npc->heads);
    free(npc->recol_s);
    free(npc->recol_d);
    if (npc->op) {
        for (int i = 0; i < 5; i++) free(npc->op[i]);
        free(npc->op);
    }
    free(npc);
}

static NpcType *npctype_new(void) {
    NpcType *npc = calloc(1, sizeof(NpcType));
    npc->index = -1L;
    npc->size = 1;
    npc->readyanim = -1;
    npc->walkanim = -1;
    npc->walkanim_b = -1;
    npc->walkanim_r = -1;
    npc->walkanim_l = -1;
    npc->animHasAlpha = false;
    npc->resizex = -1;
    npc->resizey = -1;
    npc->resizez = -1;
    npc->minimap = true;
    npc->vislevel = -1;
    npc->resizeh = 128;
    npc->resizev = 128;
    npc->alwaysontop = false;
    npc->ambient = 0;
    npc->contrast = 0;
    npc->headicon = -1;
    npc->turnspeed = 32; // TODO: verify rev254 default against real data
    return npc;
}

void npctype_unpack(Jagfile *config) {
    _NpcType.dat = jagfile_to_packet(config, "npc.dat");
    Packet *idx = jagfile_to_packet(config, "npc.idx");

    _NpcType.count = g2(idx);
    _NpcType.offsets = calloc(_NpcType.count, sizeof(int));

    int offset = 2;
    for (int id = 0; id < _NpcType.count; id++) {
        _NpcType.offsets[id] = offset;
        offset += g2(idx);
    }

    _NpcType.instances = calloc(_NpcType.count, sizeof(NpcType *));
    _NpcType.invalid = npctype_new();
#ifdef __PS2__
    // Persistent NPC meshes must remain heap-backed (the arena-lifetime fix), but the previous
    // 16-entry working set can consume nearly all remaining EE heap once a busy area has exposed
    // enough distinct NPC types. Hardware has now frozen with only ~50 KiB free, and the earlier
    // deferred-scene build also eventually died after a longer residency. Keep four hot NPC types;
    // ownership-aware eviction above immediately reclaims older meshes instead of letting the
    // persistent cache grow until there is no headroom for per-frame clones.
    _NpcType.modelCache = lrucache_new(4);
#else
    _NpcType.modelCache = lrucache_new(30);
#endif

    packet_free(idx);
}

void npctype_free_global(void) {
#ifdef __PS2__
    npctype_clear_model_cache_ps2();
#endif
    lrucache_free(_NpcType.modelCache);
    free(_NpcType.offsets);
    for (int i = 0; i < _NpcType.count; i++) {
        npctype_free(_NpcType.instances[i]);
    }
    free(_NpcType.instances);
    npctype_free(_NpcType.invalid);
    packet_free(_NpcType.dat);
}

NpcType *npctype_get(int id) {
    if (id < 0 || id >= _NpcType.count) {
        // rev254 servers can reference npc type ids beyond what's in Client3's loaded npc.idx
        // (offsets[] only has _NpcType.count entries) - indexing it out of bounds here set
        // _NpcType.dat->pos to garbage, and decoding from that garbage offset segfaulted during
        // NPC_INFO processing. Fall back to npctype_new()'s built-in defaults instead.
        rs2_error("npctype_get: npc type id %d out of range (max %d), using defaults\n", id, _NpcType.count - 1);
        return _NpcType.invalid;
    }
    if (_NpcType.instances[id]) {
        return _NpcType.instances[id];
    }
    NpcType *npc = npctype_new();
    npc->index = id;
    _NpcType.instances[id] = npc;
    _NpcType.dat->pos = _NpcType.offsets[id];
    npctype_decode(npc, _NpcType.dat);
    return npc;
}

static void npctype_decode(NpcType *npc, Packet *dat) {
    while (true) {
        int code = g1(dat);
        if (code == 0) {
            return;
        }

        if (code == 1) {
            int count = g1(dat);
            npc->models_count = count;
            npc->models = calloc(count, sizeof(int));

            for (int i = 0; i < count; i++) {
                npc->models[i] = g2(dat);
            }
        } else if (code == 2) {
            npc->name = gjstr(dat);
        } else if (code == 3) {
            npc->desc = gjstr(dat);
        } else if (code == 12) {
            npc->size = g1b(dat);
        } else if (code == 13) {
            npc->readyanim = g2(dat);
        } else if (code == 14) {
            npc->walkanim = g2(dat);
        } else if (code == 16) {
            npc->animHasAlpha = true;
        } else if (code == 17) {
            npc->walkanim = g2(dat);
            npc->walkanim_b = g2(dat);
            npc->walkanim_r = g2(dat);
            npc->walkanim_l = g2(dat);
        } else if (code >= 30 && code < 40) {
            if (!npc->op) {
                npc->op = calloc(5, sizeof(char *));
            }

            npc->op[code - 30] = gjstr(dat);
            if (platform_strcasecmp(npc->op[code - 30], "hidden") == 0) {
                free(npc->op[code - 30]);
                npc->op[code - 30] = NULL;
            }
        } else if (code == 40) {
            int count = g1(dat);
            npc->recol_count = count;
            npc->recol_s = calloc(count, sizeof(int));
            npc->recol_d = calloc(count, sizeof(int));

            for (int i = 0; i < count; i++) {
                npc->recol_s[i] = g2(dat);
                npc->recol_d[i] = g2(dat);
            }
        } else if (code == 60) {
            int count = g1(dat);
            npc->heads_count = count;
            npc->heads = calloc(count, sizeof(int));

            for (int i = 0; i < count; i++) {
                npc->heads[i] = g2(dat);
            }
        } else if (code == 90) {
            // unused
            npc->resizex = g2(dat);
        } else if (code == 91) {
            // unused
            npc->resizey = g2(dat);
        } else if (code == 92) {
            // unused
            npc->resizez = g2(dat);
        } else if (code == 93) {
            npc->minimap = false;
        } else if (code == 95) {
            npc->vislevel = g2(dat);
        } else if (code == 97) {
            npc->resizeh = g2(dat);
        } else if (code == 98) {
            npc->resizev = g2(dat);
        } else if (code == 99) {
            npc->alwaysontop = true;
        } else if (code == 100) {
            npc->ambient = g1b(dat);
        } else if (code == 101) {
            npc->contrast = g1b(dat) * 5;
        } else if (code == 102) {
            npc->headicon = g2(dat);
        } else if (code == 103) {
            npc->turnspeed = g2(dat);
        } else {
            // see objtype_decode()/loctype_decode() for the same fix - an opcode this decoder
            // doesn't recognise (rev254 npc.dat can contain some) would otherwise desync dat->pos,
            // and g1/g2/g1b do no bounds checking, so that desync can read arbitrarily far past the
            // buffer and crash much later, far from this site (confirmed: this was the real cause of
            // a sporadic crash whose location moved between world3d_draw and client_draw_minimap
            // depending on which garbage-decoded npc type got rendered first). Stop decoding instead.
            rs2_error("Error unrecognised npc config code: %d\n", code);
            return;
        }
    }
}

Model *npctype_get_sequencedmodel(NpcType *npc, int primaryTransformId, int secondaryTransformId, int *seqMask) {
    if (!npc || npc->models_count <= 0 || !npc->models) {
        return NULL;
    }

    Model *tmp = NULL;
    Model *model = (Model *)lrucache_get(_NpcType.modelCache, npc->index);

    if (!model) {
        Model **models = calloc(npc->models_count, sizeof(Model *));
        if (!models) {
            return NULL;
        }

        bool build_failed = false;
        for (int i = 0; i < npc->models_count; i++) {
#ifdef __PS2__
            // A cached NPC model outlives scene-arena rebuilds/resets. Decode every
            // component onto the normal heap so the cache never retains arena pointers.
            models[i] = model_from_id(npc->models[i], false);
#else
            models[i] = model_from_id(npc->models[i], npc->models_count == 1);
#endif
            if (!models[i]) {
                build_failed = true;
                break;
            }
        }

        if (build_failed) {
            for (int i = 0; i < npc->models_count; i++) {
                if (models[i]) {
                    model_free(models[i]);
                }
            }
            free(models);
            return NULL;
        }

        if (npc->models_count == 1) {
            model = models[0];
        } else {
#ifdef __PS2__
            model = model_from_models(models, npc->models_count, false);
#else
            model = model_from_models(models, npc->models_count, true);
#endif
            for (int i = 0; i < npc->models_count; i++) {
                model_free(models[i]);
            }
        }
        free(models);

        if (!model) {
            return NULL;
        }

        if (npc->recol_s) {
            for (int i = 0; i < npc->recol_count; i++) {
                model_recolor(model, npc->recol_s[i], npc->recol_d[i]);
            }
        }

#ifdef __PS2__
        model_create_label_references(model, false);
        model_calculate_normals(model, npc->ambient + 64, npc->contrast + 850, -30, -50, -30, true, false);
        npctype_cache_model_ps2(npc->index, model);
#else
        model_create_label_references(model, true);
        model_calculate_normals(model, npc->ambient + 64, npc->contrast + 850, -30, -50, -30, true, true);
        lrucache_put(_NpcType.modelCache, npc->index, &model->link);
#endif
    }

    tmp = model_share_alpha(model, !npc->animHasAlpha);
    if (!tmp) {
        return NULL;
    }

    if (primaryTransformId != -1 && secondaryTransformId != -1) {
        model_apply_transforms(tmp, primaryTransformId, secondaryTransformId, seqMask);
    } else if (primaryTransformId != -1) {
        model_apply_transform(tmp, primaryTransformId);
    }

    if (npc->resizeh != 128 || npc->resizev != 128) {
        model_scale(tmp, npc->resizeh, npc->resizev, npc->resizeh);
    }

    model_calculate_bounds_cylinder(tmp);
    tmp->label_faces = NULL;
    tmp->label_vertices = NULL;

    if (npc->size == 1) {
        tmp->pick_aabb = true;
    }

    return tmp;
}

Model *npctype_get_headmodel(NpcType *npc) {
    if (!npc->heads) {
        return NULL;
    }

    Model **models = calloc(npc->heads_count, sizeof(Model *));
    for (int i = 0; i < npc->heads_count; i++) {
        models[i] = model_from_id(npc->heads[i], false);
    }

    Model *model;
    if (npc->heads_count == 1) {
        model = models[0];
    } else {
        model = model_from_models(models, npc->heads_count, false);
        for (int i = 0; i < npc->heads_count; i++) {
            model_free(models[i]);
        }
    }
    free(models);

    if (npc->recol_s) {
        for (int i = 0; i < npc->recol_count; i++) {
            model_recolor(model, npc->recol_s[i], npc->recol_d[i]);
        }
    }

    return model;
}