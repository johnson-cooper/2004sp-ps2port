#!/usr/bin/env python3
"""Build a PS2-ready title-music pack from RuneScape's SF2 + packed MIDI.

The output contains only the SoundFont sample regions actually used by the
selected song. MIDI parsing, tempo resolution, SoundFont zone selection,
pitch math and ADPCM encoding all happen on the host.
"""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path

from midi_to_ps2seq import (
    EV_CONTROL,
    EV_NOTE_OFF,
    EV_NOTE_ON,
    EV_PITCH_BEND,
    EV_PROGRAM,
    events_with_time,
)
from ps2_adpcm import SAMPLES_PER_FRAME, base_pitch, encode_mono_pcm16
from sf2_to_ps2bank import (
    BAG,
    GEN,
    INST,
    PHDR,
    SHDR,
    _find_sf2_sections,
    _name,
    _records,
)

MAGIC = b"RSM1"
VERSION = 1
HEADER = struct.Struct("<4sHHIIIIIIQ")
SAMPLE_REC = struct.Struct("<II")
EVENT_REC = struct.Struct("<IBBBBHHBbH")

OUT_WAIT = 0
OUT_NOTE_ON = 1
OUT_NOTE_OFF = 2
OUT_SUSTAIN = 3
OUT_PITCH = 4
OUT_MIX = 5

GM_PROGRAM_NAMES = (
    "Acoustic Grand Piano", "Bright Acoustic Piano",
    "Electric Grand Piano", "Honky-tonk Piano",
    "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
    "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)",
    "Electric Guitar (jazz)", "Electric Guitar (clean)",
    "Electric Guitar (muted)", "Overdriven Guitar",
    "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Electric Bass (finger)",
    "Electric Bass (pick)", "Fretless Bass",
    "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass",
    "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "SynthStrings 1",
    "SynthStrings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
    "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
    "French Horn", "Brass Section", "SynthBrass 1", "SynthBrass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
    "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute",
    "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
    "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)",
    "Lead 7 (fifths)", "Lead 8 (bass + lead)",
    "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)",
    "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)",
    "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)",
    "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)",
    "FX 7 (echoes)", "FX 8 (sci-fi)",
    "Sitar", "Banjo", "Shamisen", "Koto",
    "Kalimba", "Bag Pipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
    "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
    "Telephone Ring", "Helicopter", "Applause", "Gunshot",
)


# Runtime rs2midi reserves 0x100000..0x1dffff for accurate per-song packs.
# The compact fallback bank starts at 0x1e0000, while frozen audsrv remains
# below the pack region.
PACK_SPU_BASE = 0x00100000
PACK_SPU_LIMIT = 0x001E0000
PACK_SPU_BYTES = PACK_SPU_LIMIT - PACK_SPU_BASE

GEN_START = 0
GEN_END = 1
GEN_LOOP_START = 2
GEN_LOOP_END = 3
GEN_START_COARSE = 4
GEN_END_COARSE = 12
GEN_PAN = 17
GEN_DELAY_VOL_ENV = 33
GEN_ATTACK_VOL_ENV = 34
GEN_HOLD_VOL_ENV = 35
GEN_DECAY_VOL_ENV = 36
GEN_SUSTAIN_VOL_ENV = 37
GEN_RELEASE_VOL_ENV = 38
GEN_KEYNUM_TO_VOL_ENV_HOLD = 39
GEN_KEYNUM_TO_VOL_ENV_DECAY = 40
GEN_INSTRUMENT = 41
GEN_KEY_RANGE = 43
GEN_VEL_RANGE = 44
GEN_LOOP_START_COARSE = 45
GEN_KEYNUM = 46
GEN_VELOCITY = 47
GEN_ATTENUATION = 48
GEN_LOOP_END_COARSE = 50
GEN_COARSE_TUNE = 51
GEN_FINE_TUNE = 52
GEN_SAMPLE_ID = 53
GEN_SAMPLE_MODES = 54
GEN_SCALE_TUNING = 56
GEN_ROOT_KEY = 58

SAMPLE_TYPE_RIGHT = 2
SAMPLE_TYPE_LEFT = 4


def _s16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def _range(value: int) -> tuple[int, int]:
    return value & 0xFF, (value >> 8) & 0xFF


def _zone_gens(first_bag: int, next_bag: int, bags, gens):
    zones = []
    next_bag = min(next_bag, max(0, len(bags) - 1))
    for bag_index in range(first_bag, next_bag):
        if bag_index + 1 >= len(bags):
            break
        start = bags[bag_index][0]
        end = min(bags[bag_index + 1][0], len(gens))
        zones.append(list(gens[start:end]))
    return zones


def _find_selector(gens, op: int):
    for gen_op, amount in gens:
        if gen_op == op:
            return amount
    return None


@dataclass
class Region:
    sample_id: int
    start: int
    end: int
    loop_start: int
    loop_end: int
    looping: bool
    sample_rate: int
    root_key: int
    correction: int
    coarse_tune: int
    fine_tune: int
    scale_tuning: int
    attenuation: int
    pan: int
    keynum: int | None
    fixed_velocity: int | None
    sample_name: str
    source_end: int
    sample_modes: int
    amp_delay_tc: int
    amp_attack_tc: int
    amp_hold_tc: int
    amp_decay_tc: int
    amp_sustain_cb: int
    amp_release_tc: int
    amp_keynum_to_hold: int
    amp_keynum_to_decay: int


@dataclass
class ChannelState:
    program: int = 0
    bank: int = 0
    volume: int = 127
    expression: int = 127
    pan: int = 64
    pitch_bend: int = 8192
    bend_range: int = 2
    sustain: bool = False
    rpn_msb: int = 127
    rpn_lsb: int = 127


@dataclass
class ActiveLayer:
    sample_index: int
    region: Region
    velocity: int
    token: int
    channel: int
    midi_note: int
    note_on_us: int
    release_start_us: int | None = None
    release_end_us: int | None = None
    released: bool = False


class SoundFontResolver:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        (self.smpl_offset, self.smpl_size), self.pdta = _find_sf2_sections(self.data)
        self.sample_headers = _records(self.pdta["shdr"], SHDR)[:-1]

        phdr = _records(self.pdta.get("phdr", b""), PHDR)
        pbag = _records(self.pdta.get("pbag", b""), BAG)
        pgen = _records(self.pdta.get("pgen", b""), GEN)
        inst = _records(self.pdta.get("inst", b""), INST)
        ibag = _records(self.pdta.get("ibag", b""), BAG)
        igen = _records(self.pdta.get("igen", b""), GEN)

        self.presets = {}
        if len(phdr) >= 2:
            for i in range(len(phdr) - 1):
                name, program, bank, first_bag, _library, _genre, _morph = phdr[i]
                zones = _zone_gens(first_bag, phdr[i + 1][3], pbag, pgen)
                self.presets[(bank, program)] = (_name(name), zones)

        self.instruments = []
        if len(inst) >= 2:
            for i in range(len(inst) - 1):
                zones = _zone_gens(inst[i][1], inst[i + 1][1], ibag, igen)
                self.instruments.append((_name(inst[i][0]), zones))

    @staticmethod
    def _split_global(zones, selector_op: int):
        global_zone = []
        locals_ = zones
        if zones and _find_selector(zones[0], selector_op) is None:
            global_zone = zones[0]
            locals_ = zones[1:]
        return global_zone, locals_

    @staticmethod
    def _zone_map(global_zone, local_zone):
        """Resolve one SF2 level: local generators replace matching globals."""
        values = {}
        for op, amount in global_zone:
            values[op] = amount
        for op, amount in local_zone:
            values[op] = amount
        return values

    @staticmethod
    def _zone_range(values, op: int) -> tuple[int, int]:
        amount = values.get(op)
        return _range(amount) if amount is not None else (0, 127)

    @staticmethod
    def _signed_value(values, op: int, default: int = 0) -> int:
        amount = values.get(op)
        return _s16(amount) if amount is not None else default

    @staticmethod
    def _substitution(values, op: int) -> int | None:
        amount = values.get(op)
        if amount is None or amount == 0xFFFF:
            return None
        return amount & 0x7F

    @classmethod
    def _combined_values(
        cls,
        preset_global,
        preset_local,
        instrument_global,
        instrument_local,
    ):
        """
        Resolve the generator hierarchy from SF2 §9.4.

        At a single level, a local generator replaces the matching global
        generator. Value generators at preset level are then added to the
        resolved instrument value. Range generators instead intersect between
        preset and instrument levels. Sample/substitution generators are
        instrument-only and are never added from the preset.
        """
        preset = cls._zone_map(preset_global, preset_local)
        instrument = cls._zone_map(instrument_global, instrument_local)

        preset_key_lo, preset_key_hi = cls._zone_range(
            preset, GEN_KEY_RANGE
        )
        inst_key_lo, inst_key_hi = cls._zone_range(
            instrument, GEN_KEY_RANGE
        )
        preset_vel_lo, preset_vel_hi = cls._zone_range(
            preset, GEN_VEL_RANGE
        )
        inst_vel_lo, inst_vel_hi = cls._zone_range(
            instrument, GEN_VEL_RANGE
        )

        # Sample generators are defined only at instrument level.
        sample_values = {
            GEN_START: cls._signed_value(instrument, GEN_START),
            GEN_END: cls._signed_value(instrument, GEN_END),
            GEN_LOOP_START: cls._signed_value(instrument, GEN_LOOP_START),
            GEN_LOOP_END: cls._signed_value(instrument, GEN_LOOP_END),
            GEN_START_COARSE:
                cls._signed_value(instrument, GEN_START_COARSE),
            GEN_END_COARSE:
                cls._signed_value(instrument, GEN_END_COARSE),
            GEN_LOOP_START_COARSE:
                cls._signed_value(instrument, GEN_LOOP_START_COARSE),
            GEN_LOOP_END_COARSE:
                cls._signed_value(instrument, GEN_LOOP_END_COARSE),
        }

        # Value generators: instrument value/default + preset contribution.
        for op in (
            GEN_PAN,
            GEN_ATTENUATION,
            GEN_COARSE_TUNE,
            GEN_FINE_TUNE,
        ):
            sample_values[op] = (
                cls._signed_value(instrument, op)
                + cls._signed_value(preset, op)
            )

        # scaleTuning's instrument default is 100 cents/key. The preset level
        # contributes zero unless it explicitly contains scaleTuning.
        scale_tuning = (
            cls._signed_value(instrument, GEN_SCALE_TUNING, 100)
            + cls._signed_value(preset, GEN_SCALE_TUNING, 0)
        )

        root_amount = instrument.get(GEN_ROOT_KEY)
        root_key = None
        if root_amount is not None and root_amount != 0xFFFF:
            root_key = root_amount & 0xFF

        sample_modes = instrument.get(GEN_SAMPLE_MODES, 0) & 0x3

        # TinySoundFont-compatible amplitude-envelope values for diagnostics.
        # Instrument defaults are absolute; preset values are relative and
        # therefore contribute zero unless explicitly present.
        env_defaults = {
            GEN_DELAY_VOL_ENV: -12000,
            GEN_ATTACK_VOL_ENV: -12000,
            GEN_HOLD_VOL_ENV: -12000,
            GEN_DECAY_VOL_ENV: -12000,
            GEN_SUSTAIN_VOL_ENV: 0,
            GEN_RELEASE_VOL_ENV: -12000,
            GEN_KEYNUM_TO_VOL_ENV_HOLD: 0,
            GEN_KEYNUM_TO_VOL_ENV_DECAY: 0,
        }
        env_limits = {
            GEN_DELAY_VOL_ENV: (-12000, 5000),
            GEN_ATTACK_VOL_ENV: (-12000, 8000),
            GEN_HOLD_VOL_ENV: (-12000, 5000),
            GEN_DECAY_VOL_ENV: (-12000, 8000),
            GEN_SUSTAIN_VOL_ENV: (0, 1440),
            GEN_RELEASE_VOL_ENV: (-12000, 8000),
            GEN_KEYNUM_TO_VOL_ENV_HOLD: (-1200, 1200),
            GEN_KEYNUM_TO_VOL_ENV_DECAY: (-1200, 1200),
        }
        amp_env = {}
        for op, default in env_defaults.items():
            value = (
                cls._signed_value(instrument, op, default)
                + cls._signed_value(preset, op, 0)
            )
            lo, hi = env_limits[op]
            amp_env[op] = max(lo, min(hi, value))

        return {
            **sample_values,
            "key_range": (
                max(preset_key_lo, inst_key_lo),
                min(preset_key_hi, inst_key_hi),
            ),
            "vel_range": (
                max(preset_vel_lo, inst_vel_lo),
                min(preset_vel_hi, inst_vel_hi),
            ),
            "scale_tuning": scale_tuning,
            "root_key": root_key,
            "sample_modes": sample_modes,
            "keynum": cls._substitution(instrument, GEN_KEYNUM),
            "fixed_velocity":
                cls._substitution(instrument, GEN_VELOCITY),
            "amp_delay_tc": amp_env[GEN_DELAY_VOL_ENV],
            "amp_attack_tc": amp_env[GEN_ATTACK_VOL_ENV],
            "amp_hold_tc": amp_env[GEN_HOLD_VOL_ENV],
            "amp_decay_tc": amp_env[GEN_DECAY_VOL_ENV],
            "amp_sustain_cb": amp_env[GEN_SUSTAIN_VOL_ENV],
            "amp_release_tc": amp_env[GEN_RELEASE_VOL_ENV],
            "amp_keynum_to_hold":
                amp_env[GEN_KEYNUM_TO_VOL_ENV_HOLD],
            "amp_keynum_to_decay":
                amp_env[GEN_KEYNUM_TO_VOL_ENV_DECAY],
        }

    def _expand_sample(self, sample_id: int, values, pan_adjust: int = 0):
        if sample_id < 0 or sample_id >= len(self.sample_headers):
            return []

        (
            raw_name, start, end, loop_start, loop_end, sample_rate,
            original_pitch, correction, sample_link, sample_type,
        ) = self.sample_headers[sample_id]

        if sample_type & 0x8000:
            return []

        start = int(start) + values[GEN_START] + values[GEN_START_COARSE] * 32768
        end = int(end) + values[GEN_END] + values[GEN_END_COARSE] * 32768
        loop_start = int(loop_start) + values[GEN_LOOP_START] + values[GEN_LOOP_START_COARSE] * 32768
        loop_end = int(loop_end) + values[GEN_LOOP_END] + values[GEN_LOOP_END_COARSE] * 32768

        smpl_samples = self.smpl_size // 2
        start = max(0, min(start, smpl_samples))
        end = max(start, min(end, smpl_samples))
        loop_start = max(start, min(loop_start, end))
        loop_end = max(loop_start, min(loop_end, end))

        source_end = end
        looping = bool(values["sample_modes"] & 1) and loop_end > loop_start
        if looping:
            end = loop_end

        if end <= start or sample_rate <= 0:
            return []

        root = values["root_key"]
        if root is None or root > 127:
            root = original_pitch if original_pitch <= 127 else 60

        return [Region(
            sample_id=sample_id,
            start=start,
            end=end,
            loop_start=loop_start,
            loop_end=loop_end,
            looping=looping,
            sample_rate=sample_rate,
            root_key=root,
            correction=correction,
            coarse_tune=values[GEN_COARSE_TUNE],
            fine_tune=values[GEN_FINE_TUNE],
            scale_tuning=values["scale_tuning"],
            attenuation=max(0, values[GEN_ATTENUATION]),
            pan=max(-500, min(500, values[GEN_PAN] + pan_adjust)),
            keynum=values["keynum"],
            fixed_velocity=values["fixed_velocity"],
            sample_name=_name(raw_name),
            source_end=source_end,
            sample_modes=values["sample_modes"],
            amp_delay_tc=values["amp_delay_tc"],
            amp_attack_tc=values["amp_attack_tc"],
            amp_hold_tc=values["amp_hold_tc"],
            amp_decay_tc=values["amp_decay_tc"],
            amp_sustain_cb=values["amp_sustain_cb"],
            amp_release_tc=values["amp_release_tc"],
            amp_keynum_to_hold=values["amp_keynum_to_hold"],
            amp_keynum_to_decay=values["amp_keynum_to_decay"],
        )]

    def resolve(self, bank: int, program: int, note: int, velocity: int):
        preset = self.presets.get((bank, program))
        if preset is None and bank != 0:
            preset = self.presets.get((0, program))
        if preset is None:
            preset = self.presets.get((0, 0))
        if preset is None:
            return []

        _, preset_zones = preset
        preset_global, preset_locals = self._split_global(preset_zones, GEN_INSTRUMENT)
        regions = []

        for pzone in preset_locals:
            instrument_id = _find_selector(pzone, GEN_INSTRUMENT)
            if instrument_id is None or instrument_id >= len(self.instruments):
                continue
            _, inst_zones = self.instruments[instrument_id]
            inst_global, inst_locals = self._split_global(inst_zones, GEN_SAMPLE_ID)

            for izone in inst_locals:
                sample_id = _find_selector(izone, GEN_SAMPLE_ID)
                if sample_id is None:
                    continue
                values = self._combined_values(
                    preset_global,
                    pzone,
                    inst_global,
                    izone,
                )
                key_lo, key_hi = values["key_range"]
                vel_lo, vel_hi = values["vel_range"]
                if not (
                    key_lo <= note <= key_hi
                    and vel_lo <= velocity <= vel_hi
                ):
                    continue

                if sample_id >= len(self.sample_headers):
                    continue
                sample_type = self.sample_headers[sample_id][9] & 0x7FFF
                sample_link = self.sample_headers[sample_id][8]

                # TinySoundFont uses the sample selected by the instrument
                # zone directly. It does not auto-expand sampleLink into a
                # second voice. Doing that here produced two independently
                # encoded/pitched voices and could sound like chorus/detune.
                regions += self._expand_sample(sample_id, values, 0)

        unique = {}
        for region in regions:
            key = (
                region.sample_id, region.start, region.end,
                region.loop_start, region.loop_end, region.pan,
                region.root_key, region.correction,
                region.coarse_tune, region.fine_tune,
                region.scale_tuning, region.attenuation,
                region.keynum, region.fixed_velocity,
            )
            unique[key] = region
        return list(unique.values())

    def pcm_for(self, region: Region):
        count = region.end - region.start
        byte_offset = self.smpl_offset + region.start * 2
        byte_end = byte_offset + count * 2
        if byte_end > self.smpl_offset + self.smpl_size or byte_end > len(self.data):
            raise ValueError(f"SF2 sample {region.sample_id} exceeds smpl data")
        return struct.unpack_from(f"<{count}h", self.data, byte_offset)


def _pitch_details(region: Region, note: int, state: ChannelState):
    effective_note = region.keynum if region.keynum is not None else note
    bend = (state.pitch_bend - 8192) / 8192.0 * state.bend_range * 100.0
    cents = (
        (effective_note - region.root_key) * region.scale_tuning
        + region.coarse_tune * 100
        + region.fine_tune
        + region.correction
        + bend
    )
    native = base_pitch(region.sample_rate)
    raw = int(round(native * (2.0 ** (cents / 1200.0))))
    actual = max(1, min(0x3FFF, raw))
    return actual, raw, native, effective_note, bend, cents


def _tiny_soundfont_pitch_details(
    region: Region,
    note: int,
    state: ChannelState,
):
    """Reference TinySoundFont's pitch_keycenter/keytrack calculation."""
    effective_note = region.keynum if region.keynum is not None else note
    bend = (
        (state.pitch_bend - 8192)
        / 8192.0
        * state.bend_range
        * 100.0
    )
    tune_cents = (
        region.coarse_tune * 100
        + region.fine_tune
        + region.correction
    )
    tuned_note = effective_note + tune_cents / 100.0
    adjusted_note = (
        region.root_key
        + (tuned_note - region.root_key)
        * (region.scale_tuning / 100.0)
    )
    cents = (adjusted_note - region.root_key) * 100.0 + bend
    native = base_pitch(region.sample_rate)
    raw = int(round(native * (2.0 ** (cents / 1200.0))))
    actual = max(1, min(0x3FFF, raw))
    return actual, raw, native, effective_note, bend, cents


def _pitch(region: Region, note: int, state: ChannelState) -> int:
    return _pitch_details(region, note, state)[0]


FRAME_EXACT_TRIGGER_CENTS = 50.0
FRAME_EXACT_MAX_LOOP_SAMPLES = 8192


def _loop_frame_plan(loop_start_sample: int, loop_end_sample: int):
    """Describe the current SPU2 loop error and a bounded exact alternative."""
    exact_loop = loop_end_sample - loop_start_sample
    if exact_loop <= 0:
        return {
            "exact_loop": 0,
            "legacy_loop": 0,
            "legacy_cents": 0.0,
            "repeat_count": 1,
            "aligned_loop": 0,
            "align_samples": 0,
            "apply_exact": False,
        }

    legacy_start_frame = loop_start_sample // SAMPLES_PER_FRAME
    legacy_end_frame = max(
        legacy_start_frame,
        (loop_end_sample - 1) // SAMPLES_PER_FRAME,
    )
    legacy_loop = (
        legacy_end_frame - legacy_start_frame + 1
    ) * SAMPLES_PER_FRAME
    legacy_cents = 1200.0 * math.log2(exact_loop / legacy_loop)

    # Repeating an SF2 cycle does not change its pitch. 28/gcd(L, 28)
    # copies make the total cycle span exactly divisible by one SPU2 ADPCM
    # frame, so the hardware loop flags no longer stretch the period.
    repeat_count = (
        SAMPLES_PER_FRAME
        // math.gcd(exact_loop, SAMPLES_PER_FRAME)
    )
    aligned_loop = exact_loop * repeat_count
    align_samples = (-loop_start_sample) % SAMPLES_PER_FRAME

    apply_exact = (
        abs(legacy_cents) >= FRAME_EXACT_TRIGGER_CENTS
        and aligned_loop <= FRAME_EXACT_MAX_LOOP_SAMPLES
    )
    return {
        "exact_loop": exact_loop,
        "legacy_loop": legacy_loop,
        "legacy_cents": legacy_cents,
        "repeat_count": repeat_count,
        "aligned_loop": aligned_loop,
        "align_samples": align_samples,
        "apply_exact": apply_exact,
    }


def _prepare_frame_exact_loop(
    pcm,
    loop_start_sample: int | None,
    loop_end_sample: int | None,
):
    """Reframe a badly quantized loop without changing its waveform period."""
    if (
        loop_start_sample is None
        or loop_end_sample is None
        or loop_start_sample < 0
        or loop_end_sample <= loop_start_sample
        or loop_end_sample > len(pcm)
    ):
        return list(pcm), loop_start_sample, loop_end_sample

    plan = _loop_frame_plan(loop_start_sample, loop_end_sample)
    if not plan["apply_exact"]:
        return list(pcm), loop_start_sample, loop_end_sample

    cycle = list(pcm[loop_start_sample:loop_end_sample])
    cycle_len = len(cycle)
    align_samples = plan["align_samples"]

    # Advance to the next ADPCM-frame boundary using real samples from the
    # loop itself, then rotate the repeated cycle to that exact phase. This
    # avoids inserting silence/clicks before the hardware loop starts.
    prefix = list(pcm[:loop_start_sample])
    if align_samples:
        prefix.extend(
            cycle[i % cycle_len]
            for i in range(align_samples)
        )

    phase = align_samples % cycle_len
    if phase:
        cycle = cycle[phase:] + cycle[:phase]

    loop_body = cycle * plan["repeat_count"]
    aligned_start = len(prefix)
    aligned_end = aligned_start + len(loop_body)
    prepared = (
        prefix
        + loop_body
        + list(pcm[loop_end_sample:])
    )

    if (
        aligned_start % SAMPLES_PER_FRAME != 0
        or aligned_end % SAMPLES_PER_FRAME != 0
    ):
        raise AssertionError("frame-exact loop preparation lost alignment")

    return prepared, aligned_start, aligned_end


def _timecents_to_seconds(value: float, threshold: float = -11950.0):
    if value < threshold:
        return 0.0
    return 2.0 ** (value / 1200.0)


def _tiny_amp_env(region: Region, note: int):
    hold_tc = (
        region.amp_hold_tc
        + region.amp_keynum_to_hold * (60.0 - note)
    )
    decay_tc = (
        region.amp_decay_tc
        + region.amp_keynum_to_decay * (60.0 - note)
    )
    hold_threshold = (
        -10000.0 if region.amp_keynum_to_hold else -11950.0
    )
    decay_threshold = (
        -10000.0 if region.amp_keynum_to_decay else -11950.0
    )
    return {
        "delay": _timecents_to_seconds(region.amp_delay_tc),
        "attack": _timecents_to_seconds(region.amp_attack_tc),
        "hold": _timecents_to_seconds(hold_tc, hold_threshold),
        "decay": _timecents_to_seconds(decay_tc, decay_threshold),
        "sustain_gain":
            10.0 ** (-max(0, region.amp_sustain_cb) / 200.0),
        "release": _timecents_to_seconds(region.amp_release_tc),
    }


def _mix_values(
    region: Region,
    velocity: int,
    controller_volume: int,
    controller_expression: int,
    controller_pan: int,
) -> tuple[int, int]:
    effective_velocity = (
        region.fixed_velocity
        if region.fixed_velocity is not None
        else velocity
    )
    velocity_gain = math.sqrt(
        max(0.0, min(1.0, effective_velocity / 127.0))
    )
    controller_gain = (
        controller_volume / 127.0
    ) * (
        controller_expression / 127.0
    )
    sf_gain = 10.0 ** (-max(0, region.attenuation) / 200.0)
    volume = int(round(
        100.0 * velocity_gain * controller_gain * sf_gain
    ))
    volume = max(0, min(100, volume))

    midi_pan = int(round(
        (controller_pan - 64) * (100.0 / 63.0)
    ))
    sf_pan = int(round(region.pan / 5.0))
    pan = max(-100, min(100, midi_pan + sf_pan))
    return volume, pan


def _mix(region: Region, velocity: int, state: ChannelState) -> tuple[int, int]:
    return _mix_values(
        region,
        velocity,
        state.volume,
        state.expression,
        state.pan,
    )


def _amp_level_before_release(
    region: Region,
    note: int,
    elapsed_seconds: float,
) -> float:
    env = _tiny_amp_env(region, note)
    t = max(0.0, elapsed_seconds)

    if t < env["delay"]:
        return 0.0
    t -= env["delay"]

    if env["attack"] > 0.0:
        if t < env["attack"]:
            return max(0.0, min(1.0, t / env["attack"]))
        t -= env["attack"]

    if t < env["hold"]:
        return 1.0
    t -= env["hold"]

    sustain = max(0.0, min(1.0, env["sustain_gain"]))
    decay = env["decay"]
    if decay <= 0.0:
        return sustain

    if sustain > 0.0:
        decay_to_sustain = decay * math.log(sustain) / -9.226
        decay_to_sustain = max(0.0, decay_to_sustain)
    else:
        decay_to_sustain = decay

    if t < decay_to_sustain:
        return max(0.0, min(
            1.0,
            math.exp(-9.226 * t / decay),
        ))
    return sustain


def build_pack(sf2_path: Path, midi_path: Path, output_path: Path):
    resolver = SoundFontResolver(sf2_path)
    midi_format, ppqn, timed, duration_us = events_with_time(midi_path)
    channels = [ChannelState(bank=128 if i == 9 else 0) for i in range(16)]

    sample_blobs = []
    sample_index_by_key = {}
    active: dict[tuple[int, int], list[ActiveLayer]] = {}
    out_events = []
    pitch_clamp_count = 0
    pitch_clamp_by_program: dict[tuple[int, int], int] = {}
    pitch_diff_by_program: dict[tuple[int, int], list[float]] = {}
    pitch_diag_seen = set()
    region_diag_seen = set()
    diagnostic_programs = {
        14: "Tubular Bells",
        56: "Trumpet",
        60: "French Horn",
        61: "Brass Section",
    }

    all_layers: list[ActiveLayer] = []
    mix_timeline = [
        [(0, 127, 127, 64)]
        for _ in range(16)
    ]
    # Tokens are matched by (MIDI channel, token) at runtime. Keep a
    # separate 8-bit token namespace per MIDI channel instead of imposing
    # one artificial 256-layer ceiling across the entire song.
    token_busy_until = [[-1] * 256 for _ in range(16)]
    token_cursor = [0] * 16
    envelope_mix_events = 0

    def record_mix_state(time_us: int, channel: int):
        value = (
            int(time_us),
            channels[channel].volume,
            channels[channel].expression,
            channels[channel].pan,
        )
        timeline = mix_timeline[channel]
        if timeline and timeline[-1][0] == value[0]:
            timeline[-1] = value
        else:
            timeline.append(value)

    def mix_state_at(channel: int, time_us: int):
        timeline = mix_timeline[channel]
        for when, volume, expression, pan in reversed(timeline):
            if when <= time_us:
                return volume, expression, pan
        return 127, 127, 64

    def allocate_token(time_us: int, channel: int) -> int:
        busy = token_busy_until[channel]
        cursor = token_cursor[channel]
        for offset in range(256):
            token = (cursor + offset) & 0xFF
            # Never recycle on the exact release timestamp: the old
            # token's final OUT_MIX/KOFF events are sorted at that same time.
            if busy[token] < time_us:
                busy[token] = 1 << 62
                token_cursor[channel] = (token + 1) & 0xFF
                return token
        raise ValueError(
            f"more than 256 overlapping accurate-pack note layers "
            f"on MIDI channel {channel} at "
            f"{time_us / 1_000_000.0:.3f}s"
        )

    def begin_release(
        layer: ActiveLayer,
        release_start_us: int,
        immediate: bool = False,
    ):
        if layer.release_start_us is not None:
            return
        layer.release_start_us = int(release_start_us)
        if immediate:
            layer.release_end_us = int(release_start_us)
        else:
            env = _tiny_amp_env(layer.region, layer.midi_note)
            release_seconds = env["release"]
            if release_seconds <= 0.0:
                release_seconds = 0.01
            layer.release_end_us = int(
                release_start_us
                + round(release_seconds * 1_000_000.0)
            )
        token_busy_until[layer.channel][layer.token] = layer.release_end_us

    def diagnose_region(
        time_us: int,
        channel: int,
        region: Region,
        note: int,
        state: ChannelState,
    ):
        bank = 128 if channel == 9 else state.bank
        if bank != 0 or state.program not in diagnostic_programs:
            return
        key = (
            bank,
            state.program,
            note,
            region.sample_id,
            region.root_key,
            region.scale_tuning,
            region.sample_modes,
        )
        if key in region_diag_seen:
            return
        region_diag_seen.add(key)

        env = _tiny_amp_env(region, note)
        mode = {
            0: "none",
            1: "continuous",
            3: "sustain",
        }.get(region.sample_modes & 3, "reserved")
        release_tail = max(0, region.source_end - region.loop_end)
        print(
            "REGION DIAG: "
            f"time={time_us / 1_000_000.0:.3f}s "
            f"program={state.program} "
            f"name={diagnostic_programs[state.program]!r} "
            f"note={note} sample={region.sample_id} "
            f"sample_name={region.sample_name!r} "
            f"root={region.root_key} rate={region.sample_rate} "
            f"scale={region.scale_tuning} "
            f"coarse={region.coarse_tune} "
            f"fine={region.fine_tune} "
            f"correction={region.correction} "
            f"loop_mode={mode} "
            f"loop={region.loop_start}:{region.loop_end} "
            f"source_end={region.source_end} "
            f"release_tail_samples={release_tail}"
        )
        print(
            "  AMP ENV: "
            f"delay_tc={region.amp_delay_tc} "
            f"attack_tc={region.amp_attack_tc} "
            f"hold_tc={region.amp_hold_tc} "
            f"decay_tc={region.amp_decay_tc} "
            f"sustain_cb={region.amp_sustain_cb} "
            f"release_tc={region.amp_release_tc} "
            f"key_hold={region.amp_keynum_to_hold} "
            f"key_decay={region.amp_keynum_to_decay}"
        )
        print(
            "  TSF ENV: "
            f"delay={env['delay']:.4f}s "
            f"attack={env['attack']:.4f}s "
            f"hold={env['hold']:.4f}s "
            f"decay={env['decay']:.4f}s "
            f"sustain_gain={env['sustain_gain']:.4f} "
            f"release={env['release']:.4f}s"
        )

    def trace_window_note(
        time_us: int,
        channel: int,
        region: Region,
        note: int,
        velocity: int,
        state: ChannelState,
    ):
        if not (20_000_000 <= time_us <= 30_000_000):
            return

        bank = 128 if channel == 9 else state.bank
        if channel == 9:
            program_name = "Percussion"
        elif 0 <= state.program < len(GM_PROGRAM_NAMES):
            program_name = GM_PROGRAM_NAMES[state.program]
        else:
            program_name = f"Program {state.program}"

        loop_mode = {
            0: "none",
            1: "continuous",
            3: "sustain",
        }.get(region.sample_modes & 3, "reserved")

        exact_loop = 0
        legacy_loop = 0
        legacy_cents = 0.0
        ps2_loop = 0
        remaining_cents = 0.0
        repeat_count = 1
        align_samples = 0
        frame_exact = False
        if (
            (region.sample_modes & 1)
            and region.loop_end > region.loop_start
        ):
            relative_loop_start = region.loop_start - region.start
            relative_loop_end = region.loop_end - region.start
            plan = _loop_frame_plan(
                relative_loop_start,
                relative_loop_end,
            )
            exact_loop = plan["exact_loop"]
            legacy_loop = plan["legacy_loop"]
            legacy_cents = plan["legacy_cents"]
            repeat_count = plan["repeat_count"]
            align_samples = plan["align_samples"]
            frame_exact = plan["apply_exact"]
            if frame_exact:
                ps2_loop = plan["aligned_loop"]
                remaining_cents = 0.0
            else:
                ps2_loop = legacy_loop
                remaining_cents = legacy_cents

        pitch, raw_pitch, _native, _effective, _bend, cents = (
            _pitch_details(region, note, state)
        )

        print(
            "WINDOW NOTE: "
            f"time={time_us / 1_000_000.0:.3f}s "
            f"ch={channel} bank={bank} "
            f"program={state.program} name={program_name!r} "
            f"note={note} velocity={velocity} "
            f"sample={region.sample_id} "
            f"sample_name={region.sample_name!r} "
            f"root={region.root_key} rate={region.sample_rate} "
            f"loop_mode={loop_mode} "
            f"sf2_loop_samples={exact_loop} "
            f"legacy_ps2_loop_samples={legacy_loop} "
            f"legacy_loop_pitch_error={legacy_cents:+.3f}c "
            f"frame_exact={int(frame_exact)} "
            f"loop_repeats={repeat_count} "
            f"align_samples={align_samples} "
            f"ps2_loop_samples={ps2_loop} "
            f"loop_pitch_error={remaining_cents:+.3f}c "
            f"pitch=0x{pitch:04X} raw=0x{raw_pitch:X} "
            f"pitch_cents={cents:+.3f}"
        )

    def pitch_for(
        time_us: int,
        channel: int,
        region: Region,
        note: int,
        state: ChannelState,
    ) -> int:
        nonlocal pitch_clamp_count
        actual, raw, native, effective_note, bend, cents = _pitch_details(
            region, note, state
        )
        (
            tsf_actual,
            tsf_raw,
            _tsf_native,
            _tsf_note,
            _tsf_bend,
            tsf_cents,
        ) = _tiny_soundfont_pitch_details(region, note, state)
        bank = 128 if channel == 9 else state.bank
        delta_cents = cents - tsf_cents
        pitch_diff_by_program.setdefault(
            (bank, state.program), []
        ).append(delta_cents)

        diag_key = (
            bank,
            state.program,
            note,
            region.sample_id,
            round(delta_cents, 4),
        )
        if (
            abs(delta_cents) >= 0.5
            or (
                bank == 0
                and state.program in diagnostic_programs
                and diag_key not in pitch_diag_seen
            )
        ):
            pitch_diag_seen.add(diag_key)
            name = diagnostic_programs.get(
                state.program, f"program {state.program}"
            )
            print(
                "TSF PITCH: "
                f"time={time_us / 1_000_000.0:.3f}s "
                f"ch={channel} bank={bank} "
                f"program={state.program} name={name!r} "
                f"note={note} effective_note={effective_note} "
                f"sample={region.sample_id} root={region.root_key} "
                f"rate={region.sample_rate} "
                f"scale={region.scale_tuning} "
                f"tune={region.coarse_tune * 100 + region.fine_tune + region.correction}c "
                f"ours_cents={cents:.3f} "
                f"tsf_cents={tsf_cents:.3f} "
                f"delta={delta_cents:+.3f}c "
                f"ours=0x{actual:04X} "
                f"tsf=0x{tsf_actual:04X} "
                f"ours_raw=0x{raw:X} "
                f"tsf_raw=0x{tsf_raw:X}"
            )

        if raw < 1 or raw > 0x3FFF:
            pitch_clamp_count += 1
            key = (bank, state.program)
            pitch_clamp_by_program[key] = (
                pitch_clamp_by_program.get(key, 0) + 1
            )
            direction = "LOW" if raw < 1 else "HIGH"
            print(
                "PITCH CLAMP "
                f"#{pitch_clamp_count} {direction}: "
                f"time={time_us / 1_000_000.0:.3f}s "
                f"ch={channel} bank={bank} program={state.program} "
                f"midi_note={note} effective_note={effective_note} "
                f"sample={region.sample_id} root={region.root_key} "
                f"sample_rate={region.sample_rate} "
                f"native=0x{native:04X} "
                f"scale={region.scale_tuning} "
                f"coarse={region.coarse_tune} "
                f"fine={region.fine_tune} "
                f"correction={region.correction} "
                f"bend={bend:.2f}c cents={cents:.2f} "
                f"raw=0x{raw:X} actual=0x{actual:04X}"
            )
        return actual

    def ensure_sample(region: Region):
        key = (
            region.sample_id,
            region.start,
            region.end,
            region.loop_start if region.looping else -1,
            region.loop_end if region.looping else -1,
        )
        existing = sample_index_by_key.get(key)
        if existing is not None:
            return existing

        pcm = resolver.pcm_for(region)
        loop_start = region.loop_start - region.start if region.looping else None
        loop_end = region.loop_end - region.start if region.looping else None
        if region.looping:
            pcm, loop_start, loop_end = _prepare_frame_exact_loop(
                pcm,
                loop_start,
                loop_end,
            )
        adp = encode_mono_pcm16(pcm, region.sample_rate, loop_start, loop_end)
        index = len(sample_blobs)
        if index >= 0xFFFF:
            raise ValueError("too many title-song sample variants")
        sample_index_by_key[key] = index
        sample_blobs.append(adp)
        return index

    def add_event(time_us, kind, channel=0, note=0, value=0,
                  sample_index=0xFFFF, pitch=0, volume=0, pan=0, aux=0):
        out_events.append((
            int(time_us), kind, channel & 0xFF, note & 0xFF, value & 0xFF,
            sample_index & 0xFFFF, pitch & 0xFFFF, volume & 0xFF,
            max(-128, min(127, int(pan))), aux & 0xFFFF,
        ))

    def update_channel_mix(time_us, channel):
        record_mix_state(time_us, channel)

    def update_channel_pitch(time_us, channel):
        state = channels[channel]
        for (ch, note), layers in list(active.items()):
            if ch != channel:
                continue
            for layer in layers:
                add_event(
                    time_us, OUT_PITCH, ch, layer.token, 0,
                    layer.sample_index,
                    pitch_for(
                        time_us, ch, layer.region, note, state
                    ),
                )

    for time_us, event in timed:
        event_type, channel, a, b = event[4], event[5], event[6], event[7]
        state = channels[channel]

        if event_type == EV_PROGRAM:
            state.program = a
            continue

        if event_type == EV_CONTROL:
            control, value = a, b
            if control == 0:
                state.bank = value
            elif control == 7:
                state.volume = value
                update_channel_mix(time_us, channel)
            elif control == 10:
                state.pan = value
                update_channel_mix(time_us, channel)
            elif control == 11:
                state.expression = value
                update_channel_mix(time_us, channel)
            elif control == 64:
                was = state.sustain
                state.sustain = value >= 64
                if was and not state.sustain:
                    for key in [k for k in active if k[0] == channel]:
                        kept = []
                        for layer in active[key]:
                            if layer.released:
                                begin_release(layer, time_us)
                            else:
                                kept.append(layer)
                        if kept:
                            active[key] = kept
                        else:
                            del active[key]
            elif control == 100:
                state.rpn_lsb = value
            elif control == 101:
                state.rpn_msb = value
            elif control == 6 and state.rpn_msb == 0 and state.rpn_lsb == 0:
                state.bend_range = max(0, min(24, value))
                update_channel_pitch(time_us, channel)
            elif control in (120, 123):
                for key in [k for k in active if k[0] == channel]:
                    for layer in active[key]:
                        begin_release(layer, time_us, immediate=True)
                    del active[key]
            elif control == 121:
                was_sustain = state.sustain
                state.volume = 127
                state.expression = 127
                state.pan = 64
                state.pitch_bend = 8192
                state.bend_range = 2
                state.sustain = False
                if was_sustain:
                    for key in [k for k in active if k[0] == channel]:
                        kept = []
                        for layer in active[key]:
                            if layer.released:
                                begin_release(layer, time_us)
                            else:
                                kept.append(layer)
                        if kept:
                            active[key] = kept
                        else:
                            del active[key]
                update_channel_mix(time_us, channel)
                update_channel_pitch(time_us, channel)
            continue

        if event_type == EV_PITCH_BEND:
            state.pitch_bend = (b << 7) | a
            update_channel_pitch(time_us, channel)
            continue

        if event_type == EV_NOTE_ON:
            note, velocity = a, b
            bank = 128 if channel == 9 else state.bank
            regions = resolver.resolve(bank, state.program, note, velocity)
            if not regions:
                print(
                    f"warning: no SF2 region for bank={bank} program={state.program} "
                    f"note={note} velocity={velocity}"
                )
                continue

            layers = active.setdefault((channel, note), [])
            for region in regions:
                diagnose_region(
                    time_us, channel, region, note, state
                )
                trace_window_note(
                    time_us,
                    channel,
                    region,
                    note,
                    velocity,
                    state,
                )
                sample_index = ensure_sample(region)
                volume, pan = _mix(region, velocity, state)
                initial_gain = _amp_level_before_release(
                    region, note, 0.0
                )
                volume = max(
                    0,
                    min(100, int(round(volume * initial_gain))),
                )
                pitch = pitch_for(
                    time_us, channel, region, note, state
                )
                token = allocate_token(time_us, channel)
                add_event(
                    time_us, OUT_NOTE_ON, channel, token, velocity,
                    sample_index, pitch, volume, pan,
                )
                layer = ActiveLayer(
                    sample_index=sample_index,
                    region=region,
                    velocity=velocity,
                    token=token,
                    channel=channel,
                    midi_note=note,
                    note_on_us=int(time_us),
                )
                layers.append(layer)
                all_layers.append(layer)
            continue

        if event_type == EV_NOTE_OFF:
            note = a
            layers = active.get((channel, note), [])
            if state.sustain:
                for layer in layers:
                    layer.released = True
            else:
                for layer in layers:
                    begin_release(layer, time_us)
                active.pop((channel, note), None)

    # Any voice left active at end-of-song is stopped at the MIDI boundary
    # so looping area music does not lengthen on every pass.
    for layers in list(active.values()):
        for layer in layers:
            if layer.release_start_us is None:
                begin_release(layer, duration_us, immediate=True)
    active.clear()

    def envelope_gain_at(layer: ActiveLayer, time_us: int) -> float:
        elapsed = max(
            0.0,
            (time_us - layer.note_on_us) / 1_000_000.0,
        )
        held_gain = _amp_level_before_release(
            layer.region,
            layer.midi_note,
            elapsed,
        )
        if (
            layer.release_start_us is None
            or time_us <= layer.release_start_us
        ):
            return held_gain

        release_elapsed = (
            time_us - layer.release_start_us
        ) / 1_000_000.0
        env = _tiny_amp_env(layer.region, layer.midi_note)
        release_seconds = env["release"]
        if release_seconds <= 0.0:
            release_seconds = 0.01
        start_elapsed = max(
            0.0,
            (layer.release_start_us - layer.note_on_us)
            / 1_000_000.0,
        )
        start_gain = _amp_level_before_release(
            layer.region,
            layer.midi_note,
            start_elapsed,
        )
        if release_elapsed >= release_seconds:
            return 0.0
        return max(
            0.0,
            min(
                1.0,
                start_gain
                * math.exp(
                    -9.226 * release_elapsed / release_seconds
                ),
            ),
        )

    def add_segment_points(
        points: set[int],
        start_us: int,
        end_us: int,
        steps: int,
    ):
        if end_us <= start_us or steps <= 0:
            return
        span = end_us - start_us
        for step in range(1, steps + 1):
            points.add(start_us + (span * step) // steps)

    # Bake TinySoundFont-style amplitude envelopes into existing OUT_MIX
    # events. This avoids any new PS2 runtime/IRX protocol while preserving
    # per-note attack, decay, sustain and release behavior.
    for layer in all_layers:
        release_start = (
            layer.release_start_us
            if layer.release_start_us is not None
            else duration_us
        )
        release_end = (
            layer.release_end_us
            if layer.release_end_us is not None
            else release_start
        )
        env = _tiny_amp_env(layer.region, layer.midi_note)

        delay_end = layer.note_on_us + int(
            round(env["delay"] * 1_000_000.0)
        )
        attack_end = delay_end + int(
            round(env["attack"] * 1_000_000.0)
        )
        hold_end = attack_end + int(
            round(env["hold"] * 1_000_000.0)
        )

        sustain = max(
            0.0,
            min(1.0, env["sustain_gain"]),
        )
        if env["decay"] > 0.0:
            if sustain > 0.0:
                decay_seconds = (
                    env["decay"]
                    * math.log(sustain)
                    / -9.226
                )
                decay_seconds = max(0.0, decay_seconds)
            else:
                decay_seconds = env["decay"]
        else:
            decay_seconds = 0.0
        decay_end = hold_end + int(
            round(decay_seconds * 1_000_000.0)
        )

        points: set[int] = set()
        if delay_end > layer.note_on_us:
            points.add(min(delay_end, release_start))
        add_segment_points(
            points,
            max(layer.note_on_us, delay_end),
            min(attack_end, release_start),
            8,
        )
        if hold_end > attack_end:
            points.add(min(hold_end, release_start))
        add_segment_points(
            points,
            max(layer.note_on_us, hold_end),
            min(decay_end, release_start),
            24,
        )

        # Controller changes must preserve the current envelope level.
        for when, _vol, _expr, _pan in mix_timeline[layer.channel]:
            if layer.note_on_us < when < release_end:
                points.add(when)

        if release_start > layer.note_on_us:
            points.add(release_start)
        add_segment_points(
            points,
            release_start,
            release_end,
            16,
        )

        last_mix = None
        for when in sorted(points):
            if when <= layer.note_on_us or when > release_end:
                continue
            ctrl_volume, ctrl_expression, ctrl_pan = mix_state_at(
                layer.channel, when
            )
            base_volume, pan = _mix_values(
                layer.region,
                layer.velocity,
                ctrl_volume,
                ctrl_expression,
                ctrl_pan,
            )
            gain = envelope_gain_at(layer, when)
            volume = max(
                0,
                min(100, int(round(base_volume * gain))),
            )
            mix = (volume, pan)
            if mix == last_mix:
                continue
            add_event(
                when,
                OUT_MIX,
                layer.channel,
                layer.token,
                0,
                layer.sample_index,
                0,
                volume,
                pan,
            )
            envelope_mix_events += 1
            last_mix = mix

        # OUT_MIX at the release end is inserted first, then KOFF, so the
        # final transition cannot leave a constant loop audible.
        add_event(
            release_end,
            OUT_NOTE_OFF,
            layer.channel,
            layer.token,
        )

    # Runtime uploads in hardware-proven 64-byte LOAD_ABS blocks and gives
    # each resident sample its own 64-byte-aligned SPU2 span.
    spu_payload = sum(
        (max(0, len(blob) - 16) + 63) & ~63
        for blob in sample_blobs
    )
    spu_budget = PACK_SPU_BYTES
    if spu_payload > spu_budget:
        raise ValueError(
            f"song sample set needs {spu_payload:,} SPU2 bytes; "
            f"accurate-pack region is {spu_budget:,}. "
            "A residency/cache split is required for this song."
        )

    packed_events = []
    last_us = 0
    for _, data in sorted(enumerate(out_events), key=lambda item: (item[1][0], item[0])):
        time_us, kind, channel, note, value, sample_idx, pitch, volume, pan, aux = data
        delta = time_us - last_us
        while delta > 0xFFFFFFFF:
            packed_events.append((0xFFFFFFFF, OUT_WAIT, 0, 0, 0, 0xFFFF, 0, 0, 0, 0))
            delta -= 0xFFFFFFFF
            last_us += 0xFFFFFFFF
        packed_events.append(
            (delta, kind, channel, note, value, sample_idx, pitch, volume, pan, aux)
        )
        last_us = time_us

    sample_table_offset = HEADER.size
    event_table_offset = sample_table_offset + len(sample_blobs) * SAMPLE_REC.size
    data_offset = event_table_offset + len(packed_events) * EVENT_REC.size
    data_size = sum(len(blob) for blob in sample_blobs)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as out:
        out.write(HEADER.pack(
            MAGIC,
            VERSION,
            HEADER.size,
            len(sample_blobs),
            len(packed_events),
            sample_table_offset,
            event_table_offset,
            data_offset,
            data_size,
            duration_us,
        ))

        running = data_offset
        for blob in sample_blobs:
            out.write(SAMPLE_REC.pack(running, len(blob)))
            running += len(blob)

        for event in packed_events:
            out.write(EVENT_REC.pack(*event))

        for blob in sample_blobs:
            out.write(blob)

    print(f"SoundFont:        {sf2_path}")
    print(f"MIDI:             {midi_path}")
    print(f"MIDI format/PPQN: {midi_format}/{ppqn}")
    print(f"Duration:         {duration_us / 1_000_000.0:.3f} s")
    print(f"ADPCM samples:    {len(sample_blobs)}")
    print(f"Sequence events:  {len(packed_events)}")
    print(f"Envelope events:  {envelope_mix_events}")
    print(f"Pitch clamps:     {pitch_clamp_count}")
    if pitch_diff_by_program:
        print("TinySoundFont pitch deltas by bank/program:")
        for (bank, program), values in sorted(
            pitch_diff_by_program.items()
        ):
            if not values:
                continue
            max_abs = max(abs(value) for value in values)
            mean = sum(values) / len(values)
            if (
                max_abs >= 0.5
                or (bank == 0 and program in diagnostic_programs)
            ):
                print(
                    f"  bank={bank:3d} program={program:3d} "
                    f"count={len(values):4d} "
                    f"mean={mean:+.3f}c "
                    f"max_abs={max_abs:.3f}c"
                )
    if pitch_clamp_by_program:
        print("Pitch clamps by bank/program:")
        for (bank, program), count in sorted(
            pitch_clamp_by_program.items()
        ):
            print(
                f"  bank={bank:3d} program={program:3d} "
                f"count={count}"
            )
    print(f"SPU2 pack data:   {spu_payload:,} / {spu_budget:,} bytes")
    print(f"Pack size:        {output_path.stat().st_size:,} bytes")
    print(f"Wrote:            {output_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("soundfont", type=Path)
    parser.add_argument("midi", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build_pack(args.soundfont, args.midi, args.output)


if __name__ == "__main__":
    main()
