#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hashtable.h"
#include "linkable.h"
#ifdef __PS2__
// Only for the loop-guard below - ps2_crash_client/ps2_scene_checkpoint/rs2_error. client.h doesn't
// include hashtable.h (checked), no cycle.
#include "../client.h"
#endif

HashTable *hashtable_new(int size) {
    HashTable *table = calloc(1, sizeof(HashTable));
    if (!table || size <= 0) {
        return table;
    }
    table->buckets = calloc(size, sizeof(Linkable *));
    table->bucket_count = size;

#ifdef __PS2__
    // The original implementation performs one heap allocation for every sentinel bucket. Several
    // LRUs therefore produced thousands of tiny EE heap allocations before gameplay, wasting malloc
    // metadata and badly fragmenting 32 MiB RAM. Keep identical intrusive-list semantics but allocate
    // the sentinels as one contiguous block on PS2. buckets[0] remains the base pointer for cleanup.
    Linkable *sentinels = calloc(size, sizeof(Linkable));
    if (!table->buckets || !sentinels) {
        free(sentinels);
        free(table->buckets);
        free(table);
        return NULL;
    }
    for (int i = 0; i < size; i++) {
        Linkable *sentinel = table->buckets[i] = &sentinels[i];
        sentinel->next = sentinel;
        sentinel->prev = sentinel;
    }
#else
    for (int i = 0; i < size; i++) {
        Linkable *sentinel = table->buckets[i] = calloc(1, sizeof(Linkable));
        sentinel->next = sentinel;
        sentinel->prev = sentinel;
    }
#endif

    return table;
}

void hashtable_free(HashTable *table) {
    if (!table) {
        return;
    }
#ifdef __PS2__
    if (table->buckets && table->bucket_count > 0) {
        free(table->buckets[0]);
    }
#else
    for (int i = 0; i < table->bucket_count; i++) {
        free(table->buckets[i]);
    }
#endif
    free(table->buckets);
    free(table);
}

Linkable *hashtable_get(HashTable *table, int64_t key) {
    if (!table || !table->buckets || table->bucket_count <= 0) {
        return NULL;
    }
    Linkable *sentinel = table->buckets[(int)(key & (int64_t)(table->bucket_count - 1))];

#ifdef __PS2__
    // 2026-09-14: this function was heavily instrumented during a real-hardware bisection that
    // ultimately found no corruption anywhere in the hashtable/LRU data structures it touches. Keep
    // only a cheap permanent iteration guard against a genuinely corrupt intrusive list.
    int ps2_ht_iters = 0;
    static int ps2_ht_corrupt_draws = 0;
#endif
    for (Linkable *node = sentinel->next; node != sentinel; node = node->next) {
        if (node->key == key) {
            return node;
        }
#ifdef __PS2__
        if (++ps2_ht_iters > 2000) {
            char ps2_ht_msg[80];
            snprintf(ps2_ht_msg, sizeof(ps2_ht_msg), "hashtable_get: corrupt bucket iters=%d key32=%d",
                     ps2_ht_iters, (int)key);
            rs2_error("%s\n", ps2_ht_msg);
            if (ps2_ht_corrupt_draws < 5) {
                ps2_ht_corrupt_draws++;
                ps2_scene_checkpoint(ps2_crash_client, ps2_ht_msg);
            }
            return NULL;
        }
#endif
    }

    return NULL;
}

void hashtable_put(HashTable *table, int64_t key, Linkable *value) {
    if (!table || !table->buckets || table->bucket_count <= 0 || !value) {
        return;
    }
    if (value->prev) {
        linkable_unlink(value);
    }

    Linkable *sentinel = table->buckets[(int)(key & (int64_t)(table->bucket_count - 1))];
    value->prev = sentinel->prev;
    value->next = sentinel;
    value->prev->next = value;
    value->next->prev = value;
    value->key = key;
}
