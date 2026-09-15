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

// The PS2 has only 32 MiB of EE RAM. Reserving the scene allocator's full logical
// 6 MiB at boot permanently removed that memory from the normal heap even when a
// live scene used only a fraction of it. Commit the arena in stable, non-moving
// chunks instead. Pointers returned from earlier chunks never move, while reset()
// can return every committed chunk to the heap once the scene caches are cleared.
#define PS2_BUMP_CHUNK_SIZE (256 * 1024)
typedef struct Ps2BumpChunk {
    int8_t *data;
    int capacity;
    int used;
    struct Ps2BumpChunk *next;
} Ps2BumpChunk;
#endif

typedef struct {
#ifndef __PS2__
    int8_t *data;
#else
    Ps2BumpChunk *chunks;
    Ps2BumpChunk *current;
    int committed;
#endif
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

#ifdef __PS2__
static void ps2_bump_free_chunks(void) {
    Ps2BumpChunk *chunk = alloc.chunks;
    while (chunk) {
        Ps2BumpChunk *next = chunk->next;
        free(chunk->data);
        free(chunk);
        chunk = next;
    }
    alloc.chunks = NULL;
    alloc.current = NULL;
    alloc.committed = 0;
}

static bool ps2_bump_add_chunk(int minimum) {
    int chunk_size = PS2_BUMP_CHUNK_SIZE;
    if (minimum > chunk_size) {
        chunk_size = (minimum + PS2_BUMP_CHUNK_SIZE - 1) & ~(PS2_BUMP_CHUNK_SIZE - 1);
    }
    // Never commit beyond the logical arena ceiling. The final chunk may be
    // smaller than 256 KiB when close to the cap.
    int remaining = alloc.capacity - alloc.committed;
    if (remaining <= 0) {
        return false;
    }
    if (chunk_size > remaining) {
        chunk_size = remaining;
    }
    if (chunk_size < minimum) {
        return false;
    }

    Ps2BumpChunk *chunk = calloc(1, sizeof(Ps2BumpChunk));
    if (!chunk) {
        return false;
    }
    chunk->data = malloc(chunk_size);
    if (!chunk->data) {
        free(chunk);
        return false;
    }
    chunk->capacity = chunk_size;

    if (alloc.current) {
        alloc.current->next = chunk;
    } else {
        alloc.chunks = chunk;
    }
    alloc.current = chunk;
    alloc.committed += chunk_size;
    return true;
}
#endif

int bump_allocator_used(void) {
    return alloc.used;
}

int bump_allocator_capacity(void) {
    return alloc.capacity;
}

bool bump_allocator_init(int capacity) {
#ifdef __3DS__
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
#elif defined(__PS2__)
    // Logical capacity only. Physical EE memory is committed by bump_alloc().
    // This intentionally starts at zero committed bytes so ordinary gameplay
    // gets the maximum possible heap headroom.
    ps2_bump_free_chunks();
    alloc.capacity = capacity;
    alloc.used = 0;
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
    rs2_log("PS2 scene arena: logical=%dKB committed=0KB (lazy)\n", capacity / 1024);
    return true;
#else
    alloc.data = calloc(capacity, sizeof(int8_t));
    if (!alloc.data) {
        rs2_error("Failed to init allocator with size of: %d", capacity);
        return false;
    }
#endif
    alloc.capacity = capacity;
    alloc.used = 0;
    return true;
}

void bump_allocator_free(void) {
#ifdef __3DS__
    linearFree(alloc.data);
#elif defined(__PS2__)
    ps2_bump_free_chunks();
#else
    free(alloc.data);
#endif
    alloc.used = 0;
    alloc.capacity = 0;
}

void bump_allocator_reset(void) {
#ifdef __PS2__
    if (alloc.alloc_count > 0) {
        rs2_log("Scene arena cycle complete: used=%d/%d committed=%d, alloc_count=%d, largest_alloc=%d, "
                "histogram(<=32/<=128/<=512/<=2048/<=8192/>8192): %d %d %d %d %d %d\n",
                alloc.used, alloc.capacity, alloc.committed, alloc.alloc_count, alloc.largest_alloc,
                alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                alloc.histogram[4], alloc.histogram[5]);
    }
    // client_clear_caches() runs before this reset, so no arena-backed model is
    // allowed to survive the cycle. Return the committed scene pages to the EE
    // heap instead of holding a permanent 6 MiB reservation between rebuilds.
    ps2_bump_free_chunks();
    alloc.used = 0;
    alloc.alloc_count = 0;
    alloc.largest_alloc = 0;
    memset(alloc.histogram, 0, sizeof(alloc.histogram));
#else
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
    if (count <= 0 || size <= 0 || count > 0x7fffffff / size) {
        return NULL;
    }
    int bytes = count * size;
    void *ptr = bump_alloc(bytes);
    if (ptr) {
        memset(ptr, 0, bytes);
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

    int aligned = alloc.current ? ((alloc.current->used + 3) & ~3) : 0;
    int delta = alloc.current ? (aligned - alloc.current->used + size) : size;
    if (alloc.used + delta > alloc.capacity) {
        char oom_msg[112];
        snprintf(oom_msg, sizeof(oom_msg),
                 "Allocator full: attempted=%d cap=%d count=%d largest=%d hist=%d/%d/%d/%d/%d/%d",
                 alloc.used + delta, alloc.capacity, alloc.alloc_count, alloc.largest_alloc,
                 alloc.histogram[0], alloc.histogram[1], alloc.histogram[2], alloc.histogram[3],
                 alloc.histogram[4], alloc.histogram[5]);
        rs2_error("%s\n", oom_msg);
        ps2_report_oom(oom_msg);
        return NULL;
    }

    if (!alloc.current || aligned + size > alloc.current->capacity) {
        // A new chunk starts aligned. Count only the requested payload against
        // logical usage; physical committed bytes are tracked separately.
        if (!ps2_bump_add_chunk(size)) {
            char oom_msg[112];
            snprintf(oom_msg, sizeof(oom_msg),
                     "EE heap full committing scene chunk: used=%d cap=%d committed=%d request=%d",
                     alloc.used, alloc.capacity, alloc.committed, size);
            rs2_error("%s\n", oom_msg);
            ps2_report_oom(oom_msg);
            return NULL;
        }
        aligned = 0;
        delta = size;
    }

    void *next = alloc.current->data + aligned;
    alloc.current->used = aligned + size;
    alloc.used += delta;
    return next;
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
