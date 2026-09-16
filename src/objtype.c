// PS2 ground-item model isolation wrapper.
//
// The implementation lives in objtype_impl.inc so we can intercept only external
// calls to objtype_get_interfacemodel() without rewriting the historical object
// decoder/cache implementation. Calls made internally while building 2D inventory
// icons are macro-renamed to the original implementation and are unaffected.
#define objtype_get_interfacemodel objtype_get_interfacemodel_impl
#include "objtype_impl.inc"
#undef objtype_get_interfacemodel

Model *objtype_get_interfacemodel(ObjType *obj, int count, bool use_allocator) {
#ifdef __PS2__
    // Real-hardware 2x2-terrain failure stopped immediately after
    // UPDATE_ZONE_FULL_FOLLOWS / OBJ_ADD (159/159/120), with H=191 KiB at
    // tick entry while the previous completed frame had D/G ~= 1.5 MiB.
    // sortObjStacks() is the only external client caller that requests an
    // arena-backed object interface model. Suppress just that rendered ground
    // item model for this hardware bisection while retaining the logical
    // ObjStackEntity/list and GroundObject packet state. Internal inventory/icon
    // rendering still calls objtype_get_interfacemodel_impl() directly.
    if (use_allocator) {
        return NULL;
    }
#endif
    return objtype_get_interfacemodel_impl(obj, count, use_allocator);
}
