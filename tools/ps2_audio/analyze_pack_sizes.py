#!/usr/bin/env python3
"""Analyze PS2M soundtrack size and estimate cross-pack ADPCM deduplication."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

HEADER = struct.Struct("<4sHHIIIIIIQ")
SAMPLE_REC = struct.Struct("<II")
EVENT_REC = struct.Struct("<IBBBBHHBbH")
EVENT_REC_BYTES = EVENT_REC.size
MAGIC = b"RSM1"

OUT_WAIT = 0
OUT_NOTE_ON = 1
OUT_NOTE_OFF = 2
OUT_SUSTAIN = 3
OUT_PITCH = 4
OUT_MIX = 5

EVENT_NAMES = {
    OUT_WAIT: "WAIT",
    OUT_NOTE_ON: "NOTE_ON",
    OUT_NOTE_OFF: "NOTE_OFF",
    OUT_SUSTAIN: "SUSTAIN",
    OUT_PITCH: "PITCH",
    OUT_MIX: "MIX",
}


def uleb_size(value: int) -> int:
    size = 1
    while value >= 0x80:
        value >>= 7
        size += 1
    return size


def compact_event_size(event: tuple[int, ...]) -> int:
    delta, kind, _channel, _note, _value, _sample, _pitch, _volume, _pan, _aux = event
    # Compact v2 estimate: ULEB128 delta + one byte containing kind/channel,
    # followed only by the fields each runtime event actually consumes.
    base = uleb_size(delta) + 1
    if kind == OUT_WAIT:
        return base
    if kind == OUT_NOTE_ON:
        return base + 1 + 2 + 2 + 1 + 1
    if kind == OUT_NOTE_OFF:
        return base + 1
    if kind == OUT_SUSTAIN:
        return base + 1
    if kind == OUT_PITCH:
        return base + 1 + 2
    if kind == OUT_MIX:
        return base + 1 + 1 + 1
    return EVENT_REC_BYTES


def parse_compact_events(
    data: bytes,
    start: int,
    end: int,
    event_count: int,
) -> dict[int, int]:
    counts: dict[int, int] = {}
    pos = start
    payload_sizes = {
        OUT_WAIT: 0,
        OUT_NOTE_ON: 7,
        OUT_NOTE_OFF: 1,
        OUT_SUSTAIN: 1,
        OUT_PITCH: 3,
        OUT_MIX: 3,
    }

    for _ in range(event_count):
        for index in range(5):
            if pos >= end:
                raise ValueError("truncated compact event delta")
            byte = data[pos]
            pos += 1
            if index == 4 and (byte & 0xF0):
                raise ValueError("compact event delta exceeds uint32")
            if not (byte & 0x80):
                break
        else:
            raise ValueError("invalid compact event delta")

        if pos >= end:
            raise ValueError("truncated compact event tag")
        tag = data[pos]
        pos += 1
        kind = tag >> 4
        payload = payload_sizes.get(kind)
        if payload is None:
            raise ValueError(f"unknown compact event kind {kind}")
        if payload > end - pos:
            raise ValueError("truncated compact event payload")
        pos += payload
        counts[kind] = counts.get(kind, 0) + 1

    if pos != end:
        raise ValueError(
            f"compact event table has {end - pos} trailing bytes"
        )
    return counts


def human(value: int) -> str:
    units = ("B", "KiB", "MiB", "GiB")
    amount = float(value)
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            return f"{amount:.2f} {unit}"
        amount /= 1024.0
    return f"{value} B"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "directory",
        type=Path,
        nargs="?",
        default=Path("build/bin/rom/ps2audio"),
    )
    args = parser.parse_args()

    files = sorted(args.directory.glob("*.ps2m"))
    if not files:
        raise SystemExit(f"no .ps2m files found in {args.directory}")

    total_files = 0
    total_headers = 0
    total_sample_tables = 0
    total_event_tables = 0
    total_sample_data = 0
    unique_sample_data = 0
    unique: dict[bytes, int] = {}
    sample_refs = 0
    event_kind_counts: dict[int, int] = {}
    compact_event_bytes = 0
    version_counts: dict[int, int] = {}

    for path in files:
        data = path.read_bytes()
        total_files += len(data)
        if len(data) < HEADER.size:
            print(f"SKIP {path.name}: too small")
            continue

        (
            magic,
            version,
            header_size,
            sample_count,
            event_count,
            sample_table_offset,
            event_table_offset,
            data_offset,
            data_size,
            _duration_us,
        ) = HEADER.unpack_from(data)

        if magic != MAGIC or version not in (1, 2) or header_size != HEADER.size:
            print(f"SKIP {path.name}: unsupported header")
            continue

        version_counts[version] = version_counts.get(version, 0) + 1
        sample_table_bytes = sample_count * SAMPLE_REC.size
        if version == 1:
            event_table_bytes = event_count * EVENT_REC_BYTES
        else:
            event_table_bytes = data_offset - event_table_offset

        total_headers += header_size
        total_sample_tables += sample_table_bytes
        total_event_tables += event_table_bytes
        total_sample_data += data_size

        if version == 1:
            for index in range(event_count):
                pos = event_table_offset + index * EVENT_REC_BYTES
                event = EVENT_REC.unpack_from(data, pos)
                kind = event[1]
                event_kind_counts[kind] = event_kind_counts.get(kind, 0) + 1
                compact_event_bytes += compact_event_size(event)
        else:
            counts = parse_compact_events(
                data, event_table_offset, data_offset, event_count
            )
            for kind, count in counts.items():
                event_kind_counts[kind] = (
                    event_kind_counts.get(kind, 0) + count
                )
            compact_event_bytes += event_table_bytes

        for index in range(sample_count):
            pos = sample_table_offset + index * SAMPLE_REC.size
            offset, size = SAMPLE_REC.unpack_from(data, pos)
            blob = data[offset:offset + size]
            digest = hashlib.sha256(blob).digest()
            sample_refs += 1
            if digest not in unique:
                unique[digest] = size
                unique_sample_data += size

    structural = total_headers + total_sample_tables + total_event_tables
    duplicate_sample_bytes = total_sample_data - unique_sample_data
    theoretical_shared = structural + unique_sample_data
    compact_total = (
        total_headers
        + total_sample_tables
        + compact_event_bytes
        + total_sample_data
    )

    print(f"Packs:                     {len(files)}")
    if version_counts:
        versions = ", ".join(
            f"v{version}={count}"
            for version, count in sorted(version_counts.items())
        )
        print(f"Pack formats:              {versions}")
    print(f"Current total:             {human(total_files)}")
    print(f"Headers:                   {human(total_headers)}")
    print(f"Sample tables:             {human(total_sample_tables)}")
    print(f"Event tables:              {human(total_event_tables)}")
    print(f"Embedded ADPCM samples:    {human(total_sample_data)}")
    print(f"Sample references:         {sample_refs}")
    print(f"Unique ADPCM blobs:        {len(unique)}")
    print(f"Unique ADPCM bytes:        {human(unique_sample_data)}")
    print(f"Duplicate ADPCM bytes:     {human(duplicate_sample_bytes)}")
    print()
    print("Event kinds:")
    total_events = sum(event_kind_counts.values())
    for kind, count in sorted(event_kind_counts.items()):
        name = EVENT_NAMES.get(kind, f"kind_{kind}")
        pct = (count * 100.0 / total_events) if total_events else 0.0
        print(f"  {name:10s} {count:10d}  {pct:5.1f}%")
    print(f"Total events:              {total_events}")
    print(f"Current event bytes:       {human(total_event_tables)}")
    print(f"Compact-v2 event estimate: {human(compact_event_bytes)}")
    if total_event_tables:
        event_saving = total_event_tables - compact_event_bytes
        event_pct = event_saving * 100.0 / total_event_tables
        print(
            f"Event-format saving:       {human(event_saving)} "
            f"({event_pct:.1f}%)"
        )
    print(f"Compact-v2 total estimate: {human(compact_total)}")
    print()
    print(
        "Ideal shared-sample floor: "
        f"{human(theoretical_shared)} "
        "(before shared-bank index overhead)"
    )
    if total_files:
        saving = total_files - theoretical_shared
        pct = saving * 100.0 / total_files
        print(f"Potential dedupe saving:   {human(saving)} ({pct:.1f}%)")


if __name__ == "__main__":
    main()
