# 2004Scape / RuneScape 2 for PlayStation 2

A native PlayStation 2 port of the early RuneScape 2 C client, adapted for the PS2's 32 MiB of main memory, IOP-driven networking and storage, GS video output, and DualShock 2 controls.

This branch currently targets **revision 254** protocol/cache compatibility and is developed alongside the 2004Scape/Lost City ecosystem.

> **Real-hardware status:** playable on a real PlayStation 2.  
> **Hardware-known-good baseline:** `2c6fb1c8f20d92f834dce3b3b4463dcac27a707d`  
> **Confirmed:** September 18, 2026  
> **Development branch:** `ps2-hardware-integration`

Real PS2 hardware is the acceptance target for this port. PCSX2 is extremely useful for debugging, but an emulator result is not considered proof that a change works on the console.

## What works

The current hardware-tested build can boot from USB, initialize PS2 networking, read the RuneScape cache from mass storage, connect to a revision-254 server, load into the world, traverse normal map rebuilds, render terrain/water/static locations and players, interact with the normal RuneScape UI, and play with a DualShock 2 without requiring a mouse or keyboard.

The PS2-specific client includes:

- Real DEV9/SMAP Ethernet networking with DHCP.
- USB mass-storage cache/config loading.
- A 32 MiB gameplay-first memory profile.
- Bounded terrain and static-location residency.
- Textured water using a one-slot low-memory texture pool.
- Camera-aware scene visibility tuned for the PS2 software renderer.
- Correct model/object picking in the reduced PS2 viewport.
- Animated local-player rendering with normal world occlusion.
- A PS2 virtual cursor.
- Controller-native context-menu navigation.
- Controller-native inventory/bank/shop grid navigation.
- On-screen focus outline for the selected grid slot.
- R3 escape from grid navigation without closing the interface.
- True L2/R2 camera-distance zoom.
- Right-stick camera rotation and tilt.
- Configurable cursor speed and analog deadzones.
- An on-screen virtual keyboard path for controller-only text entry.
- Real-hardware file logging for difficult boot/runtime failures.

Some expensive desktop-client features are deliberately reduced or disabled. This is a PS2 port, not a bit-for-bit desktop rendering profile.

---

## Quick start

You need:

- A PlayStation 2 capable of launching homebrew ELFs. Free McBoot with wLaunchELF/uLaunchELF is a common setup.
- A USB mass-storage device for the client/cache, unless you are using another supported launcher setup.
- Ethernet connectivity. Fat PS2 systems require a compatible network adapter; Slim systems have Ethernet built in.
- A DHCP-capable local network.
- A PC/server running a compatible revision-254 2004Scape/Lost City server.
- [PS2BUILD SDK](https://ps2.techwritescode.dev/) if you want to compile the client yourself.

### 1. Build

Clone the repository and use the PS2 hardware branch:

```sh
git clone https://github.com/johnson-cooper/2004sp-ps2port.git
cd 2004sp-ps2port
git checkout ps2-hardware-integration
```

Install PS2BUILD, then from the repository root run:

```sh
ps2build build
```

PS2BUILD uses `ps2.yaml` and produces:

```text
build/bin/client.elf
```

The PS2 target intentionally builds the EE executable at `-O1`. Do not assume aggressive compiler flags are a free performance upgrade: real hardware has exposed correctness and layout problems that PCSX2 did not.

There is also a Windows helper, `build-ps2-local.bat`, which checks that the local PS2BUILD environment is available.

### 2. Prepare the runtime files

The install should be self-contained. A recommended USB layout is:

```text
mass0:/
└── 2004sp/
    ├── client.elf
    ├── config.ini
    └── rom/
        └── cache/
            └── client/
                ├── crc
                └── ...
```

The exact cache tree from the build/runtime package should remain under `rom/`. The PS2 port validates an installation by looking for:

```text
rom/cache/client/crc
```

When launched from a USB subdirectory, the client attempts to keep using that exact directory. It can also fall back to the historical `mass0:/` / `mass:/` root layout.

**Keep `config.ini` beside `client.elf`.** A subfolder installation is expected to contain both its config and cache.

### 3. Configure the server

Create `config.ini` from `example.ini`.

At minimum, set the server address:

```ini
socketip = 192.168.1.100
username = your_username
password = your_password
```

Replace `192.168.1.100` with the **LAN IP address of the machine running your game server**.

Do **not** use:

```ini
socketip = localhost
```

on a real PS2. From the console, `localhost` refers to the PS2 itself.

The native game socket uses:

```text
43594 + portoff
```

so with the default:

```ini
portoff = 0
```

the server must be reachable on TCP port **43594**.

The client currently has an automatic PS2 login path, so putting working credentials in `config.ini` is the most predictable hardware setup.

A useful basic config is:

```ini
socketip = 192.168.1.100
portoff = 0

username = shared
password = pazazword

remember_username = 1
remember_password = 1
allow_commands = 1
```

Use credentials that actually exist on your server.

### 4. Copy to USB and launch

Copy the prepared folder to a FAT-formatted USB drive, insert it into the PS2, and launch `client.elf` with your ELF launcher.

The port remembers the directory from which `client.elf` was launched. When possible it inherits the launcher's already-working FILEIO/USB environment. If that cannot be proven usable, the client falls back to initializing its own IOP/network/storage stack.

Connect Ethernet before booting. The PS2 networking path waits for the physical link and then requests an address through DHCP.

---

## DualShock 2 controls

The controller layer is designed around normal RuneScape actions rather than inventing a second gameplay system.

| Control | Action |
| --- | --- |
| **Left stick** | Move the virtual cursor |
| **Right stick** | Rotate / tilt the game camera |
| **D-pad** | Navigate inventory/bank/shop grids, context menus, controller settings, and virtual keyboard depending on context |
| **X / Cross** | Primary/default RuneScape action; confirm highlighted menu/grid/keyboard choices |
| **Circle** | Open the normal RuneScape right-click/Options menu; press again to close it |
| **Triangle** | Back / close the current normal modal context |
| **Square** | Jump directly to the Inventory tab |
| **L1 / R1** | Cycle backward/forward through available sidebar tabs |
| **L2** | Zoom camera out |
| **R2** | Zoom camera in |
| **L3** | Open/close PS2 controller settings |
| **R3** | **Exit D-pad grid navigation and return to the free cursor without closing the current inventory/bank/shop panel** |
| **Select** | Snap the camera orientation behind the local player |
| **Start** | Open/submit the controller text-entry keyboard where applicable |

### Primary action

Cross is intentionally treated as the normal top/default RuneScape action even if the client's one-button-mouse option would otherwise turn a left click into a context menu.

For example, when the cursor is over a tree, Cross should perform the normal top action such as **Chop down** rather than unnecessarily opening Options.

Circle always remains the explicit Options/right-click control.

### Context menus

After opening a RuneScape Options menu with Circle:

- D-pad Up/Down moves through the menu.
- Cross activates the selected option.
- Circle closes the Options menu.
- Triangle can also act as normal Back.

Moving the pointer normally hands menu highlighting back to the cursor.

### Inventory, bank and shop grid navigation

Pressing the D-pad while an item grid is available enters controller grid navigation.

The client:

1. Finds the appropriate visible `TYPE_INV` interface.
2. Selects a slot in row/column order.
3. Draws a black/yellow focus outline around the selected 32x32 slot.
4. Aligns the controller interaction hotspot with the selected slot.
5. Lets Cross and Circle use RuneScape's existing item actions.
6. Scrolls supported containing interfaces as selection moves beyond the visible clip.

The grid system works with empty slots as well as occupied slots.

**Press R3 to leave grid mode.** R3 is intentionally separate from Triangle/Back so the bank, inventory, shop, or other active panel remains open when pointer control is returned.

The explicit R3 escape is the real-hardware-confirmed way to switch back to the free cursor.

### Controller settings

Press **L3** to open the PS2 controller settings overlay.

Current settings include:

- Left-stick cursor deadzone.
- Cursor speed.
- Right-stick camera deadzone.
- Reset to defaults.

Current defaults are:

```text
Cursor deadzone: 20
Cursor speed:     5
Camera deadzone: 40
```

These settings are currently session-local; they are not yet persisted to `config.ini`.

---

## Camera

The PS2 port separates camera movement from pointer movement:

- **Right stick:** yaw/pitch camera control.
- **L2:** increase camera distance.
- **R2:** decrease camera distance.
- **Select:** immediately orient the camera behind the player.

L2/R2 change the actual distance passed to the orbit camera. They do not fake zoom by changing camera pitch.

The zoom range is clamped so the camera cannot be moved unreasonably close to the player or so far away that it creates an excessive rendering burden.

---

## Networking

Networking was one of the largest real-hardware differences encountered during development.

The working PS2 path uses:

- DEV9
- NETMAN
- SMAP
- PS2IP
- DHCP

The relevant IRX modules are embedded through `ps2.yaml`. Do not casually remove or reorder them.

The proven boot order is intentionally conservative:

```text
initialize RPC/IOP
        ↓
DEV9 / NETMAN / SMAP
        ↓
wait for Ethernet link
        ↓
PS2IP + DHCP
        ↓
network is fully established
        ↓
USB mass-storage stack
        ↓
load game/cache data
```

This order exists because real hardware repeatedly stalled when USB activity was introduced during network bring-up. The same behavior was not reliably reproduced in PCSX2.

### Why networking is nonblocking

An earlier version of the live client could appear catastrophically slow while connected even though rendering and scene management were healthy.

Profiling showed `recv()` calls taking roughly **8-16 seconds** while the TCP socket was alive. When the socket died, the same build immediately returned to stable full-speed updates.

The networking path was changed so game updates are not held hostage by a blocking receive. A network problem should not freeze the entire render/update loop.

---

## USB and cache loading

Real hardware also exposed storage behavior that emulator testing did not.

An earlier `usbhdfsd` path could hang during sustained real-hardware I/O. This occurred in more than one pattern, including large numbers of map reads and seeking through a combined archive.

The current port uses the BDM stack:

```text
USBD
IOMANX
BDM
BDMFS FATFS
USBMASS_BD
```

These modules are embedded because they are the components that provide the filesystem access needed to load anything else.

The client supports:

- Launcher-inherited USB/FILEIO when the launched directory can be proven readable.
- Self-initialized BDM mass storage when inheritance is unavailable.
- `mass0:/` and `mass:/` device naming.
- Subfolder installs beside `client.elf`.
- Root-layout compatibility.

For real-hardware postmortems the port can write `boot.log` to the USB device when writable storage is available.

---

## The 32 MiB problem

A retail PlayStation 2 has only **32 MiB of EE RAM**. The desktop-style client was never designed around that budget.

The port therefore follows a gameplay-first policy:

> Preserve the world, collision, interaction and nearby gameplay first. Spend memory on cosmetic fidelity only after the game is stable.

### Current PS2 memory/render profile

The current baseline includes:

- Compact PS2-specific scene structures.
- Qword-aligned scene arena allocations.
- **80x80** bounded terrain residency.
- **72x72** static-location placement window.
- Camera-aware visibility with an approximately **18-tile** forward-facing radius.
- Smaller back/side margins rather than an expensive full square visibility area.
- **One** low-memory terrain texture slot.
- Textured water retained as the important terrain texture case.
- Minimap disabled.
- Simplified decorative UI/chrome.
- Normal interactive inventory/sidebar/chat UI retained.
- Dynamic entities culled/capped before unnecessary renderer insertion.
- Login flames disabled.
- Scene rendering performed through the known-good EE software path.

The game still keeps the information needed to play; expensive presentation data is where compromises are made first.

### Software render target

The gameplay viewport is software-rendered at:

```text
512 x 334
```

The complete RuneScape UI is then composed into the PS2 presentation path and displayed on the physical GS output.

The GS screen texture uses 16-bit color to reduce VRAM pressure. The expensive software 3D presentation is throttled by the PS2 render divisor while game logic/network updates continue independently.

---

## Major hardware problems solved

Getting from "the ELF launches" to "RuneScape is playable on a real PS2" required solving several failures that were either invisible or substantially different in PCSX2.

### 1. Scene-arena alignment

Terrain loading was once layout-sensitive and could crash only on hardware.

The scene arena had been using insufficient alignment. Moving the arena base and bump allocations to **16-byte/qword alignment** fixed that class of terrain failure.

Qword alignment is now a hard requirement for PS2 scene allocations.

### 2. World-rebuild lifetime leak

The scene arena was being reset before the old `World3D` was torn down.

The `Ground` nodes themselves lived in the arena, but some wall/decor/ground-decoration attachments they owned lived on the normal heap. Resetting the arena first destroyed the owner pointers before teardown could free those attachments.

Repeated `REBUILD_NORMAL` traversal therefore leaked static-world data.

The order was fixed so the old world is properly torn down before its backing arena bytes are recycled.

### 3. Terrain traversal window

Smaller fixed terrain windows could render the starting area but failed to bridge ordinary server-driven scene recentering.

Testing progressed through small windows until an **80x80 terrain residency window** proved large enough for continuous traversal while still fitting the PS2 budget.

Static locations use a smaller **72x72** window to reduce dense-scene memory pressure.

### 4. Static locations and model pressure

Blindly restoring the desktop static world was too expensive.

The PS2 path bounds residency and prioritizes gameplay-useful world data instead of assuming the full desktop scene can live in memory indefinitely.

### 5. Water/textures

Completely discarding terrain textures saved memory but removed important visual information.

The working compromise uses a **single low-memory texel slot**. Water is retained while avoiding a large desktop-style texture pool.

### 6. Model picking

The PS2 viewport is smaller than the original logical client canvas. Object/model picking initially used mismatched coordinates, so what the player saw and what the client thought the pointer was over could disagree.

The picking path was corrected for the 512x334 PS2 software target without breaking normal UI coordinates.

### 7. Local-player rendering

A dedicated post-scene local-player draw originally behaved like an overlay:

- the player could appear in front of walls that should occlude them;
- forcing low-memory model behavior suppressed normal sequence transforms/animation.

The local player is now submitted into normal `World3D` ordering so walls/locations occlude correctly and animation works.

### 8. Useful draw distance without square-window cost

Simply increasing a square render radius was too expensive.

The PS2 path instead uses **camera-aware culling**, spending the visibility budget primarily in front of the camera and reducing work behind/to the sides. This increased useful view distance without paying for a much larger square of terrain every frame.

### 9. Network stalls

The live socket path originally allowed blocking `recv()` behavior to stall the game for seconds at a time.

Real timing evidence showed that rendering itself was not the cause. Network reads were made nonblocking/incremental so the game loop can continue.

### 10. IOP module ordering

Networking, controllers, USB and filesystems all involve the PS2's IOP and RPC/module environment.

Several combinations that looked reasonable in code stalled only on real hardware. The current initialization sequence reflects empirical hardware testing, particularly the rule that networking should be brought fully up before the self-initialized USB storage stack.

### 11. Sustained USB I/O

The lighter storage approach was attractive for memory reasons but unreliable under sustained hardware reads.

The current BDM stack costs more memory but has been substantially more dependable.

### 12. Mouse-first UI on a controller

RuneScape's interface assumes a mouse. Simply mapping an analog stick to a cursor works, but it is not enough for comfortable console play.

The PS2 port added:

- direct Cross primary actions;
- explicit Circle Options;
- D-pad context-menu selection;
- D-pad inventory/bank/shop slot navigation;
- a visible focus outline;
- exact slot/click alignment;
- R3 grid escape;
- L1/R1 tab cycling;
- controller settings;
- virtual keyboard support;
- console-style camera controls.

Importantly, these features still feed RuneScape's existing action/menu system rather than inventing incompatible gameplay packets.

### 13. VU experimentation

A VU1 acceleration path was tested during optimization work. Real-hardware results were worse than the known-good EE software renderer.

The current baseline therefore deliberately stays on the measured, stable EE software path. Future acceleration work should be treated as a separate hardware experiment rather than assumed to be faster because it uses a vector unit.

---

## Real hardware vs PCSX2

PCSX2 is invaluable for:

- logs;
- rapid iteration;
- networking diagnostics;
- register/debugger inspection;
- detecting ordinary crashes.

But it cannot replace hardware acceptance.

Problems that have differed between emulator and console during this project include:

- IOP module timing/order;
- DEV9/SMAP bring-up;
- USB sustained-I/O behavior;
- memory/layout sensitivity;
- scene-build hangs;
- filesystem behavior.

A commit only becomes a project hardware baseline after it has been explicitly tested on a real PS2.

The current confirmed baseline is:

```text
2c6fb1c8f20d92f834dce3b3b4463dcac27a707d
```

See `PS2_HARDWARE_WORKFLOW.md` for the development/test discipline used by the project.

---

## Troubleshooting

### "Error connecting to server"

Check:

1. `socketip` is the server PC's LAN address, not `localhost`.
2. The server is listening on TCP `43594 + portoff`.
3. The PC firewall allows the server.
4. PS2 and server PC are on networks that can reach each other.
5. Ethernet is connected before the client starts.
6. DHCP is available.

### `connect() error: Host is unreachable (118)`

The PS2 has no usable route to the configured host.

Start with physical Ethernet/link, DHCP/router configuration, `socketip`, and the PC firewall. Do not debug rendering for this error.

### Client cannot find the cache

For a subfolder install, verify:

```text
<install>/client.elf
<install>/config.ini
<install>/rom/cache/client/crc
```

Keep the rest of the cache/assets under the same `rom/` tree.

### The build works in PCSX2 but hangs on hardware

Treat this as a hardware bug, not a successful build.

Check `boot.log` if it was created, identify the last hardware checkpoint reached, and compare against the known-good baseline before changing multiple systems at once.

### Networking broke after changing IRX modules

Restore the `ps2.yaml` baseline first.

The current embedded DEV9/NETMAN/SMAP and BDM storage setup is intentional. Multiple attempts to reclaim a small amount of memory by changing module placement/order caused worse boot-time hardware failures.

### D-pad grid navigation has captured the UI

Press **R3**.

R3 is the dedicated, real-hardware-confirmed escape from grid navigation. It releases controller grid focus while leaving the current panel open.

### Performance changes heavily by location

This is expected to some extent. Dense static-location/model scenes cost more than open terrain.

Do not judge performance from one scene alone. The project uses real-hardware profiling and isolated changes before raising render distance, entity caps, texture residency or presentation rate.

---

## Development rules

PS2 development is intentionally conservative because the console is much less forgiving than the desktop client.

The core rules are:

1. GitHub is the tracked-source source of truth.
2. Work on `ps2-hardware-integration` unless another branch is explicitly selected.
3. Do not modify `main` without explicit approval.
4. Make one isolated hardware question/change at a time.
5. Build locally with `ps2.yaml`.
6. Test the exact commit on a real PS2.
7. Only then promote it to the hardware-known-good baseline.
8. Stop stacking unrelated changes after a regression.
9. Preserve qword scene alignment.
10. Preserve the proven network/USB initialization order unless new hardware evidence justifies changing it.
11. Keep the EE optimization baseline at `-O1` while correctness remains the priority.
12. Do not commit generated ELFs, build directories, local IP addresses, passwords, or test logs.

For the full workflow, read:

```text
PS2_HARDWARE_WORKFLOW.md
```

---

## Project lineage

This port is built from the C99 RuneScape client work derived from **Client3** and adapted for the 2004Scape/Lost City ecosystem, with substantial PS2-specific work in rendering, memory management, I/O, networking, filesystem handling, controller UX and hardware diagnostics.

Related projects:

- [2004Scape / Lost City](https://github.com/LostCityRS/Server)
- [Client3](https://github.com/lesleyrs/Client3)
- [PS2BUILD SDK](https://ps2.techwritescode.dev/)
- [PS2SDK](https://github.com/ps2dev/ps2sdk)

The goal is not just to make RuneScape display on a PS2 emulator. The goal is a client that is **actually comfortable and stable enough to play on original PlayStation 2 hardware**.

---

## Current priorities

The current baseline is playable and controller-friendly. Future work should preserve that baseline while focusing on measured improvements such as:

- further EE software-renderer optimization;
- reducing dense-scene model/cache pressure;
- restoring remaining effects only under safe caps;
- improving controller focus/navigation for non-grid dialogue/interface controls;
- persistent controller settings;
- additional USB/launcher compatibility where hardware evidence supports it;
- presentation-rate improvements when real-hardware headroom is proven.

Features such as the minimap or substantially larger cosmetic texture residency should remain lower priority than world traversal, interaction, memory safety and frame time.

---

## Credits

This project stands on work from the RuneScape preservation/reimplementation community and the PS2 homebrew community, including the original Client3 C port, Lost City/2004Scape, PS2SDK, gsKit, PS2BUILD, and the many open-source libraries already credited in the source tree.

Special emphasis for this fork is placed on **real-hardware testing**: many of the most important fixes in the PS2 port were found only by testing exact revisions on an actual console and treating emulator/hardware disagreement as useful evidence rather than noise.
