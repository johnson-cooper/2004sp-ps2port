#!/usr/bin/env python3
"""Analyze PS2M soundtrack size and estimate cross-pack ADPCM deduplication."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

HEADER = struct.Struct("<4sHHIIIIIIQ")
SAMPLE_REC = struct.Struct("<II")
EVENT_REC_BYTES = 16
MAGIC = b"RSM1"


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

        if magic != MAGIC or version != 1 or header_size != HEADER.size:
            print(f"SKIP {path.name}: unsupported header")
            continue

        sample_table_bytes = sample_count * SAMPLE_REC.size
        event_table_bytes = event_count * EVENT_REC_BYTES

        total_headers += header_size
        total_sample_tables += sample_table_bytes
        total_event_tables += event_table_bytes
        total_sample_data += data_size

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

    print(f"Packs:                     {len(files)}")
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
