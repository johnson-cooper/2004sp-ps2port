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
#include "ps2_midi_bank.h"

#define RS2MIDI_RPC_ID 0x5253324d

#define RS2MIDI_RPC_PING       0
#define RS2MIDI_RPC_LOAD       1
#define RS2MIDI_RPC_NOTE_ON    2
#define RS2MIDI_RPC_SET_PITCH  3
#define RS2MIDI_RPC_KEY_OFF    4
#define RS2MIDI_RPC_GET_STATE  5
#define RS2MIDI_RPC_LOAD_SLOT  6
#define RS2MIDI_RPC_LOAD_ABS   7
#define RS2MIDI_RPC_NOTE_ON_ADDR 8
#define RS2MIDI_RPC_SET_MIX      9
#define RS2MIDI_RPC_SFX_NOTE_ON  10

#define RS2MIDI_PONG 0x52533250u

#define RS2MIDI_RPC_HEADER_BYTES  64u
#define RS2MIDI_MAX_SAMPLE_BYTES  800u
#define RS2MIDI_MAX_IRX_BYTES     (128u * 1024u)

#define PS2_MIDI_ARCHIVE           2
#define PS2_MIDI_MAX_TRACKS        32
#define PS2_MIDI_VOICE_COUNT       24
#define PS2_MIDI_EVENT_BUDGET      512
#define PS2_MIDI_DEFAULT_TEMPO_US  500000u
#define PS2_MIDI_BASE_NOTE         76

#define PS2_PACK_HEADER_BYTES       40u
#define PS2_PACK_SAMPLE_REC_BYTES   8u
#define PS2_PACK_EVENT_BYTES        16u
#define PS2_PACK_EVENT_BUDGET       1024
#define PS2_PACK_MAX_SAMPLES        1024u
#define PS2_PACK_MAX_EVENTS         131072u
#define PS2_PACK_SPU_BASE           0x00100000u
#define PS2_PACK_SPU_LIMIT          0x001e0000u

#define PS2_SFX_SPU_BASE             0x001e2000u
#define PS2_SFX_SPU_LIMIT            0x00200000u
#define PS2_SFX_VOICE                23u
#define PS2_SFX_QUEUE_COUNT          16u

#define PS2_AUDIO_DAT_HEADER_BYTES   64u
#define PS2_AUDIO_DAT_ENTRY_BYTES    16u
#define PS2_AUDIO_DAT_VERSION        1u
#define PS2_AUDIO_DAT_MAX_LOOPS      256u

#define PS2_PACK_OUT_WAIT           0
#define PS2_PACK_OUT_NOTE_ON        1
#define PS2_PACK_OUT_NOTE_OFF       2
#define PS2_PACK_OUT_SUSTAIN        3
#define PS2_PACK_OUT_PITCH          4
#define PS2_PACK_OUT_MIX            5

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
    uint8_t bank_loaded;
    uint8_t playing;
    uint8_t loop_current;
    uint8_t jingle_active;
    uint8_t pending_valid;
    uint8_t pending_loop;
    uint8_t pack_probe_done;
    uint8_t track_count;
    uint8_t format;
    uint16_t division;
    int32_t active_midi_id;
    int32_t desired_song_id;
    int32_t pending_midi_id;
    uint64_t jingle_deadline_ms;
    uint8_t *midi_data;
    int midi_size;
    SifRpcClientData_t rpc;
    Ps2MidiTrack tracks[PS2_MIDI_MAX_TRACKS];
    Ps2MidiVoice voices[PS2_MIDI_VOICE_COUNT];
    uint8_t channel_program[16];
} Ps2MusicState;

typedef struct Ps2PackState {
    uint32_t magic;
    FILE *file;
    uint32_t sample_addr[PS2_PACK_MAX_SAMPLES];
    uint16_t voice_sample_index[PS2_MIDI_VOICE_COUNT];
    uint8_t voice_released[PS2_MIDI_VOICE_COUNT];
    uint8_t sustain[16];
    uint8_t current_event[PS2_PACK_EVENT_BYTES];
    uint8_t playing;
    uint8_t event_valid;
    uint16_t sample_count;
    uint32_t event_table_offset;
    uint32_t event_count;
    uint32_t event_index;
    uint32_t spu_bytes;
    uint64_t elapsed_us;
    uint64_t next_event_us;
    uint64_t last_ms;
    uint64_t duration_us;
} Ps2PackState;

typedef struct Ps2AudioDatState {
    uint32_t magic;
    uint32_t music_count;
    uint32_t sfx_count;
    uint32_t sfx_loop_slots;
    uint32_t entry_size;
    uint32_t music_table_offset;
    uint32_t sfx_table_offset;
    uint32_t data_offset;
    uint32_t file_size;
    uint8_t valid;
    uint8_t invalid;
} Ps2AudioDatState;

typedef struct Ps2SfxRequest {
    uint32_t offset;
    uint32_t size;
    uint32_t duration_ms;
    uint64_t request_ms;
    uint64_t due_ms;
    uint16_t id;
    uint16_t delay_ticks;
    uint16_t trim_ticks;
    uint8_t loops;
    uint8_t selected_loops;
    uint8_t resolved;
    uint8_t from_dat;
} Ps2SfxRequest;

typedef struct Ps2SfxState {
    uint32_t magic;
    uint32_t pitch;
    uint32_t raw_bytes;
    uint32_t last_duration_ms;
    uint64_t last_start_ms;
    uint16_t loaded_id;
    uint8_t loaded_loops;
    uint8_t loaded;
    uint8_t queue_head;
    uint8_t queue_tail;
    uint8_t queue_count;
    Ps2SfxRequest queue[PS2_SFX_QUEUE_COUNT];
} Ps2SfxState;

/*
 * Everything added by the PS2 music sequencer lives in the isolated high
 * audio PT_LOAD. Give the object a non-zero initializer so it is emitted as
 * PROGBITS in .ps2_audio_data instead of moving the hardware-sensitive normal
 * client BSS.
 */
static Ps2MusicState ps2_music_state PS2_AUDIO_STATE = {
    .magic = 0x4d555349u, /* "MUSI" */
    .init_polls = 1,
    .tempo_us = PS2_MIDI_DEFAULT_TEMPO_US,
    .active_midi_id = -1,
    .desired_song_id = -1,
    .pending_midi_id = -1
};

/*
 * Accurate-pack runtime state is separate so the hardware-proven
 * Ps2MusicState/RPC field layout remains untouched.
 */
static Ps2PackState ps2_pack_state PS2_AUDIO_STATE = {
    .magic = 0x5041434bu /* "PACK" */
};

static Ps2AudioDatState ps2_audio_dat_state PS2_AUDIO_STATE = {
    .magic = 0x44415431u /* "DAT1" */
};

static Ps2SfxState ps2_sfx_state PS2_AUDIO_STATE = {
    .magic = 0x53465831u, /* "SFX1" */
    .loaded_id = 0xffffu
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
static const char ps2_midi_bank_ready_fmt[] PS2_AUDIO_RODATA =
    "audio: rs2midi program bank slots=%u bytes=%u ready=%u\n";
static const char ps2_midi_load_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI id=%d could not be loaded from ondemand archive\n";
static const char ps2_midi_bad_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI id=%d invalid/unsupported size=%d\n";
static const char ps2_midi_start_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI id=%d size=%d format=%u tracks=%u division=%u loop=%u playing on SPU2\n";
static const char ps2_midi_event_fail_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI parse stopped on track=%u tick=%u\n";
static const char ps2_midi_loop_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI id=%d loop\n";
static const char ps2_midi_jingle_resume_fmt[] PS2_AUDIO_RODATA =
    "audio: rev254 MIDI jingle complete; resume id=%d\n";
static const char ps2_pack_path_fmt[] PS2_AUDIO_RODATA =
    "%srom/ps2audio/%d.ps2m";
static const char ps2_pack_bad_fmt[] PS2_AUDIO_RODATA =
    "audio: PS2M id=%d unavailable/invalid path=%s\n";
static const char ps2_pack_start_fmt[] PS2_AUDIO_RODATA =
    "audio: PS2M id=%d samples=%u events=%u spu=%u loop=%u accurate=1\n";
static const char ps2_pack_fallback_fmt[] PS2_AUDIO_RODATA =
    "audio: PS2M id=%d unavailable/invalid; using compact MIDI fallback\n";

static const char ps2_audio_dat_path_fmt[] PS2_AUDIO_RODATA =
    "%srom/audio.dat";
static const char ps2_audio_read_mode[] PS2_AUDIO_RODATA = "rb";
static const char ps2_audio_dat_bad_fmt[] PS2_AUDIO_RODATA =
    "audio: audio.dat unavailable/invalid path=%s\n";
static const char ps2_audio_dat_ready_fmt[] PS2_AUDIO_RODATA =
    "audio: audio.dat ready music=%u sfx=%u loopslots=%u bytes=%u\n";

static const char ps2_sfx_path_fmt[] PS2_AUDIO_RODATA =
    "%srom/ps2sfx/%d.ps2a";
static const char ps2_sfx_bad_fmt[] PS2_AUDIO_RODATA =
    "audio: SFX id=%u loops=%u unavailable/invalid\n";
static const char ps2_sfx_load_fmt[] PS2_AUDIO_RODATA =
    "audio: SFX id=%u loops=%u loaded raw=%u pitch=%u\n";
static const char ps2_sfx_play_fmt[] PS2_AUDIO_RODATA =
    "audio: SFX id=%u loops=%u play status=%d\n";
static const char ps2_sfx_queue_full_fmt[] PS2_AUDIO_RODATA =
    "audio: SFX queue full; dropping id=%u loops=%u delay=%u\n";

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

PS2_AUDIO_STATIC uint16_t ps2_pack_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

PS2_AUDIO_STATIC uint32_t ps2_pack_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

PS2_AUDIO_STATIC uint64_t ps2_pack_le64(const uint8_t *p)
{
    return (uint64_t)ps2_pack_le32(p) |
           ((uint64_t)ps2_pack_le32(p + 4) << 32);
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
    ps2_pack_state.voice_sample_index[voice] = 0xffffu;
    ps2_pack_state.voice_released[voice] = 0;
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

PS2_AUDIO_STATIC uint32_t ps2_midi_sample_slot_for_program(
    uint8_t program)
{
    /*
     * The eight-slot bank is intentionally coarse, but a few General MIDI
     * programs cannot safely share their broad 16-program family.
     *
     * Tubular Bells (14) need the dedicated bell timbre in slot 7. Reverse
     * Cymbal (119) is a one-shot/noise effect; feeding it to a looped pitched
     * wavetable creates the piercing sustained ring heard in Expanse.
     */
    if (program == 14) {
        return 7u;
    }
    if (program == 119) {
        return UINT32_MAX;
    }

    return ((uint32_t)program >> 4) & 7u;
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

    /*
     * Leave headroom for polyphonic SPU2 summing. Driving every MIDI voice
     * to the hardware maximum clips noticeably during chords/busy passages.
     */
    uint8_t program = ps2_music_state.channel_program[channel];
    uint32_t max_volume = program == 14 ? 0x1000u : 0x1800u;
    uint32_t volume =
        ((uint32_t)velocity * max_volume + 63u) / 127u;

    Rs2MidiRpcPacket packet __attribute__((aligned(64)));
    memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
    uint32_t sample_slot = 0;
    if (ps2_music_state.bank_loaded) {
        sample_slot = ps2_midi_sample_slot_for_program(program);
        if (sample_slot == UINT32_MAX) {
            return;
        }
    }

    packet.words[0] = (uint32_t)chosen;
    packet.words[1] = (uint32_t)ps2_midi_pitch_for_key(key);
    packet.words[2] = volume;
    packet.words[3] = volume;
    packet.words[4] = sample_slot;

    if (ps2_audio_rpc_status(
            &ps2_music_state.rpc,
            RS2MIDI_RPC_NOTE_ON,
            &packet,
            20) == 0) {
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
    } else if (kind == 0xc0) {
        ps2_music_state.channel_program[channel] = data1 & 0x7f;
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
    memset(ps2_music_state.channel_program, 0,
           sizeof(ps2_music_state.channel_program));
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


PS2_AUDIO_STATIC void ps2_pack_clear_stream(void)
{
    if (ps2_pack_state.file) {
        fclose(ps2_pack_state.file);
        ps2_pack_state.file = NULL;
    }

    ps2_pack_state.playing = 0;
    ps2_pack_state.event_valid = 0;
    ps2_pack_state.sample_count = 0;
    ps2_pack_state.event_table_offset = 0;
    ps2_pack_state.event_count = 0;
    ps2_pack_state.event_index = 0;
    ps2_pack_state.spu_bytes = 0;
    ps2_pack_state.elapsed_us = 0;
    ps2_pack_state.next_event_us = 0;
    ps2_pack_state.last_ms = 0;
    ps2_pack_state.duration_us = 0;
    memset(ps2_pack_state.sustain, 0, sizeof(ps2_pack_state.sustain));
    memset(ps2_pack_state.voice_sample_index, 0xff,
           sizeof(ps2_pack_state.voice_sample_index));
    memset(ps2_pack_state.voice_released, 0,
           sizeof(ps2_pack_state.voice_released));
}

PS2_AUDIO_STATIC void ps2_pack_mix_volumes(
    uint8_t volume,
    int8_t pan,
    uint32_t *left,
    uint32_t *right)
{
    uint32_t base =
        ((uint32_t)(volume > 100 ? 100 : volume) * 0x1800u + 50u) / 100u;
    int p = (int)pan;
    if (p < -100) {
        p = -100;
    }
    if (p > 100) {
        p = 100;
    }

    uint32_t l = base;
    uint32_t r = base;
    if (p > 0) {
        l = (base * (uint32_t)(100 - p) + 50u) / 100u;
    } else if (p < 0) {
        r = (base * (uint32_t)(100 + p) + 50u) / 100u;
    }

    *left = l;
    *right = r;
}

PS2_AUDIO_STATIC int ps2_pack_choose_voice(void)
{
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        if (!ps2_music_state.voices[voice].active) {
            return voice;
        }
    }

    uint32_t oldest_age = UINT32_MAX;
    int chosen = -1;
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        if (ps2_music_state.voices[voice].age < oldest_age) {
            oldest_age = ps2_music_state.voices[voice].age;
            chosen = voice;
        }
    }
    return chosen;
}

PS2_AUDIO_STATIC void ps2_pack_release_deferred(uint8_t channel)
{
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        Ps2MidiVoice *slot = &ps2_music_state.voices[voice];
        if (slot->active &&
            slot->channel == channel &&
            ps2_pack_state.voice_released[voice]) {
            ps2_midi_key_off_voice(voice);
        }
    }
}

PS2_AUDIO_STATIC void ps2_pack_note_off(uint8_t channel, uint8_t key)
{
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        Ps2MidiVoice *slot = &ps2_music_state.voices[voice];
        if (!slot->active ||
            slot->channel != channel ||
            slot->key != key) {
            continue;
        }

        if (ps2_pack_state.sustain[channel]) {
            ps2_pack_state.voice_released[voice] = 1;
        } else {
            ps2_midi_key_off_voice(voice);
        }
    }
}

PS2_AUDIO_STATIC void ps2_pack_note_on(
    uint8_t channel,
    uint8_t key,
    uint16_t sample_index,
    uint16_t pitch,
    uint8_t volume,
    int8_t pan)
{
    if (sample_index >= ps2_pack_state.sample_count) {
        return;
    }

    int chosen = ps2_pack_choose_voice();
    if (chosen < 0) {
        return;
    }

    uint32_t left = 0;
    uint32_t right = 0;
    ps2_pack_mix_volumes(volume, pan, &left, &right);

    Rs2MidiRpcPacket packet __attribute__((aligned(64)));
    memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
    packet.words[0] = (uint32_t)chosen;
    packet.words[1] = (uint32_t)pitch;
    packet.words[2] = left;
    packet.words[3] = right;
    packet.words[4] = ps2_pack_state.sample_addr[sample_index];

    if (ps2_audio_rpc_status(
            &ps2_music_state.rpc,
            RS2MIDI_RPC_NOTE_ON_ADDR,
            &packet,
            5 * (int)sizeof(uint32_t)) == 0) {
        Ps2MidiVoice *slot = &ps2_music_state.voices[chosen];
        slot->active = 1;
        slot->channel = channel;
        slot->key = key;
        slot->age = ++ps2_music_state.voice_serial;
        ps2_pack_state.voice_sample_index[chosen] = sample_index;
        ps2_pack_state.voice_released[chosen] = 0;
    }
}

PS2_AUDIO_STATIC void ps2_pack_update_pitch(
    uint8_t channel,
    uint8_t key,
    uint16_t sample_index,
    uint16_t pitch)
{
    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        Ps2MidiVoice *slot = &ps2_music_state.voices[voice];
        if (!slot->active ||
            slot->channel != channel ||
            slot->key != key ||
            ps2_pack_state.voice_sample_index[voice] != sample_index) {
            continue;
        }

        Rs2MidiRpcPacket packet __attribute__((aligned(64)));
        memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
        packet.words[0] = (uint32_t)voice;
        packet.words[1] = (uint32_t)pitch;
        (void)ps2_audio_rpc_status(
            &ps2_music_state.rpc, RS2MIDI_RPC_SET_PITCH, &packet, 8);
    }
}

PS2_AUDIO_STATIC void ps2_pack_update_mix(
    uint8_t channel,
    uint8_t key,
    uint16_t sample_index,
    uint8_t volume,
    int8_t pan)
{
    uint32_t left = 0;
    uint32_t right = 0;
    ps2_pack_mix_volumes(volume, pan, &left, &right);

    for (int voice = 0; voice < PS2_MIDI_VOICE_COUNT; voice++) {
        Ps2MidiVoice *slot = &ps2_music_state.voices[voice];
        if (!slot->active ||
            slot->channel != channel ||
            slot->key != key ||
            ps2_pack_state.voice_sample_index[voice] != sample_index) {
            continue;
        }

        Rs2MidiRpcPacket packet __attribute__((aligned(64)));
        memset(&packet, 0, RS2MIDI_RPC_HEADER_BYTES);
        packet.words[0] = (uint32_t)voice;
        packet.words[1] = left;
        packet.words[2] = right;
        (void)ps2_audio_rpc_status(
            &ps2_music_state.rpc, RS2MIDI_RPC_SET_MIX, &packet, 12);
    }
}

PS2_AUDIO_STATIC void ps2_pack_process_event(const uint8_t *event)
{
    /*
     * Keep this as explicit branches on PS2. With event kinds 0..5 GCC can
     * recognize an ordinary if/else chain as a switch and emit a six-entry
     * jump table in normal .rodata. That 24-byte leak moves the hardware-
     * sensitive .sdata/.bss addresses. Volatile forces the comparisons to
     * remain loads/branches inside the isolated audio text section.
     */
    volatile uint8_t kind = event[4];
    uint8_t channel = event[5] & 0x0fu;
    uint8_t note = event[6];
    uint8_t value = event[7];
    uint16_t sample_index = ps2_pack_le16(event + 8);
    uint16_t pitch = ps2_pack_le16(event + 10);
    uint8_t volume = event[12];
    int8_t pan = (int8_t)event[13];

    if (kind == PS2_PACK_OUT_NOTE_ON) {
        ps2_pack_note_on(
            channel, note, sample_index, pitch, volume, pan);
        return;
    }
    if (kind == PS2_PACK_OUT_NOTE_OFF) {
        ps2_pack_note_off(channel, note);
        return;
    }
    if (kind == PS2_PACK_OUT_SUSTAIN) {
        uint8_t was = ps2_pack_state.sustain[channel];
        ps2_pack_state.sustain[channel] = value ? 1 : 0;
        if (was && !ps2_pack_state.sustain[channel]) {
            ps2_pack_release_deferred(channel);
        }
        return;
    }
    if (kind == PS2_PACK_OUT_PITCH) {
        ps2_pack_update_pitch(channel, note, sample_index, pitch);
        return;
    }
    if (kind == PS2_PACK_OUT_MIX) {
        ps2_pack_update_mix(
            channel, note, sample_index, volume, pan);
    }
}

PS2_AUDIO_STATIC bool ps2_pack_reset_timeline(void)
{
    if (!ps2_pack_state.file ||
        ps2_pack_state.event_count == 0 ||
        ps2_pack_state.event_table_offset > 0x7fffffffu) {
        return false;
    }

    ps2_midi_all_notes_off();
    memset(ps2_pack_state.sustain, 0, sizeof(ps2_pack_state.sustain));
    memset(ps2_pack_state.voice_sample_index, 0xff,
           sizeof(ps2_pack_state.voice_sample_index));
    memset(ps2_pack_state.voice_released, 0,
           sizeof(ps2_pack_state.voice_released));

    if (fseek(ps2_pack_state.file,
              (long)ps2_pack_state.event_table_offset,
              SEEK_SET) != 0 ||
        fread(ps2_pack_state.current_event,
              1,
              PS2_PACK_EVENT_BYTES,
              ps2_pack_state.file) != PS2_PACK_EVENT_BYTES) {
        ps2_pack_state.event_valid = 0;
        return false;
    }

    ps2_pack_state.event_index = 0;
    ps2_pack_state.elapsed_us = 0;
    ps2_pack_state.next_event_us =
        (uint64_t)ps2_pack_le32(ps2_pack_state.current_event);
    ps2_pack_state.last_ms = rs2_now();
    ps2_pack_state.event_valid = 1;
    return true;
}

PS2_AUDIO_STATIC int ps2_pack_start_id(int id, bool loop)
{
    char path[320];
    snprintf(path, sizeof(path), ps2_pack_path_fmt, ps2_cache_prefix(), id);

    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }

    uint8_t header[PS2_PACK_HEADER_BYTES];
    size_t got = fread(header, 1, sizeof(header), file);
    if (got != sizeof(header) ||
        header[0] != 'R' ||
        header[1] != 'S' ||
        header[2] != 'M' ||
        header[3] != '1' ||
        ps2_pack_le16(header + 4) != 1u ||
        ps2_pack_le16(header + 6) != PS2_PACK_HEADER_BYTES) {
        rs2_log(ps2_pack_bad_fmt, id, path);
        fclose(file);
        return -1;
    }

    uint32_t sample_count = ps2_pack_le32(header + 8);
    uint32_t event_count = ps2_pack_le32(header + 12);
    uint32_t sample_table_offset = ps2_pack_le32(header + 16);
    uint32_t event_table_offset = ps2_pack_le32(header + 20);
    uint32_t data_offset = ps2_pack_le32(header + 24);
    uint32_t data_size = ps2_pack_le32(header + 28);
    uint64_t duration_us = ps2_pack_le64(header + 32);

    int seek_end = fseek(file, 0, SEEK_END);
    long file_size_long = seek_end == 0 ? ftell(file) : -1;
    if (file_size_long <= 0 ||
        sample_count == 0 ||
        sample_count > PS2_PACK_MAX_SAMPLES ||
        event_count == 0 ||
        event_count > PS2_PACK_MAX_EVENTS ||
        sample_table_offset < PS2_PACK_HEADER_BYTES ||
        sample_count > (UINT32_MAX - sample_table_offset) /
                           PS2_PACK_SAMPLE_REC_BYTES ||
        event_count > (UINT32_MAX - event_table_offset) /
                          PS2_PACK_EVENT_BYTES) {
        rs2_log(ps2_pack_bad_fmt, id, path);
        fclose(file);
        return -1;
    }

    uint32_t file_size = (uint32_t)file_size_long;
    uint32_t sample_table_end =
        sample_table_offset + sample_count * PS2_PACK_SAMPLE_REC_BYTES;
    uint32_t event_table_end =
        event_table_offset + event_count * PS2_PACK_EVENT_BYTES;

    if (event_table_offset < sample_table_end ||
        data_offset < event_table_end ||
        sample_table_end > file_size ||
        event_table_end > file_size ||
        data_offset > file_size ||
        data_size > file_size - data_offset) {
        rs2_log(ps2_pack_bad_fmt, id, path);
        fclose(file);
        return -1;
    }

    uint32_t spu_cursor = PS2_PACK_SPU_BASE;
    bool upload_ok = true;
    Rs2MidiRpcPacket packet __attribute__((aligned(64)));

    for (uint32_t i = 0; i < sample_count; i++) {
        uint8_t rec[PS2_PACK_SAMPLE_REC_BYTES];
        uint32_t rec_pos =
            sample_table_offset + i * PS2_PACK_SAMPLE_REC_BYTES;

        if (rec_pos > 0x7fffffffu ||
            fseek(file, (long)rec_pos, SEEK_SET) != 0 ||
            fread(rec, 1, sizeof(rec), file) != sizeof(rec)) {
            upload_ok = false;
            break;
        }

        uint32_t blob_offset = ps2_pack_le32(rec);
        uint32_t blob_size = ps2_pack_le32(rec + 4);
        if (blob_size <= 16u ||
            blob_offset < data_offset ||
            blob_offset > file_size ||
            blob_size > file_size - blob_offset) {
            upload_ok = false;
            break;
        }

        uint32_t relative_offset = blob_offset - data_offset;
        if (relative_offset > data_size ||
            blob_size > data_size - relative_offset) {
            upload_ok = false;
            break;
        }

        uint32_t raw_size = blob_size - 16u;
        if (raw_size > UINT32_MAX - 63u) {
            upload_ok = false;
            break;
        }
        uint32_t padded_size = (raw_size + 63u) & ~63u;
        if (spu_cursor > PS2_PACK_SPU_LIMIT ||
            padded_size > PS2_PACK_SPU_LIMIT - spu_cursor ||
            blob_offset > 0x7fffffffu) {
            upload_ok = false;
            break;
        }

        ps2_pack_state.sample_addr[i] = spu_cursor;

        if (fseek(file, (long)blob_offset, SEEK_SET) != 0) {
            upload_ok = false;
            break;
        }

        uint8_t apcm_header[16];
        if (fread(apcm_header, 1, sizeof(apcm_header), file) !=
                sizeof(apcm_header) ||
            apcm_header[0] != 'A' ||
            apcm_header[1] != 'P' ||
            apcm_header[2] != 'C' ||
            apcm_header[3] != 'M') {
            upload_ok = false;
            break;
        }

        uint32_t remaining = raw_size;
        uint32_t dest = spu_cursor;
        while (remaining != 0u) {
            uint32_t read_size = remaining > 64u ? 64u : remaining;

            memset(&packet, 0, sizeof(packet));
            if (fread(packet.sample, 1, read_size, file) != read_size) {
                upload_ok = false;
                break;
            }

            packet.words[0] = dest;
            packet.words[1] = 64u;
            if (ps2_audio_rpc_status(
                    &ps2_music_state.rpc,
                    RS2MIDI_RPC_LOAD_ABS,
                    &packet,
                    RS2MIDI_RPC_HEADER_BYTES + 64u) < 0) {
                upload_ok = false;
                break;
            }

            remaining -= read_size;
            dest += 64u;
        }

        if (!upload_ok) {
            break;
        }

        spu_cursor += padded_size;
    }

    if (!upload_ok) {
        rs2_log(ps2_pack_bad_fmt, id, path);
        fclose(file);
        return -1;
    }

    ps2_pack_state.file = file;
    ps2_pack_state.sample_count = (uint16_t)sample_count;
    ps2_pack_state.event_table_offset = event_table_offset;
    ps2_pack_state.event_count = event_count;
    ps2_pack_state.duration_us = duration_us;
    ps2_pack_state.spu_bytes = spu_cursor - PS2_PACK_SPU_BASE;

    if (!ps2_pack_reset_timeline()) {
        rs2_log(ps2_pack_bad_fmt, id, path);
        ps2_pack_clear_stream();
        return -1;
    }

    ps2_pack_state.playing = 1;
    ps2_music_state.playing = 1;
    ps2_music_state.loop_current = loop ? 1 : 0;
    ps2_music_state.active_midi_id = id;

    rs2_log(ps2_pack_start_fmt,
            id,
            (unsigned int)sample_count,
            (unsigned int)event_count,
            (unsigned int)ps2_pack_state.spu_bytes,
            (unsigned int)ps2_music_state.loop_current);
    return 1;
}

PS2_AUDIO_STATIC void ps2_pack_finish_song(void)
{
    ps2_midi_all_notes_off();
    ps2_pack_clear_stream();
    ps2_music_state.playing = 0;
    ps2_music_state.track_count = 0;
    ps2_music_state.tick_fp = 0;
    ps2_music_state.active_midi_id = -1;
    ps2_music_state.loop_current = 0;
}

PS2_AUDIO_STATIC void ps2_pack_update_song(void)
{
    if (!ps2_pack_state.playing ||
        !ps2_pack_state.file ||
        !ps2_pack_state.event_valid) {
        return;
    }

    uint64_t now = rs2_now();
    uint64_t elapsed_ms = now - ps2_pack_state.last_ms;
    ps2_pack_state.last_ms = now;
    if (elapsed_ms > 250u) {
        elapsed_ms = 250u;
    }
    ps2_pack_state.elapsed_us += elapsed_ms * 1000ull;

    int events = 0;
    while (events < PS2_PACK_EVENT_BUDGET &&
           ps2_pack_state.event_valid &&
           ps2_pack_state.event_index < ps2_pack_state.event_count &&
           ps2_pack_state.next_event_us <= ps2_pack_state.elapsed_us) {
        ps2_pack_process_event(ps2_pack_state.current_event);
        ps2_pack_state.event_index++;
        events++;

        if (ps2_pack_state.event_index < ps2_pack_state.event_count) {
            if (fread(ps2_pack_state.current_event,
                      1,
                      PS2_PACK_EVENT_BYTES,
                      ps2_pack_state.file) != PS2_PACK_EVENT_BYTES) {
                ps2_pack_finish_song();
                return;
            }
            ps2_pack_state.next_event_us +=
                (uint64_t)ps2_pack_le32(ps2_pack_state.current_event);
        } else {
            ps2_pack_state.event_valid = 0;
        }
    }

    if (ps2_pack_state.event_index >= ps2_pack_state.event_count &&
        ps2_pack_state.elapsed_us >= ps2_pack_state.duration_us) {
        if (ps2_music_state.loop_current) {
            if (!ps2_pack_reset_timeline()) {
                ps2_pack_finish_song();
            }
        } else {
            ps2_pack_finish_song();
        }
    }
}

PS2_AUDIO_STATIC void ps2_midi_stop_song(void)
{
    ps2_midi_all_notes_off();
    ps2_pack_clear_stream();

    if (ps2_music_state.midi_data) {
        free(ps2_music_state.midi_data);
        ps2_music_state.midi_data = NULL;
    }

    ps2_music_state.midi_size = 0;
    ps2_music_state.playing = 0;
    ps2_music_state.track_count = 0;
    ps2_music_state.tick_fp = 0;
    ps2_music_state.active_midi_id = -1;
    ps2_music_state.loop_current = 0;
}

PS2_AUDIO_STATIC bool ps2_midi_start_id(int id, bool loop)
{
    if (!ps2_music_state.ready) {
        return false;
    }

    ps2_midi_stop_song();
    if (id < 0) {
        return true;
    }

    int pack_status = ps2_pack_start_id(id, loop);
    if (pack_status > 0) {
        return true;
    }

    rs2_log(ps2_pack_fallback_fmt, id);

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
    ps2_music_state.loop_current = loop ? 1 : 0;
    ps2_music_state.active_midi_id = id;
    rs2_log(ps2_midi_start_fmt,
            id,
            midi_size,
            (unsigned int)ps2_music_state.format,
            (unsigned int)ps2_music_state.track_count,
            (unsigned int)ps2_music_state.division,
            (unsigned int)ps2_music_state.loop_current);
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
        if (ps2_music_state.loop_current && ps2_midi_reset_tracks()) {
            rs2_log(ps2_midi_loop_fmt, ps2_music_state.active_midi_id);
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

    bool bank_ok = true;
    for (uint32_t slot = 0; slot < PS2_MIDI_BANK_SAMPLE_COUNT; slot++) {
        memset(&packet, 0, sizeof(packet));
        packet.words[0] = slot;
        packet.words[1] = PS2_MIDI_BANK_SAMPLE_BYTES;
        memcpy(packet.sample,
               ps2_midi_bank_adpcm[slot],
               PS2_MIDI_BANK_SAMPLE_BYTES);

        int32_t bank_status = ps2_audio_rpc_status(
            &ps2_music_state.rpc,
            RS2MIDI_RPC_LOAD_SLOT,
            &packet,
            RS2MIDI_RPC_HEADER_BYTES + PS2_MIDI_BANK_SAMPLE_BYTES);
        if (bank_status < 0) {
            bank_ok = false;
            break;
        }
    }

    if (bank_ok) {
        ps2_music_state.bank_loaded = 1;
        ps2_music_state.base_pitch = ps2_midi_bank_base_pitch;
    } else {
        /*
         * Restore the already-proven one-sample path if multi-slot loading is
         * rejected on hardware. That keeps music audible for diagnosis.
         */
        memset(&packet, 0, sizeof(packet));
        packet.words[0] = raw_size;
        memcpy(packet.sample, ps2_audio_test_adpcm + 16, raw_size);
        (void)ps2_audio_rpc_status(
            &ps2_music_state.rpc,
            RS2MIDI_RPC_LOAD,
            &packet,
            RS2MIDI_RPC_HEADER_BYTES + raw_size);
        ps2_music_state.bank_loaded = 0;
    }

    rs2_log(ps2_midi_bank_ready_fmt,
            (unsigned int)PS2_MIDI_BANK_SAMPLE_COUNT,
            (unsigned int)PS2_MIDI_BANK_SAMPLE_BYTES,
            (unsigned int)ps2_music_state.bank_loaded);

    ps2_music_state.ready = 1;
    return true;
}


PS2_AUDIO_STATIC bool ps2_sfx_load_anvil(void)
{
    if (ps2_sfx_state.loaded) {
        return true;
    }
    if (ps2_sfx_state.failed || !ps2_music_state.ready) {
        return false;
    }

    char path[320];
    snprintf(path, sizeof(path), ps2_sfx_path_fmt, ps2_cache_prefix());

    FILE *file = fopen(path, ps2_sfx_read_mode);
    if (!file) {
        rs2_log(ps2_sfx_open_fail_fmt, path);
        ps2_sfx_state.failed = 1;
        return false;
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
        size_long > (long)(PS2_SFX_SPU_LIMIT - PS2_SFX_SPU_BASE + 16u)) {
        rs2_log(ps2_sfx_bad_fmt, path, size_long);
        fclose(file);
        ps2_sfx_state.failed = 1;
        return false;
    }

    uint8_t header[16];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 'A' ||
        header[1] != 'P' ||
        header[2] != 'C' ||
        header[3] != 'M' ||
        header[4] != 1u ||
        header[5] != 1u ||
        header[6] != 0u) {
        rs2_log(ps2_sfx_bad_fmt, path, size_long);
        fclose(file);
        ps2_sfx_state.failed = 1;
        return false;
    }

    uint32_t pitch = ps2_pack_le32(header + 8);
    uint32_t raw_size = (uint32_t)size_long - 16u;
    uint32_t padded_size = (raw_size + 63u) & ~63u;
    if (pitch == 0u ||
        pitch > 0x3fffu ||
        padded_size == 0u ||
        padded_size > PS2_SFX_SPU_LIMIT - PS2_SFX_SPU_BASE) {
        rs2_log(ps2_sfx_bad_fmt, path, size_long);
        fclose(file);
        ps2_sfx_state.failed = 1;
        return false;
    }

    uint32_t remaining = raw_size;
    uint32_t dest = PS2_SFX_SPU_BASE;
    Rs2MidiRpcPacket packet __attribute__((aligned(64)));

    while (remaining != 0u) {
        uint32_t read_size = remaining > 64u ? 64u : remaining;
        memset(&packet, 0, sizeof(packet));
        if (fread(packet.sample, 1, read_size, file) != read_size) {
            fclose(file);
            ps2_sfx_state.failed = 1;
            return false;
        }

        packet.words[0] = dest;
        packet.words[1] = 64u;
        if (ps2_audio_rpc_status(
                &ps2_music_state.rpc,
                RS2MIDI_RPC_LOAD_ABS,
                &packet,
                RS2MIDI_RPC_HEADER_BYTES + 64u) < 0) {
            fclose(file);
            ps2_sfx_state.failed = 1;
            return false;
        }

        remaining -= read_size;
        dest += 64u;
    }

    fclose(file);
    ps2_sfx_state.pitch = pitch;
    ps2_sfx_state.raw_bytes = raw_size;
    ps2_sfx_state.loaded = 1;

    rs2_log(ps2_sfx_load_fmt,
            (unsigned int)raw_size,
            (unsigned int)pitch);
    return true;
}

void ps2_sfx_request(int id, int loops, int delay) PS2_AUDIO_CODE;
void ps2_sfx_request(int id, int loops, int delay)
{
    (void)loops;

    if (id != PS2_SFX_PROOF_ID ||
        delay != 0 ||
        !ps2_music_state.ready ||
        !ps2_sfx_load_anvil()) {
        return;
    }

    Rs2MidiRpcPacket packet __attribute__((aligned(64)));
    memset(&packet, 0, sizeof(packet));
    packet.words[0] = PS2_SFX_VOICE;
    packet.words[1] = ps2_sfx_state.pitch;
    packet.words[2] = 0x3fffu;
    packet.words[3] = 0x3fffu;
    packet.words[4] = PS2_SFX_SPU_BASE;

    int32_t status = ps2_audio_rpc_status(
        &ps2_music_state.rpc,
        RS2MIDI_RPC_SFX_NOTE_ON,
        &packet,
        RS2MIDI_RPC_HEADER_BYTES);
    rs2_log(ps2_sfx_play_fmt, (int)status);
}


void ps2_audio_update_late(void) PS2_AUDIO_CODE;
void ps2_audio_update_late(void)
{
    Client *c = ps2_crash_client;

    /*
     * The companion still never loads before the client is fully in-world.
     * This preserves the network-safe boot/login sequence proven on hardware.
     */
    if (!c || !c->ingame) {
        if (ps2_music_state.playing) {
            ps2_midi_stop_song();
        }
        ps2_music_state.jingle_active = 0;
        ps2_music_state.pending_valid = 0;
        ps2_music_state.pack_probe_done = 0;
        ps2_music_state.desired_song_id = -1;
        if (!ps2_music_state.ready) {
            ps2_music_state.init_polls = 1;
        }
        return;
    }

    /*
     * Keep server MIDI requests queued across REBUILD_NORMAL. A MIDI_SONG can
     * arrive while scene_state is still 1; dropping it here would leave the
     * player silent until the server happens to send another song change.
     */
    if (c->scene_state != 2) {
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
     * MIDI_JINGLE carries its duration in milliseconds. While it is active,
     * MIDI_SONG packets only update desired_song_id. At the deadline, resume
     * the newest area song exactly like the rev254 reference client.
     */
    if (ps2_music_state.jingle_active &&
        rs2_now() >= ps2_music_state.jingle_deadline_ms) {
        ps2_music_state.jingle_active = 0;
        ps2_music_state.pending_midi_id = ps2_music_state.desired_song_id;
        ps2_music_state.pending_loop = 1;
        ps2_music_state.pending_valid = 1;
        rs2_log(ps2_midi_jingle_resume_fmt, ps2_music_state.desired_song_id);
    }

    if (ps2_music_state.pending_valid) {
        int id = ps2_music_state.pending_midi_id;
        bool loop = ps2_music_state.pending_loop != 0;
        ps2_music_state.pending_valid = 0;
        (void)ps2_midi_start_id(id, loop);
    }

    if (ps2_pack_state.playing) {
        ps2_pack_update_song();
    } else {
        ps2_midi_update_song();
    }
}

/*
 * Server-driven rev254 request ingress. This function itself is placed in the
 * high audio overlay so the packet handler only pays for a tiny call site in
 * normal .text and adds no normal data/BSS.
 */
void ps2_music_request(int id, int jingle_delay_ms) PS2_AUDIO_CODE;
void ps2_music_request(int id, int jingle_delay_ms)
{
    if (id == 65535) {
        id = -1;
    }

    if (jingle_delay_ms < 0) {
        bool changed = id != ps2_music_state.desired_song_id;
        ps2_music_state.desired_song_id = id;

        /*
         * Match rev254: an area-song packet received during a jingle updates
         * the song that will resume, but does not interrupt the jingle.
         */
        if (!ps2_music_state.jingle_active &&
            (changed || ps2_music_state.active_midi_id != id)) {
            ps2_music_state.pending_midi_id = id;
            ps2_music_state.pending_loop = 1;
            ps2_music_state.pending_valid = 1;
        }
        return;
    }

    ps2_music_state.jingle_active = 1;
    ps2_music_state.jingle_deadline_ms =
        rs2_now() + (uint64_t)(uint32_t)jingle_delay_ms;
    ps2_music_state.pending_midi_id = id;
    ps2_music_state.pending_loop = 0;
    ps2_music_state.pending_valid = 1;
}

/*
 * The legacy name/CRC entry points stay present for the rest of the platform
 * abstraction, but rev254 PS2 song selection now enters through
 * ps2_music_request() from packet 163/242.
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
