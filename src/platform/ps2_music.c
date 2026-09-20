#ifdef __PS2__

#include <stdbool.h>
#include <stdio.h>

#include "../platform.h"
#include "ps2_music.h"

/*
 * Hardware A/B checkpoint.
 *
 * The first private MIDI-RPC audsrv build regressed real-hardware networking
 * even when no music pack was loaded. Keep the music entry points linkable,
 * but make them inert while we restore the exact proven voice-only audsrv
 * module. MIDI will return later as a separate post-login companion IOP
 * service instead of modifying the network-safe audsrv module.
 */
bool ps2_music_play(const char *name) {
    rs2_log("audio: MIDI disabled for voice-only network A/B: %s\n",
            name ? name : "(null)");
    return false;
}

void ps2_music_stop(void) {
}

void ps2_music_update(void) {
}

void ps2_music_set_volume(float volume) {
    (void)volume;
}

void ps2_music_shutdown(void) {
}

/*
 * Milestone 3F: pure ELF-layout A/B.
 *
 * Real hardware connects with this exact runtime audio implementation, while
 * tiny later EE changes fail before any rs2midi code can run. The good/failing
 * ELF comparison showed the failing image had exactly +0xE8 bytes of .text and
 * +0x50 bytes of .rodata. Inject those bytes here without adding any callable
 * code, branches, globals, constructors, RPC activity, or audio behavior.
 *
 * These bytes are intentionally unreachable. Their only purpose is to move
 * the following ELF sections by the same amount as the failing build.
 */
__asm__(
    ".pushsection .text\n"
    ".space 0xE8, 0\n"
    ".popsection\n"
    ".pushsection .rodata\n"
    ".space 0x50, 0\n"
    ".popsection\n"
);

#endif
