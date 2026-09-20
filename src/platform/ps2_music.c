#ifdef __PS2__

#ifdef client
#undef client
#endif

#include <delaythread.h>
#include <loadfile.h>
#include <sifrpc.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../client.h"
#include "../platform.h"
#include "ps2_music.h"

#define RS2MIDI_RPC_ID 0x5253324d
#define RS2MIDI_RPC_PING 0
#define RS2MIDI_PONG 0x52533250u

#define PS2_AUDIO_CODE __attribute__((section(".ps2_audio_text"), noinline))
#define PS2_AUDIO_RODATA __attribute__((section(".ps2_audio_rodata"), used, aligned(1)))
#define PS2_AUDIO_DATA __attribute__((section(".ps2_audio_data"), used, aligned(4)))

extern const char *ps2_cache_prefix(void);

/*
 * Keep all NEW companion-loader state out of the normal client BSS. Real
 * hardware proved that relocating the normal BSS to 0x00200c80 is enough to
 * break pre-login networking even with no functional audio change.
 *
 * This word is deliberately initialized (rather than BSS) so it is loaded as
 * part of the isolated high-memory audio segment.
 *   1..250   = consecutive live-world polls
 *   UINT32_MAX = load/PING already attempted
 */
static uint32_t ps2_audio_late_state PS2_AUDIO_DATA = 1;

static const char ps2_audio_path_fmt[] PS2_AUDIO_RODATA = "%srs2midi.irx";
static const char ps2_audio_load_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi isolated load path=%s id=%d modres=%d\n";
static const char ps2_audio_bind_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi isolated bind failed rc=%d attempt=%d\n";
static const char ps2_audio_bind_timeout_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi isolated RPC server did not bind\n";
static const char ps2_audio_ping_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi isolated ping call=%d pong=0x%08x\n";

void ps2_audio_update_late(void) PS2_AUDIO_CODE;
void ps2_audio_update_late(void) {
    if (ps2_audio_late_state == UINT32_MAX) {
        return;
    }

    Client *c = ps2_crash_client;
    if (!c || !c->ingame || c->scene_state != 2) {
        ps2_audio_late_state = 1;
        return;
    }

    if (++ps2_audio_late_state < 251) {
        return;
    }

    /* One shot only for this hardware checkpoint. */
    ps2_audio_late_state = UINT32_MAX;

    char path[320];
    snprintf(path, sizeof(path), ps2_audio_path_fmt, ps2_cache_prefix());

    int modres = -1;
    int module_id = SifLoadStartModule(path, 0, NULL, &modres);
    rs2_log(ps2_audio_load_fmt, path, module_id, modres);
    if (module_id < 0 || modres < 0) {
        return;
    }

    SifRpcClientData_t rpc;
    memset(&rpc, 0, sizeof(rpc));

    for (int attempt = 0; attempt < 64; attempt++) {
        int rc = sceSifBindRpc(&rpc, RS2MIDI_RPC_ID, 0);
        if (rc < 0) {
            rs2_log(ps2_audio_bind_fail_fmt, rc, attempt);
            return;
        }
        if (rpc.server != NULL) {
            break;
        }
        DelayThread(1000);
    }

    if (rpc.server == NULL) {
        rs2_log("%s", ps2_audio_bind_timeout_fmt);
        return;
    }

    uint32_t rpcbuf[16] __attribute__((aligned(64)));
    memset(rpcbuf, 0, sizeof(rpcbuf));
    rpcbuf[0] = 0x12345678u;

    int call_rc = sceSifCallRpc(
        &rpc, RS2MIDI_RPC_PING, 0,
        rpcbuf, sizeof(uint32_t),
        rpcbuf, sizeof(uint32_t),
        NULL, NULL);

    rs2_log(ps2_audio_ping_fmt, call_rc, (unsigned int)rpcbuf[0]);
}

/*
 * Hardware-good voice-only entry points stay normal except update.
 *
 * ps2_music_update() was an 8-byte empty MIPS function in the hardware-good
 * ELF. Make it an exact two-instruction tail jump into the isolated segment:
 * J preserves the caller's RA, so ps2_audio_update_late() returns directly to
 * the original caller. __builtin_unreachable prevents GCC from appending a
 * second normal return sequence.
 */
bool ps2_music_play(const char *name) {
    rs2_log("audio: MIDI disabled for voice-only network A/B: %s\n",
            name ? name : "(null)");
    return false;
}

void ps2_music_stop(void) {
}

void ps2_music_update(void) {
    __asm__ volatile(
        ".set noreorder\n"
        "j ps2_audio_update_late\n"
        "nop\n"
        ".set reorder\n");
    __builtin_unreachable();
}

void ps2_music_set_volume(float volume) {
    (void)volume;
}

void ps2_music_shutdown(void) {
}

#endif
