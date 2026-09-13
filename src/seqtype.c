#include <limits.h>
#include <stdlib.h>

#include "animframe.h"
#include "jagfile.h"
#include "packet.h"
#include "platform.h"
#include "seqtype.h"

SeqTypeData _SeqType = {0};

static void seqtype_decode(SeqType *seq, Packet *dat);

// Matches the reference client's SeqType.getDuration() exactly (2004sp-client's SeqType.ts): a
// per-frame delay of 0 means "use this frame's own embedded delay instead of a per-sequence
// override" - resolved here, lazily, at playback time via animframe_get() (which is exactly when
// this same frame is about to be decoded for rendering anyway, so this doesn't force any decode
// that wasn't about to happen regardless - unlike trying to resolve this at seq-decode time,
// which runs at boot before any real animation frame has been lazily loaded yet, see the removed
// code this replaced in seqtype_decode() above).
int seqtype_get_duration(SeqType *seq, int frame) {
    if (!seq->delay || !seq->frames) {
        return 0;
    }

    // The reference (JS) has no equivalent check here - an out-of-bounds array read in JS just
    // silently returns undefined, which its own comparisons (undefined > n, etc.) treat as false
    // with no crash. C has no such safety net: a real, previously-latent bug is that at least one
    // caller (startForceMovement(), entry/client.c) calls this without first checking
    // `frame < seq->frameCount` - harmless before this function existed, since the only thing read
    // out of bounds was seq->delay[frame] (usually landing on plausible-looking adjacent heap
    // data), but this function ALSO reads seq->frames[frame] - a completely separate calloc()
    // allocation - and an out-of-bounds read there can land on genuinely unmapped memory. Confirmed
    // via a real boot: TLB Miss faults plus cascading packet-desync errors immediately after this
    // function started being called more broadly. Bounds-check defensively here, once, rather than
    // auditing/fixing every call site individually. Return value matters: every real caller only
    // ever uses this in a `cycle > duration`-shaped comparison (a subtraction of the same value
    // only ever follows a comparison that already proved `frame` in-bounds for that same
    // iteration), so INT_MAX - guaranteed to make that comparison false - is the faithful match
    // for JS's real "cycle > undefined is always false" behavior on an out-of-bounds access,
    // rather than guessing at a plausible-but-arbitrary small duration.
    if (frame < 0 || frame >= seq->frameCount) {
        return INT_MAX;
    }

    int duration = seq->delay[frame];

    if (duration == 0) {
        AnimFrame *transform = animframe_get(seq->frames[frame]);
        if (transform) {
            duration = seq->delay[frame] = transform->delay;
        }
    }

    if (duration == 0) {
        duration = 1;
    }

    return duration;
}

static SeqType *seqtype_new(void) {
    SeqType *seq = calloc(1, sizeof(SeqType));
    seq->replayoff = -1;
    seq->priority = 5;
    seq->righthand = -1;
    seq->lefthand = -1;
    seq->replaycount = 99;
    seq->preanim_move = -1;
    seq->postanim_move = -1;
    seq->duplicatebehaviour = 0;
    return seq;
}

void seqtype_free_global(void) {
    for (int i = 0; i < _SeqType.count; i++) {
        free(_SeqType.instances[i]->frames);
        free(_SeqType.instances[i]->iframes);
        free(_SeqType.instances[i]->delay);
        free(_SeqType.instances[i]->walkmerge);
        free(_SeqType.instances[i]);
    }
    free(_SeqType.instances);
}

void seqtype_unpack(Jagfile *config) {
    Packet *dat = jagfile_to_packet(config, "seq.dat");
    _SeqType.count = g2(dat);

    if (!_SeqType.instances) {
        _SeqType.instances = calloc(_SeqType.count, sizeof(SeqType *));
    }

    for (int id = 0; id < _SeqType.count; id++) {
        if (!_SeqType.instances[id]) {
            _SeqType.instances[id] = seqtype_new();
        }

        seqtype_decode(_SeqType.instances[id], dat);
    }

    packet_free(dat);
}

static void seqtype_decode(SeqType *seq, Packet *dat) {
    while (true) {
        int code = g1(dat);
        if (code == 0) {
            break;
        }

        if (code == 1) {
            seq->frameCount = g1(dat);
            seq->frames = calloc(seq->frameCount, sizeof(int));
            seq->iframes = calloc(seq->frameCount, sizeof(int));
            seq->delay = calloc(seq->frameCount, sizeof(int));

            for (int i = 0; i < seq->frameCount; i++) {
                seq->frames[i] = g2(dat);

                seq->iframes[i] = g2(dat);
                if (seq->iframes[i] == 65535) {
                    seq->iframes[i] = -1;
                }

                // A real, previously-live bug lived here: this used to try resolving a 0 (see
                // seqtype_get_duration() below for what 0 actually means) via a raw
                // _AnimFrame.instances[] check, then hardcode it to 1 if that didn't find
                // anything - but this decode runs at boot, before ANY real gameplay animation
                // frame has been lazily decoded (see animframe.c's lazy-loading note), so the
                // check essentially never found anything and every 0-delay frame got permanently
                // hardcoded to the fastest possible speed (1 tick/frame) the instant it was
                // decoded, for the rest of the session. That's a strict superset of "some
                // animations play too fast" - it's every animation whose real per-frame timing
                // comes from the frame's own embedded delay rather than a per-sequence override
                // (confirmed against the reference client's SeqType.decode(), which just stores
                // the raw value here with no resolution attempt at all - see getDuration() below,
                // which is where the reference actually resolves this, lazily, at playback time).
                seq->delay[i] = g2(dat);
            }
        } else if (code == 2) {
            seq->replayoff = g2(dat);
        } else if (code == 3) {
            int count = g1(dat);
            seq->walkmerge = calloc(count + 1, sizeof(int));

            for (int i = 0; i < count; i++) {
                seq->walkmerge[i] = g1(dat);
            }

            seq->walkmerge[count] = 9999999;
        } else if (code == 4) {
            seq->stretches = true;
        } else if (code == 5) {
            seq->priority = g1(dat);
        } else if (code == 6) {
            // later RS (think RS3) seq->becomes mainhand
            seq->righthand = g2(dat);
        } else if (code == 7) {
            // later RS (think RS3) seq->becomes offhand
            seq->lefthand = g2(dat);
        } else if (code == 8) {
            seq->replaycount = g1(dat);
        } else if (code == 9) {
            seq->preanim_move = g1(dat);
        } else if (code == 10) {
            seq->postanim_move = g1(dat);
        } else if (code == 11) {
            seq->duplicatebehaviour = g1(dat);
        } else {
            rs2_error("Error unrecognised seq config code: %d\n", code);
        }
    }

    if (seq->frameCount == 0) {
        seq->frameCount = 1;

        seq->frames = calloc(1, sizeof(int));
        seq->frames[0] = -1;

        seq->iframes = calloc(1, sizeof(int));
        seq->iframes[0] = -1;

        seq->delay = calloc(1, sizeof(int));
        seq->delay[0] = -1;
    }

    if (seq->preanim_move == -1) {
        seq->preanim_move = seq->walkmerge ? 2 : 0; // MERGE : DELAYMOVE
    }

    if (seq->postanim_move == -1) {
        seq->postanim_move = seq->walkmerge ? 2 : 0; // MERGE : DELAYMOVE
    }
}
