# PS2 audio asset tools

These tools move music synthesis work off the PS2 EE.

## SF2 -> PS2 ADPCM bank

`sf2_to_ps2bank.py` reads an ordinary SoundFont 2 file directly. It extracts the 16-bit
sample pool, converts each sample to the same SPU2 ADPCM format used by PS2SDK/audsrv, and
writes:

- `samples.rsab`: fixed-size sample table followed by complete audsrv-compatible APCM buffers.
- `bank.json`: sample tuning/loop/link metadata plus the original preset/instrument generator
  zones needed by the future runtime sequencer.

The converter reports the exact number of bytes that would occupy audsrv's SPU2 sample area.
If the whole bank does not fit, the runtime will need an instrument/sample residency cache.

Loop locations are retained in metadata and quantized to 28-sample ADPCM frame indices.
They are not baked into the encoded waveform yet. That avoids duplicating the same waveform
when future MIDI regions need different loop behavior.

Usage:

```bat
py -3 tools\ps2_audio\sf2_to_ps2bank.py "Older RuneScape.sf2" build\audio\soundfont
```


## Compact runtime MIDI bank

The hardware sequencer uses eight tiny SPU2-resident timbre slots. To replace the
synthetic fallback bank with real timbres derived from the repository SoundFont,
run from the repository root:

```bat
prepare-ps2-midi-bank.bat
ps2build build
```

`prepare-ps2-midi-bank.bat` reads `rom/SCC1_Florestan.sf2` and regenerates
`src/platform/ps2_midi_bank.c`. The generator uses the existing SoundFont zone
resolver, selects one representative bank-0 General MIDI preset per 16-program
family, extracts a stable periodic waveform, normalizes it, and encodes it with
the same PS2SDK-compatible ADPCM encoder used elsewhere in this directory.

The runtime footprint intentionally remains fixed at eight 720-byte raw SPU2
samples (5,760 bytes total). SoundFont parsing and resampling never run on the
PS2 EE.

The generated C file is source input to the normal `ps2build build`. The
external `rs2midi.irx` does not need to be rebuilt solely because the sample
bytes changed, although a normal full build may rebuild it anyway.

## MIDI -> PS2 sequence

`midi_to_ps2seq.py` is dependency-free. It parses Standard MIDI Files, merges all tracks,
resolves tempo changes offline, and emits `RSEQ` events with microsecond deltas. Runtime code
therefore does not need a MIDI parser or tempo math.

Usage:

```bat
py -3 tools\ps2_audio\midi_to_ps2seq.py "rom\cache\client\songs\scape main.mid" "build\audio\songs\scape main.rseq"
```

The current PS2 runtime does not consume RSAB/RSEQ yet. First prove the audsrv/SPU2 smoke-test
commit on real hardware. The next runtime milestone is a small audsrv/IOP extension providing
per-voice pitch, key-off, volume/pan and ADSR, followed by the RSEQ scheduler.

RuneScape cache SFX are intentionally not converted by these scripts yet. Their current
`wave_generate()/tone_generate()` path is an EE software synthesizer; the planned SFX build
step will execute that synthesis offline on the host and feed the resulting PCM through the
same `ps2_adpcm.py` encoder.


## Milestone 2: real PS2 title music

The repository already contains both inputs used by the first hardware music test:

- `rom/SCC1_Florestan.sf2`
- `rom/cache/client/songs/scape_main.mid`

The cache MIDI is Jagex-packed (4-byte uncompressed length plus BZip payload), so the host tools
decode that container before parsing the Standard MIDI File.

After `ps2build build`, run:

```bat
prepare-ps2-title-music.bat
```

This writes the generated runtime pack to:

```
build\bin\rom\ps2audio\scape_main.ps2m
```

The pack contains only the SoundFont sample regions actually used by `scape_main`. SoundFont
loop points are encoded as SPU2 ADPCM repeat flags, while tempo, preset selection, tuning,
pitch-bend and controller-derived volume/pan changes are resolved on the host. The builder aborts
instead of emitting a pack if the required ADPCM sample payload exceeds audsrv's usable SPU2 RAM.

The generated `.ps2m` file is build output and should not be committed.
