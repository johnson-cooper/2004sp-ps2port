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
    // The arena is monotonic and starts calloc-zeroed. Only bytes handed out by
    // the previous scene can contain stale state; clearing the unused tail was
    // a needless 6 MiB write on every rebuild. rs2_calloc() still explicitly
    // clears its own allocation, preserving its normal calloc contract.
    memset(alloc.data, 0, alloc.used);
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
    if (!use_allocator) {
        return calloc(count, size);
    }
    // 2026-09-14: patch-plan item 2 from the systemic PS2 audit - bump_alloc() only returns zeroed
    // memory today because bump_allocator_reset() memsets the whole arena once per scene and nothing
    // in between ever reuses a byte range (monotonic allocator), so every "new" allocation happens to
    // start zeroed in practice. That's an invariant of the current call sites, not a guarantee the
    // function itself provides - any future caller, or any future change to the reset/eviction
    // discipline (see lrucache_clear() not reclaiming arena bytes - client.c's client_clear_caches()),
    // could silently break it. Explicit here instead, matching real calloc semantics unconditionally.
    void *ptr = bump_alloc(count * size);
    if (ptr) {
        memset(ptr, 0, count * size);
    }
    return ptr;
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
        char oom_msg[112];
        snprintf(oom_msg, sizeof(oom_msg),
                 "Allocator full: attempted=%d cap=%d count=%d largest=%d hist=%d/%d/%d/%d/%d/%d",
                 alloc.used + size, alloc.capacity, alloc.alloc_count, alloc.largest_alloc,
                 alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                 alloc.histogram[4], alloc.histogram[5]);
        rs2_error("%s\n", oom_msg);
        // 2026-09-14: rs2_error() only ever reaches a log file, never the screen, and
        // ps2_scene_checkpoint() (used by every other OOM site added this pass) is currently a global
        // no-op via PS2_CHECKPOINTS_ENABLED - so without this, the single most direct evidence for
        // "ran out of arena" (vs. every other freeze/crash shape) would be invisible on real hardware.
        // This is the one choke point every bump-backed allocation failure passes through, so wiring
        // it here covers every current and future caller for free. See ps2_report_oom()'s own comment
        // (platform/ps2.c) for why it deliberately bypasses the same master switch.
        ps2_report_oom(oom_msg);
        // 2026-09-14: exit(1) below was the ACTUAL root cause of this entire session's real-hardware
        // hangs, found by direct code read after real-hardware evidence (heap_free staying healthy at
        // every checkpoint, no crash message ever visible, every previous test bisection running out
        // of checkpoints with no error text - see model.c's rs2_calloc/rs2_malloc NULL-checks added
        // earlier this session, which were unreachable dead code because of this). There is no OS on
        // bare-metal PS2 for exit() to hand control back to - it almost certainly just locks up the
        // EE, and rs2_error() only writes to a log file, never the screen, so an overflow here
        // produced zero visible change: exactly the "frozen on the last checkpoint, nothing else ever
        // happens" symptom reported every single time. Returning NULL instead lets callers that
        // already check for it (rs2_malloc/rs2_calloc's NULL-checked call sites) fail cleanly and
        // visibly instead of silently locking up. Every other platform keeps exit(1) unchanged - this
        // is a real, working process-exit there.
        return NULL;
#else
        rs2_error("Allocator full: this should never happen! attempted: %d, capacity: %d", alloc.used + size, alloc.capacity);
        exit(1);
#endif
    }

    void *next = alloc.data + aligned_ptr;
    alloc.used = aligned_ptr + size;
    return next;
}
