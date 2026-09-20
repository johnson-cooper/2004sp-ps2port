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
