#pragma once

#include <stdbool.h>

#include "datastruct/linkable.h"
#include "seqtype.h"

typedef struct {
    Linkable link;
    int level;
    int type;
    int x;
    int z;
    int index;
    SeqType *seq;
    int seqFrame;
    int seqCycle;
#ifdef __PS2__
    // Set when pushLocs() computes a frame change but the per-frame rebuild budget is already spent -
    // keeps the pending model rebuild from being lost instead of silently skipped forever.
    bool pendingRebuild;
#endif
} LocEntity;

LocEntity *locentity_new(int index, int level, int type, int x, int z, SeqType *seq, bool randomFrame);
