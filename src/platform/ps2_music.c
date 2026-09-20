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

#define RS2MIDI_RPC_PING       0
#define RS2MIDI_RPC_LOAD       1
#define RS2MIDI_RPC_NOTE_ON    2
#define RS2MIDI_RPC_SET_PITCH  3
#define RS2MIDI_RPC_KEY_OFF    4

#define RS2MIDI_PONG 0x52533250u

#define RS2MIDI_RPC_HEADER_BYTES  64u
#define RS2MIDI_MAX_SAMPLE_BYTES  800u
#define RS2MIDI_TEST_VOICE        0u

#define PS2_AUDIO_CODE __attribute__((section(".ps2_audio_text"), noinline))
#define PS2_AUDIO_RODATA __attribute__((section(".ps2_audio_rodata"), used, aligned(1)))
#define PS2_AUDIO_DATA __attribute__((section(".ps2_audio_data"), used, aligned(4)))

extern const char *ps2_cache_prefix(void);
extern const unsigned char ps2_audio_test_adpcm[];
extern const unsigned int ps2_audio_test_adpcm_size;

typedef struct Rs2MidiRpcPacket {
    uint32_t words[16];
    unsigned char sample[RS2MIDI_MAX_SAMPLE_BYTES];
} Rs2MidiRpcPacket;

/*
 * Keep all NEW companion-loader state out of the normal client BSS. Real
 * hardware proved that relocating the normal BSS to 0x00200c80 is enough to
 * break pre-login networking even with no functional audio change.
 *
 * This word is deliberately initialized (rather than BSS) so it is loaded as
 * part of the isolated high-memory audio segment.
 *   1..250      = consecutive live-world polls
 *   UINT32_MAX  = load/voice test already attempted
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
static const char ps2_audio_sample_bad_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi test sample invalid size=%u raw=%u\n";
static const char ps2_audio_voice_test_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi voice test load=%d note=%d pitch=%d off=%d basepitch=%u raw=%u\n";

static int32_t ps2_audio_rpc_status(
    SifRpcClientData_t *rpc,
    int command,
    Rs2MidiRpcPacket *packet,
    int send_size) PS2_AUDIO_CODE;

static int32_t ps2_audio_rpc_status(
    SifRpcClientData_t *rpc,
    int command,
    Rs2MidiRpcPacket *packet,
    int send_size)
{
    int rc = sceSifCallRpc(
        rpc,
        command,
        0,
        packet,
        send_size,
        packet,
        RS2MIDI_RPC_HEADER_BYTES,
        NULL,
        NULL);

    if (rc < 0) {
        return rc;
    }
    return (int32_t)packet->words[0];
}

void ps2_audio_update_late(void) PS2_AUDIO_CODE;
void ps2_audio_update_late(void)
{
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
        rs2_log(ps2_audio_bind_timeout_fmt);
        return;
    }

    Rs2MidiRpcPacket packet __attribute__((aligned(64)));
    memset(&packet, 0, sizeof(packet));
    packet.words[0] = 0x12345678u;

    int ping_rc = sceSifCallRpc(
        &rpc,
        RS2MIDI_RPC_PING,
        0,
        &packet,
        sizeof(uint32_t),
        &packet,
        sizeof(uint32_t),
        NULL,
        NULL);

    rs2_log(ps2_audio_ping_fmt, ping_rc, (unsigned int)packet.words[0]);
    if (ping_rc < 0 || packet.words[0] != RS2MIDI_PONG) {
        return;
    }

    /*
     * Reuse the existing real-hardware-proven APCM smoke sample. The first
     * 16 bytes are the audsrv APCM header; rs2midi uploads only raw PS2 ADPCM.
     */
    if (ps2_audio_test_adpcm_size < 16) {
        rs2_log(ps2_audio_sample_bad_fmt,
                ps2_audio_test_adpcm_size, 0u);
        return;
    }

    uint32_t raw_size = ps2_audio_test_adpcm_size - 16u;
    if (raw_size == 0 ||
        raw_size > RS2MIDI_MAX_SAMPLE_BYTES ||
        (raw_size & 0x0fu) != 0) {
        rs2_log(ps2_audio_sample_bad_fmt,
                ps2_audio_test_adpcm_size, raw_size);
        return;
    }

    uint32_t base_pitch =
        ((uint32_t)ps2_audio_test_adpcm[8]) |
        ((uint32_t)ps2_audio_test_adpcm[9] << 8) |
        ((uint32_t)ps2_audio_test_adpcm[10] << 16) |
        ((uint32_t)ps2_audio_test_adpcm[11] << 24);

    memset(&packet, 0, sizeof(packet));
    packet.words[0] = raw_size;
    memcpy(packet.sample, ps2_audio_test_adpcm + 16, raw_size);

    int32_t load_status = ps2_audio_rpc_status(
        &rpc,
        RS2MIDI_RPC_LOAD,
        &packet,
        RS2MIDI_RPC_HEADER_BYTES + raw_size);

    int32_t note_status = -999;
    int32_t pitch_status = -999;
    int32_t off_status = -999;

    if (load_status == 0) {
        memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
        packet.words[0] = RS2MIDI_TEST_VOICE;
        packet.words[1] = base_pitch > 1 ? base_pitch / 2u : 1u;
        packet.words[2] = 0x1000;
        packet.words[3] = 0x1000;
        note_status = ps2_audio_rpc_status(
            &rpc, RS2MIDI_RPC_NOTE_ON, &packet, 16);

        if (note_status == 0) {
            DelayThread(25000);

            memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
            packet.words[0] = RS2MIDI_TEST_VOICE;
            packet.words[1] = base_pitch;
            pitch_status = ps2_audio_rpc_status(
                &rpc, RS2MIDI_RPC_SET_PITCH, &packet, 8);

            DelayThread(25000);

            memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
            packet.words[0] = RS2MIDI_TEST_VOICE;
            off_status = ps2_audio_rpc_status(
                &rpc, RS2MIDI_RPC_KEY_OFF, &packet, 4);
        }
    }

    rs2_log(ps2_audio_voice_test_fmt,
            (int)load_status,
            (int)note_status,
            (int)pitch_status,
            (int)off_status,
            (unsigned int)base_pitch,
            (unsigned int)raw_size);
}

/*
 * Hardware-good voice-only entry points stay normal except update.
 *
 * ps2_music_update() was an 8-byte empty MIPS function in the hardware-good
 * ELF. Keep the normal-client hook tiny: it tail-jumps into the isolated
 * high-memory segment, preserving the BSS/data layout proven on hardware.
 */
bool ps2_music_play(const char *name)
{
    rs2_log("audio: MIDI disabled for voice-only network A/B: %s\n",
            name ? name : "(null)");
    return false;
}

void ps2_music_stop(void)
{
}

void ps2_music_update(void)
{
    __asm__ volatile(
        ".set noreorder\n"
        "j ps2_audio_update_late\n"
        "nop\n"
        ".set reorder\n");
    __builtin_unreachable();
}

void ps2_music_set_volume(float volume)
{
    (void)volume;
}

void ps2_music_shutdown(void)
{
}

#endif
