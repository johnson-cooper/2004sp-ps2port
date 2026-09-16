// PS2 ownership shim for cached player appearance models.
//
// The PS2 player appearance cache deliberately stores complete Model objects on the normal EE heap
// so individual LRU evictions can reclaim them. The old implementation still created that cached
// model's animation label-reference tables in the 6 MiB scene bump arena, then
// playerentity_cache_model()/playerentity_clear_model_cache() passed those arena pointers to free().
// With the three-entry PS2 appearance cache this happens frequently and corrupts libc's heap while
// also leaking bump-arena space until the next scene reset. Force label references created by this
// translation unit onto the same normal heap as the cached model. Existing temporary player/spotanim
// label references already pass false, so this only changes the one cached-model call that passed true.
#ifdef __PS2__
#include "model.h"
#define model_create_label_references(model, use_allocator) model_create_label_references((model), false)
#endif

#include "playerentity_impl.inc"

#ifdef __PS2__
#undef model_create_label_references
#endif
