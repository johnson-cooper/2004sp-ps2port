#include <stdbool.h>
#include <stdint.h>

int bump_allocator_used(void);
int bump_allocator_capacity(void);
int bump_allocator_scene_used(void);
int bump_allocator_scene_capacity(void);
int bump_allocator_scene_remaining(void);
#ifdef __PS2__
int ps2_heap_headroom_bytes(void);
#endif
bool bump_allocator_init(int capacity);
void bump_allocator_free(void);
void bump_allocator_reset(void);
void *rs2_malloc(bool use_allocator, int size);
void *rs2_calloc(bool use_allocator, int count, int size);
