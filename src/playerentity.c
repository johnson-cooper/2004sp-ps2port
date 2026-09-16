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
// Include the declarations before installing the call-site rewrites below. playerentity_impl.inc
// reaches model.h through its normal header graph; if the function-like macros are already active at
// that point they rewrite the declarations themselves into invalid C. model.h is #pragma once, so
// this early include makes the later transitive include a no-op while calls inside the implementation
// still get rewritten to use the PS2 ownership helpers.
#include <stdlib.h>
#include "model.h"

static Model *ps2_player_pending_cache_model = NULL;

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
        // The cached-model build immediately lights this same model next. Remember exactly that
        // pointer so the normals wrapper can distinguish it from temporary spot-animation models,
        // whose ownership must remain unchanged.
        ps2_player_pending_cache_model = model;
    }
}

static void ps2_player_calculate_normals(Model *model, int light_ambient, int light_attenuation,
                                         int lightsrc_x, int lightsrc_y, int lightsrc_z,
                                         bool apply_lighting, bool use_allocator) {
    bool persistent_cache_model = model == ps2_player_pending_cache_model;
    int *raw_face_colors = persistent_cache_model ? model->face_colors : NULL;

    model_calculate_normals(model, light_ambient, light_attenuation, lightsrc_x, lightsrc_y,
                            lightsrc_z, apply_lighting, use_allocator);

    if (persistent_cache_model) {
        // model_apply_lighting() deliberately NULLs face_colors once a fully-lit, untextured model
        // no longer needs its original HSL colors. On the heap-backed PS2 appearance cache that
        // otherwise loses the only owner and leaks face_count*sizeof(int) on every such cache miss.
        // Textured models retain face_colors, so leave those owned by model_free() as before.
        if (!model->face_colors) {
            free(raw_face_colors);
        }
        ps2_player_pending_cache_model = NULL;
    }
}

#define model_create_label_references(model, use_allocator) \
    ps2_player_create_label_references((model), (use_allocator))
#define model_calculate_normals(model, light_ambient, light_attenuation, lightsrc_x, lightsrc_y, lightsrc_z, apply_lighting, use_allocator) \
    ps2_player_calculate_normals((model), (light_ambient), (light_attenuation), (lightsrc_x), (lightsrc_y), (lightsrc_z), (apply_lighting), (use_allocator))
#endif

#include "playerentity_impl.inc"

#ifdef __PS2__
#undef model_calculate_normals
#undef model_create_label_references
#endif
