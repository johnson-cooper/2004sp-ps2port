#!/usr/bin/env python3
"""Compare a hardware-good PS2 ELF with an SFX candidate.

The PS2 port has repeatedly shown sensitivity to ordinary EE layout changes.
This gate treats normal data/BSS addresses and sizes as critical while
reporting text/ctor movement separately. The isolated .ps2_audio_* overlay is
allowed to grow.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

ELF_HDR_MIN = 52
SHT = struct.Struct("<IIIIIIIIII")
PHDR = struct.Struct("<IIIIIIII")
PT_LOAD = 1

CRITICAL = (
    ".data",
    ".rodata",
    ".rdata",
    ".gcc_except_table",
    ".lit4",
    ".lit8",
    ".sdata",
    ".sbss",
    ".bss",
)

WATCH = (
    ".text",
    ".ctors",
    ".dtors",
    ".reginfo",
    ".MIPS.abiflags",
)

AUDIO_PREFIX = ".ps2_audio_"


@dataclass(frozen=True)
class Section:
    name: str
    addr: int
    offset: int
    size: int
    flags: int


@dataclass(frozen=True)
class Load:
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    flags: int
    align: int


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def cstr(data: bytes, off: int) -> str:
    end = data.find(b"\0", off)
    if end < 0:
        end = len(data)
    return data[off:end].decode("ascii", "replace")


def parse_elf(path: Path) -> tuple[dict[str, Section], list[Load]]:
    data = path.read_bytes()
    if len(data) < ELF_HDR_MIN or data[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF file")
    if data[4] != 1:
        raise ValueError(f"{path}: expected ELF32")
    if data[5] != 1:
        raise ValueError(f"{path}: expected little-endian ELF")

    phoff = u32(data, 0x1C)
    shoff = u32(data, 0x20)
    phentsize = u16(data, 0x2A)
    phnum = u16(data, 0x2C)
    shentsize = u16(data, 0x2E)
    shnum = u16(data, 0x30)
    shstrndx = u16(data, 0x32)

    if shentsize < SHT.size or shstrndx >= shnum:
        raise ValueError(f"{path}: invalid section table")

    raw_sections = []
    for i in range(shnum):
        off = shoff + i * shentsize
        if off + SHT.size > len(data):
            raise ValueError(f"{path}: truncated section table")
        raw_sections.append(SHT.unpack_from(data, off))

    shstr = raw_sections[shstrndx]
    str_off, str_size = shstr[4], shstr[5]
    names = data[str_off:str_off + str_size]

    sections: dict[str, Section] = {}
    for rec in raw_sections:
        name_off, _typ, flags, addr, off, size, *_rest = rec
        name = cstr(names, name_off) if name_off < len(names) else ""
        if name:
            sections[name] = Section(name, addr, off, size, flags)

    loads: list[Load] = []
    if phentsize >= PHDR.size:
        for i in range(phnum):
            off = phoff + i * phentsize
            if off + PHDR.size > len(data):
                raise ValueError(f"{path}: truncated program headers")
            typ, p_off, vaddr, _paddr, filesz, memsz, flags, align = PHDR.unpack_from(data, off)
            if typ == PT_LOAD:
                loads.append(Load(p_off, vaddr, filesz, memsz, flags, align))

    return sections, loads


def fmt(value: int) -> str:
    return f"0x{value:08x}"


def compare_section(
    name: str,
    good: dict[str, Section],
    new: dict[str, Section],
    strict: bool,
) -> bool:
    a = good.get(name)
    b = new.get(name)
    if a is None and b is None:
        return True
    if a is None or b is None:
        print(f"FAIL {name:20} present-good={a is not None} present-new={b is not None}")
        return False

    same_addr = a.addr == b.addr
    same_size = a.size == b.size
    status = "OK" if same_addr and (same_size or not strict) else "FAIL"
    delta = b.size - a.size
    print(
        f"{status:4} {name:20} "
        f"addr {fmt(a.addr)} -> {fmt(b.addr)}  "
        f"size {a.size:8d} -> {b.size:8d}  "
        f"delta {delta:+d}"
    )
    return same_addr and (same_size or not strict)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("hardware_good", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()

    good, good_loads = parse_elf(args.hardware_good)
    new, new_loads = parse_elf(args.candidate)

    print("Critical normal data/BSS layout:")
    critical_ok = True
    for name in CRITICAL:
        critical_ok &= compare_section(name, good, new, strict=True)

    print("\nNormal code/metadata watch:")
    watch_addr_ok = True
    for name in WATCH:
        watch_addr_ok &= compare_section(name, good, new, strict=False)

    print("\nAudio overlay:")
    audio_names = sorted(
        {
            name
            for name in set(good) | set(new)
            if name.startswith(AUDIO_PREFIX)
        }
    )
    for name in audio_names:
        a = good.get(name)
        b = new.get(name)
        if a and b:
            print(
                f"INFO {name:20} "
                f"addr {fmt(a.addr)} -> {fmt(b.addr)}  "
                f"size {a.size:8d} -> {b.size:8d}  "
                f"delta {b.size - a.size:+d}"
            )
        else:
            print(
                f"INFO {name:20} "
                f"present-good={a is not None} present-new={b is not None}"
            )

    normal_good_loads = sorted(
        (p for p in good_loads if p.vaddr < 0x01FC0000),
        key=lambda p: p.vaddr,
    )
    normal_new_loads = sorted(
        (p for p in new_loads if p.vaddr < 0x01FC0000),
        key=lambda p: p.vaddr,
    )
    print("\nNormal PT_LOAD:")
    load_ok = len(normal_good_loads) == len(normal_new_loads)
    if not load_ok:
        print(
            f"FAIL normal load count "
            f"{len(normal_good_loads)} -> {len(normal_new_loads)}"
        )
    else:
        for i, (a, b) in enumerate(zip(normal_good_loads, normal_new_loads)):
            same_base = a.vaddr == b.vaddr
            print(
                f"{'OK' if same_base else 'FAIL':4} load[{i}] "
                f"vaddr {fmt(a.vaddr)} -> {fmt(b.vaddr)}  "
                f"filesz {a.filesz} -> {b.filesz} ({b.filesz-a.filesz:+d})  "
                f"memsz {a.memsz} -> {b.memsz} ({b.memsz-a.memsz:+d})"
            )
            load_ok &= same_base

    print()
    if critical_ok and watch_addr_ok and load_ok:
        print("LAYOUT GATE: PASS")
        print("Critical normal addresses/sizes are preserved.")
        raise SystemExit(0)

    print("LAYOUT GATE: FAIL")
    if not critical_ok:
        print("Critical data/BSS layout changed. Do NOT hardware-test this ELF.")
    if not watch_addr_ok:
        print("One or more watched normal section start addresses moved.")
    if not load_ok:
        print("Normal PT_LOAD base/count changed.")
    raise SystemExit(1)


if __name__ == "__main__":
    main()
