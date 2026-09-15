# PS2 Hardware Development Workflow

This file is the standing workflow for the PlayStation 2 port. Read it before making or testing PS2-specific changes, including in future development sessions.

## Source of truth

- GitHub is the source of truth for source code.
- `main` is the stable/reference branch. Do **not** commit, merge, force-push, or otherwise modify `main` without Cooper's explicit approval.
- PS2 hardware development happens on a dedicated test/development branch.
- ChatGPT should commit actual source changes directly to the GitHub development branch. Do not use `.patch` files or local patch-application scripts as the normal development workflow.
- Keep commits small and attributable: ideally one hardware experiment, restoration step, or optimization per commit.

## Local checkout

Cooper's local repository is primarily the build and real-hardware test workspace.

Normal loop:

1. ChatGPT inspects the current GitHub development branch.
2. ChatGPT makes a controlled source change and pushes/commits it to that branch.
3. Cooper pulls that exact commit with GitHub Desktop.
4. The local working tree should normally be clean before the build.
5. Cooper builds locally using the PS2Build environment and `ps2.yaml`.
6. Cooper tests the resulting `build/bin/client.elf` on a real PS2.
7. Cooper reports the result, including logs/screenshots/checkpoints/performance when available.
8. A successful commit becomes the new hardware-known-good point. A failed commit is fixed or reverted before unrelated changes are introduced.

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
- Keep the EE optimization baseline at `-O1` until the correctness/UB audit and hardware restoration are sufficiently stable.
- Preserve the established DEV9/NETMAN/SMAP and USB/BDM embedded-IRX configuration/order unless new real-hardware evidence justifies changing it.
- Do not introduce `-O3`, LTO, `-ffast-math`, aggressive aliasing assumptions, or similar compiler experiments together with feature-restoration work. Compiler tuning gets its own isolated hardware test.

## Restoration strategy

Restore missing game systems gradually. Never re-enable a large collection of systems in one untestable change.

Current intended progression is approximately:

1. Harden and stabilize the current stripped-down world/scene path.
2. Basic ground/terrain rendering.
3. Terrain height variation / hills.
4. Essential UI and viewport layers.
5. Local player model and animation.
6. Simple static locs and trees.
7. Walls, buildings, and larger/multi-tile locs.
8. NPCs, initially with controlled population limits.
9. Other players/entities with controlled population limits.
10. Ground items, projectiles, overhead elements, effects, and secondary scene systems.
11. Increase draw distance, entity limits, model/texture quality, and presentation rate toward the best hardware balance.

This order is a guide, not a reason to ignore dependencies discovered during testing. Break each category into smaller hardware-testable commits whenever possible.

Example for loc restoration:

```text
known-good
  -> enable one simple loc model
  -> real PS2 test
  -> enable static trees
  -> real PS2 test
  -> enable walls
  -> real PS2 test
  -> enable building models
  -> real PS2 test
  -> enable multi-tile locs
  -> real PS2 test
  -> restore/increase normal loc population
  -> real PS2 test
  -> optimize loc/model path
  -> real PS2 test
  -> new known-good
```

## Performance and memory rule

Every restored subsystem should answer four questions:

1. Does it render and behave correctly?
2. Does it leak, overrun, corrupt, or exhaust memory?
3. What does it cost in EE/scene/model/cache memory?
4. What does it cost in update/render/GS time?

Prefer measurement over visual guesses. Maintain or add lightweight PS2 diagnostics for useful quantities such as:

- free EE heap;
- scene arena usage/high-water mark;
- model/cache memory;
- temporary loc count;
- visible tiles/locs/models/entities;
- update time;
- scene/model time;
- raster/draw time;
- GS upload/present time;
- update FPS and presented/render FPS;
- network time.

Do not keep expensive debug instrumentation permanently if it materially harms the hardware profile. Use focused diagnostics, gather evidence, then reduce/remove them when the issue is understood.

## Commit discipline

A good PS2 test commit should have one clear question, for example:

- `PS2: bound renderer depth buckets`
- `PS2: restore basic terrain height rendering`
- `PS2: enable local player model`
- `PS2: restore static tree locs`
- `PS2: reduce loc model cache pressure`

Do not combine unrelated networking, rendering, memory, UI, and compiler changes in the same test commit unless they are inseparable.

For every commit sent for hardware testing, record or communicate the exact commit SHA. Test that exact revision before proceeding.

## Known-good checkpoints and rollback

- Keep track of the most recent real-hardware-known-good commit.
- If a new commit causes a hang, corruption, major regression, or severe performance loss, stop stacking unrelated changes on it.
- Diagnose/fix the failing commit or return to the previous known-good point.
- Do not mask a regression by simultaneously disabling another unrelated system.

## Merge to main

Nothing is merged to `main` automatically.

Only merge/promote the PS2 development branch when Cooper explicitly approves it after hardware testing. Before that merge, review the cumulative diff, remove temporary debugging/test machinery that is no longer useful, and ensure local-only configuration/build artifacts are not included.
