#ifdef __PS2__
// the project's own build-flavor define (-Dclient, distinguishing this binary from
// mapview/playground) collides with a struct field literally named `client` in PS2SDK's
// sifrpc-common.h - undef it here, this file has no legitimate use of that macro itself.
#undef client
#include <kernel.h>
#include <loadfile.h>
#include <malloc.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <string.h>
#include <timer.h>
#include <delaythread.h>

#include <libpad.h>

#include <netman.h>
#include <ps2ip.h>
#include <ps2ipee.h>

#include <gsKit.h>

#include <stdarg.h>
#include <stdio.h>

#include "../client.h"
#include "../custom.h"
#include "../defines.h"
#include "../gameshell.h"
#include "../inputtracking.h"
#include "../pixmap.h"
#include "../thirdparty/bzip.h"

extern ClientData _Client;
extern InputTracking _InputTracking;
extern Custom _Custom;

// Root-caused a real-hardware-only crash to this: `errno` on this toolchain isn't the usual
// reentrant `#define errno (*__errno())` macro (confirmed - every .c file in this project compiles
// to an actual unresolved `errno` symbol reference, not a call through __errno()) - it resolves to
// a WEAK `int errno;` provided by the prebuilt ps2ip/lwIP library. That library's own errno lives
// immediately after another of its internal globals (tcp_port, a 2-byte value) with zero padding
// between them, so errno lands 2 bytes off a 4-byte boundary - not something anything in this
// project controls, since libps2ip.a is a prebuilt binary. This makes EVERY heap allocation
// (newlib's malloc -> _sbrk_r, which unconditionally clears errno first) an unaligned 4-byte store,
// which real MIPS hardware faults on (confirmed via EPC/BadVAddr from a real crash, decoded with
// addr2line+nm against this exact build) but PCSX2's emulation silently tolerates - explaining why
// this was invisible through this entire project's PCSX2-only testing until the first real-hardware
// boot. A plain, strong definition here is properly aligned (normal .bss placement, no adjacent
// odd-sized neighbor) and - being strong, not weak - the linker prefers it over ps2ip's copy for
// every reference in the program, sidestepping the bad layout entirely without touching ps2ip.
int errno;

static GSGLOBAL *gsGlobal;
static GSTEXTURE screenTexture;

// SCREEN_WIDTH/HEIGHT (765x503, defines.h) isn't 64-pixel-aligned, and PS2 GS VRAM textures are
// hardware-tiled with a row stride that must be. Two direct attempts to force the unaligned size
// through anyway failed: reprogramming the DISPLAY1 scanout register had zero visible effect
// (confirmed live via PCSX2 memory inspection), and manually setting screenTexture.TBW made the
// corruption far worse (confirmed via screenshot) - real evidence that fighting gsKit's own
// alignment assumptions blind isn't the way.
//
// First fix here rendered into a smaller SCREEN_FB_WIDTH x SCREEN_FB_HEIGHT (640x480, 64-aligned)
// physical framebuffer and cropped the game's full 765x503 canvas into it - the same pattern
// xbox.c/dreamcast.c use. That's a real fix for the alignment problem, but it means only ~84% of
// the canvas is ever visible (125px cut off horizontally, 23px vertically), which reads as
// "zoomed in" since nothing is actually made smaller, just less of it is shown.
//
// This instead keeps a full-resolution SOURCE texture - CPU-side rendering is untouched, still the
// full 765x503 logical canvas, just padded up to SCREEN_SRC_WIDTH (768, the next 64-aligned value)
// so the GS tiling requirement above is still satisfied; the padding columns are simply never drawn
// to. platform_update_surface()'s existing gsKit_prim_sprite_texture_3d call already takes
// independent source (u/v) and destination (x/y) rectangles for its textured quad - sampling the
// real 765x503 region as source but a 640x480 destination lets the GS hardware scale the whole
// canvas down for free as part of a draw call that already happens every frame, at zero added
// per-pixel CPU cost (unlike resampling every platform_blit_surface() call individually, which is
// called many times per frame for arbitrary small rects - see platform.c's draw_rect/fill_rect).
#define SCREEN_SRC_WIDTH 768

// Filling the full 640x480 destination with a 765x503 source independently per axis (640/765 for
// width, 480/503 for height) is a non-uniform stretch - the two ratios aren't equal, so circles
// become slightly elliptical and the whole picture reads as subtly "off"/zoomed wrong even though
// nothing is cropped. Scaling both axes by the same (smaller) ratio instead keeps things
// proportional; width is the more restrictive axis here (765/640 > 503/480), so scaling to exactly
// fill the width leaves the scaled height short of 480 - centered with a thin letterbox rather than
// stretched to fill it.
#define SCREEN_DST_WIDTH SCREEN_FB_WIDTH
#define SCREEN_DST_HEIGHT (SCREEN_HEIGHT * SCREEN_FB_WIDTH / SCREEN_WIDTH)
#define SCREEN_DST_X 0
#define SCREEN_DST_Y ((SCREEN_FB_HEIGHT - SCREEN_DST_HEIGHT) / 2)

// SIO2MAN/PADMAN ship in the console's own boot ROM - no IRX to bundle for basic digital/analog input.
static char padDmaBuf[256] __attribute__((aligned(64)));
static struct padButtonStatus padData;

static void SleepCb(s32 alarmId, u16 time, void *common)
{
	iWakeupThread(*(int *)(common));
}

static void SleepMsApprox()
{
	int tid = GetThreadId();
	SetAlarm(1000 * 16, &SleepCb, &tid);
	SleepThread();
}

// Auto-detects whether the game's own asset tree (the same rom/cache/client/... layout already
// shipped under build/bin/rom/) lives on a mounted USB mass-storage device (real hardware, or a
// proper USB-stick boot) rather than being served through PCSX2's host: dev-only shortcut - see
// platform_init()'s own path comment: every other cache/asset path in this codebase is a plain
// relative path with no device prefix, which PCSX2 transparently redirects to the PC's real
// build/bin folder, but that redirect simply doesn't exist on real hardware.
//
// Tries both device names actually seen in this project's own real-hardware testing: "mass0:"
// (the BDM-based stack's numbered convention - platform_init() below loads this exact stack, and a
// real boot log confirmed PCSX2's own USB-boot chain mounts a drive this way before handing off to
// this ELF) and plain "mass:" as a fallback, in case a given loader/boot path ever exposes it
// unnumbered instead. Checking both costs nothing once one succeeds.
const char *ps2_cache_prefix(void) {
    static bool checked = false;
    static const char *prefix = "";
    if (!checked) {
        checked = true;
        static const char *const candidates[] = {"mass0:/", "mass:/"};
        // platform_init() now calls this immediately after loading USB, before network
        // setup - the drive has had essentially no settling time yet at that point, unlike when
        // this was only ever reached lazily much later in boot (after 30s+ of network setup had
        // already elapsed in the background). A short fixed delay before the very first attempt,
        // matching a real, working reference project's own approach to this exact timing problem
        // (OptiJuegos/ReleasePlusPlus's Ps2UsbMass::initialize(), DelayThread(500*1000) before its
        // first availability check), costs far less than even one avoidable retry pass if a
        // not-yet-ready device makes each individual fopen() attempt itself slow rather than fast.
        SleepMsApprox();
        SleepMsApprox();
        SleepMsApprox();
        SleepMsApprox();
        SleepMsApprox();
        // A real boot log measured ~1s between the USB driver coming up and the drive actually
        // being mounted - retry for a few seconds to comfortably cover that, still bounded so a
        // PCSX2 dev/testing boot with no mass-storage device attached doesn't hang, just pays a
        // one-time few-second tax on this specific probe.
        for (int i = 0; i < 50 && !prefix[0]; i++) {
            for (size_t c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++) {
                char path[64];
                snprintf(path, sizeof(path), "%srom/cache/client/crc", candidates[c]);
                FILE *probe = fopen(path, "rb");
                if (probe) {
                    fclose(probe);
                    prefix = candidates[c];
                    break;
                }
            }
            if (!prefix[0]) {
                SleepMsApprox();
            }
        }
        rs2_log("usb: mass storage %s - cache/asset paths using %s\n", prefix[0] ? "found" : "not found",
                 prefix[0] ? prefix : "relative (host: under PCSX2)");
    }
    return prefix;
}

// See platform.c's rs2_log()/rs2_error() - a hang or crash reached via uLaunchELF/a real USB boot
// has no live console at all (unlike PCSX2, or ps2link when its own link survives), so this is the
// only way to get a postmortem trace back off real hardware afterward: plug the drive into a PC and
// read the file. Tries mass0: first (a real USB-stick/uLaunchELF boot), falling back to a plain
// relative path (works fine under PCSX2's host: shortcut, landing in build/bin/boot.log) - and
// permanently disables itself the first time NEITHER works, so a session with no writable device at
// all doesn't retry a failing fopen() on every single log line for the rest of the run.
//
// Explicit "w" (create/truncate) on the FIRST successful write to a given path, "a" (append)
// afterward - not "a" from the start. A real hardware test came back with no boot.log anywhere on
// the drive despite many earlier rs2_log() calls that must have run (title screen rendered, login
// succeeded) - "a" is defined by the C standard to create a missing file, but that's exactly the
// kind of guarantee a minimal embedded FAT driver (bdmfs_fatfs here) is plausible to not fully
// honor for a file that doesn't exist yet. This sidesteps relying on that specific guarantee.
void ps2_log_to_file(const char *format, va_list args) {
    static int mode = -1; // -1 = not yet determined, 0 = mass0:, 1 = relative, 2 = disabled
    static bool created[2] = {false, false};
    if (mode == 2) {
        return;
    }
    const char *paths[2] = {"mass0:/boot.log", "boot.log"};
    int start = mode >= 0 ? mode : 0;
    for (int i = start; i < 2; i++) {
        FILE *file = fopen(paths[i], created[i] ? "a" : "w");
        if (file) {
            created[i] = true;
            mode = i;
            vfprintf(file, format, args);
            fflush(file);
            fclose(file);
            return;
        }
    }
    mode = 2;
}

// A real-hardware boot spends a long time in platform_init() below before client_load() ever gets
// a chance to draw its own "Connecting to fileserver"/"Unpacking ..." progress bar (client_draw_
// progress(), entry/client.c) - GS/dmaKit setup previously happened at the very END of
// platform_init(), after every network/USB/pad wait, so the screen was black (no GS output bound
// at all yet) for the whole thing. Real hardware measured this at a couple of minutes total (real
// EE/IOP silicon is much slower than PCSX2 emulating it, especially for the cache/ondemand.zip
// decompression client_load() does afterward) - not something this project can make fundamentally
// faster, so at minimum it shouldn't look like a hang. Moved GS/dmaKit init to the very top of
// platform_init() (it has no dependency on anything below it - pure EE-side GS/DMA setup, untouched
// by the IOP reset) so this can draw a plain, font-less percentage bar (raw gsKit_prim_sprite
// rectangles - no PixFont/PixMap/cache dependency, none of which exist yet this early) at each
// major boot milestone.
// Exposed (not static) and reused beyond platform_init() itself - see model.c's model_unpack(),
// which has no PixFont/Client available to use the normal client_draw_progress() mechanism but
// still needs *some* way to show it's actively progressing through a real, potentially very slow
// (real EE silicon, not PCSX2's dynarec) CPU-bound loop over every model in the game, rather than
// looking indistinguishable from a genuine hang. No longer clears the screen itself (previously
// redundant during platform_init(), which already clears once before the first call) specifically
// so a reuse mid-client_load() doesn't wipe out the game's own already-drawn loading background.
void ps2_boot_progress(int percent) {
    int bar_w = 300, bar_h = 20;
    int x = (SCREEN_FB_WIDTH - bar_w) / 2;
    int y = (SCREEN_FB_HEIGHT - bar_h) / 2;
    gsKit_prim_sprite(gsGlobal, x, y, x + bar_w, y + bar_h, 1, GS_SETREG_RGBAQ(0x60, 0x60, 0x60, 0x80, 0x00));
    int fill_w = (bar_w - 4) * percent / 100;
    if (fill_w > 0) {
        gsKit_prim_sprite(gsGlobal, x + 2, y + 2, x + 2 + fill_w, y + bar_h - 2, 2, GS_SETREG_RGBAQ(0xff, 0xff, 0xff, 0x80, 0x00));
    }
    gsKit_queue_exec(gsGlobal);
    gsKit_sync_flip(gsGlobal);
}

bool platform_init(void) {
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsGlobal = gsKit_init_global();
    // This is the physical output framebuffer (640x480, aligned - see the comment above
    // platform_new()), separate from screenTexture's full 765x503 logical canvas - gsKit scales
    // between the two when drawing the sprite in platform_update_surface(). gsKit_init_global()'s
    // own default Width/Height (based on the console's detected video standard) also didn't match
    // what platform_update_surface() draws into, producing a black screen despite no draw errors,
    // so this still needs to be set explicitly either way.
    gsGlobal->Width = SCREEN_FB_WIDTH;
    gsGlobal->Height = SCREEN_FB_HEIGHT;
    gsGlobal->PSM = GS_PSM_CT24;
    gsGlobal->DoubleBuffering = GS_SETTING_ON;
    gsGlobal->ZBuffering = GS_SETTING_OFF;
    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

    gsKit_init_screen(gsGlobal);
    gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));
    ps2_boot_progress(0);

    // 5th real network attempt. The 4th (ps2ip+netman+smap[NETMAN mode] loaded via
    // SifExecModuleBuffer, matching ps2sdk's own official sample) got real, confirmed progress -
    // netman AND smap's own _start() both returned success (modres=0) for the first time ever,
    // meaning PCSX2 genuinely executed the real driver code instead of substituting its own HLE -
    // but NetManIoctl(GET_LINK_STATUS) stayed at -1 (not 0/1 - per netman.c's own real source,
    // -1 specifically means NetManInit() never actually finished internally) for a full 10s, and
    // DHCP never started. Confirmed PCSX2's own Ethernet settings were genuinely fine (Enabled,
    // sockets mode, screenshot-verified) - not a config gap. Confirmed via smap.c's own real
    // source that link status is a genuine PHY hardware register poll
    // (_smap_read_phy(...)&SMAP_PHY_BMSR_LINK, refreshed by a background timer), not something a
    // command-line flag can bypass. Tried switching back to PCAP-bridged (untested in combination
    // with real module execution before) - still no change.
    //
    // User then provided a SEPARATE, already-verified-working reference project (httpechotest,
    // same PCSX2/ps2build setup) proving this exact ps2ip+netman+smap[NETMAN] architecture DOES
    // work here. Diffing its main.cpp against this file found two real, concrete gaps:
    // 1. It embeds and loads DEV9 itself via SifExecModuleBuffer too (embed_irx: [dev9, netman,
    //    smap]) - this file still loaded DEV9 via SifLoadModule("rom0:DEV9",...), leaving it
    //    substituted by PCSX2's own HLE while netman/smap ran for real - a real, inconsistent mix
    //    this file alone would never have caught without a working reference to diff against.
    // 2. It does a full sceSifInitRpc->SifIopReset->SifIopSync->sceSifInitRpc sequence before
    //    loading ANY modules - a genuine IOP reset to a clean slate, which this file never did
    //    (just a single SifInitRpc(0), matching every attempt so far). Moved to the very start of
    //    platform_init(), before SIO2MAN/PADMAN, since a reset this late would wipe them out.
    //
    // dev9/netman/smap are embedded (see ps2.yaml's embed_irx) again, back in their original
    // position right after this reset, with USB deferred until AFTER DHCP completes below. Two
    // separate attempts at interleaving USB with network bring-up - loading USB first so dev9/
    // netman/smap could be file-loaded instead of embedded (once with usbhdfsd, then again after
    // reverting to the BDM stack) - BOTH produced a new real-hardware-only stall mid-network-bring-
    // up that this original, fully-sequential ordering never hit, on two otherwise-unrelated USB
    // driver stacks. That is itself evidence: it's not something specific to either driver, it is
    // "USB activity anywhere near network bring-up" - the exact same class of problem this project's
    // own precedent just below already found once (SIO2MAN/PADMAN moved to AFTER network bring-up
    // due to IOP resource/thread contention with SMAP's worker threads), just a different pair of
    // modules. Given a boot-time stall is a worse failure than the ~39KB EE-memory cost the
    // file-loading attempt was trying to avoid, this reverts to the proven-stable full sequence:
    // network completely up first, USB loaded only once that's done, matching how this project's
    // very first working real-hardware boot was structured before any of this reordering began.
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {
    }
    while (!SifIopSync()) {
    }
    SifInitRpc(0);

    // SIO2MAN/PADMAN moved to AFTER network bring-up (below), matching httpechotest's exact
    // sequence (which never loads them at all, being network-only) - this file previously loaded
    // them here, before dev9/netman/smap, and never once saw PCSX2's own real DEV9/SMAP driver
    // banners ("DEV9 device driver v1.0", "SMAP (Version 2.26.0)", etc) in any boot log, while
    // httpechotest (identical embedded module bytes, verified via md5) saw them every time and
    // got a real link. Real, plausible IOP resource/thread-priority contention from PADMAN's own
    // polling thread competing with SMAP's worker threads during bring-up - not yet proven, but a
    // concrete, cheap, previously-untried reordering to test directly.
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    ps2_boot_progress(5);

    extern unsigned char dev9_embed_irx[];
    extern unsigned int size_dev9_embed_irx;
    extern unsigned char netman_embed_irx[];
    extern unsigned int size_netman_embed_irx;
    extern unsigned char smap_embed_irx[];
    extern unsigned int size_smap_embed_irx;

    int dev9_modres = -1;
    int dev9_ret = SifExecModuleBuffer(dev9_embed_irx, size_dev9_embed_irx, 0, NULL, &dev9_modres);
    rs2_log("net: dev9 ret=%d modres=%d\n", dev9_ret, dev9_modres);
    ps2_boot_progress(15);

    int netman_modres = -1;
    int netman_ret = SifExecModuleBuffer(netman_embed_irx, size_netman_embed_irx, 0, NULL, &netman_modres);
    rs2_log("net: netman ret=%d modres=%d\n", netman_ret, netman_modres);
    ps2_boot_progress(25);

    int smap_modres = -1;
    int smap_ret = SifExecModuleBuffer(smap_embed_irx, size_smap_embed_irx, 0, NULL, &smap_modres);
    rs2_log("net: smap ret=%d modres=%d\n", smap_ret, smap_modres);
    ps2_boot_progress(35);

    int netman_init_ret = NetManInit();
    rs2_log("net: NetManInit=%d\n", netman_init_ret);
    ps2_boot_progress(40);

    // Wait for link BEFORE calling ps2ipInit()/ps2ip_setconfig() at all - matching the working
    // reference project's exact real order (httpechotest/main.cpp, same PCSX2/ps2build setup,
    // already proven to work), which this file had wrong: it called ps2ipInit()+setconfig()
    // immediately after NetManInit() and only waited for link afterward, concurrently with the
    // DHCP wait. Up to 10s, logging every ~2s.
    int link_state = 0;
    for (int i = 0; i < 100; i++) {
        link_state = NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0);
        if (i % 20 == 0) {
            rs2_log("net: [%d ms] link_state=%d\n", i * 100, link_state);
            // Nudges the bar within its 40-50 range across the up-to-100 iterations of this loop -
            // distinguishes "stuck immediately, iteration 0" from "grinding slowly through many
            // iterations before eventually timing out", which a single before/after marker can't.
            ps2_boot_progress(40 + i / 10);
        }
        if (link_state == NETMAN_NETIF_ETH_LINK_STATE_UP) {
            break;
        }
        SleepMsApprox();
    }
    rs2_log("net: final link_state=%d\n", link_state);
    ps2_boot_progress(50);

    struct ip4_addr zero_ip = {0}, zero_mask = {0}, zero_gw = {0};
    int ps2ip_init_ret = ps2ipInit(&zero_ip, &zero_mask, &zero_gw);

    // Read the real current config first (populates netif_name="sm0" and every other field for
    // real), THEN only flip dhcp_enabled on it - matching the reference exactly. This file
    // previously passed a blank, zeroed t_ip_info with dhcp_enabled=1 and everything else
    // (including netif_name) empty - a real, plausible reason DHCP was never actually enabled on
    // the real "sm0" interface at all.
    t_ip_info ip_info = {0};
    int getconfig_ret = ps2ip_getconfig("sm0", &ip_info);
    ip_info.dhcp_enabled = 1;
    int setconfig_ret = ps2ip_setconfig(&ip_info);
    rs2_log("net: ps2ipInit=%d getconfig=%d setconfig=%d\n", ps2ip_init_ret, getconfig_ret, setconfig_ret);

    // Then poll for DHCP completion with a timeout (20s) rather than block forever - a genuinely
    // offline/misconfigured PCSX2 network adapter must not hang the whole boot sequence.
    t_ip_info current_info = {0};
    for (int i = 0; i < 200; i++) {
        ps2ip_getconfig("sm0", &current_info);
        if (i % 20 == 0) {
            rs2_log("net: [%d ms] dhcp_status=%d ip=0x%08x\n", i * 100,
                     current_info.dhcp_status, (unsigned int)current_info.ipaddr.s_addr);
            // Nudges the bar within its 55-75 range across the up-to-200 iterations of this loop -
            // same reasoning as the link-wait loop's nudge above.
            ps2_boot_progress(55 + i / 10);
        }
        if (current_info.dhcp_status == DHCP_STATE_BOUND) {
            break;
        }
        SleepMsApprox();
    }
    rs2_log("net: dhcp_status=%d ip=0x%08x\n", current_info.dhcp_status,
             (unsigned int)current_info.ipaddr.s_addr);
    ps2_boot_progress(75);

    // USB mass storage - only loaded now, once networking is fully up, after two separate attempts
    // at loading it earlier (to let dev9/netman/smap above be file-loaded instead of embedded)
    // caused a new real-hardware-only stall mid-network-bring-up on two different USB driver
    // stacks (see this function's own comment above SifInitRpc() for the full account).
    //
    // BDM-based (usbd+iomanX+bdm+bdmfs_fatfs+usbmass_bd, ~97KB across 5 modules), not the lighter
    // usbhdfsd (a single, simple FAT driver) this project briefly switched to: usbhdfsd traded that
    // memory cost away but turned out to have a real reliability problem under sustained I/O on
    // real hardware instead, confirmed via TWO independent real-hardware hangs in different code
    // paths - one opening ~830 separate small map files one at a time, the other seeking through
    // ~8929 entries inside a SINGLE already-combined ondemand.zip archive. The second case rules out
    // "too many separate files" as the explanation (that archive was never many files), pointing at
    // usbhdfsd's own sustained-I/O handling specifically - BDM is what this project's own boot
    // loader already uses successfully to read client.elf itself, and what a separate, more mature
    // PS2 homebrew project (OptiJuegos/ReleasePlusPlus) uses in its own shipped real-hardware builds.
    //
    // Load order follows that same reference project's own sequencing: iomanX/bdm/bdmfs_fatfs load
    // first so the *receiving* side (filesystem-over-block-device) is ready, THEN usbd/usbmass_bd
    // load last, triggering the actual connect/mount once something is already listening for it.
    extern unsigned char iomanx_embed_irx[];
    extern unsigned int size_iomanx_embed_irx;
    extern unsigned char bdm_embed_irx[];
    extern unsigned int size_bdm_embed_irx;
    extern unsigned char bdmfs_fatfs_embed_irx[];
    extern unsigned int size_bdmfs_fatfs_embed_irx;
    extern unsigned char usbd_embed_irx[];
    extern unsigned int size_usbd_embed_irx;
    extern unsigned char usbmass_bd_embed_irx[];
    extern unsigned int size_usbmass_bd_embed_irx;

    int iomanx_modres = -1;
    int iomanx_ret = SifExecModuleBuffer(iomanx_embed_irx, size_iomanx_embed_irx, 0, NULL, &iomanx_modres);
    rs2_log("usb: iomanX ret=%d/%d\n", iomanx_ret, iomanx_modres);
    ps2_boot_progress(78);

    int bdm_modres = -1;
    int bdm_ret = SifExecModuleBuffer(bdm_embed_irx, size_bdm_embed_irx, 0, NULL, &bdm_modres);
    rs2_log("usb: bdm ret=%d/%d\n", bdm_ret, bdm_modres);
    ps2_boot_progress(80);

    int bdmfs_fatfs_modres = -1;
    int bdmfs_fatfs_ret = SifExecModuleBuffer(bdmfs_fatfs_embed_irx, size_bdmfs_fatfs_embed_irx, 0, NULL, &bdmfs_fatfs_modres);
    rs2_log("usb: bdmfs_fatfs ret=%d/%d\n", bdmfs_fatfs_ret, bdmfs_fatfs_modres);
    ps2_boot_progress(83);

    int usbd_modres = -1;
    int usbd_ret = SifExecModuleBuffer(usbd_embed_irx, size_usbd_embed_irx, 0, NULL, &usbd_modres);
    rs2_log("usb: usbd ret=%d/%d\n", usbd_ret, usbd_modres);
    ps2_boot_progress(86);

    int usbmass_bd_modres = -1;
    int usbmass_bd_ret = SifExecModuleBuffer(usbmass_bd_embed_irx, size_usbmass_bd_embed_irx, 0, NULL, &usbmass_bd_modres);
    rs2_log("usb: usbmass_bd ret=%d/%d\n", usbmass_bd_ret, usbmass_bd_modres);
    ps2_boot_progress(88);

    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, padDmaBuf);

    // Force DualShock2 analog mode, locked so the player can't toggle it back off with the
    // physical Analog button. Without this the pad boots in digital mode (confirmed via a real
    // PCSX2 log showing "AL: Off") - digital buttons still work, but the analog stick axes are
    // never actually centered/driven, which is what made the right-stick camera read a
    // permanently off-center value and spin continuously in one direction. Bounded wait (not an
    // infinite loop) so a real disconnected-controller boot can't hang here - if it times out,
    // padSetMainMode is still called (harmless no-op on a pad that was never present) and
    // platform_poll_events() already tolerates a pad that never reaches PAD_STATE_STABLE.
    for (int i = 0; i < 100; i++) {
        int state = padGetState(0, 0);
        if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) {
            break;
        }
        SleepMsApprox();
    }
    padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    ps2_boot_progress(90);

    StartTimerSystemTime();

    return true;
}

void platform_new(GameShell *shell) {
    // TODO lowmem/audio bring-up (ps2snd/audsrv) - video/input/networking come first per the
    // project's phasing, matches how sdl2.c also skips audio init entirely under _Client.lowmem.
    // No physical keyboard exists on PS2 - this drives entry/client.c's on-screen virtual keyboard
    // to auto-open at text-entry focus points instead (see client.h's virtual_keyboard_* fields).
    shell->has_keyboard = false;
    // Full logical canvas height (SCREEN_HEIGHT, 503 - no alignment requirement), padded width
    // (SCREEN_SRC_WIDTH, 768 - see the comment above platform_init()) - holds the whole 765x503
    // canvas rather than a 640x480 crop of it; platform_update_surface() scales it down to the
    // physical 640x480 output when drawing it as a textured sprite.
    screenTexture.Width = SCREEN_SRC_WIDTH;
    screenTexture.Height = SCREEN_HEIGHT;
    // CT16 halves this texture's VRAM cost vs CT32 (2 bytes/pixel instead of 4) - needed to fit
    // alongside the double-buffered framebuffer in GS's 4MB VRAM (see platform_init()'s note).
    screenTexture.PSM = GS_PSM_CT16;
    // LINEAR instead of NEAREST now that this texture is genuinely scaled (768x503 source down to
    // 640x480 destination, a non-integer ratio) rather than drawn 1:1 - NEAREST would alias/look
    // blocky under real scaling.
    screenTexture.Filter = GS_FILTER_LINEAR;
    // Delayed=1 ("delay upload to VRAM") isn't documented beyond its header comment (gsKit ships
    // prebuilt, no source to check its exact semantics against) and we already explicitly call
    // gsKit_texture_upload() ourselves every frame - 0 removes any ambiguity about the two
    // interacting.
    screenTexture.Delayed = 0;
    screenTexture.Mem = memalign(128, gsKit_texture_size(screenTexture.Width, screenTexture.Height, screenTexture.PSM));
    if (!screenTexture.Mem) {
        // Same reasoning as the VRAM check below: unchecked, the memset right after this would be a
        // NULL-pointer write - a real hardware fault with no handler installed, which just freezes
        // the display rather than crashing loudly, i.e. indistinguishable from a hang. Fail loudly
        // instead.
        rs2_error("platform_new: memalign failed for the screen texture (%dx%d) - out of EE RAM\n", screenTexture.Width, screenTexture.Height);
    }
    screenTexture.Vram = gsKit_vram_alloc(gsGlobal, gsKit_texture_size(screenTexture.Width, screenTexture.Height, screenTexture.PSM), GSKIT_ALLOC_USERBUFFER);
    if (screenTexture.Vram == GSKIT_ALLOC_ERROR) {
        // Unchecked, this silently aliases the texture onto VRAM address 0 - typically the live
        // framebuffer itself - so every subsequent texture upload corrupts the display instead of
        // failing loudly. Fail loudly instead.
        rs2_error("platform_new: gsKit_vram_alloc failed for the screen texture (%dx%d) - out of GS VRAM\n", screenTexture.Width, screenTexture.Height);
    }
    if (screenTexture.Mem) {
        memset(screenTexture.Mem, 0, gsKit_texture_size(screenTexture.Width, screenTexture.Height, screenTexture.PSM));
    }
}

void platform_free(void) {
    gsKit_deinit_global(gsGlobal);
    free(screenTexture.Mem);
}

void platform_update_surface(void) {
    // Each of the two double-buffered surfaces still needs its own margin cleared before it's
    // first displayed, not just whichever was active at startup, hence the per-frame clear.
    gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));
    gsKit_texture_upload(gsGlobal, &screenTexture);
    // Source rect is the real SCREEN_WIDTH x SCREEN_HEIGHT (765x503) canvas, NOT
    // screenTexture.Width/Height (768x503 - includes the alignment padding columns, which would
    // otherwise get sampled into the scaled output as a thin sliver of garbage/black on the right
    // edge). Destination rect (SCREEN_DST_*, see above) is a uniformly-scaled, letterboxed
    // sub-rectangle of gsGlobal->Width/Height (640x480), not the full thing - aspect-correct
    // instead of stretched.
    gsKit_prim_sprite_texture_3d(gsGlobal, &screenTexture,
                                  SCREEN_DST_X, SCREEN_DST_Y, 0, 0, 0,
                                  SCREEN_DST_X + SCREEN_DST_WIDTH, SCREEN_DST_Y + SCREEN_DST_HEIGHT, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                                  GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
    gsKit_queue_exec(gsGlobal);
    gsKit_sync_flip(gsGlobal);
}

void platform_blit_surface(Surface *surface, int x, int y) {
    // pix24.c packs pixels as (R<<16)|(G<<8)|B. GS_PSM_CT16 is the standard PS2 GS 16-bit format:
    // 5 bits each of R/G/B plus a 1-bit alpha/mask, packed (LSB to MSB) as R,G,B,A - i.e. value =
    // (1<<15) | (B5<<10) | (G5<<5) | R5, same R/G/B channel order as CT32, just fewer bits/channel
    // (see the CT32 note this replaced for why channel order matters here) and set opaque.
    uint16_t *dst = (uint16_t *)screenTexture.Mem;
    uint32_t *src = (uint32_t *)surface->pixels;
    // x/y arrive in the game's full logical canvas (SCREEN_WIDTH x SCREEN_HEIGHT, 765x503), which
    // is now also what screenTexture holds (see the comment above platform_init()) - no offset
    // needed, just clip to the real canvas bounds (screenTexture.Width itself is padded wider, to
    // SCREEN_SRC_WIDTH, purely for GS tiling alignment - nothing should actually draw into that
    // padding).
    for (int row = 0; row < surface->h; row++) {
        int screen_y = y + row;
        if (screen_y < 0) {
            continue;
        }
        if (screen_y >= SCREEN_HEIGHT) {
            break;
        }
        uint16_t *dst_row = &dst[screen_y * screenTexture.Width];
        uint32_t *src_row = &src[row * surface->w];
        for (int col = 0; col < surface->w; col++) {
            int screen_x = x + col;
            if (screen_x < 0) {
                continue;
            }
            if (screen_x >= SCREEN_WIDTH) {
                break;
            }
            uint32_t argb = src_row[col];
            uint32_t r5 = ((argb >> 16) & 0xFF) >> 3;
            uint32_t g5 = ((argb >> 8) & 0xFF) >> 3;
            uint32_t b5 = (argb & 0xFF) >> 3;
            dst_row[screen_x] = (uint16_t)((1 << 15) | (b5 << 10) | (g5 << 5) | r5);
        }
    }
}

// Software-cursor "restore-under" support - platform_blit_surface() above permanently overwrites
// screenTexture.Mem (there's no per-frame clear and no alpha), so any small overlay that moves
// between frames (the virtual cursor in entry/client.c) has to explicitly save what it's about to
// cover and restore it before moving on, or it leaves a permanent trail on every panel that
// doesn't happen to redraw itself that frame (sidebar, chatback, background chrome, static
// title-screen bezel - anything not the always-redrawn 3D viewport). Raw CT16 texels, not RGB -
// restoring the exact bytes that were already there is lossless and needs no format conversion.
void platform_save_region(int x, int y, int w, int h, uint16_t *out) {
    uint16_t *src = (uint16_t *)screenTexture.Mem;
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            out[row * w + col] = (sy < 0 || sy >= SCREEN_HEIGHT || sx < 0 || sx >= SCREEN_WIDTH) ? 0 : src[sy * screenTexture.Width + sx];
        }
    }
}

void platform_restore_region(int x, int y, int w, int h, const uint16_t *in) {
    uint16_t *dst = (uint16_t *)screenTexture.Mem;
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= SCREEN_HEIGHT) {
            continue;
        }
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= SCREEN_WIDTH) {
                continue;
            }
            dst[sy * screenTexture.Width + sx] = in[row * w + col];
        }
    }
}

void platform_poll_events(Client *c) {
    int state = padGetState(0, 0);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
        return;
    }

    padRead(0, 0, &padData);

    // Left stick drives a virtual mouse cursor - TODO: no on-screen cursor sprite drawn yet.
    // Text entry is covered by entry/client.c's on-screen virtual keyboard (see has_keyboard
    // above) - the left stick doubles as its cursor too when the keyboard is open.
    int dx = (padData.ljoy_h - 128) / 24;
    int dy = (padData.ljoy_v - 128) / 24;
    if (dx != 0 || dy != 0) {
        c->shell->mouse_x = MAX(0, MIN(SCREEN_WIDTH - 1, c->shell->mouse_x + dx));
        c->shell->mouse_y = MAX(0, MIN(SCREEN_HEIGHT - 1, c->shell->mouse_y + dy));
        c->shell->idle_cycles = 0;

        if (_InputTracking.enabled) {
            inputtracking_mouse_moved(&_InputTracking, c->shell->mouse_x, c->shell->mouse_y);
        }
    }

    // padButtonStatus.btns is active-low (0 == pressed).
    bool cross = !(padData.btns & PAD_CROSS);
    bool circle = !(padData.btns & PAD_CIRCLE);
    static bool cross_was_down = false;
    static bool circle_was_down = false;

    if (cross && !cross_was_down) {
        if (c->virtual_keyboard_visible) {
            // Keyboard intercepts Cross entirely while open, instead of the normal click path -
            // otherwise a key commit at these screen coordinates could also land on whatever UI
            // happens to be underneath the overlay (e.g. the login screen's buttons).
            c->controller_keyboard_confirm_pressed = true;
        } else {
            c->shell->mouse_click_x = c->shell->mouse_x;
            c->shell->mouse_click_y = c->shell->mouse_y;
            c->shell->mouse_click_button = 1;
            c->shell->mouse_button = 1;
            if (_InputTracking.enabled) {
                inputtracking_mouse_pressed(&_InputTracking, c->shell->mouse_x, c->shell->mouse_y, 0);
            }
        }
    } else if (!cross && cross_was_down) {
        c->shell->mouse_button = 0;
        if (_InputTracking.enabled) {
            inputtracking_mouse_released(&_InputTracking, 0);
        }
    }

    if (circle && !circle_was_down) {
        c->shell->mouse_click_x = c->shell->mouse_x;
        c->shell->mouse_click_y = c->shell->mouse_y;
        c->shell->mouse_click_button = 2;
        c->shell->mouse_button = 2;
        if (_InputTracking.enabled) {
            inputtracking_mouse_pressed(&_InputTracking, c->shell->mouse_x, c->shell->mouse_y, 1);
        }
    } else if (!circle && circle_was_down) {
        c->shell->mouse_button = 0;
        if (_InputTracking.enabled) {
            inputtracking_mouse_released(&_InputTracking, 1);
        }
    }

    cross_was_down = cross;
    circle_was_down = circle;

    // Right stick drives camera rotation - by setting the SAME shell->action_key[1..4] flags that
    // K_LEFT/K_RIGHT/K_UP/K_DOWN already set via key_pressed() (see defines.h/gameshell.c), rather
    // than duplicating client_update_orbit_camera()'s eased-velocity model here. That function
    // (entry/client.c) already reads these as plain "is this direction held" booleans and already
    // gates the EVENT_CAMERA_POSITION anti-cheat packet on them - reusing it is free and correct,
    // whereas re-deriving a second velocity curve from raw stick magnitude would just be a second,
    // uncoordinated easing curve stacked on top of the existing one for no real benefit (the
    // existing targets are small, +-24 yaw / +-12 pitch out of a 2048-unit circle).
    // Deadzone +-40 (of 0-255, center 128): comfortably outside real analog stick idle drift while
    // still reachable well short of full deflection. Independent per axis, matching how ljoy_h/v
    // are already handled for the cursor above (not a circular deadzone).
    const int CAM_DEADZONE = 40;
    int rh = padData.rjoy_h - 128;
    int rv = padData.rjoy_v - 128;
    c->shell->action_key[1] = rh < -CAM_DEADZONE ? 1 : 0; // yaw left
    c->shell->action_key[2] = rh > CAM_DEADZONE ? 1 : 0;  // yaw right
    c->shell->action_key[3] = rv < -CAM_DEADZONE ? 1 : 0; // pitch up (zoom in/tilt down)
    c->shell->action_key[4] = rv > CAM_DEADZONE ? 1 : 0;  // pitch down (zoom out/tilt up)

    // L1/R1 cycle sidebar tabs (see handleControllerTabInput() in entry/client.c) - edge-detected
    // (not level, unlike the camera above) so holding the button doesn't rapid-fire tab changes;
    // a physical mouse click, what this otherwise mirrors, is already a single discrete event.
    bool l1 = !(padData.btns & PAD_L1);
    bool r1 = !(padData.btns & PAD_R1);
    static bool l1_was_down = false, r1_was_down = false;
    if (r1 && !r1_was_down) {
        c->controller_tab_step = 1;
    }
    if (l1 && !l1_was_down) {
        c->controller_tab_step = -1;
    }
    l1_was_down = l1;
    r1_was_down = r1;

    // Triangle/Square/Select/Start - one-shot press-edge flags, meaning assigned and consumed in
    // handleControllerButtonInput()/the virtual keyboard logic in entry/client.c (kept there so
    // every button's real-world MEANING lives in one platform-agnostic place, while only the
    // hardware bit lives here).
    bool triangle = !(padData.btns & PAD_TRIANGLE);
    bool square = !(padData.btns & PAD_SQUARE);
    bool select = !(padData.btns & PAD_SELECT);
    bool start = !(padData.btns & PAD_START);
    static bool triangle_was_down = false, square_was_down = false, select_was_down = false, start_was_down = false;
    if (triangle && !triangle_was_down) {
        c->controller_back_pressed = true;
    }
    if (square && !square_was_down) {
        c->controller_inventory_pressed = true;
    }
    if (select && !select_was_down) {
        c->controller_snap_camera_pressed = true;
    }
    if (start && !start_was_down) {
        c->controller_start_pressed = true;
    }
    triangle_was_down = triangle;
    square_was_down = square;
    select_was_down = select;
    start_was_down = start;

    // L2/R2 - fine zoom, held (level, not edge) since it's a continuous nudge rather than a
    // discrete action. Independent of the right stick's pitch-based zoom, for finer control.
    bool l2 = !(padData.btns & PAD_L2);
    bool r2 = !(padData.btns & PAD_R2);
    c->controller_zoom_bias = r2 ? 1 : (l2 ? -1 : 0);

    // D-pad - only meaningful to the virtual keyboard's grid navigation (dead input otherwise);
    // edge-detected like the tab triggers above, one step per press.
    bool dpad_up = !(padData.btns & PAD_UP);
    bool dpad_down = !(padData.btns & PAD_DOWN);
    bool dpad_left = !(padData.btns & PAD_LEFT);
    bool dpad_right = !(padData.btns & PAD_RIGHT);
    static bool dpad_up_was_down = false, dpad_down_was_down = false, dpad_left_was_down = false, dpad_right_was_down = false;
    if (dpad_up && !dpad_up_was_down) {
        c->controller_dpad_y = -1;
    }
    if (dpad_down && !dpad_down_was_down) {
        c->controller_dpad_y = 1;
    }
    if (dpad_left && !dpad_left_was_down) {
        c->controller_dpad_x = -1;
    }
    if (dpad_right && !dpad_right_was_down) {
        c->controller_dpad_x = 1;
    }
    dpad_up_was_down = dpad_up;
    dpad_down_was_down = dpad_down;
    dpad_left_was_down = dpad_left;
    dpad_right_was_down = dpad_right;
}

uint64_t rs2_now(void) {
    u64 clocks = GetTimerSystemTime();
    u32 sec, usec;
    TimerBusClock2USec(clocks, &sec, &usec);
    return (uint64_t)sec * 1000 + usec / 1000;
}

void rs2_sleep(int ms) {
    DelayThread(ms * 1000);
}

void platform_set_wave_volume(int wavevol) {
    (void)wavevol;
}

void platform_play_wave(int8_t *src, int length) {
    (void)src, (void)length;
}

void platform_set_midi_volume(float midivol) {
    (void)midivol;
}

void platform_set_jingle(int8_t *src, int len) {
    (void)src, (void)len;
}

void platform_set_midi(const char *name, int crc, int len) {
    (void)name, (void)crc, (void)len;
}

void platform_stop_midi(void) {
}
#endif
