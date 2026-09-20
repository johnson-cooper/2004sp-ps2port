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
 * Milestone 3G: isolate heap-boundary movement from global relocation.
 *
 * The inert 3F candidate reproduced the pre-login network failure with no
 * functional change, proving placement alone is sufficient. This variant
 * restores normal .text/.data/.rodata/.bss placement and adds one separate
 * writable NOBITS orphan section immediately after the normal image.
 *
 * The symbol is forced live by ps2.yaml, but is never referenced at runtime.
 * Expected result: all normal section addresses match the hardware-good ELF,
 * while the LOAD MemSiz / effective image end grows by 0x180.
 */
__asm__(
    ".section .ps2_heap_tail,\"aw\",@nobits\n"
    ".global ps2_layout_heap_tail\n"
    ".type ps2_layout_heap_tail,@object\n"
    "ps2_layout_heap_tail:\n"
    ".space 0x180\n"
    ".size ps2_layout_heap_tail, .-ps2_layout_heap_tail\n"
    ".previous\n"
);

#endif
