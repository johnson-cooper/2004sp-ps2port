#ifdef __PS2__
#undef client
#include <audsrv.h>
#include <kernel.h>
#include <malloc.h>
#include <sifcmd.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform.h"
#include "ps2_music.h"

#define PS2_MUSIC_MAGIC "RSM1"
#define PS2_MUSIC_VERSION 1
#define PS2_MUSIC_MAX_SAMPLES 1024
#define PS2_MUSIC_MAX_EVENTS 200000
#define PS2_MUSIC_VOICES 24
#define PS2_MUSIC_MAX_EVENTS_PER_UPDATE 256

enum {
    PS2_MUSIC_WAIT = 0,
    PS2_MUSIC_NOTE_ON = 1,
    PS2_MUSIC_NOTE_OFF = 2,
    PS2_MUSIC_SUSTAIN = 3,
    PS2_MUSIC_PITCH = 4,
    PS2_MUSIC_MIX = 5,
};

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t header_size;
    uint32_t sample_count;
    uint32_t event_count;
    uint32_t sample_table_offset;
    uint32_t event_table_offset;
    uint32_t data_offset;
    uint32_t data_size;
    uint64_t duration_us;
} Ps2MusicHeader;

typedef struct {
    uint32_t file_offset;
    uint32_t size;
} Ps2MusicSampleRecord;

typedef struct __attribute__((packed)) {
    uint32_t delta_us;
    uint8_t kind;
    uint8_t channel;
    uint8_t note;
    uint8_t value;
    uint16_t sample_index;
    uint16_t pitch;
    uint8_t volume;
    int8_t pan;
    uint16_t aux;
} Ps2MusicEvent;

typedef struct {
    bool active;
    bool released;
    uint8_t channel;
    uint8_t note;
    uint16_t sample_index;
    uint8_t volume;
    int8_t pan;
    uint64_t serial;
} Ps2MusicVoice;

static audsrv_adpcm_t *music_samples;
static Ps2MusicEvent *music_events;
static uint32_t music_sample_count;
static uint32_t music_event_count;
static uint32_t music_event_index;
static uint64_t music_next_event_us;
static uint64_t music_duration_us;
static uint64_t music_started_ms;
static uint64_t music_voice_serial;
static float music_volume = 1.0f;
static bool music_playing;
static bool music_sustain[16];
static Ps2MusicVoice music_voices[PS2_MUSIC_VOICES];

static void ps2_music_clear_voices(void) {
    memset(music_sustain, 0, sizeof(music_sustain));
    memset(music_voices, 0, sizeof(music_voices));
}

static int ps2_music_scaled_volume(int volume) {
    int scaled = (int)((float)volume * music_volume + 0.5f);
    if (scaled < 0) return 0;
    if (scaled > 100) return 100;
    return scaled;
}

static void ps2_music_key_off_voice(int voice) {
    if (voice < 0 || voice >= PS2_MUSIC_VOICES || !music_voices[voice].active) return;
    audsrv_rs2_key_off(voice);
    music_voices[voice].active = false;
    music_voices[voice].released = false;
}

static void ps2_music_key_off_all(void) {
    for (int voice = 0; voice < PS2_MUSIC_VOICES; voice++) ps2_music_key_off_voice(voice);
    memset(music_sustain, 0, sizeof(music_sustain));
}

static void ps2_music_release_pack(void) {
    free(music_samples);
    free(music_events);
    music_samples = NULL;
    music_events = NULL;
    music_sample_count = 0;
    music_event_count = 0;
    music_event_index = 0;
    music_next_event_us = 0;
    music_duration_us = 0;
}

static int ps2_music_pick_voice(void) {
    for (int voice = 0; voice < PS2_MUSIC_VOICES; voice++) {
        if (!music_voices[voice].active) return voice;
    }

    int oldest = 0;
    for (int voice = 1; voice < PS2_MUSIC_VOICES; voice++) {
        if (music_voices[voice].serial < music_voices[oldest].serial) oldest = voice;
    }
    return oldest;
}

static void ps2_music_note_on(const Ps2MusicEvent *event) {
    if (event->sample_index >= music_sample_count) return;

    int voice = ps2_music_pick_voice();
    if (music_voices[voice].active) audsrv_rs2_key_off(voice);

    // Set volume before KON so a stolen voice cannot begin at audsrv's default max volume.
    audsrv_adpcm_set_volume_and_pan(
        voice, ps2_music_scaled_volume(event->volume), event->pan);

    int played = audsrv_rs2_ch_play_adpcm(
        voice, &music_samples[event->sample_index], event->pitch);
    if (played < 0) {
        rs2_log("audio: music note-on failed voice=%d sample=%u rc=%d\n",
                voice, event->sample_index, played);
        music_voices[voice].active = false;
        return;
    }

    Ps2MusicVoice *v = &music_voices[voice];
    v->active = true;
    v->released = false;
    v->channel = event->channel;
    v->note = event->note;
    v->sample_index = event->sample_index;
    v->volume = event->volume;
    v->pan = event->pan;
    v->serial = ++music_voice_serial;
}

static void ps2_music_note_off(const Ps2MusicEvent *event) {
    if (event->channel >= 16) return;
    for (int voice = 0; voice < PS2_MUSIC_VOICES; voice++) {
        Ps2MusicVoice *v = &music_voices[voice];
        if (!v->active || v->channel != event->channel || v->note != event->note) continue;
        if (music_sustain[event->channel]) v->released = true;
        else ps2_music_key_off_voice(voice);
    }
}

static void ps2_music_set_sustain(const Ps2MusicEvent *event) {
    if (event->channel >= 16) return;
    bool enabled = event->value != 0;
    bool was_enabled = music_sustain[event->channel];
    music_sustain[event->channel] = enabled;
    if (!was_enabled || enabled) return;

    for (int voice = 0; voice < PS2_MUSIC_VOICES; voice++) {
        Ps2MusicVoice *v = &music_voices[voice];
        if (v->active && v->channel == event->channel && v->released) {
            ps2_music_key_off_voice(voice);
        }
    }
}

static void ps2_music_set_pitch(const Ps2MusicEvent *event) {
    for (int voice = 0; voice < PS2_MUSIC_VOICES; voice++) {
        Ps2MusicVoice *v = &music_voices[voice];
        if (v->active && v->channel == event->channel && v->note == event->note &&
            v->sample_index == event->sample_index) {
            audsrv_rs2_set_pitch(voice, event->pitch);
        }
    }
}

static void ps2_music_set_mix(const Ps2MusicEvent *event) {
    for (int voice = 0; voice < PS2_MUSIC_VOICES; voice++) {
        Ps2MusicVoice *v = &music_voices[voice];
        if (v->active && v->channel == event->channel && v->note == event->note &&
            v->sample_index == event->sample_index) {
            v->volume = event->volume;
            v->pan = event->pan;
            audsrv_adpcm_set_volume_and_pan(
                voice, ps2_music_scaled_volume(v->volume), v->pan);
        }
    }
}

static void ps2_music_dispatch(const Ps2MusicEvent *event) {
    switch (event->kind) {
        case PS2_MUSIC_NOTE_ON: ps2_music_note_on(event); break;
        case PS2_MUSIC_NOTE_OFF: ps2_music_note_off(event); break;
        case PS2_MUSIC_SUSTAIN: ps2_music_set_sustain(event); break;
        case PS2_MUSIC_PITCH: ps2_music_set_pitch(event); break;
        case PS2_MUSIC_MIX: ps2_music_set_mix(event); break;
        case PS2_MUSIC_WAIT:
        default: break;
    }
}

static bool ps2_music_validate_header(const Ps2MusicHeader *header, long file_size) {
    if (memcmp(header->magic, PS2_MUSIC_MAGIC, 4) != 0 ||
        header->version != PS2_MUSIC_VERSION ||
        header->header_size != sizeof(Ps2MusicHeader)) return false;

    if (header->sample_count == 0 || header->sample_count > PS2_MUSIC_MAX_SAMPLES ||
        header->event_count == 0 || header->event_count > PS2_MUSIC_MAX_EVENTS) return false;

    uint64_t sample_table_end = (uint64_t)header->sample_table_offset +
        (uint64_t)header->sample_count * sizeof(Ps2MusicSampleRecord);
    uint64_t event_table_end = (uint64_t)header->event_table_offset +
        (uint64_t)header->event_count * sizeof(Ps2MusicEvent);
    uint64_t data_end = (uint64_t)header->data_offset + (uint64_t)header->data_size;

    return header->sample_table_offset >= header->header_size &&
           header->event_table_offset >= sample_table_end &&
           header->data_offset >= event_table_end &&
           data_end <= (uint64_t)file_size;
}

bool ps2_music_play(const char *name) {
    if (!name || !name[0]) return false;

    char path[384];
    snprintf(path, sizeof(path), "%srom/ps2audio/%s.ps2m", ps2_cache_prefix(), name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        rs2_log("audio: PS2 music pack missing: %s\n", path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    Ps2MusicHeader header;
    if (file_size <= 0 || fread(&header, sizeof(header), 1, file) != 1 ||
        !ps2_music_validate_header(&header, file_size)) {
        rs2_log("audio: invalid PS2 music pack: %s\n", path);
        fclose(file);
        return false;
    }

    Ps2MusicSampleRecord *records = calloc(header.sample_count, sizeof(*records));
    audsrv_adpcm_t *samples = calloc(header.sample_count, sizeof(*samples));
    Ps2MusicEvent *events = malloc((size_t)header.event_count * sizeof(*events));
    if (!records || !samples || !events) {
        rs2_log("audio: title music EE allocation failed samples=%u events=%u\n",
                header.sample_count, header.event_count);
        free(records); free(samples); free(events); fclose(file);
        return false;
    }

    if (fseek(file, (long)header.sample_table_offset, SEEK_SET) != 0 ||
        fread(records, sizeof(*records), header.sample_count, file) != header.sample_count ||
        fseek(file, (long)header.event_table_offset, SEEK_SET) != 0 ||
        fread(events, sizeof(*events), header.event_count, file) != header.event_count) {
        rs2_log("audio: truncated PS2 music tables: %s\n", path);
        free(records); free(samples); free(events); fclose(file);
        return false;
    }

    // Milestone 2 owns the ADPCM heap exclusively; RuneScape SFX stay disabled.
    ps2_music_stop();
    ps2_music_release_pack();
    audsrv_adpcm_init();
    ps2_music_clear_voices();

    uint64_t load_t0 = rs2_now();
    bool load_ok = true;
    for (uint32_t i = 0; i < header.sample_count; i++) {
        const Ps2MusicSampleRecord *record = &records[i];
        uint64_t end = (uint64_t)record->file_offset + record->size;
        if (record->size < 32 || record->file_offset < header.data_offset ||
            end > (uint64_t)file_size) {
            rs2_log("audio: invalid title sample record %u off=%u size=%u\n",
                    i, record->file_offset, record->size);
            load_ok = false;
            break;
        }

        void *buffer = memalign(64, record->size);
        if (!buffer) {
            rs2_log("audio: title sample temp allocation failed index=%u size=%u\n",
                    i, record->size);
            load_ok = false;
            break;
        }

        if (fseek(file, (long)record->file_offset, SEEK_SET) != 0 ||
            fread(buffer, 1, record->size, file) != record->size) {
            free(buffer);
            rs2_log("audio: title sample read failed index=%u\n", i);
            load_ok = false;
            break;
        }

        sceSifWriteBackDCache(buffer, record->size);
        int rc = audsrv_load_adpcm(&samples[i], buffer, (int)record->size);
        free(buffer);
        samples[i].buffer = NULL;
        if (rc != AUDSRV_ERR_NOERROR) {
            rs2_log("audio: title sample upload failed index=%u rc=%d\n", i, rc);
            load_ok = false;
            break;
        }
    }

    fclose(file);
    free(records);

    if (!load_ok) {
        audsrv_adpcm_init();
        free(samples);
        free(events);
        return false;
    }

    music_samples = samples;
    music_events = events;
    music_sample_count = header.sample_count;
    music_event_count = header.event_count;
    music_event_index = 0;
    music_duration_us = header.duration_us;
    music_next_event_us = music_events[0].delta_us;
    music_started_ms = rs2_now();
    music_playing = true;

    rs2_log("audio: title music loaded name=%s samples=%u events=%u loadms=%llu durationms=%llu\n",
            name, music_sample_count, music_event_count,
            (unsigned long long)(music_started_ms - load_t0),
            (unsigned long long)(music_duration_us / 1000));
    return true;
}

void ps2_music_stop(void) {
    ps2_music_key_off_all();
    music_playing = false;
    music_event_index = 0;
    music_next_event_us = 0;
}

void ps2_music_update(void) {
    if (!music_playing || !music_events || music_event_index >= music_event_count) return;

    uint64_t elapsed_us = (rs2_now() - music_started_ms) * 1000ULL;
    int processed = 0;
    while (music_event_index < music_event_count &&
           elapsed_us >= music_next_event_us &&
           processed < PS2_MUSIC_MAX_EVENTS_PER_UPDATE) {
        ps2_music_dispatch(&music_events[music_event_index]);
        music_event_index++;
        processed++;
        if (music_event_index < music_event_count) {
            music_next_event_us += music_events[music_event_index].delta_us;
        }
    }

    if (music_event_index >= music_event_count && elapsed_us >= music_duration_us) {
        ps2_music_stop();
    }
}

void ps2_music_set_volume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    music_volume = volume;

    for (int voice = 0; voice < PS2_MUSIC_VOICES; voice++) {
        if (music_voices[voice].active) {
            audsrv_adpcm_set_volume_and_pan(
                voice, ps2_music_scaled_volume(music_voices[voice].volume),
                music_voices[voice].pan);
        }
    }
}

void ps2_music_shutdown(void) {
    ps2_music_stop();
    ps2_music_release_pack();
}
#endif
