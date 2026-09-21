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
from ps2_adpcm import base_pitch, encode_mono_pcm16
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
        }

    def _expand_sample(self, sample_id: int, values, pan_adjust: int = 0):
        if sample_id < 0 or sample_id >= len(self.sample_headers):
            return []

        (
            _raw_name, start, end, loop_start, loop_end, sample_rate,
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

                if sample_type == SAMPLE_TYPE_LEFT:
                    regions += self._expand_sample(sample_id, values, -500)
                    if sample_link < len(self.sample_headers):
                        regions += self._expand_sample(sample_link, values, 500)
                elif sample_type == SAMPLE_TYPE_RIGHT:
                    regions += self._expand_sample(sample_id, values, 500)
                    if sample_link < len(self.sample_headers):
                        regions += self._expand_sample(sample_link, values, -500)
                else:
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


def _pitch(region: Region, note: int, state: ChannelState) -> int:
    return _pitch_details(region, note, state)[0]


def _mix(region: Region, velocity: int, state: ChannelState) -> tuple[int, int]:
    effective_velocity = (
        region.fixed_velocity
        if region.fixed_velocity is not None
        else velocity
    )
    velocity_gain = math.sqrt(
        max(0.0, min(1.0, effective_velocity / 127.0))
    )
    controller_gain = (state.volume / 127.0) * (state.expression / 127.0)
    sf_gain = 10.0 ** (-max(0, region.attenuation) / 200.0)
    volume = int(round(100.0 * velocity_gain * controller_gain * sf_gain))
    volume = max(0, min(100, volume))

    midi_pan = int(round((state.pan - 64) * (100.0 / 63.0)))
    sf_pan = int(round(region.pan / 5.0))
    pan = max(-100, min(100, midi_pan + sf_pan))
    return volume, pan


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
        if raw < 1 or raw > 0x3FFF:
            pitch_clamp_count += 1
            bank = 128 if channel == 9 else state.bank
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
        state = channels[channel]
        for (ch, note), layers in list(active.items()):
            if ch != channel:
                continue
            for layer in layers:
                volume, pan = _mix(layer.region, layer.velocity, state)
                add_event(
                    time_us, OUT_MIX, ch, note, 0,
                    layer.sample_index, 0, volume, pan,
                )

    def update_channel_pitch(time_us, channel):
        state = channels[channel]
        for (ch, note), layers in list(active.items()):
            if ch != channel:
                continue
            for layer in layers:
                add_event(
                    time_us, OUT_PITCH, ch, note, 0,
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
                if state.sustain != was:
                    add_event(time_us, OUT_SUSTAIN, channel, 0, 1 if state.sustain else 0)
                if was and not state.sustain:
                    for key in [k for k in active if k[0] == channel]:
                        kept = [layer for layer in active[key] if not layer.released]
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
                    add_event(time_us, OUT_NOTE_OFF, channel, key[1])
                    del active[key]
            elif control == 121:
                state.volume = 127
                state.expression = 127
                state.pan = 64
                state.pitch_bend = 8192
                state.bend_range = 2
                state.sustain = False
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
                sample_index = ensure_sample(region)
                volume, pan = _mix(region, velocity, state)
                pitch = pitch_for(
                    time_us, channel, region, note, state
                )
                add_event(
                    time_us, OUT_NOTE_ON, channel, note, velocity,
                    sample_index, pitch, volume, pan,
                )
                layers.append(ActiveLayer(sample_index, region, velocity))
            continue

        if event_type == EV_NOTE_OFF:
            note = a
            add_event(time_us, OUT_NOTE_OFF, channel, note)
            layers = active.get((channel, note), [])
            if state.sustain:
                for layer in layers:
                    layer.released = True
            else:
                active.pop((channel, note), None)

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
    print(f"Pitch clamps:     {pitch_clamp_count}")
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
