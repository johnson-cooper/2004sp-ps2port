#pragma once

#include "jagfile.h"

// New in revision 254 - not present in Client3's revision-225 baseline.
// A VarBitType is a bit-sliced view into a backing VarpType's integer value:
// value = (varps[basevar] >> startbit) & ((1 << (endbit - startbit + 1)) - 1).
// TODO: only the config decode is implemented here. Reading/writing a varbit's
// live value (and any interface-condition evaluation that consumes it) is not
// yet wired up anywhere in Client3 - that requires auditing component.c's
// interface condition evaluator, which this pass did not cover.
typedef struct {
    int basevar;
    int startbit;
    int endbit;
    char *debugname;
} VarBitType;

typedef struct {
    int count;
    VarBitType **instances;
} VarBitTypeData;

void varbittype_unpack(Jagfile *config);
void varbittype_free_global(void);
