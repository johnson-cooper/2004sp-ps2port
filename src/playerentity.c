// PS2 ownership and renderer-safety shim for player models.
//
// The PS2 player appearance cache deliberately stores complete Model objects on the normal EE heap
// so individual LRU evictions can reclaim them. The old implementation still created that cached
// model's animation label-reference tables in the 6 MiB scene bump arena, then
// playerentity_cache_model()/playerentity_clear_model_cache() passed those arena pointers to free().
// With the three-entry PS2 appearance cache this happens frequently and corrupts libc's heap while
// also leaking bump-arena space until the next scene reset. Force label references created by this
// translation unit onto the same normal heap as the cached model. Existing temporary player/spotanim
// label references already pass false, so this only changes the one cached-model call that passed true.
//
// There is also a separate fixed-buffer hazard in the software model renderer: each depth bucket has
// MODEL_DEPTH_FACE_COUNT entries, but model_draw2() currently appends faces without checking that
// capacity. A combined player can contain thousands of faces, so a dense pose can write past one of
// those buckets even while mallinfo() still reports free RAM. Keep this hardware iteration isolated
// to players by renaming the implementation entry point below and applying a minimum face stride to
// its final returned model. That guarantees a player submits at most MODEL_DEPTH_FACE_COUNT faces in
// total, so no individual depth bucket can overflow. This also cuts EE classification/raster work.
// Once the character-only hardware test is stable, the general production fix should add bounds at
// the insertion sites in model_draw2() before relaxing this conservative player LOD.
#ifdef __PS2__
#include "model.h"
#define model_create_label_references(model, use_allocator) model_create_label_references((model), false)
#define playerentity_draw playerentity_draw_impl
#endif

#include "playerentity_impl.inc"

#ifdef __PS2__
#undef playerentity_draw
#undef model_create_label_references

Model *playerentity_draw(PlayerEntity *entity, int loopCycle) {
    // Hardware test after the bounded-face run survived from roughly T394 to T1324 but still
    // eventually froze.  The remaining hot player path allocates a temporary Model plus three
    // vertex arrays in model_share_alpha() every draw, transforms it, then frees it after drawing.
    // At ~1 MiB free heap that repeated malloc/free churn can fragment the EE heap, and
    // model_share_alpha() currently has no allocation-failure checks.  PlayerEntity::lowmem already
    // has the exact ownership semantics we want for this isolation: draw the long-lived cached
    // appearance model directly, skip per-frame animation/spotanim copies, and do not free it in
    // entity_draw_free(). Keep the renderer face bound from the preceding test so this changes only
    // the temporary-player allocation path for the next real-hardware run.
    entity->lowmem = true;

    Model *model = playerentity_draw_impl(entity, loopCycle);
    if (!model) {
        return NULL;
    }

    int min_stride = 1;
    if (model->face_count > MODEL_DEPTH_FACE_COUNT) {
        min_stride = (model->face_count + MODEL_DEPTH_FACE_COUNT - 1) / MODEL_DEPTH_FACE_COUNT;
    }
    if (model->ps2_face_stride < min_stride) {
        model->ps2_face_stride = min_stride;
    }
    return model;
}
#endif
