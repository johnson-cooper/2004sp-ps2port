#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "platform.h"

#ifdef __3DS__
#include <3ds.h>
#endif

#ifdef __PS2__
#include <malloc.h>
#include <stdint.h>

// Declared directly (not via client.h) so this file - shared by every platform, not just PS2 -
// doesn't pull in the whole Client struct. See client.h's own declaration comment for the rationale.
extern void ps2_report_oom(const char *msg);

// Monotonic generation for the resettable scene arena. Any cache that keeps arena-backed pointers
// across scene rebuilds must key its validity to this value; bump_allocator_reset() invalidates every
// pointer handed out by the previous generation even though the backing 4 MiB block remains mapped.
unsigned int ps2_scene_arena_generation = 1;

// ReleasePlusPlus uses the gap between newlib's current program break and the live EE stack as a
// second, independent RAM signal. mallinfo().fordblks (our H/D/G overlay) only reports free blocks
// already owned by malloc; it does NOT include address space malloc can still acquire with sbrk().
// Keep a 512 KiB stack guard exactly like the reference implementation so this remains conservative.
extern void *sbrk(int incr);
static int ps2_ram_gap_bytes(void) {
    void *heap_end = sbrk(0);
    uintptr_t heap_ptr = (uintptr_t)heap_end;
    uintptr_t stack_local = (uintptr_t)&heap_end;
    uintptr_t stack_top = stack_local > heap_ptr ? stack_local : (uintptr_t)0x02000000u;
    const uintptr_t guard_bytes = 512u * 1024u;
    uintptr_t guarded_top = stack_top > guard_bytes ? stack_top - guard_bytes : stack_top;
    uintptr_t gap = guarded_top > heap_ptr ? guarded_top - heap_ptr : 0;
    return (int)gap;
}
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
#ifdef __PS2__
    // TEMPORARY HARDWARE TELEMETRY: custom.c renders this as the first value of "LRU: A / B".
    // For this bisection A is the guarded sbrk->stack gap, not scene-arena usage.
    return ps2_ram_gap_bytes();
#else
    return alloc.used;
#endif
}

int bump_allocator_capacity(void) {
#ifdef __PS2__
    // TEMPORARY HARDWARE TELEMETRY: the second "LRU" value is malloc-owned bytes. Combined with
    // H/D/G (fordblks), this distinguishes true EE-RAM exhaustion from a small malloc free-list.
    struct mallinfo info = mallinfo();
    return info.uordblks > 0 ? info.uordblks : 0;
#else
    return alloc.capacity;
#endif
}

bool bump_allocator_init(int capacity) {
#ifdef __PS2__
    // The zero-Ground scene build proved that normal-heap headroom, not terrain residency, is the
    // immediate world-entry constraint: reducing the requested 6 MiB arena to 5 MiB let hardware
    // enter Lumbridge, but libc then sat at only ~50-60 KiB free and failed around T2857. That means
    // the synchronous build/live scene is retaining substantially more normal-heap memory than the
    // temporary World arrays alone account for.
    //
    // Reserve 4 MiB. Hardware has proven this exact allocation layout can enter the world; changing
    // it again would contaminate the current 2x2-terrain memory diagnosis.
    if (capacity == (6 << 20)) {
        capacity = 4 << 20;
    }
#endif
#ifdef __3DS__
    // this large malloc fails on 3ds, so we use linearAlloc
    rs2_log("Free linear space: %d\n", linearSpaceFree());
    alloc.data = linearAlloc(capacity * sizeof(int8_t));
    rs2_log("Free linear space: %d\n", linearSpaceFree());
    if (alloc.data) {
        memset(alloc.data, 0, capacity);
    }
#elif defined(__PS2__)
    // The scene arena is a malloc/calloc replacement for arbitrary game structs and arrays. The
    // previous arena base came from calloc(), while bump_alloc() aligned each returned object to
    // only 4 bytes. That is weaker than the EE's native 128-bit/qword alignment and weaker than the
    // alignment callers can legitimately receive from the normal PS2 heap. Real hardware is much
    // less forgiving than PCSX2 when code or libraries issue aligned wider accesses. Keep this test
    // isolated to allocator alignment: same 4 MiB capacity, but a guaranteed qword-aligned base.
    alloc.data = memalign(16, capacity * sizeof(int8_t));
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
    // All arena-backed model/scene pointers from the previous scene are invalid from this point on.
    // Increment after the clear so generation-aware side caches can lazily invalidate on next use.
    ps2_scene_arena_generation++;
    if (ps2_scene_arena_generation == 0) {
        ps2_scene_arena_generation = 1;
    }
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
#ifdef __PS2__
    // PS2 EE is a 128-bit machine and several SDK/rendering paths operate naturally on qwords.
    // The old 4-byte bump alignment was not a valid malloc-equivalent alignment guarantee for every
    // arena-backed type. Keep every arena allocation qword aligned so adding one terrain allocation
    // cannot shift all later scene/model objects onto a hardware-hostile address.
    int aligned_ptr = (alloc.used + 15) & ~15;
#elif __SIZEOF_POINTER__ == 4
    int aligned_ptr = (alloc.used + 3) & ~3;
#else
    int aligned_ptr = (alloc.used + 7) & ~7;
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
                 aligned_ptr + size, alloc.capacity, alloc.alloc_count, alloc.largest_alloc,
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
        rs2_error("Allocator full: this should never happen! attempted: %d, capacity: %d", aligned_ptr + size, alloc.capacity);
        exit(1);
#endif
    }

    void *next = alloc.data + aligned_ptr;
    alloc.used = aligned_ptr + size;
    return next;
}
