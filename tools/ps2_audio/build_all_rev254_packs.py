#!/usr/bin/env python3
"""Build accurate PS2 SoundFont packs for every rev254 MIDI present locally."""

from __future__ import annotations

import argparse
from pathlib import Path

from build_title_music import build_pack


def canonical(name: str) -> str:
    return "".join(ch.lower() for ch in name if ch.isalnum())


def read_mapping(path: Path):
    rows = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        left, sep, right = line.partition("=")
        if not sep:
            raise ValueError(f"invalid mapping line: {raw!r}")
        rows.append((int(left), right.strip()))
    return rows


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--soundfont",
        type=Path,
        default=Path("rom/SCC1_Florestan.sf2"),
    )
    parser.add_argument(
        "--songs",
        type=Path,
        default=Path("rom/cache/client/songs"),
    )
    parser.add_argument(
        "--mapping",
        type=Path,
        default=Path("tools/ps2_audio/rev254_midi.pack"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/bin/rom/ps2audio"),
    )
    parser.add_argument(
        "--keep-going",
        action="store_true",
        help="continue after an individual pack fails",
    )
    args = parser.parse_args()

    if not args.soundfont.is_file():
        raise SystemExit(f"SoundFont not found: {args.soundfont}")
    if not args.songs.is_dir():
        raise SystemExit(f"song directory not found: {args.songs}")

    by_key: dict[str, list[Path]] = {}
    for midi in sorted(args.songs.glob("*.mid")):
        by_key.setdefault(canonical(midi.stem), []).append(midi)

    built = 0
    missing = 0
    failed = 0
    args.output.mkdir(parents=True, exist_ok=True)

    for midi_id, name in read_mapping(args.mapping):
        if name == "null":
            continue

        matches = by_key.get(canonical(name), [])
        if len(matches) != 1:
            print(
                f"SKIP id={midi_id:3d} name={name!r}: "
                f"local matches={len(matches)}"
            )
            missing += 1
            continue

        midi = matches[0]
        out = args.output / f"{midi_id}.ps2m"
        print()
        print("=" * 72)
        print(f"ID {midi_id}: {name} <- {midi.name}")
        print("=" * 72)

        try:
            build_pack(args.soundfont, midi, out)
            built += 1
        except Exception as exc:
            failed += 1
            print(f"FAILED id={midi_id} name={name!r}: {exc}")
            if not args.keep_going:
                raise

    print()
    print("=" * 72)
    print(f"Built:   {built}")
    print(f"Missing: {missing}")
    print(f"Failed:  {failed}")
    print(f"Output:  {args.output}")
    print("=" * 72)

    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
