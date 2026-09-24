#include <stdbool.h>
#include <stdint.h>

int bump_allocator_used(void);
int bump_allocator_capacity(void);
bool bump_allocator_init(int capacity);
void bump_allocator_free(void);
void bump_allocator_reset(void);
void *rs2_malloc(bool use_allocator, int size);
void *rs2_calloc(bool use_allocator, int count, int size);

#ifdef __PS2__
// Static-map loc streaming shares the existing scene arena instead of reserving more EE RAM.
// Permanent scene allocations grow upward; recyclable static-loc allocations grow downward.
void ps2_loc_allocator_begin(void);
void ps2_loc_allocator_end(void);
void ps2_loc_allocator_reset(void);
int ps2_loc_allocator_used(void);
int ps2_scene_allocator_used(void);
int ps2_allocator_gap(void);
bool ps2_loc_allocator_failed(void);
#endif
