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


## Milestone 3E: stop audio changes and diagnose ELF-layout sensitivity

Milestone 3D also failed before rs2midi could be loaded. The only runtime source difference from the
hardware-good 548b27c checkpoint was the replacement of the existing no-op ps2_music_update() stub.
That is enough evidence to stop treating the pre-login regression as an rs2midi runtime problem.

The runtime is restored to the exact 548b27c ps2_music.c behavior. A host-only tool,
`tools/ps2_elf_compare.ps1`, compares a known-good client.elf with a failing client.elf using the
EE toolchain's size/readelf/nm/objdump utilities. It reports hashes, section addresses/sizes, program
headers, `_end`, `errno`, and selected network/runtime symbols.

The comparison tool is not part of any PS2Build source glob and therefore cannot affect client.elf.
Do not resume MIDI integration until the reason tiny EE binary changes break pre-login networking is
understood or the ELF layout is made robust.


## Milestone 3F: inert ELF-layout A/B

The good/failing ELF report showed a very small but exact loaded-image shift:

- good .text: 0xC8F30
- failing .text: 0xC9018 (+0xE8)
- good .rodata: 0xCBC0
- failing .rodata: 0xCC10 (+0x50)
- .data size unchanged
- .bss size unchanged
- final LOAD MemSiz moved by 0x180

This test leaves the hardware-good voice-only runtime behavior intact and injects exactly 0xE8
unreachable bytes into .text plus 0x50 inert bytes into .rodata from the existing ps2_music.c
translation unit. There are no new calls, branches, globals, constructors, IOP loads, RPCs, or
audio operations.

Before hardware testing, verify the candidate ELF reaches .text 0xC9018 and .rodata 0xCC10.
The hardware question is only whether reproducing the layout shift causes the otherwise-known-good
client to fail pre-login networking.

Interpretation:
- if this inert candidate fails, ELF/memory placement alone is sufficient to trigger the bug;
- if it connects, section size/layout alone is insufficient and the failing functional changes
  themselves (or their codegen/link composition) must be investigated.


### Milestone 3F retention fix

The first inert-padding build still produced the exact hardware-good section sizes, proving the
file-scope assembler `.space` directives were discarded before the final stripped ELF.

The padding is now represented by real `used` C objects placed explicitly into `.text` and
`.rodata` with one-byte alignment. Because `ps2_music.o` is already required by the linked
music stubs, these bytes should remain in the final image.

Do not hardware-test this candidate unless `size -A build/bin/client.elf` reports:
- `.text` = 823320
- `.rodata` = 52240
- `.data` = 175780
- `.bss` = 309124


### Milestone 3F linker-retention fix

The second attempt used `used` C objects, but the final ELF still had the exact original section
sizes after a clean rebuild. That confirms link-time garbage collection, not compiler elimination.

The padding symbols are now external/linker-visible and the client target passes:
- `-Wl,--undefined=ps2_layout_text_pad`
- `-Wl,--undefined=ps2_layout_rodata_pad`

Those flags make the symbols GC roots without creating any runtime reference or call. PS2Build
supports target-level `ldflags`, so this is isolated to the linker and should retain the exact
0xE8 .text and 0x50 .rodata payloads.


## Milestone 3G: isolate heap boundary from normal global placement

Real hardware failed to connect with the pure inert 3F layout candidate. That proves no MIDI,
RPC, IOP execution, or audio behavior is required: EE layout alone can trigger the regression.

This next A/B restores the normal hardware-good .text/.data/.rodata/.bss layout and adds a separate
0x180-byte writable NOBITS orphan section named .ps2_heap_tail after the normal image. The linker is
forced to retain its symbol, but no runtime code references it.

The intended split is:
- normal sections and existing globals remain at hardware-good addresses;
- only LOAD MemSiz / effective image end moves from 0x24C284 to 0x24C404.

Before hardware testing, inspect size/readelf/objdump. Do not test if .data, .rodata, or .bss moved.
If normal sections stay put but this variant fails, the heap/image-end shift alone is sufficient.
If it connects, the bug depends on relocation of existing globals/sections rather than only heap start.


## Milestone 3H: move only .bss

Real hardware connected and entered the world with Milestone 3G: normal sections/globals stayed at
their hardware-good addresses while only the loaded-image end moved forward by 0x180. Therefore a
later heap/image boundary alone is not sufficient to cause the network failure.

This A/B removes the tail section and restores the exact hardware-good ps2_music.c source. The only
linker experiment is:

`-Wl,--section-start=.bss=0x00200c80`

That is the .bss start address from the failing ELF. .text, .data, .rodata, .sdata and their existing
contents should remain at the hardware-good addresses.

Interpretation:
- if networking fails, relocating .bss/global storage alone is sufficient;
- if networking succeeds, .bss placement is safe and the next split should move .sdata/.rodata/.data
  progressively until the sensitive region is identified.

Before hardware testing, confirm .bss addr is 2100352 (0x00200c80) while .data remains 1871744 and
.rodata remains 2047616.


## Milestone 3I: BSS +0x80 alignment-phase A/B

Real hardware failed when only .bss was moved from the hardware-good 0x00200b00 to the failing
0x00200c80 address. That proves BSS/global placement alone is sufficient to trigger the pre-login
network regression.

This test moves only .bss to 0x00200b80 (+0x80). All earlier sections remain hardware-good.
The purpose is to distinguish an alignment/modulo problem from sensitivity to the full +0x180
absolute displacement.

Interpretation:
- if +0x80 also fails, the shared low-address phase (...80 rather than ...00) is a strong clue;
- if +0x80 connects, displacement magnitude or a narrower address window matters and we continue
  with intermediate offsets.


## Milestone 3J: isolated high-memory EE shim

Milestone 3I connected successfully with .bss at 0x00200b80, while the earlier .bss-only
0x00200c80 test failed. Rather than continue brute-force offset probing, the port now treats the
normal client BSS layout as hardware-sensitive and avoids perturbing it for audio integration.

The client uses a local PS2SDK-derived linkfile. Normal .text/.data/.rodata/.sdata/.bss ordering is
unchanged. New rs2midi EE loader code, strings and its one initialized state word are emitted into a
second PT_LOAD beginning at 0x01fc0000. The normal _end remains the end of client BSS. _heap_size is
explicitly capped at 0x01fc0000 - _end so malloc can never overwrite the isolated overlay; the stock
128 KiB top-of-RAM stack reservation remains unchanged.

ps2_music_update() stays in the normal client text as a two-instruction MIPS tail jump into the
overlay. This is intended to preserve the hardware-good normal .text size while allowing the real
post-login experiment to run.

After scene_state == 2 remains live for 250 updates, the isolated shim:
1. loads external rs2midi.irx from the existing install/cache prefix;
2. binds RPC 0x5253324d;
3. sends the PING command;
4. expects 0x52533250.

No SPU2/libsd work has been added to rs2midi yet. Stage rs2midi.irx beside client.elf.

Before hardware testing, verify size/readelf:
- normal .text/.data/.rodata/.sdata/.bss addresses should match the hardware-good client;
- .bss should start at 0x00200b00;
- a second PT_LOAD should exist around 0x01fc0000 for .ps2_audio_*;
- the normal LOAD must not move its BSS to 0x00200c80.


## Hardware result: isolated EE shim + external rs2midi PING is good

Real PS2 hardware successfully connected to the server, entered the world, and remained connected
with commit `ee4a0bd1925019d4f055ee17a8b3fb55d15a9beb`.

Verified layout on that build:
- normal LOAD MemSiz: `0x14c284` (hardware-good baseline);
- normal `.data/.rodata/.sdata/.bss` addresses unchanged;
- normal `.bss` start: `0x00200b00`;
- isolated audio LOAD at `0x01fc0000`;
- isolated overlay contains only `.ps2_audio_text/.ps2_audio_rodata/.ps2_audio_data`.

The post-world external `rs2midi.irx` load/RPC PING path did not regress networking. This is now
the validated architecture for continuing PS2 MIDI work: keep audsrv frozen in voice-only mode,
keep rs2midi external, and keep all new EE companion-loader code/state in the isolated high-memory
overlay so the normal hardware-sensitive client BSS is not relocated.


## Milestone 4A: first real rs2midi SPU2 voice primitive

The validated isolated EE overlay/external-IRX architecture remains unchanged. audsrv is still frozen
in its proven voice-only form and continues to own core 1.

rs2midi now imports the already-loaded ROM LIBSD service and owns only SPU2 core 0 for this test.
It reserves one tiny sample slot at 0x001e0000 near the top of the 2 MiB SPU2 RAM; the frozen audsrv
ADPCM allocator still grows upward from 0x5010. This is intentionally a smoke-test reservation, not
the final music sample allocator.

New RPC commands:
- 0: PING
- 1: upload one raw PS2 ADPCM sample
- 2: note-on with voice/pitch/left/right volume
- 3: change pitch of a live voice
- 4: key-off a voice

After world entry, the isolated EE shim reuses the existing 660 Hz APCM smoke sample. It strips the
16-byte APCM header, sends the 784 raw ADPCM bytes to rs2midi, starts core-0 voice 0 at half the APCM
base pitch, changes to the native pitch after 25 ms, and keys off after another 25 ms.

Expected hardware result: a short audible pitch-changing chirp, followed by normal continued
network/world operation. The boot/server path is still untouched before the delayed world-live test.


## Milestone 4B: audible pattern + SPU2 state diagnostics

The first direct rs2midi voice test remained network-stable on real hardware but produced no audible
chirp. The client/server architecture therefore remains validated; only direct SPU2 voice output is
under investigation.

Two changes isolate the voice path:
- NOTE_ON no longer performs an immediate KOFF followed by KON. LIBSD itself documents that key
  transitions are asynchronous; explicit KEY_OFF plus a real delay is now used before retriggering.
- A GET_STATE RPC returns live core-0 pitch, voice L/R volume, sample start address, ENDX, core-0
  master volume, and core-1 external-input volume.

The smoke test is now deliberately obvious: three separated full-volume beeps, with the first
starting at quarter pitch and changing to half pitch while active. State snapshots are logged after
the first note-on, after the pitch change, and after key-off. This remains entirely post-world and
inside the isolated EE overlay/external rs2midi architecture.


## Milestone 4C: bypass ROM file-loader stack overflow

The Milestone 4B PCSX2 test finally exposed a failure before any direct SPU2 operation ran.
At the delayed post-world trigger, `SifLoadStartModule("mass0:/rs2midi.irx", ...)` entered the
BIOS/ROM `Module_File_loader` and PCSX2 reported a stack overflow on that loader thread
(`Stack size = 0x800`) before the call returned. There was therefore no rs2midi RPC bind, sample
upload, KON, or state snapshot. The previous silence must not be treated as evidence against the
direct core-0 voice path yet.

This checkpoint keeps every validated architectural constraint:
- audsrv remains frozen in the real-hardware-good voice-only shape;
- rs2midi remains a separate external IRX beside `client.elf`;
- loading still happens only after the world is live;
- new EE loader code/state remains in the isolated high-memory audio overlay;
- normal client BSS placement is not intentionally changed.

Only the external-load mechanism changes. The EE uses the already-proven RuneScape USB stdio path
to `fopen/fread` the IRX into a temporary 64-byte-aligned heap buffer, pads the allocation for
PS2SDK's 16-byte DMA rounding, then calls `SifExecModuleBuffer()`. This avoids asking the ROM
module loader to walk the BDM/FAT `mass0:` filesystem path itself. The temporary EE buffer is freed
immediately after `SifExecModuleBuffer()` returns.

Hardware/PCSX2 acceptance for this checkpoint:
1. no `Module_File_loader` stack overflow;
2. an `audio: rs2midi EE-buffer load ... id=... modres=...` line appears;
3. RPC PING succeeds;
4. the existing GET_STATE snapshots run;
5. the three-beep test is finally allowed to exercise SPU2;
6. RuneScape networking remains healthy throughout.

Keep `rs2midi.irx` staged beside `client.elf`. If this reaches PING/state diagnostics but remains
silent, only then resume investigation of DMA/KON/core-0 routing.
