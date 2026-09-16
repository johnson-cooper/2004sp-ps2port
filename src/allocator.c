#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "platform.h"

#ifdef __3DS__
#include <3ds.h>
#endif

#ifdef __PS2__
extern void ps2_report_oom(const char *msg);

// PS2 compatibility-first arena policy:
// - keep the original single contiguous monotonic arena semantics;
// - do not reserve the 6 MiB ceiling during boot/title/network setup;
// - reserve it only when the first non-empty scene allocation is actually requested.
//
// The earlier segmented experiment saved headroom but changed a deep assumption of the old client:
// arena-backed model data had always lived inside one stable contiguous allocation. Real hardware
// regressed as early as the title/login transition, so segmentation is rejected until/unless every
// arena consumer is audited. Lazy reservation gives boot/login the memory back without changing the
// representation or lifetime of any live scene pointer.
static unsigned int ps2_zero_alloc_sentinel __attribute__((aligned(16)));
#endif

typedef struct {
    int8_t *data;
    int capacity;
    int used;
#ifdef __PS2__
    int alloc_count;
    int largest_alloc;
    int histogram[6];
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

#ifdef __PS2__
int bump_allocator_committed(void) {
    return alloc.data ? alloc.capacity : 0;
}

static bool ps2_bump_reserve(void) {
    if (alloc.data) {
        return true;
    }
    if (alloc.capacity <= 0) {
        return false;
    }

    alloc.data = calloc(alloc.capacity, sizeof(int8_t));
    if (!alloc.data) {
        int heap_kb = mallinfo().fordblks / 1024;
        char msg[80];
        snprintf(msg, sizeof(msg), "ARENA RESERVE OOM cap=%dK heap=%dK", alloc.capacity / 1024, heap_kb);
        rs2_error("PS2 scene arena reserve failed: cap=%d heapfree=%dKB\n", alloc.capacity, heap_kb);
        ps2_report_oom(msg);
        return false;
    }

    rs2_log("PS2 scene arena: lazily reserved contiguous %dKB, heapfree=%dKB\n",
            alloc.capacity / 1024, mallinfo().fordblks / 1024);
    return true;
}
#endif

bool bump_allocator_init(int capacity) {
#ifdef __PS2__
    // Reinitialization is rare, but do not leak an earlier arena if it happens.
    free(alloc.data);
    alloc.data = NULL;
    alloc.capacity = capacity;
    alloc.used = 0;
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
    rs2_log("PS2 scene arena: lazy contiguous max=%dKB (0KB committed at init)\n", capacity / 1024);
    return capacity > 0;
#elif defined(__3DS__)
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
#ifdef __3DS__
    linearFree(alloc.data);
#else
    free(alloc.data);
#endif
    alloc.data = NULL;
    alloc.used = 0;
#ifdef __PS2__
    alloc.capacity = 0;
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
#endif
}

void bump_allocator_reset(void) {
#ifdef __PS2__
    if (alloc.alloc_count > 0) {
        rs2_log("Scene arena cycle complete: used=%d/%d committed=%d, alloc_count=%d, largest_alloc=%d, "
                "histogram(<=32/<=128/<=512/<=2048/<=8192/>8192): %d %d %d %d %d %d\n",
                alloc.used, alloc.capacity, bump_allocator_committed(), alloc.alloc_count, alloc.largest_alloc,
                alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                alloc.histogram[4], alloc.histogram[5]);
    }
#endif

    // Preserve the original arena lifetime: reset rewinds it but does not free or move the backing
    // allocation. Old pointers remain address-valid during the teardown ordering used by the client.
    if (alloc.data && alloc.used > 0) {
        memset(alloc.data, 0, alloc.used);
    }
    alloc.used = 0;

#ifdef __PS2__
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
#endif
}

void *rs2_malloc(bool use_allocator, int size) {
    return use_allocator ? bump_alloc(size) : malloc(size);
}

void *rs2_calloc(bool use_allocator, int count, int size) {
    if (!use_allocator) {
        return calloc(count, size);
    }

#ifdef __PS2__
    if (count < 0 || size < 0 || (count != 0 && size > 0x7fffffff / count)) {
        rs2_error("Invalid scene calloc: count=%d size=%d\n", count, size);
        ps2_report_oom("BAD CALLOC - see boot.log");
        return NULL;
    }
#else
    if (count < 0 || size < 0 || (count != 0 && size > 0x7fffffff / count)) {
        return NULL;
    }
#endif

    int total = count * size;
    void *ptr = bump_alloc(total);
    if (ptr && total > 0) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static void *bump_alloc(int size) {
    if (size < 0) {
        return NULL;
    }

#ifdef __PS2__
    // Preserve a non-NULL result for legitimate empty arrays without forcing the 6 MiB reservation.
    if (size == 0) {
        if (alloc.data) {
            int aligned = (alloc.used + 3) & ~3;
            return alloc.data + aligned;
        }
        return &ps2_zero_alloc_sentinel;
    }

    if (!ps2_bump_reserve()) {
        return NULL;
    }

    alloc.alloc_count++;
    if (size > alloc.largest_alloc) {
        alloc.largest_alloc = size;
    }
    int bucket = size <= 32 ? 0 : size <= 128 ? 1 : size <= 512 ? 2 : size <= 2048 ? 3 : size <= 8192 ? 4 : 5;
    alloc.histogram[bucket]++;
#endif

#if __SIZEOF_POINTER__ == 4
    int aligned_ptr = (alloc.used + 3) & ~3;
#else
    int aligned_ptr = (alloc.used + 7) & ~7;
#endif

    if (aligned_ptr > alloc.capacity || size > alloc.capacity - aligned_ptr) {
#ifdef __PS2__
        int heap_kb = mallinfo().fordblks / 1024;
        rs2_error("Scene arena OOM: request=%d used=%d cap=%d heapfree=%dKB count=%d largest=%d\n",
                  size, alloc.used, alloc.capacity, heap_kb, alloc.alloc_count, alloc.largest_alloc);
        char msg[80];
        snprintf(msg, sizeof(msg), "ARENA OOM req=%d used=%dK heap=%dK", size, alloc.used / 1024, heap_kb);
        ps2_report_oom(msg);
        return NULL;
#else
        rs2_error("Allocator full: this should never happen! attempted: %d, capacity: %d", aligned_ptr + size, alloc.capacity);
        exit(1);
#endif
    }

    void *next = alloc.data + aligned_ptr;
    alloc.used = aligned_ptr + size;
    return next;
}
