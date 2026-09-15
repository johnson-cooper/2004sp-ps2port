#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "doublylinklist.h"
#include "hashtable.h"
#include "lrucache.h"

LruCache *lrucache_new(int size) {
    LruCache *cache = calloc(1, sizeof(LruCache));
    cache->capacity = size;
    cache->available = size;
#ifdef __PS2__
    // Desktop historically gives every LRU a 1024-bucket table. On a 32 MiB PS2 that is very
    // expensive because hashtable_new() allocates one sentinel Linkable per bucket as a separate
    // heap allocation. Most client caches are far smaller than 1024 entries (the PS2 player model
    // cache is only 12), so scale the bucket table to the cache while keeping a power-of-two size
    // required by hashtable_get()/put()'s key & (bucket_count - 1) indexing.
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
    return cache;
}

void lrucache_free(LruCache *cache) {
    hashtable_free(cache->hashtable);
    doublylinklist_free(cache->history);
    free(cache);
}

DoublyLinkable *lrucache_get(LruCache *cache, int64_t key) {
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
