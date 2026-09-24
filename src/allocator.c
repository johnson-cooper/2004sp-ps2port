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
// doesn't pull in the whole Client struct.
extern void ps2_report_oom(const char *msg);

// Monotonic generation for the resettable scene arena. Any cache that keeps arena-backed pointers
// across scene rebuilds must key its validity to this value.
unsigned int ps2_scene_arena_generation = 1;

// mallinfo().fordblks only reports free blocks already owned by malloc. Keep a second conservative
// signal for the guarded gap between newlib's break and the live EE stack.
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
    int alloc_count;
    int largest_alloc;
    int histogram[6]; // <=32, <=128, <=512, <=2048, <=8192, >8192
#endif
} BumpAllocator;

static BumpAllocator alloc = {0};
#ifdef __PS2__
static void *bump_alloc(int size) __attribute__((section(".ps2_runtime_text"), noinline));
#else
static void *bump_alloc(int size);
#endif
#ifdef __PS2__
// The 6 MiB PS2 arena is shared from both ends. Permanent terrain/scene allocations grow upward
// through alloc.used. Recyclable static-loc models grow downward from ps2_loc_arena_top.
static bool ps2_loc_arena_active __attribute__((section(".ps2_runtime_data"))) = false;
static bool ps2_loc_arena_failed __attribute__((section(".ps2_runtime_data"))) = false;
static int ps2_loc_arena_top __attribute__((section(".ps2_runtime_data"))) = 0;
static int ps2_loc_alloc_count __attribute__((section(".ps2_runtime_data"))) = 0;
static int ps2_loc_largest_alloc __attribute__((section(".ps2_runtime_data"))) = 0;
#endif

int bump_allocator_used(void) {
#ifdef __PS2__
    // Temporary hardware telemetry: first value of the on-screen LRU pair.
    return ps2_ram_gap_bytes();
#else
    return alloc.used;
#endif
}

int bump_allocator_capacity(void) {
#ifdef __PS2__
    // Temporary hardware telemetry: second value of the on-screen LRU pair.
    struct mallinfo info = mallinfo();
    return info.uordblks > 0 ? info.uordblks : 0;
#else
    return alloc.capacity;
#endif
}

bool bump_allocator_init(int capacity) {
#ifdef __3DS__
    rs2_log("Free linear space: %d\n", linearSpaceFree());
    alloc.data = linearAlloc(capacity * sizeof(int8_t));
    rs2_log("Free linear space: %d\n", linearSpaceFree());
    if (alloc.data) {
        memset(alloc.data, 0, capacity);
    }
#elif defined(__PS2__)
    // EE scene allocations must remain qword aligned. Real hardware proved 4-byte bump alignment
    // was unsafe and made terrain layout changes trigger unrelated crashes. The PS2 now uses the
    // client's historical 6 MiB scene-arena request directly; dense Ardougne scenes exceed 5 MiB.
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
#ifdef __PS2__
    ps2_loc_arena_top = capacity;
    ps2_loc_arena_active = false;
    ps2_loc_arena_failed = false;
    ps2_loc_alloc_count = 0;
    ps2_loc_largest_alloc = 0;
#endif
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
#ifdef __PS2__
    if (alloc.alloc_count > 0) {
        rs2_log("Scene arena cycle complete: used=%d/%d, alloc_count=%d, largest_alloc=%d, "
                "histogram(<=32/<=128/<=512/<=2048/<=8192/>8192): %d %d %d %d %d %d\n",
                alloc.used, alloc.capacity, alloc.alloc_count, alloc.largest_alloc,
                alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                alloc.histogram[4], alloc.histogram[5]);
    }

    // IMPORTANT: do not erase the previous scene generation here.
    //
    // client_build_scene() currently calls client_clear_caches(), which reaches this reset, BEFORE
    // world3d_reset(). Resident Ground nodes are arena-backed, but they own pointers to normal-heap
    // Wall/Decor/GroundDecor/ground-object wrappers. Clearing the arena here zeroed those pointers
    // before ground_free() could see them, leaking every heap-backed static-world attachment on every
    // REBUILD_NORMAL. That only became visible once large loc windows were restored: several scene
    // transitions worked, then real hardware eventually crashed despite a still-reasonable H/D/G
    // free-list reading.
    //
    // Rewind the bump pointer without wiping the bytes. world3d_reset() runs immediately afterwards
    // and can still inspect the old Ground nodes long enough to release their heap-owned attachments.
    // New arena calloc users are explicitly zeroed by rs2_calloc(), so preserving old bytes until
    // they are overwritten does not weaken calloc semantics. rs2_malloc() retains normal malloc-style
    // uninitialised semantics. A future cleanup should reorder client_build_scene() so World3D teardown
    // happens before the reset; until then this keeps the lifetime ordering correct on hardware.
#else
    // Other platforms keep the established eager-clear behavior.
    memset(alloc.data, 0, alloc.used);
#endif

    alloc.used = 0;
#ifdef __PS2__
    // Full scene rebuild reclaims both ends at once. Do not erase bytes here: world3d_reset() still
    // needs the old arena-backed Location/Ground records long enough to free their heap wrappers.
    ps2_loc_arena_top = alloc.capacity;
    ps2_loc_arena_active = false;
    ps2_loc_arena_failed = false;
    ps2_loc_alloc_count = 0;
    ps2_loc_largest_alloc = 0;
    ps2_scene_arena_generation++;
    if (ps2_scene_arena_generation == 0) {
        ps2_scene_arena_generation = 1;
    }
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
#endif
}

#ifdef __PS2__
__attribute__((section(".ps2_runtime_text"), noinline))
void ps2_loc_allocator_begin(void) {
    ps2_loc_arena_active = true;
}

__attribute__((section(".ps2_runtime_text"), noinline))
void ps2_loc_allocator_end(void) {
    ps2_loc_arena_active = false;
}

__attribute__((section(".ps2_runtime_text"), noinline))
void ps2_loc_allocator_reset(void) {
    // Callers detach every streamed World3D wrapper and clear loc model side-caches before this.
    // Rewinding the high pointer then reclaims every static-loc model/Location allocation in O(1).
    ps2_loc_arena_active = false;
    ps2_loc_arena_top = alloc.capacity;
    ps2_loc_arena_failed = false;
    ps2_loc_alloc_count = 0;
    ps2_loc_largest_alloc = 0;
}

__attribute__((section(".ps2_runtime_text"), noinline))
int ps2_loc_allocator_used(void) {
    return alloc.capacity - ps2_loc_arena_top;
}

__attribute__((section(".ps2_runtime_text"), noinline))
int ps2_scene_allocator_used(void) {
    return alloc.used;
}

__attribute__((section(".ps2_runtime_text"), noinline))
int ps2_allocator_gap(void) {
    int gap = ps2_loc_arena_top - alloc.used;
    return gap > 0 ? gap : 0;
}

__attribute__((section(".ps2_runtime_text"), noinline))
bool ps2_loc_allocator_failed(void) {
    return ps2_loc_arena_failed;
}
#endif

void *rs2_malloc(bool use_allocator, int size) {
    return use_allocator ? bump_alloc(size) : malloc(size);
}

void *rs2_calloc(bool use_allocator, int count, int size) {
    if (!use_allocator) {
        return calloc(count, size);
    }

    // Preserve calloc semantics independently of arena reset behavior.
    void *ptr = bump_alloc(count * size);
    if (ptr) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

static void *bump_alloc(int size) {
#ifdef __PS2__
    if (size < 0) {
        return NULL;
    }

    if (ps2_loc_arena_active) {
        // Grow the recyclable loc arena downward. Align the resulting address, not merely the size,
        // so every returned EE pointer retains the hardware-proven qword alignment.
        int next_top = (ps2_loc_arena_top - size) & ~15;
        ps2_loc_alloc_count++;
        if (size > ps2_loc_largest_alloc) {
            ps2_loc_largest_alloc = size;
        }

        if (next_top < alloc.used) {
            // This is a normal streaming budget boundary, not a fatal EE-memory error. The caller
            // stops admitting lower-priority scenery and keeps the already-built structural set.
            ps2_loc_arena_failed = true;
            return NULL;
        }

        ps2_loc_arena_top = next_top;
        return alloc.data + next_top;
    }

    int aligned_ptr = (alloc.used + 15) & ~15;
    alloc.alloc_count++;
    if (size > alloc.largest_alloc) {
        alloc.largest_alloc = size;
    }
    int bucket = size <= 32 ? 0 : size <= 128 ? 1 : size <= 512 ? 2 : size <= 2048 ? 3 : size <= 8192 ? 4 : 5;
    alloc.histogram[bucket]++;

    // Permanent scene data may grow only up to the current bottom of the loc arena.
    if (aligned_ptr + size > ps2_loc_arena_top) {
        char oom_msg[112];
        snprintf(oom_msg, sizeof(oom_msg),
                 "Allocator full: attempted=%d loc_top=%d cap=%d count=%d largest=%d",
                 aligned_ptr + size, ps2_loc_arena_top, alloc.capacity, alloc.alloc_count, alloc.largest_alloc);
        rs2_error("%s\n", oom_msg);
        ps2_report_oom(oom_msg);
        return NULL;
    }

    void *next = alloc.data + aligned_ptr;
    alloc.used = aligned_ptr + size;
    return next;
#else
#if __SIZEOF_POINTER__ == 4
    int aligned_ptr = (alloc.used + 3) & ~3;
#else
    int aligned_ptr = (alloc.used + 7) & ~7;
#endif

    if (aligned_ptr + size > alloc.capacity) {
        rs2_error("Allocator full: this should never happen! attempted: %d, capacity: %d", aligned_ptr + size, alloc.capacity);
        exit(1);
    }

    void *next = alloc.data + aligned_ptr;
    alloc.used = aligned_ptr + size;
    return next;
#endif
}
