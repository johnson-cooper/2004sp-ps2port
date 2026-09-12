#include <stdlib.h>

#include "jagfile.h"
#include "packet.h"
#include "platform.h"
#include "varbittype.h"

VarBitTypeData _VarBitType = {0};

static void varbittype_decode(VarBitType *varbit, Packet *dat);

static VarBitType *varbittype_new(void) {
    VarBitType *varbit = calloc(1, sizeof(VarBitType));
    varbit->basevar = -1;
    varbit->startbit = 0;
    varbit->endbit = 0;
    return varbit;
}

void varbittype_free_global(void) {
    for (int i = 0; i < _VarBitType.count; i++) {
        free(_VarBitType.instances[i]->debugname);
        free(_VarBitType.instances[i]);
    }
    free(_VarBitType.instances);
}

void varbittype_unpack(Jagfile *config) {
    // varbit.dat doesn't exist in pre-254 caches (VarBitType is new this revision) - degrade to
    // zero entries instead of crashing so older/incomplete caches still load everything else.
    if (!jagfile_has(config, "varbit.dat")) {
        _VarBitType.count = 0;
        return;
    }

    Packet *dat = jagfile_to_packet(config, "varbit.dat");
    _VarBitType.count = g2(dat);

    if (!_VarBitType.instances) {
        _VarBitType.instances = calloc(_VarBitType.count, sizeof(VarBitType *));
    }

    for (int id = 0; id < _VarBitType.count; id++) {
        if (!_VarBitType.instances[id]) {
            _VarBitType.instances[id] = varbittype_new();
        }

        varbittype_decode(_VarBitType.instances[id], dat);
    }

    packet_free(dat);
}

static void varbittype_decode(VarBitType *varbit, Packet *dat) {
    while (true) {
        int code = g1(dat);
        if (code == 0) {
            return;
        }

        if (code == 1) {
            varbit->basevar = g2(dat);
            varbit->startbit = g1(dat);
            varbit->endbit = g1(dat);
        } else if (code == 10) {
            varbit->debugname = gjstr(dat);
        } else {
            rs2_error("Error unrecognised varbit config code: %d\n", code);
        }
    }
}
