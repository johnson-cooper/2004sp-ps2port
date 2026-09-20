# PS2 audio bring-up

The accepted gameplay baseline remains commit `c41d86e94bee3c348372af77c273ef93a5bc7c05`.
This audio work is a new real-hardware experiment and must not replace that baseline until tested.

## Milestone 1: audsrv / SPU2 smoke test

1. Run `install-audsrv-package.bat` once. It installs the pinned standalone
   `audsrv` PS2Build package under the SDK's `packages/world/audsrv` directory.
2. Build normally with `ps2build build`.
3. Boot the resulting `build/bin/client.elf` on real PS2 hardware.
4. After network, USB and controller initialization, a short low-volume beep should play.
5. Check `boot.log` for lines beginning with `audio:`.

Audio initialization is deliberately last in `platform_init()`. Previous real-hardware work
proved that adding unrelated IOP activity during DEV9/SMAP network bring-up can cause stalls.

The smoke beep is already PS2 ADPCM. No TinySoundFont, MIDI synthesis, RuneScape
`tone_generate()`, or PCM software mixing runs on the EE in this milestone.

## Target architecture

Host-side tools will convert the RuneScape SoundFont to SPU2 ADPCM samples and MIDI files
to compact timestamped event streams. The runtime audio service will schedule those events
onto SPU2 voices. RuneScape sound effects will likewise be rendered/encoded offline instead
of using the expensive EE-side 22.05 kHz tone synthesizer.

Stock audsrv is sufficient for the smoke test and one-shot SFX. MIDI playback will require a
small audsrv/IOP extension for per-voice pitch, key-off, ADSR and later sequencer ownership.
Keep that extension isolated from the generic SDK package until this base path is proven on hardware.
