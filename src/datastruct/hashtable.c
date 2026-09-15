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
    table->buckets = calloc(size, sizeof(Linkable *));
    table->bucket_count = size;

    for (int i = 0; i < size; i++) {
        Linkable *sentinel = table->buckets[i] = calloc(1, sizeof(Linkable));
        sentinel->next = sentinel;
        sentinel->prev = sentinel;
    }

    return table;
}

void hashtable_free(HashTable *table) {
    for (int i = 0; i < table->bucket_count; i++) {
        free(table->buckets[i]);
    }
    free(table->buckets);
    free(table);
}

Linkable *hashtable_get(HashTable *table, int64_t key) {
    Linkable *sentinel = table->buckets[(int)(key & (int64_t)(table->bucket_count - 1))];

#ifdef __PS2__
    // 2026-09-14: MAJOR CORRECTION, after a long real-hardware bisection here - every field of every
    // node this function touches (sentinel->next/prev, node->key, node->next) was individually
    // confirmed to read back as a sane, in-range pointer/value on real hardware. There is no corrupted
    // bucket list. The apparent "hangs" during that bisection tracked almost exactly with how many
    // ps2_scene_checkpoint() draws (each a real GS-sync flip) were stacked in a tight sequence with no
    // real work between them - a false-freeze trap this project already had CONFIRMED precedent for
    // elsewhere (see loctype.c's history comment right before its lrucache_put() call, "third
    // confirmed instance this session"). All the diagnostic screen draws that lived in this function
    // have been removed now that they've served their purpose; only the iteration cap remains, as
    // cheap, permanent hardening against a real future infinite loop (still theoretically possible,
    // just not what was actually happening here).
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
