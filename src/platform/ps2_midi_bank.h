#pragma once

#define PS2_MIDI_BANK_SAMPLE_COUNT 8
#define PS2_MIDI_BANK_SAMPLE_BYTES 720

extern const unsigned char
    ps2_midi_bank_adpcm[PS2_MIDI_BANK_SAMPLE_COUNT][PS2_MIDI_BANK_SAMPLE_BYTES];
extern const unsigned int ps2_midi_bank_base_pitch;
