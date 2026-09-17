# PS2 Hardware Development Workflow

This file is the standing workflow for the PlayStation 2 port. Read it before making or testing PS2-specific changes, including in future development sessions.

## Current integration status

- Active development branch: `ps2-hardware-integration`
- Branch starting point: `main` commit `673657249ef18526e6c33a3d4381953c132c2782`
- Last fully real-hardware-known-good integration commit: `47e8ff361703a29ed227416a832b39b2667923f2` (confirmed working on real PS2 on 2026-09-16; qword-aligned 4 MiB scene arena, 16x16 / 256-slot resident terrain window, and six-tile / 13x13 camera draw window survived beyond T3907 in Lumbridge).
- Terrain milestone: `fb83106daa4de4149bda6f6c36637000f378597a` completed the Lumbridge terrain build and reached the live world, but froze immediately after the first live frame. It is evidence that bounded terrain works, **not** a known-good gameplay checkpoint.
- The subsequent five-slot textured-terrain cache experiment regressed to a world-loading crash and was rejected.
- Current terrain finding: the old 4-byte scene-arena alignment made additional terrain residency highly layout-sensitive. After changing the PS2 scene arena to a 16-byte-aligned base with qword-aligned bump allocations, the previously failing 2x3 and 3x3 layouts became stable, a 16x16 resident window survived beyond T4488, and radius 6 survived beyond T3907. The radius-6 screenshot also showed that the fixed 16x16 resident window clips the camera-centred draw window: the orbit camera can sit several tiles behind the player, so only the overlap between camera radius and resident terrain appears. Treat qword alignment as required; grow residency separately before increasing draw radius again.
- Current policy: gameplay-first 32 MiB profile — untextured colored terrain, bounded/lazy Ground residency, tight scene radius, capped nearby dynamic entities, minimum UI, minimap off, static locs deferred until filtered streaming is ready.
- After each accepted hardware test, update the known-good commit here before starting the next restoration experiment.

## Source of truth

- GitHub is the source of truth for tracked source code.
- `main` is the stable/reference branch. Do **not** commit, merge, force-push, or otherwise modify `main` without Cooper's explicit approval.
- PS2 hardware development happens on `ps2-hardware-integration` unless Cooper explicitly selects another branch.
- ChatGPT should commit actual source changes directly to the GitHub development branch. Do not use `.patch` files or local patch-application scripts as the normal development workflow.
- Keep commits small and attributable. When several changes are intentionally grouped into a memory/performance baseline, document exactly what is being tested.

## Local checkout

Cooper's local repository is primarily the build and real-hardware test workspace.

Normal loop:

1. ChatGPT inspects the current GitHub development branch.
2. ChatGPT makes controlled source changes and commits/pushes them to that branch.
3. Cooper pulls the exact requested commit with GitHub Desktop.
4. The local working tree should normally be clean before the build.
5. Cooper builds locally using the PS2Build environment and `ps2.yaml`.
6. Cooper tests the resulting `build/bin/client.elf` on a real PS2.
7. Cooper reports the result, including logs/screenshots/checkpoints/performance when available.
8. A successful commit becomes the new hardware-known-good point. A failed commit is fixed/reverted before unrelated restoration work is stacked on it.

Do not use GitHub Actions or an emulator result as a substitute for the real-hardware acceptance test.

## Local changes and commits

- Cooper normally should **not commit source changes** to the active ChatGPT-managed PS2 development branch while ChatGPT is also editing it. This avoids conflicting writers and unclear test provenance.
- If Cooper intentionally changes source locally, preserve the change and tell ChatGPT before discarding/resetting it so it can be reviewed and incorporated.
- Do not commit generated build output, ELFs, temporary logs, local runtime IP/configuration changes, or test artifacts unless explicitly agreed.
- A local checkout is not disposable: it can contain runtime configuration, build products, logs, and diagnostic material. But GitHub remains the authoritative source for tracked source code.

## Real PS2 is authoritative

- Real PS2 hardware is the acceptance target.
- PCSX2 and desktop builds are diagnostic tools only.
- A change is not considered hardware-known-good until it has been tested on real hardware.
- When emulator and hardware behavior disagree, investigate the hardware-specific path rather than assuming the emulator result is correct.

## PS2Build baseline

Unless a specific experiment requires otherwise:

- Build locally from `ps2.yaml`.
- Keep the EE optimization baseline at `-O1` until correctness/UB and hardware restoration are sufficiently stable.
- Preserve the established DEV9/NETMAN/SMAP and USB/BDM embedded-IRX configuration/order unless new real-hardware evidence justifies changing it.
- Do not introduce `-O3`, LTO, `-ffast-math`, aggressive aliasing assumptions, or similar compiler experiments together with feature-restoration work. Compiler tuning gets its own isolated hardware test.

## Gameplay-first memory policy

The retail PS2 has 32 MiB total EE RAM. Gameplay state wins over cosmetic fidelity.

- Keep terrain heights, collision and floor colours. Terrain textures are optional and currently disabled.
- Keep terrain/scene residency bounded instead of eagerly materialising the full 104x104 map as `Ground` objects.
- Cull dynamic render entities by distance before World3D insertion and enforce per-frame population caps. Network/update state may continue outside the render radius.
- Prefer the local player and nearby interactable NPCs/players over distant entities and effects.
- Keep the minimap disabled until its >1 MiB backing/cached assets can be made substantially cheaper or lazy.
- Static loc loading must be streamed/bounded. Purely decorative locs whose only user-facing interaction is Examine should not consume PS2 model/render residency.
- When filtering an Examine-only loc, preserve gameplay-relevant collision/pathing state separately when needed; do not make a blocking wall/object walk-through merely because its visual model was culled.
- Essential/actionable locs (doors, stairs, ladders, trees/resources, banks, ranges, altars, gates, quest/interact objects, etc.) have priority over decorative scenery.
- Effects, projectiles, ground decoration, overhead elements and cosmetic UI are lower priority and receive strict caps.
- Avoid thousands of tiny heap allocations. Prefer bounded pools/arenas or contiguous metadata where lifetime permits.
- Allocation failure must never become silent memory corruption. Add capacity checks and fail/skip optional rendering work safely.

## Restoration strategy

Restore missing game systems gradually, but optimize each system for the gameplay-first profile rather than blindly restoring the desktop implementation.

Current intended progression:

1. Stabilize untextured colored terrain + hills with bounded/lazy Ground residency.
2. Restore the local player model with a strict low-memory model/cache path.
3. Stabilize nearby player/NPC entity culling and population caps.
4. Restore essential gameplay UI only.
5. Restore actionable static locs through bounded streaming/filtering.
6. Restore essential structural walls/buildings only where needed for understanding/navigation/gameplay.
7. Restore resource trees/rocks/fishing/etc. and interactive scenery.
8. Add ground items, projectiles and overhead elements under strict caps.
9. Increase draw distance/entity limits/presentation rate only when measured headroom exists.
10. Cosmetic scenery/textures/minimap are last and may remain disabled if they compromise stability.

### Loc restoration rule

Do **not** re-enable the entire desktop static-loc scene in one step.

```text
known-good terrain
  -> load one bounded loc mapsquare/window
  -> decode loc metadata without building every model
  -> preserve collision/pathing
  -> admit actionable locs
  -> discard Examine-only decorative renderables
  -> cap model/cache residency
  -> real PS2 test
  -> add structural shell if memory allows
  -> real PS2 test
```

## Performance and memory rule

Every restored subsystem should answer four questions:

1. Does it render and behave correctly?
2. Does it leak, overrun, corrupt, or exhaust memory?
3. What does it cost in EE/scene/model/cache memory?
4. What does it cost in update/render/GS time?

Prefer measurement over visual guesses. Maintain or add lightweight PS2 diagnostics for useful quantities such as free EE heap, scene arena usage/high-water mark, model/cache memory, temporary loc count, resident Ground count, visible tiles/locs/models/entities, culled entity counts, update time, scene/model time, raster/draw time, GS upload/present time, update FPS/presented FPS and network time.

Do not keep expensive debug instrumentation permanently if it materially harms the hardware profile. Use focused diagnostics, gather evidence, then reduce/remove them when the issue is understood.

## Commit discipline

A good PS2 test commit should have one clear question, for example:

- `PS2: make Ground residency lazy`
- `PS2: cull dynamic entities before scene insertion`
- `PS2: restore basic terrain height rendering`
- `PS2: enable local player model`
- `PS2: stream actionable locs only`
- `PS2: reduce loc model cache pressure`

For every commit sent for hardware testing, record or communicate the exact commit SHA. Test that exact revision before proceeding.

## Known-good checkpoints and rollback

- Keep track of the most recent real-hardware-known-good commit.
- If a new commit causes a hang, corruption, major regression, or severe performance loss, stop stacking unrelated restoration changes on it.
- Diagnose/fix the failing commit or return to the previous known-good point.
- Do not mask a regression by simultaneously disabling another unrelated system unless the explicit goal is to establish a new low-memory baseline and every bundled change is documented.

## Merge to main

Nothing is merged to `main` automatically.

Only merge/promote the PS2 development branch when Cooper explicitly approves it after hardware testing. Before that merge, review the cumulative diff, remove temporary debugging/test machinery that is no longer useful, and ensure local-only configuration/build artifacts are not included.
