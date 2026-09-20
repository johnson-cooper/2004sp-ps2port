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

This one-variable A/B test is now confirmed on real PS2 hardware. With the custom voice-only
audsrv package installed, the startup ADPCM beep played and RuneScape subsequently connected and
entered the world normally. The earlier stock-audsrv build played the same beep but failed the
later server connection. Treat the permanent PCM streaming thread / looping block DMA as the
network-conflicting component; preserve the voice-only audsrv initialization for this port.

## Milestone 2: scape_main through SPU2 voices

The smoke beep is replaced by the client's existing title-music request:

```c
platform_set_midi("scape_main", 12345678, 40000);
```

The custom audsrv package now adds three small private RPCs: explicit-channel ADPCM start with an
arbitrary SPU2 pitch, voice key-off, and live voice pitch changes. It still does not start the
stock PCM streaming thread.

`prepare-ps2-title-music.bat` converts the repository's
`rom/SCC1_Florestan.sf2` and Jagex-packed `rom/cache/client/songs/scape_main.mid` into
`build/bin/rom/ps2audio/scape_main.ps2m`. Only samples actually referenced by the title song are
encoded. SF2 loops become SPU2 ADPCM loop flags, while preset selection, tempo, pitch, controller
volume/pan and pitch-bend math are resolved offline.

The EE runtime only advances compact timestamped events. Sample playback, pitch and mixing are
performed by SPU2. This first music milestone intentionally leaves rev254 in-game MIDI_SONG/jingle
protocol work and RuneScape SFX disabled so title music can be hardware-tested in isolation.


## Milestone 2B: defer music bank upload until live world

Real-hardware testing of the first title-music build reproduced the pre-login network failure and
did not produce audible `scape_main`. The voice-only audsrv backend itself remains proven by the
earlier single-sample beep test, which connected and entered the world normally.

For the next isolated test, PS2 no longer calls `platform_set_midi("scape_main", ...)` during
`client_load()`. The backend still initializes in the same hardware-proven boot position, but no
music pack or bulk ADPCM sample upload occurs during the title/login path.

Once `PLAYER_INFO` has made `scene_state == 2`, the client waits 250 normal game ticks
(approximately five seconds) and starts `scape_main` once. This separates three outcomes:

- failure before the five-second trigger: regression is not caused by music-pack upload;
- world remains healthy until the trigger, then network/performance fails: bulk SPU2 sample upload
  is the remaining IOP/network conflict;
- world remains healthy and music plays: title-screen timing was the problem and sequencing can be
  developed safely from an in-world trigger before deciding how to handle title audio.


## Milestone 2C: restore exact voice-only audsrv for network A/B

The deferred-music build still failed to connect before any song pack was loaded. That rules out
the title-screen MIDI request and bulk `scape_main` sample upload as the immediate regression.

The remaining change relative to the hardware-good beep checkpoint was the audsrv module itself:
the MIDI experiment had added three private RPC commands and IOP voice-control handlers. This
checkpoint restores the exact earlier voice-only audsrv patch (only `audsrv_init()` is changed to
disable stock PCM streaming) and makes the PS2 MIDI runtime inert.

Expected hardware result: title screen has no music, no delayed music is attempted, and server
connection should match the proven voice-only checkpoint. If this connects again, do not extend
audsrv further; implement MIDI controls in a separate companion IOP module loaded only after the
world is live.


## Milestone 3A: separate rs2midi companion IRX, PING only

Real hardware confirmed the rollback at
`1e6cbfeff8aef0b6d5bcfdd39129860bfdf4ec65`: the exact voice-only audsrv package again connected
to the server and entered the world. This proves the failed deferred-music build was regressed by
the private MIDI modifications inside audsrv itself, not by loading `scape_main`.

The voice-only audsrv package is therefore frozen. New MIDI functionality moves into a separate
local IOP target named `rs2midi`.

Milestone 3A intentionally contains no audio functionality. `rs2midi.irx` has one low-priority
IOP thread and one RPC service with a PING command. It does not import libsd, initialize SPU2,
write SPU2 registers, allocate sample memory, or modify audsrv.

The client waits until `scene_state == 2` has remained live for 250 normal game ticks
(approximately five seconds), then:

1. loads the embedded `rs2midi.irx`;
2. binds its RPC service;
3. sends one PING;
4. expects `0x52533250` as the PONG;
5. leaves the module idle afterward.

The hardware acceptance criterion is both RPC success and continued healthy RuneScape networking
after the module is resident. Only after this passes should the companion gain one SPU2 operation
at a time.


## Milestone 3B: build rs2midi separately, do not embed it

Real hardware failed to connect in Milestone 3A before the delayed rs2midi load point could ever
run. Therefore the resident PING module itself was not exercised and cannot be blamed by that test.

Milestone 3B isolates the client ELF/layout change. The rs2midi IOP target remains in ps2.yaml and
still builds, but it is not embedded into client.elf. The EE wrapper and delayed test hook are
removed, restoring src/entry/client.c to the exact voice-only checkpoint that connected at
1e6cbfeff8aef0b6d5bcfdd39129860bfdf4ec65.

Hardware question: does RuneScape connect again when rs2midi merely exists as a separate build
artifact but contributes zero bytes, symbols or code to client.elf? If yes, the next companion test
must load rs2midi from storage after the world is live rather than embedding it.


## Milestone 3C: load standalone rs2midi.irx after world entry

Real hardware confirmed Milestone 3B at
`548b27cb8ca4e30e60a8fcb4f8c7bd92c6004924`: RuneScape connected and entered the world when
the rs2midi target was built separately but contributed no bytes or symbols to client.elf.

This proves the Milestone 3A failure happened before the companion could run and was caused by the
embedded/client-ELF configuration itself. Keep rs2midi external.

Milestone 3C adds only a small EE-side loader inside the existing PS2 platform source. After
`scene_state == 2` has remained live for 250 game ticks (about five seconds), it resolves the
same install prefix used by the cache, verifies `rs2midi.irx` exists beside `client.elf`, loads
it with `SifLoadStartModule()`, binds the PING-only RPC server, and sends one PING.

The standalone file must be staged beside `client.elf` on the USB install. The module still does
no SPU2/libsd work. The hardware acceptance test is:
1. login and world entry still succeed before the trigger;
2. external load + PING reports PASS;
3. RuneScape networking remains healthy afterward.


## Milestone 3D: reuse existing ps2_music_update hook, external load only

Milestone 3C still failed at the title/login connection stage, before the delayed external load
could execute. The external IRX therefore still was not the immediate cause. The regression was
introduced by adding new EE-side loader/hook code to src/platform/ps2.c and src/entry/client.c.

This checkpoint restores both of those source files byte-for-byte to the real-hardware-good
548b27cb8ca4e30e60a8fcb4f8c7bd92c6004924 versions. The only runtime experiment is confined to
the already-existing ps2_music.c object: the good build already calls ps2_music_update() every
frame, so that existing no-op stub now watches the already-existing ps2_crash_client pointer.

After ingame + scene_state == 2 remains true for 250 polls, ps2_music_update() performs exactly one
SifLoadStartModule() of rs2midi.irx from the install/cache prefix. There is no EE-side RPC bind or
PING in this milestone. This isolates whether a minimally invasive external module load can coexist
with networking while leaving the PS2 platform/network translation unit unchanged.
