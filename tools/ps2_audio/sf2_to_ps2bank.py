#!/usr/bin/env python3
"""Convert an SF2's sample pool + MIDI mapping metadata to a PS2-oriented bank.

This is an offline tool. It deliberately performs no work on the PS2 EE.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from ps2_adpcm import encode_mono_pcm16, quantize_loop

RIFF_HEADER = struct.Struct("<4sI4s")
SHDR = struct.Struct("<20sIIIIIBbHH")
INST = struct.Struct("<20sH")
PHDR = struct.Struct("<20sHHHIII")
BAG = struct.Struct("<HH")
GEN = struct.Struct("<HH")

BANK_HEADER = struct.Struct("<4sHHIIIIII")
BANK_ENTRY = struct.Struct("<IIIIIIIIBbH28s")
BANK_MAGIC = b"RSAB"
BANK_VERSION = 1
AUDSRV_SPU2_FIRST_SAMPLE = 0x5010
SPU2_BYTES = 2 * 1024 * 1024

GENERATOR_NAMES = {
    0: "startAddrsOffset",
    1: "endAddrsOffset",
    2: "startloopAddrsOffset",
    3: "endloopAddrsOffset",
    4: "startAddrsCoarseOffset",
    12: "endAddrsCoarseOffset",
    17: "pan",
    33: "delayVolEnv",
    34: "attackVolEnv",
    35: "holdVolEnv",
    36: "decayVolEnv",
    37: "sustainVolEnv",
    38: "releaseVolEnv",
    41: "instrument",
    43: "keyRange",
    44: "velRange",
    45: "startloopAddrsCoarseOffset",
    46: "keynum",
    47: "velocity",
    48: "initialAttenuation",
    50: "endloopAddrsCoarseOffset",
    51: "coarseTune",
    52: "fineTune",
    53: "sampleID",
    54: "sampleModes",
    56: "scaleTuning",
    57: "exclusiveClass",
    58: "overridingRootKey",
}


def _name(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin-1", errors="replace").strip()


def _chunks(data: bytes, start: int, end: int):
    pos = start
    while pos + 8 <= end:
        chunk_id = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        payload = pos + 8
        payload_end = payload + size
        if payload_end > end:
            raise ValueError(f"chunk {chunk_id!r} extends past parent")
        yield chunk_id, payload, size
        pos = payload_end + (size & 1)


def _find_sf2_sections(data: bytes):
    if len(data) < 12:
        raise ValueError("file is too small to be an SF2")
    magic, size, kind = RIFF_HEADER.unpack_from(data, 0)
    if magic != b"RIFF" or kind != b"sfbk":
        raise ValueError("not a RIFF SoundFont (sfbk)")

    riff_end = min(len(data), 8 + size)
    smpl = None
    pdta = {}

    for chunk_id, payload, chunk_size in _chunks(data, 12, riff_end):
        if chunk_id != b"LIST" or chunk_size < 4:
            continue
        list_type = data[payload:payload + 4]
        list_start = payload + 4
        list_end = payload + chunk_size
        if list_type == b"sdta":
            for sub_id, sub_payload, sub_size in _chunks(data, list_start, list_end):
                if sub_id == b"smpl":
                    smpl = (sub_payload, sub_size)
        elif list_type == b"pdta":
            for sub_id, sub_payload, sub_size in _chunks(data, list_start, list_end):
                pdta[sub_id.decode("ascii", errors="replace")] = data[
                    sub_payload:sub_payload + sub_size
                ]

    if smpl is None:
        raise ValueError("SF2 has no sdta/smpl PCM chunk")
    if "shdr" not in pdta:
        raise ValueError("SF2 has no pdta/shdr sample headers")
    return smpl, pdta


def _records(blob: bytes, st: struct.Struct):
    if len(blob) % st.size:
        raise ValueError(f"malformed pdta chunk: {len(blob)} is not a multiple of {st.size}")
    return [st.unpack_from(blob, pos) for pos in range(0, len(blob), st.size)]


def _generator(op: int, amount: int):
    signed = amount - 0x10000 if amount & 0x8000 else amount
    out = {
        "op": op,
        "name": GENERATOR_NAMES.get(op, f"generator_{op}"),
        "amount_u16": amount,
        "amount_s16": signed,
    }
    if op in (43, 44):
        out["range"] = [amount & 0xFF, (amount >> 8) & 0xFF]
    return out


def _zones(first_bag: int, next_bag: int, bags, generators):
    result = []
    next_bag = min(next_bag, max(0, len(bags) - 1))
    for bag_index in range(first_bag, next_bag):
        if bag_index + 1 >= len(bags):
            break
        gen_start = bags[bag_index][0]
        gen_end = bags[bag_index + 1][0]
        gen_end = min(gen_end, len(generators))
        result.append({
            "bag_index": bag_index,
            "generators": [_generator(*g) for g in generators[gen_start:gen_end]],
        })
    return result


def _parse_mapping(pdta):
    instruments = []
    presets = []

    inst = _records(pdta.get("inst", b""), INST)
    ibag = _records(pdta.get("ibag", b""), BAG)
    igen = _records(pdta.get("igen", b""), GEN)
    if len(inst) >= 2 and ibag:
        for i in range(len(inst) - 1):
            instruments.append({
                "index": i,
                "name": _name(inst[i][0]),
                "zones": _zones(inst[i][1], inst[i + 1][1], ibag, igen),
            })

    phdr = _records(pdta.get("phdr", b""), PHDR)
    pbag = _records(pdta.get("pbag", b""), BAG)
    pgen = _records(pdta.get("pgen", b""), GEN)
    if len(phdr) >= 2 and pbag:
        for i in range(len(phdr) - 1):
            name, program, bank, bag_index, library, genre, morphology = phdr[i]
            presets.append({
                "index": i,
                "name": _name(name),
                "program": program,
                "bank": bank,
                "library": library,
                "genre": genre,
                "morphology": morphology,
                "zones": _zones(bag_index, phdr[i + 1][3], pbag, pgen),
            })

    return instruments, presets


def convert(sf2_path: Path, out_dir: Path):
    data = sf2_path.read_bytes()
    (smpl_offset, smpl_size), pdta = _find_sf2_sections(data)
    shdr = _records(pdta["shdr"], SHDR)
    if len(shdr) < 2:
        raise ValueError("SF2 shdr has no samples")

    # Last shdr record is the mandatory EOS sentinel.
    samples = shdr[:-1]
    entries = []
    encoded = []
    spu_payload = 0

    for index, record in enumerate(samples):
        (
            raw_name, start, end, start_loop, end_loop, sample_rate,
            original_pitch, correction, sample_link, sample_type,
        ) = record

        is_rom = bool(sample_type & 0x8000)
        if is_rom or end <= start:
            adp = b""
            pcm_count = 0
        else:
            byte_start = smpl_offset + start * 2
            byte_end = smpl_offset + end * 2
            if byte_end > smpl_offset + smpl_size or byte_end > len(data):
                raise ValueError(f"sample {index} {_name(raw_name)!r} exceeds smpl data")
            pcm_blob = data[byte_start:byte_end]
            pcm_count = len(pcm_blob) // 2
            pcm = struct.unpack(f"<{pcm_count}h", pcm_blob)
            adp = encode_mono_pcm16(pcm, sample_rate)
            spu_payload += max(0, len(adp) - 16)

        rel_loop_start = int(start_loop) - int(start)
        rel_loop_end = int(end_loop) - int(start)
        loop_start_frame, loop_end_frame = quantize_loop(
            rel_loop_start, rel_loop_end, pcm_count
        )

        encoded.append(adp)
        entries.append({
            "index": index,
            "name": _name(raw_name),
            "sample_rate": sample_rate,
            "original_pitch": original_pitch,
            "pitch_correction": correction,
            "sample_link": sample_link,
            "sample_type": sample_type,
            "pcm_samples": pcm_count,
            "loop_start": rel_loop_start,
            "loop_end": rel_loop_end,
            "loop_start_frame": loop_start_frame,
            "loop_end_frame": loop_end_frame,
            "rom_sample": is_rom,
        })

    header_size = BANK_HEADER.size
    entry_size = BANK_ENTRY.size
    table_offset = header_size
    data_offset = table_offset + len(entries) * entry_size

    blob = bytearray()
    blob.extend(BANK_HEADER.pack(
        BANK_MAGIC,
        BANK_VERSION,
        header_size,
        len(entries),
        table_offset,
        data_offset,
        sum(len(x) for x in encoded),
        entry_size,
        0,
    ))

    running = 0
    for entry, adp in zip(entries, encoded):
        loop_start = entry["loop_start_frame"]
        loop_end = entry["loop_end_frame"]
        name_bytes = entry["name"].encode("utf-8", errors="replace")[:27]
        name_bytes += bytes(28 - len(name_bytes))
        blob.extend(BANK_ENTRY.pack(
            entry["index"],
            running,
            len(adp),
            entry["sample_rate"],
            (entry["sample_rate"] * 4096) // 48000 if entry["sample_rate"] else 0,
            entry["pcm_samples"],
            0xFFFFFFFF if loop_start < 0 else loop_start,
            0xFFFFFFFF if loop_end < 0 else loop_end,
            entry["original_pitch"] & 0xFF,
            max(-128, min(127, entry["pitch_correction"])),
            entry["sample_type"] & 0xFFFF,
            name_bytes,
        ))
        running += len(adp)

    for adp in encoded:
        blob.extend(adp)

    instruments, presets = _parse_mapping(pdta)
    metadata = {
        "format": "2004sp-ps2-audio-bank",
        "version": BANK_VERSION,
        "source": sf2_path.name,
        "samples_file": "samples.rsab",
        "sample_count": len(entries),
        "encoded_bytes_in_file": sum(len(x) for x in encoded),
        "spu2_payload_bytes": spu_payload,
        "audsrv_spu2_budget_bytes": SPU2_BYTES - AUDSRV_SPU2_FIRST_SAMPLE,
        "fits_audsrv_spu2_budget_if_fully_resident":
            spu_payload <= (SPU2_BYTES - AUDSRV_SPU2_FIRST_SAMPLE),
        "samples": entries,
        "instruments": instruments,
        "presets": presets,
        "notes": [
            "ADPCM frames are encoded one-shot; SoundFont loop points remain metadata.",
            "Instrument/preset generator records are preserved for the future IOP sequencer.",
            "Stereo-linked SF2 samples remain separate mono entries and keep sample_link/type.",
        ],
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "samples.rsab").write_bytes(blob)
    (out_dir / "bank.json").write_text(
        json.dumps(metadata, indent=2),
        encoding="utf-8",
    )

    budget = SPU2_BYTES - AUDSRV_SPU2_FIRST_SAMPLE
    print(f"SF2:              {sf2_path}")
    print(f"Samples:          {len(entries)}")
    print(f"Presets:          {len(presets)}")
    print(f"Instruments:      {len(instruments)}")
    print(f"Bank file:        {len(blob):,} bytes")
    print(f"SPU2 sample data: {spu_payload:,} / {budget:,} bytes")
    print("Full residency:   " + ("YES" if spu_payload <= budget else "NO - use residency/cache"))
    print(f"Wrote:            {out_dir / 'samples.rsab'}")
    print(f"Wrote:            {out_dir / 'bank.json'}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("soundfont", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    convert(args.soundfont, args.output)


if __name__ == "__main__":
    main()
