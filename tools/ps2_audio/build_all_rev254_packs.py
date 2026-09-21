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


def index_midis(*directories: Path) -> dict[str, list[Path]]:
    by_key: dict[str, list[Path]] = {}
    for directory in directories:
        if not directory.is_dir():
            continue
        for midi in sorted(directory.glob("*.mid")):
            by_key.setdefault(canonical(midi.stem), []).append(midi)
    return by_key


def failure_category(exc: Exception) -> str:
    message = str(exc)
    if message.startswith("song sample set needs "):
        return "SPU2 sample budget"
    if message.startswith("more than 256 overlapping accurate-pack note layers"):
        return "overlapping note-layer limit"
    if message == "too many title-song sample variants":
        return "sample-variant limit"
    if message.startswith("SF2 sample ") and message.endswith(" exceeds smpl data"):
        return "SoundFont sample bounds"
    return type(exc).__name__


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
        "--content",
        type=Path,
        help=(
            "optional LostCityRS/Content checkout; missing local MIDIs are "
            "resolved from its songs/ and jingles/ directories"
        ),
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
        "--failure-report",
        type=Path,
        default=Path("build/ps2-audio-failures.txt"),
        help="write a categorized per-song failure report here",
    )
    parser.add_argument(
        "--compact-events",
        action="store_true",
        help="write lossless variable-length PS2M v2 events",
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

    local_by_key = index_midis(args.songs)
    content_by_key: dict[str, list[Path]] = {}
    if args.content is not None:
        if not args.content.is_dir():
            raise SystemExit(f"Content checkout not found: {args.content}")

        content_songs = args.content / "songs"
        content_jingles = args.content / "jingles"
        if not content_songs.is_dir() or not content_jingles.is_dir():
            raise SystemExit(
                "Content checkout must contain both songs/ and jingles/: "
                f"{args.content}"
            )
        content_by_key = index_midis(content_songs, content_jingles)

    built = 0
    built_local = 0
    built_content = 0
    missing = 0
    failed = 0
    failures: list[tuple[int, str, str, Path, str, str]] = []
    args.output.mkdir(parents=True, exist_ok=True)

    for midi_id, name in read_mapping(args.mapping):
        if name == "null":
            continue

        key = canonical(name)
        local_matches = local_by_key.get(key, [])
        content_matches = content_by_key.get(key, [])

        source = "local"
        matches = local_matches
        if len(local_matches) == 0 and args.content is not None:
            source = "content"
            matches = content_matches

        if len(matches) != 1:
            detail = f"local matches={len(local_matches)}"
            if args.content is not None:
                detail += f", content matches={len(content_matches)}"
            print(
                f"SKIP id={midi_id:3d} name={name!r}: {detail}"
            )
            missing += 1
            continue

        midi = matches[0]
        out = args.output / f"{midi_id}.ps2m"
        print()
        print("=" * 72)
        print(f"ID {midi_id}: {name} <- {midi} [{source}]")
        print("=" * 72)

        try:
            build_pack(
                args.soundfont,
                midi,
                out,
                compact_events=args.compact_events,
            )
            built += 1
            if source == "local":
                built_local += 1
            else:
                built_content += 1
        except Exception as exc:
            # A few cache-extracted local MIDIs are damaged while the
            # corresponding Content/254 source MIDI is clean. Preserve local
            # precedence for normal conversion errors, but recover a decode
            # failure from the exact-name Content fallback when available.
            decode_failure = "could not decode Jagex-packed MIDI" in str(exc)
            if (
                source == "local"
                and decode_failure
                and len(content_matches) == 1
            ):
                fallback = content_matches[0]
                print(
                    f"RETRY id={midi_id} name={name!r}: "
                    f"local MIDI decode failed; using {fallback} [content]"
                )
                try:
                    build_pack(
                        args.soundfont,
                        fallback,
                        out,
                        compact_events=args.compact_events,
                    )
                    built += 1
                    built_content += 1
                    continue
                except Exception as fallback_exc:
                    source = "content"
                    midi = fallback
                    exc = fallback_exc

            failed += 1
            category = failure_category(exc)
            failures.append(
                (midi_id, name, source, midi, category, str(exc))
            )
            print(
                f"FAILED id={midi_id} name={name!r} "
                f"category={category!r}: {exc}"
            )
            if not args.keep_going:
                raise

    print()
    print("=" * 72)
    print(f"Built:   {built}")
    print(f"  local:   {built_local}")
    print(f"  content: {built_content}")
    print(f"Missing: {missing}")
    print(f"Failed:  {failed}")
    print(f"Format:  PS2M v{2 if args.compact_events else 1}")
    print(f"Output:  {args.output}")

    if failures:
        counts: dict[str, int] = {}
        for _, _, _, _, category, _ in failures:
            counts[category] = counts.get(category, 0) + 1

        print("Failure categories:")
        for category, count in sorted(
            counts.items(), key=lambda item: (-item[1], item[0])
        ):
            print(f"  {count:3d}  {category}")

        report_lines = [
            "2004sp PS2 rev254 audio pack failure report",
            "",
            f"Failed: {len(failures)}",
            "",
            "Failure categories:",
        ]
        for category, count in sorted(
            counts.items(), key=lambda item: (-item[1], item[0])
        ):
            report_lines.append(f"  {count:3d}  {category}")

        report_lines.extend(["", "Per-song failures:"])
        for midi_id, name, source, midi, category, message in failures:
            report_lines.append(
                f"id={midi_id:3d} source={source:7s} "
                f"category={category} name={name!r}"
            )
            report_lines.append(f"  midi={midi}")
            report_lines.append(f"  error={message}")

        args.failure_report.parent.mkdir(parents=True, exist_ok=True)
        args.failure_report.write_text(
            "\n".join(report_lines) + "\n",
            encoding="utf-8",
        )
        print(f"Failure report: {args.failure_report}")

    print("=" * 72)

    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
