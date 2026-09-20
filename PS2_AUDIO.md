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


## Real-hardware result: defer audio until login

The first audsrv smoke build successfully produced the ADPCM beep on real PS2 hardware, proving
LIBSD + audsrv + SPU2 playback worked. However, initializing audsrv inside `platform_init()`
left the title screen responsive but caused the later RuneScape server connection to fail.

Audio initialization is therefore now deferred until the login server has already returned a
successful login reply and `c->ingame` is set. This keeps the entire title-screen/network login
path identical to the accepted baseline. The post-login audio initializer is idempotent so
reconnect success cannot reload audsrv twice.


## Title-screen music requirement: custom ADPCM-only audsrv

Deferring all audio until after login is not the final architecture because RuneScape also plays
music on the title screen. The post-login-only experiment remains useful evidence, but the runtime
must be capable of safe audio before login.

Inspection of stock audsrv found that `audsrv_init()` always starts a permanent PCM streaming
thread and looping SPU2 block-DMA engine, even though this port intends to use preconverted ADPCM
samples and SPU2 voices only. The custom package installer now patches just that function before
building audsrv: libsd/SPU2 initialization, RPC, ADPCM sample upload and voice playback remain;
the PCM stream worker, stream semaphores, transfer callback, block DMA and format converter do not
start.

This is deliberately a one-variable real-hardware A/B test. Audio initialization is restored to
its pre-title position after network/USB/pad setup. If the startup beep still plays and RuneScape
can subsequently connect to the server, the continuous stock audsrv streaming engine was the
conflicting component and this voice-only backend can support title-screen music.
