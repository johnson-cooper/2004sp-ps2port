#!/usr/bin/env python3
"""Convert a PCM WAV into the audsrv APCM/SPU2 ADPCM format used by PS2."""

from __future__ import annotations

import argparse
import struct
import wave
from pathlib import Path

from ps2_adpcm import encode_mono_pcm16


def read_pcm16(path: Path) -> tuple[list[int], int]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        frames = wav.getnframes()
        data = wav.readframes(frames)

    if channels < 1:
        raise ValueError("WAV has no channels")
    if width not in (1, 2):
        raise ValueError(f"unsupported WAV sample width: {width * 8} bits")

    samples: list[int] = []
    if width == 1:
        frame_width = channels
        for pos in range(0, len(data), frame_width):
            values = [((data[pos + ch] - 128) << 8) for ch in range(channels)]
            samples.append(sum(values) // channels)
    else:
        frame_width = channels * 2
        for pos in range(0, len(data), frame_width):
            values = [
                struct.unpack_from("<h", data, pos + ch * 2)[0]
                for ch in range(channels)
            ]
            samples.append(sum(values) // channels)

    return samples, rate


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    pcm, rate = read_pcm16(args.input)
    if not pcm:
        raise SystemExit("input WAV contains no samples")

    encoded = encode_mono_pcm16(pcm, rate)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(encoded)

    print(f"WAV:       {args.input}")
    print(f"Rate:      {rate} Hz")
    print(f"Samples:   {len(pcm):,}")
    print(f"Duration:  {len(pcm) / rate:.3f} s")
    print(f"PS2 ADPCM: {len(encoded):,} bytes")
    print(f"Wrote:     {args.output}")


if __name__ == "__main__":
    main()
