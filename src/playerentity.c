// PS2 ownership shim for player appearance models.
//
// The PS2 player appearance cache deliberately stores complete Model objects on the normal EE heap
// so individual LRU evictions can reclaim them. The old implementation still created that cached
// model's animation label-reference tables in the 6 MiB scene bump arena, then
// playerentity_cache_model()/playerentity_clear_model_cache() passed those arena pointers to free().
// With the small PS2 appearance cache this can happen frequently and corrupt libc's heap while also
// leaking bump-arena space until the next scene reset. Force label references created by this
// translation unit onto the same normal heap as the cached model. Existing temporary player/spotanim
// label references already pass false, so this only changes the cached-model call that passed true.
//
// Previous hardware-isolation builds also forced PlayerEntity::lowmem and applied ps2_face_stride
// here. Those experiments did not move the repeatable ~T1320 failure and visibly damaged player
// meshes, so they are intentionally removed. Player animation and full face submission are restored;
// renderer safety belongs in model_draw2(), not in player-specific geometry destruction.
#ifdef __PS2__
// Include the declaration before installing the call-site rewrite below. playerentity_impl.inc
// reaches model.h through its normal header graph; if the function-like macro is already active at
// that point it rewrites the declaration itself into invalid C. model.h is #pragma once, so this
// early include makes the later transitive include a no-op while calls inside the implementation
// still get rewritten to use the normal heap.
#include "model.h"
#define model_create_label_references(model, use_allocator) model_create_label_references((model), false)
#endif

#include "playerentity_impl.inc"

#ifdef __PS2__
#undef model_create_label_references
#endif
