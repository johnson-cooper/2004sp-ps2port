#!/usr/bin/env python3
"""Build one PS2 audio.dat containing hardware-good PS2M music + rev254 SFX.

The container is intentionally uncompressed. Payload bytes remain unchanged so
the PS2 can seek directly into the file and stream the existing formats without
runtime decompression.

Layout (little-endian):
  64-byte header
  music_count * 16-byte direct entries
  sfx_count * 256 * 16-byte direct entries
  16-byte-aligned payloads

Entry:
  u32 offset
  u32 size
  u32 aux0   (SFX trim ticks, music 0)
  u32 aux1   (SFX duration ms, music 0)
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

from wav_to_ps2_adpcm import read_pcm16
from ps2_adpcm import encode_mono_pcm16

MAGIC = b"RS2A"
VERSION = 1
HEADER = struct.Struct("<4sHHIIIIIIII6I")
ENTRY = struct.Struct("<IIII")
HEADER_SIZE = HEADER.size
ENTRY_SIZE = ENTRY.size
SFX_LOOP_SLOTS = 256
SFX_SPU_BYTES = 0x00200000 - 0x001E2000

SOUND_CALL = re.compile(
    r"(?<![A-Za-z0-9_])(?:\.)?sound_synth\s*\(\s*"
    r"([^,\r\n]+?)\s*,\s*([^,\r\n]+?)\s*,",
    re.IGNORECASE,
)
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
TRIM_LINE = re.compile(r"^Trim:\s*(\d+)\s*x\s*20ms\s*$", re.MULTILINE)


def align(value: int, alignment: int = 16) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def parse_synth_pack(path: Path) -> dict[int, str]:
    mapping: dict[int, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        left, right = line.split("=", 1)
        try:
            synth_id = int(left.strip(), 10)
        except ValueError:
            continue
        name = right.strip()
        if not name:
            continue
        if synth_id < 0 or synth_id > 0xFFFF:
            raise ValueError(f"invalid synth id {synth_id} in {path}")
        if synth_id in mapping:
            raise ValueError(f"duplicate synth id {synth_id} in {path}")
        mapping[synth_id] = name
    if not mapping:
        raise ValueError(f"no synth mappings found in {path}")
    return mapping


def index_synth_files(root: Path) -> dict[str, Path]:
    by_name: dict[str, Path] = {}
    duplicates: dict[str, list[Path]] = defaultdict(list)
    for path in root.rglob("*.synth"):
        name = path.stem
        if name in by_name:
            duplicates[name].append(path)
        else:
            by_name[name] = path
    if duplicates:
        lines = ["duplicate .synth basenames found:"]
        for name, paths in sorted(duplicates.items()):
            lines.append(f"  {name}: {by_name[name]}, " + ", ".join(map(str, paths)))
        raise ValueError("\n".join(lines))
    return by_name


def scan_loop_usage(
    content_root: Path,
    names: set[str],
) -> tuple[dict[str, set[int]], set[int], int]:
    direct: dict[str, set[int]] = defaultdict(set)
    dynamic: set[int] = set()
    unresolved = 0

    scripts_root = content_root / "scripts"
    if not scripts_root.is_dir():
        return direct, {0, 1}, 0

    for path in scripts_root.rglob("*.rs2"):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in SOUND_CALL.finditer(text):
            sound_expr = match.group(1).strip()
            loop_expr = match.group(2).strip()
            try:
                loops = int(loop_expr, 10)
            except ValueError:
                unresolved += 1
                continue
            if loops < 0 or loops >= SFX_LOOP_SLOTS:
                unresolved += 1
                continue

            if IDENT.fullmatch(sound_expr) and sound_expr in names:
                direct[sound_expr].add(loops)
            else:
                # Dynamic synth selectors (params/db fields/locals) can resolve
                # to many ids, so retain their literal loop counts globally.
                dynamic.add(loops)

    # Both values are common across original/progressive content and make a
    # safe fallback even for synths only selected dynamically.
    dynamic.update((0, 1))
    return direct, dynamic, unresolved


def compile_exporter(repo_root: Path, output: Path) -> None:
    tcc = repo_root / "bin" / "tcc" / "tcc.exe"
    if not tcc.is_file():
        raise FileNotFoundError(f"TCC not found: {tcc}")

    output.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(tcc),
        "-Isrc",
        "-o",
        str(output),
        "tools/ps2_audio/export_synth_wav.c",
        "src/sound/wave.c",
        "src/sound/tone.c",
        "src/sound/envelope.c",
    ]
    subprocess.run(cmd, cwd=repo_root, check=True)


def export_variant(
    exporter: Path,
    synth: Path,
    wav_path: Path,
    loops: int,
    repo_root: Path,
) -> tuple[bytes, int, int]:
    proc = subprocess.run(
        [str(exporter), str(synth), str(wav_path), str(loops)],
        cwd=repo_root,
        check=True,
        text=True,
        capture_output=True,
    )
    match = TRIM_LINE.search(proc.stdout)
    if not match:
        raise RuntimeError(
            f"exporter did not report trim for {synth} loops={loops}:\n"
            f"{proc.stdout}\n{proc.stderr}"
        )
    trim_ticks = int(match.group(1), 10)

    pcm, rate = read_pcm16(wav_path)
    if not pcm:
        raise RuntimeError(f"{synth} loops={loops} produced no PCM")
    encoded = encode_mono_pcm16(pcm, rate)
    raw_bytes = len(encoded) - 16
    if raw_bytes <= 0 or raw_bytes > SFX_SPU_BYTES:
        raise RuntimeError(
            f"{synth} loops={loops} needs {raw_bytes:,} SPU2 bytes; "
            f"SFX region allows {SFX_SPU_BYTES:,}"
        )
    duration_ms = max(1, (len(pcm) * 1000 + rate - 1) // rate)
    return encoded, trim_ticks, duration_ms


def validate_v1_ps2m(path: Path) -> None:
    with path.open("rb") as file:
        header = file.read(8)
    if len(header) != 8 or header[:4] != b"RSM1":
        raise ValueError(f"{path} is not a PS2M pack")
    version, header_size = struct.unpack_from("<HH", header, 4)
    if version != 1 or header_size != 40:
        raise ValueError(
            f"{path} is PS2M version {version} header={header_size}; "
            "audio.dat requires the hardware-good v1 packs"
        )


def numeric_music_files(root: Path) -> dict[int, Path]:
    result: dict[int, Path] = {}
    if not root.is_dir():
        return result
    for path in root.glob("*.ps2m"):
        try:
            music_id = int(path.stem, 10)
        except ValueError:
            continue
        if music_id < 0 or music_id > 0xFFFF:
            continue
        validate_v1_ps2m(path)
        result[music_id] = path
    return result


def write_padding(out, target: int) -> None:
    pos = out.tell()
    if pos > target:
        raise RuntimeError(f"internal layout overlap: {pos} > {target}")
    if pos < target:
        out.write(b"\0" * (target - pos))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--content",
        type=Path,
        required=True,
        help="LostCityRS/Content rev254 checkout",
    )
    parser.add_argument(
        "--music-dir",
        type=Path,
        default=Path("build/bin/rom/ps2audio"),
        help="directory containing the known-good v1 .ps2m packs",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/bin/rom/audio.dat"),
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=Path("build/ps2-audio-dat-report.txt"),
    )
    parser.add_argument(
        "--keep-temp",
        action="store_true",
        help="keep temporary WAV files for SFX diagnosis",
    )
    args = parser.parse_args()

    repo_root = Path.cwd().resolve()
    content = args.content.resolve()
    music_dir = (repo_root / args.music_dir).resolve() if not args.music_dir.is_absolute() else args.music_dir
    output = (repo_root / args.output).resolve() if not args.output.is_absolute() else args.output
    report_path = (repo_root / args.report).resolve() if not args.report.is_absolute() else args.report

    synth_pack = content / "pack" / "synth.pack"
    synth_root = content / "synth"
    if not synth_pack.is_file():
        raise SystemExit(f"missing rev254 synth map: {synth_pack}")
    if not synth_root.is_dir():
        raise SystemExit(f"missing rev254 synth directory: {synth_root}")

    synth_map = parse_synth_pack(synth_pack)
    synth_files = index_synth_files(synth_root)
    missing = [
        (synth_id, name)
        for synth_id, name in synth_map.items()
        if name not in synth_files
    ]
    if missing:
        preview = "\n".join(f"  {i}={n}" for i, n in missing[:20])
        raise SystemExit(
            f"{len(missing)} mapped synth files are missing:\n{preview}"
        )

    music = numeric_music_files(music_dir)
    if not music:
        raise SystemExit(
            f"no numeric v1 PS2M music packs found in {music_dir}\n"
            "Build the known-good soundtrack first."
        )

    direct_loops, dynamic_loops, unresolved = scan_loop_usage(
        content, set(synth_map.values())
    )

    exporter = repo_root / "build" / "tools" / "export_synth_wav.exe"
    compile_exporter(repo_root, exporter)

    temp_root_obj = tempfile.TemporaryDirectory(
        prefix="ps2_sfx_", dir=repo_root / "build"
    )
    temp_root = Path(temp_root_obj.name)
    wav_path = temp_root / "current.wav"

    sfx_payloads: dict[tuple[int, int], tuple[bytes, int, int]] = {}
    failures: list[str] = []
    optional_skips: list[str] = []
    variants_built = 0

    ids = sorted(synth_map)
    for number, synth_id in enumerate(ids, 1):
        name = synth_map[synth_id]
        variants = set(dynamic_loops)
        variants.update(direct_loops.get(name, ()))
        variants.update((0, 1))

        for loops in sorted(v for v in variants if 0 <= v < SFX_LOOP_SLOTS):
            try:
                encoded, trim_ticks, duration_ms = export_variant(
                    exporter,
                    synth_files[name],
                    wav_path,
                    loops,
                    repo_root,
                )
                sfx_payloads[(synth_id, loops)] = (
                    encoded,
                    trim_ticks,
                    duration_ms,
                )
                variants_built += 1
            except Exception as exc:
                message = f"{synth_id}={name} loops={loops}: {exc}"
                if loops in (0, 1):
                    failures.append(message)
                else:
                    # Keep the complete base catalog buildable even if one
                    # unusually long repeated-loop variant exceeds the fixed
                    # 120 KiB SFX SPU2 window. Runtime falls back to loop 1/0.
                    optional_skips.append(message)

        if number % 25 == 0 or number == len(ids):
            print(
                f"SFX {number:4d}/{len(ids)}  "
                f"variants={variants_built} failures={len(failures)} "
                f"optional-skips={len(optional_skips)}"
            )

    if failures:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(
            "PS2 audio.dat SFX build failures\n"
            "================================\n" +
            "\n".join(failures) + "\n",
            encoding="utf-8",
        )
        raise SystemExit(
            f"{len(failures)} SFX variants failed; see {report_path}"
        )

    music_count = max(music) + 1
    sfx_count = max(synth_map) + 1
    music_table_offset = HEADER_SIZE
    sfx_table_offset = music_table_offset + music_count * ENTRY_SIZE
    sfx_entries_count = sfx_count * SFX_LOOP_SLOTS
    data_offset = align(
        sfx_table_offset + sfx_entries_count * ENTRY_SIZE,
        16,
    )

    music_table = [bytearray(ENTRY_SIZE) for _ in range(music_count)]
    sfx_table = [bytearray(ENTRY_SIZE) for _ in range(sfx_entries_count)]

    output.parent.mkdir(parents=True, exist_ok=True)
    dedupe: dict[bytes, tuple[int, int]] = {}
    dedupe_saved = 0
    payload_count = 0

    with output.open("w+b") as out:
        out.truncate(data_offset)
        out.seek(data_offset)

        for music_id, path in sorted(music.items()):
            pos = align(out.tell(), 16)
            write_padding(out, pos)
            size = path.stat().st_size
            with path.open("rb") as src:
                shutil.copyfileobj(src, out, length=1024 * 1024)
            music_table[music_id][:] = ENTRY.pack(pos, size, 0, 0)
            payload_count += 1

        for (synth_id, loops), (blob, trim_ticks, duration_ms) in sorted(
            sfx_payloads.items()
        ):
            digest = hashlib.sha256(blob).digest()
            existing = dedupe.get(digest)
            if existing is not None and existing[1] == len(blob):
                pos, size = existing
                dedupe_saved += size
            else:
                pos = align(out.tell(), 16)
                write_padding(out, pos)
                out.write(blob)
                size = len(blob)
                dedupe[digest] = (pos, size)
                payload_count += 1

            index = synth_id * SFX_LOOP_SLOTS + loops
            sfx_table[index][:] = ENTRY.pack(
                pos,
                size,
                trim_ticks,
                duration_ms,
            )

        file_size = out.tell()

        out.seek(0)
        out.write(
            HEADER.pack(
                MAGIC,
                VERSION,
                HEADER_SIZE,
                music_count,
                sfx_count,
                SFX_LOOP_SLOTS,
                ENTRY_SIZE,
                music_table_offset,
                sfx_table_offset,
                data_offset,
                file_size,
                0, 0, 0, 0, 0, 0,
            )
        )
        for rec in music_table:
            out.write(rec)
        for rec in sfx_table:
            out.write(rec)

    if args.keep_temp:
        keep = repo_root / "build" / "diagnostics" / "sfx_audio_dat_temp"
        if keep.exists():
            shutil.rmtree(keep)
        shutil.copytree(temp_root, keep)
    temp_root_obj.cleanup()

    report_lines = [
        "PS2 audio.dat build report",
        "==========================",
        f"Output:              {output}",
        f"File size:           {output.stat().st_size:,} bytes",
        f"Music packs:         {len(music)}",
        f"Music table slots:   {music_count}",
        f"Synth ids:           {len(synth_map)}",
        f"SFX table slots:     {sfx_entries_count}",
        f"SFX variants built:  {variants_built}",
        f"Unique SFX blobs:    {len(dedupe)}",
        f"SFX dedupe saving:   {dedupe_saved:,} bytes",
        f"Optional variants skipped: {len(optional_skips)}",
        f"Dynamic loop values: {sorted(dynamic_loops)}",
        f"Unresolved loop args: {unresolved}",
        f"Payload writes:      {payload_count}",
    ]
    if file_size > 0x7FFFFFFF:
        output.unlink(missing_ok=True)
        raise SystemExit(
            f"audio.dat is {file_size:,} bytes; PS2 runtime requires < 2 GiB"
        )

    if optional_skips:
        report_lines.append("")
        report_lines.append("Optional loop variants skipped (runtime falls back to 1/0):")
        report_lines.extend(f"  {line}" for line in optional_skips)

    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(report_lines) + "\n", encoding="utf-8")

    print()
    print("\n".join(report_lines))
    print()
    print("USB deployment:")
    print("  rom\\audio.dat")
    print("The loose ps2audio/ and ps2sfx/ directories are not required by")
    print("the DAT-capable runtime, but can be retained as debugging fallbacks.")


if __name__ == "__main__":
    main()
