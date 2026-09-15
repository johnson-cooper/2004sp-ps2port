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

// The EE only has 32 MiB and the old PS2 bump allocator reserved its entire 6 MiB ceiling in one
// calloc at startup.  That guaranteed scene space, but permanently hid every unused arena byte from
// ordinary malloc users.  Real-hardware telemetry reached only a few dozen KiB of libc heap while
// the arena still had unused capacity.  Commit the same logical arena in moderately-sized stable
// blocks instead.  Blocks NEVER move while a scene is alive, so every raw Model/Location pointer
// keeps exactly the same lifetime/semantics as the old contiguous bump arena.  All blocks are
// returned to libc together at bump_allocator_reset(), which is already the scene/cache lifetime
// boundary.  This is deliberately not a general reallocating/compacting allocator.
#define PS2_BUMP_BLOCK_BYTES (256 * 1024)
#define PS2_BUMP_GROW_ALIGN  (64 * 1024)
#define PS2_BUMP_MAX_BLOCKS 32

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
    // Diagnostic-only (see PHASE 7 audit notes): callers don't tag their allocations by subsystem,
    // so a size histogram + running count is the cheapest way to see WHAT is filling the scene arena.
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

static Ps2BumpBlock *ps2_bump_add_block(int minimum) {
    if (alloc.block_count >= PS2_BUMP_MAX_BLOCKS) {
        return NULL;
    }

    // Normal growth is 256 KiB.  Oversized single allocations get a block rounded to 64 KiB so
    // they do not waste an entire extra 256 KiB step.  The final block may use the exact remaining
    // logical arena budget.
    int block_capacity = PS2_BUMP_BLOCK_BYTES;
    if (block_capacity < minimum) {
        block_capacity = (minimum + PS2_BUMP_GROW_ALIGN - 1) & ~(PS2_BUMP_GROW_ALIGN - 1);
    }

    int budget = alloc.capacity - alloc.committed;
    if (block_capacity > budget) {
        block_capacity = budget;
    }
    if (block_capacity < minimum || block_capacity <= 0) {
        return NULL;
    }

    int8_t *data = malloc(block_capacity);
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
    // Demand-commit on PS2.  `capacity` is still a hard upper bound (currently 6 MiB), but zero of
    // it is removed from the general heap until the first scene allocation actually needs it.
    // This lets UI/network/transient game state use RAM that the scene has not committed yet.
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
    // rs2_log("Allocator reset: clearing caches (size %d)", alloc.used);
#ifdef __PS2__
    // Log the completed cycle's peak usage BEFORE releasing it.  `committed` is important on PS2:
    // it tells us how much of the 6 MiB ceiling was actually borrowed from the ordinary heap.
    if (alloc.alloc_count > 0) {
        rs2_log("Scene arena cycle complete: used=%d/%d committed=%d, alloc_count=%d, largest_alloc=%d, "
                "histogram(<=32/<=128/<=512/<=2048/<=8192/>8192): %d %d %d %d %d %d\n",
                alloc.used, alloc.capacity, alloc.committed, alloc.alloc_count, alloc.largest_alloc,
                alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                alloc.histogram[4], alloc.histogram[5]);
    }

    // Scene/cache reset is the only point at which every bump-backed pointer is dead.  Returning all
    // committed blocks here is therefore safe and makes the arena dynamic in BOTH directions: it can
    // grow toward 6 MiB during a dense scene, then hand every byte back when that scene is discarded.
    ps2_bump_release_blocks();
    alloc.used = 0;
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
#else
    // The arena is monotonic and starts calloc-zeroed. Only bytes handed out by
    // the previous scene can contain stale state; clearing the unused tail was
    // a needless large write on every rebuild. rs2_calloc() still explicitly
    // clears its own allocation, preserving its normal calloc contract.
    memset(alloc.data, 0, alloc.used);
    alloc.used = 0;
#endif
}

void *rs2_malloc(bool use_allocator, int size) {
    // use_allocator = false;
    return use_allocator ? bump_alloc(size) : malloc(size);
}

void *rs2_calloc(bool use_allocator, int count, int size) {
    // use_allocator = false;
    if (!use_allocator) {
        return calloc(count, size);
    }
    // Explicitly preserve calloc semantics.  This is required for the PS2 segmented arena because
    // newly malloc'd blocks are intentionally not zero-filled wholesale.
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

    // Reuse any tail that still fits before committing another block.  This is still a monotonic
    // allocator: an individual block's used pointer only moves forward until the whole scene resets.
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

    char oom_msg[128];
    snprintf(oom_msg, sizeof(oom_msg),
             "Scene arena OOM: request=%d used=%d cap=%d committed=%d heapfree=%dKB count=%d",
             size, alloc.used, alloc.capacity, alloc.committed,
             mallinfo().fordblks / 1024, alloc.alloc_count);
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
