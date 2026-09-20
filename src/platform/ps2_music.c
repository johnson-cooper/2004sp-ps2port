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
#include "../ondemand.h"
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

#define PS2_MIDI_ARCHIVE           2
#define PS2_MIDI_AUTOPLAY_ID       0
#define PS2_MIDI_MAX_TRACKS        32
#define PS2_MIDI_VOICE_COUNT       24
#define PS2_MIDI_EVENT_BUDGET      512
#define PS2_MIDI_DEFAULT_TEMPO_US  500000u
#define PS2_MIDI_BASE_NOTE         76

#define PS2_AUDIO_CODE __attribute__((section(".ps2_audio_text"), noinline))
#define PS2_AUDIO_STATIC static __attribute__((section(".ps2_audio_text"), noinline))
#define PS2_AUDIO_RODATA __attribute__((section(".ps2_audio_rodata"), used, aligned(1)))
#define PS2_AUDIO_STATE __attribute__((section(".ps2_audio_data"), used, aligned(64)))

extern const char *ps2_cache_prefix(void);
extern const unsigned char ps2_audio_test_adpcm[];
extern const unsigned int ps2_audio_test_adpcm_size;

typedef struct Rs2MidiRpcPacket {
    uint32_t words[16];
    unsigned char sample[RS2MIDI_MAX_SAMPLE_BYTES];
} Rs2MidiRpcPacket;

typedef struct Ps2MidiTrack {
    const uint8_t *pos;
    const uint8_t *end;
    uint32_t next_tick;
    uint8_t running_status;
    uint8_t active;
} Ps2MidiTrack;

typedef struct Ps2MidiVoice {
    uint32_t age;
    uint8_t active;
    uint8_t channel;
    uint8_t key;
} Ps2MidiVoice;

typedef struct Ps2MusicState {
    uint32_t magic;
    uint32_t init_polls;
    uint32_t tempo_us;
    uint32_t base_pitch;
    uint32_t voice_serial;
    uint64_t tick_fp;
    uint64_t last_ms;
    uint8_t ready;
    uint8_t sample_loaded;
    uint8_t playing;
    uint8_t autoplay_attempted;
    uint8_t track_count;
    uint8_t format;
    uint16_t division;
    uint8_t *midi_data;
    int midi_size;
    SifRpcClientData_t rpc;
    Ps2MidiTrack tracks[PS2_MIDI_MAX_TRACKS];
    Ps2MidiVoice voices[PS2_MIDI_VOICE_COUNT];
} Ps2MusicState;

/*
 * Everything added by the PS2 music sequencer lives in the isolated high
 * audio PT_LOAD. Give the object a non-zero initializer so it is emitted as
 * PROGBITS in .ps2_audio_data instead of moving the hardware-sensitive normal
 * client BSS.
 */
static Ps2MusicState ps2_music_state PS2_AUDIO_STATE = {
    .magic = 0x4d555349u, /* "MUSI" */
    .init_polls = 1,
    .tempo_us = PS2_MIDI_DEFAULT_TEMPO_US
};

static const uint16_t ps2_midi_semitone_q12[12] PS2_AUDIO_RODATA = {
    4096, 4339, 4597, 4871, 5161, 5468,
    5793, 6137, 6502, 6889, 7298, 7732
};

/*
 * These MIDI chunk signatures must live in the isolated audio overlay too.
 * Leaving them as string literals inside overlay functions lets GCC place
 * them in ordinary .rodata, which is enough to perturb the hardware-sensitive
 * normal client ELF layout.
 */
static const uint8_t ps2_midi_mthd[4] PS2_AUDIO_RODATA = {
    'M', 'T', 'h', 'd'
};
static const uint8_t ps2_midi_mtrk[4] PS2_AUDIO_RODATA = {
    'M', 'T', 'r', 'k'
};

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
static const char ps2_audio_sample_ready_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi sample ready xfer=%d load=%d basepitch=%u raw=%u\n";
static const char ps2_midi_load_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI id=%d could not be loaded from ondemand archive\n";
static const char ps2_midi_bad_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI id=%d invalid/unsupported size=%d\n";
static const char ps2_midi_start_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI id=%d size=%d format=%u tracks=%u division=%u playing on SPU2\n";
static const char ps2_midi_event_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI parse stopped on track=%u tick=%u\n";
static const char ps2_midi_loop_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI id=%d loop\n";

PS2_AUDIO_STATIC uint16_t ps2_midi_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

PS2_AUDIO_STATIC uint32_t ps2_midi_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

PS2_AUDIO_STATIC bool ps2_midi_read_vlq(
    const uint8_t **cursor,
    const uint8_t *end,
    uint32_t *value)
{
    uint32_t result = 0;

    for (int i = 0; i < 4; i++) {
        if (*cursor >= end) {
            return false;
        }

        uint8_t byte = *(*cursor)++;
        result = (result << 7) | (uint32_t)(byte & 0x7f);
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
    }

    return false;
}

PS2_AUDIO_STATIC int32_t ps2_audio_rpc_status(
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

PS2_AUDIO_STATIC void ps2_midi_key_off_voice(int voice)
{
    if (voice < 0 || voice >= PS2_MIDI_VOICE_COUNT ||
        !ps2_music_state.ready) {
        return;
    }

    Rs2MidiRpcPacket packet __attribute__((aligned(64)));
    memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
    packet.words[0] = (uint32_t)voice;
    (void)ps2_audio_rpc_status(
        &ps2_music_state.rpc, RS2MIDI_RPC_KEY_OFF, &packet, 4);

    ps2_music_state.voices[voice].active = 0;
}

PS2_AUDIO_STATIC void ps2_midi_all_notes_off_channel(uint8_t channel)
{
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        if (ps2_music_state.voices[voice].active &&
            ps2_music_state.voices[voice].channel == channel) {
            ps2_midi_key_off_voice(voice);
        }
    }
}

PS2_AUDIO_STATIC void ps2_midi_all_notes_off(void)
{
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        if (ps2_music_state.voices[voice].active) {
            ps2_midi_key_off_voice(voice);
        }
    }
}

PS2_AUDIO_STATIC uint16_t ps2_midi_pitch_for_key(uint8_t key)
{
    int delta = (int)key - PS2_MIDI_BASE_NOTE;
    uint32_t pitch = ps2_music_state.base_pitch;

    if (delta >= 0) {
        int octaves = delta / 12;
        int semitone = delta % 12;
        pitch = (pitch * ps2_midi_semitone_q12[semitone] + 2048u) >> 12;
        if (octaves >= 16 || pitch > (0x3fffu >> octaves)) {
            pitch = 0x3fffu;
        } else {
            pitch <<= octaves;
        }
    } else {
        int down = -delta;
        int octaves = down / 12;
        int semitone = down % 12;
        pitch = (pitch * 4096u +
                 (uint32_t)ps2_midi_semitone_q12[semitone] / 2u) /
                (uint32_t)ps2_midi_semitone_q12[semitone];
        if (octaves >= 16) {
            pitch = 1;
        } else {
            pitch >>= octaves;
        }
    }

    if (pitch < 1) {
        pitch = 1;
    }
    if (pitch > 0x3fff) {
        pitch = 0x3fff;
    }
    return (uint16_t)pitch;
}

PS2_AUDIO_STATIC void ps2_midi_note_off(uint8_t channel, uint8_t key)
{
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        Ps2MidiVoice *slot = &ps2_music_state.voices[voice];
        if (slot->active && slot->channel == channel && slot->key == key) {
            ps2_midi_key_off_voice(voice);
        }
    }
}

PS2_AUDIO_STATIC void ps2_midi_note_on(
    uint8_t channel,
    uint8_t key,
    uint8_t velocity)
{
    /*
     * Milestone 5A intentionally leaves percussion silent. The proven 660 Hz
     * diagnostic sample is a temporary pitched instrument, not a drum bank.
     */
    if (channel == 9 || velocity == 0 || !ps2_music_state.sample_loaded) {
        if (velocity == 0) {
            ps2_midi_note_off(channel, key);
        }
        return;
    }

    int chosen = -1;

    /* Repeated note: retrigger the voice already representing this key. */
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        Ps2MidiVoice *slot = &ps2_music_state.voices[voice];
        if (slot->active && slot->channel == channel && slot->key == key) {
            chosen = voice;
            break;
        }
    }

    /* Otherwise prefer a genuinely free hardware voice. */
    if (chosen < 0) {
        for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
            if (!ps2_music_state.voices[voice].active) {
                chosen = voice;
                break;
            }
        }
    }

    /* Polyphony overflow: steal the oldest voice. KON retriggers its SSA. */
    if (chosen < 0) {
        uint32_t oldest_age = UINT32_MAX;
        for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
            if (ps2_music_state.voices[voice].age < oldest_age) {
                oldest_age = ps2_music_state.voices[voice].age;
                chosen = voice;
            }
        }
    }

    if (chosen < 0) {
        return;
    }

    uint32_t volume =
        ((uint32_t)velocity * 0x3fffu + 63u) / 127u;

    Rs2MidiRpcPacket packet __attribute__((aligned(64)));
    memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
    packet.words[0] = (uint32_t)chosen;
    packet.words[1] = (uint32_t)ps2_midi_pitch_for_key(key);
    packet.words[2] = volume;
    packet.words[3] = volume;

    if (ps2_audio_rpc_status(
            &ps2_music_state.rpc,
            RS2MIDI_RPC_NOTE_ON,
            &packet,
            16) == 0) {
        Ps2MidiVoice *slot = &ps2_music_state.voices[chosen];
        slot->active = 1;
        slot->channel = channel;
        slot->key = key;
        slot->age = ++ps2_music_state.voice_serial;
    }
}

PS2_AUDIO_STATIC bool ps2_midi_schedule_next(Ps2MidiTrack *track)
{
    if (track->pos >= track->end) {
        track->active = 0;
        return true;
    }

    uint32_t delta = 0;
    if (!ps2_midi_read_vlq(&track->pos, track->end, &delta)) {
        track->active = 0;
        return false;
    }

    track->next_tick += delta;
    return true;
}

PS2_AUDIO_STATIC bool ps2_midi_process_event(Ps2MidiTrack *track)
{
    if (!track->active || track->pos >= track->end) {
        track->active = 0;
        return true;
    }

    uint8_t status = *track->pos;
    if (status & 0x80) {
        track->pos++;
        if (status < 0xf0) {
            track->running_status = status;
        } else {
            track->running_status = 0;
        }
    } else {
        status = track->running_status;
        if (status < 0x80 || status >= 0xf0) {
            track->active = 0;
            return false;
        }
    }

    if (status == 0xff) {
        if (track->pos >= track->end) {
            track->active = 0;
            return false;
        }

        uint8_t meta_type = *track->pos++;
        uint32_t length = 0;
        if (!ps2_midi_read_vlq(&track->pos, track->end, &length) ||
            length > (uint32_t)(track->end - track->pos)) {
            track->active = 0;
            return false;
        }

        if (meta_type == 0x51 && length == 3) {
            uint32_t tempo =
                ((uint32_t)track->pos[0] << 16) |
                ((uint32_t)track->pos[1] << 8) |
                (uint32_t)track->pos[2];
            if (tempo != 0) {
                ps2_music_state.tempo_us = tempo;
            }
        }

        track->pos += length;
        if (meta_type == 0x2f) {
            track->active = 0;
            return true;
        }

        return ps2_midi_schedule_next(track);
    }

    if (status == 0xf0 || status == 0xf7) {
        uint32_t length = 0;
        if (!ps2_midi_read_vlq(&track->pos, track->end, &length) ||
            length > (uint32_t)(track->end - track->pos)) {
            track->active = 0;
            return false;
        }
        track->pos += length;
        return ps2_midi_schedule_next(track);
    }

    if (status >= 0xf0) {
        int bytes = 0;
        if (status == 0xf1 || status == 0xf3) {
            bytes = 1;
        } else if (status == 0xf2) {
            bytes = 2;
        }

        if ((size_t)(track->end - track->pos) < (size_t)bytes) {
            track->active = 0;
            return false;
        }
        track->pos += bytes;
        return ps2_midi_schedule_next(track);
    }

    uint8_t kind = status & 0xf0;
    uint8_t channel = status & 0x0f;
    int data_bytes = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;

    if ((size_t)(track->end - track->pos) < (size_t)data_bytes) {
        track->active = 0;
        return false;
    }

    uint8_t data1 = *track->pos++;
    uint8_t data2 = data_bytes == 2 ? *track->pos++ : 0;

    /*
     * Keep this as comparisons rather than a switch. On MIPS, a switch may
     * legally grow a compiler-generated jump table in ordinary .rodata even
     * though this function itself is assigned to .ps2_audio_text.
     */
    if (kind == 0x80) {
        ps2_midi_note_off(channel, data1);
    } else if (kind == 0x90) {
        if (data2 == 0) {
            ps2_midi_note_off(channel, data1);
        } else {
            ps2_midi_note_on(channel, data1, data2);
        }
    } else if (kind == 0xb0 && (data1 == 120 || data1 == 123)) {
        ps2_midi_all_notes_off_channel(channel);
    }

    return ps2_midi_schedule_next(track);
}

PS2_AUDIO_STATIC bool ps2_midi_reset_tracks(void)
{
    const uint8_t *data = ps2_music_state.midi_data;
    int size = ps2_music_state.midi_size;

    if (!data || size < 14 || memcmp(data, ps2_midi_mthd, sizeof(ps2_midi_mthd)) != 0) {
        return false;
    }

    uint32_t header_size = ps2_midi_be32(data + 4);
    if (header_size < 6 || header_size > (uint32_t)(size - 8)) {
        return false;
    }

    uint16_t format = ps2_midi_be16(data + 8);
    uint16_t track_count = ps2_midi_be16(data + 10);
    uint16_t division = ps2_midi_be16(data + 12);

    if (format > 1 ||
        track_count == 0 ||
        track_count > PS2_MIDI_MAX_TRACKS ||
        division == 0 ||
        (division & 0x8000) != 0) {
        return false;
    }

    memset(ps2_music_state.tracks, 0, sizeof(ps2_music_state.tracks));
    const uint8_t *cursor = data + 8 + header_size;
    const uint8_t *file_end = data + size;

    for (uint16_t i = 0; i < track_count; i++) {
        if ((size_t)(file_end - cursor) < 8 ||
            memcmp(cursor, ps2_midi_mtrk, sizeof(ps2_midi_mtrk)) != 0) {
            return false;
        }

        uint32_t track_size = ps2_midi_be32(cursor + 4);
        cursor += 8;
        if (track_size > (uint32_t)(file_end - cursor)) {
            return false;
        }

        Ps2MidiTrack *track = &ps2_music_state.tracks[i];
        track->pos = cursor;
        track->end = cursor + track_size;
        track->next_tick = 0;
        track->running_status = 0;
        track->active = track_size != 0;

        if (track->active) {
            uint32_t first_delta = 0;
            if (!ps2_midi_read_vlq(
                    &track->pos, track->end, &first_delta)) {
                return false;
            }
            track->next_tick = first_delta;
        }

        cursor += track_size;
    }

    ps2_music_state.format = (uint8_t)format;
    ps2_music_state.track_count = (uint8_t)track_count;
    ps2_music_state.division = division;
    ps2_music_state.tempo_us = PS2_MIDI_DEFAULT_TEMPO_US;
    ps2_music_state.tick_fp = 0;
    ps2_music_state.last_ms = rs2_now();
    return true;
}

PS2_AUDIO_STATIC bool ps2_midi_any_track_active(void)
{
    for (int i = 0; i < ps2_music_state.track_count; i++) {
        if (ps2_music_state.tracks[i].active) {
            return true;
        }
    }
    return false;
}

PS2_AUDIO_STATIC void ps2_midi_stop_song(void)
{
    ps2_midi_all_notes_off();

    if (ps2_music_state.midi_data) {
        free(ps2_music_state.midi_data);
        ps2_music_state.midi_data = NULL;
    }

    ps2_music_state.midi_size = 0;
    ps2_music_state.playing = 0;
    ps2_music_state.track_count = 0;
    ps2_music_state.tick_fp = 0;
}

PS2_AUDIO_STATIC bool ps2_midi_start_id(int id)
{
    if (!ps2_music_state.ready || id < 0) {
        return false;
    }

    ps2_midi_stop_song();

    int midi_size = 0;
    int8_t *midi_data =
        ondemand_get(PS2_MIDI_ARCHIVE, id, &midi_size);
    if (!midi_data) {
        rs2_log(ps2_midi_load_fail_fmt, id);
        return false;
    }

    ps2_music_state.midi_data = (uint8_t *)midi_data;
    ps2_music_state.midi_size = midi_size;

    if (!ps2_midi_reset_tracks()) {
        rs2_log(ps2_midi_bad_fmt, id, midi_size);
        free(ps2_music_state.midi_data);
        ps2_music_state.midi_data = NULL;
        ps2_music_state.midi_size = 0;
        return false;
    }

    ps2_music_state.playing = 1;
    rs2_log(ps2_midi_start_fmt,
            id,
            midi_size,
            (unsigned int)ps2_music_state.format,
            (unsigned int)ps2_music_state.track_count,
            (unsigned int)ps2_music_state.division);
    return true;
}

PS2_AUDIO_STATIC void ps2_midi_update_song(void)
{
    if (!ps2_music_state.playing ||
        !ps2_music_state.midi_data ||
        ps2_music_state.division == 0 ||
        ps2_music_state.tempo_us == 0) {
        return;
    }

    uint64_t now = rs2_now();
    uint64_t elapsed_ms = now - ps2_music_state.last_ms;
    ps2_music_state.last_ms = now;

    /* Do not fast-forward seconds of music after a transient gameplay stall. */
    if (elapsed_ms > 250) {
        elapsed_ms = 250;
    }

    uint64_t advance =
        elapsed_ms * 1000ull *
        (uint64_t)ps2_music_state.division * 65536ull /
        (uint64_t)ps2_music_state.tempo_us;
    ps2_music_state.tick_fp += advance;

    uint32_t current_tick = (uint32_t)(ps2_music_state.tick_fp >> 16);
    int events = 0;

    while (events < PS2_MIDI_EVENT_BUDGET) {
        int chosen = -1;
        uint32_t chosen_tick = UINT32_MAX;

        for (int i = 0; i < ps2_music_state.track_count; i++) {
            Ps2MidiTrack *track = &ps2_music_state.tracks[i];
            if (track->active && track->next_tick < chosen_tick) {
                chosen = i;
                chosen_tick = track->next_tick;
            }
        }

        if (chosen < 0 || chosen_tick > current_tick) {
            break;
        }

        if (!ps2_midi_process_event(&ps2_music_state.tracks[chosen])) {
            rs2_log(ps2_midi_event_fail_fmt,
                    (unsigned int)chosen,
                    (unsigned int)chosen_tick);
        }
        events++;
    }

    if (!ps2_midi_any_track_active()) {
        ps2_midi_all_notes_off();
        if (ps2_midi_reset_tracks()) {
            rs2_log(ps2_midi_loop_fmt, PS2_MIDI_AUTOPLAY_ID);
        } else {
            ps2_midi_stop_song();
        }
    }
}

PS2_AUDIO_STATIC bool ps2_audio_init_backend(void)
{
    char path[320];
    snprintf(path, sizeof(path), ps2_audio_path_fmt, ps2_cache_prefix());

    /*
     * Preserve the now-proven loader path exactly: read the external IRX on
     * the EE and execute the in-memory image. Asking the ROM file loader to
     * traverse mass0:/ overflowed its tiny loader-thread stack in PCSX2.
     */
    FILE *irx_file = fopen(path, "rb");
    if (!irx_file) {
        rs2_log(ps2_audio_open_fail_fmt, path);
        return false;
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
        return false;
    }

    uint32_t irx_size = (uint32_t)irx_size_long;
    uint32_t irx_padded_size = (irx_size + 15u) & ~15u;
    unsigned char *irx_alloc =
        (unsigned char *)malloc((size_t)irx_padded_size + 63u);
    if (!irx_alloc) {
        rs2_log(ps2_audio_alloc_fail_fmt, (unsigned int)irx_size);
        fclose(irx_file);
        return false;
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
        return false;
    }

    int modres = -1;
    int module_id = SifExecModuleBuffer(
        irx_buffer, irx_size, 0, NULL, &modres);
    free(irx_alloc);

    rs2_log(ps2_audio_load_fmt,
            path, (unsigned int)irx_size, module_id, modres);
    if (module_id < 0 || modres < 0) {
        return false;
    }

    memset(&ps2_music_state.rpc, 0, sizeof(ps2_music_state.rpc));
    for (int attempt = 0; attempt < 64; attempt++) {
        int rc = sceSifBindRpc(
            &ps2_music_state.rpc, RS2MIDI_RPC_ID, 0);
        if (rc < 0) {
            rs2_log(ps2_audio_bind_fail_fmt, rc, attempt);
            return false;
        }
        if (ps2_music_state.rpc.server != NULL) {
            break;
        }
        DelayThread(1000);
    }

    if (ps2_music_state.rpc.server == NULL) {
        rs2_log(ps2_audio_bind_timeout_fmt);
        return false;
    }

    Rs2MidiRpcPacket packet __attribute__((aligned(64)));
    memset(&packet, 0, sizeof(packet));
    packet.words[0] = 0x12345678u;

    int ping_rc = sceSifCallRpc(
        &ps2_music_state.rpc,
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
        return false;
    }

    /*
     * Keep the exact sample and ROM-LIBSD acceptance rule that produced the
     * first audible hardware success. It is a temporary single-timbre MIDI
     * instrument for this sequencer proof, not the final RuneScape bank.
     */
    if (ps2_audio_test_adpcm_size < 16) {
        rs2_log(ps2_audio_sample_bad_fmt,
                ps2_audio_test_adpcm_size, 0u);
        return false;
    }

    uint32_t raw_size = ps2_audio_test_adpcm_size - 16u;
    if (raw_size == 0 ||
        raw_size > RS2MIDI_MAX_SAMPLE_BYTES ||
        (raw_size & 0x0fu) != 0) {
        rs2_log(ps2_audio_sample_bad_fmt,
                ps2_audio_test_adpcm_size, raw_size);
        return false;
    }

    ps2_music_state.base_pitch =
        ((uint32_t)ps2_audio_test_adpcm[8]) |
        ((uint32_t)ps2_audio_test_adpcm[9] << 8) |
        ((uint32_t)ps2_audio_test_adpcm[10] << 16) |
        ((uint32_t)ps2_audio_test_adpcm[11] << 24);

    memset(&packet, 0, sizeof(packet));
    packet.words[0] = raw_size;
    memcpy(packet.sample, ps2_audio_test_adpcm + 16, raw_size);

    int32_t load_status = ps2_audio_rpc_status(
        &ps2_music_state.rpc,
        RS2MIDI_RPC_LOAD,
        &packet,
        RS2MIDI_RPC_HEADER_BYTES + raw_size);
    int32_t transfer_result = (int32_t)packet.words[1];

    rs2_log(ps2_audio_sample_ready_fmt,
            (int)transfer_result,
            (int)load_status,
            (unsigned int)ps2_music_state.base_pitch,
            (unsigned int)raw_size);

    if (load_status < 0) {
        return false;
    }

    ps2_music_state.sample_loaded = 1;
    ps2_music_state.ready = 1;
    return true;
}

void ps2_audio_update_late(void) PS2_AUDIO_CODE;
void ps2_audio_update_late(void)
{
    Client *c = ps2_crash_client;

    /*
     * The companion still never loads before the client is fully in-world.
     * This preserves the network-safe boot/login sequence proven on hardware.
     */
    if (!c || !c->ingame || c->scene_state != 2) {
        if (ps2_music_state.playing) {
            ps2_midi_stop_song();
        }
        ps2_music_state.autoplay_attempted = 0;
        if (!ps2_music_state.ready) {
            ps2_music_state.init_polls = 1;
        }
        return;
    }

    if (!ps2_music_state.ready) {
        if (ps2_music_state.init_polls == UINT32_MAX) {
            return;
        }
        if (++ps2_music_state.init_polls < 251) {
            return;
        }

        /*
         * One backend attempt per in-world session. If it fails, do not hammer
         * USB/IOP every frame; leaving init_polls above the threshold keeps the
         * failure visible in the log without repeated module loads.
         */
        ps2_music_state.init_polls = UINT32_MAX;
        if (!ps2_audio_init_backend()) {
            return;
        }
    }

    /*
     * Milestone 5A: prove actual rev254 MIDI sequencing without touching the
     * normal client/protocol layout yet. Archive 2, id 0 is the reference
     * client's startup song. We intentionally start it only after world entry
     * so title/login networking remains identical to the audible-beep baseline.
     */
    if (!ps2_music_state.autoplay_attempted) {
        ps2_music_state.autoplay_attempted = 1;
        (void)ps2_midi_start_id(PS2_MIDI_AUTOPLAY_ID);
    }

    ps2_midi_update_song();
}

/*
 * Keep the normal-client entry points tiny and unchanged for this A/B. The
 * real sequencer currently starts from the isolated post-world update hook.
 * Server-driven rev254 song/jingle ids are the next step after this hardware
 * proof succeeds.
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
