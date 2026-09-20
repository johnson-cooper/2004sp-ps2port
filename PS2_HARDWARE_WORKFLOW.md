# PS2 Hardware Development Workflow

This file is the standing workflow for the PlayStation 2 port. Read it before making or testing PS2-specific changes, including in future development sessions.

## Current integration status

- Active development branch: `ps2-hardware-integration`
- Branch starting point: `main` commit `673657249ef18526e6c33a3d4381953c132c2782`
- Last fully real-hardware-known-good integration commit: `6791e4cd09d4a92b2c8604c366dd364060c83478` (confirmed stable on real PS2 on 2026-09-20). This baseline includes the prior hardware-stable scene/memory/render work and controller-native UX, accepted plane-transition/chat readability fixes, the model-projection reciprocal path, inactive-loc detail culling, sparse model depth buckets, distance-based remote-player LOD, the divisor-2 / 25 Hz presentation target, restored project-wide `-O1` model compilation, and the slightly relaxed PS2 camera visibility wedge that reduces wall/building-edge popping at oblique camera angles.
- Terrain finding: the old 4-byte scene-arena alignment made terrain residency layout-sensitive. A 16-byte/qword-aligned base and qword-aligned bump allocations fixed that class of hardware crash. 2x3, 3x3, 16x16, 32x32, 48x48 and 80x80 traversal-bridge terrain configurations have run on real hardware. Keep qword alignment as mandatory.
- Traversal finding: 32x32 and 48x48 fixed terrain windows ended before the client could comfortably reach every normal server-driven scene recenter. The 80x80 terrain window bridges that gap and supports continuous `REBUILD_NORMAL` traversal. Static loc/model residency is deliberately smaller at 72x72 to reduce dense-scene pressure while still repopulating walls/objects after transitions.
- Scene-lifetime finding: `client_clear_caches()` reset the scene bump arena before `world3d_reset()` tore down the old World3D. Because resident `Ground` nodes are arena-backed but own normal-heap wall/decor/ground-decoration attachments, zeroing the arena first erased those owner pointers and leaked static-world wrappers on each `REBUILD_NORMAL`. Commit `d0aa55048ea99ebe66eab538040f3642414331cc` preserves the old arena bytes until World3D teardown can free those attachments; repeated Lumbridge traversal then survived hardware stress testing.
- Texture finding: `PIX3D_POOL_COUNT=1` is sufficient for the low-memory texture path and costs 64 KiB for the active texel slot. Water/rivers are hardware-good with this configuration. Treat water as the essential terrain texture; non-water terrain textures can later fall back to average/flat colour so the single slot is not churned unnecessarily.
- Picking finding: the 512x334 PS2 software target had mismatched viewport/model-picking coordinates. Commit `04099cbaf5de2d59545e5fbc1fd89948d8abd749` corrects the model AABB/triangle picking path while leaving UI input unchanged; real hardware confirmed the fix.
- Local-player finding: the old dedicated post-World3D local-player pass behaved like a final overlay, so the avatar appeared in front of walls and also forced `player->lowmem = true`, suppressing sequence transforms. Baseline `0775c19761f0b0815f7c7efab520495ae039443d` submits the local player into World3D temporary-location ordering with lowmem disabled. Real hardware confirms animation and correct wall/loc occlusion with no crash.
- Performance finding: with restored static loc/model rendering, live update FPS varies roughly 30-50 FPS depending on scene load. The expensive 3D software render/presentation remains throttled by `PS2_RENDER_DIVISOR`. Real-hardware testing found the attempted VU1 path performed worse, so keep the known-good EE software renderer for now. Commit `980bfd5e3d0239d9e7ec6b6b46b89abd1e11c0e0` improves useful draw distance by replacing the radius-14 square visibility mask with a camera-aware radius-18 wedge. A dedicated divisor-2 test (25 Hz presentation target) was a major real-hardware smoothness improvement and is now the preferred presentation target while further renderer work continues; divisor 1 remains unproven.
- Dense-entity finding: isolated real-hardware tests of ground-item pile LOD (`bd40227c527ee9a00ba1340c2507ac3fb00aba37`), a nearest-first 32 ordinary-remote-player render budget (`65bdc0fb9d51cc38a323c91b9d629ec790e6984a`), and a 48-draw inactive-loc budget outside the near ring (`baf4ac43e022c14e0ea3cc658d18c01786b07ae4`) each improved performance a little by themselves. Commit `05a0cc2cceb46796413d9bc618b268309f2fd344` combines all three on the integration branch and requires a combined real-hardware acceptance run before becoming the known-good baseline.
- Current policy: gameplay-first 32 MiB profile — bounded/lazy Ground residency, eighteen-tile camera-facing render radius, one-slot low-memory terrain texturing with water as the essential texture, animated local player in normal scene ordering, bounded static-loc placement, capped nearby dynamic entities, normal interactive gameplay UI and inventory icons with simplified chrome, minimap off.
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
- Keep the EE optimization baseline at `-O1`. A dedicated whole-client `-O2` hardware test reached the title screen and then crashed on real PS2; a dedicated `-O0` test booted but was noticeably slower; and a model.c-only `-O2` hardware test was stable but ended up noticeably slower than the normal `-O1` renderer. Therefore `-O1` remains the hardware performance/stability baseline. Do not keep selective model `-O2` in the normal build unless new evidence reverses that result.
- Preserve the established DEV9/NETMAN/SMAP and USB/BDM embedded-IRX configuration/order unless new real-hardware evidence justifies changing it.
- Do not introduce `-O3`, LTO, `-ffast-math`, aggressive aliasing assumptions, or similar compiler experiments together with feature-restoration work. Compiler tuning gets its own isolated hardware test.

## Gameplay-first memory policy

The retail PS2 has 32 MiB total EE RAM. Gameplay state wins over cosmetic fidelity.

- Keep terrain heights, collision and floor colours. Water is the essential terrain texture; non-water terrain textures are optional.
- Keep terrain/scene residency bounded instead of eagerly materialising the full 104x104 map as `Ground` objects.
- Cull dynamic render entities by distance before World3D insertion and enforce per-frame population caps. Network/update state may continue outside the render radius.
- Prefer the local player and nearby interactable NPCs/players over distant entities and effects.
- Keep the minimap disabled until its backing/cached assets can be made substantially cheaper or lazy.
- Static loc loading must be streamed/bounded. Purely decorative locs whose only user-facing interaction is Examine should not consume PS2 model/render residency when memory is tight.
- When filtering a loc, preserve gameplay-relevant collision/pathing state separately when needed; do not make a blocking wall/object walk-through merely because its visual model was culled.
- Essential/actionable locs (doors, stairs, ladders, trees/resources, banks, ranges, altars, gates, quest/interact objects, etc.) have priority over decorative scenery.
- Effects, projectiles, ground decoration, overhead elements and cosmetic UI are lower priority and receive strict caps.
- Avoid thousands of tiny heap allocations. Prefer bounded pools/arenas or contiguous metadata where lifetime permits.
- Allocation failure must never become silent memory corruption. Add capacity checks and fail/skip optional rendering work safely.

## Restoration and optimization strategy

Restore missing game systems gradually, but optimize each system for the gameplay-first profile rather than blindly restoring the desktop implementation.

Current intended progression:

1. Preserve baseline `6791e4cd09d4a92b2c8604c366dd364060c83478`: qword-aligned 80x80 terrain / 72x72 loc traversal, compact scene allocations, lazy interfaces, one-slot water texture, corrected picking, animated/occluded local player, normal gameplay UI, radius-18 camera-aware visibility with relaxed edge margins, controller-native UX, bounded scene rebuilds on floor/plane changes, the PS2 chatbox readability pass, the hardware-stable model-projection reciprocal path, accepted distant inactive-loc detail culling, the hardware-good sparse model depth-bucket sorter, accepted distance-based remote-player LOD, project-wide `-O1`, and divisor-2 / 25 Hz presentation.
2. Continue optimizing the known-good EE software 3D path with measured, isolated changes. Do not reintroduce VU1 unless Cooper explicitly asks to revisit it; the attempted VU1 path performed worse on real hardware.
3. Re-profile World3D traversal, terrain transform/raster, model transform/sort/raster, viewport upscale, and GS/full-surface presentation when pursuing further performance work.
4. Preserve camera-aware culling when experimenting with draw distance rather than returning to the expensive square visibility window.
5. Re-profile dense loc scenes before increasing static model residency or entity limits.
6. Reduce `PS2_RENDER_DIVISOR` only when measured real-hardware headroom exists.
7. Restore/verify remaining ground items, projectiles and overhead elements under strict caps.
8. Increase presentation rate/draw distance/entity limits only when measured headroom exists.
9. Cosmetic terrain textures/minimap remain last and may stay disabled if they compromise stability.

### Loc restoration rule

Do **not** re-enable the entire desktop static-loc scene in one step.

```text
hardware-good traversal
  -> retain/load loc metadata needed around the moving player
  -> preserve collision/pathing
  -> admit actionable/structural locs first
  -> cap model/cache residency
  -> real PS2 test
  -> extend the moving loc window
  -> real PS2 test
```

The final loc strategy should follow the traversable player rather than depending on one permanent startup-only local block. If a larger temporary loc window is used to prove correctness, replace it with a moving/recycled bounded window if its memory cost is too high.

## Performance and memory rule

Every restored subsystem should answer four questions:

1. Does it render and behave correctly?
2. Does it leak, overrun, corrupt, or exhaust memory?
3. What does it cost in EE/scene/model/cache memory?
4. What does it cost in update/render/GS time?

Prefer measurement over visual guesses. Maintain or add lightweight PS2 diagnostics for useful quantities such as free EE heap, scene arena usage/high-water mark, model/cache memory, resident Ground/loc counts, visible models/entities, culled counts, update time, scene/model time, raster/draw time, GS upload/present time, update FPS/presented FPS and network time.

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
