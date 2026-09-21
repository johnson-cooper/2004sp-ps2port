#!/usr/bin/env python3
"""Build the tiny runtime PS2 MIDI bank from the repository SoundFont.

This deliberately does all SoundFont parsing/resampling/ADPCM encoding on the
host. The PS2 runtime keeps only eight 720-byte SPU2 ADPCM wavetables.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from build_title_music import SoundFontResolver
from ps2_adpcm import base_pitch, encode_mono_pcm16

SLOT_COUNT = 8
OUTPUT_RATE = 22050
OUTPUT_SAMPLES = 1232          # 44 SPU2 ADPCM data frames
OUTPUT_CYCLES = 37             # exact loop count; ~662.2 Hz at 22050 Hz
BASE_NOTE = 76                 # E5, matches ps2_music.c

# Representative GM presets for the eight 16-program families used by the
# hardware sequencer. If a SoundFont lacks the preferred preset, the first
# bank-0 preset in that 16-program family is used instead.
PREFERRED_PROGRAMS = (
    0,    # Acoustic Grand Piano
    24,   # Acoustic Guitar (nylon)
    40,   # Violin
    56,   # Trumpet
    73,   # Flute
    80,   # Lead 1 (square)
    104,  # Sitar
    14,   # Tubular Bells (explicit runtime exception for classic RS tracks)
)

REFERENCE_NOTES = (60, 64, 67, 69, 72, 55, 48, 76)
REFERENCE_VELOCITY = 100
TARGET_PEAK = 0.70

# A single SPU2 wavetable is transposed over a large MIDI range. Rich upper
# harmonics that sound correct near the base note can alias into whistle/ring
# tones several octaves higher. Keep more detail for naturally soft families
# and progressively band-limit the brighter/high-risk families.
SLOT_MAX_HARMONICS = (
    12,  # piano / chromatic percussion
    10,  # organ / guitar
    10,  # bass / strings
    9,   # ensemble / brass
    9,   # reed / pipe
    7,   # synth lead / pad
    6,   # synth effects / ethnic
    4,   # dedicated Tubular Bells: suppress high-note alias/ringing
)


def _preset_candidates_for_slot(
    resolver: SoundFontResolver,
    slot: int,
) -> list[tuple[int, str]]:
    """Return preferred preset first, then bank-0 family fallbacks.

    Slot 7 intentionally prefers GM 14 Tubular Bells even though its normal
    fallback family remains 112..127. This lets the runtime dedicate one tiny
    hardware timbre to a classic RuneScape bell sound without expanding the
    eight-slot SPU2 footprint.
    """
    preferred = PREFERRED_PROGRAMS[slot]
    lo = slot * 16
    hi = lo + 15

    candidates = []

    preferred_preset = resolver.presets.get((0, preferred))
    if preferred_preset is not None:
        candidates.append((preferred, preferred_preset[0]))

    candidates.extend(
        sorted(
            (program, name_zones[0])
            for (bank, program), name_zones in resolver.presets.items()
            if bank == 0
            and lo <= program <= hi
            and program != preferred
        )
    )

    if not candidates:
        raise ValueError(
            f"SoundFont has neither preferred program {preferred} nor "
            f"a bank-0 preset in GM family {lo}..{hi}"
        )

    return candidates


def _region_for_program(resolver: SoundFontResolver, program: int):
    choices = []
    for note in REFERENCE_NOTES:
        regions = resolver.resolve(0, program, note, REFERENCE_VELOCITY)
        for region in regions:
            # Prefer a true SoundFont sustain loop, then a centered/quietly
            # attenuated mono region. Stereo-linked regions remain usable; one
            # side is enough for this tiny mono hardware bank.
            score = (
                0 if region.looping else 1,
                abs(region.pan),
                region.attenuation,
                abs(note - 64),
            )
            choices.append((score, note, region))
        if choices and any(item[2].looping for item in choices):
            break

    if not choices:
        raise ValueError(f"no playable SoundFont region for program {program}")

    choices.sort(key=lambda item: item[0])
    _score, note, region = choices[0]
    return note, region


def _stored_fundamental(resolver: SoundFontResolver, region) -> float:
    """Estimate the pitch physically stored in the raw SF2 PCM.

    SoundFont region root-key/coarse/fine generators describe how a sample is
    mapped during playback. They do not change the pitch already recorded in
    the sample pool. Using them here can make a one-cycle extractor choose a
    two-cycle segment (or vice versa), producing octave errors in the compact
    wavetable.

    The sample header's originalPitch is the correct anchor for the raw PCM.
    pitchCorrection is the cents adjustment required at playback, so invert it
    to recover the stored waveform's native pitch.
    """
    header = resolver.sample_headers[region.sample_id]
    original_pitch = int(header[6])
    pitch_correction = int(header[7])

    if original_pitch < 0 or original_pitch > 127:
        original_pitch = 60

    nominal = 440.0 * (2.0 ** ((original_pitch - 69) / 12.0))
    return nominal * (2.0 ** (-pitch_correction / 1200.0))


def _best_period(resolver: SoundFontResolver, pcm, region) -> tuple[int, int, int, float]:
    """Return (period_samples, stable_start, stable_end)."""
    if len(pcm) < 64:
        raise ValueError("SoundFont region is too short for wavetable extraction")

    if region.looping:
        lo = max(0, region.loop_start - region.start)
        hi = min(len(pcm), region.loop_end - region.start)
    else:
        lo = min(len(pcm) // 5, max(0, len(pcm) - 64))
        hi = len(pcm)

    if hi - lo < 64:
        lo, hi = 0, len(pcm)

    fundamental = _stored_fundamental(resolver, region)
    if fundamental <= 1.0:
        raise ValueError("invalid SoundFont sample tuning")

    estimate = region.sample_rate / fundamental

    # Stay close to the SF2 sample-header pitch. The old +/-18% search could
    # follow a strong harmonic when region mapping metadata differed from the
    # raw sample pitch. A narrow window still lets autocorrelation find a clean
    # seam without permitting octave-family mistakes.
    pmin = max(8, int(math.floor(estimate * 0.94)))
    pmax = max(pmin, int(math.ceil(estimate * 1.06)))

    # Use a stable area near the beginning of the SoundFont loop/sustain.
    anchor = lo
    available = hi - anchor

    best = None
    for period in range(pmin, pmax + 1):
        pairs = min(8, available // period - 1)
        if pairs < 2:
            continue

        error = 0.0
        signal = 0.0
        count = pairs * period
        for i in range(count):
            a = float(pcm[anchor + i])
            z = float(pcm[anchor + i + period])
            d = a - z
            error += d * d
            signal += a * a

        normalized = error / max(signal, 1.0)
        # Prefer a clean seam, but penalize drifting away from the SF2 header
        # period so a strong overtone cannot win merely by correlating better.
        relative_error = abs(period - estimate) / max(estimate, 1.0)
        score = normalized + relative_error * 0.20
        candidate = (score, relative_error, period)
        if best is None or candidate < best:
            best = candidate

    if best is None:
        period = max(8, min(pmax, int(round(estimate))))
    else:
        period = best[2]

    ratio = period / max(estimate, 1.0)
    if not 0.90 <= ratio <= 1.10:
        raise ValueError(
            f"unsafe SoundFont period selection: expected {estimate:.2f}, "
            f"selected {period} (ratio {ratio:.3f})"
        )

    return period, anchor, hi, estimate


def _average_cycle(pcm, period: int, anchor: int, end: int) -> list[float]:
    cycle_count = min(12, (end - anchor) // period)
    if cycle_count < 1:
        raise ValueError("not enough stable PCM for one cycle")

    cycle = [0.0] * period
    for c in range(cycle_count):
        base = anchor + c * period
        for i in range(period):
            cycle[i] += pcm[base + i]

    inv = 1.0 / cycle_count
    cycle = [v * inv for v in cycle]

    # Remove DC so the tiny loop does not waste headroom or click around zero.
    mean = sum(cycle) / len(cycle)
    return [v - mean for v in cycle]


def _bandlimit_cycle(cycle: list[float], max_harmonics: int) -> list[float]:
    """Rebuild one periodic cycle from a bounded Fourier series.

    This is intentionally dependency-free and runs only on the host. The
    cycles are tiny, so the direct DFT is fast enough and avoids adding numpy
    or a DSP library to the asset pipeline.
    """
    count = len(cycle)
    if count < 3:
        return cycle[:]

    limit = max(1, min(int(max_harmonics), (count - 1) // 2))
    coefficients = []

    for harmonic in range(1, limit + 1):
        real = 0.0
        imag = 0.0
        for index, value in enumerate(cycle):
            angle = 2.0 * math.pi * harmonic * index / count
            real += value * math.cos(angle)
            imag -= value * math.sin(angle)
        coefficients.append((real / count, imag / count))

    rebuilt = []
    for index in range(count):
        value = 0.0
        for harmonic, (real, imag) in enumerate(coefficients, start=1):
            angle = 2.0 * math.pi * harmonic * index / count
            value += 2.0 * (
                real * math.cos(angle) - imag * math.sin(angle)
            )
        rebuilt.append(value)

    return rebuilt


def _sample_cycle(cycle: list[float], phase: float) -> float:
    pos = phase * len(cycle)
    i0 = int(pos) % len(cycle)
    i1 = (i0 + 1) % len(cycle)
    frac = pos - math.floor(pos)
    return cycle[i0] * (1.0 - frac) + cycle[i1] * frac


def _make_wavetable(cycle: list[float]) -> list[int]:
    out = []
    peak = 0.0

    for n in range(OUTPUT_SAMPLES):
        phase = ((n * OUTPUT_CYCLES) % OUTPUT_SAMPLES) / OUTPUT_SAMPLES
        value = _sample_cycle(cycle, phase)
        out.append(value)
        peak = max(peak, abs(value))

    if peak < 1.0:
        raise ValueError("extracted SoundFont cycle is effectively silent")

    gain = TARGET_PEAK * 32767.0 / peak
    return [
        max(-32768, min(32767, int(round(v * gain))))
        for v in out
    ]


def _raw_adpcm(pcm: list[int]) -> bytes:
    encoded = encode_mono_pcm16(
        pcm,
        OUTPUT_RATE,
        loop_start_sample=0,
        loop_end_sample=len(pcm),
    )
    raw = encoded[16:]  # runtime slots contain raw SPU2 frames, not APCM header
    if len(raw) != 720:
        raise ValueError(f"expected 720 raw ADPCM bytes, got {len(raw)}")
    return raw


def _c_bytes(blob: bytes, indent: str = "        ") -> str:
    rows = []
    for pos in range(0, len(blob), 16):
        row = ", ".join(f"0x{value:02x}" for value in blob[pos:pos + 16])
        rows.append(indent + row)
    return ",\n".join(rows)


def build(soundfont: Path, output: Path) -> None:
    resolver = SoundFontResolver(soundfont)
    slots = []

    for slot in range(SLOT_COUNT):
        selected = None
        rejected = []

        for program, preset_name in _preset_candidates_for_slot(resolver, slot):
            try:
                reference_note, region = _region_for_program(resolver, program)
            except ValueError as exc:
                rejected.append(f"{program}:{preset_name} ({exc})")
                continue

            fundamental = _stored_fundamental(resolver, region)
            expected_period = region.sample_rate / fundamental
            if expected_period < 8.0:
                rejected.append(
                    f"{program}:{preset_name} "
                    f"(native period {expected_period:.2f} < 8 samples)"
                )
                continue

            pcm = resolver.pcm_for(region)
            try:
                period, anchor, stable_end, expected_period = _best_period(
                    resolver, pcm, region
                )
            except ValueError as exc:
                rejected.append(f"{program}:{preset_name} ({exc})")
                continue

            selected = (
                program,
                preset_name,
                reference_note,
                region,
                pcm,
                period,
                anchor,
                stable_end,
                expected_period,
            )
            break

        if selected is None:
            details = "; ".join(rejected[:8])
            raise ValueError(
                f"no safe compact-wavetable preset for slot {slot}: {details}"
            )

        (
            program,
            preset_name,
            reference_note,
            region,
            pcm,
            period,
            anchor,
            stable_end,
            expected_period,
        ) = selected

        cycle = _average_cycle(pcm, period, anchor, stable_end)
        max_harmonics = SLOT_MAX_HARMONICS[slot]
        cycle = _bandlimit_cycle(cycle, max_harmonics)
        wavetable = _make_wavetable(cycle)
        raw = _raw_adpcm(wavetable)

        slots.append((program, preset_name, reference_note, region, period, raw))
        sample_header = resolver.sample_headers[region.sample_id]
        original_pitch = int(sample_header[6])
        print(
            f"slot {slot}: program={program:3d} {preset_name!r} "
            f"sample={region.sample_id} sf2_pitch={original_pitch} "
            f"mapped_root={region.root_key} rate={region.sample_rate} "
            f"period={period} expected={expected_period:.2f} "
            f"ratio={period / expected_period:.4f} "
            f"harmonics={max_harmonics} loop={region.looping}"
        )
        if rejected:
            print(f"  skipped: {rejected[0]}")

    target_freq = 440.0 * (2.0 ** ((BASE_NOTE - 69) / 12.0))
    wavetable_freq = OUTPUT_RATE * OUTPUT_CYCLES / OUTPUT_SAMPLES
    corrected_pitch = int(round(
        base_pitch(OUTPUT_RATE) * target_freq / wavetable_freq
    ))
    corrected_pitch = max(1, min(0x3FFF, corrected_pitch))

    lines = [
        '#include "ps2_midi_bank.h"',
        "",
        "#ifdef __PS2__",
        '#define PS2_MIDI_BANK_RODATA __attribute__((section(".ps2_audio_rodata"), used))',
        "#else",
        "#define PS2_MIDI_BANK_RODATA",
        "#endif",
        "",
        "/*",
        " * Compact SoundFont-derived PS2 MIDI bank.",
        " *",
        " * GENERATED by tools/ps2_audio/build_compact_midi_bank.py from",
        f" * {soundfont.as_posix()}. Do not hand-edit sample bytes.",
        " *",
        " * Each slot is a seamless 37-cycle wavetable extracted from a real",
        " * SCC1_Florestan preset, Fourier-band-limited for safe high-note",
        " * transposition, then encoded with the PS2SDK-compatible ADPCM",
        " * predictor. Runtime size remains 8 x 720 bytes.",
        " */",
        "const unsigned char ps2_midi_bank_adpcm[PS2_MIDI_BANK_SAMPLE_COUNT][PS2_MIDI_BANK_SAMPLE_BYTES]",
        "    PS2_MIDI_BANK_RODATA __attribute__((aligned(64))) = {",
    ]

    for slot, (program, name, note, region, period, raw) in enumerate(slots):
        safe_name = name.replace("*/", "* /")
        lines += [
            f"    /* {slot}: GM {program} {safe_name} (source note {note}, sample {region.sample_id}, period {period}) */",
            "    {",
            _c_bytes(raw),
            "    }" + ("," if slot + 1 < SLOT_COUNT else ""),
        ]

    lines += [
        "};",
        "",
        "const unsigned int ps2_midi_bank_base_pitch",
        f"    PS2_MIDI_BANK_RODATA __attribute__((aligned(4))) = {corrected_pitch}u;",
        "",
    ]

    output.write_text("\n".join(lines), encoding="utf-8")
    print(f"base note/pitch: {BASE_NOTE} / {corrected_pitch}")
    print(f"bank payload:    {SLOT_COUNT * 720:,} bytes")
    print(f"wrote:           {output}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "soundfont",
        nargs="?",
        type=Path,
        default=Path("rom/SCC1_Florestan.sf2"),
    )
    parser.add_argument(
        "output",
        nargs="?",
        type=Path,
        default=Path("src/platform/ps2_midi_bank.c"),
    )
    args = parser.parse_args()

    if not args.soundfont.is_file():
        raise SystemExit(f"SoundFont not found: {args.soundfont}")

    build(args.soundfont, args.output)


if __name__ == "__main__":
    main()
