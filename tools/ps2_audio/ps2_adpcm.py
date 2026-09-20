#!/usr/bin/env python3
"""PS2 SPU2 ADPCM encoder compatible with PS2SDK audsrv/adpenc."""

from __future__ import annotations

import struct
from typing import Sequence

APCM_HEADER = struct.Struct("<4sBBBBII")
PS2_NATIVE_RATE = 48000
PS2_PITCH_ONE = 4096
SAMPLES_PER_FRAME = 28
BYTES_PER_FRAME = 16

_FILTERS = (
    (0.0, 0.0),
    (-60.0 / 64.0, 0.0),
    (-115.0 / 64.0, 52.0 / 64.0),
    (-98.0 / 64.0, 55.0 / 64.0),
    (-122.0 / 64.0, 60.0 / 64.0),
)


def base_pitch(sample_rate: int) -> int:
    return max(1, min(0x3FFF, (int(sample_rate) * PS2_PITCH_ONE) // PS2_NATIVE_RATE))


def _signed32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def encode_mono_pcm16(
    samples: Sequence[int],
    sample_rate: int,
    loop_start_sample: int | None = None,
    loop_end_sample: int | None = None,
) -> bytes:
    """Encode signed 16-bit mono PCM as an audsrv APCM buffer.

    When a loop range is supplied, SPU2 repeat flags are written into the
    ADPCM frames themselves. SoundFont loop end is exclusive.
    """
    pcm = [max(-32768, min(32767, int(v))) for v in samples]
    raw_s1 = raw_s2 = 0.0
    err_s1 = err_s2 = 0.0
    frames = bytearray()
    last_predict = 0
    last_shift = 0

    looped = (
        loop_start_sample is not None
        and loop_end_sample is not None
        and 0 <= loop_start_sample < loop_end_sample <= len(pcm)
    )
    if looped:
        loop_start_frame = int(loop_start_sample) // SAMPLES_PER_FRAME
        loop_end_frame = max(
            loop_start_frame,
            (int(loop_end_sample) - 1) // SAMPLES_PER_FRAME,
        )
    else:
        loop_start_frame = loop_end_frame = -1

    frame_count = (len(pcm) + SAMPLES_PER_FRAME - 1) // SAMPLES_PER_FRAME
    for frame_index in range(frame_count):
        block = pcm[
            frame_index * SAMPLES_PER_FRAME:
            (frame_index + 1) * SAMPLES_PER_FRAME
        ]
        if len(block) < SAMPLES_PER_FRAME:
            block = block + [0] * (SAMPLES_PER_FRAME - len(block))

        predictor = 0
        best_max = 1.0e30
        candidates = []

        for candidate, (coef0, coef1) in enumerate(_FILTERS):
            s1 = raw_s1
            s2 = raw_s2
            converted = []
            max_abs = 0.0
            for sample in block:
                s0 = float(max(-30720, min(30719, sample)))
                delta = s0 + s1 * coef0 + s2 * coef1
                converted.append(delta)
                if abs(delta) > max_abs:
                    max_abs = abs(delta)
                s2 = s1
                s1 = s0

            candidates.append(converted)
            if max_abs < best_max:
                best_max = max_abs
                predictor = candidate

            if best_max <= 7.0:
                predictor = 0
                break

        predictor_samples = candidates[predictor]

        raw_s2 = float(max(-30720, min(30719, block[-2])))
        raw_s1 = float(max(-30720, min(30719, block[-1])))

        min2 = int(best_max)
        shift_mask = 0x4000
        shift = 0
        while shift < 12:
            if shift_mask & (min2 + (shift_mask >> 3)):
                break
            shift += 1
            shift_mask >>= 1

        coef0, coef1 = _FILTERS[predictor]
        packed = []
        for delta in predictor_samples:
            sample0 = delta + err_s1 * coef0 + err_s2 * coef1
            value = sample0 * float(1 << shift)
            quantized = _signed32((int(value) + 0x800) & 0xFFFFF000)
            quantized = max(-32768, min(32767, quantized))
            packed.append(quantized)

            shifted = quantized >> shift
            err_s2 = err_s1
            err_s1 = float(shifted) - sample0

        if looped and loop_start_frame == loop_end_frame and frame_index == loop_start_frame:
            flags = 0x07
        elif looped and frame_index == loop_start_frame:
            flags = 0x06
        elif looped and frame_index == loop_end_frame:
            flags = 0x03
        elif looped and loop_start_frame < frame_index < loop_end_frame:
            flags = 0x02
        elif not looped and frame_index == frame_count - 1:
            flags = 0x01
        else:
            flags = 0x00

        frames.append((predictor << 4) | shift)
        frames.append(flags)
        for i in range(0, SAMPLES_PER_FRAME, 2):
            frames.append(((packed[i + 1] >> 8) & 0xF0) | ((packed[i] >> 12) & 0x0F))

        last_predict = predictor
        last_shift = shift

    frames.extend(bytes(((last_predict << 4) | last_shift, 0x07)) + bytes(14))

    header = APCM_HEADER.pack(
        b"APCM",
        1,
        1,
        1 if looped else 0,
        0,
        base_pitch(sample_rate),
        len(pcm),
    )
    return header + frames


def quantize_loop(start_sample: int, end_sample: int, sample_count: int) -> tuple[int, int]:
    """Return inclusive ADPCM frame indices covering a SoundFont loop."""
    if start_sample < 0 or end_sample <= start_sample or start_sample >= sample_count:
        return -1, -1
    end_sample = min(end_sample, sample_count)
    start_frame = start_sample // SAMPLES_PER_FRAME
    end_frame = max(start_frame, (end_sample - 1) // SAMPLES_PER_FRAME)
    return start_frame, end_frame
