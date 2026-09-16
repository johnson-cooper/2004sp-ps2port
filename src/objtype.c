// PS2 ground-item draw isolation wrapper.
//
// Keep the historical implementation in objtype_impl.inc so we can let the
// ground-item path perform its normal model lookup/build/cache work, while
// intercepting only the model pointer returned to the scene. Calls made inside
// the historical implementation (for example inventory/icon work) are renamed
// to the implementation directly and remain unchanged.
#define objtype_get_interfacemodel objtype_get_interfacemodel_impl
#include "objtype_impl.inc"
#undef objtype_get_interfacemodel

Model *objtype_get_interfacemodel(ObjType *obj, int count, bool use_allocator) {
#ifdef __PS2__
    if (use_allocator) {
        // Hardware just froze at T167 with L159/159/120 and H=203 KiB while
        // the previous completed draw/present still had about 1.5 MiB free.
        // Ground-stack callers are the external use_allocator=true path.
        //
        // IMPORTANT: unlike the earlier 631d240 isolation, DO build/cache the
        // requested model here. Returning NULL only prevents that model pointer
        // from being attached to/drawn by the GroundObject. The logical object
        // stack, GroundObject allocation, model construction and model-cache
        // residency all still happen. This isolates ground-item rasterization
        // from the sudden zone-update memory collapse with one variable.
        (void)objtype_get_interfacemodel_impl(obj, count, use_allocator);
        return NULL;
    }
#endif
    return objtype_get_interfacemodel_impl(obj, count, use_allocator);
}
