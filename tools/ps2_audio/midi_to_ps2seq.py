#!/usr/bin/env python3
"""Compile RuneScape/Standard MIDI data to a compact, tempo-resolved event stream."""

from __future__ import annotations

import argparse
import bz2
import struct
from pathlib import Path

HEADER = struct.Struct("<4sHHIIQ")
EVENT = struct.Struct("<IBBBB")
MAGIC = b"RSEQ"
VERSION = 1

EV_WAIT = 0
EV_NOTE_ON = 1
EV_NOTE_OFF = 2
EV_PROGRAM = 3
EV_CONTROL = 4
EV_PITCH_BEND = 5
EV_POLY_PRESSURE = 6
EV_CHANNEL_PRESSURE = 7

EVENT_NAMES = {
    EV_NOTE_ON: "note_on",
    EV_NOTE_OFF: "note_off",
    EV_PROGRAM: "program",
    EV_CONTROL: "control",
    EV_PITCH_BEND: "pitch_bend",
    EV_POLY_PRESSURE: "poly_pressure",
    EV_CHANNEL_PRESSURE: "channel_pressure",
}


def _u16be(data: bytes, pos: int) -> int:
    return struct.unpack_from(">H", data, pos)[0]


def _u32be(data: bytes, pos: int) -> int:
    return struct.unpack_from(">I", data, pos)[0]


def read_midi_bytes(path: Path) -> bytes:
    """Read either a raw SMF or this client's Jagex-packed .mid file."""
    data = path.read_bytes()
    if data.startswith(b"MThd"):
        return data
    if len(data) < 5:
        raise ValueError(f"{path}: too small to contain RuneScape MIDI data")

    expected = _u32be(data, 0)
    body = data[4:]
    attempts = [body] + [
        b"BZh" + str(level).encode("ascii") + body
        for level in range(1, 10)
    ]
    last_error = None
    for packed in attempts:
        try:
            raw = bz2.decompress(packed)
        except OSError as exc:
            last_error = exc
            continue
        if expected and len(raw) != expected:
            continue
        if raw.startswith(b"MThd"):
            return raw

    raise ValueError(
        f"{path}: could not decode Jagex-packed MIDI"
        + (f" ({last_error})" if last_error else "")
    )


def _vlq(data: bytes, pos: int):
    value = 0
    for _ in range(4):
        if pos >= len(data):
            raise ValueError("truncated MIDI variable-length quantity")
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not (byte & 0x80):
            return value, pos
    raise ValueError("invalid MIDI variable-length quantity")


def _parse_track(track: bytes, track_index: int):
    pos = 0
    tick = 0
    running = None
    order = 0
    events = []

    while pos < len(track):
        delta, pos = _vlq(track, pos)
        tick += delta
        if pos >= len(track):
            break

        first = track[pos]
        if first & 0x80:
            status = first
            pos += 1
            if status < 0xF0:
                running = status
            elif status >= 0xF8 and status != 0xFF:
                pass
            else:
                running = None
        else:
            if running is None:
                raise ValueError(f"track {track_index}: running status without status byte")
            status = running

        if status == 0xFF:
            if pos >= len(track):
                raise ValueError("truncated MIDI meta event")
            meta_type = track[pos]
            pos += 1
            length, pos = _vlq(track, pos)
            payload = track[pos:pos + length]
            if len(payload) != length:
                raise ValueError("truncated MIDI meta payload")
            pos += length
            if meta_type == 0x51 and length == 3:
                tempo = int.from_bytes(payload, "big")
                events.append((tick, track_index, order, "tempo", tempo, 0, 0, 0))
            elif meta_type == 0x2F:
                break
            order += 1
            continue

        if status in (0xF0, 0xF7):
            length, pos = _vlq(track, pos)
            pos += length
            if pos > len(track):
                raise ValueError("truncated MIDI SysEx payload")
            order += 1
            continue

        if status >= 0xF0:
            system_lengths = {
                0xF1: 1, 0xF2: 2, 0xF3: 1, 0xF4: 0,
                0xF5: 0, 0xF6: 0, 0xF8: 0, 0xF9: 0,
                0xFA: 0, 0xFB: 0, 0xFC: 0, 0xFD: 0, 0xFE: 0,
            }
            length = system_lengths.get(status, 0)
            if pos + length > len(track):
                raise ValueError("truncated MIDI system message")
            pos += length
            order += 1
            continue

        kind = status & 0xF0
        channel = status & 0x0F
        length = 1 if kind in (0xC0, 0xD0) else 2
        if pos + length > len(track):
            raise ValueError("truncated MIDI channel event")
        a = track[pos]
        b = track[pos + 1] if length == 2 else 0
        pos += length

        if kind == 0x80:
            event_type = EV_NOTE_OFF
        elif kind == 0x90:
            event_type = EV_NOTE_OFF if b == 0 else EV_NOTE_ON
        elif kind == 0xA0:
            event_type = EV_POLY_PRESSURE
        elif kind == 0xB0:
            event_type = EV_CONTROL
        elif kind == 0xC0:
            event_type = EV_PROGRAM
        elif kind == 0xD0:
            event_type = EV_CHANNEL_PRESSURE
        elif kind == 0xE0:
            event_type = EV_PITCH_BEND
        else:
            order += 1
            continue

        events.append((tick, track_index, order, "midi", event_type, channel, a, b))
        order += 1

    return events, tick


def _parse_midi_bytes(data: bytes):
    if len(data) < 14 or data[:4] != b"MThd":
        raise ValueError("not a Standard MIDI File")
    header_len = _u32be(data, 4)
    if header_len < 6 or 8 + header_len > len(data):
        raise ValueError("invalid MThd length")

    midi_format = _u16be(data, 8)
    track_count = _u16be(data, 10)
    division = _u16be(data, 12)
    if midi_format not in (0, 1):
        raise ValueError(
            f"MIDI format {midi_format} contains independent sequences; only formats 0/1 are supported"
        )
    if division & 0x8000:
        raise ValueError("SMPTE MIDI timing is not supported; expected PPQ")
    if division == 0:
        raise ValueError("MIDI PPQ division is zero")

    pos = 8 + header_len
    all_events = []
    max_tick = 0
    parsed_tracks = 0
    while parsed_tracks < track_count:
        if pos + 8 > len(data):
            raise ValueError("truncated MIDI track header")
        chunk_id = data[pos:pos + 4]
        chunk_len = _u32be(data, pos + 4)
        pos += 8
        payload = data[pos:pos + chunk_len]
        if len(payload) != chunk_len:
            raise ValueError("truncated MIDI track")
        pos += chunk_len

        if chunk_id != b"MTrk":
            continue

        events, end_tick = _parse_track(payload, parsed_tracks)
        all_events.extend(events)
        max_tick = max(max_tick, end_tick)
        parsed_tracks += 1

    all_events.sort(key=lambda e: (e[0], e[1], e[2]))
    return midi_format, division, all_events, max_tick


def _parse_midi(path: Path):
    return _parse_midi_bytes(read_midi_bytes(path))


def events_with_time(path: Path):
    """Return parsed events as (time_us, event_tuple) and total duration."""
    midi_format, ppqn, source_events, max_tick = _parse_midi(path)
    tempo = 500000
    last_tick = 0
    elapsed_num = 0
    timed = []

    for event in source_events:
        tick = event[0]
        elapsed_num += (tick - last_tick) * tempo
        last_tick = tick
        now_us = elapsed_num // ppqn
        if event[3] == "tempo":
            tempo = event[4]
        else:
            timed.append((now_us, event))

    elapsed_num += (max_tick - last_tick) * tempo
    return midi_format, ppqn, timed, elapsed_num // ppqn


def compile_midi(src: Path, dst: Path):
    midi_format, ppqn, timed_events, duration_us = events_with_time(src)
    last_emit_us = 0
    output = []
    counts = {name: 0 for name in EVENT_NAMES.values()}

    for now_us, event in timed_events:
        event_type, channel, a, b = event[4], event[5], event[6], event[7]
        delta_us = now_us - last_emit_us
        while delta_us > 0xFFFFFFFF:
            output.append((0xFFFFFFFF, EV_WAIT, 0, 0, 0))
            delta_us -= 0xFFFFFFFF
            last_emit_us += 0xFFFFFFFF

        output.append((int(delta_us), event_type, channel, a, b))
        last_emit_us = now_us
        counts[EVENT_NAMES[event_type]] += 1

    dst.parent.mkdir(parents=True, exist_ok=True)
    with dst.open("wb") as out:
        out.write(HEADER.pack(MAGIC, VERSION, HEADER.size, len(output), ppqn, duration_us))
        for event in output:
            out.write(EVENT.pack(*event))

    print(f"MIDI:       {src}")
    print(f"Format:     {midi_format}")
    print(f"PPQN:       {ppqn}")
    print(f"Events:     {len(output)}")
    print(f"Duration:   {duration_us / 1_000_000.0:.3f} s")
    for name, count in counts.items():
        if count:
            print(f"  {name:16s} {count}")
    print(f"Wrote:      {dst} ({dst.stat().st_size:,} bytes)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("midi", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    compile_midi(args.midi, args.output)


if __name__ == "__main__":
    main()
