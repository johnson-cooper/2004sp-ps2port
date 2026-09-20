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
#include <stdlib.h>
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
#define RS2MIDI_RPC_GET_STATE  5

#define RS2MIDI_PONG 0x52533250u

#define RS2MIDI_RPC_HEADER_BYTES  64u
#define RS2MIDI_MAX_SAMPLE_BYTES  800u
#define RS2MIDI_MAX_IRX_BYTES     (128u * 1024u)
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
static const char ps2_audio_open_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi EE open failed path=%s\n";
static const char ps2_audio_size_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi EE invalid size path=%s size=%ld seek=%d\n";
static const char ps2_audio_alloc_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi EE buffer alloc failed size=%u\n";
static const char ps2_audio_read_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi EE read failed got=%u expected=%u\n";
static const char ps2_audio_load_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi EE-buffer load path=%s size=%u id=%d modres=%d\n";
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
static const char ps2_audio_state_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi state %s rc=%d loaded=%u pitch=%u voll=%u volr=%u ssa=0x%08x endx=0x%06x mv=%u/%u ext=%u/%u\n";
static const char ps2_audio_state_after_on[] PS2_AUDIO_RODATA = "after-on";
static const char ps2_audio_state_after_pitch[] PS2_AUDIO_RODATA = "after-pitch";
static const char ps2_audio_state_after_off[] PS2_AUDIO_RODATA = "after-off";

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

static void ps2_audio_log_state(
    SifRpcClientData_t *rpc,
    Rs2MidiRpcPacket *packet,
    const char *label) PS2_AUDIO_CODE;

static void ps2_audio_log_state(
    SifRpcClientData_t *rpc,
    Rs2MidiRpcPacket *packet,
    const char *label)
{
    memset(packet, 0, RS2MIDI_RPC_HEADER_BYTES);
    packet->words[0] = RS2MIDI_TEST_VOICE;
    int32_t rc = ps2_audio_rpc_status(
        rpc, RS2MIDI_RPC_GET_STATE, packet, sizeof(uint32_t));

    rs2_log(ps2_audio_state_fmt,
            label,
            (int)rc,
            (unsigned int)packet->words[1],
            (unsigned int)packet->words[2],
            (unsigned int)packet->words[3],
            (unsigned int)packet->words[4],
            (unsigned int)packet->words[5],
            (unsigned int)packet->words[6],
            (unsigned int)packet->words[7],
            (unsigned int)packet->words[8],
            (unsigned int)packet->words[9],
            (unsigned int)packet->words[10]);
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

    /*
     * Do not ask the ROM Module_File_loader to traverse mass0:/ here.
     * PCSX2 exposed that path overflowing the BIOS loader thread's 0x800-byte
     * stack before rs2midi could start. The EE-side stdio/BDM path is already
     * heavily exercised by RuneScape cache/map loading, so read the external
     * IRX here and hand PS2SDK an in-memory image instead.
     *
     * SifExecModuleBuffer() rounds its DMA byte count up to 16 bytes. Allocate
     * explicit padding and align the source to a cache line so that rounding
     * cannot read beyond the temporary allocation.
     */
    FILE *irx_file = fopen(path, "rb");
    if (!irx_file) {
        rs2_log(ps2_audio_open_fail_fmt, path);
        return;
    }

    int seek_end = fseek(irx_file, 0, SEEK_END);
    long irx_size_long = seek_end == 0 ? ftell(irx_file) : -1;
    int seek_start = 0;
    if (irx_size_long > 0) {
        seek_start = fseek(irx_file, 0, SEEK_SET);
    }

    if (seek_end != 0 ||
        seek_start != 0 ||
        irx_size_long <= 0 ||
        irx_size_long > (long)RS2MIDI_MAX_IRX_BYTES) {
        rs2_log(ps2_audio_size_fail_fmt,
                path, irx_size_long, seek_end != 0 ? seek_end : seek_start);
        fclose(irx_file);
        return;
    }

    uint32_t irx_size = (uint32_t)irx_size_long;
    uint32_t irx_padded_size = (irx_size + 15u) & ~15u;
    unsigned char *irx_alloc =
        (unsigned char *)malloc((size_t)irx_padded_size + 63u);
    if (!irx_alloc) {
        rs2_log(ps2_audio_alloc_fail_fmt, (unsigned int)irx_size);
        fclose(irx_file);
        return;
    }

    unsigned char *irx_buffer = (unsigned char *)(
        ((uintptr_t)irx_alloc + 63u) & ~(uintptr_t)63u);
    memset(irx_buffer, 0, irx_padded_size);

    size_t irx_read = fread(irx_buffer, 1, irx_size, irx_file);
    fclose(irx_file);
    if (irx_read != irx_size) {
        rs2_log(ps2_audio_read_fail_fmt,
                (unsigned int)irx_read, (unsigned int)irx_size);
        free(irx_alloc);
        return;
    }

    int modres = -1;
    int module_id = SifExecModuleBuffer(
        irx_buffer, irx_size, 0, NULL, &modres);
    free(irx_alloc);

    rs2_log(ps2_audio_load_fmt,
            path, (unsigned int)irx_size, module_id, modres);
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
        /*
         * Make the hardware smoke test unmistakable:
         *   chirp 1: quarter pitch -> half pitch while still playing
         *   chirp 2: half pitch
         *   chirp 3: native pitch
         * Use full voice volume and leave large gaps between re-triggers.
         */
        memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
        packet.words[0] = RS2MIDI_TEST_VOICE;
        packet.words[1] = base_pitch > 3 ? base_pitch / 4u : 1u;
        packet.words[2] = 0x3fff;
        packet.words[3] = 0x3fff;
        note_status = ps2_audio_rpc_status(
            &rpc, RS2MIDI_RPC_NOTE_ON, &packet, 16);

        if (note_status == 0) {
            DelayThread(20000);
            ps2_audio_log_state(&rpc, &packet, ps2_audio_state_after_on);

            DelayThread(80000);
            memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
            packet.words[0] = RS2MIDI_TEST_VOICE;
            packet.words[1] = base_pitch > 1 ? base_pitch / 2u : 1u;
            pitch_status = ps2_audio_rpc_status(
                &rpc, RS2MIDI_RPC_SET_PITCH, &packet, 8);

            DelayThread(50000);
            ps2_audio_log_state(&rpc, &packet, ps2_audio_state_after_pitch);

            DelayThread(50000);
            memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
            packet.words[0] = RS2MIDI_TEST_VOICE;
            off_status = ps2_audio_rpc_status(
                &rpc, RS2MIDI_RPC_KEY_OFF, &packet, 4);

            DelayThread(20000);
            ps2_audio_log_state(&rpc, &packet, ps2_audio_state_after_off);

            /* Give KOFF ample real-hardware time before reusing the voice. */
            DelayThread(280000);

            memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
            packet.words[0] = RS2MIDI_TEST_VOICE;
            packet.words[1] = base_pitch > 1 ? base_pitch / 2u : 1u;
            packet.words[2] = 0x3fff;
            packet.words[3] = 0x3fff;
            (void)ps2_audio_rpc_status(
                &rpc, RS2MIDI_RPC_NOTE_ON, &packet, 16);
            DelayThread(150000);

            memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
            packet.words[0] = RS2MIDI_TEST_VOICE;
            (void)ps2_audio_rpc_status(
                &rpc, RS2MIDI_RPC_KEY_OFF, &packet, 4);

            DelayThread(300000);

            memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
            packet.words[0] = RS2MIDI_TEST_VOICE;
            packet.words[1] = base_pitch;
            packet.words[2] = 0x3fff;
            packet.words[3] = 0x3fff;
            (void)ps2_audio_rpc_status(
                &rpc, RS2MIDI_RPC_NOTE_ON, &packet, 16);
            DelayThread(100000);

            memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
            packet.words[0] = RS2MIDI_TEST_VOICE;
            (void)ps2_audio_rpc_status(
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
