#ifdef __PS2__

#include <audsrv.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../client.h"
#include "../platform.h"
#include "ps2_sfx.h"

#define PS2_SFX_CODE \
    __attribute__((section(".ps2_audio_text"), noinline, used))
#define PS2_SFX_STATIC \
    static __attribute__((section(".ps2_audio_text"), noinline, used))
#define PS2_SFX_RODATA \
    __attribute__((section(".ps2_audio_rodata"), used, aligned(1)))
#define PS2_SFX_STATE \
    __attribute__((section(".ps2_audio_data"), used, aligned(64)))

#define PS2_SFX_PROOF_ID 468
#define PS2_SFX_MAX_FILE_BYTES (128u * 1024u)

typedef struct Ps2SfxState {
    uint32_t magic;
    audsrv_adpcm_t anvil;
    void *anvil_source;
    uint32_t anvil_source_size;
    uint8_t anvil_loaded;
    uint8_t anvil_failed;
} Ps2SfxState;

/*
 * Non-zero initializer keeps this state in the existing high-memory PROGBITS
 * audio segment. It must never become normal .bss/.sbss.
 */
static Ps2SfxState ps2_sfx_state PS2_SFX_STATE = {
    .magic = 0x53465831u /* "SFX1" */
};

static const char ps2_sfx_path_fmt[] PS2_SFX_RODATA =
    "%srom/ps2sfx/468.ps2a";
static const char ps2_sfx_read_mode[] PS2_SFX_RODATA = "rb";
static const char ps2_sfx_open_fail_fmt[] PS2_SFX_RODATA =
    "audio: SFX 468 open failed path=%s\n";
static const char ps2_sfx_bad_fmt[] PS2_SFX_RODATA =
    "audio: SFX 468 invalid path=%s size=%ld\n";
static const char ps2_sfx_alloc_fail_fmt[] PS2_SFX_RODATA =
    "audio: SFX 468 alloc failed size=%u\n";
static const char ps2_sfx_load_fmt[] PS2_SFX_RODATA =
    "audio: SFX 468 load=%d bytes=%u pitch=%d loop=%d channels=%d\n";
static const char ps2_sfx_play_fmt[] PS2_SFX_RODATA =
    "audio: SFX 468 play ch=%d\n";
static const char ps2_sfx_play_fail_fmt[] PS2_SFX_RODATA =
    "audio: SFX 468 play failed=%d\n";

PS2_SFX_STATIC int ps2_sfx_load_anvil(void)
{
    if (ps2_sfx_state.anvil_loaded) {
        return 1;
    }
    if (ps2_sfx_state.anvil_failed) {
        return 0;
    }

    char path[320];
    snprintf(path, sizeof(path), ps2_sfx_path_fmt, ps2_cache_prefix());

    FILE *file = fopen(path, ps2_sfx_read_mode);
    if (!file) {
        rs2_log(ps2_sfx_open_fail_fmt, path);
        ps2_sfx_state.anvil_failed = 1;
        return 0;
    }

    int seek_end = fseek(file, 0, SEEK_END);
    long size_long = seek_end == 0 ? ftell(file) : -1;
    int seek_start = 0;
    if (size_long > 0) {
        seek_start = fseek(file, 0, SEEK_SET);
    }

    if (seek_end != 0 ||
        seek_start != 0 ||
        size_long <= 16 ||
        size_long > (long)PS2_SFX_MAX_FILE_BYTES) {
        rs2_log(ps2_sfx_bad_fmt, path, size_long);
        fclose(file);
        ps2_sfx_state.anvil_failed = 1;
        return 0;
    }

    uint32_t size = (uint32_t)size_long;
    uint32_t padded = (size + 63u) & ~63u;
    unsigned char *buffer =
        (unsigned char *)memalign(64, (size_t)padded);
    if (!buffer) {
        rs2_log(ps2_sfx_alloc_fail_fmt, (unsigned int)size);
        fclose(file);
        ps2_sfx_state.anvil_failed = 1;
        return 0;
    }

    memset(buffer, 0, (size_t)padded);
    size_t got = fread(buffer, 1, size, file);
    fclose(file);

    if (got != size ||
        buffer[0] != 'A' ||
        buffer[1] != 'P' ||
        buffer[2] != 'C' ||
        buffer[3] != 'M') {
        rs2_log(ps2_sfx_bad_fmt, path, size_long);
        free(buffer);
        ps2_sfx_state.anvil_failed = 1;
        return 0;
    }

    int load =
        audsrv_load_adpcm(&ps2_sfx_state.anvil, buffer, (int)size);
    rs2_log(
        ps2_sfx_load_fmt,
        load,
        (unsigned int)size,
        ps2_sfx_state.anvil.pitch,
        ps2_sfx_state.anvil.loop,
        ps2_sfx_state.anvil.channels);

    if (load != AUDSRV_ERR_NOERROR) {
        free(buffer);
        ps2_sfx_state.anvil_failed = 1;
        return 0;
    }

    /*
     * Keep the EE source resident for the first hardware proof. audsrv has
     * already copied the encoded sample to its own audio-side storage, but
     * retaining this small buffer removes source-lifetime uncertainty.
     */
    ps2_sfx_state.anvil_source = buffer;
    ps2_sfx_state.anvil_source_size = size;
    ps2_sfx_state.anvil_loaded = 1;
    return 1;
}

void ps2_sfx_request(int id, int loops, int delay) PS2_SFX_CODE;
void ps2_sfx_request(int id, int loops, int delay)
{
    Client *c = ps2_crash_client;

    /*
     * Proof scope only: rev254 smithing sends anvil_4 as synth 468 with no
     * scheduling delay. All other packet-25 traffic remains a no-op on PS2
     * lowmem exactly as before.
     */
    if (!c ||
        !c->ingame ||
        !c->wave_enabled ||
        id != PS2_SFX_PROOF_ID ||
        delay != 0) {
        return;
    }

    (void)loops;

    if (!ps2_sfx_load_anvil()) {
        return;
    }

    int channel = audsrv_ch_play_adpcm(-1, &ps2_sfx_state.anvil);
    if (channel >= 0) {
        (void)audsrv_adpcm_set_volume_and_pan(
            channel, MAX_VOLUME, 0);
        rs2_log(ps2_sfx_play_fmt, channel);
    } else {
        rs2_log(ps2_sfx_play_fail_fmt, channel);
    }
}

#endif
