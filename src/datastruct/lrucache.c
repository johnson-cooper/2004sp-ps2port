#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "doublylinklist.h"
#include "hashtable.h"
#include "lrucache.h"

#ifdef __PS2__
// objtype.c is the sole 50-entry LRU user in the current client. Its model values can be allocated
// from the resettable scene bump arena, so linking the Model's embedded DoublyLinkable into the
// process-lifetime generic LRU is unsafe on real hardware. Keep the exact lrucache_new(50)
// allocation/layout, but store candidate {key,pointer} pairs in a fixed BSS-backed sidecar that never
// touches the Model's embedded links or allocates heap memory.
#define PS2_OBJ_SCENE_CACHE_CAPACITY 50

typedef struct {
    int64_t key;
    DoublyLinkable *value;
    bool valid;
} Ps2ObjSceneCacheEntry;

static LruCache *ps2_obj_scene_cache_owner = NULL;
static Ps2ObjSceneCacheEntry ps2_obj_scene_cache[PS2_OBJ_SCENE_CACHE_CAPACITY];
static int ps2_obj_scene_cache_replace = 0;

static void ps2_obj_scene_cache_reset(LruCache *owner) {
    for (int i = 0; i < PS2_OBJ_SCENE_CACHE_CAPACITY; i++) {
        ps2_obj_scene_cache[i].key = 0;
        ps2_obj_scene_cache[i].value = NULL;
        ps2_obj_scene_cache[i].valid = false;
    }
    ps2_obj_scene_cache_replace = 0;
    ps2_obj_scene_cache_owner = owner;
}

static bool ps2_is_obj_model_cache(LruCache *cache) {
    return cache && cache->capacity == PS2_OBJ_SCENE_CACHE_CAPACITY;
}
#endif

LruCache *lrucache_new(int size) {
#ifdef __PS2__
    // playerentity.c is the sole 12-entry LRU user in the current client. Preserve the exact
    // three-entry constructor/allocation shape that is proven to pass real-hardware world entry:
    // in particular this keeps the same 16-bucket hashtable and startup heap layout. After those
    // allocations are complete we expand only the logical residency below. A three-model working
    // set thrashes as soon as the local player plus >=3 other appearances are visible, rebuilding
    // heap-backed meshes over and over instead of reaching a steady state.
    bool ps2_player_appearance_cache = size == 12;
    if (ps2_player_appearance_cache) {
        size = 3;
    }
#endif

    LruCache *cache = calloc(1, sizeof(LruCache));
    cache->capacity = size;
    cache->available = size;
#ifdef __PS2__
    // Desktop historically gives every LRU a 1024-bucket table. On a 32 MiB PS2 that is very
    // expensive because hashtable_new() allocates one sentinel Linkable per bucket as a separate
    // heap allocation. Most client caches are far smaller than 1024 entries, so scale the bucket
    // table to the cache while keeping a power-of-two size required by hashtable_get()/put()'s
    // key & (bucket_count - 1) indexing.
    int buckets = 16;
    int target = size > 0 ? size * 2 : 16;
    while (buckets < target && buckets < 256) {
        buckets <<= 1;
    }
    cache->hashtable = hashtable_new(buckets);
#else
    cache->hashtable = hashtable_new(1024);
#endif
    cache->history = doublylinklist_new();
#ifdef __PS2__
    if (ps2_player_appearance_cache) {
        // Eight slots cover the local player plus the seven other players seen in the hardware
        // Lumbridge traces. No extra allocation is performed here: the existing 16-bucket table
        // comfortably handles eight linked entries.
        cache->capacity = 8;
        cache->available = 8;
    }
#endif
    return cache;
}

void lrucache_free(LruCache *cache) {
#ifdef __PS2__
    if (ps2_obj_scene_cache_owner == cache) {
        ps2_obj_scene_cache_reset(NULL);
    }
#endif
    hashtable_free(cache->hashtable);
    doublylinklist_free(cache->history);
    free(cache);
}

DoublyLinkable *lrucache_get(LruCache *cache, int64_t key) {
#ifdef __PS2__
    if (ps2_is_obj_model_cache(cache)) {
        if (ps2_obj_scene_cache_owner != cache) {
            ps2_obj_scene_cache_reset(cache);
        }
        // c407a568 failed during the initial S1 region load with L91/91/91. The only behavioral
        // difference from the T10226-proven bypass was that callers could now receive a previously
        // built arena-backed Model from this sidecar. Keep recording candidates below, but force a
        // miss here. This preserves the non-intrusive storage/clear path while isolating MODEL REUSE
        // itself. If hardware enters Lumbridge again, a cached Model is not safe to reuse with the
        // current arena lifetime/build semantics even when the cache structure itself is non-intrusive.
        (void)key;
        return NULL;
    }
#endif
    // 2026-09-14: this function was heavily instrumented during a real-hardware bisection that
    // ultimately found no corruption anywhere in the hashtable/LRU data structures it touches.
    DoublyLinkable *node = (DoublyLinkable *)hashtable_get(cache->hashtable, key);
    if (node) {
        doublylinklist_push(cache->history, node);
    }

    return node;
}

void lrucache_put(LruCache *cache, int64_t key, DoublyLinkable *value) {
#ifdef __PS2__
    if (ps2_is_obj_model_cache(cache)) {
        if (ps2_obj_scene_cache_owner != cache) {
            ps2_obj_scene_cache_reset(cache);
        }

        // Record the same candidate pointers as c407a568, but lrucache_get() deliberately does not
        // return them in this hardware test. No intrusive Model links are touched.
        for (int i = 0; i < PS2_OBJ_SCENE_CACHE_CAPACITY; i++) {
            if (ps2_obj_scene_cache[i].valid && ps2_obj_scene_cache[i].key == key) {
                ps2_obj_scene_cache[i].value = value;
                return;
            }
        }

        for (int i = 0; i < PS2_OBJ_SCENE_CACHE_CAPACITY; i++) {
            if (!ps2_obj_scene_cache[i].valid) {
                ps2_obj_scene_cache[i].key = key;
                ps2_obj_scene_cache[i].value = value;
                ps2_obj_scene_cache[i].valid = true;
                return;
            }
        }

        int slot = ps2_obj_scene_cache_replace++ % PS2_OBJ_SCENE_CACHE_CAPACITY;
        ps2_obj_scene_cache[slot].key = key;
        ps2_obj_scene_cache[slot].value = value;
        ps2_obj_scene_cache[slot].valid = true;
        return;
    }
#endif
    if (cache->available == 0) {
        DoublyLinkable *node = doublylinklist_pop(cache->history);
        linkable_unlink(&node->link);
        doublylinkable_uncache(node);
    } else {
        cache->available--;
    }
    hashtable_put(cache->hashtable, key, &value->link);
    doublylinklist_push(cache->history, value);
}

void lrucache_clear(LruCache *cache) {
#ifdef __PS2__
    if (ps2_is_obj_model_cache(cache)) {
        ps2_obj_scene_cache_reset(cache);
        cache->available = cache->capacity;
        return;
    }
#endif
    while (true) {
        DoublyLinkable *node = doublylinklist_pop(cache->history);
        if (!node) {
            cache->available = cache->capacity;
            return;
        }

        linkable_unlink(&node->link);
        doublylinkable_uncache(node);
    }
}
