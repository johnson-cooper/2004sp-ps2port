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
#include <stdlib.h>
#include "model.h"

static void ps2_player_create_label_references(Model *model, bool persistent_cache_model) {
    // Only the cached appearance call originally passes true. Its Model and raw label arrays are
    // heap-owned, and model_create_label_references() consumes those raw arrays by building the
    // animation lookup tables and then setting vertex_labels/face_labels to NULL. If we do not free
    // the original owners here, every appearance-cache miss permanently loses those allocations.
    //
    // Calls that originally pass false include temporary/shared spot-animation models. Their raw
    // label pointers may be shared with another model, so they must retain the old behavior.
    int *raw_vertex_labels = persistent_cache_model ? model->vertex_labels : NULL;
    int *raw_face_labels = persistent_cache_model ? model->face_labels : NULL;

    model_create_label_references(model, false);

    if (persistent_cache_model) {
        free(raw_vertex_labels);
        free(raw_face_labels);
    }
}

#define model_create_label_references(model, use_allocator) \
    ps2_player_create_label_references((model), (use_allocator))
#endif

#include "playerentity_impl.inc"

#ifdef __PS2__
#undef model_create_label_references
#endif
