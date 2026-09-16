#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "platform.h"

#ifdef __3DS__
#include <3ds.h>
#endif

#ifdef __PS2__
// Declared directly (not via client.h) so this file - shared by every platform, not just PS2 -
// doesn't pull in the whole Client struct. See client.h's own declaration comment for the rationale.
extern void ps2_report_oom(const char *msg);

// The EE only has 32 MiB.  Keep the scene arena's established 6 MiB logical ceiling, but commit it
// from libc only as the scene actually needs it.  Blocks never move while the allocator is alive,
// so Model/Location pointers remain stable.  IMPORTANT: bump_allocator_reset() historically only
// rewound the arena; it did NOT invalidate its backing storage.  Some world teardown paths reset the
// arena immediately before world3d_reset() finishes walking old scene structures, so committed blocks
// must remain valid across reset.  They are released only by bump_allocator_free()/re-init.
#define PS2_BUMP_BLOCK_BYTES (128 * 1024)
#define PS2_BUMP_GROW_ALIGN  (16 * 1024)
#define PS2_BUMP_MIN_FALLBACK (32 * 1024)
#define PS2_BUMP_MAX_BLOCKS 128

typedef struct {
    int8_t *data;
    int capacity;
    int used;
} Ps2BumpBlock;
#endif

typedef struct {
    int8_t *data;
    int capacity;
    int used;
#ifdef __PS2__
    int committed;
    int block_count;
    Ps2BumpBlock blocks[PS2_BUMP_MAX_BLOCKS];
    // Diagnostic-only: callers don't tag their allocations by subsystem, so a size histogram plus
    // running count is the cheapest way to see what is filling the scene arena on real hardware.
    int alloc_count;
    int largest_alloc;
    int histogram[6]; // buckets: <=32, <=128, <=512, <=2048, <=8192, >8192 bytes
#endif
} BumpAllocator;

static BumpAllocator alloc = {0};

static void *bump_alloc(int size);

int bump_allocator_used(void) {
    return alloc.used;
}

int bump_allocator_capacity(void) {
    // On PS2 this remains the logical ceiling, not the amount currently committed from libc.
    return alloc.capacity;
}

#ifdef __PS2__
int bump_allocator_committed(void) {
    return alloc.committed;
}

static void ps2_bump_release_blocks(void) {
    for (int i = 0; i < alloc.block_count; i++) {
        free(alloc.blocks[i].data);
        alloc.blocks[i] = (Ps2BumpBlock){0};
    }
    alloc.block_count = 0;
    alloc.committed = 0;
}

static void ps2_bump_rewind_blocks(void) {
    // Preserve every block address.  This matches the old contiguous allocator's reset semantics:
    // old pointers remain address-valid until the teardown that follows reset has completed, while
    // subsequent arena allocations may reuse the storage once the next scene starts building.
    for (int i = 0; i < alloc.block_count; i++) {
        alloc.blocks[i].used = 0;
    }
    alloc.used = 0;
}

static Ps2BumpBlock *ps2_bump_add_block(int minimum) {
    if (minimum <= 0 || alloc.block_count >= PS2_BUMP_MAX_BLOCKS) {
        return NULL;
    }

    int budget = alloc.capacity - alloc.committed;
    if (budget < minimum) {
        return NULL;
    }

    // 128 KiB is intentionally smaller than the first version's 256 KiB growth unit.  The PS2 heap
    // gets fragmented by models, packets, UI, and scene structures during a region build; requiring
    // a fresh contiguous quarter-megabyte late in loading is unnecessarily fragile.  Large single
    // requests still get a dedicated block rounded to 16 KiB.
    int block_capacity = PS2_BUMP_BLOCK_BYTES;
    if (block_capacity < minimum) {
        block_capacity = (minimum + PS2_BUMP_GROW_ALIGN - 1) & ~(PS2_BUMP_GROW_ALIGN - 1);
    }
    if (block_capacity > budget) {
        block_capacity = budget;
    }

    int8_t *data = malloc(block_capacity);

    // If normal growth cannot find a contiguous block, retry with the smallest useful allocation.
    // This preserves demand growth under fragmentation instead of reporting an arena OOM merely
    // because 128 KiB was unavailable as one run while a smaller block would have satisfied request.
    if (!data && block_capacity > minimum) {
        int fallback = (minimum + PS2_BUMP_GROW_ALIGN - 1) & ~(PS2_BUMP_GROW_ALIGN - 1);
        if (fallback < PS2_BUMP_MIN_FALLBACK) {
            fallback = PS2_BUMP_MIN_FALLBACK;
        }
        if (fallback > budget) {
            fallback = budget;
        }
        if (fallback >= minimum && fallback < block_capacity) {
            block_capacity = fallback;
            data = malloc(block_capacity);
        }
    }

    if (!data) {
        return NULL;
    }

    Ps2BumpBlock *block = &alloc.blocks[alloc.block_count++];
    block->data = data;
    block->capacity = block_capacity;
    block->used = 0;
    alloc.committed += block_capacity;
    return block;
}
#endif

bool bump_allocator_init(int capacity) {
#ifdef __PS2__
    // Demand-commit on PS2. `capacity` remains a hard upper bound (currently 6 MiB), but no scene
    // memory is removed from the general heap until an arena allocation actually needs it.
    ps2_bump_release_blocks();
    alloc.data = NULL;
    alloc.capacity = capacity;
    alloc.used = 0;
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
    rs2_log("PS2 scene arena: demand-committed, max=%dKB block=%dKB\n",
            capacity / 1024, PS2_BUMP_BLOCK_BYTES / 1024);
    return capacity > 0;
#elif defined(__3DS__)
    // this large malloc fails on 3ds, so we use linearAlloc
    rs2_log("Free linear space: %d\n", linearSpaceFree());
    alloc.data = linearAlloc(capacity * sizeof(int8_t));
    rs2_log("Free linear space: %d\n", linearSpaceFree());
    if (alloc.data) {
        memset(alloc.data, 0, capacity);
    }
    if (!alloc.data) {
        rs2_error("Failed to init allocator with size of: %d", capacity);
        return false;
    }
    alloc.capacity = capacity;
    alloc.used = 0;
    return true;
#else
    alloc.data = calloc(capacity, sizeof(int8_t));
    if (!alloc.data) {
        rs2_error("Failed to init allocator with size of: %d", capacity);
        return false;
    }
    alloc.capacity = capacity;
    alloc.used = 0;
    return true;
#endif
}

void bump_allocator_free(void) {
#ifdef __PS2__
    ps2_bump_release_blocks();
    alloc.used = 0;
    alloc.capacity = 0;
#elif defined(__3DS__)
    linearFree(alloc.data);
#else
    free(alloc.data);
#endif
}

void bump_allocator_reset(void) {
#ifdef __PS2__
    // Log the completed cycle's peak usage BEFORE rewinding it.  `committed` shows how much of the
    // logical 6 MiB maximum has actually been borrowed from ordinary libc heap.
    if (alloc.alloc_count > 0) {
        rs2_log("Scene arena cycle complete: used=%d/%d committed=%d, alloc_count=%d, largest_alloc=%d, "
                "histogram(<=32/<=128/<=512/<=2048/<=8192/>8192): %d %d %d %d %d %d\n",
                alloc.used, alloc.capacity, alloc.committed, alloc.alloc_count, alloc.largest_alloc,
                alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                alloc.histogram[4], alloc.histogram[5]);
    }

    // Do NOT free blocks here.  The previous segmented implementation did that and changed a subtle
    // but important lifetime property of the original allocator.  client_clear_caches() can invoke
    // this reset before world3d_reset() has finished traversing the old scene.  Rewinding preserves
    // pointer validity through teardown while still avoiding the old up-front 6 MiB reservation.
    ps2_bump_rewind_blocks();
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
#else
    // Only bytes handed out by the previous scene can contain stale state. rs2_calloc() explicitly
    // clears its own allocation, preserving normal calloc semantics.
    memset(alloc.data, 0, alloc.used);
    alloc.used = 0;
#endif
}

void *rs2_malloc(bool use_allocator, int size) {
    return use_allocator ? bump_alloc(size) : malloc(size);
}

void *rs2_calloc(bool use_allocator, int count, int size) {
    if (!use_allocator) {
        return calloc(count, size);
    }

    // Avoid integer overflow before handing a byte count to the arena.  Current callers are small,
    // but failing explicitly is far safer than wrapping into an undersized allocation on the EE.
    if (count <= 0 || size <= 0 || count > 0x7fffffff / size) {
#ifdef __PS2__
        ps2_report_oom("Invalid scene calloc size");
#endif
        return NULL;
    }

    void *ptr = bump_alloc(count * size);
    if (ptr) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

static void *bump_alloc(int size) {
    if (size <= 0) {
        return NULL;
    }

#ifdef __PS2__
    alloc.alloc_count++;
    if (size > alloc.largest_alloc) {
        alloc.largest_alloc = size;
    }
    int bucket = size <= 32 ? 0 : size <= 128 ? 1 : size <= 512 ? 2 : size <= 2048 ? 3 : size <= 8192 ? 4 : 5;
    alloc.histogram[bucket]++;

    // Search existing block tails before committing more heap.  This is still monotonic within one
    // scene cycle; addresses never move and no individual allocation is reclaimed early.
    for (int i = alloc.block_count - 1; i >= 0; i--) {
        Ps2BumpBlock *block = &alloc.blocks[i];
        int aligned = (block->used + 3) & ~3;
        if (aligned <= block->capacity && size <= block->capacity - aligned) {
            int consumed = aligned + size - block->used;
            void *next = block->data + aligned;
            block->used = aligned + size;
            alloc.used += consumed;
            return next;
        }
    }

    Ps2BumpBlock *block = ps2_bump_add_block(size);
    if (block) {
        block->used = size;
        alloc.used += size;
        return block->data;
    }

    char oom_msg[160];
    snprintf(oom_msg, sizeof(oom_msg),
             "Scene arena OOM: request=%d used=%d cap=%d committed=%d heapfree=%dKB blocks=%d count=%d",
             size, alloc.used, alloc.capacity, alloc.committed,
             mallinfo().fordblks / 1024, alloc.block_count, alloc.alloc_count);
    rs2_error("%s\n", oom_msg);
    ps2_report_oom(oom_msg);
    return NULL;
#else
#if __SIZEOF_POINTER__ == 4
    int aligned_ptr = (alloc.used + 3) & ~3;
#else
    int aligned_ptr = (alloc.used + 7) & ~7;
#endif
    if (aligned_ptr + size > alloc.capacity) {
        rs2_error("Allocator full: this should never happen! attempted: %d, capacity: %d", alloc.used + size, alloc.capacity);
        exit(1);
    }

    void *next = alloc.data + aligned_ptr;
    alloc.used = aligned_ptr + size;
    return next;
#endif
}
