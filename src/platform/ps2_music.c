#ifdef __PS2__

#ifdef client
#undef client
#endif

#include <loadfile.h>

#include <stdbool.h>
#include <stdio.h>

#include "../client.h"
#include "../platform.h"
#include "ps2_music.h"

/*
 * Hardware-safe companion-module experiment.
 *
 * Keep the known-good PS2 platform/network object untouched. The successful
 * 548b27c build already calls ps2_music_update() every frame, so reuse this
 * existing stub instead of adding another hook to client.c or ps2.c.
 *
 * rs2midi remains a standalone IRX beside client.elf. This milestone only
 * loads the module after the world has been live for a while; it does not
 * bind/call its RPC yet and it does not touch audsrv.
 */
extern const char *ps2_cache_prefix(void);

static bool rs2midi_load_attempted;
static int rs2midi_live_polls;

bool ps2_music_play(const char *name) {
    rs2_log("audio: MIDI disabled for voice-only network A/B: %s\n",
            name ? name : "(null)");
    return false;
}

void ps2_music_stop(void) {
}

void ps2_music_update(void) {
    if (rs2midi_load_attempted) {
        return;
    }

    Client *c = ps2_crash_client;
    if (!c || !c->ingame || c->scene_state != 2) {
        rs2midi_live_polls = 0;
        return;
    }

    if (++rs2midi_live_polls < 250) {
        return;
    }

    rs2midi_load_attempted = true;

    char path[320];
    snprintf(path, sizeof(path), "%srs2midi.irx", ps2_cache_prefix());

    int modres = -1;
    int module_id = SifLoadStartModule(path, 0, NULL, &modres);
    rs2_log("audio: external rs2midi load-only path=%s id=%d modres=%d\n",
            path, module_id, modres);
}

void ps2_music_set_volume(float volume) {
    (void)volume;
}

void ps2_music_shutdown(void) {
}

#endif
