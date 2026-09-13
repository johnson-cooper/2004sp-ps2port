#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "platform.h"

#ifdef __3DS__
#include <3ds.h>
#endif

typedef struct {
    int8_t *data;
    int capacity;
    int used;
#ifdef __PS2__
    // Diagnostic-only (see PHASE 7 audit notes): the scene bump arena has essentially zero safety
    // margin on PS2 (sized from observed overflow amounts, not a real budget - see the sizing
    // comment at bump_allocator_init()'s PS2 call site in entry/client.c) and callers don't tag
    // their allocations by subsystem, so a plain size histogram + running count is the cheapest way
    // to see WHAT is filling it without instrumenting ~150 individual call sites in model.c.
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
    return alloc.capacity;
}

bool bump_allocator_init(int capacity) {
#ifdef __3DS__
    // this large malloc fails on 3ds, so we use linearAlloc
    rs2_log("Free linear space: %d\n", linearSpaceFree());
    alloc.data = linearAlloc(capacity * sizeof(int8_t));
    rs2_log("Free linear space: %d\n", linearSpaceFree());
    if (alloc.data) {
        memset(alloc.data, 0, capacity);
    }
#else
    alloc.data = calloc(capacity, sizeof(int8_t));
#endif
    if (!alloc.data) {
        rs2_error("Failed to init allocator with size of: %d", capacity);
        return false;
    }
    alloc.capacity = capacity;
    alloc.used = 0;
    return true;
}

void bump_allocator_free(void) {
#ifdef __3DS__
    linearFree(alloc.data);
#else
    free(alloc.data);
#endif
}

void bump_allocator_reset(void) {
    // rs2_log("Allocator reset: clearing caches (size %d)", alloc.used);
#ifdef __PS2__
    // Log the completed cycle's peak usage BEFORE clearing it - this is the only place a
    // successful (non-overflowing) scene build's real high-water mark is visible; previously only
    // an actual overflow logged anything at all.
    if (alloc.alloc_count > 0) {
        rs2_log("Scene arena cycle complete: used=%d/%d, alloc_count=%d, largest_alloc=%d, "
                "histogram(<=32/<=128/<=512/<=2048/<=8192/>8192): %d %d %d %d %d %d\n",
                alloc.used, alloc.capacity, alloc.alloc_count, alloc.largest_alloc,
                alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                alloc.histogram[4], alloc.histogram[5]);
    }
#endif
    memset(alloc.data, 0, alloc.capacity);
    alloc.used = 0;
#ifdef __PS2__
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
#endif
}

void *rs2_malloc(bool use_allocator, int size) {
    // use_allocator = false;
    return use_allocator ? bump_alloc(size) : malloc(size);
}

void *rs2_calloc(bool use_allocator, int count, int size) {
    // use_allocator = false;
    return use_allocator ? bump_alloc(count * size) : calloc(count, size);
}

static void *bump_alloc(int size) {
#if __SIZEOF_POINTER__ == 4
    int aligned_ptr = alloc.used + 3 & ~3;
#else
    int aligned_ptr = alloc.used + 7 & ~7;
#endif
#ifdef __PS2__
    alloc.alloc_count++;
    if (size > alloc.largest_alloc) {
        alloc.largest_alloc = size;
    }
    int bucket = size <= 32 ? 0 : size <= 128 ? 1 : size <= 512 ? 2 : size <= 2048 ? 3 : size <= 8192 ? 4 : 5;
    alloc.histogram[bucket]++;
#endif
    if (aligned_ptr + size > alloc.capacity) {
#ifdef __PS2__
        rs2_error("Allocator full: this should never happen! attempted: %d, capacity: %d, alloc_count: %d, "
                  "largest_alloc: %d, histogram(<=32/<=128/<=512/<=2048/<=8192/>8192): %d %d %d %d %d %d",
                  alloc.used + size, alloc.capacity, alloc.alloc_count, alloc.largest_alloc,
                  alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                  alloc.histogram[4], alloc.histogram[5]);
#else
        rs2_error("Allocator full: this should never happen! attempted: %d, capacity: %d", alloc.used + size, alloc.capacity);
#endif
        exit(1);
    }

    void *next = alloc.data + aligned_ptr;
    alloc.used = aligned_ptr + size;
    return next;
}
