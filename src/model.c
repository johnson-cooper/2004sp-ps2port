// Keep the original renderer implementation in model_impl.inc so PS2 can tune
// the fixed scratch bucket geometry without changing desktop/other platforms.
// The PS2 previously used 600 depth buckets x 80 faces = 48,000 face slots.
// Camera rotation can concentrate many coplanar/model faces into one depth bin,
// so 80 entries is dangerously small and model_draw2 has no per-bin capacity
// guard. Rebalance the SAME 48,000 slots as 240 x 200: no extra EE RAM, fewer
// empty depth scans, and 2.5x more headroom per active depth bin. Faces beyond
// the reduced depth range are already discarded by the renderer's existing
// MODEL_MAX_DEPTH check, which is preferable to corrupting adjacent memory.
#ifdef __PS2__
#include "defines.h"
#undef MODEL_MAX_DEPTH
#undef MODEL_DEPTH_FACE_COUNT
#define MODEL_MAX_DEPTH 240
#define MODEL_DEPTH_FACE_COUNT 200
#endif

#include "model_impl.inc"
