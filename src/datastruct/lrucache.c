#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "doublylinklist.h"
#include "hashtable.h"
#include "lrucache.h"

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
        // comfortably handles eight linked entries. This tests whether the long-run heap collapse
        // was caused by the old 3-entry cache continuously evicting/rebuilding nearby appearances,
        // without perturbing the fragile pre-world allocation layout.
        cache->capacity = 8;
        cache->available = 8;
    }
#endif
    return cache;
}

void lrucache_free(LruCache *cache) {
    hashtable_free(cache->hashtable);
    doublylinklist_free(cache->history);
    free(cache);
}

DoublyLinkable *lrucache_get(LruCache *cache, int64_t key) {
#ifdef __PS2__
    // objtype.c is the only 50-entry LRU user. Its cached interface/ground-item models are built
    // with use_allocator=true, so the model and its arrays live in the resettable scene bump arena.
    // Keeping those pointers in a process-lifetime LRU can hand a later OBJ_ADD/icon request a model
    // whose arena storage has already been reset/reused. For this hardware bisection, leave the
    // constructor/allocation layout completely unchanged but make that one cache non-resident.
    // Callers still build and receive the model normally; only cross-call reuse is disabled.
    if (cache && cache->capacity == 50) {
        return NULL;
    }
#endif
    // 2026-09-14: this function was heavily instrumented during a real-hardware bisection that
    // ultimately found no corruption anywhere in the hashtable/LRU data structures it touches (see
    // hashtable.c's hashtable_get() history comment) - the apparent "hangs" tracked with how many
    // ps2_scene_checkpoint() draws were stacked in a tight sequence, a false-freeze trap this project
    // has separate confirmed precedent for (loctype.c, right before its lrucache_put() call). All
    // diagnostic checkpoints removed now that they've served their purpose; loctype_get_model()'s own
    // "dynamic cache lookup done" checkpoint (right after its lrucache_get() call) already shows
    // whether this function completes, with far less overhead than duplicating that here.
    DoublyLinkable *node = (DoublyLinkable *)hashtable_get(cache->hashtable, key);
    if (node) {
        doublylinklist_push(cache->history, node);
    }

    return node;
}

void lrucache_put(LruCache *cache, int64_t key, DoublyLinkable *value) {
#ifdef __PS2__
    // Pair with the 50-entry get() bypass above. The arena-backed model remains owned by the current
    // scene/caller and is reclaimed by the arena reset; do not publish its pointer into a longer-lived
    // cache. This intentionally changes no allocation made by lrucache_new(), which matters because
    // real-hardware world entry has proven sensitive to startup heap layout.
    if (cache && cache->capacity == 50) {
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
