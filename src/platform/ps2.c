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

static GSGLOBAL *gsGlobal;
static GSTEXTURE screenTexture;

extern unsigned char dev9_irx[];
extern unsigned int size_dev9_irx;

extern unsigned char netman_irx[];
extern unsigned int size_netman_irx;

extern unsigned char smap_irx[];
extern unsigned int size_smap_irx;

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

bool platform_init(void) {
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
    // dev9_irx/netman_irx/smap_irx are now embedded via ps2build's own native embed_irx: feature
    // (see ps2.yaml) instead of the hand-rolled ps2_net_modules.h header used before - matches
    // httpechotest's exact mechanism (BIN2C-generated, external non-const linkage) rather than a
    // manually-generated static const array, on the chance the two differed in some way that
    // mattered (not confirmed which, if either, was the actual fix - see SleepMsApprox() above,
    // a second real change applied in the same round).
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

    int dev9_modres = -1, netman_modres = -1, smap_modres = -1;
    int dev9_ret = SifExecModuleBuffer((void *)dev9_irx, size_dev9_irx, 0, NULL, &dev9_modres);
    int netman_ret = SifExecModuleBuffer((void *)netman_irx, size_netman_irx, 0, NULL, &netman_modres);
    int smap_ret = SifExecModuleBuffer((void *)smap_irx, size_smap_irx, 0, NULL, &smap_modres);
    rs2_log("net: dev9 ret=%d modres=%d netman ret=%d modres=%d smap ret=%d modres=%d\n", dev9_ret,
             dev9_modres, netman_ret, netman_modres, smap_ret, smap_modres);

    int netman_init_ret = NetManInit();
    rs2_log("net: NetManInit=%d\n", netman_init_ret);

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
        }
        if (link_state == NETMAN_NETIF_ETH_LINK_STATE_UP) {
            break;
        }
        SleepMsApprox();
    }
    rs2_log("net: final link_state=%d\n", link_state);

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
        }
        if (current_info.dhcp_status == DHCP_STATE_BOUND) {
            break;
        }
        SleepMsApprox();
    }
    rs2_log("net: dhcp_status=%d ip=0x%08x\n", current_info.dhcp_status,
             (unsigned int)current_info.ipaddr.s_addr);

    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, padDmaBuf);

    StartTimerSystemTime();

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsGlobal = gsKit_init_global();
    // This is the physical output framebuffer (640x480, aligned - see the comment above
    // platform_init()), separate from screenTexture's full 765x503 logical canvas - gsKit scales
    // between the two when drawing the sprite in platform_update_surface(). gsKit_init_global()'s
    // own default Width/Height (based on the console's detected video standard) also didn't match
    // what platform_update_surface() draws into, producing a black screen despite no draw errors,
    // so this still needs to be set explicitly either way.
    gsGlobal->Width = SCREEN_FB_WIDTH;
    gsGlobal->Height = SCREEN_FB_HEIGHT;
    gsGlobal->PSM = GS_PSM_CT24;
    // GS only has 4MB of VRAM - a double-buffered framebuffer plus the separate CT32 upload
    // texture platform_new() allocates below didn't fit ("ERROR: Not enough VRAM for this
    // allocation!") when both were sized to the (now-abandoned) full 765x503 canvas, and the
    // resulting failed gsKit_vram_alloc() (silently unchecked) aliased the texture onto the
    // framebuffer's own VRAM, producing a sheared/torn display. Tried disabling DoubleBuffering to
    // make room, but that produced a fully black screen instead (most likely gsKit_sync_flip()'s
    // presentation logic assumes double buffering - gsKit ships prebuilt with no source available
    // to confirm). Reverted to the standard double-buffered config and shrank the upload texture's
    // own format instead (see platform_new()'s GS_PSM_CT16 below) - now doubly unnecessary to
    // revisit since both buffers are also smaller (640x480 instead of 765x503).
    gsGlobal->DoubleBuffering = GS_SETTING_ON;
    gsGlobal->ZBuffering = GS_SETTING_OFF;
    // Once the VRAM allocation above actually succeeded (no more aliasing onto the framebuffer),
    // the screen went fully black instead of showing the sprite - in both single- and double-
    // buffered configs, so buffering mode wasn't the real cause after all. PS2 GS has a
    // well-documented quirk where a texture's alpha (including CT16's 1-bit alpha/mask field) needs
    // explicit TEXA register expansion to read as opaque - without it, alpha-blended primitives can
    // render fully transparent. This full-screen background blit never needed blending in the first
    // place (nothing is ever drawn behind it), so disabling it entirely sidesteps the ambiguity
    // rather than trying to get TEXA expansion exactly right blind.
    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

    gsKit_init_screen(gsGlobal);
    gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));

    return true;
}

void platform_new(GameShell *shell) {
    // TODO lowmem/audio bring-up (ps2snd/audsrv) - video/input/networking come first per the
    // project's phasing, matches how sdl2.c also skips audio init entirely under _Client.lowmem.
    (void)shell;
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
    screenTexture.Vram = gsKit_vram_alloc(gsGlobal, gsKit_texture_size(screenTexture.Width, screenTexture.Height, screenTexture.PSM), GSKIT_ALLOC_USERBUFFER);
    if (screenTexture.Vram == GSKIT_ALLOC_ERROR) {
        // Unchecked, this silently aliases the texture onto VRAM address 0 - typically the live
        // framebuffer itself - so every subsequent texture upload corrupts the display instead of
        // failing loudly. Fail loudly instead.
        rs2_error("platform_new: gsKit_vram_alloc failed for the screen texture (%dx%d) - out of GS VRAM\n", screenTexture.Width, screenTexture.Height);
    }
    memset(screenTexture.Mem, 0, gsKit_texture_size(screenTexture.Width, screenTexture.Height, screenTexture.PSM));
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

void platform_poll_events(Client *c) {
    int state = padGetState(0, 0);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
        return;
    }

    padRead(0, 0, &padData);

    // Left stick drives a virtual mouse cursor - TODO: no on-screen cursor sprite drawn yet,
    // and there's no keyboard equivalent for chat/typing input on this pass.
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
        c->shell->mouse_click_x = c->shell->mouse_x;
        c->shell->mouse_click_y = c->shell->mouse_y;
        c->shell->mouse_click_button = 1;
        c->shell->mouse_button = 1;
        if (_InputTracking.enabled) {
            inputtracking_mouse_pressed(&_InputTracking, c->shell->mouse_x, c->shell->mouse_y, 0);
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
