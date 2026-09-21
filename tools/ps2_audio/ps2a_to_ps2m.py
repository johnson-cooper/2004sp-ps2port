#!/usr/bin/env python3
"""Wrap one audsrv APCM sample in the existing hardware-good PS2M v1 format."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

HEADER = struct.Struct("<4sHHIIIIIIQ")
SAMPLE_REC = struct.Struct("<II")
EVENT_REC = struct.Struct("<IBBBBHHBbH")

MAGIC = b"RSM1"
VERSION = 1
OUT_NOTE_ON = 1
OUT_NOTE_OFF = 2

APCM = struct.Struct("<4sBBBBII")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="input audsrv APCM/PS2 ADPCM file")
    parser.add_argument("output", type=Path, help="output PS2M v1 file")
    parser.add_argument("--sample-rate", type=int, default=22050)
    parser.add_argument("--synth-id", type=int, default=-1)
    parser.add_argument("--mapped-id", type=int, default=-1)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()

    blob = args.input.read_bytes()
    if len(blob) <= APCM.size:
        raise SystemExit("APCM input is too small")

    magic, version, channels, looped, _reserved, pitch, sample_count = APCM.unpack_from(blob)
    if magic != b"APCM":
        raise SystemExit("input is not an APCM sample")
    if version != 1 or channels != 1:
        raise SystemExit(f"unsupported APCM format version={version} channels={channels}")
    if looped:
        raise SystemExit("SFX proof expects a one-shot non-looping APCM sample")
    if args.sample_rate <= 0:
        raise SystemExit("sample rate must be positive")
    if not (1 <= pitch <= 0x3FFF):
        raise SystemExit(f"invalid APCM pitch: {pitch}")
    if sample_count <= 0:
        raise SystemExit("APCM sample count is zero")

    duration_us = max(1, round(sample_count * 1_000_000 / args.sample_rate))
    duration_ms = max(1, (duration_us + 999) // 1000)

    # One logical layer on channel 15/token 1. The existing PS2M runtime maps
    # this through the same NOTE_ON/NOTE_OFF path already proven by music.
    events = [
        (0, OUT_NOTE_ON, 15, 1, 0, 0, pitch, 100, 0, 0),
        (duration_us, OUT_NOTE_OFF, 15, 1, 0, 0xFFFF, 0, 0, 0, 0),
    ]

    sample_table_offset = HEADER.size
    event_table_offset = sample_table_offset + SAMPLE_REC.size
    data_offset = event_table_offset + len(events) * EVENT_REC.size
    data_size = len(blob)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as out:
        out.write(HEADER.pack(
            MAGIC,
            VERSION,
            HEADER.size,
            1,
            len(events),
            sample_table_offset,
            event_table_offset,
            data_offset,
            data_size,
            duration_us,
        ))
        out.write(SAMPLE_REC.pack(data_offset, len(blob)))
        for event in events:
            out.write(EVENT_REC.pack(*event))
        out.write(blob)

    print(f"Input APCM:   {args.input}")
    print(f"Pitch:        {pitch}")
    print(f"Samples:      {sample_count:,}")
    print(f"Duration:     {duration_us / 1_000_000.0:.3f} s")
    print(f"Jingle delay: {duration_ms} ms")
    print(f"PS2M v1:      {args.output}")
    print(f"Pack bytes:   {args.output.stat().st_size:,}")

    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(
            json.dumps(
                {
                    "synth_id": args.synth_id,
                    "mapped_midi_id": args.mapped_id,
                    "duration_ms": duration_ms,
                    "sample_rate": args.sample_rate,
                    "pitch": pitch,
                    "sample_count": sample_count,
                    "ps2m": str(args.output),
                },
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )
        print(f"Manifest:     {args.manifest}")


if __name__ == "__main__":
    main()
