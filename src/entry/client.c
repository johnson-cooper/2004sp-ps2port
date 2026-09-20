#ifdef client
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_arch_dreamcast) || defined(__NDS__) || defined(__PS2__)
#include <malloc.h>
#endif

#include "../allocator.h"
#include "../animbase.h"
#include "../animframe.h"
#include "../client.h"
#include "../clientstream.h"
#include "../collisionmap.h"
#include "../component.h"
#include "../custom.h"
#include "../datastruct/jstring.h"
#include "../datastruct/linklist.h"
#include "../defines.h"
#include "../flotype.h"
#include "../gameshell.h"
#include "../idktype.h"
#include "../inputtracking.h"
#include "../jagfile.h"
#include "../locaddentity.h"
#include "../locentity.h"
#include "../locmergeentity.h"
#include "../loctype.h"
#include "../model.h"
#include "../npcentity.h"
#include "../npctype.h"
#include "../objstackentity.h"
#include "../objtype.h"
#include "../ondemand.h"
#include "../packet.h"
#include "../pix24.h"
#include "../pix3d.h"
#include "../pix8.h"
#include "../pixmap.h"
#include "../platform.h"
#include "../playerentity.h"
#include "../projectileentity.h"
#include "../protocol.h"
#include "../seqtype.h"
#include "../sound/wave.h"
#include "../spotanimentity.h"
#include "../spotanimtype.h"
#include "../thirdparty/bzip.h"
#include "../thirdparty/ini.h"
#include "../thirdparty/isaac.h"
#include "../varbittype.h"
#include "../varptype.h"
#include "../wordenc/wordfilter.h"
#include "../wordenc/wordpack.h"
#include "../world.h"
#include "../world3d.h"
#include "../gl11.h"

// 2026-09-14: MASTER OFF SWITCH for every PS2 on-screen diagnostic checkpoint in this file (both
// calls through ps2_scene_checkpoint() and the several raw-draw sites that bypass it entirely - see
// e.g. the "Loading map"/"last read"/"All map reads done"/"loc decode" blocks in
// client_load()/client_build_scene()). After repeated (4+) confirmed rounds this session of stacked
// checkpoint I/O (each a real GS-sync flip, some also unconditional rs2_log() USB/BDM file writes)
// itself causing a false freeze indistinguishable from a genuine hang - most recently bisected all
// the way down inside hashtable_get()/lrucache_get() to individual struct fields that all turned out
// to read back perfectly sane - the user asked to strip every checkpoint rather than keep chasing
// overhead. Defined here (top of file, before any use) rather than at ps2_scene_checkpoint()'s own
// definition so it actually covers the earlier raw-draw sites too, not just calls to that function.
// Flip to 1 for one targeted bisection session if a genuine new hang ever needs this technique again.
#define PS2_CHECKPOINTS_ENABLED 0

#ifdef __PS2__
static unsigned long ps2_live_update_count = 0;
static int ps2_live_stage = 0;
static int ps2_heap_tick_begin_kb = 0;
static int ps2_heap_after_packets_kb = 0;
static int ps2_heap_after_logic_kb = 0;
int ps2_heap_after_draw_kb = 0;
int ps2_heap_after_present_kb = 0;
#endif

extern int DESIGN_BODY_COLOR_LENGTH[];
extern int *DESIGN_BODY_COLOR[];
extern int DESIGN_HAIR_COLOR[];

extern Pix2D _Pix2D;
extern Pix3D _Pix3D;
extern InputTracking _InputTracking;
extern ModelData _Model;
extern ObjTypeData _ObjType;
extern ComponentData _Component;
extern IdkTypeData _IdkType;
extern VarpTypeData _VarpType;
extern VarBitTypeData _VarBitType;
extern SeqTypeData _SeqType;
extern LocTypeData _LocType;
extern SpotAnimTypeData _SpotAnimType;
extern NpcTypeData _NpcType;
extern PlayerEntityData _PlayerEntity;
extern WaveData _Wave;
extern WorldData _World;
extern SceneData _World3D;
extern Custom _Custom;

ClientData _Client = {
    .clientversion = 254,
    .members = true,
    .nodeid = 10,
    .socketip = "localhost",
#ifdef WITH_RSA_BIGINT
    // original rsa keys in dec, only used with js bigints: openssl with dec requires bigger result array and it doesn't work with rsa-tiny
    .rsa_exponent = "58778699976184461502525193738213253649000149147835990136706041084440742975821",
    .rsa_modulus = "7162900525229798032761816791230527296329313291232324290237849263501208207972894053929065636522363163621000728841182238772712427862772219676577293600221789",
#else
    // original rsa keys in hex
    .rsa_exponent = "81f390b2cf8ca7039ee507975951d5a0b15a87bf8b3f99c966834118c50fd94d", // pad exponent to an even number (prefix a 0 if needed)
    .rsa_modulus = "88c38748a58228f7261cdc340b5691d7d0975dee0ecdb717609e6bf971eb3fe723ef9d130e4686813739768ad9472eb46d8bfcc042c1a5fcb05e931f632eea5d",
#endif
};

const int CHAT_COLORS[6] = {YELLOW, RED, GREEN, CYAN, MAGENTA, WHITE};
const int LOC_SHAPE_TO_LAYER[23] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3};

// TODO add more static funcs here
static void client_draw_interface(Client *c, Component *com, int x, int y, int scrollY);
static void client_scenemap_free(Client *c);
static void client_build_scene(Client *c);
static void client_clear_caches(void);
static void client_update_orbit_camera(Client *c);
static int8_t *client_load_map_file(const char *kind, int mapsquareX, int mapsquareZ, int *out_size);
static void client_cache_file_path(const char *filename_only, char *out, size_t out_size);
#ifndef __PS2__
static int8_t *client_load_raw_file(const char *filename_only, int *out_size);
#endif
static inline bool component_valid(int id);
static inline Component *component_get(int id);
static void useMenuOption(Client *cl, int optionId);
static void handleControllerTabInput(Client *c);
static void handleControllerButtonInput(Client *c);
static void handleControllerGridInput(Client *c);
static void controller_settings_draw(Client *c);
static void virtual_keyboard_maybe_open(Client *c, int target);
static void virtual_keyboard_close(Client *c, bool submit);
static void virtual_keyboard_handle_input(Client *c);
static void virtual_keyboard_draw(Client *c);
static void virtual_cursor_draw(Client *c);

#ifdef __PS2__
// See platform_save_region()/platform_restore_region() in platform/ps2.c - used by
// virtual_cursor_draw() below to avoid leaving a permanent cursor trail on panels that don't
// redraw themselves every frame.
void platform_save_region(int x, int y, int w, int h, uint16_t *out);
void platform_restore_region(int x, int y, int w, int h, const uint16_t *in);

// See platform/ps2.c - returns "mass:/" once a USB mass-storage device carrying the game's own
// rom/cache/client/... tree is detected (real hardware / a USB-stick boot), or "" to keep using
// today's plain relative paths (which PCSX2's host: dev shortcut transparently redirects).
const char *ps2_cache_prefix(void);
static void ps2_draw_large_status(Client *c, const char *status);
static void ps2_runtime_checkpoint(Client *c, const char *status);
#endif

#ifdef __PS2__
// PHASE 4 audit instrumentation: net wait (clientstream.c) already ruled out as the dominant cost
// inside client_update_game() - update time is genuinely CPU-bound. Split its main phases so the
// next PS2 PERF report says which one, instead of guessing from code reading alone (that
// discipline already found two real bugs this session - the ondemand.zip residency issue and the
// player double-free - worth applying again here rather than reasoning blind about a huge function).
typedef struct {
    int64_t packets_ms;
    int64_t players_ms;
    int64_t npcs_ms;
    int64_t chats_ms;
    int64_t mergelocs_ms;
    int64_t getnpcpos_ms;
    int64_t getplayer_ms;
} TickPhaseAccum;
static TickPhaseAccum _TickPhase = {0};

int64_t client_tick_packets_ms(void) { return _TickPhase.packets_ms; }
int64_t client_tick_players_ms(void) { return _TickPhase.players_ms; }
int64_t client_tick_npcs_ms(void) { return _TickPhase.npcs_ms; }
int64_t client_tick_chats_ms(void) { return _TickPhase.chats_ms; }
int64_t client_tick_mergelocs_ms(void) { return _TickPhase.mergelocs_ms; }
int64_t client_tick_getnpcpos_ms(void) { return _TickPhase.getnpcpos_ms; }
int64_t client_tick_getplayer_ms(void) { return _TickPhase.getplayer_ms; }
void client_tick_phase_reset(void) { _TickPhase = (TickPhaseAccum){0}; }
#endif

void client_init_global(void) {
    int acc = 0;
    for (int i = 0; i < 99; i++) {
        int level = i + 1;
        int delta = (int)((double)level + pow(2.0, (double)level / 7.0) * 300.0);
        acc += delta;
        _Client.levelExperience[i] = acc / 4;
    }
}

void client_load(Client *c) {
    gl_load();

// TODO missing bits
// String vendor = System.getProperties().getProperty("java.vendor");
// if (vendor.toLowerCase().indexOf("sun") != -1 || vendor.toLowerCase().indexOf("apple") != -1) {
// 	signlink.sunjava = true;
// }
// if (signlink.sunjava) {
// 	super.mindel = 5;
// }

    if (!_Client.lowmem) {
        platform_set_midi("scape_main", 12345678, 40000);
    }

    if (_Client.started) {
        c->error_started = true;
        return;
    }

    _Client.started = true;

    // bool good = c->shell->window;
    // const char* host = this.getHost();
    // if (host.endsWith("2004scape.org")) {
    // 	// intended domain for players
    // 	good = true;
    // }
    // if (host.endsWith("localhost") || host.endsWith("127.0.0.1")) {
    // 	// allow localhost
    // 	good = true;
    // }
    // if (host.startsWith("192.168.") || host.startsWith("172.16.") || host.startsWith("10.")) {
    // 	// allow lan
    // 	good = true;
    // }
    // if (!good) {
    // 	this.errorHost = true;
    // 	return;
    // }

#ifdef __wasm
    int retry = 5;
#endif
    c->archive_checksum[8] = 0;
    while (c->archive_checksum[8] == 0) {
        client_draw_progress(c, "Connecting to fileserver", 10);
        char message[PATH_MAX];
        sprintf(message, "crc%d", (int)(jrand() * 9.9999999e7));
        int size = 0;
        int8_t *buffer = client_openurl(message, &size);
        if (!buffer) {
#ifdef __wasm
            for (int i = retry; i > 0; i--) {
                sprintf(message, "Error loading - Will retry in %d secs.", i);
                client_draw_progress(c, message, 10);
                rs2_sleep(1000);
            }
            retry *= 2;
            if (retry > 60) {
                retry = 60;
            }
#else
            // Native builds don't fetch this over HTTP (client_openurl is a stub) - read the same
            // 36-byte binary CRC table from the local cache instead, matching the file a bulk cache
            // download saves as rom/cache/client/crc. Falls back to a stale rev225 hardcoded table
            // (guaranteed to fail the server's CRC check) only if that file isn't present at all.
            char crc_filename[PATH_MAX];
#ifdef _arch_dreamcast
            snprintf(crc_filename, sizeof(crc_filename), "cache/client/crc.");
#elif defined(NXDK)
            snprintf(crc_filename, sizeof(crc_filename), "D:\\cache\\client\\crc");
#elif defined(__PS2__)
            snprintf(crc_filename, sizeof(crc_filename), "%srom/cache/client/crc", ps2_cache_prefix());
#else
            snprintf(crc_filename, sizeof(crc_filename), "rom/cache/client/crc");
#endif
            FILE *crc_file = fopen(crc_filename, "rb");
            bool crc_loaded = false;
            if (crc_file) {
                int8_t crc_buf[36];
                if (fread(crc_buf, 1, 36, crc_file) == 36) {
                    Packet *checksums = packet_new(crc_buf, 36);
                    for (int i = 0; i < 9; i++) {
                        c->archive_checksum[i] = g4(checksums);
                    }
                    checksums->data = NULL; // stack buffer, don't let packet_free() try to free it
                    packet_free(checksums);
                    crc_loaded = true;
                }
                fclose(crc_file);
            }
            if (!crc_loaded) {
                rs2_error("Failed to load local crc file, falling back to stale hardcoded checksums\n");
                c->archive_checksum[0] = 0;
                c->archive_checksum[1] = 784449929;
                c->archive_checksum[2] = -1494598746;
                c->archive_checksum[3] = 1614084464;
                c->archive_checksum[4] = 855958935;
                c->archive_checksum[5] = -2000991154;
                c->archive_checksum[6] = -313801935;
                c->archive_checksum[7] = 1570981179;
                c->archive_checksum[8] = -1532605973;
            }
#endif
        } else {
            Packet *checksums = packet_new(buffer, size); // 36
            for (int i = 0; i < 9; i++) {
                c->archive_checksum[i] = g4(checksums);
            }
        }
    }

    c->archive_title = load_archive(c, "title", c->archive_checksum[1], "title screen", 10);
    if (!c->archive_title) {
        c->error_loading = true;
        return;
    } else {
        platform_free_font();
    }
    c->font_plain11 = pixfont_from_archive(c->archive_title, "p11");
    c->font_plain12 = pixfont_from_archive(c->archive_title, "p12");
    c->font_bold12 = pixfont_from_archive(c->archive_title, "b12");
    c->font_quill8 = pixfont_from_archive(c->archive_title, "q8");
    client_load_title_background(c);
    client_load_title_images(c);

    Jagfile *config = load_archive(c, "config", c->archive_checksum[2], "config", 15);
    Jagfile *inter = load_archive(c, "interface", c->archive_checksum[3], "interface", 20);
    Jagfile *media = load_archive(c, "media", c->archive_checksum[4], "2d graphics", 30);
    Jagfile *models = load_archive(c, "models", c->archive_checksum[5], "3d graphics", 40);
    Jagfile *textures = load_archive(c, "textures", c->archive_checksum[6], "textures", 60);
#ifdef __PS2__
    // wordenc isn't touched until wordfilter_unpack() much later (after interfaces are unpacked) -
    // deferring its load until right before that call frees its resident memory for the entire
    // textures/models/animframes/interfaces phase, exactly where PS2's 32MB budget is tightest
    // (confirmed via mallinfo() - component_unpack() was the last blocker before this). Loaded (and
    // checked for failure) at its actual use site below instead.
    Jagfile *wordenc = NULL;
    // sounds is never touched at all when lowmem=1 (the only reader, wave_unpack, is behind
    // `if (!_Client.lowmem)` below) - on PS2's tight 32MB budget, loading and holding it resident
    // for the whole rest of client_load() anyway was pure waste. _Client.lowmem is already set from
    // config.ini by the time client_load() runs (main() sets it before boot), so it's safe to check
    // here at load time instead of only at free time.
    Jagfile *sounds = _Client.lowmem ? NULL : load_archive(c, "sounds", c->archive_checksum[8], "sound effects", 70);
    if (!config || !inter || !media || !models || !textures || (!_Client.lowmem && !sounds)) {
#else
    Jagfile *wordenc = load_archive(c, "wordenc", c->archive_checksum[7], "chat system", 65);
    Jagfile *sounds = load_archive(c, "sounds", c->archive_checksum[8], "sound effects", 70);
    if (!config || !inter || !media || !models || !textures || !wordenc || !sounds) {
#endif
        c->error_loading = true;
        return;
    }

    // rev254 delivers real model data through ondemand.zip instead of the "models" archive above
    // (which the server doesn't even serve for rev254 - "models" here is a stale placeholder kept
    // only so the NULL-check above still passes). Not fatal if missing: model_from_id() falls back
    // to the old models.jag-based path (producing wrong/missing geometry, not a crash) when
    // ondemand isn't loaded, matching how every other best-effort fallback in this file behaves.
#ifdef __PS2__
    // ondemand.zip is ~6MB - mz_zip_reader_init_mem's whole-file-resident approach (every other
    // platform, below) would alone consume ~19% of PS2's fixed 32MB EE RAM for the rest of the
    // session, permanently, on top of everything else client_load() holds resident. Open it
    // straight off disk instead (mz_zip_reader_init_file - real stdio fopen/fseek/fread under the
    // hood, the same file API client_load_raw_file() already uses for every other file on this
    // platform): only the zip's own central directory (entry names/offsets - tens of KB, not
    // entry payloads) stays resident. ondemand_get()/ondemand_get_by_index() are unchanged - miniz
    // reads whichever entry is actually requested straight from disk at the point it's needed,
    // which is already the same "lazy, on first real use" moment those callers request it at.
    char ondemand_zip_path[PATH_MAX];
    client_cache_file_path("ondemand.zip", ondemand_zip_path, sizeof(ondemand_zip_path));
    ondemand_load_file(ondemand_zip_path);
    rs2_log("MEM after streaming ondemand.zip (file-backed, not resident): used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
#else
    int ondemand_zip_size = 0;
    int8_t *ondemand_zip_data = client_load_raw_file("ondemand.zip", &ondemand_zip_size);
    if (ondemand_zip_data) {
        if (!ondemand_load(ondemand_zip_data, ondemand_zip_size)) {
            free(ondemand_zip_data);
        }
        // on success, ondemand_load() keeps ondemand_zip_data alive for the client's lifetime
        // (miniz's mz_zip_reader_init_mem does not copy the buffer) - do not free it here.
    }
#endif

    c->levelTileFlags = calloc(4, sizeof(*c->levelTileFlags));
    c->levelHeightmap = calloc(4, sizeof(*c->levelHeightmap));
    c->scene = world3d_new(c->levelHeightmap, 104, 4, 104);
    for (int level = 0; level < 4; level++) {
        c->levelCollisionMap[level] = collisionmap_new(104, 104);
    }
#if !defined(__PS2__) || !PS2_DISABLE_MINIMAP
    c->image_minimap = pix24_new(512, 512, false);
#endif
    client_draw_progress(c, "Unpacking media", 75);
    c->image_invback = pix8_from_archive(media, "invback", 0);
    c->image_chatback = pix8_from_archive(media, "chatback", 0);
    c->image_mapback = pix8_from_archive(media, "mapback", 0);
    c->image_backbase1 = pix8_from_archive(media, "backbase1", 0);
    c->image_backbase2 = pix8_from_archive(media, "backbase2", 0);
    c->image_backhmid1 = pix8_from_archive(media, "backhmid1", 0);
    for (int i = 0; i < 13; i++) {
        c->image_sideicons[i] = pix8_from_archive(media, "sideicons", i);
    }
#if !defined(__PS2__) || !PS2_DISABLE_MINIMAP
    c->image_compass = pix24_from_archive(media, "compass", 0);
#endif

#if !defined(__PS2__) || !PS2_DISABLE_MINIMAP
    for (int i = 0; i < 50; i++) {
        if (_Custom.hide_debug_sprite) {
            if (i == 22) {
                // weird debug sprite along water
                continue;
            }
        }

        c->image_mapscene[i] = pix8_from_archive(media, "mapscene", i);
        if (!c->image_mapscene[i]) {
            break;
        }
    }
    for (int i = 0; i < 50; i++) {
        c->image_mapfunction[i] = pix24_from_archive(media, "mapfunction", i);
        if (!c->image_mapfunction[i]) {
            break;
        }
    }
#endif
    for (int i = 0; i < 20; i++) {
        c->image_hitmarks[i] = pix24_from_archive(media, "hitmarks", i);
        if (!c->image_hitmarks[i]) {
            break;
        }
    }
    for (int i = 0; i < 20; i++) {
        c->image_headicons[i] = pix24_from_archive(media, "headicons", i);
        if (!c->image_headicons[i]) {
            break;
        }
    }
#if !defined(__PS2__) || !PS2_DISABLE_MINIMAP
    // rev254's media archive calls this sprite "mapmarker" (frame 0 = destination flag, frame 1 =
    // hint/quest arrow, used elsewhere) - "mapflag" doesn't exist in this revision's real archive.
    c->image_mapflag = pix24_from_archive(media, "mapmarker", 0);
#endif
    for (int i = 0; i < 8; i++) {
        c->image_crosses[i] = pix24_from_archive(media, "cross", i);
    }
#if !defined(__PS2__) || !PS2_DISABLE_MINIMAP
    c->image_mapdot0 = pix24_from_archive(media, "mapdots", 0);
    c->image_mapdot1 = pix24_from_archive(media, "mapdots", 1);
    c->image_mapdot2 = pix24_from_archive(media, "mapdots", 2);
    c->image_mapdot3 = pix24_from_archive(media, "mapdots", 3);
#endif
    c->image_scrollbar0 = pix8_from_archive(media, "scrollbar", 0);
    c->image_scrollbar1 = pix8_from_archive(media, "scrollbar", 1);
    c->image_redstone1 = pix8_from_archive(media, "redstone1", 0);
    c->image_redstone2 = pix8_from_archive(media, "redstone2", 0);
    c->image_redstone3 = pix8_from_archive(media, "redstone3", 0);
    c->image_redstone1h = pix8_from_archive(media, "redstone1", 0);
    pix8_flip_horizontally(c->image_redstone1h);
    c->image_redstone2h = pix8_from_archive(media, "redstone2", 0);
    pix8_flip_horizontally(c->image_redstone2h);
    c->image_redstone1v = pix8_from_archive(media, "redstone1", 0);
    pix8_flip_vertically(c->image_redstone1v);
    c->image_redstone2v = pix8_from_archive(media, "redstone2", 0);
    pix8_flip_vertically(c->image_redstone2v);
    c->image_redstone3v = pix8_from_archive(media, "redstone3", 0);
    pix8_flip_vertically(c->image_redstone3v);
    c->image_redstone1hv = pix8_from_archive(media, "redstone1", 0);
    pix8_flip_horizontally(c->image_redstone1hv);
    pix8_flip_vertically(c->image_redstone1hv);
    c->image_redstone2hv = pix8_from_archive(media, "redstone2", 0);
    pix8_flip_horizontally(c->image_redstone2hv);
    pix8_flip_vertically(c->image_redstone2hv);
    Pix24 *backleft1 = pix24_from_archive(media, "backleft1", 0);
    if (backleft1) {
        c->area_backleft1 = pixmap_new(backleft1->width, backleft1->height);
        pix24_blit_opaque(backleft1, 0, 0);
    }
    Pix24 *backleft2 = pix24_from_archive(media, "backleft2", 0);
    if (backleft2) {
        c->area_backleft2 = pixmap_new(backleft2->width, backleft2->height);
        pix24_blit_opaque(backleft2, 0, 0);
    }
    Pix24 *backright1 = pix24_from_archive(media, "backright1", 0);
    if (backright1) {
        c->area_backright1 = pixmap_new(backright1->width, backright1->height);
        pix24_blit_opaque(backright1, 0, 0);
    }
    Pix24 *backright2 = pix24_from_archive(media, "backright2", 0);
    if (backright2) {
        c->area_backright2 = pixmap_new(backright2->width, backright2->height);
        pix24_blit_opaque(backright2, 0, 0);
    }
    Pix24 *backtop1 = pix24_from_archive(media, "backtop1", 0);
    if (backtop1) {
        c->area_backtop1 = pixmap_new(backtop1->width, backtop1->height);
        pix24_blit_opaque(backtop1, 0, 0);
    }
    // rev254's game frame has a single full-width top panel (backtop1) - there is no "backtop2"
    // sprite in this revision's real media archive (confirmed against the reference client), so
    // attempting to load and draw one always failed and left a permanent black gap at (561, 0).
    Pix24 *backvmid1 = pix24_from_archive(media, "backvmid1", 0);
    if (backvmid1) {
        c->area_backvmid1 = pixmap_new(backvmid1->width, backvmid1->height);
        pix24_blit_opaque(backvmid1, 0, 0);
    }
    Pix24 *backvmid2 = pix24_from_archive(media, "backvmid2", 0);
    if (backvmid2) {
        c->area_backvmid2 = pixmap_new(backvmid2->width, backvmid2->height);
        pix24_blit_opaque(backvmid2, 0, 0);
    }
    Pix24 *backvmid3 = pix24_from_archive(media, "backvmid3", 0);
    if (backvmid3) {
        c->area_backvmid3 = pixmap_new(backvmid3->width, backvmid3->height);
        pix24_blit_opaque(backvmid3, 0, 0);
    }
    Pix24 *backhmid2 = pix24_from_archive(media, "backhmid2", 0);
    if (backhmid2) {
        c->area_backhmid2 = pixmap_new(backhmid2->width, backhmid2->height);
        pix24_blit_opaque(backhmid2, 0, 0);
    }

#if !defined(__PS2__) || !PS2_DISABLE_MINIMAP
    int rand_r = (int)(jrand() * 21.0) - 10;
    int rand_g = (int)(jrand() * 21.0) - 10;
    int rand_b = (int)(jrand() * 21.0) - 10;
    int _rand = (int)(jrand() * 41.0) - 20;
#endif
    for (int i = 0; i < 50; i++) {
#if !defined(__PS2__) || !PS2_DISABLE_MINIMAP
        if (c->image_mapfunction[i]) {
            pix24_translate(c->image_mapfunction[i], rand_r + _rand, rand_g + _rand, rand_b + _rand);
        }

        if (c->image_mapscene[i]) {
            pix8_translate(c->image_mapscene[i], rand_r + _rand, rand_g + _rand, rand_b + _rand);
        }
#endif
    }

#ifdef __PS2__
    rs2_log("MEM before textures: used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
#endif
    client_draw_progress(c, "Unpacking textures", 80);
    pix3d_unpack_textures(textures);
    pix3d_set_brightness(0.8);
    pix3d_init_pool(PIX3D_POOL_COUNT);
#ifdef __PS2__
    // textures/models are each only needed for their own unpack call below, unlike config/media/
    // wordenc which get re-read throughout client_load() - holding all 7 archives resident until
    // the batched jagfile_free() calls at the end of this function left no headroom for the
    // memory-hungry ondemand.zip animation-frame scan on PS2's fixed 32MB. Free textures/models as
    // soon as their last use is done instead of waiting.
    jagfile_free(textures);
    rs2_log("MEM before models: used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
#endif
    client_draw_progress(c, "Unpacking models", 83);
    model_unpack(models);
#ifdef __PS2__
    rs2_log("MEM before animframes: used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
#endif
    if (ondemand_is_loaded()) {
        // rev254 delivers anim frames (and their embedded base skeletons) via ondemand.zip -
        // see animframe_unpack_ondemand() for why mixing rev225 animation data with correctly
        // decoded rev254 model geometry produced visibly stretched/twisted limbs.
#ifdef __PS2__
        jagfile_free(models);
        rs2_log("MEM after freeing models: used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
#endif
        animframe_unpack_ondemand();
#ifdef __PS2__
        rs2_log("MEM after animframes: used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
#endif
    } else {
        animbase_unpack(models);
        animframe_unpack(models);
#ifdef __PS2__
        jagfile_free(models);
#endif
    }

    client_draw_progress(c, "Unpacking config", 86);
    seqtype_unpack(config);
    loctype_unpack(config);
    flotype_unpack(config);
    objtype_unpack(config);
    npctype_unpack(config);
    idktype_unpack(config);
    spotanimtype_unpack(config);
    varptype_unpack(config);
    varbittype_unpack(config);
#ifdef __PS2__
    // same reasoning as the earlier textures/models early-frees: config's real last use is the
    // xxx_unpack(config) run above, and sounds is never read at all when lowmem=1 (wave_unpack is
    // skipped below) - both were otherwise sitting resident, unused, right through the interface
    // phase until the batched jagfile_free() calls at the end of this function.
    jagfile_free(config);
#endif

    _ObjType.membersWorld = _Client.members;
    if (!_Client.lowmem) {
        client_draw_progress(c, "Unpacking sounds", 90);
        Packet *sound_dat = jagfile_to_packet(sounds, "sounds.dat");
        wave_unpack(sound_dat);
        packet_free(sound_dat);
    }
#ifdef __PS2__
    jagfile_free(sounds);
    rs2_log("MEM before interfaces: used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
#endif
    client_draw_progress(c, "Unpacking interfaces", 92);
    PixFont *fonts[] = {c->font_plain11, c->font_plain12, c->font_bold12, c->font_quill8};
    component_unpack(inter, media, fonts);
#ifdef __PS2__
    rs2_log("MEM after interfaces: used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
#endif

    client_draw_progress(c, "Preparing game engine", 97);
    // rev254's real "mapback" sprite is 172x156 - these scan bounds (and the offset subtracted per
    // row) assumed a differently-shaped rev225 image and read past the buffer / used the wrong hole
    // position, corrupting the circular minimap clip mask. Bounds confirmed against the reference
    // client: compass scan x<34 (was 35), minimap scan y in [5,156) x in [25,172) with offset -25
    // (was y in [9,160) x in [10,168) offset -21).
    for (int y = 0; y < 33; y++) {
        int left = 999;
        int right = 0;
        for (int x = 0; x < 34; x++) {
            if (c->image_mapback->pixels[x + y * c->image_mapback->width] == 0) {
                if (left == 999) {
                    left = x;
                }
            } else if (left != 999) {
                right = x;
                break;
            }
        }
        c->compass_mask_line_offsets[y] = left;
        c->compass_mask_line_lengths[y] = right - left;
    }

    for (int y = 5; y < 156; y++) {
        int left = 999;
        int right = 0;
        for (int x = 25; x < 172; x++) {
            if (c->image_mapback->pixels[x + y * c->image_mapback->width] == 0 && (x > 34 || y > 34)) {
                if (left == 999) {
                    left = x;
                }
            } else if (left != 999) {
                right = x;
                break;
            }
        }
        c->minimap_mask_line_offsets[y - 5] = left - 25;
        c->minimap_mask_line_lengths[y - 5] = right - left;
    }

    pix3d_init3d(479, 96);
    c->area_chatback_offsets = _Pix3D.line_offset;
    pix3d_init3d(190, 261);
    c->area_sidebar_offsets = _Pix3D.line_offset;
    pix3d_init3d(512, 334);
    c->area_viewport_offsets = _Pix3D.line_offset;
#ifdef __PS2__
    pix3d_init3d(PS2_3D_RENDER_WIDTH, PS2_3D_RENDER_HEIGHT);
    c->area_viewport_3d_offsets = _Pix3D.line_offset;
#endif

    int *distance = malloc(9 * sizeof(int));
    for (int x = 0; x < 9; x++) {
        int angle = x * 32 + 128 + 15;
        int offset = angle * 3 + 600;
        int sin = _Pix3D.sin_table[angle];
        distance[x] = offset * sin >> 16;
    }

#ifdef __PS2__
    world3d_init(PS2_3D_RENDER_WIDTH, PS2_3D_RENDER_HEIGHT, 500, 800, distance);
#else
    world3d_init(512, 334, 500, 800, distance);
#endif
    free(distance);
#ifdef __PS2__
    // 65 was its original progress-bar percentage back when it loaded upfront alongside the other
    // archives - now that it loads here instead (well past "Unpacking interfaces" at 92), use a
    // later value so the progress bar doesn't visibly jump backward.
    wordenc = load_archive(c, "wordenc", c->archive_checksum[7], "chat system", 93);
    if (!wordenc) {
        c->error_loading = true;
        return;
    }
#endif
    wordfilter_unpack(wordenc);

    pix24_free(backleft1);
    pix24_free(backleft2);
    pix24_free(backright1);
    pix24_free(backright2);
    pix24_free(backtop1);
    pix24_free(backvmid1);
    pix24_free(backvmid2);
    pix24_free(backvmid3);
    pix24_free(backhmid2);

#ifndef __PS2__
    // already freed earlier on PS2, right after their last use - see the memory-pressure notes above
    jagfile_free(config);
#endif
    jagfile_free(inter);
#ifndef __PS2__
    jagfile_free(media);
    jagfile_free(models);
    jagfile_free(textures);
#endif
    // media is kept alive on PS2 (see _Component.media in component.c/.h) - component_unpack()
    // deferred decoding most interface graphics, and they're only decoded from media on first
    // real use, potentially long after this point
    jagfile_free(wordenc);
#ifndef __PS2__
    jagfile_free(sounds);
#endif
    // } catch (Exception ex) {
    // 	ex.printStackTrace();
    // 	this.errorLoading = true;
    // }

// NOTE: we can't grow it so it needs to fit the max usage, left value is shifted to MiB (arbitrary value)
#if defined(_arch_dreamcast) || defined(__NDS__)
    malloc_stats();
    if (!bump_allocator_init(8 << 20)) {
#elif defined(__PS2__)
    // 2026-09-13 update: ondemand.zip (~6MB) used to be read whole into a permanently-resident heap
    // buffer (mz_zip_reader_init_mem) - switched to mz_zip_reader_init_file (see ondemand_load_file()
    // call site above) so only its small central-directory index stays resident. Confirmed via
    // mallinfo() this genuinely reclaims ~5.9MB: `used` at this exact checkpoint dropped from
    // ~26.8MB to ~20.9MB, real data from a real boot, not projected. That directly changes the risk
    // calculus for this arena's size - the corrupting "# Restart." failure mode described below was
    // observed at a *total footprint* (baseline used + arena capacity) of ~28.8MB (26.8MB + 2MB);
    // the largest *clean* (non-corrupting) attempt was ~28.4MB (26.8MB + 1.57MB). That ~300-400KB
    // band is the closest real data point to wherever the actual physical ceiling is - nothing here
    // pins it down more precisely than that, and this is still a step taken without being able to
    // run the emulator directly in this pass, so it is deliberately conservative rather than pushing
    // all the way to that observed edge.
    //
    // 2026-09-13, round 2: gave the 3MB size above a real boot. Result ruled out "just needs a
    // slightly bigger arena": alloc_count went from 32,455 (at 1,703,936 cap) to 62,209 (at
    // 3,145,728 cap) - almost exactly doubling in lockstep with the arena itself (avg ~51 bytes/
    // alloc BOTH times). A bump allocator has no idea how much capacity remains until it fails, so
    // it always fails on whatever the next allocation happens to be - the fact both overflow amounts
    // (172 bytes, then 288 bytes) were tiny relative to capacity is a property of THIS allocator's
    // failure mode, not evidence the scene was nearly finished either time. The real signal is that
    // consumption scales linearly with whatever capacity you give it, with no sign yet of leveling
    // off - i.e. this region's real total requirement is unknown and could be substantially larger
    // than anything tried so far. That's not necessarily a bug: this exact bump arena is 16-32MB on
    // every non-lowmem platform for the same purpose (temporary per-scene model/loc geometry), and
    // this map region (real evidence: 60K+ small allocations and counting) may simply be a busy one.
    //
    // Given that, the only real lever left without deeper surgery (compacting model.c's int32
    // vertex/face arrays to int16, or reducing simultaneously-loaded region count - see the PHASE 2
    // audit notes, not attempted this round) is to spend more of the headroom the ondemand.zip fix
    // freed, and see where it actually lands. Safety ceiling estimate unchanged from above: total
    // footprint (used + capacity) corrupted at ~28.8MB and stayed clean at ~28.4MB, both under the
    // OLD ~26.8MB baseline. At the NEW ~20.9MB baseline, 6MB keeps total footprint at ~26.9MB - a
    // full 1.5MB under the clean data point, not just under the corruption one, while giving 2x the
    // arena of the last (still-insufficient) attempt. bump_allocator_reset()'s new peak-usage log
    // (allocator.c) will show whether this scene actually completes now, or keeps scaling - if it's
    // still not enough, the next move should be the model-compaction/region-count option above, not
    // another blind capacity jump toward the ceiling.
    //
    // History of prior attempts, kept for context: 1MB+128KB (1,179,648) overflowed by 4 bytes, then
    // 1MB+192KB (1,245,184) overflowed by 12, then 1MB+512KB (1,572,864) overflowed by 40, then 3MB
    // (3,145,728) overflowed by 288 - all clean "Allocator full" errors, all but the last under the
    // OLD baseline. Tried reasoning our way to 2MB once under the OLD baseline (math suggested it
    // should fit with room to spare) and got the corrupting "# Restart." / "DMAC(5) Handler does not
    // exist." failure mode instead - twice.
    rs2_log("MEM before bump allocator: used=%d free=%d\n", mallinfo().uordblks, mallinfo().fordblks);
    // Six MiB is the established working headroom for the later model build.
    // The new PS2 area streamer avoids spending it on distant location models.
    if (!bump_allocator_init(6 << 20)) {
#else
    if (!(_Client.lowmem ? bump_allocator_init(16 << 20) : bump_allocator_init(32 << 20))) {
#endif
        c->error_loading = true;
    }

    // network init happens here after game loads instead of in platform_init because being connected disables fast-forward in emulators
    if (!clientstream_init()) {
        c->error_loading = true;
    }

// TODO temp: wait for wiiu and switch touch input fixes, melonds 32mb emulation
// PS2 has no keyboard/text-input path at all yet (platform_poll_events only drives a pad-based
// virtual mouse) - auto-login is the only way to reach the game past the title screen for now.
#if defined(__WIIU__) || defined(__SWITCH__) || defined(__NDS__) || defined(__PS2__)
    client_login(c, c->username, c->password, false);
#endif

#if defined(_arch_dreamcast) || defined(__NDS__)
    // it's fine for the consoles memory to be full here, it frees the login screen after this
    malloc_stats();
#endif
}

void client_load_title_background(Client *c) {
    int length;
    uint8_t *data = jagfile_to_bytes(c->archive_title, "title.dat", &length);
    Pix24 *title = pix24_from_jpeg(data, length);
    free(data);

    pixmap_bind(c->image_title0);
    pix24_blit_opaque(title, 0, 0);

    pixmap_bind(c->image_title1);
    pix24_blit_opaque(title, -637, 0);

    pixmap_bind(c->image_title2);
    pix24_blit_opaque(title, -128, 0);

    pixmap_bind(c->image_title3);
    pix24_blit_opaque(title, -202, -371);

    pixmap_bind(c->image_title4);
    pix24_blit_opaque(title, -202, -171);

    pixmap_bind(c->image_title5);
    pix24_blit_opaque(title, 0, -265);

    pixmap_bind(c->image_title6);
    pix24_blit_opaque(title, -562, -265);

    pixmap_bind(c->image_title7);
    pix24_blit_opaque(title, -128, -171);

    pixmap_bind(c->image_title8);
    pix24_blit_opaque(title, -562, -171);

    int *mirror = malloc(title->width * sizeof(int));
    for (int y = 0; y < title->height; y++) {
        for (int x = 0; x < title->width; x++) {
            mirror[x] = title->pixels[title->width + title->width * y - x - 1];
        }

        if (title->width >= 0) {
            memcpy(&title->pixels[title->width * y], mirror, title->width * sizeof(int));
        }
    }
    free(mirror);

    pixmap_bind(c->image_title0);
    pix24_blit_opaque(title, 382, 0);

    pixmap_bind(c->image_title1);
    pix24_blit_opaque(title, -255, 0);

    pixmap_bind(c->image_title2);
    pix24_blit_opaque(title, 254, 0);

    pixmap_bind(c->image_title3);
    pix24_blit_opaque(title, 180, -371);

    pixmap_bind(c->image_title4);
    pix24_blit_opaque(title, 180, -171);

    pixmap_bind(c->image_title5);
    pix24_blit_opaque(title, 382, -265);

    pixmap_bind(c->image_title6);
    pix24_blit_opaque(title, -180, -265);

    pixmap_bind(c->image_title7);
    pix24_blit_opaque(title, 254, -171);

    pixmap_bind(c->image_title8);
    pix24_blit_opaque(title, -180, -171);

    pix24_free(title);
    title = pix24_from_archive(c->archive_title, "logo", 0);
    pixmap_bind(c->image_title2);
    pix24_draw(title, c->shell->screen_width / 2 - title->width / 2 - 128, 18);

    pix24_free(title);
    title = NULL;
}

void client_update_flame_buffer(Client *c, Pix8 *image) {
    int flame_height = 256;
    memset(c->flame_buffer0, 0, FLAME_BUFFER_SIZE * sizeof(int));

    for (int i = 0; i < 5000; i++) {
        int index = (int)(jrand() * 128.0 * (double)flame_height);
        c->flame_buffer0[index] = (int)(jrand() * 256.0);
    }

    for (int i = 0; i < 20; i++) {
        for (int y = 1; y < flame_height - 1; y++) {
            for (int x = 1; x < 127; x++) {
                int index = x + (y << 7);
                c->flame_buffer1[index] = (c->flame_buffer0[index - 1] + c->flame_buffer0[index + 1] + c->flame_buffer0[index - 128] + c->flame_buffer0[index + 128]) / 4;
            }
        }

        int *last = c->flame_buffer0;
        c->flame_buffer0 = c->flame_buffer1;
        c->flame_buffer1 = last;
    }

    if (image) {
        int off = 0;

        for (int y = 0; y < image->height; y++) {
            for (int x = 0; x < image->width; x++) {
                if (image->pixels[off++] != 0) {
                    int x0 = x + image->crop_x + 16;
                    int y0 = y + image->crop_y + 16;
                    int index = x0 + (y0 << 7);
                    c->flame_buffer0[index] = 0;
                }
            }
        }
    }
}

void client_load_title_images(Client *c) {
    c->image_titlebox = pix8_from_archive(c->archive_title, "titlebox", 0);
    c->image_titlebutton = pix8_from_archive(c->archive_title, "titlebutton", 0);
#ifdef DISABLE_FLAMES
    // TODO: redraw behind "flames" when there's any spare memory, gets freed after login
    // c->image_flames_left = pix24_new(128, 265, false);
    // c->image_flames_right = pix24_new(128, 265, false);
    // memcpy(c->image_flames_left->pixels, c->image_title0->pixels, 33920 * sizeof(int));
    // memcpy(c->image_flames_right->pixels, c->image_title1->pixels, 33920 * sizeof(int));
#else
    c->image_runes = calloc(12, sizeof(Pix8 *));
    for (int i = 0; i < 12; i++) {
        c->image_runes[i] = pix8_from_archive(c->archive_title, "runes", i);
    }
    c->image_flames_left = pix24_new(128, 265, false);
    c->image_flames_right = pix24_new(128, 265, false);
    memcpy(c->image_flames_left->pixels, c->image_title0->pixels, 33920 * sizeof(int));
    memcpy(c->image_flames_right->pixels, c->image_title1->pixels, 33920 * sizeof(int));
    c->flame_gradient0 = calloc(256, sizeof(int));
    for (int i = 0; i < 64; i++) {
        c->flame_gradient0[i] = i * 262144;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient0[i + 64] = i * 1024 + RED;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient0[i + 128] = i * 4 + YELLOW;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient0[i + 192] = WHITE;
    }
    c->flame_gradient1 = calloc(256, sizeof(int));
    for (int i = 0; i < 64; i++) {
        c->flame_gradient1[i] = i * 1024;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient1[i + 64] = i * 4 + GREEN;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient1[i + 128] = i * 262144 + CYAN;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient1[i + 192] = WHITE;
    }
    c->flame_gradient2 = calloc(256, sizeof(int));
    for (int i = 0; i < 64; i++) {
        c->flame_gradient2[i] = i * 4;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient2[i + 64] = i * 262144 + BLUE;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient2[i + 128] = i * 1024 + MAGENTA;
    }
    for (int i = 0; i < 64; i++) {
        c->flame_gradient2[i + 192] = WHITE;
    }
    c->flame_gradient = calloc(256, sizeof(int));
    c->flame_buffer0 = calloc(FLAME_BUFFER_SIZE, sizeof(int));
    c->flame_buffer1 = calloc(FLAME_BUFFER_SIZE, sizeof(int));
    client_update_flame_buffer(c, NULL);
    c->flame_buffer3 = calloc(FLAME_BUFFER_SIZE, sizeof(int));
    c->flame_buffer2 = calloc(FLAME_BUFFER_SIZE, sizeof(int));
    client_draw_progress(c, "Connecting to fileserver", 10);
    if (!c->flame_active) {
        c->flame_active = true;
    }
#endif
}

static void client_update_flames(Client *c) {
    int height = 256;
    for (int x = 10; x < 117; x++) {
        int _rand = (int)(jrand() * 100.0);
        if (_rand < 50) {
            c->flame_buffer3[x + ((height - 2) << 7)] = 255;
        }
    }

    for (int l = 0; l < 100; l++) {
        int x = (int)(jrand() * 124.0) + 2;
        int y = (int)(jrand() * 128.0) + 128;
        int index = x + (y << 7);
        c->flame_buffer3[index] = 192;
    }

    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < 127; x++) {
            int index = x + (y << 7);
            c->flame_buffer2[index] = (c->flame_buffer3[index - 1] + c->flame_buffer3[index + 1] + c->flame_buffer3[index - 128] + c->flame_buffer3[index + 128]) / 4;
        }
    }

    c->flame_cycle0 += 128;
    if (c->flame_cycle0 > FLAME_BUFFER_SIZE) {
        c->flame_cycle0 -= FLAME_BUFFER_SIZE;
        int _rand = (int)(jrand() * 12.0);
        client_update_flame_buffer(c, c->image_runes[_rand]);
    }

    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < 127; x++) {
            int index = x + (y << 7);
            int intensity = c->flame_buffer2[index + 128] - c->flame_buffer0[index + c->flame_cycle0 & FLAME_BUFFER_SIZE - 1] / 5;
            if (intensity < 0) {
                intensity = 0;
            }
            c->flame_buffer3[index] = intensity;
        }
    }

    for (int y = 0; y < height - 1; y++) {
        c->flame_line_offset[y] = c->flame_line_offset[y + 1];
    }

    c->flame_line_offset[height - 1] = (int)(sin((double)_Client.loop_cycle / 14.0) * 16.0 + sin((double)_Client.loop_cycle / 15.0) * 14.0 + sin((double)_Client.loop_cycle / 16.0) * 12.0);

    if (c->flameGradientCycle0 > 0) {
        c->flameGradientCycle0 -= 4;
    }

    if (c->flameGradientCycle1 > 0) {
        c->flameGradientCycle1 -= 4;
    }

    if (c->flameGradientCycle0 == 0 && c->flameGradientCycle1 == 0) {
        int _rand = (int)(jrand() * 2000.0);

        if (_rand == 0) {
            c->flameGradientCycle0 = 1024;
        } else if (_rand == 1) {
            c->flameGradientCycle1 = 1024;
        }
    }
}

static int mix(int src, int alpha, int dst) {
    int invAlpha = 256 - alpha;
    return (((src & 0xff00ff) * invAlpha + (dst & 0xff00ff) * alpha & 0xff00ff00) + ((src & 0xff00) * invAlpha + (dst & 0xff00) * alpha & 0xff0000)) >> 8;
}

static void client_draw_flames(Client *c) {
    int height = 256;

    if (c->flameGradientCycle0 > 0) {
        for (int i = 0; i < 256; i++) {
            if (c->flameGradientCycle0 > 768) {
                c->flame_gradient[i] = mix(c->flame_gradient0[i], 1024 - c->flameGradientCycle0, c->flame_gradient1[i]);
            } else if (c->flameGradientCycle0 > 256) {
                c->flame_gradient[i] = c->flame_gradient1[i];
            } else {
                c->flame_gradient[i] = mix(c->flame_gradient1[i], 256 - c->flameGradientCycle0, c->flame_gradient0[i]);
            }
        }
    } else if (c->flameGradientCycle1 > 0) {
        for (int i = 0; i < 256; i++) {
            if (c->flameGradientCycle1 > 768) {
                c->flame_gradient[i] = mix(c->flame_gradient0[i], 1024 - c->flameGradientCycle1, c->flame_gradient2[i]);
            } else if (c->flameGradientCycle1 > 256) {
                c->flame_gradient[i] = c->flame_gradient2[i];
            } else {
                c->flame_gradient[i] = mix(c->flame_gradient2[i], 256 - c->flameGradientCycle1, c->flame_gradient0[i]);
            }
        }
    } else {
        memcpy(c->flame_gradient, c->flame_gradient0, 256 * sizeof(int));
    }
    memcpy(c->image_title0->pixels, c->image_flames_left->pixels, 33920 * sizeof(int));

    int srcOffset = 0;
    int dstOffset = 1152;

    for (int y = 1; y < height - 1; y++) {
        int offset = c->flame_line_offset[y] * (height - y) / height;
        int step = offset + 22;
        if (step < 0) {
            step = 0;
        }
        srcOffset += step;
        for (int x = step; x < 128; x++) {
            int value = c->flame_buffer3[srcOffset++];
            if (value == 0) {
                dstOffset++;
            } else {
                int alpha = value;
                int invAlpha = 256 - value;
                value = c->flame_gradient[value];
                int background = c->image_title0->pixels[dstOffset];
                c->image_title0->pixels[dstOffset++] = (((value & 0xff00ff) * alpha + (background & 0xff00ff) * invAlpha & 0xff00ff00) + ((value & 0xff00) * alpha + (background & 0xff00) * invAlpha & 0xff0000)) >> 8;
            }
        }
        dstOffset += step;
    }

    pixmap_draw(c->image_title0, 0, 0);

    memcpy(c->image_title1->pixels, c->image_flames_right->pixels, 33920 * sizeof(int));

    srcOffset = 0;
    dstOffset = 1176;
    for (int y = 1; y < height - 1; y++) {
        int offset = c->flame_line_offset[y] * (height - y) / height;
        int step = 103 - offset;
        dstOffset += offset;
        for (int x = 0; x < step; x++) {
            int value = c->flame_buffer3[srcOffset++];
            if (value == 0) {
                dstOffset++;
            } else {
                int alpha = value;
                int invAlpha = 256 - value;
                value = c->flame_gradient[value];
                int background = c->image_title1->pixels[dstOffset];
                c->image_title1->pixels[dstOffset++] = (((value & 0xff00ff) * alpha + (background & 0xff00ff) * invAlpha & 0xff00ff00) + ((value & 0xff00) * alpha + (background & 0xff00) * invAlpha & 0xff0000)) >> 8;
            }
        }
        srcOffset += 128 - step;
        dstOffset += 128 - step - offset;
    }

    pixmap_draw(c->image_title1, 637, 0);
}

void client_run_flames(Client *c) {
    static uint64_t next = 0;
    if (!c->flame_active || next >= rs2_now()) {
        return;
    }
    client_update_flames(c);
    client_update_flames(c);
    client_draw_flames(c);
    next = rs2_now() + 35; // hardcode interval of 35 to avoid inconsistent rate

    /* NOTE: original
    // try {
    uint64_t last = rs2_now();
    int cycle = 0;
    int interval = 20;
    while (c->flame_active) {
        client_update_flames(c);
        client_update_flames(c);
        client_draw_flames(c);

        cycle++;

        if (cycle > 10) {
            uint64_t now = rs2_now();
            int delay = (int)(now - last) / 10 - interval;

            interval = 40 - delay;
            if (interval < 5) {
                interval = 5;
            }

            cycle = 0;
            last = now;
        }

        // try {
        rs2_sleep(interval);
        // } catch (@Pc(52) Exception ignored) {
        // }
    }
    // } catch (@Pc(58) Exception ignored) {
    // }
    */
}

void client_update(Client *c) {
    if (c->error_started || c->error_loading || c->error_host) {
        return;
    }

    _Client.loop_cycle++;
    // Runs unconditionally (not just from client_update_game()'s per-tick input block) since the
    // virtual keyboard also has to work on the login screen, which goes through
    // client_update_title() instead and never reaches that block.
    virtual_keyboard_handle_input(c);
    if (c->ingame) {
        client_update_game(c);
    } else {
        client_update_title(c);
    }
}

void handleChatMouseInput(Client *c, int mouseX, int mouseY) {
    (void)mouseX;
#ifdef __PS2__
    const int chatLineHeight = 15;
    const int chatBaseY = 74;
#else
    const int chatLineHeight = 14;
    const int chatBaseY = 70;
#endif
    int line = 0;
    for (int i = 0; i < 100; i++) {
        if (c->message_text[i][0] == '\0') {
            continue;
        }

        int type = c->message_type[i];
        int y = c->chat_scroll_offset + chatBaseY + 4 - line * chatLineHeight;
        if (y < -20) {
            break;
        }

        if (type == 0) {
            line++;
        }

        if ((type == 1 || type == 2) && (type == 1 || c->public_chat_setting == 0 || (c->public_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
            if (mouseY > y - chatLineHeight && mouseY <= y && strcmp(c->message_sender[i], c->local_player->name) != 0) {
                if (c->rights) {
                    sprintf(c->menu_option[c->menu_size], "Report abuse @whi@%s", c->message_sender[i]);
                    c->menu_action[c->menu_size] = 34;
                    c->menu_size++;
                }

                sprintf(c->menu_option[c->menu_size], "Add ignore @whi@%s", c->message_sender[i]);
                c->menu_action[c->menu_size] = 436;
                c->menu_size++;
                sprintf(c->menu_option[c->menu_size], "Add friend @whi@%s", c->message_sender[i]);
                c->menu_action[c->menu_size] = 406;
                c->menu_size++;
            }

            line++;
        }

        if ((type == 3 || type == 7) && c->split_private_chat == 0 && (type == 7 || c->private_chat_setting == 0 || (c->private_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
            if (mouseY > y - chatLineHeight && mouseY <= y) {
                if (c->rights) {
                    sprintf(c->menu_option[c->menu_size], "Report abuse @whi@%s", c->message_sender[i]);
                    c->menu_action[c->menu_size] = 34;
                    c->menu_size++;
                }

                sprintf(c->menu_option[c->menu_size], "Add ignore @whi@%s", c->message_sender[i]);
                c->menu_action[c->menu_size] = 436;
                c->menu_size++;
                sprintf(c->menu_option[c->menu_size], "Add friend @whi@%s", c->message_sender[i]);
                c->menu_action[c->menu_size] = 406;
                c->menu_size++;
            }

            line++;
        }

        if (type == 4 && (c->trade_chat_setting == 0 || (c->trade_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
            if (mouseY > y - chatLineHeight && mouseY <= y) {
                sprintf(c->menu_option[c->menu_size], "Accept trade @whi@%s", c->message_sender[i]);
                c->menu_action[c->menu_size] = 903;
                c->menu_size++;
            }

            line++;
        }

        if ((type == 5 || type == 6) && c->split_private_chat == 0 && c->private_chat_setting < 2) {
            line++;
        }

        if (type == 8 && (c->trade_chat_setting == 0 || (c->trade_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
            if (mouseY > y - chatLineHeight && mouseY <= y) {
                sprintf(c->menu_option[c->menu_size], "Accept duel @whi@%s", c->message_sender[i]);
                c->menu_action[c->menu_size] = 363;
                c->menu_size++;
            }

            line++;
        }
    }
}

void handleInterfaceInput(Client *c, Component *com, int mouseX, int mouseY, int x, int y, int scrollPosition) {
    if (!com) {
        return;
    }
    if (com->type != 0 || !com->childId || com->hide || (mouseX < x || mouseY < y || mouseX > x + com->width || mouseY > y + com->height)) {
        return;
    }

    int children = com->childCount;
    for (int i = 0; i < children; i++) {
        int childX = com->childX[i] + x;
        int childY = com->childY[i] + y - scrollPosition;
        Component *child = component_get(com->childId[i]);

        childX += child->x;
        childY += child->y;

        if ((child->overLayer >= 0 || child->overColour != 0) && mouseX >= childX && mouseY >= childY && mouseX < childX + child->width && mouseY < childY + child->height) {
            if (child->overLayer >= 0) {
                c->lastHoveredInterfaceId = child->overLayer;
            } else {
                c->lastHoveredInterfaceId = child->id;
            }
        }

        if (child->type == 0) {
            handleInterfaceInput(c, child, mouseX, mouseY, childX, childY, child->scrollPosition);

            if (child->scroll > child->height) {
                handleScrollInput(c, mouseX, mouseY, child->scroll, child->height, true, childX + child->width, childY, child);
            }
        } else if (child->type == 2) {
            int slot = 0;

            for (int row = 0; row < child->height; row++) {
                for (int col = 0; col < child->width; col++) {
                    int slotX = childX + col * (child->marginX + 32);
                    int slotY = childY + row * (child->marginY + 32);

                    if (slot < 20) {
                        slotX += child->invSlotOffsetX[slot];
                        slotY += child->invSlotOffsetY[slot];
                    }

                    if (mouseX < slotX || mouseY < slotY || mouseX >= slotX + 32 || mouseY >= slotY + 32) {
                        slot++;
                        continue;
                    }

                    c->hoveredSlot = slot;
                    c->hoveredSlotParentId = child->id;

                    if (child->invSlotObjId[slot] <= 0) {
                        slot++;
                        continue;
                    }

                    ObjType *obj = objtype_get(child->invSlotObjId[slot] - 1);

                    if (c->obj_selected == 1 && child->interactable) {
                        if (child->id != c->objSelectedInterface || slot != c->objSelectedSlot) {
                            sprintf(c->menu_option[c->menu_size], "Use %s with @lre@%s", c->objSelectedName, obj->name);
                            c->menu_action[c->menu_size] = 881;
                            c->menuParamA[c->menu_size] = obj->index;
                            c->menuParamB[c->menu_size] = slot;
                            c->menuParamC[c->menu_size] = child->id;
                            c->menu_size++;
                        }
                    } else if (c->spell_selected == 1 && child->interactable) {
                        if ((c->activeSpellFlags & 0x10) == 16) {
                            sprintf(c->menu_option[c->menu_size], "%s @lre@%s", c->spellCaption, obj->name);
                            c->menu_action[c->menu_size] = 391;
                            c->menuParamA[c->menu_size] = obj->index;
                            c->menuParamB[c->menu_size] = slot;
                            c->menuParamC[c->menu_size] = child->id;
                            c->menu_size++;
                        }
                    } else {
                        if (child->interactable) {
                            for (int op = 4; op >= 3; op--) {
                                if (obj->iop && obj->iop[op]) {
                                    sprintf(c->menu_option[c->menu_size], "%s @lre@%s", obj->iop[op], obj->name);
                                    if (op == 3) {
                                        c->menu_action[c->menu_size] = 478;
                                    } else if (op == 4) {
                                        c->menu_action[c->menu_size] = 347;
                                    }
                                    c->menuParamA[c->menu_size] = obj->index;
                                    c->menuParamB[c->menu_size] = slot;
                                    c->menuParamC[c->menu_size] = child->id;
                                    c->menu_size++;
                                } else if (op == 4) {
                                    sprintf(c->menu_option[c->menu_size], "Drop @lre@%s", obj->name);
                                    c->menu_action[c->menu_size] = 347;
                                    c->menuParamA[c->menu_size] = obj->index;
                                    c->menuParamB[c->menu_size] = slot;
                                    c->menuParamC[c->menu_size] = child->id;
                                    c->menu_size++;
                                }
                            }
                        }

                        if (child->usable) {
                            sprintf(c->menu_option[c->menu_size], "Use @lre@%s", obj->name);
                            c->menu_action[c->menu_size] = 188;
                            c->menuParamA[c->menu_size] = obj->index;
                            c->menuParamB[c->menu_size] = slot;
                            c->menuParamC[c->menu_size] = child->id;
                            c->menu_size++;
                        }

                        if (child->interactable && obj->iop) {
                            for (int op = 2; op >= 0; op--) {
                                if (obj->iop[op]) {
                                    sprintf(c->menu_option[c->menu_size], "%s @lre@%s", obj->iop[op], obj->name);
                                    if (op == 0) {
                                        c->menu_action[c->menu_size] = 405;
                                    } else if (op == 1) {
                                        c->menu_action[c->menu_size] = 38;
                                    } else if (op == 2) {
                                        c->menu_action[c->menu_size] = 422;
                                    }
                                    c->menuParamA[c->menu_size] = obj->index;
                                    c->menuParamB[c->menu_size] = slot;
                                    c->menuParamC[c->menu_size] = child->id;
                                    c->menu_size++;
                                }
                            }
                        }

                        if (child->iops) {
                            for (int op = 4; op >= 0; op--) {
                                if (child->iops[op]) {
                                    sprintf(c->menu_option[c->menu_size], "%s @lre@%s", child->iops[op], obj->name);
                                    if (op == 0) {
                                        c->menu_action[c->menu_size] = 602;
                                    } else if (op == 1) {
                                        c->menu_action[c->menu_size] = 596;
                                    } else if (op == 2) {
                                        c->menu_action[c->menu_size] = 22;
                                    } else if (op == 3) {
                                        c->menu_action[c->menu_size] = 892;
                                    } else if (op == 4) {
                                        c->menu_action[c->menu_size] = 415;
                                    }
                                    c->menuParamA[c->menu_size] = obj->index;
                                    c->menuParamB[c->menu_size] = slot;
                                    c->menuParamC[c->menu_size] = child->id;
                                    c->menu_size++;
                                }
                            }
                        }

                        sprintf(c->menu_option[c->menu_size], "Examine @lre@%s", obj->name);
                        // TODO
                        // if (c->show_debug) {
                        // 	c->menu_option[c->menu_size] += "@whi@ (" + (obj.index) + ")";
                        // }
                        c->menu_action[c->menu_size] = 1773;
                        c->menuParamA[c->menu_size] = obj->index;
                        c->menuParamC[c->menu_size] = child->invSlotObjCount[slot];
                        c->menu_size++;
                    }

                    slot++;
                }
            }
        } else if (mouseX >= childX && mouseY >= childY && mouseX < childX + child->width && mouseY < childY + child->height) {
            if (child->buttonType == BUTTON_OK) {
                bool override = false;
                if (child->clientCode != 0) {
                    override = handleSocialMenuOption(c, child);
                }

                if (!override) {
                    strcpy(c->menu_option[c->menu_size], child->option);
                    c->menu_action[c->menu_size] = 951;
                    c->menuParamC[c->menu_size] = child->id;
                    c->menu_size++;
                }
            } else if (child->buttonType == BUTTON_TARGET && c->spell_selected == 0) {
                char *prefix = child->actionVerb;
                bool _free = false;
                if (indexof_chr(prefix, ' ') != -1) {
                    prefix = substring(prefix, 0, indexof_chr(prefix, ' '));
                    _free = true;
                }

                sprintf(c->menu_option[c->menu_size], "%s @gre@%s", prefix, child->action);
                if (_free) {
                    free(prefix);
                }
                c->menu_action[c->menu_size] = 930;
                c->menuParamC[c->menu_size] = child->id;
                c->menu_size++;
            } else if (child->buttonType == BUTTON_CLOSE) {
                strcpy(c->menu_option[c->menu_size], "Close");
                c->menu_action[c->menu_size] = 947;
                c->menuParamC[c->menu_size] = child->id;
                c->menu_size++;
            } else if (child->buttonType == BUTTON_TOGGLE) {
                strcpy(c->menu_option[c->menu_size], child->option);
                c->menu_action[c->menu_size] = 465;
                c->menuParamC[c->menu_size] = child->id;
                c->menu_size++;
            } else if (child->buttonType == BUTTON_SELECT) {
                strcpy(c->menu_option[c->menu_size], child->option);
                c->menu_action[c->menu_size] = 960;
                c->menuParamC[c->menu_size] = child->id;
                c->menu_size++;
            } else if (child->buttonType == BUTTON_CONTINUE && !c->pressed_continue_option) {
                strcpy(c->menu_option[c->menu_size], child->option);
                c->menu_action[c->menu_size] = 44;
                c->menuParamC[c->menu_size] = child->id;
                c->menu_size++;
            }
        }
    }
}

bool handleSocialMenuOption(Client *c, Component *component) {
    int type = component->clientCode;
    if (type >= 1 && type <= 200) {
        if (type >= 101) {
            type -= 101;
        } else {
            type--;
        }

        sprintf(c->menu_option[c->menu_size], "Remove @whi@%s", c->friendName[type]);
        c->menu_action[c->menu_size] = 557;
        c->menu_size++;

        sprintf(c->menu_option[c->menu_size], "Message @whi@%s", c->friendName[type]);
        c->menu_action[c->menu_size] = 679;
        c->menu_size++;
        return true;
    } else if (type >= 401 && type <= 500) {
        sprintf(c->menu_option[c->menu_size], "Remove @whi@%s", component->text);
        c->menu_action[c->menu_size] = 556;
        c->menu_size++;
        return true;
    } else {
        return false;
    }
}

void handlePrivateChatInput(Client *c, int mouse_x, int mouse_y) {
    if (c->split_private_chat == 0) {
        return;
    }

    int lineOffset = 0;
    if (c->system_update_timer != 0) {
        lineOffset = 1;
    }

    for (int i = 0; i < 100; i++) {
        if (c->message_text[i][0]) {
            int type = c->message_type[i];
            if ((type == 3 || type == 7) && (type == 7 || c->private_chat_setting == 0 || (c->private_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
                int y = 329 - lineOffset * 13;
                // super.mouseX was used here for no reason when they are both passed to func
                if (mouse_x > 4 && mouse_x < 516 && mouse_y - 4 > y - 10 && mouse_y - 4 <= y + 3) {
                    if (c->rights) {
                        sprintf(c->menu_option[c->menu_size], "Report abuse @whi@%s", c->message_sender[i]);
                        c->menu_action[c->menu_size] = 2034;
                        c->menu_size++;
                    }
                    sprintf(c->menu_option[c->menu_size], "Add ignore @whi@%s", c->message_sender[i]);
                    c->menu_action[c->menu_size] = 2436;
                    c->menu_size++;
                    sprintf(c->menu_option[c->menu_size], "Add friend @whi@%s", c->message_sender[i]);
                    c->menu_action[c->menu_size] = 2406;
                    c->menu_size++;
                }

                lineOffset++;
                if (lineOffset >= 5) {
                    return;
                }
            }

            if ((type == 5 || type == 6) && c->private_chat_setting < 2) {
                lineOffset++;
                if (lineOffset >= 5) {
                    return;
                }
            }
        }
    }
}

static const char *getCombatLevelColorTag(int viewerLevel, int otherLevel) {
    int diff = viewerLevel - otherLevel;
    if (diff < -9) {
        return "@red@";
    } else if (diff < -6) {
        return "@or3@";
    } else if (diff < -3) {
        return "@or2@";
    } else if (diff < 0) {
        return "@or1@";
    } else if (diff > 9) {
        return "@gre@";
    } else if (diff > 6) {
        return "@gr3@";
    } else if (diff > 3) {
        return "@gr2@";
    } else if (diff > 0) {
        return "@gr1@";
    } else {
        return "@yel@";
    }
}

void addNpcOptions(Client *cl, NpcType *npc, int a, int b, int c) {
    if (cl->menu_size >= 400) {
        return;
    }

    // npc->name is only set if the npc's config data actually included a name entry (npctype.c code
    // 2) - a type id beyond the loaded npc.idx range (falls back to npctype_new()'s defaults) or a
    // real type that simply has no name both leave it NULL, and this used to strcpy() it unguarded -
    // a real crash confirmed when clicking such an NPC.
    const char *npc_name = npc->name ? npc->name : "Unknown";

    char tooltip[MAX_STR];
    if (npc->vislevel != 0) {
        char tmp[HALF_STR];
        strcpy(tmp, npc_name);
        sprintf(tooltip, "%s%s (level-%d)", tmp, getCombatLevelColorTag(cl->local_player->combatLevel, npc->vislevel), npc->vislevel);
    } else {
        strcpy(tooltip, npc_name);
    }

    if (cl->obj_selected == 1) {
        sprintf(cl->menu_option[cl->menu_size], "Use %s with @yel@%s", cl->objSelectedName, tooltip);
        cl->menu_action[cl->menu_size] = 900;
        cl->menuParamA[cl->menu_size] = a;
        cl->menuParamB[cl->menu_size] = b;
        cl->menuParamC[cl->menu_size] = c;
        cl->menu_size++;
    } else if (cl->spell_selected != 1) {
        int type;
        if (npc->op) {
            for (type = 4; type >= 0; type--) {
                if (npc->op[type] && platform_strcasecmp(npc->op[type], "attack") != 0) {
                    sprintf(cl->menu_option[cl->menu_size], "%s @yel@%s", npc->op[type], tooltip);

                    if (type == 0) {
                        cl->menu_action[cl->menu_size] = 728;
                    } else if (type == 1) {
                        cl->menu_action[cl->menu_size] = 542;
                    } else if (type == 2) {
                        cl->menu_action[cl->menu_size] = 6;
                    } else if (type == 3) {
                        cl->menu_action[cl->menu_size] = 963;
                    } else if (type == 4) {
                        cl->menu_action[cl->menu_size] = 245;
                    }

                    cl->menuParamA[cl->menu_size] = a;
                    cl->menuParamB[cl->menu_size] = b;
                    cl->menuParamC[cl->menu_size] = c;
                    cl->menu_size++;
                }
            }
        }

        if (npc->op) {
            for (type = 4; type >= 0; type--) {
                if (npc->op[type] && platform_strcasecmp(npc->op[type], "attack") == 0) {
                    int action = 0;
                    if (npc->vislevel > cl->local_player->combatLevel) {
                        action = 2000;
                    }

                    sprintf(cl->menu_option[cl->menu_size], "%s @yel@%s", npc->op[type], tooltip);

                    if (type == 0) {
                        cl->menu_action[cl->menu_size] = action + 728;
                    } else if (type == 1) {
                        cl->menu_action[cl->menu_size] = action + 542;
                    } else if (type == 2) {
                        cl->menu_action[cl->menu_size] = action + 6;
                    } else if (type == 3) {
                        cl->menu_action[cl->menu_size] = action + 963;
                    } else if (type == 4) {
                        cl->menu_action[cl->menu_size] = action + 245;
                    }

                    cl->menuParamA[cl->menu_size] = a;
                    cl->menuParamB[cl->menu_size] = b;
                    cl->menuParamC[cl->menu_size] = c;
                    cl->menu_size++;
                }
            }
        }

        sprintf(cl->menu_option[cl->menu_size], "Examine @yel@%s", tooltip);
        // TODO:
        // if (cl->show_debug) {
        // 	cl->menu_option[cl->menu_size] += "@whi@ (" + (npc.index) + ")";
        // }
        cl->menu_action[cl->menu_size] = 1607;
        cl->menuParamA[cl->menu_size] = a;
        cl->menuParamB[cl->menu_size] = b;
        cl->menuParamC[cl->menu_size] = c;
        cl->menu_size++;
    } else if ((cl->activeSpellFlags & 0x2) == 2) {
        sprintf(cl->menu_option[cl->menu_size], "%s @yel@%s", cl->spellCaption, tooltip);
        cl->menu_action[cl->menu_size] = 265;
        cl->menuParamA[cl->menu_size] = a;
        cl->menuParamB[cl->menu_size] = b;
        cl->menuParamC[cl->menu_size] = c;
        cl->menu_size++;
    }
}

void addPlayerOptions(Client *cl, PlayerEntity *player, int a, int b, int c) {
    if (player == cl->local_player || cl->menu_size >= 400) {
        return;
    }

    char tooltip[MAX_STR];
    sprintf(tooltip, "%s%s (level-%d)", player->name, getCombatLevelColorTag(cl->local_player->combatLevel, player->combatLevel), player->combatLevel);
    if (cl->obj_selected == 1) {
        sprintf(cl->menu_option[cl->menu_size], "Use %s with @whi@%s", cl->objSelectedName, tooltip);
        cl->menu_action[cl->menu_size] = 367;
        cl->menuParamA[cl->menu_size] = a;
        cl->menuParamB[cl->menu_size] = b;
        cl->menuParamC[cl->menu_size] = c;
        cl->menu_size++;
    } else if (cl->spell_selected != 1) {
        sprintf(cl->menu_option[cl->menu_size], "Follow @whi@%s", tooltip);
        cl->menu_action[cl->menu_size] = 1544;
        cl->menuParamA[cl->menu_size] = a;
        cl->menuParamB[cl->menu_size] = b;
        cl->menuParamC[cl->menu_size] = c;
        cl->menu_size++;

        if (cl->overrideChat == 0) {
            sprintf(cl->menu_option[cl->menu_size], "Trade with @whi@%s", tooltip);
            cl->menu_action[cl->menu_size] = 1373;
            cl->menuParamA[cl->menu_size] = a;
            cl->menuParamB[cl->menu_size] = b;
            cl->menuParamC[cl->menu_size] = c;
            cl->menu_size++;
        }

        if (cl->wildernessLevel > 0) {
            sprintf(cl->menu_option[cl->menu_size], "Attack @whi@%s", tooltip);
            if (cl->local_player->combatLevel >= player->combatLevel) {
                cl->menu_action[cl->menu_size] = 151;
            } else {
                cl->menu_action[cl->menu_size] = 2151;
            }
            cl->menuParamA[cl->menu_size] = a;
            cl->menuParamB[cl->menu_size] = b;
            cl->menuParamC[cl->menu_size] = c;
            cl->menu_size++;
        }

        if (cl->worldLocationState == 1) {
            sprintf(cl->menu_option[cl->menu_size], "Fight @whi@%s", tooltip);
            cl->menu_action[cl->menu_size] = 151;
            cl->menuParamA[cl->menu_size] = a;
            cl->menuParamB[cl->menu_size] = b;
            cl->menuParamC[cl->menu_size] = c;
            cl->menu_size++;
        }

        if (cl->worldLocationState == 2) {
            sprintf(cl->menu_option[cl->menu_size], "Duel-with @whi@%s", tooltip);
            cl->menu_action[cl->menu_size] = 1101;
            cl->menuParamA[cl->menu_size] = a;
            cl->menuParamB[cl->menu_size] = b;
            cl->menuParamC[cl->menu_size] = c;
            cl->menu_size++;
        }
    } else if ((cl->activeSpellFlags & 0x8) == 8) {
        sprintf(cl->menu_option[cl->menu_size], "%s @whi@%s", cl->spellCaption, tooltip);
        cl->menu_action[cl->menu_size] = 651;
        cl->menuParamA[cl->menu_size] = a;
        cl->menuParamB[cl->menu_size] = b;
        cl->menuParamC[cl->menu_size] = c;
        cl->menu_size++;
    }

    for (int i = 0; i < cl->menu_size; i++) {
        if (cl->menu_action[i] == 660) {
            sprintf(cl->menu_option[i], "Walk here @whi@%s", tooltip);
            return;
        }
    }
}

void handleViewportOptions(Client *c) {
    if (c->obj_selected == 0 && c->spell_selected == 0) {
        strcpy(c->menu_option[c->menu_size], "Walk here");
        c->menu_action[c->menu_size] = 660;
        c->menuParamB[c->menu_size] = c->shell->mouse_x;
        c->menuParamC[c->menu_size] = c->shell->mouse_y;
        c->menu_size++;
    }

    int lastBitset = -1;
    for (int picked = 0; picked < _Model.picked_count; picked++) {
        int bitset = _Model.picked_bitsets[picked];
        int x = bitset & 0x7f;
        int z = bitset >> 7 & 0x7f;
        int entityType = bitset >> 29 & 0x3;
        int typeId = bitset >> 14 & 0x7fff;

        if (bitset == lastBitset) {
            continue;
        }

        lastBitset = bitset;

        if (entityType == 2 && world3d_get_info(c->scene, c->currentLevel, x, z, bitset) >= 0) {
            LocType *loc = loctype_get(typeId);
            if (c->obj_selected == 1) {
                sprintf(c->menu_option[c->menu_size], "Use %s with @cya@%s", c->objSelectedName, loc->name);
                c->menu_action[c->menu_size] = 450;
                c->menuParamA[c->menu_size] = bitset;
                c->menuParamB[c->menu_size] = x;
                c->menuParamC[c->menu_size] = z;
                c->menu_size++;
            } else if (c->spell_selected != 1) {
                if (loc->op) {
                    for (int op = 4; op >= 0; op--) {
                        if (loc->op[op]) {
                            sprintf(c->menu_option[c->menu_size], "%s @cya@%s", loc->op[op], loc->name);
                            if (op == 0) {
                                c->menu_action[c->menu_size] = 285;
                            }

                            if (op == 1) {
                                c->menu_action[c->menu_size] = 504;
                            }

                            if (op == 2) {
                                c->menu_action[c->menu_size] = 364;
                            }

                            if (op == 3) {
                                c->menu_action[c->menu_size] = 581;
                            }

                            if (op == 4) {
                                c->menu_action[c->menu_size] = 1501;
                            }

                            c->menuParamA[c->menu_size] = bitset;
                            c->menuParamB[c->menu_size] = x;
                            c->menuParamC[c->menu_size] = z;
                            c->menu_size++;
                        }
                    }
                }

                sprintf(c->menu_option[c->menu_size], "Examine @cya@%s", loc->name);
                // TODO
                // if (c->show_debug) {
                // 	c->menu_option[c->menu_size] += "@whi@ (" + (loc.index) + ")";
                // }
                c->menu_action[c->menu_size] = 1175;
                c->menuParamA[c->menu_size] = bitset;
                c->menuParamB[c->menu_size] = x;
                c->menuParamC[c->menu_size] = z;
                c->menu_size++;
            } else if ((c->activeSpellFlags & 0x4) == 4) {
                sprintf(c->menu_option[c->menu_size], "%s @cya@%s", c->spellCaption, loc->name);
                c->menu_action[c->menu_size] = 55;
                c->menuParamA[c->menu_size] = bitset;
                c->menuParamB[c->menu_size] = x;
                c->menuParamC[c->menu_size] = z;
                c->menu_size++;
            }
        }

        if (entityType == 1) {
            // c->npcs[typeId] is a 3D-picked entity index (from the model click/hover picking
            // system) - the picked npc can have already been removed/desynced by the time this
            // runs, and this was dereferenced with no NULL check at all. Confirmed real crash:
            // clicking an NPC whose slot had gone stale segfaulted here immediately.
            NpcEntity *npc = c->npcs[typeId];
            // npc->type is set immediately when an NpcEntity is created (getNpcPosNewVis) and only
            // ever cleared to NULL right before the entity itself is freed (getNpcPos's removal
            // loop) - but defend against it anyway since npc itself being non-NULL doesn't
            // guarantee type is populated for every code path that can reach this pick.
            if (npc && npc->type) {
                if (npc->type->size == 1 && (npc->pathing_entity.x & 0x7f) == 64 && (npc->pathing_entity.z & 0x7f) == 64) {
                    for (int i = 0; i < c->npc_count; i++) {
                        NpcEntity *other = c->npcs[c->npc_ids[i]];

                        if (other && other != npc && other->type && other->type->size == 1 && other->pathing_entity.x == npc->pathing_entity.x && other->pathing_entity.z == npc->pathing_entity.z) {
                            addNpcOptions(c, other->type, c->npc_ids[i], x, z);
                        }
                    }
                }

                addNpcOptions(c, npc->type, typeId, x, z);
            }
        }

        if (entityType == 0) {
            // see the entityType == 1 branch above for the same fix, same rationale.
            PlayerEntity *player = c->players[typeId];
            if (player && (player->pathing_entity.x & 0x7f) == 64 && (player->pathing_entity.z & 0x7f) == 64) {
                for (int i = 0; i < c->npc_count; i++) {
                    NpcEntity *other = c->npcs[c->npc_ids[i]];

                    if (other && other->type->size == 1 && other->pathing_entity.x == player->pathing_entity.x && other->pathing_entity.z == player->pathing_entity.z) {
                        addNpcOptions(c, other->type, c->npc_ids[i], x, z);
                    }
                }

                for (int i = 0; i < c->player_count; i++) {
                    PlayerEntity *other = c->players[c->player_ids[i]];

                    if (other && other != player && other->pathing_entity.x == player->pathing_entity.x && other->pathing_entity.z == player->pathing_entity.z) {
                        addPlayerOptions(c, other, c->player_ids[i], x, z);
                    }
                }
            }

            if (player) {
                addPlayerOptions(c, player, typeId, x, z);
            }
        }

        if (entityType == 3) {
            LinkList *objs = c->level_obj_stacks[c->currentLevel][x][z];
            if (!objs) {
                continue;
            }

            for (ObjStackEntity *obj = (ObjStackEntity *)linklist_tail(objs); obj; obj = (ObjStackEntity *)linklist_prev(objs)) {
                ObjType *type = objtype_get(obj->index);
                if (c->obj_selected == 1) {
                    sprintf(c->menu_option[c->menu_size], "Use %s with @lre@%s", c->objSelectedName, type->name);
                    c->menu_action[c->menu_size] = 217;
                    c->menuParamA[c->menu_size] = obj->index;
                    c->menuParamB[c->menu_size] = x;
                    c->menuParamC[c->menu_size] = z;
                    c->menu_size++;
                } else if (c->spell_selected != 1) {
                    for (int op = 4; op >= 0; op--) {
                        if (type->op && type->op[op]) {
                            sprintf(c->menu_option[c->menu_size], "%s @lre@%s", type->op[op], type->name);
                            if (op == 0) {
                                c->menu_action[c->menu_size] = 224;
                            }

                            if (op == 1) {
                                c->menu_action[c->menu_size] = 993;
                            }

                            if (op == 2) {
                                c->menu_action[c->menu_size] = 99;
                            }

                            if (op == 3) {
                                c->menu_action[c->menu_size] = 746;
                            }

                            if (op == 4) {
                                c->menu_action[c->menu_size] = 877;
                            }

                            c->menuParamA[c->menu_size] = obj->index;
                            c->menuParamB[c->menu_size] = x;
                            c->menuParamC[c->menu_size] = z;
                            c->menu_size++;
                        } else if (op == 2) {
                            sprintf(c->menu_option[c->menu_size], "Take @lre@%s", type->name);
                            c->menu_action[c->menu_size] = 99;
                            c->menuParamA[c->menu_size] = obj->index;
                            c->menuParamB[c->menu_size] = x;
                            c->menuParamC[c->menu_size] = z;
                            c->menu_size++;
                        }
                    }

                    sprintf(c->menu_option[c->menu_size], "Examine @lre@%s", type->name);
                    // TODO
                    // if (c->show_debug) {
                    // 	c->menu_option[c->menu_size] += "@whi@ (" + (obj.index) + ")";
                    // }
                    c->menu_action[c->menu_size] = 1102;
                    c->menuParamA[c->menu_size] = obj->index;
                    c->menuParamB[c->menu_size] = x;
                    c->menuParamC[c->menu_size] = z;
                    c->menu_size++;
                } else if ((c->activeSpellFlags & 0x1) == 1) {
                    sprintf(c->menu_option[c->menu_size], "%s @lre@%s", c->spellCaption, type->name);
                    c->menu_action[c->menu_size] = 965;
                    c->menuParamA[c->menu_size] = obj->index;
                    c->menuParamB[c->menu_size] = x;
                    c->menuParamC[c->menu_size] = z;
                    c->menu_size++;
                }
            }
        }
    }
}

void client_handle_input(Client *c) {
    if (c->obj_drag_area != 0) {
        return;
    }

    strcpy(c->menu_option[0], "Cancel");
    c->menu_action[0] = 1252;
    c->menu_size = 1;
    handlePrivateChatInput(c, c->shell->mouse_x, c->shell->mouse_y);
    c->lastHoveredInterfaceId = 0;

    if (c->shell->mouse_x > 4 && c->shell->mouse_y > 4 && c->shell->mouse_x < 516 && c->shell->mouse_y < 338) {
        if (c->viewport_interface_id == -1) {
            handleViewportOptions(c);
        } else {
            handleInterfaceInput(c, component_get(c->viewport_interface_id), c->shell->mouse_x, c->shell->mouse_y, 4, 4, 0);
        }
    }

    if (c->lastHoveredInterfaceId != c->viewportHoveredInterfaceIndex) {
        c->viewportHoveredInterfaceIndex = c->lastHoveredInterfaceId;
    }

    c->lastHoveredInterfaceId = 0;

    if (c->shell->mouse_x > 553 && c->shell->mouse_y > 205 && c->shell->mouse_x < 743 && c->shell->mouse_y < 466) {
        if (c->sidebar_interface_id != -1) {
            handleInterfaceInput(c, component_get(c->sidebar_interface_id), c->shell->mouse_x, c->shell->mouse_y, 553, 205, 0);
        } else if (c->tab_interface_id[c->selected_tab] != -1) {
            handleInterfaceInput(c, component_get(c->tab_interface_id[c->selected_tab]), c->shell->mouse_x, c->shell->mouse_y, 553, 205, 0);
        }
    }

    if (c->lastHoveredInterfaceId != c->sidebarHoveredInterfaceIndex) {
        c->redraw_sidebar = true;
        c->sidebarHoveredInterfaceIndex = c->lastHoveredInterfaceId;
    }

    c->lastHoveredInterfaceId = 0;

    if (c->shell->mouse_x > 17 && c->shell->mouse_y > 357 && c->shell->mouse_x < 426 && c->shell->mouse_y < 453) {
        if (c->chat_interface_id == -1) {
            handleChatMouseInput(c, c->shell->mouse_x - 17, c->shell->mouse_y - 357);
        } else {
            handleInterfaceInput(c, component_get(c->chat_interface_id), c->shell->mouse_x, c->shell->mouse_y, 17, 357, 0);
        }
    }

    if (c->chat_interface_id != -1 && c->lastHoveredInterfaceId != c->chatHoveredInterfaceIndex) {
        c->redraw_chatback = true;
        c->chatHoveredInterfaceIndex = c->lastHoveredInterfaceId;
    }

    bool done = false;
    while (!done) {
        done = true;

        for (int i = 0; i < c->menu_size - 1; i++) {
            if (c->menu_action[i] < 1000 && c->menu_action[i + 1] > 1000) {
                char tmp0[MAX_STR];
                strcpy(tmp0, c->menu_option[i]);
                strcpy(c->menu_option[i], c->menu_option[i + 1]);
                strcpy(c->menu_option[i + 1], tmp0);

                int tmp1 = c->menu_action[i];
                c->menu_action[i] = c->menu_action[i + 1];
                c->menu_action[i + 1] = tmp1;

                int tmp2 = c->menuParamB[i];
                c->menuParamB[i] = c->menuParamB[i + 1];
                c->menuParamB[i + 1] = tmp2;

                int tmp3 = c->menuParamC[i];
                c->menuParamC[i] = c->menuParamC[i + 1];
                c->menuParamC[i + 1] = tmp3;

                int tmp4 = c->menuParamA[i];
                c->menuParamA[i] = c->menuParamA[i + 1];
                c->menuParamA[i + 1] = tmp4;

                done = false;
            }
        }
    }
}

bool handleInterfaceAction(Client *c, Component *com) {
    int clientCode = com->clientCode;
    if (clientCode == 201) {
        c->redraw_chatback = true;
        c->chatback_input_open = false;
        c->show_social_input = true;
        c->social_input[0] = '\0';
        c->social_action = 1;
        strcpy(c->social_message, "Enter name of friend to add to list");
        virtual_keyboard_maybe_open(c, 3);
    }

    if (clientCode == 202) {
        c->redraw_chatback = true;
        c->chatback_input_open = false;
        c->show_social_input = true;
        c->social_input[0] = '\0';
        c->social_action = 2;
        strcpy(c->social_message, "Enter name of friend to delete from list");
        virtual_keyboard_maybe_open(c, 3);
    }

    if (clientCode == 205) {
        c->idle_timeout = 250;
        return true;
    }

    if (clientCode == 501) {
        c->redraw_chatback = true;
        c->chatback_input_open = false;
        c->show_social_input = true;
        c->social_input[0] = '\0';
        c->social_action = 4;
        strcpy(c->social_message, "Enter name of player to add to list");
        virtual_keyboard_maybe_open(c, 3);
    }

    if (clientCode == 502) {
        c->redraw_chatback = true;
        c->chatback_input_open = false;
        c->show_social_input = true;
        c->social_input[0] = '\0';
        c->social_action = 5;
        strcpy(c->social_message, "Enter name of player to delete from list");
        virtual_keyboard_maybe_open(c, 3);
    }

    if (clientCode >= 300 && clientCode <= 313) {
        int part = (clientCode - 300) / 2;
        int direction = clientCode & 0x1;
        int kit = c->designIdentikits[part];

        if (kit != -1) {
            while (true) {
                if (direction == 0) {
                    kit--;
                    if (kit < 0) {
                        kit = _IdkType.count - 1;
                    }
                }

                if (direction == 1) {
                    kit++;
                    if (kit >= _IdkType.count) {
                        kit = 0;
                    }
                }

                if (!_IdkType.instances[kit]->disable && _IdkType.instances[kit]->type == part + (c->design_gender_male ? 0 : 7)) {
                    c->designIdentikits[part] = kit;
                    c->update_design_model = true;
                    break;
                }
            }
        }
    }

    if (clientCode >= 314 && clientCode <= 323) {
        int part = (clientCode - 314) / 2;
        int direction = clientCode & 0x1;
        int color = c->design_colors[part];

        if (direction == 0) {
            color--;
            if (color < 0) {
                color = DESIGN_BODY_COLOR_LENGTH[part] - 1;
            }
        }

        if (direction == 1) {
            color++;
            if (color >= DESIGN_BODY_COLOR_LENGTH[part]) {
                color = 0;
            }
        }

        c->design_colors[part] = color;
        c->update_design_model = true;
    }

    if (clientCode == 324 && !c->design_gender_male) {
        c->design_gender_male = true;
        client_validate_character_design(c);
    }

    if (clientCode == 325 && c->design_gender_male) {
        c->design_gender_male = false;
        client_validate_character_design(c);
    }

    if (clientCode == 326) {
        // IF_PLAYERDESIGN
        p1isaac(c->out, 13); // IDK_SAVEDESIGN
        p1(c->out, c->design_gender_male ? 0 : 1);
        for (int i = 0; i < 7; i++) {
            p1(c->out, c->designIdentikits[i]);
        }
        for (int i = 0; i < 5; i++) {
            p1(c->out, c->design_colors[i]);
        }
        return true;
    }

    if (clientCode == 613) {
        c->reportAbuseMuteOption = !c->reportAbuseMuteOption;
    }

    if (clientCode >= 601 && clientCode <= 612) {
        closeInterfaces(c);

        if (strlen(c->reportAbuseInput) > 0) {
            // BUG_REPORT
            p1isaac(c->out, 203); // REPORT_ABUSE
            p8(c->out, jstring_to_base37(c->reportAbuseInput));
            p1(c->out, clientCode - 601);
            p1(c->out, c->reportAbuseMuteOption ? 1 : 0);
        }
    }

    return false;
}

void handleScrollInput(Client *c, int mouseX, int mouseY, int scrollableHeight, int height, bool redraw, int left, int top, Component *component) {
    if (c->scrollGrabbed) {
        c->scrollInputPadding = 32;
    } else {
        c->scrollInputPadding = 0;
    }

    c->scrollGrabbed = false;

    if (mouseX >= left && mouseX < left + 16 && mouseY >= top && mouseY < top + 16) {
        component->scrollPosition -= c->drag_cycles * 4;
        if (redraw) {
            c->redraw_sidebar = true;
        }
    } else if (mouseX >= left && mouseX < left + 16 && mouseY >= top + height - 16 && mouseY < top + height) {
        component->scrollPosition += c->drag_cycles * 4;
        if (redraw) {
            c->redraw_sidebar = true;
        }
    } else if (mouseX >= left - c->scrollInputPadding && mouseX < left + c->scrollInputPadding + 16 && mouseY >= top + 16 && mouseY < top + height - 16 && c->drag_cycles > 0) {
        int gripSize = (height - 32) * height / scrollableHeight;
        if (gripSize < 8) {
            gripSize = 8;
        }
        int gripY = mouseY - top - gripSize / 2 - 16;
        int maxY = height - gripSize - 32;
        component->scrollPosition = (scrollableHeight - height) * gripY / maxY;
        if (redraw) {
            c->redraw_sidebar = true;
        }
        c->scrollGrabbed = true;
    }
}

void showContextMenu(Client *c) {
    int width = stringWidth(c->font_bold12, "Choose Option");
    int maxWidth;
    for (int i = 0; i < c->menu_size; i++) {
        maxWidth = stringWidth(c->font_bold12, c->menu_option[i]);
        if (maxWidth > width) {
            width = maxWidth;
        }
    }
    width += 8;

    int height = c->menu_size * 15 + 21;

    int x;
    int y;
    if (c->shell->mouse_click_x > 4 && c->shell->mouse_click_y > 4 && c->shell->mouse_click_x < 516 && c->shell->mouse_click_y < 338) {
        x = c->shell->mouse_click_x - width / 2 - 4;
        if (x + width > 512) {
            x = 512 - width;
        } else if (x < 0) {
            x = 0;
        }

        y = c->shell->mouse_click_y - 4;
        if (y + height > 334) {
            y = 334 - height;
        } else if (y < 0) {
            y = 0;
        }

        c->menu_visible = true;
        c->controller_menu_index = c->menu_size - 1;
        c->menu_area = 0;
        c->menu_x = x;
        c->menu_y = y;
        c->menu_width = width;
        c->menu_height = c->menu_size * 15 + 22;
    }
    if (c->shell->mouse_click_x > 553 && c->shell->mouse_click_y > 205 && c->shell->mouse_click_x < 743 && c->shell->mouse_click_y < 466) {
        x = c->shell->mouse_click_x - width / 2 - 553;
        if (x < 0) {
            x = 0;
        } else if (x + width > 190) {
            x = 190 - width;
        }

        y = c->shell->mouse_click_y - 205;
        if (y < 0) {
            y = 0;
        } else if (y + height > 261) {
            y = 261 - height;
        }

        c->menu_visible = true;
        c->controller_menu_index = c->menu_size - 1;
        c->menu_area = 1;
        c->menu_x = x;
        c->menu_y = y;
        c->menu_width = width;
        c->menu_height = c->menu_size * 15 + 22;
    }
    if (c->shell->mouse_click_x > 17 && c->shell->mouse_click_y > 357 && c->shell->mouse_click_x < 496 && c->shell->mouse_click_y < 453) {
        x = c->shell->mouse_click_x - width / 2 - 17;
        if (x < 0) {
            x = 0;
        } else if (x + width > 479) {
            x = 479 - width;
        }

        y = c->shell->mouse_click_y - 357;
        if (y < 0) {
            y = 0;
        } else if (y + height > 96) {
            y = 96 - height;
        }

        c->menu_visible = true;
        c->controller_menu_index = c->menu_size - 1;
        c->menu_area = 2;
        c->menu_x = x;
        c->menu_y = y;
        c->menu_width = width;
        c->menu_height = c->menu_size * 15 + 22;
    }
}

bool isAddFriendOption(Client *c, int option) {
    if (option < 0) {
        return false;
    }

    int action = c->menu_action[option];
    if (action >= 2000) {
        action -= 2000;
    }
    return action == 406;
}

void updateMergeLocs(Client *c) {
    if (c->scene_state == 2) {
        for (LocMergeEntity *loc = (LocMergeEntity *)linklist_head(c->merged_locations); loc; loc = (LocMergeEntity *)linklist_next(c->merged_locations)) {
            if (_Client.loop_cycle >= loc->lastCycle) {
                addLoc(c, loc->plane, loc->x, loc->z, loc->locIndex, loc->angle, loc->shape, loc->layer);
                linkable_unlink(&loc->link);
                free(loc);
            }
        }

        _Client.cyclelogic5++;
        if (_Client.cyclelogic5 > 85) {
            _Client.cyclelogic5 = 0;
            // ANTICHEAT_CYCLELOGIC5
            p1isaac(c->out, 100); // ANTICHEAT_CYCLELOGIC5
        }
    }
}

void updateEntityChats(Client *c) {
    for (int i = -1; i < c->player_count; i++) {
        int index;
        if (i == -1) {
            index = LOCAL_PLAYER_INDEX;
        } else {
            index = c->player_ids[i];
        }

        PlayerEntity *player = c->players[index];
        if (player && player->pathing_entity.chatTimer > 0) {
            player->pathing_entity.chatTimer--;

            if (player->pathing_entity.chatTimer == 0) {
                player->pathing_entity.chat[0] = '\0';
            }
        }
    }

    for (int i = 0; i < c->npc_count; i++) {
        int index = c->npc_ids[i];
        NpcEntity *npc = c->npcs[index];

        if (npc && npc->pathing_entity.chatTimer > 0) {
            npc->pathing_entity.chatTimer--;

            if (npc->pathing_entity.chatTimer == 0) {
                npc->pathing_entity.chat[0] = '\0';
            }
        }
    }
}

static void updateForceMovement(PathingEntity *entity) {
    int delta = entity->forceMoveEndCycle - _Client.loop_cycle;
    int dstX = entity->forceMoveStartSceneTileX * 128 + entity->size * 64;
    int dstZ = entity->forceMoveStartSceneTileZ * 128 + entity->size * 64;

    entity->x += (dstX - entity->x) / delta;
    entity->z += (dstZ - entity->z) / delta;

    entity->seqTrigger = 0;

    if (entity->forceMoveFaceDirection == 0) {
        entity->dstYaw = 1024;
    }

    if (entity->forceMoveFaceDirection == 1) {
        entity->dstYaw = 1536;
    }

    if (entity->forceMoveFaceDirection == 2) {
        entity->dstYaw = 0;
    }

    if (entity->forceMoveFaceDirection == 3) {
        entity->dstYaw = 512;
    }
}

static void startForceMovement(PathingEntity *entity) {
    if (entity->forceMoveStartCycle == _Client.loop_cycle || entity->primarySeqId == -1 || entity->primarySeqDelay != 0 || entity->primarySeqCycle + 1 > seqtype_get_duration(_SeqType.instances[entity->primarySeqId], entity->primarySeqFrame)) {
        int duration = entity->forceMoveStartCycle - entity->forceMoveEndCycle;
        int delta = _Client.loop_cycle - entity->forceMoveEndCycle;
        int dx0 = entity->forceMoveStartSceneTileX * 128 + entity->size * 64;
        int dz0 = entity->forceMoveStartSceneTileZ * 128 + entity->size * 64;
        int dx1 = entity->forceMoveEndSceneTileX * 128 + entity->size * 64;
        int dz1 = entity->forceMoveEndSceneTileZ * 128 + entity->size * 64;
        entity->x = (dx0 * (duration - delta) + dx1 * delta) / duration;
        entity->z = (dz0 * (duration - delta) + dz1 * delta) / duration;
    }

    entity->seqTrigger = 0;

    if (entity->forceMoveFaceDirection == 0) {
        entity->dstYaw = 1024;
    }

    if (entity->forceMoveFaceDirection == 1) {
        entity->dstYaw = 1536;
    }

    if (entity->forceMoveFaceDirection == 2) {
        entity->dstYaw = 0;
    }

    if (entity->forceMoveFaceDirection == 3) {
        entity->dstYaw = 512;
    }

    entity->yaw = entity->dstYaw;
}

static void updateMovement(PathingEntity *entity) {
    entity->secondarySeqId = entity->seqStandId;

    if (entity->pathLength == 0) {
        entity->seqTrigger = 0;
        return;
    }

    if (entity->primarySeqId != -1 && entity->primarySeqDelay == 0) {
        SeqType *seq = _SeqType.instances[entity->primarySeqId];
        if (!seq->walkmerge) {
            entity->seqTrigger++;
            return;
        }
    }

    int x = entity->x;
    int z = entity->z;
    int dstX = entity->pathTileX[entity->pathLength - 1] * 128 + entity->size * 64;
    int dstZ = entity->pathTileZ[entity->pathLength - 1] * 128 + entity->size * 64;

    if (dstX - x <= 256 && dstX - x >= -256 && dstZ - z <= 256 && dstZ - z >= -256) {
        if (x < dstX) {
            if (z < dstZ) {
                entity->dstYaw = 1280;
            } else if (z > dstZ) {
                entity->dstYaw = 1792;
            } else {
                entity->dstYaw = 1536;
            }
        } else if (x > dstX) {
            if (z < dstZ) {
                entity->dstYaw = 768;
            } else if (z > dstZ) {
                entity->dstYaw = 256;
            } else {
                entity->dstYaw = 512;
            }
        } else if (z < dstZ) {
            entity->dstYaw = 1024;
        } else {
            entity->dstYaw = 0;
        }

        int deltaYaw = entity->dstYaw - entity->yaw & 0x7ff;
        if (deltaYaw > 1024) {
            deltaYaw -= 2048;
        }

        int seqId = entity->seqTurnAroundId;
        if (deltaYaw >= -256 && deltaYaw <= 256) {
            seqId = entity->seqWalkId;
        } else if (deltaYaw >= 256 && deltaYaw < 768) {
            seqId = entity->seqTurnRightId;
        } else if (deltaYaw >= -768 && deltaYaw <= -256) {
            seqId = entity->seqTurnLeftId;
        }

        if (seqId == -1) {
            seqId = entity->seqWalkId;
        }

        entity->secondarySeqId = seqId;
        int moveSpeed = 4;
        if (entity->yaw != entity->dstYaw && entity->targetId == -1) {
            moveSpeed = 2;
        }

        if (entity->pathLength > 2) {
            moveSpeed = 6;
        }

        if (entity->pathLength > 3) {
            moveSpeed = 8;
        }

        if (entity->seqTrigger > 0 && entity->pathLength > 1) {
            moveSpeed = 8;
            entity->seqTrigger--;
        }

        if (entity->pathRunning[entity->pathLength - 1]) {
            moveSpeed <<= 0x1;
        }

        if (moveSpeed >= 8 && entity->secondarySeqId == entity->seqWalkId && entity->seqRunId != -1) {
            entity->secondarySeqId = entity->seqRunId;
        }

        if (x < dstX) {
            entity->x += moveSpeed;
            if (entity->x > dstX) {
                entity->x = dstX;
            }
        } else if (x > dstX) {
            entity->x -= moveSpeed;
            if (entity->x < dstX) {
                entity->x = dstX;
            }
        }
        if (z < dstZ) {
            entity->z += moveSpeed;
            if (entity->z > dstZ) {
                entity->z = dstZ;
            }
        } else if (z > dstZ) {
            entity->z -= moveSpeed;
            if (entity->z < dstZ) {
                entity->z = dstZ;
            }
        }

        if (entity->x == dstX && entity->z == dstZ) {
            entity->pathLength--;
        }
    } else {
        entity->x = dstX;
        entity->z = dstZ;
    }
}

static void updateFacingDirection(Client *c, PathingEntity *e) {
    if (e->targetId != -1 && e->targetId < 32768) {
        NpcEntity *npc = c->npcs[e->targetId];
        if (npc) {
            int dstX = e->x - npc->pathing_entity.x;
            int dstZ = e->z - npc->pathing_entity.z;

            if (dstX != 0 || dstZ != 0) {
                e->dstYaw = (int)(atan2(dstX, dstZ) * RADIANS_TO_RS) & 0x7ff;
            }
        }
    }

    if (e->targetId >= 32768) {
        int index = e->targetId - 32768;
        if (index == c->local_pid) {
            index = LOCAL_PLAYER_INDEX;
        }

        PlayerEntity *player = c->players[index];
        if (player) {
            int dstX = e->x - player->pathing_entity.x;
            int dstZ = e->z - player->pathing_entity.z;

            if (dstX != 0 || dstZ != 0) {
                e->dstYaw = (int)(atan2(dstX, dstZ) * RADIANS_TO_RS) & 0x7ff;
            }
        }
    }

    if ((e->targetTileX != 0 || e->targetTileZ != 0) && (e->pathLength == 0 || e->seqTrigger > 0)) {
        int dstX = e->x - (e->targetTileX - c->sceneBaseTileX - c->sceneBaseTileX) * 64;
        int dstZ = e->z - (e->targetTileZ - c->sceneBaseTileZ - c->sceneBaseTileZ) * 64;

        if (dstX != 0 || dstZ != 0) {
            e->dstYaw = (int)(atan2(dstX, dstZ) * RADIANS_TO_RS) & 0x7ff;
        }

        e->targetTileX = 0;
        e->targetTileZ = 0;
    }

    int remainingYaw = e->dstYaw - e->yaw & 0x7ff;

    if (remainingYaw != 0) {
        if (remainingYaw < e->turnRate || remainingYaw > 2048 - e->turnRate) {
            e->yaw = e->dstYaw;
        } else if (remainingYaw > 1024) {
            e->yaw -= e->turnRate;
        } else {
            e->yaw += e->turnRate;
        }

        e->yaw &= 0x7ff;

        if (e->secondarySeqId == e->seqStandId && e->yaw != e->dstYaw) {
            if (e->seqTurnId != -1) {
                e->secondarySeqId = e->seqTurnId;
                return;
            }

            e->secondarySeqId = e->seqWalkId;
        }
    }
}

static void updateSequences(PathingEntity *e) {
    e->seqStretches = false;

    SeqType *seq;
    if (e->secondarySeqId != -1) {
        seq = _SeqType.instances[e->secondarySeqId];
        e->secondarySeqCycle++;
        if (e->secondarySeqFrame < seq->frameCount && e->secondarySeqCycle > seqtype_get_duration(seq, e->secondarySeqFrame)) {
            e->secondarySeqCycle = 0;
            e->secondarySeqFrame++;
        }
        if (e->secondarySeqFrame >= seq->frameCount) {
            e->secondarySeqCycle = 0;
            e->secondarySeqFrame = 0;
        }
    }

    if (e->primarySeqId != -1 && e->primarySeqDelay == 0) {
        seq = _SeqType.instances[e->primarySeqId];
        e->primarySeqCycle++;
        while (e->primarySeqFrame < seq->frameCount && e->primarySeqCycle > seqtype_get_duration(seq, e->primarySeqFrame)) {
            e->primarySeqCycle -= seqtype_get_duration(seq, e->primarySeqFrame);
            e->primarySeqFrame++;
        }

        if (e->primarySeqFrame >= seq->frameCount) {
            e->primarySeqFrame -= seq->replayoff;
            e->primarySeqLoop++;
            if (e->primarySeqLoop >= seq->replaycount) {
                e->primarySeqId = -1;
            }
            if (e->primarySeqFrame < 0 || e->primarySeqFrame >= seq->frameCount) {
                e->primarySeqId = -1;
            }
        }

        e->seqStretches = seq->stretches;
    }

    if (e->primarySeqDelay > 0) {
        e->primarySeqDelay--;
    }

    if (e->spotanimId != -1 && _Client.loop_cycle >= e->spotanimLastCycle) {
        if (e->spotanimFrame < 0) {
            e->spotanimFrame = 0;
        }

        seq = _SpotAnimType.instances[e->spotanimId]->seq;
        e->spotanimCycle++;
        while (e->spotanimFrame < seq->frameCount && e->spotanimCycle > seqtype_get_duration(seq, e->spotanimFrame)) {
            e->spotanimCycle -= seqtype_get_duration(seq, e->spotanimFrame);
            e->spotanimFrame++;
        }

        if (e->spotanimFrame >= seq->frameCount) {
            if (e->spotanimFrame < 0 || e->spotanimFrame >= seq->frameCount) {
                e->spotanimId = -1;
            }
        }
    }
}

static void updateEntity(Client *c, PathingEntity *entity, int size) {
    (void)size;
    if (entity->x < 128 || entity->z < 128 || entity->x >= 13184 || entity->z >= 13184) {
        entity->primarySeqId = -1;
        entity->spotanimId = -1;
        entity->forceMoveEndCycle = 0;
        entity->forceMoveStartCycle = 0;
        entity->x = entity->pathTileX[0] * 128 + entity->size * 64;
        entity->z = entity->pathTileZ[0] * 128 + entity->size * 64;
        entity->pathLength = 0;
    }

    if (entity == &c->local_player->pathing_entity && (entity->x < 1536 || entity->z < 1536 || entity->x >= 11776 || entity->z >= 11776)) {
        entity->primarySeqId = -1;
        entity->spotanimId = -1;
        entity->forceMoveEndCycle = 0;
        entity->forceMoveStartCycle = 0;
        entity->x = entity->pathTileX[0] * 128 + entity->size * 64;
        entity->z = entity->pathTileZ[0] * 128 + entity->size * 64;
        entity->pathLength = 0;
    }

    if (entity->forceMoveEndCycle > _Client.loop_cycle) {
        updateForceMovement(entity);
    } else if (entity->forceMoveStartCycle >= _Client.loop_cycle) {
        startForceMovement(entity);
    } else {
        updateMovement(entity);
    }

    updateFacingDirection(c, entity);
    updateSequences(entity);
}

void updatePlayers(Client *c) {
    for (int i = -1; i < c->player_count; i++) {
        int index;
        if (i == -1) {
            index = LOCAL_PLAYER_INDEX;
        } else {
            index = c->player_ids[i];
        }

        PlayerEntity *player = c->players[index];
        if (player) {
            updateEntity(c, &player->pathing_entity, 1);
        }
    }

    _Client.cyclelogic6++;
    if (_Client.cyclelogic6 > 1406) {
        _Client.cyclelogic6 = 0;
        // ANTICHEAT_CYCLELOGIC1 (variable/self-length-prefixed shape matched rev254's CYCLELOGIC1, not CYCLELOGIC6 as originally commented)
        p1isaac(c->out, 51); // ANTICHEAT_CYCLELOGIC1
        p1(c->out, 0);
        int start = c->out->pos;
        p1(c->out, 162);
        p1(c->out, 22);
        if ((int)(jrand() * 2.0) == 0) {
            p1(c->out, 84);
        }
        p2(c->out, 31824);
        p2(c->out, 13490);
        if ((int)(jrand() * 2.0) == 0) {
            p1(c->out, 123);
        }
        if ((int)(jrand() * 2.0) == 0) {
            p1(c->out, 134);
        }
        p1(c->out, 100);
        p1(c->out, 94);
        p2(c->out, 35521);
        psize1(c->out, c->out->pos - start);
    }
}

static void updateNpcs(Client *c) {
    for (int i = 0; i < c->npc_count; i++) {
        int id = c->npc_ids[i];
        NpcEntity *npc = c->npcs[id];
        if (npc) {
            updateEntity(c, &npc->pathing_entity, npc->type->size);
        }
    }
}

static void client_update_orbit_camera(Client *c) {
    int orbitX = c->local_player->pathing_entity.x + c->camera_anticheat_offset_x;
    int orbitZ = c->local_player->pathing_entity.z + c->camera_anticheat_offset_z;
    if (c->orbitCameraX - orbitX < -500 || c->orbitCameraX - orbitX > 500 || c->orbitCameraZ - orbitZ < -500 || c->orbitCameraZ - orbitZ > 500) {
        c->orbitCameraX = orbitX;
        c->orbitCameraZ = orbitZ;
    }
    if (c->orbitCameraX != orbitX) {
        c->orbitCameraX += (orbitX - c->orbitCameraX) / 16;
    }
    if (c->orbitCameraZ != orbitZ) {
        c->orbitCameraZ += (orbitZ - c->orbitCameraZ) / 16;
    }
    if (c->shell->action_key[1] == 1) {
        c->orbitCameraYawVelocity += (-c->orbitCameraYawVelocity - 24) / 2;
    } else if (c->shell->action_key[2] == 1) {
        c->orbitCameraYawVelocity += (24 - c->orbitCameraYawVelocity) / 2;
    } else {
        c->orbitCameraYawVelocity /= 2;
    }
    if (c->shell->action_key[3] == 1) {
        c->orbitCameraPitchVelocity += (12 - c->orbitCameraPitchVelocity) / 2;
    } else if (c->shell->action_key[4] == 1) {
        c->orbitCameraPitchVelocity += (-c->orbitCameraPitchVelocity - 12) / 2;
    } else {
        c->orbitCameraPitchVelocity /= 2;
    }
    c->orbit_camera_yaw = c->orbit_camera_yaw + c->orbitCameraYawVelocity / 2 & 0x7ff;
    c->orbit_camera_pitch += c->orbitCameraPitchVelocity / 2;
    if (c->orbit_camera_pitch < 128) {
        c->orbit_camera_pitch = 128;
    }
    if (c->orbit_camera_pitch > 383) {
        c->orbit_camera_pitch = 383;
    }

    int orbitTileX = c->orbitCameraX >> 7;
    int orbitTileZ = c->orbitCameraZ >> 7;
    int orbitY = getHeightmapY(c, c->currentLevel, c->orbitCameraX, c->orbitCameraZ);
    int maxY = 0;

    if (orbitTileX > 3 && orbitTileZ > 3 && orbitTileX < 100 && orbitTileZ < 100) {
        for (int x = orbitTileX - 4; x <= orbitTileX + 4; x++) {
            for (int z = orbitTileZ - 4; z <= orbitTileZ + 4; z++) {
                int level = c->currentLevel;
                if (level < 3 && (c->levelTileFlags[1][x][z] & 0x2) == 2) {
                    level++;
                }

                int y = orbitY - c->levelHeightmap[level][x][z];
                if (y > maxY) {
                    maxY = y;
                }
            }
        }
    }

    int clamp = maxY * 192;
    if (clamp > 98048) {
        clamp = 98048;
    }

    if (clamp < 32768) {
        clamp = 32768;
    }

    if (clamp > c->cameraPitchClamp) {
        c->cameraPitchClamp += (clamp - c->cameraPitchClamp) / 24;
    } else if (clamp < c->cameraPitchClamp) {
        c->cameraPitchClamp += (clamp - c->cameraPitchClamp) / 80;
    }
}

static bool client_try_move(Client *c, int srcX, int srcZ, int dx, int dz, int type, int locWidth, int locLength, int locRotation, int locShape, int forceapproach, bool tryNearest) {
    int8_t sceneWidth = 104;
    int8_t sceneLength = 104;
    for (int x = 0; x < sceneWidth; x++) {
        for (int z = 0; z < sceneLength; z++) {
            c->bfsDirection[x][z] = 0;
            c->bfsCost[x][z] = 99999999;
        }
    }

    int x = srcX;
    int z = srcZ;

    c->bfsDirection[srcX][srcZ] = 99;
    c->bfsCost[srcX][srcZ] = 0;

    int steps = 0;
    int length = 0;

    c->bfsStepX[steps] = srcX;
    c->bfsStepZ[steps++] = srcZ;

    bool arrived = false;
    int bufferSize = BFS_STEP_SIZE;
    int **flags = c->levelCollisionMap[c->currentLevel]->flags;

    while (length != steps) {
        x = c->bfsStepX[length];
        z = c->bfsStepZ[length];
        length = (length + 1) % bufferSize;

        if (x == dx && z == dz) {
            arrived = true;
            break;
        }

        if (locShape != 0) {
            int shape = locShape - 1;

            if ((shape <= WALL_SQUARECORNER || shape == WALL_DIAGONAL) && collisionmap_test_wall(c->levelCollisionMap[c->currentLevel], x, z, dx, dz, shape, locRotation)) {
                arrived = true;
                break;
            }

            if (shape <= WALLDECOR_DIAGONAL_BOTH && collisionmap_test_wdecor(c->levelCollisionMap[c->currentLevel], x, z, dx, dz, shape, locRotation)) {
                arrived = true;
                break;
            }
        }

        if (locWidth != 0 && locLength != 0 && collisionmap_test_loc(c->levelCollisionMap[c->currentLevel], x, z, dx, dz, locWidth, locLength, forceapproach)) {
            arrived = true;
            break;
        }

        int nextCost = c->bfsCost[x][z] + 1;
        if (x > 0 && c->bfsDirection[x - 1][z] == 0 && (flags[x - 1][z] & 0x280108) == 0) {
            c->bfsStepX[steps] = x - 1;
            c->bfsStepZ[steps] = z;
            steps = (steps + 1) % bufferSize;
            c->bfsDirection[x - 1][z] = 2;
            c->bfsCost[x - 1][z] = nextCost;
        }

        if (x < sceneWidth - 1 && c->bfsDirection[x + 1][z] == 0 && (flags[x + 1][z] & 0x280180) == 0) {
            c->bfsStepX[steps] = x + 1;
            c->bfsStepZ[steps] = z;
            steps = (steps + 1) % bufferSize;
            c->bfsDirection[x + 1][z] = 8;
            c->bfsCost[x + 1][z] = nextCost;
        }

        if (z > 0 && c->bfsDirection[x][z - 1] == 0 && (flags[x][z - 1] & 0x280102) == 0) {
            c->bfsStepX[steps] = x;
            c->bfsStepZ[steps] = z - 1;
            steps = (steps + 1) % bufferSize;
            c->bfsDirection[x][z - 1] = 1;
            c->bfsCost[x][z - 1] = nextCost;
        }

        if (z < sceneLength - 1 && c->bfsDirection[x][z + 1] == 0 && (flags[x][z + 1] & 0x280120) == 0) {
            c->bfsStepX[steps] = x;
            c->bfsStepZ[steps] = z + 1;
            steps = (steps + 1) % bufferSize;
            c->bfsDirection[x][z + 1] = 4;
            c->bfsCost[x][z + 1] = nextCost;
        }

        if (x > 0 && z > 0 && c->bfsDirection[x - 1][z - 1] == 0 && (flags[x - 1][z - 1] & 0x28010E) == 0 && (flags[x - 1][z] & 0x280108) == 0 && (flags[x][z - 1] & 0x280102) == 0) {
            c->bfsStepX[steps] = x - 1;
            c->bfsStepZ[steps] = z - 1;
            steps = (steps + 1) % bufferSize;
            c->bfsDirection[x - 1][z - 1] = 3;
            c->bfsCost[x - 1][z - 1] = nextCost;
        }

        if (x < sceneWidth - 1 && z > 0 && c->bfsDirection[x + 1][z - 1] == 0 && (flags[x + 1][z - 1] & 0x280183) == 0 && (flags[x + 1][z] & 0x280180) == 0 && (flags[x][z - 1] & 0x280102) == 0) {
            c->bfsStepX[steps] = x + 1;
            c->bfsStepZ[steps] = z - 1;
            steps = (steps + 1) % bufferSize;
            c->bfsDirection[x + 1][z - 1] = 9;
            c->bfsCost[x + 1][z - 1] = nextCost;
        }

        if (x > 0 && z < sceneLength - 1 && c->bfsDirection[x - 1][z + 1] == 0 && (flags[x - 1][z + 1] & 0x280138) == 0 && (flags[x - 1][z] & 0x280108) == 0 && (flags[x][z + 1] & 0x280120) == 0) {
            c->bfsStepX[steps] = x - 1;
            c->bfsStepZ[steps] = z + 1;
            steps = (steps + 1) % bufferSize;
            c->bfsDirection[x - 1][z + 1] = 6;
            c->bfsCost[x - 1][z + 1] = nextCost;
        }

        if (x < sceneWidth - 1 && z < sceneLength - 1 && c->bfsDirection[x + 1][z + 1] == 0 && (flags[x + 1][z + 1] & 0x2801E0) == 0 && (flags[x + 1][z] & 0x280180) == 0 && (flags[x][z + 1] & 0x280120) == 0) {
            c->bfsStepX[steps] = x + 1;
            c->bfsStepZ[steps] = z + 1;
            steps = (steps + 1) % bufferSize;
            c->bfsDirection[x + 1][z + 1] = 12;
            c->bfsCost[x + 1][z + 1] = nextCost;
        }
    }

    c->tryMoveNearest = 0;

    if (!arrived) {
        if (tryNearest) {
            int min = 100;
            for (int padding = 1; padding < 2; padding++) {
                for (int px = dx - padding; px <= dx + padding; px++) {
                    for (int pz = dz - padding; pz <= dz + padding; pz++) {
                        if (px >= 0 && pz >= 0 && px < 104 && pz < 104 && c->bfsCost[px][pz] < min) {
                            min = c->bfsCost[px][pz];
                            x = px;
                            z = pz;
                            c->tryMoveNearest = 1;
                            arrived = true;
                        }
                    }
                }

                if (arrived) {
                    break;
                }
            }
        }

        if (!arrived) {
            return false;
        }
    }

    length = 0;
    c->bfsStepX[length] = x;
    c->bfsStepZ[length++] = z;

    int dir = c->bfsDirection[x][z];
    int next = dir;
    while (x != srcX || z != srcZ) {
        if (next != dir) {
            dir = next;
            c->bfsStepX[length] = x;
            c->bfsStepZ[length++] = z;
        }

        if ((next & 0x2) != 0) {
            x++;
        } else if ((next & 0x8) != 0) {
            x--;
        }

        if ((next & 0x1) != 0) {
            z++;
        } else if ((next & 0x4) != 0) {
            z--;
        }

        next = c->bfsDirection[x][z];
    }

    if (length > 0) {
        bufferSize = length;

        if (length > 25) {
            bufferSize = 25;
        }

        length--;

        int startX = c->bfsStepX[length];
        int startZ = c->bfsStepZ[length];

        // TODO showdebug
        // if (c->show_debug && super.actionKey[6] == 1 && super.actionKey[7] == 1) {
        // 	// check if tile is already added, if so remove it
        // 	for (int i = 0; i < c->userTileMarkers.length; i++) {
        // 		if (c->userTileMarkers[i] != null && c->userTileMarkers[i].x == World3D.clickTileX && c->userTileMarkers[i].z == World3D.clickTileZ) {
        // 			c->userTileMarkers[i] = null;
        // 			return false;
        // 		}
        // 	}

        // 	// add new
        // 	c->userTileMarkers[c->userTileMarkerIndex] = new Ground(c->currentLevel, World3D.clickTileX, World3D.clickTileZ);
        // 	c->userTileMarkerIndex = c->userTileMarkerIndex + 1 & (c->userTileMarkers.length - 1);
        // 	return false;
        // }

        if (type == 0) {
            // MOVE_GAMECLICK
            p1isaac(c->out, 6); // MOVE_GAMECLICK
            p1(c->out, bufferSize + bufferSize + 3);
        } else if (type == 1) {
            // MOVE_MINIMAPCLICK
            p1isaac(c->out, 220); // MOVE_MINIMAPCLICK
            p1(c->out, bufferSize + bufferSize + 3 + 14);
        } else if (type == 2) {
            // MOVE_OPCLICK
            p1isaac(c->out, 127); // MOVE_OPCLICK
            p1(c->out, bufferSize + bufferSize + 3);
        }

        if (c->shell->action_key[5] == 1) {
            p1(c->out, 1);
        } else {
            p1(c->out, 0);
        }

        p2(c->out, startX + c->sceneBaseTileX);
        p2(c->out, startZ + c->sceneBaseTileZ);
        c->flagSceneTileX = c->bfsStepX[0];
        c->flagSceneTileZ = c->bfsStepZ[0];

        for (int i = 1; i < bufferSize; i++) {
            length--;
            p1(c->out, c->bfsStepX[length] - startX);
            p1(c->out, c->bfsStepZ[length] - startZ);
        }

        return true;
    }

    return type != 1;
}

static bool interactWithLoc(Client *c, int opcode, int x, int z, int bitset) {
    int locId = bitset >> 14 & 0x7fff;
    int info = world3d_get_info(c->scene, c->currentLevel, x, z, bitset);
    if (info == -1) {
        return false;
    }

    int type = info & 0x1f;
    int angle = info >> 6 & 0x3;
    if (type == 10 || type == 11 || type == 22) {
        LocType *loc = loctype_get(locId);
        int width;
        int height;

        if (angle == 0 || angle == 2) {
            width = loc->width;
            height = loc->length;
        } else {
            width = loc->length;
            height = loc->width;
        }

        int forceapproach = loc->forceapproach;
        if (angle != 0) {
            forceapproach = (forceapproach << angle & 0xf) + (forceapproach >> (4 - angle));
        }

        client_try_move(c, c->local_player->pathing_entity.pathTileX[0], c->local_player->pathing_entity.pathTileZ[0], x, z, 2, width, height, 0, 0, forceapproach, false);
    } else {
        client_try_move(c, c->local_player->pathing_entity.pathTileX[0], c->local_player->pathing_entity.pathTileZ[0], x, z, 2, 0, 0, angle, type + 1, 0, false);
    }

    c->crossX = c->shell->mouse_click_x;
    c->crossY = c->shell->mouse_click_y;
    c->cross_mode = 2;
    c->cross_cycle = 0;

    p1isaac(c->out, opcode);
    p2(c->out, x + c->sceneBaseTileX);
    p2(c->out, z + c->sceneBaseTileZ);
    p2(c->out, locId);
    return true;
}

static void addFriend(Client *c, int64_t username) {
    if (username == 0L) {
        return;
    }

    if (c->friend_count >= 100) {
        client_add_message(c, 0, "Your friends list is full. Max of 100 hit", "");
        return;
    }

    char *displayName = jstring_format_name(jstring_from_base37(username));
    for (int i = 0; i < c->friend_count; i++) {
        if (c->friendName37[i] == username) {
            char buf[MAX_STR];
            sprintf(buf, "%s is already on your friend list", displayName);
            client_add_message(c, 0, buf, "");
            return;
        }
    }

    for (int i = 0; i < c->ignoreCount; i++) {
        if (c->ignoreName37[i] == username) {
            char buf[MAX_STR];
            sprintf(buf, "Please remove %s from your ignore list first", displayName);
            client_add_message(c, 0, buf, "");
            return;
        }
    }

    if (strcmp(displayName, c->local_player->name) != 0) {
        c->friendName[c->friend_count] = displayName;
        c->friendName37[c->friend_count] = username;
        c->friendWorld[c->friend_count] = 0;
        c->friend_count++;
        c->redraw_sidebar = true;

        // FRIENDLIST_ADD
        p1isaac(c->out, 9); // FRIENDLIST_ADD
        p8(c->out, username);
    }
}

static void addIgnore(Client *c, int64_t username) {
    if (username == 0L) {
        return;
    }

    if (c->ignoreCount >= 100) {
        client_add_message(c, 0, "Your ignore list is full. Max of 100 hit", "");
        return;
    }

    char *displayName = jstring_format_name(jstring_from_base37(username));
    for (int i = 0; i < c->ignoreCount; i++) {
        if (c->ignoreName37[i] == username) {
            char buf[MAX_STR];
            sprintf(buf, "%s is already on your ignore list", displayName);
            client_add_message(c, 0, buf, "");
            return;
        }
    }

    for (int i = 0; i < c->friend_count; i++) {
        if (c->friendName37[i] == username) {
            char buf[MAX_STR];
            sprintf(buf, "Please remove %s from your friend list first", displayName);
            client_add_message(c, 0, buf, "");
            return;
        }
    }

    c->ignoreName37[c->ignoreCount++] = username;
    c->redraw_sidebar = true;
    // IGNORELIST_ADD
    p1isaac(c->out, 189); // IGNORELIST_ADD
    p8(c->out, username);
}

static void removeFriend(Client *c, int64_t username) {
    if (username == 0L) {
        return;
    }

    for (int i = 0; i < c->friend_count; i++) {
        if (c->friendName37[i] == username) {
            c->friend_count--;
            c->redraw_sidebar = true;
            for (int j = i; j < c->friend_count; j++) {
                c->friendName[j] = c->friendName[j + 1];
                c->friendWorld[j] = c->friendWorld[j + 1];
                c->friendName37[j] = c->friendName37[j + 1];
            }
            // FRIENDLIST_DEL
            p1isaac(c->out, 84); // FRIENDLIST_DEL
            p8(c->out, username);
            return;
        }
    }
}

static void removeIgnore(Client *c, int64_t username) {
    if (username == 0L) {
        return;
    }

    for (int i = 0; i < c->ignoreCount; i++) {
        if (c->ignoreName37[i] == username) {
            c->ignoreCount--;
            c->redraw_sidebar = true;
            for (int j = i; j < c->ignoreCount; j++) {
                c->ignoreName37[j] = c->ignoreName37[j + 1];
            }
            // IGNORELIST_DEL
            p1isaac(c->out, 193); // IGNORELIST_DEL
            p8(c->out, username);
            return;
        }
    }
}

static void useMenuOption(Client *cl, int optionId) {
    if (optionId < 0) {
        return;
    }

    if (cl->chatback_input_open) {
        cl->chatback_input_open = false;
        cl->redraw_chatback = true;
    }

    int action = cl->menu_action[optionId];
    int a = cl->menuParamA[optionId];
    int b = cl->menuParamB[optionId];
    int c = cl->menuParamC[optionId];

    if (action >= 2000) {
        action -= 2000;
    }

    if (action == 903 || action == 363) {
        char *option = cl->menu_option[optionId];
        int tag = indexof(option, "@whi@");

        if (tag != -1) {
            option = substring(option, tag + 5, strlen(option));
            strtrim(option);
            char *name = jstring_format_name(jstring_from_base37(jstring_to_base37(option)));
            bool found = false;

            for (int i = 0; i < cl->player_count; i++) {
                PlayerEntity *player = cl->players[cl->player_ids[i]];

                if (player && player->name[0] && platform_strcasecmp(player->name, name) == 0) {
                    client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], player->pathing_entity.pathTileX[0], player->pathing_entity.pathTileZ[0], 2, 1, 1, 0, 0, 0, false);

                    if (action == 903) {
                        // OPPLAYER4
                        p1isaac(cl->out, 72); // OPPLAYER4
                    } else if (action == 363) {
                        // OPPLAYER1
                        p1isaac(cl->out, 192); // OPPLAYER1
                    }

                    p2(cl->out, cl->player_ids[i]);
                    found = true;
                    break;
                }
            }

            if (!found) {
                char buf[MAX_STR];
                sprintf(buf, "Unable to find %s", name);
                client_add_message(cl, 0, buf, "");
            }
        }
    } else if (action == 450) {
        // OPLOCU
        if (interactWithLoc(cl, 240, b, c, a)) { // OPLOCU
            p2(cl->out, cl->objInterface);
            p2(cl->out, cl->objSelectedSlot);
            p2(cl->out, cl->objSelectedInterface);
        }
    } else if (action == 405 || action == 38 || action == 422 || action == 478 || action == 347) {
        if (action == 478) {
            if ((b & 0x3) == 0) {
                _Client.oplogic5++;
            }

            if (_Client.oplogic5 >= 90) {
                // ANTICHEAT_OPLOGIC5
                p1isaac(cl->out, 233); // ANTICHEAT_OPLOGIC5
                p1(cl->out, 154);
            }

            // OPHELD4
            p1isaac(cl->out, 163); // OPHELD4
        } else if (action == 347) {
            // OPHELD5
            p1isaac(cl->out, 74); // OPHELD5
        } else if (action == 422) {
            // OPHELD3
            p1isaac(cl->out, 80); // OPHELD3
        } else if (action == 405) {
            _Client.oplogic3 += a;
            if (_Client.oplogic3 >= 97) {
                // ANTICHEAT_OPLOGIC3
                p1isaac(cl->out, 56); // ANTICHEAT_OPLOGIC3
                p4(cl->out, 0);
            }

            // OPHELD1
            p1isaac(cl->out, 243); // OPHELD1
        } else if (action == 38) {
            // OPHELD2
            p1isaac(cl->out, 228); // OPHELD2
        }

        p2(cl->out, a);
        p2(cl->out, b);
        p2(cl->out, c);
        cl->selected_cycle = 0;
        cl->selectedInterface = c;
        cl->selectedItem = b;
        cl->selected_area = 2;

        if (component_get(c)->layer == cl->viewport_interface_id) {
            cl->selected_area = 1;
        }

        if (component_get(c)->layer == cl->chat_interface_id) {
            cl->selected_area = 3;
        }
    } else if (action == 728 || action == 542 || action == 6 || action == 963 || action == 245) {
        NpcEntity *npc = cl->npcs[a];
        if (npc) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], npc->pathing_entity.pathTileX[0], npc->pathing_entity.pathTileZ[0], 2, 1, 1, 0, 0, 0, false);

            cl->crossX = cl->shell->mouse_click_x;
            cl->crossY = cl->shell->mouse_click_y;
            cl->cross_mode = 2;
            cl->cross_cycle = 0;

            if (action == 542) {
                // OPNPC2
                p1isaac(cl->out, 195); // OPNPC2
            } else if (action == 6) {
                if ((a & 0x3) == 0) {
                    _Client.oplogic2++;
                }

                if (_Client.oplogic2 >= 124) {
                    // ANTICHEAT_OPLOGIC2
                    p1isaac(cl->out, 77); // ANTICHEAT_OPLOGIC2
                    p2(cl->out, 37954);
                }

                // OPNPC3
                p1isaac(cl->out, 69); // OPNPC3
            } else if (action == 963) {
                // OPNPC4
                p1isaac(cl->out, 122); // OPNPC4
            } else if (action == 728) {
                // OPNPC1
                p1isaac(cl->out, 143); // OPNPC1
            } else if (action == 245) {
                if ((a & 0x3) == 0) {
                    _Client.oplogic4++;
                }

                if (_Client.oplogic4 >= 85) {
                    // ANTICHEAT_OPLOGIC4
                    p1isaac(cl->out, 121); // ANTICHEAT_OPLOGIC4
                    p1(cl->out, 131);
                }

                // OPNPC5
                p1isaac(cl->out, 118); // OPNPC5
            }

            p2(cl->out, a);
        }
    } else if (action == 217) {
        bool success = client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], b, c, 2, 0, 0, 0, 0, 0, false);
        if (!success) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], b, c, 2, 1, 1, 0, 0, 0, false);
        }

        cl->crossX = cl->shell->mouse_click_x;
        cl->crossY = cl->shell->mouse_click_y;
        cl->cross_mode = 2;
        cl->cross_cycle = 0;

        // OPOBJU
        p1isaac(cl->out, 245); // OPOBJU
        p2(cl->out, b + cl->sceneBaseTileX);
        p2(cl->out, c + cl->sceneBaseTileZ);
        p2(cl->out, a);
        p2(cl->out, cl->objInterface);
        p2(cl->out, cl->objSelectedSlot);
        p2(cl->out, cl->objSelectedInterface);
    } else if (action == 1175) {
        int locId = a >> 14 & 0x7fff;
        LocType *loc = loctype_get(locId);

        char examine[MAX_STR];
        if (!loc->desc) {
            sprintf(examine, "It's a %s.", loc->name);
        } else {
            strcpy(examine, loc->desc);
        }

        client_add_message(cl, 0, examine, "");
    } else if (action == 285) {
        // OPLOC1
        interactWithLoc(cl, 33, b, c, a); // OPLOC1
    } else if (action == 881) {
        // OPHELDU
        p1isaac(cl->out, 200); // OPHELDU
        p2(cl->out, a);
        p2(cl->out, b);
        p2(cl->out, c);
        p2(cl->out, cl->objInterface);
        p2(cl->out, cl->objSelectedSlot);
        p2(cl->out, cl->objSelectedInterface);

        cl->selected_cycle = 0;
        cl->selectedInterface = c;
        cl->selectedItem = b;
        cl->selected_area = 2;

        if (component_get(c)->layer == cl->viewport_interface_id) {
            cl->selected_area = 1;
        }

        if (component_get(c)->layer == cl->chat_interface_id) {
            cl->selected_area = 3;
        }
    } else if (action == 391) {
        // OPHELDT
        p1isaac(cl->out, 102); // OPHELDT
        p2(cl->out, a);
        p2(cl->out, b);
        p2(cl->out, c);
        p2(cl->out, cl->activeSpellId);

        cl->selected_cycle = 0;
        cl->selectedInterface = c;
        cl->selectedItem = b;
        cl->selected_area = 2;

        if (component_get(c)->layer == cl->viewport_interface_id) {
            cl->selected_area = 1;
        }

        if (component_get(c)->layer == cl->chat_interface_id) {
            cl->selected_area = 3;
        }
    } else if (action == 660) {
        if (cl->menu_visible) {
            world3d_click(b - 8, c - 11);
        } else {
            world3d_click(cl->shell->mouse_click_x - 4, cl->shell->mouse_click_y - 4);
        }
    } else if (action == 188) {
        cl->obj_selected = 1;
        cl->objSelectedSlot = b;
        cl->objSelectedInterface = c;
        cl->objInterface = a;
        cl->objSelectedName = objtype_get(a)->name;
        cl->spell_selected = 0;
        if (_Custom.item_outlines) {
            cl->redraw_sidebar = true;
        }
        return;
    } else if (action == 44) {
        if (!cl->pressed_continue_option) {
            // RESUME_PAUSEBUTTON
            p1isaac(cl->out, 146); // RESUME_PAUSEBUTTON
            p2(cl->out, c);
            cl->pressed_continue_option = true;
        }
    } else if (action == 1773) {
        ObjType *obj = objtype_get(a);
        char examine[MAX_STR];

        if (c >= 100000) {
            sprintf(examine, "%d x %s", c, obj->name);
        } else if (obj->desc[0] == '\0') {
            sprintf(examine, "It's a %s.", obj->name);
        } else {
            strcpy(examine, obj->desc);
        }

        client_add_message(cl, 0, examine, "");
    } else if (action == 900) {
        NpcEntity *npc = cl->npcs[a];

        if (npc) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], npc->pathing_entity.pathTileX[0], npc->pathing_entity.pathTileZ[0], 2, 1, 1, 0, 0, 0, false);
            cl->crossX = cl->shell->mouse_click_x;
            cl->crossY = cl->shell->mouse_click_y;
            cl->cross_mode = 2;
            cl->cross_cycle = 0;
            // OPNPCU
            p1isaac(cl->out, 119); // OPNPCU
            p2(cl->out, a);
            p2(cl->out, cl->objInterface);
            p2(cl->out, cl->objSelectedSlot);
            p2(cl->out, cl->objSelectedInterface);
        }
    } else if (action == 1373 || action == 1544 || action == 151 || action == 1101) {
        PlayerEntity *player = cl->players[a];
        if (player) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], player->pathing_entity.pathTileX[0], player->pathing_entity.pathTileZ[0], 2, 1, 1, 0, 0, 0, false);

            cl->crossX = cl->shell->mouse_click_x;
            cl->crossY = cl->shell->mouse_click_y;
            cl->cross_mode = 2;
            cl->cross_cycle = 0;

            if (action == 1101) {
                // OPPLAYER1
                p1isaac(cl->out, 192); // OPPLAYER1
            } else if (action == 151) {
                _Client.oplogic8++;
                if (_Client.oplogic8 >= 90) {
                    // ANTICHEAT_OPLOGIC8
                    p1isaac(cl->out, 206); // ANTICHEAT_OPLOGIC8
                    p1(cl->out, 19);
                }

                // OPPLAYER2
                p1isaac(cl->out, 17); // OPPLAYER2
            } else if (action == 1373) {
                // OPPLAYER4
                p1isaac(cl->out, 72); // OPPLAYER4
            } else if (action == 1544) {
                // OPPLAYER3
                p1isaac(cl->out, 18); // OPPLAYER3
            }

            p2(cl->out, a);
        }
    } else if (action == 265) {
        NpcEntity *npc = cl->npcs[a];
        if (npc) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], npc->pathing_entity.pathTileX[0], npc->pathing_entity.pathTileZ[0], 2, 1, 1, 0, 0, 0, false);

            cl->crossX = cl->shell->mouse_click_x;
            cl->crossY = cl->shell->mouse_click_y;
            cl->cross_mode = 2;
            cl->cross_cycle = 0;

            // OPNPCT
            p1isaac(cl->out, 231); // OPNPCT
            p2(cl->out, a);
            p2(cl->out, cl->activeSpellId);
        }
    } else if (action == 679) {
        const char *option = cl->menu_option[optionId];
        int tag = indexof(option, "@whi@");

        if (tag != -1) {
            char *name = substring(option, tag + 5, strlen(option));
            strtrim(name);
            int64_t name37 = jstring_to_base37(name);
            int friend = -1;
            for (int i = 0; i < cl->friend_count; i++) {
                if (cl->friendName37[i] == name37) {
                    friend = i;
                    break;
                }
            }

            if (friend != -1 && cl->friendWorld[friend] > 0) {
                cl->redraw_chatback = true;
                cl->chatback_input_open = false;
                cl->show_social_input = true;
                cl->social_input[0] = '\0';
                cl->social_action = 3;
                cl->social_name37 = cl->friendName37[friend];
                sprintf(cl->social_message, "Enter message to send to %s", cl->friendName[friend]);
                virtual_keyboard_maybe_open(cl, 3);
            }
        }
    } else if (action == 55) {
        // OPLOCT
        if (interactWithLoc(cl, 26, b, c, a)) { // OPLOCT
            p2(cl->out, cl->activeSpellId);
        }
    } else if (action == 224 || action == 993 || action == 99 || action == 746 || action == 877) {
        bool success = client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], b, c, 2, 0, 0, 0, 0, 0, false);
        if (!success) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], b, c, 2, 1, 1, 0, 0, 0, false);
        }

        cl->crossX = cl->shell->mouse_click_x;
        cl->crossY = cl->shell->mouse_click_y;
        cl->cross_mode = 2;
        cl->cross_cycle = 0;

        if (action == 224) {
            // OPOBJ1
            p1isaac(cl->out, 141); // OPOBJ1
        } else if (action == 746) {
            // OPOBJ4
            p1isaac(cl->out, 47); // OPOBJ4
        } else if (action == 877) {
            // OPOBJ5
            p1isaac(cl->out, 97); // OPOBJ5
        } else if (action == 99) {
            // OPOBJ3
            p1isaac(cl->out, 178); // OPOBJ3
        } else if (action == 993) {
            // OPOBJ2
            p1isaac(cl->out, 67); // OPOBJ2
        }

        p2(cl->out, b + cl->sceneBaseTileX);
        p2(cl->out, c + cl->sceneBaseTileZ);
        p2(cl->out, a);
    } else if (action == 1607) {
        NpcEntity *npc = cl->npcs[a];
        if (npc) {
            char examine[MAX_STR];

            if (!npc->type->desc) {
                // see addNpcOptions() - npc->type->name can be NULL for a type with no name entry
                sprintf(examine, "It's a %s.", npc->type->name ? npc->type->name : "Unknown");
            } else {
                strcpy(examine, npc->type->desc);
            }

            client_add_message(cl, 0, examine, "");
        }
    } else if (action == 504) {
        // OPLOC2
        interactWithLoc(cl, 213, b, c, a); // OPLOC2
    } else if (action == 930) {
        Component *com = component_get(c);
        cl->spell_selected = 1;
        cl->activeSpellId = c;
        cl->activeSpellFlags = com->actionTarget;
        cl->obj_selected = 0;
        if (_Custom.item_outlines) {
            cl->redraw_sidebar = true;
        }

        char *prefix = com->actionVerb;
        bool free_prefix = false;
        if (indexof_chr(prefix, ' ') != -1) {
            prefix = substring(prefix, 0, indexof_chr(prefix, ' '));
            free_prefix = true;
        }

        char *suffix = com->actionVerb;
        bool free_suffix = false;
        if (indexof_chr(suffix, ' ') != -1) {
            suffix = substring(suffix, indexof_chr(suffix, ' ') + 1, strlen(suffix));
            free_suffix = true;
        }

        sprintf(cl->spellCaption, "%s %s %s", prefix, com->action, suffix);
        if (free_prefix) {
            free(prefix);
        }
        if (free_suffix) {
            free(suffix);
        }

        if (cl->activeSpellFlags == 16) {
            cl->redraw_sidebar = true;
            cl->selected_tab = 3;
            cl->redraw_sideicons = true;
        }

        return;
    } else if (action == 951) {
        Component *com = component_get(c);
        bool notify = true;

        if (com->clientCode > 0) {
            notify = handleInterfaceAction(cl, com);
        }

        if (notify) {
            // IF_BUTTON
            p1isaac(cl->out, 244); // IF_BUTTON
            p2(cl->out, c);
        }
    } else if (action == 602 || action == 596 || action == 22 || action == 892 || action == 415) {
        if (action == 22) {
            // INV_BUTTON3
            p1isaac(cl->out, 59); // INV_BUTTON3
        } else if (action == 415) {
            if ((c & 0x3) == 0) {
                _Client.oplogic7++;
            }

            if (_Client.oplogic7 >= 55) {
                // ANTICHEAT_OPLOGIC7
                p1isaac(cl->out, 187); // ANTICHEAT_OPLOGIC7
                p4(cl->out, 0);
            }

            // INV_BUTTON5
            p1isaac(cl->out, 62); // INV_BUTTON5
        } else if (action == 602) {
            // INV_BUTTON1
            p1isaac(cl->out, 181); // INV_BUTTON1
        } else if (action == 892) {
            if ((b & 0x3) == 0) {
                _Client.oplogic9++;
            }

            if (_Client.oplogic9 >= 130) {
                // ANTICHEAT_OPLOGIC9
                p1isaac(cl->out, 162); // ANTICHEAT_OPLOGIC9
                p3(cl->out, 13018169);
            }

            // INV_BUTTON4
            p1isaac(cl->out, 160); // INV_BUTTON4
        } else if (action == 596) {
            // INV_BUTTON2
            p1isaac(cl->out, 70); // INV_BUTTON2
        }

        p2(cl->out, a);
        p2(cl->out, b);
        p2(cl->out, c);

        cl->selected_cycle = 0;
        cl->selectedInterface = c;
        cl->selectedItem = b;
        cl->selected_area = 2;

        if (component_get(c)->layer == cl->viewport_interface_id) {
            cl->selected_area = 1;
        }

        if (component_get(c)->layer == cl->chat_interface_id) {
            cl->selected_area = 3;
        }
    } else if (action == 581) {
        if ((a & 0x3) == 0) {
            _Client.oplogic1++;
        }

        if (_Client.oplogic1 >= 99) {
            // ANTICHEAT_OPLOGIC1
            p1isaac(cl->out, 28); // ANTICHEAT_OPLOGIC1
            p4(cl->out, 0);
        }

        // OPLOC4
        interactWithLoc(cl, 87, b, c, a); // OPLOC4
    } else if (action == 965) {
        bool success = client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], b, c, 2, 0, 0, 0, 0, 0, false);
        if (!success) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], b, c, 2, 1, 1, 0, 0, 0, false);
        }

        cl->crossX = cl->shell->mouse_click_x;
        cl->crossY = cl->shell->mouse_click_y;
        cl->cross_mode = 2;
        cl->cross_cycle = 0;

        // OPOBJT
        p1isaac(cl->out, 202); // OPOBJT
        p2(cl->out, b + cl->sceneBaseTileX);
        p2(cl->out, c + cl->sceneBaseTileZ);
        p2(cl->out, a);
        p2(cl->out, cl->activeSpellId);
    } else if (action == 1501) {
        _Client.oplogic6 += cl->sceneBaseTileZ;
        if (_Client.oplogic6 >= 92) {
            // ANTICHEAT_OPLOGIC6
            p1isaac(cl->out, 131); // ANTICHEAT_OPLOGIC6
            p2(cl->out, 6118);
        }

        // OPLOC5
        interactWithLoc(cl, 147, b, c, a); // OPLOC5
    } else if (action == 364) {
        // OPLOC3
        interactWithLoc(cl, 98, b, c, a); // OPLOC3
    } else if (action == 1102) {
        ObjType *obj = objtype_get(a);
        char examine[MAX_STR];

        if (obj->desc[0] == '\0') {
            sprintf(examine, "It's a %s.", obj->name);
        } else {
            strcpy(examine, obj->desc);
        }
        client_add_message(cl, 0, examine, "");
    } else if (action == 960) {
        // IF_BUTTON
        p1isaac(cl->out, 244); // IF_BUTTON
        p2(cl->out, c);

        Component *com = component_get(c);
        if (com->scripts && com->scripts[0][0] == 5) {
            int varp = com->scripts[0][1];
            if (cl->varps[varp] != com->scriptOperand[0]) {
                cl->varps[varp] = com->scriptOperand[0];
                updateVarp(cl, varp);
                cl->redraw_sidebar = true;
            }
        }
    } else if (action == 34) {
        const char *option = cl->menu_option[optionId];
        int tag = indexof(option, "@whi@");

        if (tag != -1) {
            closeInterfaces(cl);

            strcpy(cl->reportAbuseInput, substring(option, tag + 5, strlen(option)));
            strtrim(cl->reportAbuseInput);
            cl->reportAbuseMuteOption = false;

            Component *report = component_find_by_client_code(600);
            if (report) cl->reportAbuseInterfaceID = cl->viewport_interface_id = report->layer;
        }
    } else if (action == 947) {
        closeInterfaces(cl);
    } else if (action == 367) {
        PlayerEntity *player = cl->players[a];
        if (player) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], player->pathing_entity.pathTileX[0], player->pathing_entity.pathTileZ[0], 2, 1, 1, 0, 0, 0, false);

            cl->crossX = cl->shell->mouse_click_x;
            cl->crossY = cl->shell->mouse_click_y;
            cl->cross_mode = 2;
            cl->cross_cycle = 0;

            // OPPLAYERU
            p1isaac(cl->out, 113); // OPPLAYERU
            p2(cl->out, a);
            p2(cl->out, cl->objInterface);
            p2(cl->out, cl->objSelectedSlot);
            p2(cl->out, cl->objSelectedInterface);
        }
    } else if (action == 465) {
        // IF_BUTTON
        p1isaac(cl->out, 244); // IF_BUTTON
        p2(cl->out, c);

        Component *com = component_get(c);
        if (com->scripts && com->scripts[0][0] == 5) {
            int varp = com->scripts[0][1];
            cl->varps[varp] = 1 - cl->varps[varp];
            updateVarp(cl, varp);
            cl->redraw_sidebar = true;
        }
    } else if (action == 406 || action == 436 || action == 557 || action == 556) {
        const char *option = cl->menu_option[optionId];
        int tag = indexof(option, "@whi@");

        if (tag != -1) {
            char *username = substring(option, tag + 5, strlen(option));
            strtrim(username);
            int64_t username37 = jstring_to_base37(username);
            if (action == 406) {
                addFriend(cl, username37);
            } else if (action == 436) {
                addIgnore(cl, username37);
            } else if (action == 557) {
                removeFriend(cl, username37);
            } else if (action == 556) {
                removeIgnore(cl, username37);
            }
        }
    } else if (action == 651) {
        PlayerEntity *player = cl->players[a];

        if (player) {
            client_try_move(cl, cl->local_player->pathing_entity.pathTileX[0], cl->local_player->pathing_entity.pathTileZ[0], player->pathing_entity.pathTileX[0], player->pathing_entity.pathTileZ[0], 2, 1, 1, 0, 0, 0, false);

            cl->crossX = cl->shell->mouse_click_x;
            cl->crossY = cl->shell->mouse_click_y;
            cl->cross_mode = 2;
            cl->cross_cycle = 0;

            // OPPLAYERT
            p1isaac(cl->out, 68); // OPPLAYERT
            p2(cl->out, a);
            p2(cl->out, cl->activeSpellId);
        }
    }

    cl->obj_selected = 0;
    cl->spell_selected = 0;
    if (_Custom.item_outlines) {
        cl->redraw_sidebar = true;
    }
}

static void applyCutscene(Client *c) {
    int x = c->cutsceneSrcLocalTileX * 128 + 64;
    int z = c->cutsceneSrcLocalTileZ * 128 + 64;
    int y = getHeightmapY(c, c->currentLevel, c->cutsceneSrcLocalTileX, c->cutsceneSrcLocalTileZ) - c->cutsceneSrcHeight;

    if (c->cameraX < x) {
        c->cameraX += c->cutsceneMoveSpeed + (x - c->cameraX) * c->cutsceneMoveAcceleration / 1000;
        if (c->cameraX > x) {
            c->cameraX = x;
        }
    }

    if (c->cameraX > x) {
        c->cameraX -= c->cutsceneMoveSpeed + (c->cameraX - x) * c->cutsceneMoveAcceleration / 1000;
        if (c->cameraX < x) {
            c->cameraX = x;
        }
    }

    if (c->cameraY < y) {
        c->cameraY += c->cutsceneMoveSpeed + (y - c->cameraY) * c->cutsceneMoveAcceleration / 1000;
        if (c->cameraY > y) {
            c->cameraY = y;
        }
    }

    if (c->cameraY > y) {
        c->cameraY -= c->cutsceneMoveSpeed + (c->cameraY - y) * c->cutsceneMoveAcceleration / 1000;
        if (c->cameraY < y) {
            c->cameraY = y;
        }
    }

    if (c->cameraZ < z) {
        c->cameraZ += c->cutsceneMoveSpeed + (z - c->cameraZ) * c->cutsceneMoveAcceleration / 1000;
        if (c->cameraZ > z) {
            c->cameraZ = z;
        }
    }

    if (c->cameraZ > z) {
        c->cameraZ -= c->cutsceneMoveSpeed + (c->cameraZ - z) * c->cutsceneMoveAcceleration / 1000;
        if (c->cameraZ < z) {
            c->cameraZ = z;
        }
    }

    x = c->cutsceneDstLocalTileX * 128 + 64;
    z = c->cutsceneDstLocalTileZ * 128 + 64;
    y = getHeightmapY(c, c->currentLevel, c->cutsceneDstLocalTileX, c->cutsceneDstLocalTileZ) - c->cutsceneDstHeight;

    int deltaX = x - c->cameraX;
    int deltaY = y - c->cameraY;
    int deltaZ = z - c->cameraZ;

    int distance = (int)sqrt(deltaX * deltaX + deltaZ * deltaZ);
    int pitch = (int)(atan2(deltaY, distance) * RADIANS_TO_RS) & 0x7ff;
    int yaw = (int)(atan2(deltaX, deltaZ) * -RADIANS_TO_RS) & 0x7ff;

    if (pitch < 128) {
        pitch = 128;
    }

    if (pitch > 383) {
        pitch = 383;
    }

    if (c->cameraPitch < pitch) {
        c->cameraPitch += c->cutsceneRotateSpeed + (pitch - c->cameraPitch) * c->cutsceneRotateAcceleration / 1000;
        if (c->cameraPitch > pitch) {
            c->cameraPitch = pitch;
        }
    }

    if (c->cameraPitch > pitch) {
        c->cameraPitch -= c->cutsceneRotateSpeed + (c->cameraPitch - pitch) * c->cutsceneRotateAcceleration / 1000;
        if (c->cameraPitch < pitch) {
            c->cameraPitch = pitch;
        }
    }

    int deltaYaw = yaw - c->cameraYaw;
    if (deltaYaw > 1024) {
        deltaYaw -= 2048;
    }

    if (deltaYaw < -1024) {
        deltaYaw += 2048;
    }

    if (deltaYaw > 0) {
        c->cameraYaw += c->cutsceneRotateSpeed + deltaYaw * c->cutsceneRotateAcceleration / 1000;
        c->cameraYaw &= 0x7ff;
    }

    if (deltaYaw < 0) {
        c->cameraYaw -= c->cutsceneRotateSpeed + -deltaYaw * c->cutsceneRotateAcceleration / 1000;
        c->cameraYaw &= 0x7ff;
    }

    int tmp = yaw - c->cameraYaw;
    if (tmp > 1024) {
        tmp -= 2048;
    }

    if (tmp < -1024) {
        tmp += 2048;
    }

    if ((tmp < 0 && deltaYaw > 0) || (tmp > 0 && deltaYaw < 0)) {
        c->cameraYaw = yaw;
    }
}

static void handleInputKey(Client *c) {
    while (true) {
        int key;
        do {
            while (true) {
                key = poll_key(c->shell);
                if (key == -1) {
                    return;
                }

                if (c->viewport_interface_id != -1 && c->viewport_interface_id == c->reportAbuseInterfaceID) {
                    size_t len = strlen(c->reportAbuseInput);
                    if (key == 8 && len > 0) {
                        c->reportAbuseInput[len - 1] = '\0';
                    }
                    break;
                }

                if (c->show_social_input) {
                    size_t len = strlen(c->social_input);
                    if (key >= 32 && key <= 122 && len < CHAT_LENGTH) {
                        c->social_input[len] = (char)key;
                        c->social_input[len + 1] = '\0';
                        c->redraw_chatback = true;
                    }

                    if (key == 8 && len > 0) {
                        c->social_input[len - 1] = '\0';
                        c->redraw_chatback = true;
                    }

                    if (key == 13 || key == 10) {
                        c->show_social_input = false;
                        c->redraw_chatback = true;

                        int64_t username;
                        if (c->social_action == 1) {
                            username = jstring_to_base37(c->social_input);
                            addFriend(c, username);
                        }

                        if (c->social_action == 2 && c->friend_count > 0) {
                            username = jstring_to_base37(c->social_input);
                            removeFriend(c, username);
                        }

                        if (c->social_action == 3 && len > 0) {
                            // MESSAGE_PRIVATE
                            p1isaac(c->out, 214); // MESSAGE_PRIVATE
                            p1(c->out, 0);
                            int start = c->out->pos;
                            p8(c->out, c->social_name37);
                            wordpack_pack(c->out, c->social_input);
                            psize1(c->out, c->out->pos - start);
                            jstring_to_sentence_case(c->social_input);
                            wordfilter_filter(c->social_input);
                            client_add_message(c, 6, c->social_input, jstring_format_name(jstring_from_base37(c->social_name37)));
                            if (c->private_chat_setting == 2) {
                                c->private_chat_setting = 1;
                                c->redraw_privacy_settings = true;
                                // CHAT_SETMODE
                                p1isaac(c->out, 129); // CHAT_SETMODE
                                p1(c->out, c->public_chat_setting);
                                p1(c->out, c->private_chat_setting);
                                p1(c->out, c->trade_chat_setting);
                            }
                        }

                        if (c->social_action == 4 && c->ignoreCount < 100) {
                            username = jstring_to_base37(c->social_input);
                            addIgnore(c, username);
                        }

                        if (c->social_action == 5 && c->ignoreCount > 0) {
                            username = jstring_to_base37(c->social_input);
                            removeIgnore(c, username);
                        }
                    }
                } else if (c->chatback_input_open) {
                    size_t len = strlen(c->chatback_input);
                    if (key >= 48 && key <= 57 && len < CHATBACK_LENGTH) {
                        c->chatback_input[len] = (char)key;
                        c->chatback_input[len + 1] = '\0';
                        c->redraw_chatback = true;
                    }

                    if (key == 8 && len > 0) {
                        c->chatback_input[len - 1] = '\0';
                        c->redraw_chatback = true;
                    }

                    if (key == 13 || key == 10) {
                        if (len > 0) {
                            int value = 0;
                            // try {
                            value = atoi(c->chatback_input);
                            // } catch (Exception ignored) {
                            // }
                            // RESUME_P_COUNTDIALOG
                            p1isaac(c->out, 161); // RESUME_P_COUNTDIALOG
                            p4(c->out, value);
                        }
                        c->chatback_input_open = false;
                        c->redraw_chatback = true;
                    }
                } else if (c->chat_interface_id == -1) {
                    size_t len = strlen(c->chat_typed);
                    if (_Custom.allow_debugprocs) {
                        if (key >= 32 && key <= 126 && len < CHAT_LENGTH) {
                            c->chat_typed[len] = (char)key;
                            c->chat_typed[len + 1] = '\0';
                            c->redraw_chatback = true;
                        }
                    } else {
                        if (key >= 32 && key <= 122 && len < CHAT_LENGTH) {
                            c->chat_typed[len] = (char)key;
                            c->chat_typed[len + 1] = '\0';
                            c->redraw_chatback = true;
                        }
                    }

                    if (key == 8 && len > 0) {
                        c->chat_typed[len - 1] = '\0';
                        c->redraw_chatback = true;
                    }

                    len = strlen(c->chat_typed);
                    if ((key == 13 || key == 10) && len > 0) {
                        // custom, originally only with frame or local servers
                        if (c->rights || _Custom.allow_commands) {
                            if (strcmp(c->chat_typed, "::clientdrop") == 0 /* && c->shell->window */) {
                                client_try_reconnect(c);
                            } else if (strcmp(c->chat_typed, "::noclip") == 0) {
                                for (int level = 0; level < 4; level++) {
                                    for (int x = 1; x < 104 - 1; x++) {
                                        for (int z = 1; z < 104 - 1; z++) {
                                            c->levelCollisionMap[level]->flags[x][z] = 0;
                                        }
                                    }
                                }
                            } else if (strcmp(c->chat_typed, "::debug") == 0) {
                                // _Custom.show_debug = !_Custom.show_debug;
                            } else if (strcmp(c->chat_typed, "::perf") == 0) {
                                _Custom.show_performance = !_Custom.show_performance;
                            } else if (strcmp(c->chat_typed, "::gl") == 0) {
                                _Custom.use_opengl11 = !_Custom.use_opengl11;

#ifdef GL11
                                char buf[MAX_STR];
                                sprintf(buf, "OpenGL renderer is now %s.", _Custom.use_opengl11 ? "enabled" : "disabled");
                                client_add_message(c, 0, buf, "");
#else
                                client_add_message(c, 0, "This client was not built with OpenGL support!", "");
#endif
                            } else if (strcmp(c->chat_typed, "::wf") == 0) {
#ifdef GL11
                                static bool wireframe;
                                if (!wireframe) {
                                    glPolygonMode(GL_FRONT, GL_LINE);
                                } else {
                                    glPolygonMode(GL_FRONT, GL_FILL);
                                }
                                wireframe = !wireframe;
#endif
                            } else if (strcmp(c->chat_typed, "::camera") == 0) {
                                // _Custom.camera_editor = !_Custom.camera_editor;
                                // c->cutscene = _Custom.camera_editor;
                                // c->cutsceneDstLocalTileX = 52;
                                // c->cutsceneDstLocalTileZ = 52;
                                // c->cutsceneSrcLocalTileX = 52;
                                // c->cutsceneSrcLocalTileZ = 52;
                                // c->cutsceneSrcHeight = 1000;
                                // c->cutsceneDstHeight = 1000;
                                // TODO
                                // } else if (c->chat_typed.startsWith("::camsrc ")) {
                                //     const char** args = c->chat_typed.split(" ");
                                //     if (args.length == 3) {
                                //         c->cutsceneSrcLocalTileX = atoi(args[1]);
                                //         c->cutsceneSrcLocalTileZ = atoi(args[2]);
                                //     } else if (args.length == 4) {
                                //         c->cutsceneSrcLocalTileX = atoi(args[1]);
                                //         c->cutsceneSrcLocalTileZ = atoi(args[2]);
                                //         c->cutsceneSrcHeight = atoi(args[3]);
                                //     }
                                // } else if (c->chat_typed.startsWith("::camdst ")) {
                                //     const char** args = c->chat_typed.split(" ");
                                //     if (args.length == 3) {
                                //         c->cutsceneDstLocalTileX = atoi(args[1]);
                                //         c->cutsceneDstLocalTileZ = atoi(args[2]);
                                //     } else if (args.length == 4) {
                                //         c->cutsceneDstLocalTileX = atoi(args[1]);
                                //         c->cutsceneDstLocalTileZ = atoi(args[2]);
                                //         c->cutsceneDstHeight = atoi(args[3]);
                                //     }
                                // }
                            }
                        }

                        if (strstartswith(c->chat_typed, "::")) {
                            // CLIENT_CHEAT
                            p1isaac(c->out, 86); // CLIENT_CHEAT
                            p1(c->out, len - 1);
                            char *sub = substring(c->chat_typed, 2, len);
                            pjstr(c->out, sub);
                            free(sub);
                        } else {
                            int8_t color = 0;
                            if (strstartswith(c->chat_typed, "yellow:")) {
                                color = 0;
                                strcpy(c->chat_typed, substring(c->chat_typed, 7, len));
                            } else if (strstartswith(c->chat_typed, "red:")) {
                                color = 1;
                                strcpy(c->chat_typed, substring(c->chat_typed, 4, len));
                            } else if (strstartswith(c->chat_typed, "green:")) {
                                color = 2;
                                strcpy(c->chat_typed, substring(c->chat_typed, 6, len));
                            } else if (strstartswith(c->chat_typed, "cyan:")) {
                                color = 3;
                                strcpy(c->chat_typed, substring(c->chat_typed, 5, len));
                            } else if (strstartswith(c->chat_typed, "purple:")) {
                                color = 4;
                                strcpy(c->chat_typed, substring(c->chat_typed, 7, len));
                            } else if (strstartswith(c->chat_typed, "white:")) {
                                color = 5;
                                strcpy(c->chat_typed, substring(c->chat_typed, 6, len));
                            } else if (strstartswith(c->chat_typed, "flash1:")) {
                                color = 6;
                                strcpy(c->chat_typed, substring(c->chat_typed, 7, len));
                            } else if (strstartswith(c->chat_typed, "flash2:")) {
                                color = 7;
                                strcpy(c->chat_typed, substring(c->chat_typed, 7, len));
                            } else if (strstartswith(c->chat_typed, "flash3:")) {
                                color = 8;
                                strcpy(c->chat_typed, substring(c->chat_typed, 7, len));
                            } else if (strstartswith(c->chat_typed, "glow1:")) {
                                color = 9;
                                strcpy(c->chat_typed, substring(c->chat_typed, 6, len));
                            } else if (strstartswith(c->chat_typed, "glow2:")) {
                                color = 10;
                                strcpy(c->chat_typed, substring(c->chat_typed, 6, len));
                            } else if (strstartswith(c->chat_typed, "glow3:")) {
                                color = 11;
                                strcpy(c->chat_typed, substring(c->chat_typed, 6, len));
                            }

                            int8_t effect = 0;
                            if (strstartswith(c->chat_typed, "wave:")) {
                                effect = 1;
                                strcpy(c->chat_typed, substring(c->chat_typed, 5, strlen(c->chat_typed)));
                            }
                            if (strstartswith(c->chat_typed, "scroll:")) {
                                effect = 2;
                                strcpy(c->chat_typed, substring(c->chat_typed, 7, strlen(c->chat_typed)));
                            }

                            // MESSAGE_PUBLIC
                            p1isaac(c->out, 83); // MESSAGE_PUBLIC
                            p1(c->out, 0);
                            int start = c->out->pos;
                            p1(c->out, color);
                            p1(c->out, effect);
                            wordpack_pack(c->out, c->chat_typed);
                            psize1(c->out, c->out->pos - start);

                            jstring_to_sentence_case(c->chat_typed);
                            wordfilter_filter(c->chat_typed);
                            strcpy(c->local_player->pathing_entity.chat, c->chat_typed);
                            c->local_player->pathing_entity.chatColor = color;
                            c->local_player->pathing_entity.chatStyle = effect;
                            c->local_player->pathing_entity.chatTimer = 150;
                            client_add_message(c, 2, c->local_player->pathing_entity.chat, c->local_player->name);

                            if (c->public_chat_setting == 2) {
                                c->public_chat_setting = 3;
                                c->redraw_privacy_settings = true;
                                // CHAT_SETMODE
                                p1isaac(c->out, 129); // CHAT_SETMODE
                                p1(c->out, c->public_chat_setting);
                                p1(c->out, c->private_chat_setting);
                                p1(c->out, c->trade_chat_setting);
                            }
                        }

                        c->chat_typed[0] = '\0';
                        c->redraw_chatback = true;
                    }
                }
            }
        } while ((key < 97 || key > 122) && (key < 65 || key > 90) && (key < 48 || key > 57) && key != 32);

        size_t len = strlen(c->reportAbuseInput);
        if (len < REPORT_ABUSE_LENGTH) {
            c->reportAbuseInput[len] = (char)key;
            c->reportAbuseInput[len + 1] = '\0';
        }
    }
}

static void handleMouseInput(Client *c) {
#ifdef __PS2__
    if (!c->controller_grid_analog_override &&
        c->controller_grid_screen_valid && c->controller_grid_component >= 0) {
        c->shell->mouse_x = c->controller_grid_screen_x;
        c->shell->mouse_y = c->controller_grid_screen_y;
        if (c->shell->mouse_click_button != 0) {
            c->shell->mouse_click_x = c->controller_grid_screen_x;
            c->shell->mouse_click_y = c->controller_grid_screen_y;
        }
    }
#endif
    if (c->obj_drag_area != 0) {
        return;
    }

    int button = c->shell->mouse_click_button;
    bool controller_primary = c->controller_primary_action;
    c->controller_primary_action = false;
    if (c->spell_selected == 1 && c->shell->mouse_click_x >= 516 && c->shell->mouse_click_y >= 160 && c->shell->mouse_click_x <= 765 && c->shell->mouse_click_y <= 205) {
        button = 0;
    }

    if (c->menu_visible) {
        if (button != 1 && c->controller_menu_index < 0) {
            int x = c->shell->mouse_x;
            int y = c->shell->mouse_y;

            if (c->menu_area == 0) {
                x -= 4;
                y -= 4;
            } else if (c->menu_area == 1) {
                x -= 553;
                y -= 205;
            } else if (c->menu_area == 2) {
                x -= 17;
                y -= 357;
            }

            if (x < c->menu_x - 10 || x > c->menu_x + c->menu_width + 10 || y < c->menu_y - 10 || y > c->menu_y + c->menu_height + 10) {
                c->menu_visible = false;
                if (c->menu_area == 1) {
                    c->redraw_sidebar = true;
                }
                if (c->menu_area == 2) {
                    c->redraw_chatback = true;
                }
            }
        }

        if (button == 1) {
            int menuX = c->menu_x;
            int menuY = c->menu_y;
            int menuWidth = c->menu_width;

            int clickX = c->shell->mouse_click_x;
            int clickY = c->shell->mouse_click_y;

            if (c->menu_area == 0) {
                clickX -= 4;
                clickY -= 4;
            } else if (c->menu_area == 1) {
                clickX -= 553;
                clickY -= 205;
            } else if (c->menu_area == 2) {
                clickX -= 17;
                clickY -= 357;
            }

            int option = -1;
            for (int i = 0; i < c->menu_size; i++) {
                int optionY = menuY + (c->menu_size - 1 - i) * 15 + 31;
                if (clickX > menuX && clickX < menuX + menuWidth && clickY > optionY - 13 && clickY < optionY + 3) {
                    option = i;
                }
            }

            if (option != -1) {
                useMenuOption(c, option);
            }

            c->menu_visible = false;
            c->controller_menu_index = -1;
            if (c->menu_area == 1) {
                c->redraw_sidebar = true;
            } else if (c->menu_area == 2) {
                c->redraw_chatback = true;
            }
        }
    } else {
        if (button == 1 && c->menu_size > 0) {
            int action = c->menu_action[c->menu_size - 1];

            if (action == 602 || action == 596 || action == 22 || action == 892 || action == 415 || action == 405 || action == 38 || action == 422 || action == 478 || action == 347 || action == 188) {
                int slot = c->menuParamB[c->menu_size - 1];
                int comId = c->menuParamC[c->menu_size - 1];
                // menuParamC's component id traces back to menu-building code that stores whatever
                // id a hovered/picked entity happened to carry - same unchecked-network/state-id
                // class of bug fixed everywhere else this session. com->draggable was dereferenced
                // with zero validation.
                Component *com = component_get(comId);

                if (com && com->draggable) {
                    c->objGrabThreshold = false;
                    c->obj_drag_cycles = 0;
                    c->objDragInterfaceId = comId;
                    c->objDragSlot = slot;
                    c->obj_drag_area = 2;
                    c->objGrabX = c->shell->mouse_click_x;
                    c->objGrabY = c->shell->mouse_click_y;

                    if (com->layer == c->viewport_interface_id) {
                        c->obj_drag_area = 1;
                    }

                    if (com->layer == c->chat_interface_id) {
                        c->obj_drag_area = 3;
                    }

                    return;
                }
            }
        }

        if (button == 1 && !controller_primary &&
            (c->mouseButtonsOption == 1 || isAddFriendOption(c, c->menu_size - 1)) &&
            c->menu_size > 2) {
            button = 2;
        }

        if (button == 1 && c->menu_size > 0) {
            useMenuOption(c, c->menu_size - 1);
        }

        if (button != 2 || c->menu_size <= 0) {
            return;
        }

        showContextMenu(c);
    }
}

static void handleMinimapInput(Client *c) {
    if (c->shell->mouse_click_button == 1) {
        int x = c->shell->mouse_click_x - 25 - 550;
        int y = c->shell->mouse_click_y - 4 - 4;

        if (x >= 0 && y >= 0 && x < 146 && y < 151) {
            x -= 73;
            y -= 75;

            int yaw = c->orbit_camera_yaw + c->minimap_anticheat_angle & 0x7ff;
            int sinYaw = _Pix3D.sin_table[yaw];
            int cosYaw = _Pix3D.cos_table[yaw];

            sinYaw = (sinYaw * (c->minimap_zoom + 256)) >> 8;
            cosYaw = (cosYaw * (c->minimap_zoom + 256)) >> 8;

            int relX = (y * sinYaw + x * cosYaw) >> 11;
            int relY = (y * cosYaw - x * sinYaw) >> 11;

            int tileX = (c->local_player->pathing_entity.x + relX) >> 7;
            int tileZ = (c->local_player->pathing_entity.z - relY) >> 7;

            bool success = client_try_move(c, c->local_player->pathing_entity.pathTileX[0], c->local_player->pathing_entity.pathTileZ[0], tileX, tileZ, 1, 0, 0, 0, 0, 0, true);
            if (success) {
                // the additional 14-bytes in MOVE_MINIMAPCLICK
                p1(c->out, x);
                p1(c->out, y);
                p2(c->out, c->orbit_camera_yaw);
                p1(c->out, 57);
                p1(c->out, c->minimap_anticheat_angle);
                p1(c->out, c->minimap_zoom);
                p1(c->out, 89);
                p2(c->out, c->local_player->pathing_entity.x);
                p2(c->out, c->local_player->pathing_entity.z);
                p1(c->out, c->tryMoveNearest);
                p1(c->out, 63);
            }
        }
    }
}

static void handleTabInput(Client *c) {
    if (c->shell->mouse_click_button != 1) {
        return;
    }

    // hit-box values below are irregular (169 vs 168, 203/502 vs 505/503, differing widths) exactly
    // like the real client's own tab boxes - copied verbatim from the reference rather than
    // "normalized", since these correspond to the fixed-mode (765x503) frame's actual sprite seams.
    if (c->shell->mouse_click_x >= 539 && c->shell->mouse_click_x <= 573 && c->shell->mouse_click_y >= 169 && c->shell->mouse_click_y < 205 && c->tab_interface_id[0] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 0;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 569 && c->shell->mouse_click_x <= 599 && c->shell->mouse_click_y >= 168 && c->shell->mouse_click_y < 205 && c->tab_interface_id[1] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 1;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 597 && c->shell->mouse_click_x <= 627 && c->shell->mouse_click_y >= 168 && c->shell->mouse_click_y < 205 && c->tab_interface_id[2] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 2;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 625 && c->shell->mouse_click_x <= 669 && c->shell->mouse_click_y >= 168 && c->shell->mouse_click_y < 203 && c->tab_interface_id[3] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 3;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 666 && c->shell->mouse_click_x <= 696 && c->shell->mouse_click_y >= 168 && c->shell->mouse_click_y < 205 && c->tab_interface_id[4] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 4;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 694 && c->shell->mouse_click_x <= 724 && c->shell->mouse_click_y >= 168 && c->shell->mouse_click_y < 205 && c->tab_interface_id[5] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 5;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 722 && c->shell->mouse_click_x <= 756 && c->shell->mouse_click_y >= 169 && c->shell->mouse_click_y < 205 && c->tab_interface_id[6] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 6;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 540 && c->shell->mouse_click_x <= 574 && c->shell->mouse_click_y >= 466 && c->shell->mouse_click_y < 502 && c->tab_interface_id[7] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 7;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 572 && c->shell->mouse_click_x <= 602 && c->shell->mouse_click_y >= 466 && c->shell->mouse_click_y < 503 && c->tab_interface_id[8] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 8;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 599 && c->shell->mouse_click_x <= 629 && c->shell->mouse_click_y >= 466 && c->shell->mouse_click_y < 503 && c->tab_interface_id[9] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 9;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 627 && c->shell->mouse_click_x <= 671 && c->shell->mouse_click_y >= 467 && c->shell->mouse_click_y < 502 && c->tab_interface_id[10] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 10;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 669 && c->shell->mouse_click_x <= 699 && c->shell->mouse_click_y >= 466 && c->shell->mouse_click_y < 503 && c->tab_interface_id[11] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 11;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 696 && c->shell->mouse_click_x <= 726 && c->shell->mouse_click_y >= 466 && c->shell->mouse_click_y < 503 && c->tab_interface_id[12] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 12;
        c->redraw_sideicons = true;
    } else if (c->shell->mouse_click_x >= 724 && c->shell->mouse_click_x <= 758 && c->shell->mouse_click_y >= 466 && c->shell->mouse_click_y < 502 && c->tab_interface_id[13] != -1) {
        c->redraw_sidebar = true;
        c->selected_tab = 13;
        c->redraw_sideicons = true;
    }

    _Client.cyclelogic1++;
    if (_Client.cyclelogic1 > 150) {
        _Client.cyclelogic1 = 0;
        // ANTICHEAT_CYCLELOGIC6 (fixed 1-byte shape matched rev254's CYCLELOGIC6, not CYCLELOGIC1 as originally commented - real CYCLELOGIC1 is variable-length, see the c->out 51 site above)
        p1isaac(c->out, 36); // ANTICHEAT_CYCLELOGIC6
        p1(c->out, 62);
    }
}

// L1/R1 sidebar tab cycling - mirrors handleTabInput()'s exact 3-line effect above (redraw_sidebar/
// selected_tab/redraw_sideicons), stepping to the next/prev tab whose slot isn't -1 rather than
// hit-testing pixel rects. c->controller_tab_step is a one-shot +-1 set by ps2.c on L1/R1 press.
static void handleControllerTabInput(Client *c) {
    if (c->controller_tab_step == 0) {
        return;
    }

    int step = c->controller_tab_step;
    c->controller_tab_step = 0;
    if (c->controller_settings_visible || c->virtual_keyboard_visible || c->menu_visible) {
        return;
    }

    int next = c->selected_tab;
    for (int tries = 0; tries < 14; tries++) {
        next = (next + step + 14) % 14;
        if (c->tab_interface_id[next] != -1) {
            c->redraw_sidebar = true;
            c->selected_tab = next;
            c->redraw_sideicons = true;
            c->controller_grid_component = -1;
            c->controller_grid_slot = -1;
            c->controller_grid_screen_valid = false;
            c->controller_grid_analog_override = true;
            break;
        }
    }
}

// The rest of the controller button map (see src/platform/ps2.c for which hardware bit sets each
// one-shot/level field). Each button's real-world meaning lives here, platform-agnostically -
// ps2.c only knows about hardware bits, never about tabs/camera/chat.
static void handleControllerButtonInput(Client *c) {
    if (c->controller_grid_cancel_pressed) {
        c->controller_grid_cancel_pressed = false;

        // R3 releases controller focus ONLY. Do not call closeInterfaces(), change any interface
        // ID, change the selected tab, or feed the press into generic Back handling.
        if (c->controller_grid_screen_valid) {
            c->controller_free_cursor_x = c->controller_grid_screen_x;
            c->controller_free_cursor_y = c->controller_grid_screen_y;
            c->controller_free_cursor_valid = true;
            c->shell->mouse_x = c->controller_grid_screen_x;
            c->shell->mouse_y = c->controller_grid_screen_y;
        } else {
            c->controller_free_cursor_x = c->shell->mouse_x;
            c->controller_free_cursor_y = c->shell->mouse_y;
            c->controller_free_cursor_valid = true;
        }

        c->controller_grid_component = -1;
        c->controller_grid_slot = -1;
        c->controller_grid_screen_valid = false;
        c->controller_grid_analog_override = true;
        c->controller_dpad_x = 0;
        c->controller_dpad_y = 0;

        // Only repaint retained surfaces to erase the focus border; this does not close them.
        c->redraw_sidebar = true;
        c->redraw_chatback = true;
    }

    if (c->controller_options_pressed) {
        c->controller_options_pressed = false;
        if (c->menu_visible) {
            c->menu_visible = false;
            c->controller_menu_index = -1;
            if (c->menu_area == 1) c->redraw_sidebar = true;
            if (c->menu_area == 2) c->redraw_chatback = true;
        }
    }

    if (c->controller_settings_pressed) {
        c->controller_settings_pressed = false;
        if (c->ingame && !c->virtual_keyboard_visible) {
            c->controller_settings_visible = !c->controller_settings_visible;
            c->controller_settings_row = 0;
            if (c->controller_settings_visible && c->menu_visible) {
                c->menu_visible = false;
                c->controller_menu_index = -1;
            }
        }
    }

    if (c->controller_settings_visible) {
        int dpad_x = c->controller_dpad_x;
        int dpad_y = c->controller_dpad_y;
        c->controller_dpad_x = 0;
        c->controller_dpad_y = 0;

        if (dpad_y != 0) {
            c->controller_settings_row += dpad_y;
            if (c->controller_settings_row < 0) c->controller_settings_row = 0;
            if (c->controller_settings_row > 3) c->controller_settings_row = 3;
        }
        if (dpad_x != 0) {
            if (c->controller_settings_row == 0) {
                c->controller_cursor_deadzone += dpad_x * 4;
                if (c->controller_cursor_deadzone < 4) c->controller_cursor_deadzone = 4;
                if (c->controller_cursor_deadzone > 48) c->controller_cursor_deadzone = 48;
            } else if (c->controller_settings_row == 1) {
                c->controller_cursor_speed += dpad_x;
                if (c->controller_cursor_speed < 2) c->controller_cursor_speed = 2;
                if (c->controller_cursor_speed > 10) c->controller_cursor_speed = 10;
            } else if (c->controller_settings_row == 2) {
                c->controller_camera_deadzone += dpad_x * 4;
                if (c->controller_camera_deadzone < 16) c->controller_camera_deadzone = 16;
                if (c->controller_camera_deadzone > 64) c->controller_camera_deadzone = 64;
            }
        }

        if (c->controller_confirm_pressed) {
            c->controller_confirm_pressed = false;
            if (c->controller_settings_row == 3) {
                c->controller_cursor_deadzone = 20;
                c->controller_cursor_speed = 5;
                c->controller_camera_deadzone = 40;
            }
        }
        if (c->controller_back_pressed) {
            c->controller_back_pressed = false;
            c->controller_settings_visible = false;
        }

        c->controller_inventory_pressed = false;
        c->controller_snap_camera_pressed = false;
        c->controller_start_pressed = false;
        c->controller_zoom_bias = 0;
        return;
    }

    if (c->menu_visible) {
        if (c->controller_dpad_x != 0) {
            c->controller_dpad_x = 0;
        }
        if (c->controller_dpad_y != 0) {
            if (c->controller_menu_index < 0 || c->controller_menu_index >= c->menu_size) {
                c->controller_menu_index = c->menu_size - 1;
            }
            // Menu storage is bottom-to-top; visual Down therefore decrements the option index.
            c->controller_menu_index -= c->controller_dpad_y;
            if (c->controller_menu_index < 0) c->controller_menu_index = c->menu_size - 1;
            if (c->controller_menu_index >= c->menu_size) c->controller_menu_index = 0;
            c->controller_dpad_y = 0;
            c->controller_dpad_x = 0;
        }
        if (c->controller_confirm_pressed) {
            c->controller_confirm_pressed = false;
            int option = c->controller_menu_index;
            if (option < 0 || option >= c->menu_size) option = c->menu_size - 1;
            c->menu_visible = false;
            c->controller_menu_index = -1;
            if (c->menu_area == 1) c->redraw_sidebar = true;
            if (c->menu_area == 2) c->redraw_chatback = true;
            if (option >= 0) useMenuOption(c, option);
            return;
        }
    } else {
        c->controller_confirm_pressed = false;
    }

    if (c->controller_inventory_pressed) {
        c->controller_inventory_pressed = false;
        if (c->tab_interface_id[3] != -1) {
            c->redraw_sidebar = true;
            c->selected_tab = 3;
            c->redraw_sideicons = true;
            c->controller_grid_component = -1;
            c->controller_grid_slot = -1;
            c->controller_grid_screen_valid = false;
            c->controller_grid_analog_override = true;
        }
    }

    if (c->controller_snap_camera_pressed) {
        c->controller_snap_camera_pressed = false;
        if (c->local_player) {
            // Immediate snap rather than easing toward it - a deliberate, explicit "orient behind
            // me" reset should feel instant, not glide like the stick-driven rotation does.
            c->orbit_camera_yaw = c->local_player->pathing_entity.yaw & 0x7ff;
        }
    }

    if (c->controller_zoom_bias != 0) {
        // True zoom: R2 (+1) moves the orbit camera toward the player; L2 (-1) moves it away.
        // Pitch remains exclusively controlled by the right stick.
        c->controller_camera_zoom -= c->controller_zoom_bias * 12;
        if (c->controller_camera_zoom < -400) c->controller_camera_zoom = -400;
        if (c->controller_camera_zoom > 600) c->controller_camera_zoom = 600;
    }

    if (c->controller_start_pressed) {
        c->controller_start_pressed = false;
        if (c->virtual_keyboard_visible) {
            virtual_keyboard_close(c, true);
        } else if (c->ingame && c->chat_interface_id == -1 && !c->show_social_input && !c->chatback_input_open) {
            // "Press Start to type" - the PS2-equivalent of pressing any key to start typing,
            // since there's no physical keyboard to just start pressing.
            virtual_keyboard_maybe_open(c, 2);
        }
    }

    if (c->controller_back_pressed) {
        // Left unconsumed (and untouched) here while the keyboard is open - virtual_keyboard_
        // handle_input() consumes it itself, as Backspace, in that case. This guard makes the two
        // consumers order-independent regardless of which is called first each tick.
        if (!c->virtual_keyboard_visible) {
            c->controller_back_pressed = false;
            if (c->menu_visible) {
                c->menu_visible = false;
                c->controller_menu_index = -1;
            } else if (c->modal_message[0]) {
                c->modal_message[0] = '\0';
                c->redraw_chatback = true;
            } else if (c->show_social_input) {
                c->show_social_input = false;
                c->redraw_chatback = true;
            } else if (c->chatback_input_open) {
                c->chatback_input_open = false;
                c->redraw_chatback = true;
            }
        }
    }
}

// On-screen virtual keyboard - QWERTY, 6 rows (digits, QWERTYUIOP, ASDFGHJKL, ZXCVBNM, punctuation,
// function row). Letter rows store both cases explicitly (avoids pulling in <ctype.h> for two
// chars' worth of case-folding); digits/punctuation/function row are case-independent.
#define VKB_COLS 10
#define VKB_ROWS 6
#define VKB_CELL_W 28
#define VKB_CELL_H 20
#define VKB_FN_COUNT 5

static const char *const VKB_ROW_UPPER[VKB_ROWS - 1] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", ".,!?'-"};
static const char *const VKB_ROW_LOWER[VKB_ROWS - 1] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm", ".,!?'-"};
static const char *const VKB_FN_LABELS[VKB_FN_COUNT] = {"Shift", "Space", "Back", "Cancel", "Done"};

static int vkb_row_len(int row) {
    if (row == 5) {
        return VKB_FN_COUNT;
    }
    return (int)strlen(VKB_ROW_UPPER[row]);
}

static char vkb_row_char(Client *c, int row, int col) {
    const char *src = c->virtual_keyboard_shift ? VKB_ROW_UPPER[row] : VKB_ROW_LOWER[row];
    return src[col];
}

// Row 0/1 (10 cols) span the full grid width; shorter rows are centered within it; the function
// row (5 wide cells) also spans the full width. This mirrors a real staggered keyboard layout
// rather than a plain uniform grid.
static void vkb_cell_rect(Client *c, int row, int col, int *out_x, int *out_y, int *out_w, int *out_h) {
    int grid_w = VKB_COLS * VKB_CELL_W;
    int origin_x = c->shell->screen_width / 2 - grid_w / 2;
    int origin_y = c->shell->screen_height - VKB_ROWS * VKB_CELL_H - 10;

    if (row == 5) {
        int cell_w = grid_w / VKB_FN_COUNT;
        *out_x = origin_x + col * cell_w;
        *out_w = cell_w;
    } else {
        int n = vkb_row_len(row);
        int row_w = n * VKB_CELL_W;
        *out_x = origin_x + (grid_w - row_w) / 2 + col * VKB_CELL_W;
        *out_w = VKB_CELL_W;
    }
    *out_y = origin_y + row * VKB_CELL_H;
    *out_h = VKB_CELL_H;
}

static void vkb_commit_cell(Client *c, int row, int col) {
    if (row == 5) {
        switch (col) {
            case 0: // Shift
                c->virtual_keyboard_shift = !c->virtual_keyboard_shift;
                break;
            case 1: // Space
                key_pressed(c->shell, 0, ' ');
                break;
            case 2: // Backspace
                key_pressed(c->shell, 8, 8);
                break;
            case 3: // Cancel
                virtual_keyboard_close(c, false);
                break;
            case 4: // Done
                virtual_keyboard_close(c, true);
                break;
        }
        return;
    }

    char ch = vkb_row_char(c, row, col);
    key_pressed(c->shell, 0, (int)ch);
}


typedef struct {
    Component *grid;
    int x;
    int y;
    Component *scroll_owner;
    int clip_top;
    int clip_bottom;
    int best_distance;
    bool found;
} ControllerGridTarget;

static void controller_grid_search(Component *layer, int x, int y, int scroll_position,
                                   int mouse_x, int mouse_y, int wanted_id, bool require_point,
                                   Component *scroll_owner, int clip_top, int clip_bottom,
                                   ControllerGridTarget *best) {
    if (!layer || layer->type != TYPE_LAYER || !layer->childId) {
        return;
    }

    Component *owner = scroll_owner;
    int owner_top = clip_top;
    int owner_bottom = clip_bottom;
    if (layer->scroll > layer->height) {
        owner = layer;
        owner_top = y;
        owner_bottom = y + layer->height;
    }

    for (int i = 0; i < layer->childCount; i++) {
        Component *child = component_get(layer->childId[i]);
        if (!child) continue;

        int child_x = x + layer->childX[i] + child->x;
        int child_y = y + layer->childY[i] + child->y - scroll_position;

        if (child->type == TYPE_LAYER) {
            controller_grid_search(child, child_x, child_y, child->scrollPosition,
                                   mouse_x, mouse_y, wanted_id, require_point,
                                   owner, owner_top, owner_bottom, best);
            continue;
        }
        if (child->type != TYPE_INV || child->width <= 0 || child->height <= 0) {
            continue;
        }
        if (wanted_id >= 0 && child->id != wanted_id) {
            continue;
        }

        int cols = child->width;
        int rows = child->height;
        int min_x = child_x;
        int min_y = child_y;
        int max_x = child_x + cols * 32 + (cols - 1) * child->marginX;
        int max_y = child_y + rows * 32 + (rows - 1) * child->marginY;
        // The first twenty slots may have archive-defined offsets. Widen the hit envelope enough
        // to include those real slot rectangles instead of assuming a perfectly regular grid.
        int offset_slots = cols * rows;
        if (offset_slots > 20) offset_slots = 20;
        for (int slot = 0; slot < offset_slots; slot++) {
            int sx = child_x + (slot % cols) * (child->marginX + 32);
            int sy = child_y + (slot / cols) * (child->marginY + 32);
            if (child->invSlotOffsetX) sx += child->invSlotOffsetX[slot];
            if (child->invSlotOffsetY) sy += child->invSlotOffsetY[slot];
            if (sx < min_x) min_x = sx;
            if (sy < min_y) min_y = sy;
            if (sx + 32 > max_x) max_x = sx + 32;
            if (sy + 32 > max_y) max_y = sy + 32;
        }

        bool inside = mouse_x >= min_x && mouse_x < max_x && mouse_y >= min_y && mouse_y < max_y;
        if (require_point && !inside) continue;

        int dx = 0;
        int dy = 0;
        if (mouse_x < min_x) dx = min_x - mouse_x;
        else if (mouse_x >= max_x) dx = mouse_x - max_x + 1;
        if (mouse_y < min_y) dy = min_y - mouse_y;
        else if (mouse_y >= max_y) dy = mouse_y - max_y + 1;
        int distance = dx * dx + dy * dy;

        if (!best->found || distance < best->best_distance) {
            best->found = true;
            best->best_distance = distance;
            best->grid = child;
            best->x = child_x;
            best->y = child_y;
            best->scroll_owner = owner;
            best->clip_top = owner_top;
            best->clip_bottom = owner_bottom;
        }
    }
}

static bool controller_find_grid(Client *c, int wanted_id, bool require_point, ControllerGridTarget *out) {
    memset(out, 0, sizeof(*out));
    out->best_distance = 0x7fffffff;

    // If the cursor is already over a grid, respect that before applying modal/default priority.
    Component *viewport = c->viewport_interface_id != -1 ? component_get(c->viewport_interface_id) : NULL;
    Component *sidebar = NULL;
    if (c->sidebar_interface_id != -1) {
        sidebar = component_get(c->sidebar_interface_id);
    } else if (c->selected_tab >= 0 && c->selected_tab < 14 && c->tab_interface_id[c->selected_tab] != -1) {
        sidebar = component_get(c->tab_interface_id[c->selected_tab]);
    }
    Component *chat = c->chat_interface_id != -1 ? component_get(c->chat_interface_id) : NULL;

    if (viewport) controller_grid_search(viewport, 4, 4, 0, c->shell->mouse_x, c->shell->mouse_y,
                                         wanted_id, require_point, NULL, 4, 338, out);
    if (sidebar) controller_grid_search(sidebar, 553, 205, 0, c->shell->mouse_x, c->shell->mouse_y,
                                        wanted_id, require_point, NULL, 205, 466, out);
    if (chat) controller_grid_search(chat, 17, 357, 0, c->shell->mouse_x, c->shell->mouse_y,
                                     wanted_id, require_point, NULL, 357, 453, out);
    return out->found;
}

static bool controller_find_default_grid(Client *c, ControllerGridTarget *out) {
    // A viewport interface is modal gameplay UI (bank/shop/trade/etc.), so prefer its inventory
    // grid when the cursor is not already sitting on a particular grid. Otherwise use the sidebar.
    memset(out, 0, sizeof(*out));
    out->best_distance = 0x7fffffff;
    if (c->viewport_interface_id != -1) {
        Component *viewport = component_get(c->viewport_interface_id);
        if (viewport) {
            controller_grid_search(viewport, 4, 4, 0, c->shell->mouse_x, c->shell->mouse_y,
                                   -1, false, NULL, 4, 338, out);
            if (out->found) return true;
        }
    }

    Component *sidebar = NULL;
    if (c->sidebar_interface_id != -1) {
        sidebar = component_get(c->sidebar_interface_id);
    } else if (c->selected_tab >= 0 && c->selected_tab < 14 && c->tab_interface_id[c->selected_tab] != -1) {
        sidebar = component_get(c->tab_interface_id[c->selected_tab]);
    }
    if (sidebar) {
        controller_grid_search(sidebar, 553, 205, 0, c->shell->mouse_x, c->shell->mouse_y,
                               -1, false, NULL, 205, 466, out);
        if (out->found) return true;
    }

    if (c->chat_interface_id != -1) {
        Component *chat = component_get(c->chat_interface_id);
        if (chat) {
            controller_grid_search(chat, 17, 357, 0, c->shell->mouse_x, c->shell->mouse_y,
                                   -1, false, NULL, 357, 453, out);
        }
    }
    return out->found;
}

static void controller_grid_slot_center(ControllerGridTarget *target, int slot, int *cx, int *cy) {
    Component *grid = target->grid;
    int col = slot % grid->width;
    int row = slot / grid->width;
    int x = target->x + col * (grid->marginX + 32);
    int y = target->y + row * (grid->marginY + 32);
    if (slot < 20) {
        if (grid->invSlotOffsetX) x += grid->invSlotOffsetX[slot];
        if (grid->invSlotOffsetY) y += grid->invSlotOffsetY[slot];
    }
    *cx = x + 16;
    *cy = y + 16;
}

static int controller_grid_nearest_slot(ControllerGridTarget *target, int mouse_x, int mouse_y) {
    int count = target->grid->width * target->grid->height;
    int best_slot = 0;
    int best_distance = 0x7fffffff;
    for (int slot = 0; slot < count; slot++) {
        int cx, cy;
        controller_grid_slot_center(target, slot, &cx, &cy);
        int dx = cx - mouse_x;
        int dy = cy - mouse_y;
        int distance = dx * dx + dy * dy;
        if (distance < best_distance) {
            best_distance = distance;
            best_slot = slot;
        }
    }
    return best_slot;
}

static void handleControllerGridInput(Client *c) {
    if (c->controller_grid_analog_override) {
        c->controller_dpad_x = 0;
        c->controller_dpad_y = 0;
        return;
    }
    if (c->controller_dpad_x == 0 && c->controller_dpad_y == 0) return;
    if (c->virtual_keyboard_visible || c->controller_settings_visible || c->menu_visible) return;

    int dpad_x = c->controller_dpad_x;
    int dpad_y = c->controller_dpad_y;
    c->controller_dpad_x = 0;
    c->controller_dpad_y = 0;

    ControllerGridTarget target;
    bool active = c->controller_grid_component >= 0 &&
                  controller_find_grid(c, c->controller_grid_component, false, &target);
    if (!active) {
        // RuneScape's native hover pass already knows the exact TYPE_INV parent when the pointer
        // starts on a slot. Prefer that authoritative component before doing our generic search.
        bool found = false;
        if (c->hoveredSlotParentId >= 0) {
            found = controller_find_grid(c, c->hoveredSlotParentId, true, &target);
        }
        if (!found) {
            found = controller_find_grid(c, -1, true, &target);
        }
        if (!found) {
            found = controller_find_default_grid(c, &target);
        }
        if (!found) {
            c->controller_grid_component = -1;
            c->controller_grid_slot = -1;
            c->controller_grid_screen_valid = false;
            return;
        }
        if (!c->controller_free_cursor_valid) {
            c->controller_free_cursor_x = c->shell->mouse_x;
            c->controller_free_cursor_y = c->shell->mouse_y;
            c->controller_free_cursor_valid = true;
        }
        c->controller_grid_component = target.grid->id;
        c->controller_grid_slot = controller_grid_nearest_slot(&target, c->shell->mouse_x, c->shell->mouse_y);
    }

    Component *grid = target.grid;
    int count = grid->width * grid->height;
    if (count <= 0) return;
    int slot = c->controller_grid_slot;
    if (slot < 0 || slot >= count) {
        slot = controller_grid_nearest_slot(&target, c->shell->mouse_x, c->shell->mouse_y);
    }

    int col = slot % grid->width;
    int row = slot / grid->width;
    col += dpad_x;
    row += dpad_y;
    if (col < 0) col = 0;
    if (col >= grid->width) col = grid->width - 1;
    if (row < 0) row = 0;
    if (row >= grid->height) row = grid->height - 1;
    slot = row * grid->width + col;
    if (slot >= count) slot = count - 1;
    c->controller_grid_slot = slot;
    // The interface renderer will publish the authoritative on-screen center on the next draw.
    c->controller_grid_screen_valid = false;

    int cx, cy;
    controller_grid_slot_center(&target, slot, &cx, &cy);

    // Auto-scroll the containing bank/shop layer when D-pad navigation reaches a slot just outside
    // its visible clip. Re-find geometry afterward because changing scrollPosition moves the grid.
    if (target.scroll_owner) {
        int desired = cy;
        int top = target.clip_top + 16;
        int bottom = target.clip_bottom - 16;
        if (cy < top) desired = top;
        if (cy > bottom) desired = bottom;
        if (desired != cy) {
            target.scroll_owner->scrollPosition += cy - desired;
            int max_scroll = target.scroll_owner->scroll - target.scroll_owner->height;
            if (target.scroll_owner->scrollPosition < 0) target.scroll_owner->scrollPosition = 0;
            if (target.scroll_owner->scrollPosition > max_scroll) target.scroll_owner->scrollPosition = max_scroll;
            if (controller_find_grid(c, c->controller_grid_component, false, &target)) {
                controller_grid_slot_center(&target, slot, &cx, &cy);
            }
            c->redraw_sidebar = true;
            c->redraw_chatback = true;
        }
    }

    c->shell->mouse_x = MAX(0, MIN(SCREEN_WIDTH - 1, cx));
    c->shell->mouse_y = MAX(0, MIN(SCREEN_HEIGHT - 1, cy));
    c->shell->idle_cycles = 0;

    // Sidebar/chatback are retained PixMaps and otherwise may not redraw just because the logical
    // controller selection moved. Dirty the owning panel so the slot highlight visibly follows D-pad.
    if (cx >= 553 && cx < 743 && cy >= 205 && cy < 466) {
        c->redraw_sidebar = true;
    } else if (cx >= 17 && cx < 496 && cy >= 357 && cy < 453) {
        c->redraw_chatback = true;
    }

    // Refresh RuneScape's native menu/hover state immediately at the snapped slot. Cross and Circle
    // therefore operate on the new item even if the player presses them before the next rendered frame.
    client_handle_input(c);
}

static void virtual_keyboard_maybe_open(Client *c, int target) {
    if (c->shell->has_keyboard) {
        return;
    }
    c->virtual_keyboard_visible = true;
    c->virtual_keyboard_target = target;
    c->virtual_keyboard_cursor_row = 0;
    c->virtual_keyboard_cursor_col = 0;
    c->virtual_keyboard_shift = false;

    // Warp the shared stick-cursor onto cell (0,0) too - virtual_keyboard_handle_input()'s hover
    // tracking runs every tick and will otherwise immediately overwrite cursor_row/col above from
    // wherever shell->mouse_x/mouse_y was last left (e.g. mid-viewport from walking around before
    // the keyboard opened), which can coincidentally fall inside a totally unrelated grid cell and
    // silently mis-highlight it the instant the overlay appears.
    int x, y, w, h;
    vkb_cell_rect(c, 0, 0, &x, &y, &w, &h);
    c->shell->mouse_x = x + w / 2;
    c->shell->mouse_y = y + h / 2;
}

static void virtual_keyboard_close(Client *c, bool submit) {
    if (submit) {
        switch (c->virtual_keyboard_target) {
            case 0:
                // Username done - advance to the password field the same way physical Tab/Enter
                // does in client_update_title()'s own key handling, and keep the keyboard open.
                key_pressed(c->shell, 9, 9);
                c->virtual_keyboard_target = 1;
                c->virtual_keyboard_shift = false;
                return;
            case 1:
                // The login screen has no physical "submit" key - Tab/Enter there only toggle
                // fields (see client_update_title()) - so mirror the Login button's click path
                // directly. client_login() force-closes the keyboard itself on entry.
                client_login(c, c->username, c->password, false);
                break;
            default:
                // Chat/social/chatback all already treat Enter as submit-and-clear.
                key_pressed(c->shell, 13, 13);
                break;
        }
    } else if (c->show_social_input) {
        // Cancelling has to back out of the field it was covering too, the same way Triangle
        // does when the keyboard isn't open - otherwise the overlay disappears but the field
        // stays open with no other way to close it.
        c->show_social_input = false;
        c->redraw_chatback = true;
    } else if (c->chatback_input_open) {
        c->chatback_input_open = false;
        c->redraw_chatback = true;
    }

    c->virtual_keyboard_visible = false;
    c->virtual_keyboard_shift = false;

    // The keyboard panel is static (bottom of the screen) and fully repaints itself every frame
    // while open, so it never trails on its own - but the moment it closes, nothing else is going
    // to redraw that region on its own initiative (sidebar/chatback/background chrome only redraw
    // when their own dirty flags are set), so the last-drawn keyboard image would otherwise stay
    // permanently baked into the screen. Force one full non-viewport redraw to clear it - a
    // one-time cost on close, not a per-frame one. (No-op while not in-game/not visible; the title
    // screen already redraws everything unconditionally every frame on its own.)
    c->redraw_background = true;
}

static void virtual_keyboard_handle_input(Client *c) {
    if (!c->virtual_keyboard_visible) {
        c->controller_keyboard_confirm_pressed = false;
        return;
    }

    int dpad_x = c->controller_dpad_x;
    int dpad_y = c->controller_dpad_y;
    c->controller_dpad_x = 0;
    c->controller_dpad_y = 0;
    bool confirm = c->controller_keyboard_confirm_pressed;
    c->controller_keyboard_confirm_pressed = false;

    // The left-stick-driven cursor (shell->mouse_x/mouse_y) hovers a cell just by being inside
    // it - converges on the same cursor_row/cursor_col the D-pad drives, so either input method
    // works interchangeably for both highlighting and (via controller_keyboard_confirm_pressed)
    // committing.
    for (int row = 0; row < VKB_ROWS; row++) {
        int len = vkb_row_len(row);
        for (int col = 0; col < len; col++) {
            int x, y, w, h;
            vkb_cell_rect(c, row, col, &x, &y, &w, &h);
            if (c->shell->mouse_x >= x && c->shell->mouse_x < x + w && c->shell->mouse_y >= y && c->shell->mouse_y < y + h) {
                c->virtual_keyboard_cursor_row = row;
                c->virtual_keyboard_cursor_col = col;
            }
        }
    }

    if (dpad_y != 0) {
        int row = c->virtual_keyboard_cursor_row + dpad_y;
        if (row < 0) {
            row = 0;
        }
        if (row >= VKB_ROWS) {
            row = VKB_ROWS - 1;
        }
        c->virtual_keyboard_cursor_row = row;
        int len = vkb_row_len(row);
        if (c->virtual_keyboard_cursor_col >= len) {
            c->virtual_keyboard_cursor_col = len - 1;
        }
    }

    if (dpad_x != 0) {
        int len = vkb_row_len(c->virtual_keyboard_cursor_row);
        int col = c->virtual_keyboard_cursor_col + dpad_x;
        if (col < 0) {
            col = 0;
        }
        if (col >= len) {
            col = len - 1;
        }
        c->virtual_keyboard_cursor_col = col;
    }

    // Backspace here (not in handleControllerButtonInput()) - that function deliberately leaves
    // controller_back_pressed unconsumed while the keyboard is visible for exactly this purpose.
    if (c->controller_back_pressed) {
        c->controller_back_pressed = false;
        key_pressed(c->shell, 8, 8);
    }

    if (confirm) {
        vkb_commit_cell(c, c->virtual_keyboard_cursor_row, c->virtual_keyboard_cursor_col);
    }
}

// pix2d_fill_rect()/pix2d_hline()/etc. all write into whatever buffer is currently pix2d_bind()-ed
// (a single global draw target, _Pix2D - see pix2d.c). client_draw_scene() (world3d_draw() and
// friends) relies on that staying bound to c->area_viewport CONTINUOUSLY ACROSS FRAMES - it never
// re-binds itself before drawing. Any temporary rebind elsewhere must save and restore the exact
// previous binding (matching the established pattern in objtype.c's icon builder), or the very
// next frame's 3D scene render silently writes into the wrong buffer - confirmed as the actual
// cause of a real regression here (screen frozen except for whatever drew into its own buffer).
typedef struct {
    int *pixels, width, height, left, top, right, bottom;
} Pix2DBinding;

static Pix2DBinding pix2d_save_binding(void) {
    Pix2DBinding saved = {_Pix2D.pixels, _Pix2D.width, _Pix2D.height, _Pix2D.left, _Pix2D.top, _Pix2D.right, _Pix2D.bottom};
    return saved;
}

static void pix2d_restore_binding(Pix2DBinding saved) {
    pix2d_bind(saved.width, saved.height, saved.pixels);
    pix2d_set_clipping(saved.bottom, saved.right, saved.top, saved.left);
}

static void virtual_keyboard_draw(Client *c) {
    // Every visible panel in this engine (sidebar, chatback, the login screen text) composites
    // onto the real screen by binding its own dedicated PixMap, drawing into it, then calling
    // pixmap_draw() -> platform_blit_surface(). Do the same here: a panel spanning the full screen
    // width (so vkb_cell_rect()'s absolute x already lines up as local x with no translation
    // needed) but only as tall as the keyboard itself.
    Pix2DBinding saved = pix2d_save_binding();

    static PixMap *vkb_panel = NULL;
    int panel_h = VKB_ROWS * VKB_CELL_H;
    if (!vkb_panel) {
        vkb_panel = pixmap_new(c->shell->screen_width, panel_h);
    }
    pixmap_bind(vkb_panel);
    int origin_y = c->shell->screen_height - panel_h - 10;

    for (int row = 0; row < VKB_ROWS; row++) {
        int len = vkb_row_len(row);
        for (int col = 0; col < len; col++) {
            int x, y, w, h;
            vkb_cell_rect(c, row, col, &x, &y, &w, &h);
            int local_y = y - origin_y;
            bool highlighted = row == c->virtual_keyboard_cursor_row && col == c->virtual_keyboard_cursor_col;

            pix2d_fill_rect(x, local_y, BLACK, w, h);
            pix2d_draw_rect(x, local_y, highlighted ? YELLOW : WHITE, w, h);

            char label[8];
            if (row == 5) {
                strncpy(label, VKB_FN_LABELS[col], sizeof(label) - 1);
                label[sizeof(label) - 1] = '\0';
            } else {
                label[0] = vkb_row_char(c, row, col);
                label[1] = '\0';
            }
            drawStringCenter(c->font_plain12, x + w / 2, local_y + h / 2 + 4, label, highlighted ? YELLOW : WHITE);
        }
    }

    pixmap_draw(vkb_panel, 0, origin_y);

    pix2d_restore_binding(saved);
}

// Only reachable on a platform with no native pointer of its own (currently just PS2, same
// has_keyboard flag the virtual keyboard already gates on - see gameshell.h) - every other
// platform relies on the host OS's own mouse cursor and would get a confusing duplicate cursor
// drawn on top of it here. Without this, the stick-driven shell->mouse_x/mouse_y cursor used for
// every click in the game (menus, the login screen, the virtual keyboard itself) is completely
// invisible, making it guesswork to click anything precisely.
static void virtual_cursor_draw(Client *c) {
    // Same reasoning as virtual_keyboard_draw() above (see pix2d_save_binding()'s comment) -
    // composite through a small dedicated PixMap, and save/restore the binding around it so
    // client_draw_scene()'s next frame still finds _Pix2D pointed at c->area_viewport.
    Pix2DBinding saved = pix2d_save_binding();

    static PixMap *cursor_panel = NULL;
    const int SIZE = 13;
    if (!cursor_panel) {
        cursor_panel = pixmap_new(SIZE, SIZE);
    }

    int cursor_x = c->shell->mouse_x;
    int cursor_y = c->shell->mouse_y;
#ifdef __PS2__
    if (!c->controller_grid_analog_override &&
        c->controller_grid_screen_valid && c->controller_grid_component >= 0) {
        cursor_x = c->controller_grid_screen_x;
        cursor_y = c->controller_grid_screen_y;
        // Do not write shell->mouse_x/y here. The grid click path locks those coordinates only
        // while processing input; drawing the cursor must never mutate the analog cursor position.
    }
#endif
    int x = cursor_x - SIZE / 2;
    int y = cursor_y - SIZE / 2;

#ifdef __PS2__
    // Restore whatever the cursor covered last frame BEFORE drawing it at the new position -
    // platform_blit_surface() never clears anything, it only overwrites, so without this every
    // panel that doesn't happen to redraw itself this frame keeps yesterday's cursor mark baked in
    // forever (this is what "cursor leaves a trail" actually was).
    static uint16_t backing[13 * 13];
    static bool backing_valid = false;
    static int last_x, last_y;
    if (backing_valid) {
        platform_restore_region(last_x, last_y, SIZE, SIZE, backing);
    }
    platform_save_region(x, y, SIZE, SIZE, backing);
    backing_valid = true;
    last_x = x;
    last_y = y;
#endif

    pixmap_bind(cursor_panel);
    // Black-outlined white crosshair filling the whole panel (13px cross, then a 9px cross on
    // top) so there's always a 1px dark border - keeps the hotspot visible against both light and
    // dark UI panels.
    pix2d_fill_rect(0, 0, BLACK, SIZE, SIZE);
    pix2d_hline(0, 6, BLACK, 13);
    pix2d_vline(6, 0, BLACK, 13);
    pix2d_hline(2, 6, WHITE, 9);
    pix2d_vline(6, 2, WHITE, 9);
    pixmap_draw(cursor_panel, x, y);

    pix2d_restore_binding(saved);
}

static void handleChatSettingsInput(Client *c) {
    if (c->shell->mouse_click_button != 1) {
        return;
    }

    if (c->shell->mouse_click_x >= 6 && c->shell->mouse_click_x <= 106 && c->shell->mouse_click_y >= 467 && c->shell->mouse_click_y <= 499) {
        c->public_chat_setting = (c->public_chat_setting + 1) % 4;
        c->redraw_privacy_settings = true;
        c->redraw_chatback = true;

        // CHAT_SETMODE
        p1isaac(c->out, 129); // CHAT_SETMODE
        p1(c->out, c->public_chat_setting);
        p1(c->out, c->private_chat_setting);
        p1(c->out, c->trade_chat_setting);
    } else if (c->shell->mouse_click_x >= 135 && c->shell->mouse_click_x <= 235 && c->shell->mouse_click_y >= 467 && c->shell->mouse_click_y <= 499) {
        c->private_chat_setting = (c->private_chat_setting + 1) % 3;
        c->redraw_privacy_settings = true;
        c->redraw_chatback = true;

        // CHAT_SETMODE
        p1isaac(c->out, 129); // CHAT_SETMODE
        p1(c->out, c->public_chat_setting);
        p1(c->out, c->private_chat_setting);
        p1(c->out, c->trade_chat_setting);
    } else if (c->shell->mouse_click_x >= 273 && c->shell->mouse_click_x <= 373 && c->shell->mouse_click_y >= 467 && c->shell->mouse_click_y <= 499) {
        c->trade_chat_setting = (c->trade_chat_setting + 1) % 3;
        c->redraw_privacy_settings = true;
        c->redraw_chatback = true;

        // CHAT_SETMODE
        p1isaac(c->out, 129); // CHAT_SETMODE
        p1(c->out, c->public_chat_setting);
        p1(c->out, c->private_chat_setting);
        p1(c->out, c->trade_chat_setting);
    } else if (c->shell->mouse_click_x >= 412 && c->shell->mouse_click_x <= 512 && c->shell->mouse_click_y >= 467 && c->shell->mouse_click_y <= 499) {
        closeInterfaces(c);

        c->reportAbuseInput[0] = '\0';
        c->reportAbuseMuteOption = false;

        Component *report = component_find_by_client_code(600);
        if (report) {
            c->reportAbuseInterfaceID = c->viewport_interface_id = report->layer;
            return;
        }
    }
}

static void client_scenemap_free(Client *c) {
    for (int i = 0; i < c->sceneMapIndexLength; i++) {
        free(c->sceneMapLandData[i]);
        free(c->sceneMapLocData[i]);
    }
    free(c->sceneMapLandData);
    free(c->sceneMapLocData);
    free(c->sceneMapIndex);
    free(c->sceneMapLandDataIndexLength);
    free(c->sceneMapLocDataIndexLength);
}

void client_update_game(Client *c) {
#ifdef __PS2__
    ps2_live_update_count++;
    ps2_live_stage = 1; // entered game update
    ps2_heap_tick_begin_kb = mallinfo().fordblks / 1024;
    // Present an explicit marker before each major section of only the first
    // three live updates.  If one section blocks, its marker remains visible
    // on the TV.  This is a temporary bisection aid, deliberately bounded so
    // it cannot become a permanent per-tick GS cost.
    // Phase checkpoints were useful for bisection but each one performs a
    // full GS presentation.  Keep them disabled in the normal hardware build.
    static int runtime_trace_updates_left = 0;
    const bool runtime_trace = c->scene_state == 2 && runtime_trace_updates_left > 0;
#define PS2_RUNTIME_TRACE(stage) \
    do {                          \
        if (runtime_trace) {      \
            ps2_runtime_checkpoint(c, stage); \
        }                         \
    } while (0)
#else
#define PS2_RUNTIME_TRACE(stage) do { } while (0)
#endif
    if (c->system_update_timer > 1) {
        c->system_update_timer--;
    }

    if (c->idle_timeout > 0) {
        c->idle_timeout--;
    }

#ifdef __PS2__
    int64_t phase_t0 = rs2_now();
#endif
    PS2_RUNTIME_TRACE("NET");
    // A five-packet burst can contain enough scene/interface work to monopolise
    // an EE tick immediately after login.  Keep the game responsive and let the
    // normal 50 Hz update loop drain it progressively on the 32 MiB target.
#ifdef __PS2__
    // During map construction one packet at a time prevents a burst of interface/zone work from
    // monopolising the EE. Once the world is live, use the established three-packet budget, but
    // keep REBUILD_NORMAL + its immediately-following PLAYER_INFO atomic. A hard teleport shifts
    // every entity into the new local coordinate base in REBUILD_NORMAL; if that packet occupies
    // the last normal budget slot, running the rest of a PS2 tick before PLAYER_INFO leaves the
    // local player temporarily far outside the 104x104 scene. Tutorial-skip is a reproducible case
    // because its interface/varp traffic can place REBUILD_NORMAL at that boundary. Grant exactly
    // one extra read only when a live scene changes 2 -> 1 during this drain. Initial login remains
    // one packet per tick and ordinary live traffic remains capped at three.
    int packet_budget = c->scene_state == 2 ? 3 : 1;
    for (int i = 0; i < packet_budget; i++) {
        int scene_state_before = c->scene_state;
        if (!client_read(c)) {
            break;
        }
        if (scene_state_before == 2 && c->scene_state == 1 && packet_budget == 3) {
            packet_budget = 4;
        }
    }
#else
    const int packet_budget = 5;
    for (int i = 0; i < packet_budget && client_read(c); i++) {
    }
#endif
#ifdef __PS2__
    ps2_live_stage = 2; // packet handling returned
    ps2_heap_after_packets_kb = mallinfo().fordblks / 1024;
    _TickPhase.packets_ms += rs2_now() - phase_t0;
#endif

    #ifdef __PS2__
    ps2_live_stage = 3; // entering non-network game logic
    #endif
    if (c->ingame) {
        for (int wave = 0; wave < c->wave_count; wave++) {
            if (c->wave_delay[wave] <= 0) {
                // deprecated code unused to save wav for the browser to play it
                // bool failed = false;
                // try {
                // if (c->wave_ids[wave] != c->last_wave_id || c->wave_loops[wave] != c->last_wave_loops) {
                Packet *buf = wave_generate(c->wave_ids[wave], c->wave_loops[wave]);

                if (rs2_now() + (uint64_t)(buf->pos / 22) > c->last_wave_start_time + (uint64_t)(c->last_wave_length / 22)) {
                    c->last_wave_length = buf->pos;
                    c->last_wave_start_time = rs2_now();
                    // if (c->saveWave(buf->data, buf->pos)) {
                    c->last_wave_id = c->wave_ids[wave];
                    c->last_wave_loops = c->wave_loops[wave];
                    platform_play_wave(buf->data, buf->pos);
                    // } else {
                    // 	failed = true;
                    // }
                }
                // } else if (!c->replayWave()) {
                // 	failed = true;
                // }
                // } catch (Exception ignored) {
                // }

                // if (failed && c->wave_delay[wave] != -5) {
                // 	c->wave_delay[wave] = -5;
                // } else {
                c->wave_count--;
                for (int i = wave; i < c->wave_count; i++) {
                    c->wave_ids[i] = c->wave_ids[i + 1];
                    c->wave_loops[i] = c->wave_loops[i + 1];
                    c->wave_delay[i] = c->wave_delay[i + 1];
                }
                wave--;
                // }
            } else {
                c->wave_delay[wave]--;
            }
        }

        if (c->nextMusicDelay > 0) {
            c->nextMusicDelay -= 20;
            if (c->nextMusicDelay < 0) {
                c->nextMusicDelay = 0;
            }
            if (c->nextMusicDelay == 0 && c->midiActive && !_Client.lowmem) {
                platform_set_midi(c->currentMidi, c->midiCrc, c->midiSize);
            }
        }

        Packet *tracking = inputtracking_flush(&_InputTracking);
        if (tracking) {
            // EVENT_TRACKING
            p1isaac(c->out, 142); // EVENT_TRACKING
            p2(c->out, tracking->pos);
            pdata(c->out, tracking->data, tracking->pos, 0);
            packet_release(tracking);
        }

        c->idle_net_cycles++;
        if (c->idle_net_cycles > 750) {
            client_try_reconnect(c);
        }

#ifdef __PS2__
        PS2_RUNTIME_TRACE("ENT");
        phase_t0 = rs2_now();
        updatePlayers(c);
        _TickPhase.players_ms += rs2_now() - phase_t0;
        phase_t0 = rs2_now();
        updateNpcs(c);
        _TickPhase.npcs_ms += rs2_now() - phase_t0;
        phase_t0 = rs2_now();
        updateEntityChats(c);
        _TickPhase.chats_ms += rs2_now() - phase_t0;
        phase_t0 = rs2_now();
        updateMergeLocs(c);
        _TickPhase.mergelocs_ms += rs2_now() - phase_t0;
#else
        updatePlayers(c);
        updateNpcs(c);
        updateEntityChats(c);
        updateMergeLocs(c);
#endif

        if ((c->shell->action_key[1] == 1 || c->shell->action_key[2] == 1 || c->shell->action_key[3] == 1 || c->shell->action_key[4] == 1) && c->camera_moved_write++ > 5) {
            c->camera_moved_write = 0;
            // EVENT_CAMERA_POSITION
            p1isaac(c->out, 91); // EVENT_CAMERA_POSITION
            p2(c->out, c->orbit_camera_pitch);
            p2(c->out, c->orbit_camera_yaw);
        }

        c->scene_delta++;
        if (c->cross_mode != 0) {
            c->cross_cycle += 20;
            if (c->cross_cycle >= 400) {
                c->cross_mode = 0;
            }
        }

        if (c->selected_area != 0) {
            c->selected_cycle++;
            if (c->selected_cycle >= 15) {
                if (c->selected_area == 2) {
                    c->redraw_sidebar = true;
                }
                if (c->selected_area == 3) {
                    c->redraw_chatback = true;
                }
                c->selected_area = 0;
            }
        }

        if (c->obj_drag_area != 0) {
            c->obj_drag_cycles++;
            if (c->shell->mouse_x > c->objGrabX + 5 || c->shell->mouse_x < c->objGrabX - 5 || c->shell->mouse_y > c->objGrabY + 5 || c->shell->mouse_y < c->objGrabY - 5) {
                c->objGrabThreshold = true;
            }

            if (c->shell->mouse_button == 0) {
                if (c->obj_drag_area == 2) {
                    c->redraw_sidebar = true;
                }
                if (c->obj_drag_area == 3) {
                    c->redraw_chatback = true;
                }

                c->obj_drag_area = 0;
                if (c->objGrabThreshold && c->obj_drag_cycles >= 5) {
                    c->hoveredSlotParentId = -1;
                    client_handle_input(c);
                    if (c->hoveredSlotParentId == c->objDragInterfaceId && c->hoveredSlot != c->objDragSlot) {
                        Component *com = component_get(c->objDragInterfaceId);
                        int obj = com->invSlotObjId[c->hoveredSlot];
                        com->invSlotObjId[c->hoveredSlot] = com->invSlotObjId[c->objDragSlot];
                        com->invSlotObjId[c->objDragSlot] = obj;

                        int count = com->invSlotObjCount[c->hoveredSlot];
                        com->invSlotObjCount[c->hoveredSlot] = com->invSlotObjCount[c->objDragSlot];
                        com->invSlotObjCount[c->objDragSlot] = count;

                        // INV_BUTTOND
                        p1isaac(c->out, 176); // INV_BUTTOND
                        p2(c->out, c->objDragInterfaceId);
                        p2(c->out, c->objDragSlot);
                        p2(c->out, c->hoveredSlot);
                        p1(c->out, 0); // TODO: rev254 bank-arrange-mode byte; always 0 until bank rearrange mode is implemented, see audit
                    }
                } else if ((c->mouseButtonsOption == 1 || isAddFriendOption(c, c->menu_size - 1)) && c->menu_size > 2) {
                    showContextMenu(c);
                } else if (c->menu_size > 0) {
                    useMenuOption(c, c->menu_size - 1);
                }

                c->selected_cycle = 10;
                c->shell->mouse_click_button = 0;
            }
        }

        _Client.cyclelogic3++;
        if (_Client.cyclelogic3 > 127) {
            _Client.cyclelogic3 = 0;
            // ANTICHEAT_CYCLELOGIC3
            p1isaac(c->out, 4); // ANTICHEAT_CYCLELOGIC3
            p1(c->out, 50);
        }

        if (_World3D.clickTileX != -1) {
            int x = _World3D.clickTileX;
            int z = _World3D.clickTileZ;
            bool success = client_try_move(c, c->local_player->pathing_entity.pathTileX[0], c->local_player->pathing_entity.pathTileZ[0], x, z, 0, 0, 0, 0, 0, 0, true);
            _World3D.clickTileX = -1;

            if (success) {
                c->crossX = c->shell->mouse_click_x;
                c->crossY = c->shell->mouse_click_y;
                c->cross_mode = 1;
                c->cross_cycle = 0;
            }
        }

        if (c->shell->mouse_click_button == 1 && c->modal_message[0]) {
            c->modal_message[0] = '\0';
            c->redraw_chatback = true;
            c->shell->mouse_click_button = 0;
        }

        PS2_RUNTIME_TRACE("INPUT");
        handleMouseInput(c);
        handleMinimapInput(c);
        handleTabInput(c);
        handleControllerTabInput(c);
        handleControllerButtonInput(c);
        handleControllerGridInput(c);
        handleChatSettingsInput(c);

        if (c->shell->mouse_button == 1 || c->shell->mouse_click_button == 1) {
            c->drag_cycles++;
        }

        if (c->scene_state == 2) {
            // NOTE unused
            // if (_Custom.camera_editor) {
            //     update_camera_editor(c);
            // } else {
            client_update_orbit_camera(c);
            // }
        }
        if (c->scene_state == 2 && c->cutscene) {
            applyCutscene(c);
        }

        for (int i = 0; i < 5; i++) {
            c->cameraModifierCycle[i]++;
        }

        handleInputKey(c);
        c->shell->idle_cycles++;
        if (c->shell->idle_cycles > 4500) {
            c->idle_timeout = 250;
            c->shell->idle_cycles -= 500;
            // IDLE_TIMER
            p1isaac(c->out, 144); // IDLE_TIMER
        }

        c->cameraOffsetCycle++;
        if (c->cameraOffsetCycle > 500) {
            c->cameraOffsetCycle = 0;
            int _rand = (int)(jrand() * 8.0);
            if ((_rand & 0x1) == 1) {
                c->camera_anticheat_offset_x += c->cameraOffsetXModifier;
            }
            if ((_rand & 0x2) == 2) {
                c->camera_anticheat_offset_z += c->cameraOffsetZModifier;
            }
            if ((_rand & 0x4) == 4) {
                c->camera_anticheat_angle += c->cameraOffsetYawModifier;
            }
        }

        if (c->camera_anticheat_offset_x < -50) {
            c->cameraOffsetXModifier = 2;
        }
        if (c->camera_anticheat_offset_x > 50) {
            c->cameraOffsetXModifier = -2;
        }
        if (c->camera_anticheat_offset_z < -55) {
            c->cameraOffsetZModifier = 2;
        }
        if (c->camera_anticheat_offset_z > 55) {
            c->cameraOffsetZModifier = -2;
        }
        if (c->camera_anticheat_angle < -40) {
            c->cameraOffsetYawModifier = 1;
        }
        if (c->camera_anticheat_angle > 40) {
            c->cameraOffsetYawModifier = -1;
        }

        c->cameraOffsetCycle++;
        if (c->cameraOffsetCycle > 500) {
            c->cameraOffsetCycle = 0;
            int random = (int)(jrand() * 8.0);
            if ((random & 0x1) == 1) {
                c->minimap_anticheat_angle += c->minimapAngleModifier;
            }
            if ((random & 0x2) == 2) {
                c->minimap_zoom += c->minimapZoomModifier;
            }
        }

        if (c->minimap_anticheat_angle < -60) {
            c->minimapAngleModifier = 2;
        }
        if (c->minimap_anticheat_angle > 60) {
            c->minimapAngleModifier = -2;
        }

        if (c->minimap_zoom < -20) {
            c->minimapZoomModifier = 1;
        }
        if (c->minimap_zoom > 10) {
            c->minimapZoomModifier = -1;
        }

        _Client.cyclelogic4++;
        if (_Client.cyclelogic4 > 110) {
            _Client.cyclelogic4 = 0;
            // ANTICHEAT_CYCLELOGIC4
            p1isaac(c->out, 226); // ANTICHEAT_CYCLELOGIC4
            p1(c->out, 232);
        }

        c->heartbeatTimer++;
        if (c->heartbeatTimer > 50) {
            // NO_TIMEOUT
            p1isaac(c->out, 239); // NO_TIMEOUT
        }

        // try {
        if (c->stream && c->out->pos > 0) {
            int sent = clientstream_write(c->stream, c->out->data, c->out->pos, 0);
            if (sent > 0) {
                // A nonblocking socket may only accept a prefix.  Preserve
                // the unsent tail in order; dropping it would desynchronise
                // the revision-254 packet stream just as surely as a hang.
                if (sent < c->out->pos) {
                    memmove(c->out->data, c->out->data + sent, c->out->pos - sent);
                }
                c->out->pos -= sent;
                c->heartbeatTimer = 0;
            }
        }
        // NOTE: no catch for logout or reconn
        // } catch (IOException ignored) {
        // client_try_reconnect(c);
        // } catch (Exception ignored) {
        // client_logout(c);
        // }
    }
#ifdef __PS2__
    ps2_live_stage = 4; // game logic and outbound send returned
    ps2_heap_after_logic_kb = mallinfo().fordblks / 1024;
    if (runtime_trace) {
        ps2_runtime_checkpoint(c, "DONE");
        runtime_trace_updates_left--;
    }
#endif
#undef PS2_RUNTIME_TRACE
}

#ifdef __PS2__
// Real PS2 USB mass storage (see ps2.c's own USB driver history) racks up disproportionate FAT/
// directory-lookup overhead per discrete file open on slow media - REBUILD_NORMAL below opens up to
// ~18 separate map files per rebuild out of ~830 total in the full set, each a fresh fopen()/fseek/
// fread/fclose. maps.dat (built by scripts/pack_maps.cjs) combines all of them into one file with a
// small in-memory index, so a real session needs exactly one fopen() for the whole game instead of
// hundreds - after that, every map load is just a seek within an already-open handle. Falls back to
// the original one-file-per-mapsquare path below if maps.dat isn't present (e.g. not yet copied onto
// a given deployment), so this is a strict improvement, never a hard new requirement.
typedef struct {
    uint8_t kind;
    uint8_t mapsquareX;
    uint8_t mapsquareZ;
    uint32_t offset;
    uint32_t length;
} Ps2MapArchiveEntry;

static bool ps2_map_archive_checked = false;
static FILE *ps2_map_archive_file = NULL;
static Ps2MapArchiveEntry *ps2_map_archive_index = NULL;
static int ps2_map_archive_count = 0;

static bool ps2_map_archive_open(void) {
    if (ps2_map_archive_checked) {
        return ps2_map_archive_file != NULL;
    }
    ps2_map_archive_checked = true;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%srom/cache/client/maps.dat", ps2_cache_prefix());
    FILE *file = fopen(path, "rb");
    if (!file) {
        rs2_log("map: no combined %s - falling back to individual map files\n", path);
        return false;
    }

    char magic[4];
    uint32_t count;
    if (fread(magic, 1, 4, file) != 4 || memcmp(magic, "MAPZ", 4) != 0 || fread(&count, sizeof(count), 1, file) != 1) {
        rs2_error("map: %s has an invalid header - falling back to individual map files\n", path);
        fclose(file);
        return false;
    }

    Ps2MapArchiveEntry *index = malloc(count * sizeof(Ps2MapArchiveEntry));
    for (uint32_t i = 0; i < count; i++) {
        // Raw 12-byte little-endian records (see pack_maps.cjs) decoded by hand rather than a
        // direct struct fread - this project's own struct layout/padding/endianness isn't
        // guaranteed to match the packer's fixed on-disk format bit-for-bit.
        uint8_t raw[12];
        if (fread(raw, 1, sizeof(raw), file) != sizeof(raw)) {
            rs2_error("map: %s index truncated at entry %u - falling back to individual map files\n", path, i);
            free(index);
            fclose(file);
            return false;
        }
        index[i].kind = raw[0];
        index[i].mapsquareX = raw[1];
        index[i].mapsquareZ = raw[2];
        index[i].offset = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) | ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
        index[i].length = (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) | ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
    }

    ps2_map_archive_file = file;
    ps2_map_archive_index = index;
    ps2_map_archive_count = (int)count;
    rs2_log("map: opened combined %s, %d entries\n", path, ps2_map_archive_count);
    return true;
}

// Returns NULL (without logging) if the archive isn't open or doesn't contain this square, exactly
// like a cache miss - the caller (client_load_map_file()) treats that as "fall back to the
// individual-file path", not as an error in its own right.
static int8_t *ps2_map_archive_read(const char *kind, int mapsquareX, int mapsquareZ, int *out_size) {
    if (!ps2_map_archive_open()) {
        return NULL;
    }
    uint8_t want_kind = (uint8_t)kind[0];
    for (int i = 0; i < ps2_map_archive_count; i++) {
        Ps2MapArchiveEntry *entry = &ps2_map_archive_index[i];
        if (entry->kind != want_kind || entry->mapsquareX != mapsquareX || entry->mapsquareZ != mapsquareZ) {
            continue;
        }
        int8_t *data = malloc(entry->length);
        fseek(ps2_map_archive_file, (long)entry->offset, SEEK_SET);
        size_t got = fread(data, 1, entry->length, ps2_map_archive_file);
        if (got != entry->length) {
            // This used to log and then hand back `data` anyway, with *out_size still set to the
            // full declared entry->length - the caller (and eventually bzip_decompress(), fed this
            // buffer as compressed loc/land data) had no way to tell that everything past byte
            // `got` is uninitialized malloc() garbage, not real compressed bytes. Decompressing a
            // stream with a garbage tail is exactly how a truncated/incomplete real-hardware USB
            // read (this project's USB/BDM stack has its own documented history of flakiness) turns
            // into decompressor state that never finds a valid end-of-stream marker - the exact
            // condition that motivated hardening bzip_decompress()'s own EOF handling in
            // thirdparty/bzip.c (see the jmpbuf re-arm comment there). Treat a short read the same
            // as a missing file (return NULL) instead of returning a corrupt buffer dressed up as a
            // complete one.
            rs2_error("map: short read from maps.dat for %s%d_%d (got %zu of %u bytes) - treating as missing\n", kind,
                      mapsquareX, mapsquareZ, got, entry->length);
            free(data);
            return NULL;
        }
        // 2026-09-14: a FlushCache(INVALIDATE_DCACHE) call used to sit here, on the theory that this
        // buffer's D-cache lines could be stale relative to the IOP's SIF-DMA'd bytes. Tested on real
        // hardware and made things WORSE, not better - the freeze moved EARLIER, into this very read
        // loop (on the very first mapsquare), a stage that had completed cleanly in every prior test
        // without exception. Root cause of the regression: FlushCache() on PS2 has no address-range
        // parameter - it operates on the ENTIRE EE data cache, not just this buffer - and
        // INVALIDATE_DCACHE discards cache content WITHOUT writing it back first. Calling that
        // repeatedly in a tight loop risks silently dropping dirty (modified-but-unflushed) cache
        // lines belonging to completely unrelated live state (heap bookkeeping, other globals,
        // in-flight SIF RPC buffers) - a much bigger blast radius than "flush this one buffer".
        // Reverted. Don't re-attempt a bare INVALIDATE_DCACHE call in this loop without new evidence;
        // if cache coherency is ever revisited, it would need WRITEBACK_DCACHE first (or a
        // targeted/rare use, not per-file-read) to avoid this exact failure mode.
        *out_size = (int)entry->length;
        return data;
    }
    return NULL;
}
#endif

// Loads a "<kind><mapsquareX>_<mapsquareZ>" map file (kind is "m" for land or "l" for locs) from
// the platform cache path, mirroring the per-platform path conventions client_load()/load_archive()
// already use. Returns NULL (and logs) if the file is missing - there is no rev254 server-side
// fallback request for missing map data (see REBUILD_NORMAL below), so a missing file here means an
// incomplete local cache, not a recoverable network condition.
static int8_t *client_load_map_file(const char *kind, int mapsquareX, int mapsquareZ, int *out_size) {
#ifdef __PS2__
    int8_t *archived = ps2_map_archive_read(kind, mapsquareX, mapsquareZ, out_size);
    if (archived) {
        return archived;
    }
#endif
    char filename[PATH_MAX];
#ifdef _arch_dreamcast
    snprintf(filename, sizeof(filename), "cache/client/maps/%s%d_%d.", kind, mapsquareX, mapsquareZ);
#elif defined(NXDK)
    snprintf(filename, sizeof(filename), "D:\\cache\\client\\maps\\%s%d_%d", kind, mapsquareX, mapsquareZ);
#elif defined(__EMSCRIPTEN__)
    snprintf(filename, sizeof(filename), "%s%d_%d", kind, mapsquareX, mapsquareZ);
#elif defined(__PS2__)
    snprintf(filename, sizeof(filename), "%srom/cache/client/maps/%s%d_%d", ps2_cache_prefix(), kind, mapsquareX, mapsquareZ);
#else
    snprintf(filename, sizeof(filename), "rom/cache/client/maps/%s%d_%d", kind, mapsquareX, mapsquareZ);
#endif

#ifdef __PS2__
    // Logged BEFORE the open (not just on failure) so a real-hardware hang inside fopen() itself -
    // e.g. a slow/failing USB read retry on a missing file, a real, plausible failure mode for a
    // minimal FAT driver that this project doesn't control - still shows exactly which map square
    // it got stuck on, via boot.log (see ps2.c's ps2_log_to_file()), rather than nothing at all.
    rs2_log("map: opening %s\n", filename);
#endif
#if ANDROID
    SDL_RWops *file = SDL_RWFromFile(filename, "rb");
#else
    FILE *file = fopen(filename, "rb");
#endif
    if (!file) {
        rs2_error("Missing map data: %s\n", filename);
        return NULL;
    }
#ifdef __PS2__
    rs2_log("map: opened %s\n", filename);
#endif

    size_t size;
#ifdef ANDROID
    size = SDL_RWseek(file, 0, RW_SEEK_END);
    SDL_RWseek(file, 0, RW_SEEK_SET);
#else
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
#endif

    int8_t *data = malloc(size);
#ifdef ANDROID
    size_t got = SDL_RWread(file, data, 1, size);
#else
    size_t got = fread(data, 1, size, file);
#endif
    bool short_read = got != size;
    if (short_read) {
        rs2_error("Failed to read file: %s (%s, got %zu of %zu bytes)\n", filename, strerror(errno), got, size);
    }
#ifdef ANDROID
    SDL_RWclose(file);
#else
    fclose(file);
#endif

    if (short_read) {
        // See ps2_map_archive_read()'s comment above: this used to hand back `data` with *out_size
        // == the full stat()'d file size regardless of how many bytes fread actually delivered, so a
        // genuinely short/interrupted read fed a partially-uninitialized buffer into
        // bzip_decompress() as if it were a complete compressed stream. Report it the same way a
        // missing file already is - NULL - instead of silently handing corrupt data onward.
        free(data);
        return NULL;
    }

    // 2026-09-14: see ps2_map_archive_read()'s matching comment above - a FlushCache(INVALIDATE_DCACHE)
    // call used to sit here too and was reverted for the same reason (tested on real hardware,
    // regressed the freeze to happen earlier, right in this read loop). Don't re-add without new
    // evidence.

    *out_size = (int)size;
    return data;
}

// Shared with client_load_raw_file() below and (on PS2) the streaming ondemand.zip opener - keeps
// every caller's notion of "the platform's cache path" in exactly one place.
static void client_cache_file_path(const char *filename_only, char *out, size_t out_size) {
#ifdef _arch_dreamcast
    snprintf(out, out_size, "cache/client/%s", filename_only);
#elif defined(NXDK)
    snprintf(out, out_size, "D:\\cache\\client\\%s", filename_only);
#elif defined(__EMSCRIPTEN__)
    snprintf(out, out_size, "%s", filename_only);
#elif defined(__PS2__)
    snprintf(out, out_size, "%srom/cache/client/%s", ps2_cache_prefix(), filename_only);
#else
    snprintf(out, out_size, "rom/cache/client/%s", filename_only);
#endif
}

// Reads one whole file straight from the platform's cache path with no Jagfile/.jag-specific
// header parsing (unlike load_archive() above, which expects the classic bzip2 .jag container) -
// used for ondemand.zip on every platform except PS2 (see the PS2-specific streaming path at this
// function's call site in client_load()). Unused (and compiled out) on PS2 itself for that reason.
#ifndef __PS2__
static int8_t *client_load_raw_file(const char *filename_only, int *out_size) {
    char filename[PATH_MAX];
    client_cache_file_path(filename_only, filename, sizeof(filename));

#if ANDROID
    SDL_RWops *file = SDL_RWFromFile(filename, "rb");
#else
    FILE *file = fopen(filename, "rb");
#endif
    if (!file) {
        rs2_error("Missing file: %s\n", filename);
        return NULL;
    }

    size_t size;
#ifdef ANDROID
    size = SDL_RWseek(file, 0, RW_SEEK_END);
    SDL_RWseek(file, 0, RW_SEEK_SET);
#else
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
#endif

    int8_t *data = malloc(size);
#ifdef ANDROID
    size_t got = SDL_RWread(file, data, 1, size);
#else
    size_t got = fread(data, 1, size, file);
#endif
    bool short_read = got != size;
    if (short_read) {
        rs2_error("Failed to read file: %s (%s, got %zu of %zu bytes)\n", filename, strerror(errno), got, size);
    }
#ifdef ANDROID
    SDL_RWclose(file);
#else
    fclose(file);
#endif

    if (short_read) {
        // Same bug class as client_load_map_file()'s equivalent fix above: don't hand back a
        // buffer claiming the full file size when a short read left its tail uninitialized.
        free(data);
        return NULL;
    }

    *out_size = (int)size;
    return data;
}
#endif

// Several IF_SET* server packets carry a raw component id straight from the network. rev254's
// Progressive server can reference interfaces/components Client3 hasn't instantiated locally (or
// ids past the loaded interface archive's range), which is an out-of-bounds/NULL dereference on
// component_get(id) - this was the root cause of a real crash during login. Every direct
// network-supplied component id must be checked with this before dereferencing.
static inline bool component_valid(int id) {
    return component_exists(id);
}

// Centralized lookup is also the PS2 lazy-definition boundary. Invalid Progressive-only ids remain
// safely rejected; valid serialized definitions are materialized the first time they are referenced.
static inline Component *component_get(int id) {
    return component_get_by_id(id);
}

bool client_read(Client *c) {
    if (!c->stream) {
        return false;
    }

    // try {
    if (!clientstream_available(c->stream, 1)) {
        return false;
    }

    if (c->packet_type == -1) {
        clientstream_read_bytes(c->stream, c->in->data, 0, 1);
        c->packet_type = c->in->data[0] & 0xff;
        // if (c->random_in) {
        c->packet_type = (c->packet_type - isaac_next(&c->random_in)) & 0xff;
        // }
        c->packet_size = _Protocol.SERVERPROT_SIZES[c->packet_type];
    }

    if (c->packet_size == -1) {
        if (!clientstream_available(c->stream, 1)) {
            return false;
        }

        clientstream_read_bytes(c->stream, c->in->data, 0, 1);
        c->packet_size = c->in->data[0] & 0xff;
    }

    if (c->packet_size == -2) {
        if (!clientstream_available(c->stream, 2)) {
            return false;
        }

        clientstream_read_bytes(c->stream, c->in->data, 0, 2);
        c->in->pos = 0;
        c->packet_size = g2(c->in);
    }

    if (!clientstream_available(c->stream, c->packet_size)) {
        return false;
    }
    c->in->pos = 0;
    clientstream_read_bytes(c->stream, c->in->data, 0, c->packet_size);
    c->idle_net_cycles = 0;
    c->last_packet_type2 = c->last_packet_type1;
    c->last_packet_type1 = c->last_packet_type0;
    c->last_packet_type0 = c->packet_type;

    if (c->packet_type == 186) { // VARP_SMALL
        // VARP_SMALL
        int varp = g2(c->in);
        int8_t value = g1b(c->in);
        // rev254 servers can send varp ids outside Client3's VARPS_COUNT table; without this check
        // an out-of-range id is an out-of-bounds heap write that corrupts unrelated memory and crashes
        // later, far from this packet, which is what made the original crash so hard to bisect.
        if (varp < 0 || varp >= VARPS_COUNT) {
            rs2_error("VARP_SMALL: varp id %d out of range (max %d), ignoring\n", varp, VARPS_COUNT - 1);
            c->packet_type = -1;
            return true;
        }
        c->varCache[varp] = value;
        if (c->varps[varp] != value) {
            c->varps[varp] = value;
            updateVarp(c, varp);
            c->redraw_sidebar = true;
            if (c->sticky_chat_interface_id != -1) {
                c->redraw_chatback = true;
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 111) { // UPDATE_FRIENDLIST
        // UPDATE_FRIENDLIST
        int64_t username = g8(c->in);
        int world = g1(c->in);
        char *display_name = jstring_format_name(jstring_from_base37(username));
        for (int i = 0; i < c->friend_count; i++) {
            if (username == c->friendName37[i]) {
                if (c->friendWorld[i] != world) {
                    c->friendWorld[i] = world;
                    c->redraw_sidebar = true;
                    char buf[MAX_STR];
                    if (world > 0) {
                        sprintf(buf, "%s has logged in.", display_name);
                        client_add_message(c, 5, buf, "");
                    }
                    if (world == 0) {
                        sprintf(buf, "%s has logged out.", display_name);
                        client_add_message(c, 5, buf, "");
                    }
                }
                display_name = NULL;
                break;
            }
        }
        if (display_name && c->friend_count < 100) {
            c->friendName37[c->friend_count] = username;
            c->friendName[c->friend_count] = display_name;
            c->friendWorld[c->friend_count] = world;
            c->friend_count++;
            c->redraw_sidebar = true;
        }
        bool sorted = false;
        while (!sorted) {
            sorted = true;
            for (int i = 0; i < c->friend_count - 1; i++) {
                if ((c->friendWorld[i] != _Client.nodeid && c->friendWorld[i + 1] == _Client.nodeid) || (c->friendWorld[i] == 0 && c->friendWorld[i + 1] != 0)) {
                    int oldWorld = c->friendWorld[i];
                    c->friendWorld[i] = c->friendWorld[i + 1];
                    c->friendWorld[i + 1] = oldWorld;

                    char *oldName = c->friendName[i];
                    c->friendName[i] = c->friendName[i + 1];
                    c->friendName[i + 1] = oldName;

                    int64_t oldName37 = c->friendName37[i];
                    c->friendName37[i] = c->friendName37[i + 1];
                    c->friendName37[i + 1] = oldName37;
                    c->redraw_sidebar = true;
                    sorted = false;
                }
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 143) { // UPDATE_REBOOT_TIMER
        // UPDATE_REBOOT_TIMER
        c->system_update_timer = g2(c->in) * 30;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 80) { // TODO: legacy DATA_LAND_DONE has no rev254 equivalent (JS5/bulk cache fetch replaces it), see audit
        // DATA_LAND_DONE
        int x = g1(c->in);
        int z = g1(c->in);
        int index = -1;
        for (int i = 0; i < c->sceneMapIndexLength; i++) {
            if (c->sceneMapIndex[i] == (x << 8) + z) {
                index = i;
            }
        }
        if (index != -1) {
#ifdef __EMSCRIPTEN__
            // TODO use indexeddb instead of emscripten memfs
            char filename[PATH_MAX];
            sprintf(filename, "m%d_%d", x, z);
            FILE *file = fopen(filename, "wb");
            fwrite(c->sceneMapLandData[index], 1, c->sceneMapLandDataIndexLength[index], file);
            fclose(file);
#endif
            // signlink.cachesave("m" + x + "_" + z, this.sceneMapLandData[index]);
            c->scene_state = 1;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 123) { // NPC_INFO
        // NPC_INFO
#ifdef __PS2__
        int64_t npcpos_t0 = rs2_now();
        getNpcPos(c, c->in, c->packet_size);
        _TickPhase.getnpcpos_ms += rs2_now() - npcpos_t0;
#else
        getNpcPos(c, c->in, c->packet_size);
#endif
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 209) { // REBUILD_NORMAL
        int zoneX = g2(c->in);
        int zoneZ = g2(c->in);
        if (c->sceneCenterZoneX == zoneX && c->sceneCenterZoneZ == zoneZ && c->scene_state != 0) {
            c->packet_type = -1;
            return true;
        }
        c->sceneCenterZoneX = zoneX;
        c->sceneCenterZoneZ = zoneZ;
        c->sceneBaseTileX = (c->sceneCenterZoneX - 6) * 8;
        c->sceneBaseTileZ = (c->sceneCenterZoneZ - 6) * 8;
        c->scene_state = 1;
        pixmap_bind(c->area_viewport);
        drawStringCenter(c->font_plain12, 257, 151, "Loading - please wait.", BLACK);
        drawStringCenter(c->font_plain12, 256, 150, "Loading - please wait.", WHITE);
        pixmap_draw(c->area_viewport, 4, 4);

#if defined(__PS2__) && PS2_NULL_SCENE_REBUILD
        // This is intentionally a no-scene diagnostic, not a production map loader.  The screen
        // above proved that the residual coordinate/entity relocation below can still block the
        // single network thread after all map decode/build work has been deferred.  Completing the
        // protocol acknowledgement here lets us distinguish that work from networking or display.
        c->scene_state = 2;
        _World.levelBuilt = c->currentLevel;
        p1isaac(c->out, 134); // MAP_BUILD_COMPLETE
        c->packet_type = -1;
        return true;
#endif

        // rev254 no longer sends a per-region CRC list in this packet (see the revision-254 audit) -
        // the client is expected to already have the full map pack locally and computes the needed
        // mapsquare grid itself from the center zone, matching the reference client's own algorithm.
        int minMapsquareX = (c->sceneCenterZoneX - 6) / 8;
        int maxMapsquareX = (c->sceneCenterZoneX + 6) / 8;
        int minMapsquareZ = (c->sceneCenterZoneZ - 6) / 8;
        int maxMapsquareZ = (c->sceneCenterZoneZ + 6) / 8;
        int regions = (maxMapsquareX - minMapsquareX + 1) * (maxMapsquareZ - minMapsquareZ + 1);
#if defined(__PS2__) && PS2_CHECKPOINTS_ENABLED
        // 2026-09-13: the per-read timing trail showed read #9 (the last one seen frozen on screen)
        // actually completing in 10ms before the freeze, not stalling mid-read as assumed - so the
        // read loop may simply have FINISHED (regions == 9) and the real hang is in whatever runs
        // right after it. Logging this here settles that directly: if regions is 9, the freeze is
        // downstream of this loop, not inside it, and every USB/file-organization/pacing experiment
        // so far was correctly targeting a loop that was never actually the problem.
        rs2_log("map: regions=%d (zone %d,%d -> mapsquare X %d..%d Z %d..%d)\n", regions, c->sceneCenterZoneX,
                c->sceneCenterZoneZ, minMapsquareX, maxMapsquareX, minMapsquareZ, maxMapsquareZ);
#endif

        // The regular client opens/decompresses every map square synchronously here, then does a
        // second synchronous build on the following PLAYER_INFO packet.  That blocks all later
        // interface/inventory packets.  The PS2 flat-terrain profile deliberately starts live
        // gameplay with no static map scene; an incremental terrain/loc streamer owns this later.
#if defined(__PS2__) && PS2_DEFER_SCENE_REBUILD
        client_scenemap_free(c);
        c->sceneMapLandData = NULL;
        c->sceneMapLocData = NULL;
        c->sceneMapIndex = NULL;
        c->sceneMapLandDataIndexLength = NULL;
        c->sceneMapLocDataIndexLength = NULL;
        c->sceneMapIndexLength = 0;
#else
        client_scenemap_free(c);
        c->sceneMapLandData = calloc(regions, sizeof(int8_t *));
        c->sceneMapLocData = calloc(regions, sizeof(int8_t *));
        c->sceneMapIndex = calloc(regions, sizeof(int));
        c->sceneMapLandDataIndexLength = calloc(regions, sizeof(int));
        c->sceneMapLocDataIndexLength = calloc(regions, sizeof(int));
        c->sceneMapIndexLength = regions;

        int i = 0;
        for (int mapsquareX = minMapsquareX; mapsquareX <= maxMapsquareX; mapsquareX++) {
            for (int mapsquareZ = minMapsquareZ; mapsquareZ <= maxMapsquareZ; mapsquareZ++) {
                c->sceneMapIndex[i] = (mapsquareX << 8) + mapsquareZ;

#if defined(__PS2__) && PS2_CHECKPOINTS_ENABLED
                // This whole loop is synchronous/blocking and the client is single-threaded - if
                // client_load_map_file() ever hangs on a real-hardware-only USB/filesystem issue,
                // nothing ever draws again, so whatever's on screen at that exact instant is frozen
                // forever. Show which mapsquare is about to load and force an immediate flip (not
                // just bind+draw, which only updates the area_viewport PixMap in memory -
                // platform_update_surface() is what actually presents a frame, same pattern
                // client_login() already uses for its own "Connecting..." screen) so a hang leaves
                // a legible "stuck on X_Z" on screen instead of a generic, uninformative "Loading".
                // Counted (not just coordinate-labeled) to distinguish "this specific file is the
                // problem" from "a fixed number of file opens exhausts some small IOP-side resource
                // table (open-file slots, directory cache, ...) regardless of which file is Nth" -
                // switching USB drivers entirely (BDM -> usbhdfsd) didn't change which coordinate
                // this hangs on, which points at the latter, not anything about this one file.
                // Also shows whether maps.dat is actually in use - a silent fallback to the
                // individual-file path (e.g. maps.dat not yet copied onto a given drive) would
                // otherwise look identical to this same freeze with no indication the archive was
                // ever tried, exactly what happened testing this the first time.
                char loading_msg[64];
                snprintf(loading_msg, sizeof(loading_msg), "Loading map %d_%d... (#%d, %s)", mapsquareX, mapsquareZ, i + 1,
                         ps2_map_archive_open() ? "archive" : "individual");
                pixmap_bind(c->area_viewport);
                pix2d_fill_rect(0, 130, BLACK, 512, 40);
                drawStringCenter(c->font_plain12, 257, 151, loading_msg, BLACK);
                drawStringCenter(c->font_plain12, 256, 150, loading_msg, WHITE);
                pixmap_draw(c->area_viewport, 4, 4);
                platform_update_surface();
#endif

#if defined(__PS2__) && PS2_CHECKPOINTS_ENABLED
                // Rate-limiting test (20ms sleep after each read) is DONE and disproven - confirmed
                // via real hardware, this still hangs at the exact same read regardless, ruling out
                // a USB controller/driver queuing issue triggered by rapid back-to-back small reads.
                // Removed rather than left in: it was pure per-iteration cost with zero remaining
                // diagnostic value.
                //
                // What's left to distinguish, now that file count/organization, driver stack, and
                // read pacing are all ruled out: does per-read latency climb before the freeze
                // (pointing at some IOP-side resource leaking a little more each read, eventually
                // deadlocking) or stay flat right up to a sudden cutoff (pointing at a fixed-size
                // table/queue that's fine until it's exactly full)? boot.log has never actually been
                // confirmed to show up on a real drive (fopen/fwrite/fflush/fclose all "succeed" per
                // ps2_log_to_file's own bookkeeping, but the FAT driver may only commit the directory
                // entry on a clean unmount, which a hang/power-cycle never gets) - so this can't rely
                // on it. It goes on screen instead: this always shows the LAST read that actually
                // completed, so whatever it says right before a freeze is real data even if nothing
                // written to boot.log ever survives to be read back.
                char timing_msg[64];
                int64_t land_t0 = rs2_now();
#endif
                int landSize = 0;
                int8_t *landData = client_load_map_file("m", mapsquareX, mapsquareZ, &landSize);
                if (landData) {
                    c->sceneMapLandDataIndexLength[i] = landSize;
                    c->sceneMapLandData[i] = landData;
                }
#if defined(__PS2__) && PS2_CHECKPOINTS_ENABLED
                int land_ms = (int)(rs2_now() - land_t0);
                rs2_log("map read #%d land %d_%d: %dms (%d bytes)\n", i + 1, mapsquareX, mapsquareZ, land_ms, landSize);
                snprintf(timing_msg, sizeof(timing_msg), "last read: #%d land %dms (%dB)", i + 1, land_ms, landSize);
                pixmap_bind(c->area_viewport);
                pix2d_fill_rect(0, 170, BLACK, 512, 20);
                drawStringCenter(c->font_plain12, 257, 181, timing_msg, BLACK);
                drawStringCenter(c->font_plain12, 256, 180, timing_msg, WHITE);
                pixmap_draw(c->area_viewport, 4, 4);
                platform_update_surface();

                int64_t loc_t0 = rs2_now();
#endif
                int locSize = 0;
#ifdef __PS2__
                // PS2 streaming mode: retain only the mapsquares that overlap
                // the 32x32 local-player window.  Do this before the file read
                // and bzip pass, rather than merely ignoring distant locs after
                // decoding them.  The 104x104 terrain grid is still loaded.
                const int ps2ActiveLocMin = 32;
                const int ps2ActiveLocMax = 64; // exclusive
                int squareLocalX = mapsquareX * 64 - c->sceneBaseTileX;
                int squareLocalZ = mapsquareZ * 64 - c->sceneBaseTileZ;
                bool ps2LoadLoc = squareLocalX < ps2ActiveLocMax && squareLocalX + 64 > ps2ActiveLocMin &&
                                  squareLocalZ < ps2ActiveLocMax && squareLocalZ + 64 > ps2ActiveLocMin;
#if PS2_DEFER_STATIC_LOCATIONS
                // The location stream is the only remaining work between the
                // confirmed land-decode checkpoint and the hardware freeze.
                // Do not read or decompress it in the synchronous rebuild.
                // Terrain, players, NPCs, and server movement remain live.
                ps2LoadLoc = false;
#endif
                if (ps2LoadLoc) {
#endif
                    int8_t *locData = client_load_map_file("l", mapsquareX, mapsquareZ, &locSize);
                    if (locData) {
                        c->sceneMapLocDataIndexLength[i] = locSize;
                        c->sceneMapLocData[i] = locData;
                    }
#ifdef __PS2__
                }
#endif
#if defined(__PS2__) && PS2_CHECKPOINTS_ENABLED
                int loc_ms = (int)(rs2_now() - loc_t0);
                rs2_log("map read #%d loc %d_%d: %dms (%d bytes)\n", i + 1, mapsquareX, mapsquareZ, loc_ms, locSize);
                snprintf(timing_msg, sizeof(timing_msg), "last read: #%d loc %dms (%dB)", i + 1, loc_ms, locSize);
                pixmap_bind(c->area_viewport);
                pix2d_fill_rect(0, 170, BLACK, 512, 20);
                drawStringCenter(c->font_plain12, 257, 181, timing_msg, BLACK);
                drawStringCenter(c->font_plain12, 256, 180, timing_msg, WHITE);
                pixmap_draw(c->area_viewport, 4, 4);
                platform_update_surface();
#endif

                i++;
            }
        }
#endif

#if defined(__PS2__) && PS2_CHECKPOINTS_ENABLED
        // Checkpoint: if this line's flip is the LAST thing ever visible on a frozen screen (instead
        // of the "last read: ..." line from inside the loop above), that proves every map file read
        // finished and the hang is somewhere in the tile-shift/npc/player/scene-build code below,
        // not in file I/O at all - directly testable, not a guess.
        // 2026-09-14: added heap_free - two consecutive real-hardware tests with byte-for-byte
        // identical _Client.lowmem state (config.ini already sets lowmem=1, so it was never actually
        // toggled by the custom.c edit around it) landed on two different checkpoints, one much
        // earlier than the other. That variability, not a deterministic pointer bug, is the signature
        // of a resource-exhaustion race - this number is the actual data needed to confirm it instead
        // of continuing to infer it indirectly from where a freeze happens to land.
        char ps2_allmaps_msg[64];
        snprintf(ps2_allmaps_msg, sizeof(ps2_allmaps_msg), "All map reads done, building scene... (free=%d)",
                 mallinfo().fordblks);
        pixmap_bind(c->area_viewport);
        pix2d_fill_rect(0, 170, BLACK, 512, 20);
        drawStringCenter(c->font_plain12, 257, 181, ps2_allmaps_msg, BLACK);
        drawStringCenter(c->font_plain12, 256, 180, ps2_allmaps_msg, WHITE);
        pixmap_draw(c->area_viewport, 4, 4);
        platform_update_surface();
        rs2_log("map: all %d region reads complete, entering post-load scene build\n", regions);
#endif

        pixmap_bind(c->area_viewport);
        pixmap_draw(c->area_viewport, 4, 4);
        int dx = c->sceneBaseTileX - c->mapLastBaseX;
        int dz = c->sceneBaseTileZ - c->mapLastBaseZ;
        c->mapLastBaseX = c->sceneBaseTileX;
        c->mapLastBaseZ = c->sceneBaseTileZ;
        for (int j = 0; j < MAX_NPC_COUNT; j++) {
            NpcEntity *npc = c->npcs[j];
            if (npc) {
                for (int k = 0; k < 10; k++) {
                    npc->pathing_entity.pathTileX[k] -= dx;
                    npc->pathing_entity.pathTileZ[k] -= dz;
                }
                npc->pathing_entity.x -= dx * 128;
                npc->pathing_entity.z -= dz * 128;
            }
        }
        for (int j = 0; j < MAX_PLAYER_COUNT; j++) {
            PlayerEntity *player = c->players[j];
            if (player) {
                for (int k = 0; k < 10; k++) {
                    player->pathing_entity.pathTileX[k] -= dx;
                    player->pathing_entity.pathTileZ[k] -= dz;
                }
                player->pathing_entity.x -= dx * 128;
                player->pathing_entity.z -= dz * 128;
            }
        }
        int8_t startTileX = 0;
        int8_t endTileX = 104;
        int8_t dirX = 1;
        if (dx < 0) {
            startTileX = 104 - 1;
            endTileX = -1;
            dirX = -1;
        }
        int8_t startTileZ = 0;
        int8_t endTileZ = 104;
        int8_t dirZ = 1;
        if (dz < 0) {
            startTileZ = 104 - 1;
            endTileZ = -1;
            dirZ = -1;
        }
        for (int x = startTileX; x != endTileX; x += dirX) {
            for (int z = startTileZ; z != endTileZ; z += dirZ) {
                int lastX = x + dx;
                int lastZ = z + dz;
                for (int level = 0; level < 4; level++) {
                    if (lastX >= 0 && lastZ >= 0 && lastX < 104 && lastZ < 104) {
                        c->level_obj_stacks[level][x][z] = c->level_obj_stacks[level][lastX][lastZ];
                    } else {
                        c->level_obj_stacks[level][x][z] = NULL;
                    }
                }
            }
        }
        for (LocAddEntity *loc = (LocAddEntity *)linklist_head(c->spawned_locations); loc; loc = (LocAddEntity *)linklist_next(c->spawned_locations)) {
            loc->x -= dx;
            loc->z -= dz;
            if (loc->x < 0 || loc->z < 0 || loc->x >= 104 || loc->z >= 104) {
                linkable_unlink(&loc->link);
                free(loc);
            }
        }
        if (c->flagSceneTileX != 0) {
            c->flagSceneTileX -= dx;
            c->flagSceneTileZ -= dz;
        }
        c->cutscene = false;

        // rev254 replaces the old REBUILD_GETMAPS retry-request with a simple completion ack once
        // local data is loaded (see the revision-254 audit) - no payload.
        p1isaac(c->out, 134); // MAP_BUILD_COMPLETE

        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 161) { // IF_SETPLAYERHEAD
        // IF_SETPLAYERHEAD
        int com = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        component_set_dynamic_model(component_get(com), playerentity_get_headmodel(c->local_player));
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 64) { // HINT_ARROW
        // HINT_ARROW
        c->hint_type = g1(c->in);
        if (c->hint_type == 1) {
            c->hint_npc = g2(c->in);
        }
        if (c->hint_type >= 2 && c->hint_type <= 6) {
            if (c->hint_type == 2) {
                c->hint_offset_x = 64;
                c->hint_offset_z = 64;
            }
            if (c->hint_type == 3) {
                c->hint_offset_x = 0;
                c->hint_offset_z = 64;
            }
            if (c->hint_type == 4) {
                c->hint_offset_x = 128;
                c->hint_offset_z = 64;
            }
            if (c->hint_type == 5) {
                c->hint_offset_x = 64;
                c->hint_offset_z = 0;
            }
            if (c->hint_type == 6) {
                c->hint_offset_x = 64;
                c->hint_offset_z = 128;
            }
            c->hint_type = 2;
            c->hint_tile_x = g2(c->in);
            c->hint_tile_z = g2(c->in);
            c->hint_height = g1(c->in);
        }
        if (c->hint_type == 10) {
            c->hint_player = g2(c->in);
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 54) { // TODO: rev254 MIDI_SONG(163,2) is fixed 2 bytes, not this embedded name+crc+len payload; needs redesign, see audit
        // MIDI_SONG
        char *name = gjstr(c->in);
        int crc = g4(c->in);
        int length = g4(c->in);
        if (strcmp(name, c->currentMidi) != 0 && c->midiActive && !_Client.lowmem) {
            platform_set_midi(name, crc, length);
        }
        strcpy(c->currentMidi, name);
        free(name);
        c->midiCrc = crc;
        c->midiSize = length;
        c->nextMusicDelay = 0;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 21) { // LOGOUT
        // LOGOUT
        client_logout(c);
        c->packet_type = -1;
        return false;
    }
    if (c->packet_type == 20) { // TODO: legacy DATA_LOC_DONE has no rev254 equivalent (JS5/bulk cache fetch replaces it), see audit
        // DATA_LOC_DONE
        int x = g1(c->in);
        int z = g1(c->in);
        int index = -1;
        for (int i = 0; i < c->sceneMapIndexLength; i++) {
            if (c->sceneMapIndex[i] == (x << 8) + z) {
                index = i;
            }
        }
        if (index != -1) {
#if defined(__EMSCRIPTEN__)
            // TODO use indexeddb instead of emscripten memfs
            char filename[PATH_MAX];
            sprintf(filename, "l%d_%d", x, z);
            FILE *file = fopen(filename, "wb");
            fwrite(c->sceneMapLocData[index], 1, c->sceneMapLocDataIndexLength[index], file);
            fclose(file);
#endif
            // signlink.cachesave("l" + x + "_" + z, c->sceneMapLocData[index]);
            c->scene_state = 1;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 108) { // UNSET_MAP_FLAG
        // UNSET_MAP_FLAG
        c->flagSceneTileX = 0;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 213) { // UPDATE_PID
        // UPDATE_UID192
        c->local_pid = g2(c->in);
        g1(c->in); // TODO: rev254 UPDATE_PID is 1 byte longer than rev225's; unidentified extra field, discarded for now
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 98 || c->packet_type == 218 || c->packet_type == 8 || c->packet_type == 114 || c->packet_type == 37 || c->packet_type == 115 || c->packet_type == 120 || c->packet_type == 30 || c->packet_type == 88 || c->packet_type == 70) {
        // OBJ_COUNT, P_LOCMERGE, OBJ_REVEAL, MAP_ANIM, MAP_PROJANIM, OBJ_DEL, OBJ_ADD, LOC_ANIM, LOC_DEL, LOC_ADD_CHANGE
#if defined(__PS2__) && PS2_NULL_SCENE_REBUILD
        c->packet_type = -1;
        return true;
#endif
        // Zone Protocol
        readZonePacket(c, c->in, c->packet_type);
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 249) { // IF_OPENMAIN_SIDE
        // IF_OPENMAINSIDEMODAL
        int main = g2(c->in);
        int side = g2(c->in);
        if (c->chat_interface_id != -1) {
            c->chat_interface_id = -1;
            c->redraw_chatback = true;
        }
        if (c->chatback_input_open) {
            c->chatback_input_open = false;
            c->redraw_chatback = true;
        }
        c->viewport_interface_id = main;
        c->sidebar_interface_id = side;
        c->redraw_sidebar = true;
        c->redraw_sideicons = true;
        c->pressed_continue_option = false;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 196) { // VARP_LARGE
        // VARP_LARGE
        int varp = g2(c->in);
        int value = g4(c->in);
        // see VARP_SMALL above: rev254 varp ids can exceed VARPS_COUNT, don't write out of bounds.
        if (varp < 0 || varp >= VARPS_COUNT) {
            rs2_error("VARP_LARGE: varp id %d out of range (max %d), ignoring\n", varp, VARPS_COUNT - 1);
            c->packet_type = -1;
            return true;
        }
        c->varCache[varp] = value;
        if (c->varps[varp] != value) {
            c->varps[varp] = value;
            updateVarp(c, varp);
            c->redraw_sidebar = true;
            if (c->sticky_chat_interface_id != -1) {
                c->redraw_chatback = true;
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 95) { // IF_SETANIM
        // IF_SETANIM
        int com = g2(c->in);
        int seqId = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        // unlike every other seqId consumer this session (getPlayerExtended/getNpcPosExtended), this
        // one stored the raw network value with no 65535->-1 sentinel normalization and no bounds
        // check at all - client_draw_interface's type==6 (animated model) branch later indexes
        // _SeqType.instances[seqId] unconditionally whenever anim != -1, so an out-of-range or
        // still-raw-65535 value here was a real crash confirmed to fire when an interface (e.g. an
        // NPC dialogue's animated chat-head) uses this animation. Confirmed real root cause of
        // "crashes when I click an NPC" (both left-click default-interact and right-click Talk-to
        // open the same dialogue interface).
        if (seqId == 65535 || seqId < 0 || seqId >= _SeqType.count) {
            seqId = -1;
        }
        component_get(com)->anim = seqId;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 91) { // IF_SETTAB
        // IF_OPENSIDEOVERLAY
        int com = g2(c->in);
        int tab = g1(c->in);
        if (com == 65535) {
            com = -1;
        }
        c->tab_interface_id[tab] = com;
        c->redraw_sidebar = true;
        c->redraw_sideicons = true;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 220) { // TODO: legacy DATA_LOC has no rev254 equivalent (JS5/bulk cache fetch replaces it), see audit
        // DATA_LOC
        int x = g1(c->in);
        int z = g1(c->in);
        int offset = g2(c->in);
        int length = g2(c->in);
        int index = -1;
        for (int i = 0; i < c->sceneMapIndexLength; i++) {
            if (c->sceneMapIndex[i] == (x << 8) + z) {
                index = i;
            }
        }
        if (index != -1) {
            if (!c->sceneMapLocData[index] || c->sceneMapLocDataIndexLength[index] != length) {
                free(c->sceneMapLocData[index]);
                c->sceneMapLocData[index] = calloc(length, sizeof(int8_t));
            }
            gdata(c->in, c->packet_size - 6, offset, c->sceneMapLocData[index]);
            c->sceneMapLocDataIndexLength[index] = length;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 29) { // FINISH_TRACKING
        // FINISH_TRACKING
        Packet *tracking = inputtracking_stop(&_InputTracking);
        if (tracking) {
            // EVENT_TRACKING
            p1isaac(c->out, 142); // EVENT_TRACKING
            p2(c->out, tracking->pos);
            pdata(c->out, tracking->data, tracking->pos, 0);
            packet_release(tracking);
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 28) { // UPDATE_INV_FULL
        // UPDATE_INV_FULL
        c->redraw_sidebar = true;
        int com = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        Component *inv = component_get(com);
        int size = g1(c->in);
        for (int i = 0; i < size; i++) {
            inv->invSlotObjId[i] = g2(c->in);
            int count = g1(c->in);
            if (count == 255) {
                count = g4(c->in);
            }
            inv->invSlotObjCount[i] = count;
        }
        for (int i = size; i < inv->width * inv->height; i++) {
            inv->invSlotObjId[i] = 0;
            inv->invSlotObjCount[i] = 0;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 251) { // ENABLE_TRACKING
        // ENABLE_TRACKING
        inputtracking_set_enabled(&_InputTracking);
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 5) { // P_COUNTDIALOG
        // P_COUNTDIALOG
        c->show_social_input = false;
        c->chatback_input_open = true;
        c->chatback_input[0] = '\0';
        c->redraw_chatback = true;
        c->packet_type = -1;
        virtual_keyboard_maybe_open(c, 4);
        return true;
    }
    if (c->packet_type == 168) { // UPDATE_INV_STOP_TRANSMIT
        // UPDATE_INV_STOP_TRANSMIT
        int com = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        Component *inv = component_get(com);
        for (int i = 0; i < inv->width * inv->height; i++) {
            inv->invSlotObjId[i] = -1;
            inv->invSlotObjId[i] = 0;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 146) { // LAST_LOGIN_INFO
        // LAST_LOGIN_INFO
        c->lastAddress = g4(c->in);
        c->daysSinceLastLogin = g2(c->in);
        c->daysSinceRecoveriesChanged = g1(c->in);
        c->unreadMessages = g2(c->in);
        g1(c->in); // TODO: rev254 LAST_LOGIN_INFO is 1 byte longer than rev225's; unidentified extra field, discarded for now
        if (c->lastAddress != 0 && c->viewport_interface_id == -1) {
            if (_Custom.hide_dns) {
                _Client.dns = "unknown";
            } else {
                _Client.dns = dnslookup(jstring_format_ipv4(c->lastAddress));
            }
            closeInterfaces(c);
            int clientCode = 650;
            if (c->daysSinceRecoveriesChanged != 201) {
                clientCode = 655;
            }
            c->reportAbuseInput[0] = '\0';
            c->reportAbuseMuteOption = false;
            Component *welcome = component_find_by_client_code(clientCode);
            if (welcome) c->viewport_interface_id = welcome->layer;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 58) { // TUT_FLASH
        // TUTORIAL_FLASHSIDE
        c->flashing_tab = g1(c->in);
        if (c->flashing_tab == c->selected_tab) {
            if (c->flashing_tab == 3) {
                c->selected_tab = 1;
            } else {
                c->selected_tab = 3;
            }
            c->redraw_sidebar = true;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 212) { // TODO: rev254 MIDI_JINGLE(242,4) is fixed 4 bytes, not this embedded/decompressed payload; needs redesign, see audit
        // MIDI_JINGLE
        if (c->midiActive && !_Client.lowmem) {
            int delay = g2(c->in);
            int length = g4(c->in);
            int remaining = c->packet_size - 6;
            int8_t *src = calloc(length, sizeof(int8_t));
#ifdef __PS2__
            bzip_decompress(src, c->in->data, remaining, c->in->pos, NULL, length);
#else
            bzip_decompress(src, c->in->data, remaining, c->in->pos);
#endif
            platform_set_jingle(src, length);
            c->nextMusicDelay = delay;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 75) { // SET_MULTIWAY
        // SET_MULTIWAY
        c->in_multizone = g1(c->in);
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 25) { // SYNTH_SOUND
        // SYNTH_SOUND
        int id = g2(c->in);
        int loop = g1(c->in);
        int delay = g2(c->in);
        if (c->wave_enabled && !_Client.lowmem && c->wave_count < 50) {
            c->wave_ids[c->wave_count] = id;
            c->wave_loops[c->wave_count] = loop;
            c->wave_delay[c->wave_count] = delay + _Wave.delays[id];
            c->wave_count++;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 3) { // IF_SETNPCHEAD
        // IF_SETNPCHEAD
        int com = g2(c->in);
        int npcId = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        NpcType *npc = npctype_get(npcId);
        component_set_dynamic_model(component_get(com), npctype_get_headmodel(npc));
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 173) { // UPDATE_ZONE_PARTIAL_FOLLOWS
        // UPDATE_ZONE_PARTIAL_FOLLOWS
        c->baseX = g1(c->in);
        c->baseZ = g1(c->in);
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 103) { // TODO: no rev254 equivalent identified for IF_SETRECOL, see audit
        // IF_SETRECOL
        int com = g2(c->in);
        int src = g2(c->in);
        int dst = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        Component *inter = component_get(com);
#ifdef __PS2__
        component_ensure_model(inter);
#endif
        Model *model = inter->model;
        if (model) {
            model_recolor(model, src, dst);
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 24) { // CHAT_FILTER_SETTINGS
        // CHAT_FILTER_SETTINGS
        c->public_chat_setting = g1(c->in);
        c->private_chat_setting = g1(c->in);
        c->trade_chat_setting = g1(c->in);
        c->redraw_privacy_settings = true;
        c->redraw_chatback = true;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 187) { // IF_OPENSIDE
        // IF_OPENSIDEMODAL
        int com = g2(c->in);
        reset_interface_animation(com);
        if (c->chat_interface_id != -1) {
            c->chat_interface_id = -1;
            c->redraw_chatback = true;
        }
        if (c->chatback_input_open) {
            c->chatback_input_open = false;
            c->redraw_chatback = true;
        }
        c->sidebar_interface_id = com;
        c->redraw_sidebar = true;
        c->redraw_sideicons = true;
        c->viewport_interface_id = -1;
        c->pressed_continue_option = false;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 141) { // IF_OPENCHAT
        // IF_OPENCHATMODAL
        int com = g2(c->in);
        reset_interface_animation(com);
        if (c->sidebar_interface_id != -1) {
            c->sidebar_interface_id = -1;
            c->redraw_sidebar = true;
            c->redraw_sideicons = true;
        }
        c->chat_interface_id = com;
        c->redraw_chatback = true;
        c->viewport_interface_id = -1;
        c->pressed_continue_option = false;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 27) { // IF_SETPOSITION
        // IF_SETPOSITION
        int com = g2(c->in);
        int x = g2b(c->in);
        int z = g2b(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        Component *inter = component_get(com);
        inter->x = x;
        inter->y = z;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 55) { // CAM_MOVETO
        // CAM_MOVETO
        c->cutscene = true;
        c->cutsceneSrcLocalTileX = g1(c->in);
        c->cutsceneSrcLocalTileZ = g1(c->in);
        c->cutsceneSrcHeight = g2(c->in);
        c->cutsceneMoveSpeed = g1(c->in);
        c->cutsceneMoveAcceleration = g1(c->in);
        if (c->cutsceneMoveAcceleration >= 100) {
            c->cameraX = c->cutsceneSrcLocalTileX * 128 + 64;
            c->cameraZ = c->cutsceneSrcLocalTileZ * 128 + 64;
            c->cameraY = getHeightmapY(c, c->currentLevel, c->cutsceneSrcLocalTileX, c->cutsceneSrcLocalTileZ) - c->cutsceneSrcHeight;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 159) { // UPDATE_ZONE_FULL_FOLLOWS
        // UPDATE_ZONE_FULL_FOLLOWS
        c->baseX = g1(c->in);
        c->baseZ = g1(c->in);
#if defined(__PS2__) && PS2_NULL_SCENE_REBUILD
        // Preserve the zone base but do not apply object/loc work to an intentionally empty scene.
        c->packet_type = -1;
        return true;
#endif
        for (int x = c->baseX; x < c->baseX + 8; x++) {
            for (int z = c->baseZ; z < c->baseZ + 8; z++) {
                if (c->level_obj_stacks[c->currentLevel][x][z]) {
                    linklist_free(c->level_obj_stacks[c->currentLevel][x][z]);
                    c->level_obj_stacks[c->currentLevel][x][z] = NULL;
                }
                sortObjStacks(c, x, z);
            }
        }
        for (LocAddEntity *loc = (LocAddEntity *)linklist_head(c->spawned_locations); loc; loc = (LocAddEntity *)linklist_next(c->spawned_locations)) {
            if (loc->x >= c->baseX && loc->x < c->baseX + 8 && loc->z >= c->baseZ && loc->z < c->baseZ + 8 && loc->plane == c->currentLevel) {
                addLoc(c, loc->plane, loc->x, loc->z, loc->lastLocIndex, loc->lastAngle, loc->lastShape, loc->layer);
                linkable_unlink(&loc->link);
                free(loc);
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 132) { // TODO: legacy DATA_LAND has no rev254 equivalent (JS5/bulk cache fetch replaces it), see audit
        // DATA_LAND
        int x = g1(c->in);
        int z = g1(c->in);
        int offset = g2(c->in);
        int length = g2(c->in);
        int index = -1;
        for (int i = 0; i < c->sceneMapIndexLength; i++) {
            if (c->sceneMapIndex[i] == (x << 8) + z) {
                index = i;
            }
        }
        if (index != -1) {
            if (!c->sceneMapLandData[index] || c->sceneMapLandDataIndexLength[index] != length) {
                free(c->sceneMapLandData[index]);
                c->sceneMapLandData[index] = calloc(length, sizeof(int8_t));
            }
            gdata(c->in, c->packet_size - 6, offset, c->sceneMapLandData[index]);
            c->sceneMapLandDataIndexLength[index] = length;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 60) { // MESSAGE_PRIVATE
        // MESSAGE_PRIVATE
        int64_t from = g8(c->in);
        int messageId = g4(c->in);
        int staffModLevel = g1(c->in);
        bool ignored = false;
        for (int i = 0; i < 100; i++) {
            if (c->messageIds[i] == messageId) {
                ignored = true;
                break;
            }
        }
        if (staffModLevel <= 1) {
            for (int i = 0; i < c->ignoreCount; i++) {
                if (c->ignoreName37[i] == from) {
                    ignored = true;
                    break;
                }
            }
        }
        if (!ignored && c->overrideChat == 0) {
            // try {
            c->messageIds[c->privateMessageCount] = messageId;
            c->privateMessageCount = (c->privateMessageCount + 1) % 100;
            char *uncompressed = wordpack_unpack(c->in, c->packet_size - 13);
            wordfilter_filter(uncompressed);
            char *sender = jstring_format_name(jstring_from_base37(from));
            if (staffModLevel > 1) {
                client_add_message(c, 7, uncompressed, sender);
            } else {
                client_add_message(c, 3, uncompressed, sender);
            }
            free(sender);
            free(uncompressed);
            // } catch (@Pc(2752) Exception ex) {
            // 	signlink.reporterror("cde1");
            // }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 140) { // RESET_CLIENT_VARCACHE
        // RESET_CLIENT_VARCACHE
        for (int i = 0; i < VARPS_COUNT; i++) {
            if (c->varps[i] != c->varCache[i]) {
                c->varps[i] = c->varCache[i];
                updateVarp(c, i);
                c->redraw_sidebar = true;
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 211) { // IF_SETMODEL
        // IF_SETMODEL
        int com = g2(c->in);
        int model = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        component_set_dynamic_model(component_get(com), model_from_id(model, false));
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 239) { // TUT_OPEN
        // TUTORIAL_OPENCHAT
        int com = g2b(c->in);
        c->sticky_chat_interface_id = com;
        c->redraw_chatback = true;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 94) { // UPDATE_RUNENERGY
        // UPDATE_RUNENERGY
        if (c->selected_tab == 12) {
            c->redraw_sidebar = true;
        }
        c->energy = g1(c->in);
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 0) { // CAM_LOOKAT
        // CAM_LOOKAT
        c->cutscene = true;
        c->cutsceneDstLocalTileX = g1(c->in);
        c->cutsceneDstLocalTileZ = g1(c->in);
        c->cutsceneDstHeight = g2(c->in);
        c->cutsceneRotateSpeed = g1(c->in);
        c->cutsceneRotateAcceleration = g1(c->in);
        if (c->cutsceneRotateAcceleration >= 100) {
            int sceneX = c->cutsceneDstLocalTileX * 128 + 64;
            int sceneZ = c->cutsceneDstLocalTileZ * 128 + 64;
            int sceneY = getHeightmapY(c, c->currentLevel, c->cutsceneDstLocalTileX, c->cutsceneDstLocalTileZ) - c->cutsceneDstHeight;
            int deltaX = sceneX - c->cameraX;
            int deltaY = sceneY - c->cameraY;
            int deltaZ = sceneZ - c->cameraZ;
            int distance = (int)sqrt(deltaX * deltaX + deltaZ * deltaZ);
            c->cameraPitch = (int)(atan2(deltaY, distance) * RADIANS_TO_RS) & 0x7ff;
            c->cameraYaw = (int)(atan2(deltaX, deltaZ) * -RADIANS_TO_RS) & 0x7ff;
            if (c->cameraPitch < 128) {
                c->cameraPitch = 128;
            }
            if (c->cameraPitch > 383) {
                c->cameraPitch = 383;
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 138) { // IF_SETTAB_ACTIVE
        // IF_SHOWSIDE
        c->selected_tab = g1(c->in);
        c->redraw_sidebar = true;
        c->redraw_sideicons = true;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 73) { // MESSAGE_GAME
        // MESSAGE_GAME
        char *message = gjstr(c->in);
        int64_t username;
        if (strendswith(message, ":tradereq:")) {
            char *player = substring(message, 0, indexof_chr(message, ':'));
            username = jstring_to_base37(player);
            bool ignored = false;
            for (int i = 0; i < c->ignoreCount; i++) {
                if (c->ignoreName37[i] == username) {
                    ignored = true;
                    break;
                }
            }
            if (!ignored && c->overrideChat == 0) {
                client_add_message(c, 4, "wishes to trade with you.", player);
            }
        } else if (strendswith(message, ":duelreq:")) {
            char *player = substring(message, 0, indexof_chr(message, ':'));
            username = jstring_to_base37(player);
            bool ignored = false;
            for (int i = 0; i < c->ignoreCount; i++) {
                if (c->ignoreName37[i] == username) {
                    ignored = true;
                    break;
                }
            }
            if (!ignored && c->overrideChat == 0) {
                client_add_message(c, 8, "wishes to duel with you.", player);
            }
        } else {
            client_add_message(c, 0, message, "");
        }
        free(message);
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 222) { // IF_SETOBJECT
        // IF_SETOBJECT
        int com = g2(c->in);
        int objId = g2(c->in);
        int zoom = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        ObjType *obj = objtype_get(objId);
        component_set_dynamic_model(component_get(com), objtype_get_interfacemodel(obj, 50, false));
        component_get(com)->xan = obj->xan2d;
        component_get(com)->yan = obj->yan2d;
        component_get(com)->zoom = obj->zoom2d * 100 / zoom;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 197) { // IF_OPENMAIN
        // IF_OPENMAINMODAL
        int com = g2(c->in);
        reset_interface_animation(com);
        if (c->sidebar_interface_id != -1) {
            c->sidebar_interface_id = -1;
            c->redraw_sidebar = true;
            c->redraw_sideicons = true;
        }
        if (c->chat_interface_id != -1) {
            c->chat_interface_id = -1;
            c->redraw_chatback = true;
        }
        if (c->chatback_input_open) {
            c->chatback_input_open = false;
            c->redraw_chatback = true;
        }
        c->viewport_interface_id = com;
        c->pressed_continue_option = false;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 38) { // IF_SETCOLOUR
        // IF_SETCOLOUR
        int com = g2(c->in);
        int color = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        int r = color >> 10 & 0x1f;
        int g = color >> 5 & 0x1f;
        int b = color & 0x1f;
        component_get(com)->colour = (r << 19) + (g << 11) + (b << 3);
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 203) { // RESET_ANIMS
        // RESET_ANIMS
        for (int i = 0; i < MAX_PLAYER_COUNT; i++) {
            if (c->players[i]) {
                c->players[i]->pathing_entity.primarySeqId = -1;
            }
        }
        for (int i = 0; i < MAX_NPC_COUNT; i++) {
            if (c->npcs[i]) {
                c->npcs[i]->pathing_entity.primarySeqId = -1;
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 227) { // IF_SETHIDE
        // IF_SETHIDE
        int com = g2(c->in);
        bool hide = g1(c->in) == 1;
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        component_get(com)->hide = hide;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 63) { // UPDATE_IGNORELIST
        // UPDATE_IGNORELIST
        c->ignoreCount = c->packet_size / 8;
        for (int i = 0; i < c->ignoreCount; i++) {
            c->ignoreName37[i] = g8(c->in);
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 167) { // CAM_RESET
        // CAM_RESET
        c->cutscene = false;
        for (int i = 0; i < 5; i++) {
            c->cameraModifierEnabled[i] = false;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 174) { // IF_CLOSE
        // IF_CLOSE
        if (c->sidebar_interface_id != -1) {
            c->sidebar_interface_id = -1;
            c->redraw_sidebar = true;
            c->redraw_sideicons = true;
        }
        if (c->chat_interface_id != -1) {
            c->chat_interface_id = -1;
            c->redraw_chatback = true;
        }
        if (c->chatback_input_open) {
            c->chatback_input_open = false;
            c->redraw_chatback = true;
        }
        c->viewport_interface_id = -1;
        c->pressed_continue_option = false;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 41) { // IF_SETTEXT
        // IF_SETTEXT
        int com = g2(c->in);
        char *text = gjstr(c->in);
        if (!component_valid(com)) {
            free(text);
            c->packet_type = -1;
            return true;
        }
        if (!component_get(com)->text) {
            component_get(com)->text = malloc(DOUBLE_STR);
        }
        strncpy(component_get(com)->text, text, DOUBLE_STR - 1);
        component_get(com)->text[DOUBLE_STR - 1] = '\0';
        free(text);
        if (component_get(com)->layer == c->tab_interface_id[c->selected_tab]) {
            c->redraw_sidebar = true;
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 136) { // UPDATE_STAT
        // UPDATE_STAT
        c->redraw_sidebar = true;
        int stat = g1(c->in);
        int xp = g4(c->in);
        int level = g1(c->in);
        c->skillExperience[stat] = xp;
        c->skillLevel[stat] = level;
        c->skillBaseLevel[stat] = 1;
        for (int i = 0; i < 98; i++) {
            if (xp >= _Client.levelExperience[i]) {
                c->skillBaseLevel[stat] = i + 2;
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 61) { // UPDATE_ZONE_PARTIAL_ENCLOSED
        // UPDATE_ZONE_PARTIAL_ENCLOSED
        c->baseX = g1(c->in);
        c->baseZ = g1(c->in);
#if defined(__PS2__) && PS2_NULL_SCENE_REBUILD
        c->packet_type = -1;
        return true;
#endif
        while (c->in->pos < c->packet_size) {
            int opcode = g1(c->in);
            readZonePacket(c, c->in, opcode);
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 164) { // UPDATE_RUNWEIGHT
        // UPDATE_RUNWEIGHT
        if (c->selected_tab == 12) {
            c->redraw_sidebar = true;
        }
        c->weightCarried = g2b(c->in);
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 225) { // CAM_SHAKE
        // CAM_SHAKE
        int type = g1(c->in);
        int jitter = g1(c->in);
        int wobbleScale = g1(c->in);
        int wobbleSpeed = g1(c->in);
        c->cameraModifierEnabled[type] = true;
        c->cameraModifierJitter[type] = jitter;
        c->cameraModifierWobbleScale[type] = wobbleScale;
        c->cameraModifierWobbleSpeed[type] = wobbleSpeed;
        c->cameraModifierCycle[type] = 0;
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 170) { // UPDATE_INV_PARTIAL
        // UPDATE_INV_PARTIAL
        c->redraw_sidebar = true;
        int com = g2(c->in);
        if (!component_valid(com)) {
            c->packet_type = -1;
            return true;
        }
        Component *inv = component_get(com);
        while (c->in->pos < c->packet_size) {
            int slot = g1(c->in);
            int id = g2(c->in);
            int count = g1(c->in);
            if (count == 255) {
                count = g4(c->in);
            }
            if (slot >= 0 && slot < inv->width * inv->height) {
                inv->invSlotObjId[slot] = id;
                inv->invSlotObjCount[slot] = count;
            }
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 87) { // PLAYER_INFO
        // PLAYER_INFO
#ifdef __PS2__
        int64_t getplayer_t0 = rs2_now();
        getPlayer(c, c->in, c->packet_size);
        _TickPhase.getplayer_ms += rs2_now() - getplayer_t0;
#else
        getPlayer(c, c->in, c->packet_size);
#endif
        if (c->scene_state == 1) {
            c->scene_state = 2;
            _World.levelBuilt = c->currentLevel;
#if !defined(__PS2__) || !PS2_DEFER_SCENE_REBUILD
            client_build_scene(c);
#endif
        }
        if (_Client.lowmem && c->scene_state == 2 && _World.levelBuilt != c->currentLevel) {
#ifndef __PS2__
            pixmap_bind(c->area_viewport);
            drawStringCenter(c->font_plain12, 257, 151, "Loading - please wait.", BLACK);
            drawStringCenter(c->font_plain12, 256, 150, "Loading - please wait.", WHITE);
            pixmap_draw(c->area_viewport, 4, 4);
#endif
            _World.levelBuilt = c->currentLevel;
#ifdef __PS2__
            // The low-memory PS2 scene only materialises the active visual plane.
            // PLAYER_INFO can move currentLevel (stairs/ladders) without a region
            // change, so keeping the old scene here leaves the actor at the new
            // floor height while the old floor remains resident. Rebuild the same
            // bounded terrain/loc window for the new plane; client_build_scene()
            // resets and reuses the scene arena rather than retaining both floors.
#endif
            client_build_scene(c);
        }
        if (c->currentLevel != c->minimap_level && c->scene_state == 2) {
#if defined(__PS2__) && PS2_DISABLE_MINIMAP
            c->minimap_level = c->currentLevel;
#else
            c->minimap_level = c->currentLevel;
            createMinimap(c, c->currentLevel);
#endif
        }
        c->packet_type = -1;
        return true;
    }
    if (c->packet_type == 255) { // FRIENDLIST_LOADED
        // TODO: no rev225 equivalent existed to model this on; discarding the 1-byte payload for now
        // since any unhandled opcode falls through to the T1 error path below, which logs the client out.
        g1(c->in);
        c->packet_type = -1;
        return true;
    }
    rs2_error("T1 - %i,%i - %i,%i\n", c->packet_type, c->packet_size, c->last_packet_type1, c->last_packet_type2);
    // signlink.reporterror("T1 - " + c->packet_type + "," + c->packetSize + " - " + c->lastPacketType1 + "," + c->lastPacketType2);
    // rev254: an unrecognised opcode here means "not yet implemented" (the byte count was already read
    // correctly via SERVERPROT_SIZES, so the stream isn't desynced), not necessarily real corruption
    // like it would have been for a matched, fully-implemented rev225 protocol - don't force a logout
    // over a missing feature. TODO: revisit once the remaining rev254-only opcodes are all implemented.
    // client_logout(c);
    c->packet_type = -1;
    // } catch (@Pc(3862) IOException ex) {
    // client_try_reconnect(c);
    // } catch (@Pc(3867) Exception ex) {
    // ex.printStackTrace();
    // const char* error = "T2 - " + c->packet_type + "," + c->lastPacketType1 + "," + c->lastPacketType2 + " - " + c->packetSize + "," + (c->sceneBaseTileX + c->localPlayer.pathTileX[0]) + "," + (c->sceneBaseTileZ + c->localPlayer.pathTileZ[0]) + " - ";
    // for (int i = 0; i < c->packet_size && i < 50; i++) {
    // 	error = error + c->in->data[i] + ",";
    // }
    // signlink.reporterror(error);
    // client_logout(c);
    // }
    return true;
}

void getPlayerLocal(Client *c, Packet *buf, int size) {
    (void)size;
    access_bits(buf);

    int info = gbit(buf, 1);
    if (info != 0) {
        int op = gbit(buf, 2);

        if (op == 0) {
            c->entityUpdateIds[c->entityUpdateCount++] = LOCAL_PLAYER_INDEX;
        } else if (op == 1) {
            int walkDir = gbit(buf, 3);
            pathingentity_movealongroute(&c->local_player->pathing_entity, false, walkDir);

            int extendedInfo = gbit(buf, 1);
            if (extendedInfo == 1) {
                c->entityUpdateIds[c->entityUpdateCount++] = LOCAL_PLAYER_INDEX;
            }
        } else if (op == 2) {
            int walkDir = gbit(buf, 3);
            pathingentity_movealongroute(&c->local_player->pathing_entity, true, walkDir);
            int runDir = gbit(buf, 3);
            pathingentity_movealongroute(&c->local_player->pathing_entity, true, runDir);

            int extendedInfo = gbit(buf, 1);
            if (extendedInfo == 1) {
                c->entityUpdateIds[c->entityUpdateCount++] = LOCAL_PLAYER_INDEX;
            }
        } else if (op == 3) {
            c->currentLevel = gbit(buf, 2);
            // TODO:
            // if (c->show_debug) {
            // 	c->userTileMarkers = ground_new(4);
            // 	c->userTileMarkerIndex = 0;
            // }
            int localX = gbit(buf, 7);
            int localZ = gbit(buf, 7);
            int jump = gbit(buf, 1);
            pathingentity_teleport(&c->local_player->pathing_entity, jump == 1, localX, localZ);

            int extendedInfo = gbit(buf, 1);
            if (extendedInfo == 1) {
                c->entityUpdateIds[c->entityUpdateCount++] = LOCAL_PLAYER_INDEX;
            }
        }
    }
}

void getPlayerOldVis(Client *c, Packet *buf, int size) {
    (void)size;
    int count = gbit(buf, 8);

    if (count < c->player_count) {
        for (int i = count; i < c->player_count; i++) {
            c->entityRemovalIds[c->entityRemovalCount++] = c->player_ids[i];
        }
    }

    if (count > c->player_count) {
        rs2_error("%s Too many players\n", c->username);
        // signlink.reporterror(c->username + " Too many players");
        // throw new RuntimeException("eek");
    }

    c->player_count = 0;
    for (int i = 0; i < count; i++) {
        int index = c->player_ids[i];
        PlayerEntity *player = c->players[index];

        int info = gbit(buf, 1);
        if (info == 0) {
            c->player_ids[c->player_count++] = index;
            player->pathing_entity.cycle = _Client.loop_cycle;
        } else {
            int op = gbit(buf, 2);

            if (op == 0) {
                c->player_ids[c->player_count++] = index;
                player->pathing_entity.cycle = _Client.loop_cycle;
                c->entityUpdateIds[c->entityUpdateCount++] = index;
            } else if (op == 1) {
                c->player_ids[c->player_count++] = index;
                player->pathing_entity.cycle = _Client.loop_cycle;

                int walkDir = gbit(buf, 3);
                pathingentity_movealongroute(&player->pathing_entity, false, walkDir);

                int extendedInfo = gbit(buf, 1);
                if (extendedInfo == 1) {
                    c->entityUpdateIds[c->entityUpdateCount++] = index;
                }
            } else if (op == 2) {
                c->player_ids[c->player_count++] = index;
                player->pathing_entity.cycle = _Client.loop_cycle;

                int walkDir = gbit(buf, 3);
                pathingentity_movealongroute(&player->pathing_entity, true, walkDir);
                int runDir = gbit(buf, 3);
                pathingentity_movealongroute(&player->pathing_entity, true, runDir);

                int extendedInfo = gbit(buf, 1);
                if (extendedInfo == 1) {
                    c->entityUpdateIds[c->entityUpdateCount++] = index;
                }
            } else if (op == 3) {
                c->entityRemovalIds[c->entityRemovalCount++] = index;
            }
        }
    }
}

void getPlayerNewVis(Client *c, int size, Packet *buf) {
    int index;
    while (buf->bit_pos + 10 < size * 8) {
        index = gbit(buf, 11);
        if (index == 2047) {
            break;
        }

        if (!c->players[index]) {
            c->players[index] = playerentity_new();
            if (c->player_appearance_buffer[index]) {
                playerentity_read(c->players[index], c->player_appearance_buffer[index]);
            }
        }

        c->player_ids[c->player_count++] = index;
        PlayerEntity *player = c->players[index];
        player->pathing_entity.cycle = _Client.loop_cycle;
        int dx = gbit(buf, 5);
        if (dx > 15) {
            dx -= 32;
        }
        int dz = gbit(buf, 5);
        if (dz > 15) {
            dz -= 32;
        }
        int jump = gbit(buf, 1);
        pathingentity_teleport(&player->pathing_entity, jump == 1, c->local_player->pathing_entity.pathTileX[0] + dx, c->local_player->pathing_entity.pathTileZ[0] + dz);

        int extendedInfo = gbit(buf, 1);
        if (extendedInfo == 1) {
            c->entityUpdateIds[c->entityUpdateCount++] = index;
        }
    }

    access_bytes(buf);
}

void getPlayerExtended(Client *c, Packet *buf, int size) {
    (void)size;
    for (int i = 0; i < c->entityUpdateCount; i++) {
        int index = c->entityUpdateIds[i];
        // see getNpcPosExtended's identical fix for the same rationale - c->players[index] was
        // dereferenced (via getPlayerExtended2) completely unconditionally with no NULL check, the
        // real cause of a crash confirmed to fire during combat (both npc and player extended-info
        // updates get frequent during combat, and both had this exact same gap).
        static PlayerEntity dummy_player;
        memset(&dummy_player, 0, sizeof(dummy_player));
        PlayerEntity *player = c->players[index] ? c->players[index] : &dummy_player;
        int mask = g1(buf);
        if ((mask & 0x80) == 128) {
            mask += g1(buf) << 8;
        }
        getPlayerExtended2(c, player, index, mask, buf);
    }
}

void getPlayerExtended2(Client *c, PlayerEntity *player, int index, int mask, Packet *buf) {
    player->pathing_entity.lastMask = mask;
    player->pathing_entity.lastMaskCycle = _Client.loop_cycle;

    if ((mask & 0x1) == 1) {
        int length = g1(buf);
        int8_t *data = calloc(length, sizeof(int8_t));
        Packet *appearance = packet_new(data, length);
        gdata(buf, length, 0, data);

        if (c->player_appearance_buffer[index]) {
            packet_free(c->player_appearance_buffer[index]);
        }
        c->player_appearance_buffer[index] = appearance;
        playerentity_read(player, appearance);
    }
    if ((mask & 0x2) == 2) {
        int seqId = g2(buf);
        // treat an out-of-range seqId the same as the existing "no animation" sentinel (65535->-1):
        // rev254 can send a seq id beyond Client3's loaded _SeqType.count, and indexing
        // _SeqType.instances[] with it (or with -1, before this fix) was an out-of-bounds/negative
        // array read - see the sentinel checks reordered below, which must run before any
        // _SeqType.instances[] access so an invalid id short-circuits before the dereference.
        if (seqId == 65535 || seqId < 0 || seqId >= _SeqType.count) {
            seqId = -1;
        }
        if (seqId == player->pathing_entity.primarySeqId) {
            player->pathing_entity.primarySeqLoop = 0;
        }
        int delay = g1(buf);
        // TODO: duplicatebehaviour!=0 assumed to mean "RESET" (restart from frame 0); verify polarity against real gameplay, see audit
        if (seqId == -1 || player->pathing_entity.primarySeqId == -1 || (seqId == player->pathing_entity.primarySeqId && _SeqType.instances[seqId]->duplicatebehaviour != 0) || _SeqType.instances[seqId]->priority > _SeqType.instances[player->pathing_entity.primarySeqId]->priority || _SeqType.instances[player->pathing_entity.primarySeqId]->priority == 0) {
            player->pathing_entity.primarySeqId = seqId;
            player->pathing_entity.primarySeqFrame = 0;
            player->pathing_entity.primarySeqCycle = 0;
            player->pathing_entity.primarySeqDelay = delay;
            player->pathing_entity.primarySeqLoop = 0;
        }
    }
    if ((mask & 0x4) == 4) {
        player->pathing_entity.targetId = g2(buf);
        if (player->pathing_entity.targetId == 65535) {
            player->pathing_entity.targetId = -1;
        }
    }
    if ((mask & 0x8) == 8) {
        strcpy(player->pathing_entity.chat, gjstr(buf));
        player->pathing_entity.chatColor = 0;
        player->pathing_entity.chatStyle = 0;
        player->pathing_entity.chatTimer = 150;
        client_add_message(c, 2, player->pathing_entity.chat, player->name);
    }
    if ((mask & 0x10) == 16) {
        player->pathing_entity.damage = g1(buf);
        player->pathing_entity.damageType = g1(buf);
        player->pathing_entity.combatCycle = _Client.loop_cycle + 400;
        player->pathing_entity.health = g1(buf);
        player->pathing_entity.totalHealth = g1(buf);
    }
    if ((mask & 0x20) == 32) {
        player->pathing_entity.targetTileX = g2(buf);
        player->pathing_entity.targetTileZ = g2(buf);
        player->pathing_entity.lastFaceX = player->pathing_entity.targetTileX;
        player->pathing_entity.lastFaceZ = player->pathing_entity.targetTileZ;
    }
    if ((mask & 0x40) == 64) {
        int colorEffect = g2(buf);
        int type = g1(buf);
        int length = g1(buf);
        int start = buf->pos;
        if (player->name[0]) {
            int64_t username = jstring_to_base37(player->name);
            bool ignored = false;
            if (type <= 1) {
                for (int i = 0; i < c->ignoreCount; i++) {
                    if (c->ignoreName37[i] == username) {
                        ignored = true;
                        break;
                    }
                }
            }
            if (!ignored && c->overrideChat == 0) {
                // try {
                char *uncompressed = wordpack_unpack(buf, length);
                wordfilter_filter(uncompressed);
                strcpy(player->pathing_entity.chat, uncompressed);
                player->pathing_entity.chatColor = colorEffect >> 8;
                player->pathing_entity.chatStyle = colorEffect & 0xff;
                player->pathing_entity.chatTimer = 150;
                if (type > 1) {
                    client_add_message(c, 1, uncompressed, player->name);
                } else {
                    client_add_message(c, 2, uncompressed, player->name);
                }
                free(uncompressed);
                // } catch (Exception ex) {
                // 	signlink.reporterror("cde2");
                // }
            }
        }
        buf->pos = start + length;
    }
    if ((mask & 0x100) == 256) {
        player->pathing_entity.spotanimId = g2(buf);
        int heightDelay = g4(buf);
        player->pathing_entity.spotanimOffset = heightDelay >> 16;
        player->pathing_entity.spotanimLastCycle = _Client.loop_cycle + (heightDelay & 0xffff);
        player->pathing_entity.spotanimFrame = 0;
        player->pathing_entity.spotanimCycle = 0;
        if (player->pathing_entity.spotanimLastCycle > _Client.loop_cycle) {
            player->pathing_entity.spotanimFrame = -1;
        }
        if (player->pathing_entity.spotanimId == 65535) {
            player->pathing_entity.spotanimId = -1;
        }
    }
    if ((mask & 0x200) == 512) {
        player->pathing_entity.forceMoveStartSceneTileX = g1(buf);
        player->pathing_entity.forceMoveStartSceneTileZ = g1(buf);
        player->pathing_entity.forceMoveEndSceneTileX = g1(buf);
        player->pathing_entity.forceMoveEndSceneTileZ = g1(buf);
        player->pathing_entity.forceMoveEndCycle = g2(buf) + _Client.loop_cycle;
        player->pathing_entity.forceMoveStartCycle = g2(buf) + _Client.loop_cycle;
        player->pathing_entity.forceMoveFaceDirection = g1(buf);
        player->pathing_entity.pathLength = 0;
        player->pathing_entity.pathTileX[0] = player->pathing_entity.forceMoveEndSceneTileX;
        player->pathing_entity.pathTileZ[0] = player->pathing_entity.forceMoveEndSceneTileZ;
    }
    if ((mask & 0x400) == 1024) {
        player->pathing_entity.damage2 = g1(buf);
        player->pathing_entity.damageType2 = g1(buf);
        player->pathing_entity.combatCycle2 = _Client.loop_cycle + 400;
        player->pathing_entity.health = g1(buf);
        player->pathing_entity.totalHealth = g1(buf);
    }
}

void getPlayer(Client *c, Packet *buf, int size) {
    c->entityRemovalCount = 0;
    c->entityUpdateCount = 0;

    getPlayerLocal(c, buf, size);
    getPlayerOldVis(c, buf, size);
    getPlayerNewVis(c, size, buf);
    getPlayerExtended(c, buf, size);

    for (int i = 0; i < c->entityRemovalCount; i++) {
        int index = c->entityRemovalIds[i];
        if (c->players[index]->pathing_entity.cycle != _Client.loop_cycle) {
            // Was `free(c->players[i])` - froze the loop counter's slot instead of the actual
            // removed player at `index` (the NPC-removal code a few hundred lines below this one
            // gets it right - free(c->npcs[index]) - confirming this was an isolated copy/paste
            // bug, not an intentional pattern). Two real, compounding bugs from one typo: every
            // real player removal permanently leaked its PlayerEntity (never freed, since
            // c->players[index] was set to NULL without freeing it first), while c->players[i]
            // (i is just 0/1/2/... the position within this tick's short removal list, an
            // unrelated small index almost always still occupied by a live player) got a genuine
            // double-free the next time any removal list also reached that same small i - each hit
            // corrupts the heap allocator's internal free-list further, which is entirely
            // consistent with the observed per-tick slowdown that gets worse over a session
            // (busier areas -> more removals/tick -> more corruption events) rather than a single
            // one-time cost.
            free(c->players[index]);
            c->players[index] = NULL;
        }
    }

    if (buf->pos != size) {
        rs2_error("Error packet size mismatch in getplayer pos:%d psize:%d\n", buf->pos, size);
        // signlink.reporterror("Error packet size mismatch in getplayer pos:" + buf.pos + " psize:" + size);
        // throw new RuntimeException("eek");
    }

    for (int index = 0; index < c->player_count; index++) {
        if (!c->players[c->player_ids[index]]) {
            rs2_error("%s null entry in pl list - pos:%d size:%d\n", c->username, index, c->player_count);
            // signlink.reporterror(c->username + " null entry in pl list - pos:" + index + " size:" + c->playerCount);
            // throw new RuntimeException("eek");
        }
    }
}

static void client_clear_caches(void) {
    lrucache_clear(_LocType.modelCacheStatic);
    lrucache_clear(_LocType.modelCacheDynamic);
    lrucache_clear(_NpcType.modelCache);
    lrucache_clear(_ObjType.modelCache);
    lrucache_clear(_ObjType.iconCache);
    playerentity_clear_model_cache();
    lrucache_clear(_SpotAnimType.modelCache);
    bump_allocator_reset();
}

#ifdef __PS2__
// Bisecting client_build_scene() stage by stage: the map-file read loop above was proven (via its
// own on-screen "All map reads done, building scene..." checkpoint surviving to a frozen screen)
// to always finish - the freeze is somewhere inside THIS function instead. It has several large,
// allocation-heavy stages (world_new, per-mapsquare bzip decompress into a 100KB scratch buffer,
// world_build's model/loc geometry into the scene bump arena) on a port that was already
// documented as tight on EE memory before this specific bug - one breadcrumb per stage says
// exactly which one is failing instead of continuing to guess at the whole function.
// PS2_CHECKPOINTS_ENABLED is now defined near the top of this file (before any use) - see that
// definition's comment for the full rationale.

void ps2_scene_checkpoint(Client *c, const char *label) {
#if !PS2_CHECKPOINTS_ENABLED
    (void)c;
    (void)label;
    return;
#else
    // 2026-09-14: rs2_log() (a real, unconditional boot.log write - genuine USB/BDM file I/O, no
    // throttle) REMOVED entirely. Two real, direct reasons: (1) boot.log has never once been
    // confirmed to actually survive to be read back this whole session ("boot.log never worked
    // properly") - it was providing zero verified value. (2) It's genuine, real I/O cost on every
    // single checkpoint call, and this project has now confirmed three separate times this session
    // that enough of that stacked into a tight spot becomes the actual cause of a freeze rather than
    // a way to observe one (the bzip heartbeat, the ps2_boot_progress() GS-sync in
    // world_load_locations(), and a cluster of checkpoints added to loctype_get_model() that
    // produced a real, reproducible, much-earlier freeze on an otherwise-identical fresh-login/same-
    // position retest). The on-screen draw below is the only diagnostic output this function
    // produces now - still real GS-flip cost, but a single distinct one, not two compounded I/O
    // paths per call.
    if (!c) {
        // world_build() runs before some callers have a fully set-up Client (and is reused from
        // contexts that only care about coarse logging that no longer exists) - nothing left to do
        // for a NULL c, so return before even computing heap_free.
        return;
    }
    int heap_free = mallinfo().fordblks;
    // heap_free is drawn on screen: given this whole project's history of OOM-adjacent bugs right
    // around scene-build memory boundaries, knowing whether heap_free was already critically low at
    // the LAST checkpoint before a freeze is exactly the data needed to confirm or rule out an
    // out-of-memory allocation failure as the cause, without needing another instrumented round trip.
    // 2026-09-14: added bump_allocator_used()/capacity() - every checkpoint so far has only ever shown
    // GENERAL heap free (mallinfo), never the SEPARATE 6MB scene bump arena that model_calculate_normals()
    // and friends actually allocate from (rs2_calloc/rs2_malloc with use_allocator=true go through
    // bump_alloc(), not malloc/calloc). heap_free staying healthy at every checkpoint this session
    // never actually ruled out the arena specifically being the thing that's tight - this closes that
    // gap for free on every single existing checkpoint.
    // 2026-09-14: switched to KB with explicit spacing after a real-hardware reading came back as an
    // ambiguous run-together digit string ("666076291456") that needed reverse-engineering against
    // the known capacity constant to parse - the old byte-precision format was too long for this
    // screen region and got visually clipped/hard to read. KB precision is more than enough here.
    char label_with_mem[96];
    snprintf(label_with_mem, sizeof(label_with_mem), "%s f%dK a%dK/%dK", label, heap_free / 1024,
              bump_allocator_used() / 1024, bump_allocator_capacity() / 1024);
    pixmap_bind(c->area_viewport);
    pix2d_fill_rect(0, 170, BLACK, 512, 20);
    drawStringCenter(c->font_plain12, 257, 181, label_with_mem, BLACK);
    drawStringCenter(c->font_plain12, 256, 180, label_with_mem, WHITE);
    pixmap_draw(c->area_viewport, 4, 4);
    platform_update_surface();
#endif
}

// 2026-09-14, later session: added after the patch-plan item-1/4 fixes AND hooking bus errors (see
// platform/ps2.c) still produced a totally silent hang on real hardware - no "Allocator full" screen,
// no "EE EXCEPTION" screen, identical freeze shape to every prior hang. That means the freeze point
// may now be somewhere ps2_scene_checkpoint() can no longer report on, since PS2_CHECKPOINTS_ENABLED
// is 0 and stays 0 - flipping that master switch back on would also reintroduce the dense per-object
// sampled checkpoints in loctype_get_model()/model_calculate_normals()/model_calculate_bounds_cylinder()/
// world_add_loc2(), the confirmed, repeated (3+ times) source of false freezes from stacked GS-flip
// I/O this session. This is a deliberately separate function, only for genuinely coarse, once-or-a-
// few-times-per-scene-build phase markers (at most ~15 draws per scene build, each with substantial
// real computation before/after it - nothing like the hundreds-to-thousands-of-calls density that
// caused the stacking problem), so it always draws regardless of the master switch. Reuses the same
// enriched heap_free/bump-arena message format as ps2_scene_checkpoint() above, at a different screen
// row (150 vs 170) so a phase marker and a later OOM/exception screen (rows 190/170) can coexist
// without stomping each other if both fire before the next photo. Only wired at true phase boundaries
// (see call sites in client_build_scene()/world_build()) - every dense per-object site is untouched
// and stays exactly as gated as it was.
#define PS2_PHASE_CHECKPOINTS_ENABLED 0
void ps2_phase_checkpoint(Client *c, const char *label) {
#if !PS2_PHASE_CHECKPOINTS_ENABLED
    (void)c;
    (void)label;
    return;
#else
    if (!c) {
        return;
    }
    int heap_free = mallinfo().fordblks;
    char label_with_mem[96];
    snprintf(label_with_mem, sizeof(label_with_mem), "%s f%dK a%dK/%dK", label, heap_free / 1024,
              bump_allocator_used() / 1024, bump_allocator_capacity() / 1024);
    pixmap_bind(c->area_viewport);
    pix2d_fill_rect(0, 150, BLACK, 512, 20);
    drawStringCenter(c->font_plain12, 257, 161, label_with_mem, BLACK);
    drawStringCenter(c->font_plain12, 256, 160, label_with_mem, WHITE);
    pixmap_draw(c->area_viewport, 4, 4);
    platform_update_surface();
#endif
}
#endif

static void client_build_scene(Client *c) {
    // try {
    c->minimap_level = -1;
    linklist_clear(c->merged_locations);
    linklist_clear(c->locList);
    linklist_clear(c->spotanims);
    linklist_clear(c->projectiles);
    pix3d_clear_texels();
    client_clear_caches();
    world3d_reset(c->scene);
    for (int level = 0; level < 4; level++) {
        collisionmap_reset(c->levelCollisionMap[level]);
    }
#ifdef __PS2__
    ps2_scene_checkpoint(c, "scene: reset done");
    ps2_phase_checkpoint(c, "scene: reset done");
#endif

    World *world = world_new(104, 104, c->levelHeightmap, c->levelTileFlags);
    _World.lowMemory = _World3D.lowMemory;
#ifdef __PS2__
    ps2_scene_checkpoint(c, "scene: world_new done");
    ps2_phase_checkpoint(c, "scene: world_new done");
#endif

    int maps = c->sceneMapIndexLength;

    for (int index = 0; index < maps; index++) {
        int mapsquareX = c->sceneMapIndex[index] >> 8;
        int mapsquareZ = c->sceneMapIndex[index] & 0xff;

        // underground pass check
        if (mapsquareX == 33 && mapsquareZ >= 71 && mapsquareZ <= 73) {
            _World.lowMemory = false;
            break;
        }
    }

    if (_World.lowMemory) {
        world3d_set_minlevel(c->scene, c->currentLevel);
    } else {
        world3d_set_minlevel(c->scene, 0);
    }

    int8_t *data = calloc(100000, sizeof(int8_t));
#ifdef __PS2__
    if (!data) {
        // Same "fail loudly instead of a silent NULL-pointer hardware fault" reasoning as the
        // platform_new() screen texture check - bzip_decompress() below would otherwise write
        // straight into a NULL scratch buffer.
        rs2_error("client_build_scene: calloc(100000) scratch buffer failed - out of EE RAM\n");
    }
    ps2_scene_checkpoint(c, "scene: scratch buffer allocated");
    ps2_phase_checkpoint(c, "scene: scratch buffer allocated");
#endif

    // NO_TIMEOUT
    p1isaac(c->out, 239); // NO_TIMEOUT
    for (int i = 0; i < maps; i++) {
        int x = (c->sceneMapIndex[i] >> 8) * 64 - c->sceneBaseTileX;
        int z = (c->sceneMapIndex[i] & 0xff) * 64 - c->sceneBaseTileZ;
        int8_t *src = c->sceneMapLandData[i];

        if (src) {
            Packet *buf = packet_new(src, c->sceneMapLandDataIndexLength[i]);
            int length = g4(buf);
#ifdef __PS2__
            // bzip_decompress() writes into `data` (the fixed 100000-byte scratch buffer above) with
            // no capacity parameter of its own - it trusts the caller to have sized the destination
            // correctly. That's always held for land data in practice (fixed 64x64 heightmap per
            // square), but nothing actually enforces it. A silent overflow here would corrupt
            // whatever heap chunk follows `data` - on PS2's much smaller, more tightly packed heap
            // (vs. a 64-bit desktop build of the same source) that's far more likely to corrupt
            // something load-bearing on the very next allocation instead of quietly going unnoticed.
            if (length > 100000) {
                rs2_error("client_build_scene: land data for mapsquare %d_%d decompresses to %d bytes - exceeds "
                          "100000-byte scratch buffer, skipping to avoid heap corruption\n",
                          c->sceneMapIndex[i] >> 8, c->sceneMapIndex[i] & 0xff, length);
                free(buf);
                continue;
            }
#endif
#ifdef __PS2__
            // NULL here, not `c`: land decode has never once failed to reach "scene: land decode
            // done" in any real-hardware test so far - only loc decode (below) needs the full
            // on-screen checkpoint treatment. Still gets log-only diagnostics for free (same
            // ps2_scene_checkpoint() calls inside bzip_decompress(), just skipping the screen draw).
            bzip_decompress(data, src, c->sceneMapLandDataIndexLength[i] - 4, 4, NULL, 100000);
#else
            bzip_decompress(data, src, c->sceneMapLandDataIndexLength[i] - 4, 4);
#endif
            free(buf);
            world_load_ground(world, (c->sceneCenterZoneX - 6) * 8, (c->sceneCenterZoneZ - 6) * 8, x, z, data, length);
        } else if (c->sceneCenterZoneZ < 800) {
            clearLandscape(world, z, x, 64, 64);
        }
    }
#ifdef __PS2__
    ps2_scene_checkpoint(c, "scene: land decode done");
    // This is deliberately the next single coarse marker, not an additional
    // per-object diagnostic. In PS2_DEFER_STATIC_LOCATIONS builds it proves the
    // executable reached the location stage without doing location I/O.
    ps2_phase_checkpoint(c, "scene: loc stream skipped");
#endif

    // NO_TIMEOUT
    p1isaac(c->out, 239); // NO_TIMEOUT
    for (int i = 0; i < maps; i++) {
        int8_t *src = c->sceneMapLocData[i];
        if (src) {
            Packet *buf = packet_new(src, c->sceneMapLocDataIndexLength[i]);
            int length = g4(buf);
#ifdef __PS2__
            // Same overflow guard as the land loop above, and the prime suspect for THIS specific
            // hang (loc decode is variable-length per square, unlike land's fixed 64x64 write, so a
            // single unusually loc-dense square - exactly what a spawn town square would be - is the
            // one case where the shared 100000-byte scratch buffer could plausibly be too small).
            if (length > 100000) {
                rs2_error("client_build_scene: loc data for mapsquare %d_%d decompresses to %d bytes - exceeds "
                          "100000-byte scratch buffer, skipping to avoid heap corruption\n",
                          c->sceneMapIndex[i] >> 8, c->sceneMapIndex[i] & 0xff, length);
                free(buf);
                continue;
            }
            // Real-hardware testing narrowed a hang to somewhere between "scene: land decode done"
            // and this loop's own post-decompress checkpoint below - which only ever showed the
            // former, meaning bzip_decompress() itself (or something in its immediate setup: the
            // malloc'd `headered` buffer, the bd struct, or its dbuf) never returned. This checkpoint,
            // right before the call, is the only way to tell "never even got here" (would mean the
            // hang is in packet_new()/g4() instead, upstream of this line) from "entered
            // bzip_decompress and never came back" (the leading theory - a heap allocation inside it
            // failing on this platform's tight/fragmented EE memory, or a genuine decode-loop bug
            // exposed only by loc data's size/shape) once the next boot's photo is compared against
            // whether the POST-decompress checkpoint below also shows up.
            char loc_pre_msg[80];
            snprintf(loc_pre_msg, sizeof(loc_pre_msg), "loc #%d/%d %d_%d pre-decompress (len=%d)", i + 1, maps,
                     c->sceneMapIndex[i] >> 8, c->sceneMapIndex[i] & 0xff, length);
            ps2_scene_checkpoint(c, loc_pre_msg);
            bzip_decompress(data, src, c->sceneMapLocDataIndexLength[i] - 4, 4, c, 100000);
            {
                char loc_bzip_done_msg[72];
                snprintf(loc_bzip_done_msg, sizeof(loc_bzip_done_msg), "loc #%d/%d bzip returned", i + 1, maps);
                ps2_scene_checkpoint(c, loc_bzip_done_msg);
            }
#else
            bzip_decompress(data, src, c->sceneMapLocDataIndexLength[i] - 4, 4);
#endif
            free(buf);
#ifdef __PS2__
            {
                char loc_buf_free_msg[72];
                snprintf(loc_buf_free_msg, sizeof(loc_buf_free_msg), "loc #%d/%d packet freed", i + 1, maps);
                ps2_scene_checkpoint(c, loc_buf_free_msg);
            }
#endif
            int x = (c->sceneMapIndex[i] >> 8) * 64 - c->sceneBaseTileX;
            int z = (c->sceneMapIndex[i] & 0xff) * 64 - c->sceneBaseTileZ;
#if defined(__PS2__) && PS2_CHECKPOINTS_ENABLED
            // 2026-09-14: this site bypassed ps2_scene_checkpoint() entirely - its own raw draw call
            // PLUS an unconditional, unthrottled rs2_log() (real USB/BDM file I/O every single
            // mapsquare) - so the master PS2_CHECKPOINTS_ENABLED switch didn't cover it. Found only
            // because disabling every checkpoint still left a real-hardware hang reported right here.
            // Gated the same way as everything else now.
            char loc_msg[64];
            snprintf(loc_msg, sizeof(loc_msg), "loc decode #%d/%d (mapsquare %d_%d)", i + 1, maps, c->sceneMapIndex[i] >> 8,
                     c->sceneMapIndex[i] & 0xff);
            rs2_log("%s: bump=%d/%d heap_used=%d heap_free=%d\n", loc_msg, bump_allocator_used(), bump_allocator_capacity(),
                     mallinfo().uordblks, mallinfo().fordblks);
            pixmap_bind(c->area_viewport);
            pix2d_fill_rect(0, 170, BLACK, 512, 20);
            drawStringCenter(c->font_plain12, 257, 181, loc_msg, BLACK);
            drawStringCenter(c->font_plain12, 256, 180, loc_msg, WHITE);
            pixmap_draw(c->area_viewport, 4, 4);
            platform_update_surface();
#endif
            world_load_locations(world, c->scene, c->locList, c->levelCollisionMap, data, length, x, z);
#ifdef __PS2__
            // Coarse before/after for this specific call (the "loc #N/M pre-decompress" checkpoint
            // above already covers "before" for real; this is the "after" half) - see world.c's
            // PS2_LOC_DECODE_ONLY comment for why this matters right now: without this, "it returned"
            // vs. "it's still hung in there" is only inferable from whether the NEXT square's
            // pre-decompress message ever appears, which is a much less direct signal to read off a
            // photo.
            ps2_scene_checkpoint(c, "world_load_locations returned");
            {
                char loc_load_done_msg[72];
                snprintf(loc_load_done_msg, sizeof(loc_load_done_msg), "loc #%d/%d load returned", i + 1, maps);
                ps2_scene_checkpoint(c, loc_load_done_msg);
            }
#endif
        }
    }
#ifdef __PS2__
    ps2_scene_checkpoint(c, "scene: loc decode done");
    ps2_phase_checkpoint(c, "scene: loc decode done");
#endif

    free(data);

    // NO_TIMEOUT
    p1isaac(c->out, 239); // NO_TIMEOUT
#ifdef __PS2__
    world_build(world, c->scene, c->levelCollisionMap, c);
#else
    world_build(world, c->scene, c->levelCollisionMap);
#endif
    pixmap_bind(c->area_viewport);
#ifdef __PS2__
    ps2_scene_checkpoint(c, "scene: world_build done");
    ps2_phase_checkpoint(c, "scene: world_build done");
#endif

    // NO_TIMEOUT
    p1isaac(c->out, 239); // NO_TIMEOUT
    for (LocEntity *loc = (LocEntity *)linklist_head(c->locList); loc; loc = (LocEntity *)linklist_next(c->locList)) {
        if ((c->levelTileFlags[1][loc->x][loc->z] & 0x2) == 2) {
            loc->level--;
            if (loc->level < 0) {
                linkable_unlink(&loc->link);
                free(loc);
            }
        }
    }

    for (int x = 0; x < 104; x++) {
        for (int z = 0; z < 104; z++) {
            sortObjStacks(c, x, z);
        }
    }
#ifdef __PS2__
    ps2_scene_checkpoint(c, "scene: obj stacks sorted");
    ps2_phase_checkpoint(c, "scene: obj stacks sorted");
#endif

    for (LocAddEntity *loc = (LocAddEntity *)linklist_head(c->spawned_locations); loc; loc = (LocAddEntity *)linklist_next(c->spawned_locations)) {
        addLoc(c, loc->plane, loc->x, loc->z, loc->locIndex, loc->angle, loc->shape, loc->layer);
    }
    // } catch (Exception ignored) {
    // }

    lrucache_clear(_LocType.modelCacheStatic);
    pix3d_init_pool(PIX3D_POOL_COUNT);
    world_free(world);
#ifdef __PS2__
    ps2_scene_checkpoint(c, "scene: build_scene complete");
    ps2_phase_checkpoint(c, "scene: build_scene complete");
#endif
}

void drawMinimapLoc(Client *c, int tileX, int tileZ, int level, int wallRgb, int doorRgb) {
    int bitset = world3d_get_wallbitset(c->scene, level, tileX, tileZ);
    if (bitset != 0) {
        int info = world3d_get_info(c->scene, level, tileX, tileZ, bitset);
        int angle = info >> 6 & 0x3;
        int shape = info & 0x1f;
        int rgb = wallRgb;
        if (bitset > 0) {
            rgb = doorRgb;
        }

        int *dst = c->image_minimap->pixels;
        int offset = tileX * 4 + (104 - 1 - tileZ) * 512 * 4 + 24624;
        int locId = bitset >> 14 & 0x7fff;

        LocType *loc = loctype_get(locId);
        if (loc->mapscene == -1) {
            if (shape == WALL_STRAIGHT || shape == WALL_L) {
                if (angle == 0) {
                    dst[offset] = rgb;
                    dst[offset + 512] = rgb;
                    dst[offset + 1024] = rgb;
                    dst[offset + 1536] = rgb;
                } else if (angle == 1) {
                    dst[offset] = rgb;
                    dst[offset + 1] = rgb;
                    dst[offset + 2] = rgb;
                    dst[offset + 3] = rgb;
                } else if (angle == 2) {
                    dst[offset + 3] = rgb;
                    dst[offset + 3 + 512] = rgb;
                    dst[offset + 3 + 1024] = rgb;
                    dst[offset + 3 + 1536] = rgb;
                } else if (angle == 3) {
                    dst[offset + 1536] = rgb;
                    dst[offset + 1536 + 1] = rgb;
                    dst[offset + 1536 + 2] = rgb;
                    dst[offset + 1536 + 3] = rgb;
                }
            }

            if (shape == WALL_SQUARECORNER) {
                if (angle == 0) {
                    dst[offset] = rgb;
                } else if (angle == 1) {
                    dst[offset + 3] = rgb;
                } else if (angle == 2) {
                    dst[offset + 3 + 1536] = rgb;
                } else if (angle == 3) {
                    dst[offset + 1536] = rgb;
                }
            }

            if (shape == WALL_L) {
                if (angle == 3) {
                    dst[offset] = rgb;
                    dst[offset + 512] = rgb;
                    dst[offset + 1024] = rgb;
                    dst[offset + 1536] = rgb;
                } else if (angle == 0) {
                    dst[offset] = rgb;
                    dst[offset + 1] = rgb;
                    dst[offset + 2] = rgb;
                    dst[offset + 3] = rgb;
                } else if (angle == 1) {
                    dst[offset + 3] = rgb;
                    dst[offset + 3 + 512] = rgb;
                    dst[offset + 3 + 1024] = rgb;
                    dst[offset + 3 + 1536] = rgb;
                } else if (angle == 2) {
                    dst[offset + 1536] = rgb;
                    dst[offset + 1536 + 1] = rgb;
                    dst[offset + 1536 + 2] = rgb;
                    dst[offset + 1536 + 3] = rgb;
                }
            }
        } else {
            Pix8 *scene = c->image_mapscene[loc->mapscene];
            if (scene) {
                int offsetX = (loc->width * 4 - scene->width) / 2;
                int offsetY = (loc->length * 4 - scene->height) / 2;
                pix8_draw(scene, tileX * 4 + 48 + offsetX, (104 - tileZ - loc->length) * 4 + offsetY + 48);
            }
        }
    }

    bitset = world3d_get_locbitset(c->scene, level, tileX, tileZ);
    if (bitset != 0) {
        int info = world3d_get_info(c->scene, level, tileX, tileZ, bitset);
        int angle = info >> 6 & 0x3;
        int shape = info & 0x1f;
        int locId = bitset >> 14 & 0x7fff;
        LocType *loc = loctype_get(locId);

        if (loc->mapscene != -1) {
            Pix8 *scene = c->image_mapscene[loc->mapscene];
            if (scene) {
                int offsetX = (loc->width * 4 - scene->width) / 2;
                int offsetY = (loc->length * 4 - scene->height) / 2;
                pix8_draw(scene, tileX * 4 + 48 + offsetX, (104 - tileZ - loc->length) * 4 + offsetY + 48);
            }
        } else if (shape == WALL_DIAGONAL) {
            int rgb = 0xeeeeee;
            if (bitset > 0) {
                rgb = 0xee0000;
            }

            int *dst = c->image_minimap->pixels;
            int offset = tileX * 4 + (104 - 1 - tileZ) * 512 * 4 + 24624;

            if (angle == 0 || angle == 2) {
                dst[offset + 1536] = rgb;
                dst[offset + 1024 + 1] = rgb;
                dst[offset + 512 + 2] = rgb;
                dst[offset + 3] = rgb;
            } else {
                dst[offset] = rgb;
                dst[offset + 512 + 1] = rgb;
                dst[offset + 1024 + 2] = rgb;
                dst[offset + 1536 + 3] = rgb;
            }
        }
    }

    bitset = world3d_get_grounddecorationbitset(c->scene, level, tileX, tileZ);
    if (bitset != 0) {
        int locId = bitset >> 14 & 0x7fff;
        LocType *loc = loctype_get(locId);
        if (loc->mapscene != -1) {
            Pix8 *scene = c->image_mapscene[loc->mapscene];
            if (scene) {
                int offsetX = (loc->width * 4 - scene->width) / 2;
                int offsetY = (loc->length * 4 - scene->height) / 2;
                pix8_draw(scene, tileX * 4 + 48 + offsetX, (104 - tileZ - loc->length) * 4 + offsetY + 48);
            }
        }
    }
}

void createMinimap(Client *c, int level) {
#if defined(__PS2__) && PS2_DISABLE_MINIMAP
    (void)c;
    (void)level;
    return;
#endif
    int *pixels = c->image_minimap->pixels;
    int length = c->image_minimap->width * c->image_minimap->height;
    for (int i = 0; i < length; i++) {
        pixels[i] = 0;
    }

    for (int z = 1; z < 104 - 1; z++) {
        int offset = (104 - 1 - z) * 512 * 4 + 24628;

        for (int x = 1; x < 104 - 1; x++) {
            if ((c->levelTileFlags[level][x][z] & 0x18) == 0) {
                world3d_draw_minimaptile(c->scene, level, x, z, pixels, offset, 512);
            }

            if (level < 3 && (c->levelTileFlags[level + 1][x][z] & 0x8) != 0) {
                world3d_draw_minimaptile(c->scene, level + 1, x, z, pixels, offset, 512);
            }

            offset += 4;
        }
    }

    int wallRgb = (((int)(jrand() * 20.0) + 238 - 10) << 16) + (((int)(jrand() * 20.0) + 238 - 10) << 8) + (int)(jrand() * 20.0) + 238 - 10;
    int doorRgb = ((int)(jrand() * 20.0) + 238 - 10) << 16;

    pix24_bind(c->image_minimap);

    for (int z = 1; z < 104 - 1; z++) {
        for (int x = 1; x < 104 - 1; x++) {
            if ((c->levelTileFlags[level][x][z] & 0x18) == 0) {
                drawMinimapLoc(c, x, z, level, wallRgb, doorRgb);
            }

            if (level < 3 && (c->levelTileFlags[level + 1][x][z] & 0x8) != 0) {
                drawMinimapLoc(c, x, z, level + 1, wallRgb, doorRgb);
            }
        }
    }

    pixmap_bind(c->area_viewport);
    c->activeMapFunctionCount = 0;

    for (int x = 0; x < 104; x++) {
        for (int z = 0; z < 104; z++) {
            int bitset = world3d_get_grounddecorationbitset(c->scene, c->currentLevel, x, z);
            if (bitset == 0) {
                continue;
            }

            bitset = bitset >> 14 & 0x7fff;

            int func = loctype_get(bitset)->mapfunction;
            if (func < 0) {
                continue;
            }

            int stx = x;
            int stz = z;

            if (func != 22 && func != 29 && func != 34 && func != 36 && func != 46 && func != 47 && func != 48) {
                int8_t maxX = 104;
                int8_t maxZ = 104;
                int **flags = c->levelCollisionMap[c->currentLevel]->flags;

                for (int i = 0; i < 10; i++) {
                    int random = (int)(jrand() * 4.0);
                    if (random == 0 && stx > 0 && stx > x - 3 && (flags[stx - 1][stz] & 0x280108) == 0) {
                        stx--;
                    }

                    if (random == 1 && stx < maxX - 1 && stx < x + 3 && (flags[stx + 1][stz] & 0x280180) == 0) {
                        stx++;
                    }

                    if (random == 2 && stz > 0 && stz > z - 3 && (flags[stx][stz - 1] & 0x280102) == 0) {
                        stz--;
                    }

                    if (random == 3 && stz < maxZ - 1 && stz < z + 3 && (flags[stx][stz + 1] & 0x280120) == 0) {
                        stz++;
                    }
                }
            }

            c->activeMapFunctions[c->activeMapFunctionCount] = c->image_mapfunction[func];
            c->activeMapFunctionX[c->activeMapFunctionCount] = stx;
            c->activeMapFunctionZ[c->activeMapFunctionCount] = stz;
            c->activeMapFunctionCount++;
        }
    }
}

void closeInterfaces(Client *c) {
    // CLOSE_MODAL
    p1isaac(c->out, 58); // CLOSE_MODAL

    if (c->sidebar_interface_id != -1) {
        c->sidebar_interface_id = -1;
        c->redraw_sidebar = true;
        c->pressed_continue_option = false;
        c->redraw_sideicons = true;
    }

    if (c->chat_interface_id != -1) {
        c->chat_interface_id = -1;
        c->redraw_chatback = true;
        c->pressed_continue_option = false;
    }

    c->viewport_interface_id = -1;
}

void addLoc(Client *c, int level, int x, int z, int id, int angle, int shape, int layer) {
    if (x < 1 || z < 1 || x > 102 || z > 102) {
        return;
    }

    if (_Client.lowmem && level != c->currentLevel) {
        return;
    }

    int bitset = 0;

    if (layer == 0) {
        bitset = world3d_get_wallbitset(c->scene, level, x, z);
    }

    if (layer == 1) {
        bitset = world3d_get_walldecorationbitset(c->scene, level, z, x);
    }

    if (layer == 2) {
        bitset = world3d_get_locbitset(c->scene, level, x, z);
    }

    if (layer == 3) {
        bitset = world3d_get_grounddecorationbitset(c->scene, level, x, z);
    }

    if (bitset != 0) {
        int otherInfo = world3d_get_info(c->scene, level, x, z, bitset);
        int otherId = bitset >> 14 & 0x7fff;
        int otherShape = otherInfo & 0x1f;
        int otherRotation = otherInfo >> 6;

        if (layer == 0) {
            world3d_remove_wall(c->scene, level, x, z, 1);
            LocType *type = loctype_get(otherId);

            if (type->blockwalk) {
                collisionmap_del_wall(c->levelCollisionMap[level], x, z, otherShape, otherRotation, type->blockrange);
            }
        }

        if (layer == 1) {
            world3d_remove_walldecoration(c->scene, level, x, z);
        }

        if (layer == 2) {
            world3d_remove_loc(c->scene, level, x, z);
            LocType *type = loctype_get(otherId);

            if (x + type->width > 104 - 1 || z + type->width > 104 - 1 || x + type->length > 104 - 1 || z + type->length > 104 - 1) {
                return;
            }

            if (type->blockwalk) {
                collisionmap_del_loc(c->levelCollisionMap[level], x, z, type->width, type->length, otherRotation, type->blockrange);
            }
        }

        if (layer == 3) {
            world3d_remove_grounddecoration(c->scene, level, x, z);
            LocType *type = loctype_get(otherId);

            if (type->blockwalk && type->active) {
                collisionmap_remove_blocked(c->levelCollisionMap[level], x, z);
            }
        }
    }

    if (id >= 0) {
        int tileLevel = level;

        if (level < 3 && (c->levelTileFlags[1][x][z] & 0x2) == 2) {
            tileLevel = level + 1;
        }

        world_add_loc(level, x, z, c->scene, c->levelHeightmap, c->locList, c->levelCollisionMap[level], id, shape, angle, tileLevel);
    }
}

void sortObjStacks(Client *c, int x, int z) {
    LinkList *objStacks = c->level_obj_stacks[c->currentLevel][x][z];
    if (!objStacks) {
        world3d_remove_objstack(c->scene, c->currentLevel, x, z);
        return;
    }

    int topCost = -99999999;
    ObjStackEntity *topObj = NULL;

    for (ObjStackEntity *obj = (ObjStackEntity *)linklist_head(objStacks); obj != NULL; obj = (ObjStackEntity *)linklist_next(objStacks)) {
        ObjType *type = objtype_get(obj->index);
        int cost = type->cost;

        if (type->stackable) {
            cost *= obj->count + 1;
        }

        if (cost > topCost) {
            topCost = cost;
            topObj = obj;
        }
    }

    linklist_add_head(objStacks, &topObj->link);

    int bottomObjId = -1;
    int middleObjId = -1;
    int bottomObjCount = 0;
    int middleObjCount = 0;
    for (ObjStackEntity *obj = (ObjStackEntity *)linklist_head(objStacks); obj; obj = (ObjStackEntity *)linklist_next(objStacks)) {
        if (obj->index != topObj->index && bottomObjId == -1) {
            bottomObjId = obj->index;
            bottomObjCount = obj->count;
        }

        if (obj->index != topObj->index && obj->index != bottomObjId && middleObjId == -1) {
            middleObjId = obj->index;
            middleObjCount = obj->count;
        }
    }

    Model *bottomObj = NULL;
    if (bottomObjId != -1) {
        bottomObj = objtype_get_interfacemodel(objtype_get(bottomObjId), bottomObjCount, true);
    }

    Model *middleObj = NULL;
    if (middleObjId != -1) {
        middleObj = objtype_get_interfacemodel(objtype_get(middleObjId), middleObjCount, true);
    }

    int bitset = x + (z << 7) + 1610612736;
    ObjType *type = objtype_get(topObj->index);
    world3d_add_objstack(c->scene, x, z, getHeightmapY(c, c->currentLevel, x * 128 + 64, z * 128 + 64), c->currentLevel, bitset, objtype_get_interfacemodel(type, topObj->count, true), middleObj, bottomObj);
}

void readZonePacket(Client *c, Packet *buf, int opcode) {
    int pos = g1(buf);
    int x = c->baseX + (pos >> 4 & 0x7);
    int z = c->baseZ + (pos & 0x7);

    if (opcode == 70 || opcode == 88) {
        // LOC_ADD_CHANGE || LOC_DEL
        int info = g1(buf);
        int shape = info >> 2;
        int angle = info & 0x3;
        int layer = LOC_SHAPE_TO_LAYER[shape];
        int id;
        if (opcode == 88) {
            id = -1;
        } else {
            id = g2(buf);
        }
        if (x >= 0 && z >= 0 && x < 104 && z < 104) {
            LocAddEntity *loc = NULL;
            for (LocAddEntity *next = (LocAddEntity *)linklist_head(c->spawned_locations); next != NULL; next = (LocAddEntity *)linklist_next(c->spawned_locations)) {
                if (next->plane == c->currentLevel && next->x == x && next->z == z && next->layer == layer) {
                    loc = next;
                    break;
                }
            }
            if (!loc) {
                int bitset = 0;
                int otherId = -1;
                int otherShape = 0;
                int otherAngle = 0;
                if (layer == 0) {
                    bitset = world3d_get_wallbitset(c->scene, c->currentLevel, x, z);
                }
                if (layer == 1) {
                    bitset = world3d_get_walldecorationbitset(c->scene, c->currentLevel, z, x);
                }
                if (layer == 2) {
                    bitset = world3d_get_locbitset(c->scene, c->currentLevel, x, z);
                }
                if (layer == 3) {
                    bitset = world3d_get_grounddecorationbitset(c->scene, c->currentLevel, x, z);
                }
                if (bitset != 0) {
                    int otherInfo = world3d_get_info(c->scene, c->currentLevel, x, z, bitset);
                    otherId = bitset >> 14 & 0x7fff;
                    otherShape = otherInfo & 0x1f;
                    otherAngle = otherInfo >> 6;
                }
                loc = calloc(1, sizeof(LocAddEntity));
                loc->plane = c->currentLevel;
                loc->layer = layer;
                loc->x = x;
                loc->z = z;
                loc->lastLocIndex = otherId;
                loc->lastShape = otherShape;
                loc->lastAngle = otherAngle;
                linklist_add_tail(c->spawned_locations, &loc->link);
            }
            loc->locIndex = id;
            loc->shape = shape;
            loc->angle = angle;
            addLoc(c, c->currentLevel, x, z, id, angle, shape, layer);
        }
    } else if (opcode == 30) {
        // LOC_ANIM
        int info = g1(buf);
        int shape = info >> 2;
        int layer = LOC_SHAPE_TO_LAYER[shape];
        int id = g2(buf);
        if (x >= 0 && z >= 0 && x < 104 && z < 104) {
            int bitset = 0;
            if (layer == 0) {
                bitset = world3d_get_wallbitset(c->scene, c->currentLevel, x, z);
            }
            if (layer == 1) {
                bitset = world3d_get_walldecorationbitset(c->scene, c->currentLevel, z, x);
            }
            if (layer == 2) {
                bitset = world3d_get_locbitset(c->scene, c->currentLevel, x, z);
            }
            if (layer == 3) {
                bitset = world3d_get_grounddecorationbitset(c->scene, c->currentLevel, x, z);
            }
            // LOC_ANIM's seq id is read raw off the network with no sentinel normalization at all
            // (unlike the player/npc extended-info seqIds above) - guard it the same way before
            // indexing _SeqType.instances[].
            if (bitset != 0 && id >= 0 && id < _SeqType.count) {
                LocEntity *loc = locentity_new(bitset >> 14 & 0x7fff, c->currentLevel, layer, x, z, _SeqType.instances[id], false);
                linklist_add_tail(c->locList, &loc->link);
            }
        }
    } else if (opcode == 120) {
        // OBJ_ADD
        int id = g2(buf);
        int count = g2(buf);
        if (x >= 0 && z >= 0 && x < 104 && z < 104) {
            ObjStackEntity *obj = objstackentity_new();
            obj->index = id;
            obj->count = count;
            if (!c->level_obj_stacks[c->currentLevel][x][z]) {
                c->level_obj_stacks[c->currentLevel][x][z] = linklist_new();
            }
            linklist_add_tail(c->level_obj_stacks[c->currentLevel][x][z], &obj->link);
            sortObjStacks(c, x, z);
        }
    } else if (opcode == 115) {
        // OBJ_DEL
        int id = g2(buf);
        if (x >= 0 && z >= 0 && x < 104 && z < 104) {
            LinkList *list = c->level_obj_stacks[c->currentLevel][x][z];
            if (list) {
                for (ObjStackEntity *next = (ObjStackEntity *)linklist_head(list); next; next = (ObjStackEntity *)linklist_next(list)) {
                    if (next->index == (id & 0x7fff)) {
                        linkable_unlink(&next->link);
                        free(next);
                        break;
                    }
                }
                if (!linklist_head(list)) {
                    linklist_free(c->level_obj_stacks[c->currentLevel][x][z]);
                    c->level_obj_stacks[c->currentLevel][x][z] = NULL;
                }
                sortObjStacks(c, x, z);
            }
        }
    } else if (opcode == 37) {
        // MAP_PROJANIM
        int dx = x + g1b(buf);
        int dz = z + g1b(buf);
        int target = g2b(buf);
        int spotanim = g2(buf);
        int srcHeight = g1(buf);
        int dstHeight = g1(buf);
        int startDelay = g2(buf);
        int endDelay = g2(buf);
        int peak = g1(buf);
        int arc = g1(buf);
        if (x >= 0 && z >= 0 && x < 104 && z < 104 && dx >= 0 && dz >= 0 && dx < 104 && dz < 104) {
            x = x * 128 + 64;
            z = z * 128 + 64;
            dx = dx * 128 + 64;
            dz = dz * 128 + 64;
            ProjectileEntity *proj = projectileentity_new(spotanim, c->currentLevel, x, getHeightmapY(c, c->currentLevel, x, z) - srcHeight, z, startDelay + _Client.loop_cycle, endDelay + _Client.loop_cycle, peak, arc, target, dstHeight);
            projectileentity_update_velocity(proj, dx, getHeightmapY(c, c->currentLevel, dx, dz) - dstHeight, dz, startDelay + _Client.loop_cycle);
            linklist_add_tail(c->projectiles, &proj->entity.link);
        }
    } else if (opcode == 114) {
        // MAP_ANIM
        int id = g2(buf);
        int height = g1(buf);
        int delay = g2(buf);
        if (x >= 0 && z >= 0 && x < 104 && z < 104) {
            x = x * 128 + 64;
            z = z * 128 + 64;
            SpotAnimEntity *spotanim = spotanimentity_new(id, c->currentLevel, x, z, getHeightmapY(c, c->currentLevel, x, z) - height, _Client.loop_cycle, delay);
            linklist_add_tail(c->spotanims, &spotanim->entity.link);
        }
    } else if (opcode == 8) {
        // OBJ_REVEAL
        int id = g2(buf);
        int count = g2(buf);
        int receiver = g2(buf);
        if (x >= 0 && z >= 0 && x < 104 && z < 104 && receiver != c->local_pid) {
            ObjStackEntity *obj = objstackentity_new();
            obj->index = id;
            obj->count = count;
            if (!c->level_obj_stacks[c->currentLevel][x][z]) {
                c->level_obj_stacks[c->currentLevel][x][z] = linklist_new();
            }
            linklist_add_tail(c->level_obj_stacks[c->currentLevel][x][z], &obj->link);
            sortObjStacks(c, x, z);
        }
    } else if (opcode == 218) {
        // LOC_MERGE
        int info = g1(buf);
        int shape = info >> 2;
        int angle = info & 0x3;
        int layer = LOC_SHAPE_TO_LAYER[shape];
        int id = g2(buf);
        int start = g2(buf);
        int end = g2(buf);
        int pid = g2(buf);
        int8_t east = g1b(buf);
        int8_t south = g1b(buf);
        int8_t west = g1b(buf);
        int8_t north = g1b(buf);

        PlayerEntity *player;
        if (pid == c->local_pid) {
            player = c->local_player;
        } else {
            player = c->players[pid];
        }

        if (player) {
            LocMergeEntity *loc1 = locmergeentity_new(c->currentLevel, layer, x, z, -1, angle, shape, start + _Client.loop_cycle);
            linklist_add_tail(c->merged_locations, &loc1->link);

            LocMergeEntity *loc2 = locmergeentity_new(c->currentLevel, layer, x, z, id, angle, shape, end + _Client.loop_cycle);
            linklist_add_tail(c->merged_locations, &loc2->link);

            int y0 = c->levelHeightmap[c->currentLevel][x][z];
            int y1 = c->levelHeightmap[c->currentLevel][x + 1][z];
            int y2 = c->levelHeightmap[c->currentLevel][x + 1][z + 1];
            int y3 = c->levelHeightmap[c->currentLevel][x][z + 1];
            LocType *loc = loctype_get(id);

            player->locStartCycle = start + _Client.loop_cycle;
            player->locStopCycle = end + _Client.loop_cycle;
            player->locModel = loctype_get_model(loc, shape, angle, y0, y1, y2, y3, -1);

            int width = loc->width;
            int height = loc->length;
            if (angle == 1 || angle == 3) {
                width = loc->length;
                height = loc->width;
            }

            player->locOffsetX = x * 128 + width * 64;
            player->locOffsetZ = z * 128 + height * 64;
            player->locOffsetY = getHeightmapY(c, c->currentLevel, player->locOffsetX, player->locOffsetZ);

            int8_t tmp;
            if (east > west) {
                tmp = east;
                east = west;
                west = tmp;
            }

            if (south > north) {
                tmp = south;
                south = north;
                north = tmp;
            }

            player->minTileX = x + east;
            player->maxTileX = x + west;
            player->minTileZ = z + south;
            player->maxTileZ = z + north;
        }
    } else if (opcode == 98) {
        // OBJ_COUNT
        int id = g2(buf);
        int oldCount = g2(buf);
        int newCount = g2(buf);
        if (x >= 0 && z >= 0 && x < 104 && z < 104) {
            LinkList *list = c->level_obj_stacks[c->currentLevel][x][z];
            if (list) {
                for (ObjStackEntity *next = (ObjStackEntity *)linklist_head(list); next; next = (ObjStackEntity *)linklist_next(list)) {
                    if (next->index == (id & 0x7fff) && next->count == oldCount) {
                        next->count = newCount;
                        break;
                    }
                }
                sortObjStacks(c, x, z);
            }
        }
    }
}

void getNpcPos(Client *c, Packet *buf, int size) {
    c->entityRemovalCount = 0;
    c->entityUpdateCount = 0;

    getNpcPosOldVis(c, buf, size);
    getNpcPosNewVis(c, buf, size);
    getNpcPosExtended(c, buf, size);

    for (int i = 0; i < c->entityRemovalCount; i++) {
        int index = c->entityRemovalIds[i];
        if (c->npcs[index]->pathing_entity.cycle != _Client.loop_cycle) {
            c->npcs[index]->type = NULL;
            free(c->npcs[index]);
            c->npcs[index] = NULL;
        }
    }

    if (buf->pos != size) {
        rs2_error("%s size mismatch in getnpcpos - pos:%d psize:%d\n", c->username, buf->pos, size);
        // signlink.reporterror(c->username + " size mismatch in getnpcpos - pos:" + buf.pos + " psize:" + size);
        // throw new RuntimeException("eek");
    }

    for (int i = 0; i < c->npc_count; i++) {
        if (c->npcs[c->npc_ids[i]] == NULL) {
            rs2_error("%s null entry in npc lit - pos:%d size:%d\n", c->username, i, c->npc_count);
            // signlink.reporterror(c->username + " null entry in npc list - pos:" + i + " size:" + c->npcCount);
            // throw new RuntimeException("eek");
        }
    }
}

void getNpcPosExtended(Client *c, Packet *buf, int size) {
    (void)size;
    for (int i = 0; i < c->entityUpdateCount; i++) {
        int id = c->entityUpdateIds[i];
        // the mask byte (and whichever optional fields it flags) must always be consumed to keep
        // the packet's byte framing correct, even if this specific npc slot has already gone stale
        // by the time this runs (e.g. removed the same tick, or a timing gap between the position
        // pass queuing this update and this extended-info pass consuming it) - c->npcs[id] was
        // dereferenced completely unconditionally here with no NULL check at all, unlike every other
        // npc-array access fixed elsewhere this session. Route the writes at a disposable dummy
        // instead of skipping them, so the read logic/stream position below stays untouched.
        static NpcEntity dummy_npc;
        memset(&dummy_npc, 0, sizeof(dummy_npc));
        NpcEntity *npc = c->npcs[id] ? c->npcs[id] : &dummy_npc;
        int mask = g1(buf);

        npc->pathing_entity.lastMask = mask;
        npc->pathing_entity.lastMaskCycle = _Client.loop_cycle;

        if ((mask & 0x1) == 1) {
            npc->pathing_entity.damage2 = g1(buf);
            npc->pathing_entity.damageType2 = g1(buf);
            npc->pathing_entity.combatCycle2 = _Client.loop_cycle + 400;
            npc->pathing_entity.health = g1(buf);
            npc->pathing_entity.totalHealth = g1(buf);
        }
        if ((mask & 0x2) == 2) {
            int seqId = g2(buf);
            // see getPlayerExtended's identical fix above for why out-of-range must map to -1 and
            // why the sentinel checks below must run before any _SeqType.instances[] access.
            if (seqId == 65535 || seqId < 0 || seqId >= _SeqType.count) {
                seqId = -1;
            }
            if (seqId == npc->pathing_entity.primarySeqId) {
                npc->pathing_entity.primarySeqLoop = 0;
            }
            int delay = g1(buf);
            // TODO: duplicatebehaviour!=0 assumed to mean "RESET" (restart from frame 0); verify polarity against real gameplay, see audit
            if (seqId == -1 || npc->pathing_entity.primarySeqId == -1 || (seqId == npc->pathing_entity.primarySeqId && _SeqType.instances[seqId]->duplicatebehaviour != 0) || _SeqType.instances[seqId]->priority > _SeqType.instances[npc->pathing_entity.primarySeqId]->priority || _SeqType.instances[npc->pathing_entity.primarySeqId]->priority == 0) {
                npc->pathing_entity.primarySeqId = seqId;
                npc->pathing_entity.primarySeqFrame = 0;
                npc->pathing_entity.primarySeqCycle = 0;
                npc->pathing_entity.primarySeqDelay = delay;
                npc->pathing_entity.primarySeqLoop = 0;
            }
        }
        if ((mask & 0x4) == 4) {
            npc->pathing_entity.targetId = g2(buf);
            if (npc->pathing_entity.targetId == 65535) {
                npc->pathing_entity.targetId = -1;
            }
        }
        if ((mask & 0x8) == 8) {
            strcpy(npc->pathing_entity.chat, gjstr(buf));
            npc->pathing_entity.chatTimer = 100;
        }
        if ((mask & 0x10) == 16) {
            npc->pathing_entity.damage = g1(buf);
            npc->pathing_entity.damageType = g1(buf);
            npc->pathing_entity.combatCycle = _Client.loop_cycle + 400;
            npc->pathing_entity.health = g1(buf);
            npc->pathing_entity.totalHealth = g1(buf);
        }
        if ((mask & 0x20) == 32) {
            npc->type = npctype_get(g2(buf));
            npc->pathing_entity.seqWalkId = npc->type->walkanim;
            npc->pathing_entity.seqTurnAroundId = npc->type->walkanim_b;
            npc->pathing_entity.seqTurnLeftId = npc->type->walkanim_r;
            npc->pathing_entity.seqTurnRightId = npc->type->walkanim_l;
            npc->pathing_entity.turnRate = npc->type->turnspeed;
            npc->pathing_entity.seqStandId = npc->type->readyanim;
        }
        if ((mask & 0x40) == 64) {
            npc->pathing_entity.spotanimId = g2(buf);
            int info = g4(buf);
            npc->pathing_entity.spotanimOffset = info >> 16;
            npc->pathing_entity.spotanimLastCycle = _Client.loop_cycle + (info & 0xffff);
            npc->pathing_entity.spotanimFrame = 0;
            npc->pathing_entity.spotanimCycle = 0;
            if (npc->pathing_entity.spotanimLastCycle > _Client.loop_cycle) {
                npc->pathing_entity.spotanimFrame = -1;
            }
            if (npc->pathing_entity.spotanimId == 65535) {
                npc->pathing_entity.spotanimId = -1;
            }
        }
        if ((mask & 0x80) == 128) {
            npc->pathing_entity.targetTileX = g2(buf);
            npc->pathing_entity.targetTileZ = g2(buf);
            npc->pathing_entity.lastFaceX = npc->pathing_entity.targetTileX;
            npc->pathing_entity.lastFaceZ = npc->pathing_entity.targetTileZ;
        }
    }
}

void getNpcPosNewVis(Client *c, Packet *buf, int size) {
    while (buf->bit_pos + 21 < size * 8) {
        // rev254's npc index field is 14 bits (sentinel 16383/0x3FFF), not 13 bits (8191) - the
        // narrower rev225-era width read 1 bit less per new npc than the server actually sent,
        // progressively desyncing the rest of this bit-packed block (and everything after it in the
        // same packet). Confirmed against the reference client (2004sp-client) and by observing
        // "size mismatch in getnpcpos" errors with inconsistent pos/psize deltas that grew or shrank
        // depending on how many new npcs appeared in a given NPC_INFO packet - this was the real root
        // cause of a hard-to-bisect crash deep in scene/minimap rendering, far from this actual bug.
        int index = gbit(buf, 14);
        if (index == 16383) {
            break;
        }
        if (!c->npcs[index]) {
            c->npcs[index] = npcentity_new();
        }
        NpcEntity *npc = c->npcs[index];
        c->npc_ids[c->npc_count++] = index;
        npc->pathing_entity.cycle = _Client.loop_cycle;
        npc->type = npctype_get(gbit(buf, 11));
        npc->pathing_entity.size = npc->type->size;
        npc->pathing_entity.seqWalkId = npc->type->walkanim;
        npc->pathing_entity.seqTurnAroundId = npc->type->walkanim_b;
        npc->pathing_entity.seqTurnLeftId = npc->type->walkanim_r;
        npc->pathing_entity.seqTurnRightId = npc->type->walkanim_l;
        npc->pathing_entity.turnRate = npc->type->turnspeed;
        npc->pathing_entity.seqStandId = npc->type->readyanim;
        int dx = gbit(buf, 5);
        if (dx > 15) {
            dx -= 32;
        }
        int dz = gbit(buf, 5);
        if (dz > 15) {
            dz -= 32;
        }
        pathingentity_teleport(&npc->pathing_entity, false, c->local_player->pathing_entity.pathTileX[0] + dx, c->local_player->pathing_entity.pathTileZ[0] + dz);
        int extendedInfo = gbit(buf, 1);
        if (extendedInfo == 1) {
            c->entityUpdateIds[c->entityUpdateCount++] = index;
        }
    }
    access_bytes(buf);
}

void getNpcPosOldVis(Client *c, Packet *buf, int size) {
    (void)size;
    access_bits(buf);

    int count = gbit(buf, 8);
    if (count < c->npc_count) {
        for (int i = count; i < c->npc_count; i++) {
            c->entityRemovalIds[c->entityRemovalCount++] = c->npc_ids[i];
        }
    }

    if (count > c->npc_count) {
        rs2_error("%s Too many npcs\n", c->username);
        // signlink.reporterror(c->username + " Too many npcs");
        // throw new RuntimeException("eek");
    }

    c->npc_count = 0;
    for (int i = 0; i < count; i++) {
        int index = c->npc_ids[i];
        NpcEntity *npc = c->npcs[index];

        int info = gbit(buf, 1);
        if (info == 0) {
            c->npc_ids[c->npc_count++] = index;
            npc->pathing_entity.cycle = _Client.loop_cycle;
        } else {
            int op = gbit(buf, 2);

            if (op == 0) {
                c->npc_ids[c->npc_count++] = index;
                npc->pathing_entity.cycle = _Client.loop_cycle;
                c->entityUpdateIds[c->entityUpdateCount++] = index;
            } else if (op == 1) {
                c->npc_ids[c->npc_count++] = index;
                npc->pathing_entity.cycle = _Client.loop_cycle;

                int walkDir = gbit(buf, 3);
                pathingentity_movealongroute(&npc->pathing_entity, false, walkDir);

                int extendedInfo = gbit(buf, 1);
                if (extendedInfo == 1) {
                    c->entityUpdateIds[c->entityUpdateCount++] = index;
                }
            } else if (op == 2) {
                c->npc_ids[c->npc_count++] = index;
                npc->pathing_entity.cycle = _Client.loop_cycle;

                int walkDir = gbit(buf, 3);
                pathingentity_movealongroute(&npc->pathing_entity, true, walkDir);
                int runDir = gbit(buf, 3);
                pathingentity_movealongroute(&npc->pathing_entity, true, runDir);

                int extendedInfo = gbit(buf, 1);
                if (extendedInfo == 1) {
                    c->entityUpdateIds[c->entityUpdateCount++] = index;
                }
            } else if (op == 3) {
                c->entityRemovalIds[c->entityRemovalCount++] = index;
            }
        }
    }
}

void client_add_message(Client *c, int type, const char *text, const char *sender) {
    if (type == 0 && c->sticky_chat_interface_id != -1) {
        strcpy(c->modal_message, text);
        c->shell->mouse_click_button = 0;
    }

    if (c->chat_interface_id == -1) {
        c->redraw_chatback = true;
    }

    for (int i = 99; i > 0; i--) {
        c->message_type[i] = c->message_type[i - 1];
        strcpy(c->message_sender[i], c->message_sender[i - 1]);
        strcpy(c->message_text[i], c->message_text[i - 1]);
    }

    // TODO: debug
    // if (c->show_debug && type == 0) {
    // 	text = "[" + (loopCycle / 30) + "]: " + text;
    // }

    c->message_type[0] = type;
    strcpy(c->message_sender[0], sender);
    strcpy(c->message_text[0], text);
}

void updateVarp(Client *c, int id) {
    int clientcode = _VarpType.instances[id]->clientcode;
    if (clientcode == 0) {
        return;
    }

    int value = c->varps[id];
    if (clientcode == 1) {
        if (value == 1) {
            pix3d_set_brightness(0.9);
        } else if (value == 2) {
            pix3d_set_brightness(0.8);
        } else if (value == 3) {
            pix3d_set_brightness(0.7);
        } else if (value == 4) {
            pix3d_set_brightness(0.6);
        }

        lrucache_clear(_ObjType.iconCache);
        c->redraw_background = true;
    // NOTE different volume values as bgsound element isn't used
    } else if (clientcode == 3) {
        bool lastMidiActive = c->midiActive;
        if (value == 0) {
            platform_set_midi_volume(1.0); // 0
            c->midiActive = true;
        } else if (value == 1) {
            platform_set_midi_volume(0.75); // -400
            c->midiActive = true;
        } else if (value == 2) {
            platform_set_midi_volume(0.5); // -800
            c->midiActive = true;
        } else if (value == 3) {
            platform_set_midi_volume(0.25); // -1200
            c->midiActive = true;
        } else if (value == 4) {
            c->midiActive = false;
        }

        if (c->midiActive != lastMidiActive) {
            if (c->midiActive) {
                platform_set_midi(c->currentMidi, c->midiCrc, c->midiSize);
            } else {
                platform_stop_midi();
            }

            c->nextMusicDelay = 0;
        }
    } else if (clientcode == 4) {
        if (value == 0) {
            c->wave_enabled = true;
            platform_set_wave_volume(127);
        } else if (value == 1) {
            c->wave_enabled = true;
            platform_set_wave_volume(96);
        } else if (value == 2) {
            c->wave_enabled = true;
            platform_set_wave_volume(64);
        } else if (value == 3) {
            c->wave_enabled = true;
            platform_set_wave_volume(32);
        } else if (value == 4) {
            c->wave_enabled = false;
        }
    } else if (clientcode == 5) {
        c->mouseButtonsOption = value;
    } else if (clientcode == 6) {
        c->chatEffects = value;
    } else if (clientcode == 8) {
        c->split_private_chat = value;
        c->redraw_chatback = true;
    }
}

void reset_interface_animation(int id) {
    // called directly from IF_OPENCHAT (the packet that opens an NPC dialogue box, e.g. via
    // Talk-to) with a completely unvalidated component id - confirmed real crash: rev254 can open
    // a dialogue referencing an interface component Client3 hasn't instantiated, and this
    // dereferenced it with zero bounds/NULL check at all, unlike every other _Component.instances[]
    // access already guarded this session.
    if (!component_valid(id)) {
        return;
    }
    Component *parent = component_get(id);
    for (int i = 0; i < parent->childCount && parent->childId[i] != -1; i++) {
        if (!component_valid(parent->childId[i])) {
            continue;
        }
        Component *child = component_get(parent->childId[i]);
        if (child->type == 1) {
            reset_interface_animation(child->id);
        }
        child->seqFrame = 0;
        child->seqCycle = 0;
    }
}

void client_try_reconnect(Client *c) {
    if (c->idle_timeout > 0) {
        client_logout(c);
    } else {
        pixmap_bind(c->area_viewport);
        drawStringCenter(c->font_plain12, 257, 144, "Connection lost", BLACK);
        drawStringCenter(c->font_plain12, 256, 143, "Connection lost", WHITE);
        drawStringCenter(c->font_plain12, 257, 159, "Please wait - attempting to reestablish", BLACK);
        drawStringCenter(c->font_plain12, 256, 158, "Please wait - attempting to reestablish", WHITE);
        pixmap_draw(c->area_viewport, 4, 4);
        c->flag_scene_tile_x = 0;
        ClientStream *stream = c->stream;
        c->ingame = false;

        client_login(c, c->username, c->password, true);
        if (!c->ingame) {
            client_logout(c);
        }

        // try {
        clientstream_close(stream);
        // } catch (@Pc(80) Exception ex) {
        // }
    }
}

void client_logout(Client *c) {
    // try {
    if (c->stream) {
        clientstream_close(c->stream);
    }
    // } catch (@Pc(9) Exception ignored) {
    // }

    c->stream = NULL;
    c->ingame = false;
    c->title_screen_state = 0;
    if (!_Custom.remember_username) {
        c->username[0] = '\0';
    }
    if (!_Custom.remember_password) {
        c->password[0] = '\0';
    }

    inputtracking_set_disabled(&_InputTracking);
    client_clear_caches();
    world3d_reset(c->scene);

    for (int level = 0; level < 4; level++) {
        collisionmap_reset(c->levelCollisionMap[level]);
    }

    platform_stop_midi();

    c->currentMidi[0] = '\0';
    c->nextMusicDelay = 0;
}

void client_update_title(Client *c) {
    if (c->title_screen_state == 0) {
        int x = c->shell->screen_width / 2 - 80;
        int y = c->shell->screen_height / 2 + 20;

        y += 20;
        if (c->shell->mouse_click_button == 1 && c->shell->mouse_click_x >= x - 75 && c->shell->mouse_click_x <= x + 75 && c->shell->mouse_click_y >= y - 20 && c->shell->mouse_click_y <= y + 20) {
            c->title_screen_state = 3;
            c->title_login_field = 0;
        }

        x = c->shell->screen_width / 2 + 80;
        if (c->shell->mouse_click_button == 1 && c->shell->mouse_click_x >= x - 75 && c->shell->mouse_click_x <= x + 75 && c->shell->mouse_click_y >= y - 20 && c->shell->mouse_click_y <= y + 20) {
            c->login_message0 = "";
            c->login_message1 = "Enter your username & password.";
            c->title_screen_state = 2;
            c->title_login_field = 0;
            virtual_keyboard_maybe_open(c, 0);
        }
    } else if (c->title_screen_state == 2) {
        int y = c->shell->screen_height / 2 - 40;
        y += 30;
        y += 25;

        if (c->shell->mouse_click_button == 1 && c->shell->mouse_click_y >= y - 15 && c->shell->mouse_click_y < y) {
            c->title_login_field = 0;
            virtual_keyboard_maybe_open(c, 0);
        }
        y += 15;

        if (c->shell->mouse_click_button == 1 && c->shell->mouse_click_y >= y - 15 && c->shell->mouse_click_y < y) {
            c->title_login_field = 1;
            virtual_keyboard_maybe_open(c, 1);
        }
        y += 15;

        int buttonX = c->shell->screen_width / 2 - 80;
        int buttonY = c->shell->screen_height / 2 + 50;
        buttonY += 20;

        if (c->shell->mouse_click_button == 1 && c->shell->mouse_click_x >= buttonX - 75 && c->shell->mouse_click_x <= buttonX + 75 && c->shell->mouse_click_y >= buttonY - 20 && c->shell->mouse_click_y <= buttonY + 20) {
            client_login(c, c->username, c->password, false);
        }

        buttonX = c->shell->screen_width / 2 + 80;
        if (c->shell->mouse_click_button == 1 && c->shell->mouse_click_x >= buttonX - 75 && c->shell->mouse_click_x <= buttonX + 75 && c->shell->mouse_click_y >= buttonY - 20 && c->shell->mouse_click_y <= buttonY + 20) {
            c->title_screen_state = 0;
            c->virtual_keyboard_visible = false;
            if (!_Custom.remember_username) {
                c->username[0] = '\0';
            }
            if (!_Custom.remember_password) {
                c->password[0] = '\0';
            }
        }

        while (true) {
            while (true) {
                int key = poll_key(c->shell);
                if (key == -1) {
                    return;
                }

                bool valid = false;
                size_t len = strlen(CHARSET);
                for (size_t i = 0; i < len; i++) {
                    if (key == CHARSET[i]) {
                        valid = true;
                        break;
                    }
                }

                if (c->title_login_field == 0) {
                    len = strlen(c->username);
                    if (key == 8 && len > 0) {
                        c->username[len - 1] = '\0';
                    }

                    if (key == 9 || key == 10 || key == 13) {
                        c->title_login_field = 1;
                    }

                    if (valid) {
                        len = strlen(c->username);
                        if (len < USERNAME_LENGTH) {
                            c->username[len] = (char)key;
                            c->username[len + 1] = '\0';
                        }
                    }
                } else if (c->title_login_field == 1) {
                    len = strlen(c->password);
                    if (key == 8 && len > 0) {
                        c->password[len - 1] = '\0';
                    }

                    if (key == 9 || key == 10 || key == 13) {
                        c->title_login_field = 0;
                    }

                    if (valid) {
                        len = strlen(c->password);
                        if (len < PASSWORD_LENGTH) {
                            c->password[len] = (char)key;
                            c->password[len + 1] = '\0';
                        }
                    }
                }
            }
        }
    } else if (c->title_screen_state == 3) {
        int x = c->shell->screen_width / 2;
        int y = c->shell->screen_height / 2 + 50;
        y += 20;

        if (c->shell->mouse_click_button == 1 && c->shell->mouse_click_x >= x - 75 && c->shell->mouse_click_x <= x + 75 && c->shell->mouse_click_y >= y - 20 && c->shell->mouse_click_y <= y + 20) {
            c->title_screen_state = 0;
        }
    }
}

void client_login(Client *c, const char *username, const char *password, bool reconnect) {
    // signlink.errorname = username;
    // try {
    if (!reconnect) {
        c->login_message0 = "";
        c->login_message1 = "Connecting to server...";
        client_draw_title_screen(c);
        // Force-close defensively regardless of which field/keyboard state was active - an actual
        // login attempt is now underway, so a stale virtual keyboard overlay must never linger
        // into the "Connecting..."/in-game screens that follow.
        c->virtual_keyboard_visible = false;
    }
    platform_update_surface();

#ifdef __wasm
    c->stream = clientstream_opensocket(_Custom.http_port);
#else
    c->stream = clientstream_opensocket(_Client.portoff + 43594);
#endif
    if (!c->stream) {
        goto login_fail;
    }

    // rev254 adds a preliminary session-key exchange before the seed is sent (not present in rev225):
    // client sends opcode 14 + a 1-byte loginServer id, server acks 8 zero bytes (discarded) + 1-byte
    // status, THEN sends the real 8-byte seed. TODO: loginServer should be derived from the username
    // hash for multi-world load balancing; hardcoded to 0 here since a single-world dev server ignores it.
    c->out->pos = 0;
    p1(c->out, 14);
    p1(c->out, 0);
    clientstream_write(c->stream, c->out->data, c->out->pos, 0);
    if (clientstream_read_bytes(c->stream, c->in->data, 0, 8) == -1) {
        goto login_fail;
    }
    int sessionStatus = clientstream_read_byte(c->stream);
    if (sessionStatus != 0) {
        goto login_fail;
    }

    if (clientstream_read_bytes(c->stream, c->in->data, 0, 8) == -1) {
        goto login_fail;
    }
    c->in->pos = 0;

    c->server_seed = g8(c->in);
    int seed[] = {(int)(jrand() * 9.9999999E7), (int)(jrand() * 9.9999999E7), (int)(c->server_seed >> 32), (int)c->server_seed};

    c->out->pos = 0;
    p1(c->out, 10);
    p4(c->out, seed[0]);
    p4(c->out, seed[1]);
    p4(c->out, seed[2]);
    p4(c->out, seed[3]);
    p4(c->out, _Client.uid);
    pjstr(c->out, username);
    pjstr(c->out, password);
    rsaenc(c->out, _Client.rsa_modulus, _Client.rsa_exponent);

    c->login->pos = 0;
    if (reconnect) {
        p1(c->login, 18);
    } else {
        p1(c->login, 16);
    }

    p1(c->login, c->out->pos + 36 + 1 + 1);
    p1(c->login, _Client.clientversion);
    p1(c->login, _Client.lowmem ? 1 : 0);

    for (int i = 0; i < 9; i++) {
        p4(c->login, c->archive_checksum[i]);
    }
    pdata(c->login, c->out->data, c->out->pos, 0);

    memset(c->out->random.randrsl, 0,
           sizeof(c->out->random.randrsl));

    memset(c->random_in.randrsl, 0,
           sizeof(c->random_in.randrsl));

    memcpy(c->out->random.randrsl, seed, sizeof(seed));
    isaac_init(&c->out->random, 1);
    for (int i = 0; i < 4; i++) {
        seed[i] += 50;
    }
    memcpy(c->random_in.randrsl, seed, sizeof(seed));
    isaac_init(&c->random_in, 1);
    clientstream_write(c->stream, c->login->data, c->login->pos, 0);

    int reply = clientstream_read_byte(c->stream);
    if (reply == 1) {
        rs2_sleep(2000);
        client_login(c, username, password, reconnect);
    } else if (reply == 2 || reply == 18) {
        c->rights = reply == 18;
        if (reply == 2) {
            // Success (2) sends 2 extra bytes not present after the legacy admin-success (18) reply:
            // staffModLevel + mouseTrackingFlag. Without reading these, the next 2 stream bytes get
            // misread as the start of the first in-game packet, desyncing everything after login.
            // TODO: these are discarded for now - verify whether staffModLevel should feed c->rights.
            clientstream_read_byte(c->stream);
            clientstream_read_byte(c->stream);
        }
        inputtracking_set_disabled(&_InputTracking);

        c->ingame = true;
        c->out->pos = 0;
        c->in->pos = 0;
        c->packet_type = -1;
        c->last_packet_type0 = -1;
        c->last_packet_type1 = -1;
        c->last_packet_type2 = -1;
        c->packet_size = 0;
        c->idle_net_cycles = 0;
        c->system_update_timer = 0;
        c->idle_timeout = 0;
        c->hint_type = 0;
        c->menu_size = 0;
        c->menu_visible = false;
        c->shell->idle_cycles = 0;

        for (int i = 0; i < 100; i++) {
            c->message_text[i][0] = '\0';
        }

        c->obj_selected = 0;
        c->spell_selected = 0;
        c->scene_state = 0;
        c->wave_count = 0;

        c->camera_anticheat_offset_x = (int)(jrand() * 100.0) - 50;
        c->camera_anticheat_offset_z = (int)(jrand() * 110.0) - 55;
        c->camera_anticheat_angle = (int)(jrand() * 80.0) - 40;
        c->minimap_anticheat_angle = (int)(jrand() * 120.0) - 60;
        c->minimap_zoom = (int)(jrand() * 30.0) - 20;
        c->orbit_camera_yaw = (int)(jrand() * 20.0) - 10 & 0x7ff;

        c->minimap_level = -1;
        c->flag_scene_tile_x = 0;
        c->flag_scene_tile_z = 0;

        c->player_count = 0;
        c->npc_count = 0;

        for (int i = 0; i < MAX_PLAYER_COUNT; i++) {
            free(c->players[i]);
            c->players[i] = NULL;
            if (c->player_appearance_buffer[i]) {
                packet_free(c->player_appearance_buffer[i]);
                c->player_appearance_buffer[i] = NULL;
            }
        }

        for (int i = 0; i < MAX_NPC_COUNT; i++) {
            if (c->npcs[i]) {
                free(c->npcs[i]);
                c->npcs[i] = NULL;
            }
        }

        c->local_player = c->players[LOCAL_PLAYER_INDEX] = playerentity_new();
        linklist_clear(c->projectiles);
        linklist_clear(c->spotanims);
        linklist_clear(c->merged_locations);
        for (int level = 0; level < 4; level++) {
            for (int x = 0; x < 104; x++) {
                for (int z = 0; z < 104; z++) {
                    if (c->level_obj_stacks[level][x][z]) {
                        linklist_free(c->level_obj_stacks[level][x][z]);
                        c->level_obj_stacks[level][x][z] = NULL;
                    }
                }
            }
        }

        // TODO: why not linklist_clear originally?
        linklist_free(c->spawned_locations);
        c->spawned_locations = linklist_new();
        c->friend_count = 0;
        c->sticky_chat_interface_id = -1;
        c->chat_interface_id = -1;
        c->viewport_interface_id = -1;
        c->sidebar_interface_id = -1;
        c->pressed_continue_option = false;
        c->selected_tab = 3;
        c->chatback_input_open = false;
        c->menu_visible = false;
        c->show_social_input = false;
        c->modal_message[0] = '\0';
        c->in_multizone = 0;
        c->flashing_tab = -1;
        c->design_gender_male = true;

        client_validate_character_design(c);
        for (int i = 0; i < 5; i++) {
            c->design_colors[i] = 0;
        }

        _Client.oplogic1 = 0;
        _Client.oplogic2 = 0;
        _Client.oplogic3 = 0;
        _Client.oplogic4 = 0;
        _Client.oplogic5 = 0;
        _Client.oplogic6 = 0;
        _Client.oplogic7 = 0;
        _Client.oplogic8 = 0;
        _Client.oplogic9 = 0;

        client_prepare_game_screen(c);
    } else if (reply == 3) {
        c->login_message0 = "";
        c->login_message1 = "Invalid username or password.";
    } else if (reply == 4) {
        c->login_message0 = "Your account has been disabled.";
        c->login_message1 = "Please check your message-centre for details.";
    } else if (reply == 5) {
        c->login_message0 = "Your account is already logged in.";
        c->login_message1 = "Try again in 60 secs...";
    } else if (reply == 6) {
        c->login_message0 = "RuneScape has been updated!";
        c->login_message1 = "Please reload this page.";
    } else if (reply == 7) {
        c->login_message0 = "This world is full.";
        c->login_message1 = "Please use a different world.";
    } else if (reply == 8) {
        c->login_message0 = "Unable to connect.";
        c->login_message1 = "Login server offline.";
    } else if (reply == 9) {
        c->login_message0 = "Login limit exceeded.";
        c->login_message1 = "Too many connections from your address.";
    } else if (reply == 10) {
        c->login_message0 = "Unable to connect.";
        c->login_message1 = "Bad session id.";
    } else if (reply == 11) {
        c->login_message0 = "Login server rejected session.";
        c->login_message1 = "Please try again.";
    } else if (reply == 12) {
        c->login_message0 = "You need a members account to login to this world.";
        c->login_message1 = "Please subscribe, or use a different world.";
    } else if (reply == 13) {
        c->login_message0 = "Could not complete login.";
        c->login_message1 = "Please try using a different world.";
    } else if (reply == 14) {
        c->login_message0 = "The server is being updated.";
        c->login_message1 = "Please wait 1 minute and try again.";
    } else if (reply == 15) {
        c->ingame = true;
        c->out->pos = 0;
        c->in->pos = 0;
        c->packet_type = -1;
        c->last_packet_type0 = -1;
        c->last_packet_type1 = -1;
        c->last_packet_type2 = -1;
        c->packet_size = 0;
        c->idle_net_cycles = 0;
        c->system_update_timer = 0;
        c->menu_size = 0;
        c->menu_visible = false;
    } else if (reply == 16) {
        c->login_message0 = "Login attempts exceeded.";
        c->login_message1 = "Please wait 1 minute and try again.";
    } else if (reply == 17) {
        c->login_message0 = "You are standing in a members-only area.";
        c->login_message1 = "To play on this world move to a free area first";
    } else if (reply == 21) {
        int seconds = clientstream_read_byte(c->stream);
        c->login_message0 = "";
        c->login_message1 = "Login limited, retrying...";
        rs2_sleep(seconds * 1000);
        client_login(c, username, password, reconnect);
    }
    return;
login_fail:
    c->login_message0 = "";
    c->login_message1 = "Error connecting to server.";
}

void client_prepare_game_screen(Client *c) {
    if (c->area_chatback) {
        return;
    }

    client_unload_title(c);

#if defined(__PS2__) && PS2_SIMPLE_UI
    // These nine PixMaps are static copies of the decorative stone frame.  They are only
    // ever composited behind other panels, so releasing them leaves a deliberately simple
    // black UI surround without touching the live viewport, chat/sidebar, or tab icons.
    pixmap_free(c->area_backleft1);  c->area_backleft1 = NULL;
    pixmap_free(c->area_backleft2);  c->area_backleft2 = NULL;
    pixmap_free(c->area_backright1); c->area_backright1 = NULL;
    pixmap_free(c->area_backright2); c->area_backright2 = NULL;
    pixmap_free(c->area_backtop1);   c->area_backtop1 = NULL;
    pixmap_free(c->area_backvmid1);  c->area_backvmid1 = NULL;
    pixmap_free(c->area_backvmid2);  c->area_backvmid2 = NULL;
    pixmap_free(c->area_backvmid3);  c->area_backvmid3 = NULL;
    pixmap_free(c->area_backhmid2);  c->area_backhmid2 = NULL;
    // Its masks were built during client_load(), and the minimap is disabled above.
    pix8_free(c->image_mapback);     c->image_mapback = NULL;
    pix8_free(c->image_invback);     c->image_invback = NULL;
    pix8_free(c->image_chatback);    c->image_chatback = NULL;
    pix8_free(c->image_backbase1);   c->image_backbase1 = NULL;
    pix8_free(c->image_backbase2);   c->image_backbase2 = NULL;
    pix8_free(c->image_backhmid1);   c->image_backhmid1 = NULL;
    // PixMap draws permanently composite into the PS2's CPU-side presentation texture.
    // Once the frame panels are released, erase their previous title/game pixels so they
    // do not remain as stale fragments around the new simple layout.
    platform_clear_surface();
#endif

    if (c->shell->draw_area) {
        pixmap_free(c->shell->draw_area);
        c->shell->draw_area = NULL;
    }
    pixmap_free(c->image_title0);
    pixmap_free(c->image_title1);
    pixmap_free(c->image_title2);
    pixmap_free(c->image_title3);
    pixmap_free(c->image_title4);
    pixmap_free(c->image_title5);
    pixmap_free(c->image_title6);
    pixmap_free(c->image_title7);
    pixmap_free(c->image_title8);
    c->image_title2 = NULL;
    c->image_title3 = NULL;
    c->image_title4 = NULL;
    c->image_title0 = NULL;
    c->image_title1 = NULL;
    c->image_title5 = NULL;
    c->image_title6 = NULL;
    c->image_title7 = NULL;
    c->image_title8 = NULL;
    // sizes corrected to match the real rev254 fixed-mode (765x503) layout, confirmed against the
    // reference client - Client3 was hardcoded to the larger "resizable" mode's panel sizes, which
    // don't match the real sprites' actual dimensions (image_mapback alone is genuinely 172x156;
    // pix8_draw()'ing it into a 168x160 buffer clipped/misaligned it, part of the minimap corruption).
    c->area_chatback = pixmap_new(479, 96);
#if !defined(__PS2__) || !PS2_SIMPLE_UI
    c->area_mapback = pixmap_new(172, 156);
    pix2d_clear();
    pix8_draw(c->image_mapback, 0, 0);
#endif
    c->area_sidebar = pixmap_new(190, 261);
#ifdef __PS2__
    c->area_viewport_3d = pixmap_new(PS2_3D_RENDER_WIDTH, PS2_3D_RENDER_HEIGHT);
#endif
    c->area_viewport = pixmap_new(512, 334);
    pix2d_clear();
    c->area_backbase1 = pixmap_new(496, 50);
    c->area_backbase2 = pixmap_new(269, 37);
    c->area_backhmid1 = pixmap_new(249, 45);
    c->redraw_background = true;
}

void client_unload_title(Client *c) {
    c->flame_active = false;
    pix8_free(c->image_titlebox);
    pix8_free(c->image_titlebutton);
#ifndef DISABLE_FLAMES
    for (int i = 0; i < 12; i++) {
        pix8_free(c->image_runes[i]);
    }
    free(c->image_runes);
    free(c->flame_gradient0);
    free(c->flame_gradient1);
    free(c->flame_gradient2);
    free(c->flame_gradient);
    free(c->flame_buffer0);
    free(c->flame_buffer1);
    free(c->flame_buffer3);
    free(c->flame_buffer2);
    pix24_free(c->image_flames_left);
    pix24_free(c->image_flames_right);
#endif

    c->image_titlebox = NULL;
    c->image_titlebutton = NULL;
    for (int i = 0; i < 12; i++) {
        c->image_runes = NULL;
    }
    c->image_runes = NULL;
    c->flame_gradient = NULL;
    c->flame_gradient0 = NULL;
    c->flame_gradient1 = NULL;
    c->flame_gradient2 = NULL;
    c->flame_buffer0 = NULL;
    c->flame_buffer1 = NULL;
    c->flame_buffer3 = NULL;
    c->flame_buffer2 = NULL;
    c->image_flames_left = NULL;
    c->image_flames_right = NULL;
}

void client_validate_character_design(Client *c) {
    c->update_design_model = true;

    for (int i = 0; i < 7; i++) {
        c->designIdentikits[i] = -1;

        for (int j = 0; j < _IdkType.count; j++) {
            if (!_IdkType.instances[j]->disable && _IdkType.instances[j]->type == i + (c->design_gender_male ? 0 : 7)) {
                c->designIdentikits[i] = j;
                break;
            }
        }
    }
}

void client_draw(Client *c) {
    gl_start_frame();

    if (c->error_started || c->error_loading || c->error_host) {
        client_draw_error(c);
    } else {
        if (c->ingame) {
            client_draw_game(c);
        } else {
            client_draw_title_screen(c);
        }

        c->drag_cycles = 0;
    }

    if (c->controller_settings_visible && c->ingame) {
        controller_settings_draw(c);
    }

    // On-screen virtual keyboard overlay (see gameshell.h's has_keyboard) - drawn last so it
    // paints over the 3D scene, sidebar, chatback, and any context menu regardless of which of
    // the two branches above ran, matching how both the login screen and in-game chat need it.
    if (c->virtual_keyboard_visible) {
        virtual_keyboard_draw(c);
    }

#if !defined(__PS2__) || (!PS2_NULL_UI && !PS2_SAFE_INTERFACE)
    if (!c->shell->has_keyboard && !c->controller_settings_visible) {
        virtual_cursor_draw(c);
    }
#endif

    gl_end_frame();
}

#if defined(__PS2__) && PS2_NULL_UI
/*
 * The normal interface tree includes inventory icons and model widgets.  Those
 * widgets drive the same software model/icon path that is currently unsafe on
 * hardware, so keep a deliberately boring shell alive while the 3D and packet
 * paths are being validated.  It is redrawn every frame because the PS2
 * presenter owns the display buffer, rather than relying on a retained UI.
 */
static void client_draw_ps2_safe_ui(Client *c) {
    pixmap_bind(c->area_chatback);
    _Pix3D.line_offset = c->area_chatback_offsets;
    pix2d_fill_rect(0, 0, 0xe5e5e5, c->area_chatback->width, c->area_chatback->height);
    pix2d_fill_rect(0, 77, 0xc0c0c0, c->area_chatback->width, 1);
    drawString(c->font_plain12, 4, 18, "PS2 safe UI - interface models disabled", BLACK);
    drawString(c->font_plain12, 4, 92, "Chat input disabled while profiling", DARKBLUE);
    pixmap_draw(c->area_chatback, 17, 357);

    pixmap_bind(c->area_sidebar);
    _Pix3D.line_offset = c->area_sidebar_offsets;
    pix2d_fill_rect(0, 0, 0x252a33, c->area_sidebar->width, c->area_sidebar->height);
    drawString(c->font_plain12, 10, 22, "PS2 safe UI", WHITE);
    drawString(c->font_plain12, 10, 40, "Inventory/icons", WHITE);
    drawString(c->font_plain12, 10, 54, "temporarily off", WHITE);
    pixmap_draw(c->area_sidebar, 553, 205);

    pixmap_bind(c->area_viewport);
    _Pix3D.line_offset = c->area_viewport_offsets;
}
#endif


static void controller_settings_draw(Client *c) {
    pixmap_bind(c->area_viewport);
    _Pix3D.line_offset = c->area_viewport_offsets;

    const int x = 92;
    const int y = 54;
    const int w = 328;
    const int h = 218;
    pix2d_fill_rect(x, y, 0x20252d, w, h);
    pix2d_draw_rect(x, y, WHITE, w, h);
    pix2d_fill_rect(x + 1, y + 1, 0x303946, w - 2, 25);
    drawStringTaggableCenter(c->font_bold12, "PlayStation 2 Controller Settings", x + w / 2, y + 18, WHITE, true);

    const char *labels[4] = {
        "Left stick deadzone",
        "Cursor speed",
        "Right stick deadzone",
        "Reset defaults"
    };
    char value[32];
    for (int row = 0; row < 4; row++) {
        int rowY = y + 52 + row * 31;
        int color = row == c->controller_settings_row ? YELLOW : WHITE;
        if (row == 0) {
            snprintf(value, sizeof(value), "%d", c->controller_cursor_deadzone);
        } else if (row == 1) {
            snprintf(value, sizeof(value), "%d", c->controller_cursor_speed);
        } else if (row == 2) {
            snprintf(value, sizeof(value), "%d", c->controller_camera_deadzone);
        } else {
            strcpy(value, "X");
        }
        if (row == c->controller_settings_row) {
            pix2d_fill_rect(x + 12, rowY - 15, 0x394757, w - 24, 22);
        }
        drawString(c->font_plain12, x + 22, rowY, labels[row], color);
        drawStringTaggable(c->font_bold12, x + w - 62, rowY, value, color, true);
    }

    drawStringTaggableCenter(c->font_plain12, "D-Pad: Navigate / Adjust", x + w / 2, y + h - 34, 0xc0c0c0, true);
    drawStringTaggableCenter(c->font_plain12, "L3 or Triangle: Close", x + w / 2, y + h - 17, 0xc0c0c0, true);
    pixmap_draw(c->area_viewport, 4, 4);
}

void client_draw_game(Client *c) {
    if (c->redraw_background) {
        c->redraw_background = false;
        pixmap_draw(c->area_backleft1, 0, 4);
        pixmap_draw(c->area_backleft2, 0, 357);
        pixmap_draw(c->area_backright1, 722, 4);
        pixmap_draw(c->area_backright2, 743, 205);
        pixmap_draw(c->area_backtop1, 0, 0);
        pixmap_draw(c->area_backvmid1, 516, 4);
        pixmap_draw(c->area_backvmid2, 516, 205);
        pixmap_draw(c->area_backvmid3, 496, 357);
        pixmap_draw(c->area_backhmid2, 0, 338);
        c->redraw_sidebar = true;
        c->redraw_chatback = true;
        c->redraw_sideicons = true;
        c->redraw_privacy_settings = true;
        if (c->scene_state != 2) {
            pixmap_draw(c->area_viewport, 4, 4);
            pixmap_draw(c->area_mapback, 550, 4);
        }
    }
#ifdef GL11
    else {
        pixmap_draw(c->area_backleft1, 0, 4);
        pixmap_draw(c->area_backleft2, 0, 357);
        pixmap_draw(c->area_backright1, 722, 4);
        pixmap_draw(c->area_backright2, 743, 205);
        pixmap_draw(c->area_backtop1, 0, 0);
        pixmap_draw(c->area_backvmid1, 516, 4);
        pixmap_draw(c->area_backvmid2, 516, 205);
        pixmap_draw(c->area_backvmid3, 496, 357);
        pixmap_draw(c->area_backhmid2, 0, 338);
        if (c->scene_state != 2) {
            pixmap_draw(c->area_viewport, 4, 4);
            pixmap_draw(c->area_mapback, 550, 4);
        }
    }
#endif

    if (c->scene_state == 2) {
        client_draw_scene(c);
#if defined(__PS2__) && PS2_NULL_UI
        client_draw_ps2_safe_ui(c);
        return;
#endif
    }

#if defined(__PS2__) && PS2_UI_PROFILE == 1
    // Keep the chat path live for this bisection, but eliminate every other
    // normal UI composition pass.  The static-shell profile already proved
    // these areas can be painted; this identifies a dynamic pass, if any.
    c->redraw_sidebar = false;
    c->redraw_sideicons = false;
    c->redraw_privacy_settings = false;
#endif

    if (c->menu_visible && c->menu_area == 1) {
        c->redraw_sidebar = true;
    }

    if (c->sidebar_interface_id != -1) {
        bool redraw = client_update_interface_animation(c, c->sidebar_interface_id, c->scene_delta);
        if (redraw) {
            c->redraw_sidebar = true;
        }
    }

    if (c->selected_area == 2) {
        c->redraw_sidebar = true;
    }

    if (c->obj_drag_area == 2) {
        c->redraw_sidebar = true;
    }

    if (c->redraw_sidebar) {
        client_draw_sidebar(c);
        c->redraw_sidebar = false;
    }
#ifdef GL11
    else {
        pixmap_draw(c->area_sidebar, 553, 205);
    }
#endif

    if (c->chat_interface_id == -1) {
        c->chat_interface->scrollPosition = c->chat_scroll_height - c->chat_scroll_offset - 77;
        if (c->shell->mouse_x > 448 && c->shell->mouse_x < 560 && c->shell->mouse_y > 332) {
            client_handle_scroll_input(c, c->shell->mouse_x - 17, c->shell->mouse_y - 357, c->chat_scroll_height, 77, false, 463, 0, c->chat_interface);
        }

        int offset = c->chat_scroll_height - c->chat_interface->scrollPosition - 77;
        if (offset < 0) {
            offset = 0;
        }

        if (offset > c->chat_scroll_height - 77) {
            offset = c->chat_scroll_height - 77;
        }

        if (c->chat_scroll_offset != offset) {
            c->chat_scroll_offset = offset;
            c->redraw_chatback = true;
        }
    }

    if (c->chat_interface_id != -1) {
        bool redraw = client_update_interface_animation(c, c->chat_interface_id, c->scene_delta);
        if (redraw) {
            c->redraw_chatback = true;
        }
    }

    if (c->selected_area == 3) {
        c->redraw_chatback = true;
    }

    if (c->obj_drag_area == 3) {
        c->redraw_chatback = true;
    }

    if (c->modal_message[0]) {
        c->redraw_chatback = true;
    }

    if (c->menu_visible && c->menu_area == 2) {
        c->redraw_chatback = true;
    }

    if (c->redraw_chatback) {
        client_draw_chatback(c);
        c->redraw_chatback = false;
    }
#ifdef GL11
    else {
        pixmap_draw(c->area_chatback, 17, 357);
    }
#endif

    if (c->scene_state == 2) {
#if !defined(__PS2__) || PS2_UI_PROFILE == 0
        client_draw_minimap(c);
        pixmap_draw(c->area_mapback, 550, 4);
#endif
    }

    if (c->flashing_tab != -1) {
        c->redraw_sideicons = true;
    }

    if (c->redraw_sideicons) {
        if (c->flashing_tab != -1 && c->flashing_tab == c->selected_tab) {
            c->flashing_tab = -1;
            // TUTORIAL_CLICKSIDE
            p1isaac(c->out, 201); // TUT_CLICKSIDE
            p1(c->out, c->selected_tab);
        }

        c->redraw_sideicons = false;
        pixmap_bind(c->area_backhmid1);
#if defined(__PS2__) && PS2_SIMPLE_UI
        pix2d_fill_rect(0, 0, 0x252a33, 249, 45);
#else
        pix8_draw(c->image_backhmid1, 0, 0);
#endif

        if (c->sidebar_interface_id == -1) {
            if (c->tab_interface_id[c->selected_tab] != -1) {
                if (c->selected_tab == 0) {
                    pix8_draw(c->image_redstone1, 22, 10);
                } else if (c->selected_tab == 1) {
                    pix8_draw(c->image_redstone2, 54, 8);
                } else if (c->selected_tab == 2) {
                    pix8_draw(c->image_redstone2, 82, 8);
                } else if (c->selected_tab == 3) {
                    pix8_draw(c->image_redstone3, 110, 8);
                } else if (c->selected_tab == 4) {
                    pix8_draw(c->image_redstone2h, 153, 8);
                } else if (c->selected_tab == 5) {
                    pix8_draw(c->image_redstone2h, 181, 8);
                } else if (c->selected_tab == 6) {
                    pix8_draw(c->image_redstone1h, 209, 9);
                }
            }

            if (c->tab_interface_id[0] != -1 && (c->flashing_tab != 0 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[0], 29, 13);
            }

            if (c->tab_interface_id[1] != -1 && (c->flashing_tab != 1 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[1], 53, 11);
            }

            if (c->tab_interface_id[2] != -1 && (c->flashing_tab != 2 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[2], 82, 11);
            }

            if (c->tab_interface_id[3] != -1 && (c->flashing_tab != 3 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[3], 115, 12);
            }

            if (c->tab_interface_id[4] != -1 && (c->flashing_tab != 4 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[4], 153, 13);
            }

            if (c->tab_interface_id[5] != -1 && (c->flashing_tab != 5 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[5], 180, 11);
            }

            if (c->tab_interface_id[6] != -1 && (c->flashing_tab != 6 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[6], 208, 13);
            }
        }

        pixmap_draw(c->area_backhmid1, 516, 160);
        pixmap_bind(c->area_backbase2);
#if defined(__PS2__) && PS2_SIMPLE_UI
        pix2d_fill_rect(0, 0, 0x252a33, 269, 37);
#else
        pix8_draw(c->image_backbase2, 0, 0);
#endif

        if (c->sidebar_interface_id == -1) {
            if (c->tab_interface_id[c->selected_tab] != -1) {
                if (c->selected_tab == 7) {
                    pix8_draw(c->image_redstone1v, 42, 0);
                } else if (c->selected_tab == 8) {
                    pix8_draw(c->image_redstone2v, 74, 0);
                } else if (c->selected_tab == 9) {
                    pix8_draw(c->image_redstone2v, 102, 0);
                } else if (c->selected_tab == 10) {
                    pix8_draw(c->image_redstone3v, 130, 1);
                } else if (c->selected_tab == 11) {
                    pix8_draw(c->image_redstone2hv, 173, 0);
                } else if (c->selected_tab == 12) {
                    pix8_draw(c->image_redstone2hv, 201, 0);
                } else if (c->selected_tab == 13) {
                    pix8_draw(c->image_redstone1hv, 229, 0);
                }
            }

            if (c->tab_interface_id[8] != -1 && (c->flashing_tab != 8 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[7], 74, 2);
            }

            if (c->tab_interface_id[9] != -1 && (c->flashing_tab != 9 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[8], 102, 3);
            }

            if (c->tab_interface_id[10] != -1 && (c->flashing_tab != 10 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[9], 137, 4);
            }

            if (c->tab_interface_id[11] != -1 && (c->flashing_tab != 11 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[10], 174, 2);
            }

            if (c->tab_interface_id[12] != -1 && (c->flashing_tab != 12 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[11], 201, 2);
            }

            if (c->tab_interface_id[13] != -1 && (c->flashing_tab != 13 || _Client.loop_cycle % 20 < 10)) {
                pix8_draw(c->image_sideicons[12], 226, 2);
            }
        }
        pixmap_draw(c->area_backbase2, 496, 466);
        pixmap_bind(c->area_viewport);
    }
#ifdef GL11
    else {
        pixmap_draw(c->area_backhmid1, 516, 160);
        pixmap_draw(c->area_backbase2, 496, 466);
    }
#endif


    if (c->redraw_privacy_settings) {
        c->redraw_privacy_settings = false;
        pixmap_bind(c->area_backbase1);
#if defined(__PS2__) && PS2_SIMPLE_UI
        pix2d_fill_rect(0, 0, 0x252a33, 496, 50);
#else
        pix8_draw(c->image_backbase1, 0, 0);
#endif

        drawStringTaggableCenter(c->font_plain12, "Public chat", 55, 28, WHITE, true);
        if (c->public_chat_setting == 0) {
            drawStringTaggableCenter(c->font_plain12, "On", 55, 41, GREEN, true);
        }
        if (c->public_chat_setting == 1) {
            drawStringTaggableCenter(c->font_plain12, "Friends", 55, 41, YELLOW, true);
        }
        if (c->public_chat_setting == 2) {
            drawStringTaggableCenter(c->font_plain12, "Off", 55, 41, RED, true);
        }
        if (c->public_chat_setting == 3) {
            drawStringTaggableCenter(c->font_plain12, "Hide", 55, 41, CYAN, true);
        }

        drawStringTaggableCenter(c->font_plain12, "Private chat", 184, 28, WHITE, true);
        if (c->private_chat_setting == 0) {
            drawStringTaggableCenter(c->font_plain12, "On", 184, 41, GREEN, true);
        }
        if (c->private_chat_setting == 1) {
            drawStringTaggableCenter(c->font_plain12, "Friends", 184, 41, YELLOW, true);
        }
        if (c->private_chat_setting == 2) {
            drawStringTaggableCenter(c->font_plain12, "Off", 184, 41, RED, true);
        }

        drawStringTaggableCenter(c->font_plain12, "Trade/duel", 324, 28, WHITE, true);
        if (c->trade_chat_setting == 0) {
            drawStringTaggableCenter(c->font_plain12, "On", 324, 41, GREEN, true);
        }
        if (c->trade_chat_setting == 1) {
            drawStringTaggableCenter(c->font_plain12, "Friends", 324, 41, YELLOW, true);
        }
        if (c->trade_chat_setting == 2) {
            drawStringTaggableCenter(c->font_plain12, "Off", 324, 41, RED, true);
        }

        drawStringTaggableCenter(c->font_plain12, "Report abuse", 458, 33, WHITE, true);
        pixmap_draw(c->area_backbase1, 0, 453);
        pixmap_bind(c->area_viewport);
    }
#ifdef GL11
    else {
        pixmap_draw(c->area_backbase1, 0, 453);
    }
#endif

    c->scene_delta = 0;
}

void client_handle_scroll_input(Client *c, int mouseX, int mouseY, int scrollableHeight, int height, bool redraw, int left, int top, Component *component) {
    if (c->scrollGrabbed) {
        c->scrollInputPadding = 32;
    } else {
        c->scrollInputPadding = 0;
    }

    c->scrollGrabbed = false;

    if (mouseX >= left && mouseX < left + 16 && mouseY >= top && mouseY < top + 16) {
        component->scrollPosition -= c->drag_cycles * 4;
        if (redraw) {
            c->redraw_sidebar = true;
        }
    } else if (mouseX >= left && mouseX < left + 16 && mouseY >= top + height - 16 && mouseY < top + height) {
        component->scrollPosition += c->drag_cycles * 4;
        if (redraw) {
            c->redraw_sidebar = true;
        }
    } else if (mouseX >= left - c->scrollInputPadding && mouseX < left + c->scrollInputPadding + 16 && mouseY >= top + 16 && mouseY < top + height - 16 && c->drag_cycles > 0) {
        int gripSize = (height - 32) * height / scrollableHeight;
        if (gripSize < 8) {
            gripSize = 8;
        }
        int gripY = mouseY - top - gripSize / 2 - 16;
        int maxY = height - gripSize - 32;
        component->scrollPosition = (scrollableHeight - height) * gripY / maxY;
        if (redraw) {
            c->redraw_sidebar = true;
        }
        c->scrollGrabbed = true;
    }
}

bool client_update_interface_animation(Client *c, int id, int delta) {
    bool updated = false;
    // id ultimately traces back to interface ids set from network packets (sidebar/chat/tab/viewport
    // interface ids) that rev254 can point at components Client3 never instantiated - same class of
    // bug as the IF_SET* handlers above, but reached later via stored state rather than a fresh read.
    if (!component_valid(id)) {
        return false;
    }
    Component *parent = component_get(id);
    for (int i = 0; i < parent->childCount && parent->childId[i] != -1; i++) {
        if (!component_valid(parent->childId[i])) {
            continue;
        }
        Component *child = component_get(parent->childId[i]);
        if (child->type == 1) {
            updated |= client_update_interface_animation(c, child->id, delta);
        }
        if (child->type == 6 && (child->anim != -1 || child->activeAnim != -1)) {
            bool active = client_execute_interface_script(c, child);
            int seqId;
            if (active) {
                seqId = child->activeAnim;
            } else {
                seqId = child->anim;
            }
            if (seqId < -1 || seqId >= _SeqType.count) {
                seqId = -1;
            }
            if (seqId != -1) {
                SeqType *type = _SeqType.instances[seqId];
                child->seqCycle += delta;
                while (child->seqCycle > seqtype_get_duration(type, child->seqFrame)) {
                    child->seqCycle -= seqtype_get_duration(type, child->seqFrame) + 1;
                    child->seqFrame++;
                    if (child->seqFrame >= type->frameCount) {
                        child->seqFrame -= type->replayoff;
                        if (child->seqFrame < 0 || child->seqFrame >= type->frameCount) {
                            child->seqFrame = 0;
                        }
                    }
                    updated = true;
                }
            }
        }
    }
    return updated;
}

bool client_execute_interface_script(Client *c, Component *com) {
    if (!com->scriptComparator) {
        return false;
    }

    for (int i = 0; i < com->comparatorCount; i++) {
        int value = client_execute_clientscript1(c, com, i);
        int operand = com->scriptOperand[i];

        if (com->scriptComparator[i] == 2) {
            if (value >= operand) {
                return false;
            }
        } else if (com->scriptComparator[i] == 3) {
            if (value <= operand) {
                return false;
            }
        } else if (com->scriptComparator[i] == 4) {
            if (value == operand) {
                return false;
            }
        } else if (value != operand) {
            return false;
        }
    }

    return true;
}

int client_execute_clientscript1(Client *c, Component *component, int scriptId) {
    if (!component->scripts || scriptId >= component->scriptCount) {
        return -2;
    }

    // try {
    int *script = component->scripts[scriptId];
    int _register = 0;
    int pc = 0;

    while (true) {
        int opcode = script[pc++];
        if (opcode == 0) {
            return _register;
        }

        if (opcode == 1) { // load_skill_level {skill}
            _register += c->skillLevel[script[pc++]];
        } else if (opcode == 2) { // load_skill_base_level {skill}
            _register += c->skillBaseLevel[script[pc++]];
        } else if (opcode == 3) { // load_skill_exp {skill}
            _register += c->skillExperience[script[pc++]];
        } else if (opcode == 4) { // load_inv_count {interface id} {obj id}
            Component *com = component_get(script[pc++]);
            int obj = script[pc++] + 1;

            for (int i = 0; i < com->width * com->height; i++) {
                if (com->invSlotObjId[i] == obj) {
                    _register += com->invSlotObjCount[i];
                }
            }
        } else if (opcode == 5) { // load_var {id}
            _register += c->varps[script[pc++]];
        } else if (opcode == 6) { // load_next_level_xp {skill}
            _register += _Client.levelExperience[c->skillBaseLevel[script[pc++]] - 1];
        } else if (opcode == 7) {
            _register += c->varps[script[pc++]] * 100 / 46875;
        } else if (opcode == 8) { // load_combat_level
            _register += c->local_player->combatLevel;
        } else if (opcode == 9) { // load_total_level
            for (int i = 0; i < 19; i++) {
                if (i == 18) {
                    // runecrafting
                    i = 20;
                }

                _register += c->skillBaseLevel[i];
            }
        } else if (opcode == 10) { // load_inv_contains {interface id} {obj id}
            Component *com = component_get(script[pc++]);
            int obj = script[pc++] + 1;

            for (int i = 0; i < com->width * com->height; i++) {
                if (com->invSlotObjId[i] == obj) {
                    _register += 999999999;
                    break;
                }
            }
        } else if (opcode == 11) { // load_energy
            _register += c->energy;
        } else if (opcode == 12) { // load_weight
            _register += c->weightCarried;
        } else if (opcode == 13) { // load_bool {varp} {bit: 0..31}
            int varp = c->varps[script[pc++]];
            int lsb = script[pc++];

            _register += (varp & 0x1 << lsb) == 0 ? 0 : 1;
        }
    }
    // } catch (@Pc(282) Exception ex) {
    // 	return -1;
    // }
}

void projectFromGround(Client *c, PathingEntity *entity, int height) {
    projectFromGround2(c, entity->x, height, entity->z);
}

void projectFromGround2(Client *c, int x, int height, int z) {
    if (x < 128 || z < 128 || x > 13056 || z > 13056) {
        c->projectX = -1;
        c->projectY = -1;
        return;
    }

    int y = getHeightmapY(c, c->currentLevel, x, z) - height;
    project(c, x, y, z);
}

void project(Client *c, int x, int y, int z) {
    int dx = x - c->cameraX;
    int dy = y - c->cameraY;
    int dz = z - c->cameraZ;

    int sinPitch = _Pix3D.sin_table[c->cameraPitch];
    int cosPitch = _Pix3D.cos_table[c->cameraPitch];
    int sinYaw = _Pix3D.sin_table[c->cameraYaw];
    int cosYaw = _Pix3D.cos_table[c->cameraYaw];

    int tmp = (dz * sinYaw + dx * cosYaw) >> 16;
    dz = (dz * cosYaw - dx * sinYaw) >> 16;
    dx = tmp;

    tmp = (dy * cosPitch - dz * sinPitch) >> 16;
    dz = (dy * sinPitch + dz * cosPitch) >> 16;
    dy = tmp;

    if (dz >= 50) {
        c->projectX = _Pix3D.center_x + (dx << 9) / dz;
        c->projectY = _Pix3D.center_y + (dy << 9) / dz;
    } else {
        c->projectX = -1;
        c->projectY = -1;
    }
}

static void draw2DEntityElements(Client *c) {
    c->chatCount = 0;

    for (int index = -1; index < c->player_count + c->npc_count; index++) {
        PathingEntity *entity;
        if (index == -1) {
            entity = &c->local_player->pathing_entity;
        } else if (index < c->player_count) {
            entity = &c->players[c->player_ids[index]]->pathing_entity;
        } else {
            entity = &c->npcs[c->npc_ids[index - c->player_count]]->pathing_entity;
        }

        if (!entity || !pathingentity_is_visible(entity)) {
            continue;
        }

        // TODO
        // if (c->show_debug) {
        // 	// true tile overlay
        // 	if (entity.pathLength > 0 || entity.forceMoveEndCycle >= loopCycle || entity.forceMoveStartCycle > loopCycle) {
        // 		int halfUnit = 64 * entity.size;
        // 		c->drawTileOverlay(entity.pathTileX[0] * 128 + halfUnit, entity.pathTileZ[0] * 128 + halfUnit, c->currentLevel, entity.size, 0x00FFFF, false);
        // 	}

        // 	// local tile overlay
        // 	c->drawTileOverlay(entity.x, entity.z, c->currentLevel, entity.size, 0x666666, false);

        // 	int offsetY = 0;
        // 	c->projectFromGround(entity, entity.height + 30);

        // 	if (index < c->playerCount) {
        // 		// player debug
        // 		PlayerEntity player = (PlayerEntity) entity;

        // 		c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, player.name, 0xffffff);
        // 		offsetY -= 15;

        // 		if (player.lastMask != -1 && loopCycle - player.lastMaskCycle < 30) {
        // 			if ((player.lastMask & 0x1) == 0x1) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Appearance Update", 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((player.lastMask & 0x2) == 0x2) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Play Seq: " + player.primarySeqId, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((player.lastMask & 0x4) == 0x4) {
        // 				int target = player.targetId;
        // 				if (target > 32767) {
        // 					target -= 32768;
        // 				}
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Face Entity: " + target, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((player.lastMask & 0x8) == 0x8) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Say", 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((player.lastMask & 0x10) == 0x10) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Hit: Type " + player.damageType + " Amount " + player.damage + " HP " + player.health + "/" + player.totalHealth, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((player.lastMask & 0x20) == 0x20) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Face Coord: " + (player.lastFaceX / 2) + " " + (player.lastFaceZ / 2), 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((player.lastMask & 0x40) == 0x40) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Chat", 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((player.lastMask & 0x100) == 0x100) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Play Spotanim: " + player.spotanimId, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((player.lastMask & 0x200) == 0x200) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Exact Move", 0xffffff);
        // 				offsetY -= 15;
        // 			}
        // 		}
        // 	} else {
        // 		// npc debug
        // 		NpcEntity npc = (NpcEntity) entity;

        // 		c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, npc.type.name, 0xffffff);
        // 		offsetY -= 15;

        // 		if (npc.lastMask != -1 && loopCycle - npc.lastMaskCycle < 30) {
        // 			if ((npc.lastMask & 0x2) == 0x2) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Play Seq: " + npc.primarySeqId, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((npc.lastMask & 0x4) == 0x4) {
        // 				int target = npc.targetId;
        // 				if (target > 32767) {
        // 					target -= 32768;
        // 				}
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Face Entity: " + target, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((npc.lastMask & 0x8) == 0x8) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Say", 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((npc.lastMask & 0x10) == 0x10) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Hit: Type " + npc.damageType + " Amount " + npc.damage + " HP " + npc.health + "/" + npc.totalHealth, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((npc.lastMask & 0x20) == 0x20) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Change Type: " + npc.type.index, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((npc.lastMask & 0x40) == 0x40) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Play Spotanim: " + npc.spotanimId, 0xffffff);
        // 				offsetY -= 15;
        // 			}

        // 			if ((npc.lastMask & 0x80) == 0x80) {
        // 				c->fontPlain11.drawStringCenter(c->projectX, c->projectY + offsetY, "Face Coord: " + (npc.lastFaceX / 2) + " " + (npc.lastFaceZ / 2), 0xffffff);
        // 				offsetY -= 15;
        // 			}
        // 		}
        // 	}
        // }

        if (index < c->player_count) {
            int y = 30;

            PlayerEntity *player = (PlayerEntity *)entity;
            if (player->headicons != 0) {
                projectFromGround(c, entity, entity->height + 15);

                if (c->projectX > -1) {
                    for (int icon = 0; icon < 8; icon++) {
                        if ((player->headicons & 0x1 << icon) != 0) {
                            pix24_draw(c->image_headicons[icon], c->projectX - 12, c->projectY - y);
                            y -= 25;
                        }
                    }
                }
            }

            if (index >= 0 && c->hint_type == 10 && c->hint_player == c->player_ids[index]) {
                projectFromGround(c, entity, entity->height + 15);
                if (c->projectX > -1) {
                    pix24_draw(c->image_headicons[7], c->projectX - 12, c->projectY - y);
                }
            }
        } else if (c->hint_type == 1 && c->hint_npc == c->npc_ids[index - c->player_count] && _Client.loop_cycle % 20 < 10) {
            projectFromGround(c, entity, entity->height + 15);
            if (c->projectX > -1) {
                pix24_draw(c->image_headicons[2], c->projectX - 12, c->projectY - 28);
            }
        } else {
            NpcEntity *npc = (NpcEntity *)entity;
            if (npc->type->headicon != -1) {
                projectFromGround(c, entity, entity->height + 15);
                if (c->projectX > -1) {
                    pix24_draw(c->image_headicons[npc->type->headicon], c->projectX - 12, c->projectY - 30);
                }
            }
        }

        if (entity->chat[0] && (index >= c->player_count || c->public_chat_setting == 0 || c->public_chat_setting == 3 || (c->public_chat_setting == 1 && client_is_friend(c, ((PlayerEntity *)entity)->name)))) {
            projectFromGround(c, entity, entity->height);

            if (c->projectX > -1 && c->chatCount < MAX_CHATS) {
                c->chatWidth[c->chatCount] = stringWidth(c->font_bold12, entity->chat) / 2;
                c->chatHeight[c->chatCount] = c->font_bold12->height;
                c->chatX[c->chatCount] = c->projectX;
                c->chatY[c->chatCount] = c->projectY;

                c->chatColors[c->chatCount] = entity->chatColor;
                c->chatStyles[c->chatCount] = entity->chatStyle;
                c->chatTimers[c->chatCount] = entity->chatTimer;
                strcpy(c->chats[c->chatCount++], entity->chat);

                if (c->chatEffects == 0 && entity->chatStyle == 1) {
                    c->chatHeight[c->chatCount] += 10;
                    c->chatY[c->chatCount] += 5;
                }

                if (c->chatEffects == 0 && entity->chatStyle == 2) {
                    c->chatWidth[c->chatCount] = 60;
                }
            }
        }

        if (entity->combatCycle > _Client.loop_cycle + 100) {
            projectFromGround(c, entity, entity->height + 15);

            if (c->projectX > -1) {
                int w = entity->health * 30 / entity->totalHealth;
                if (w > 30) {
                    w = 30;
                }
                pix2d_fill_rect(c->projectX - 15, c->projectY - 3, GREEN, w, 5);
                pix2d_fill_rect(c->projectX - 15 + w, c->projectY - 3, RED, 30 - w, 5);
            }
        }

        if (entity->combatCycle > _Client.loop_cycle + 330) {
            projectFromGround(c, entity, entity->height / 2);

            if (c->projectX > -1) {
                int splatX = entity->combatCycle2 > _Client.loop_cycle + 330 ? c->projectX - 24 : c->projectX - 12;
                pix24_draw(c->image_hitmarks[entity->damageType], splatX, c->projectY - 12);
                char *damage = valueof(entity->damage);
                drawStringCenter(c->font_plain11, splatX + 12, c->projectY + 4, damage, BLACK);
                drawStringCenter(c->font_plain11, splatX + 11, c->projectY + 3, damage, WHITE);
                free(damage);
            }
        }

        if (entity->combatCycle2 > _Client.loop_cycle + 330) {
            projectFromGround(c, entity, entity->height / 2);

            if (c->projectX > -1) {
                int splatX = entity->combatCycle > _Client.loop_cycle + 330 ? c->projectX + 24 : c->projectX - 12;
                pix24_draw(c->image_hitmarks[entity->damageType2], splatX, c->projectY - 12);
                char *damage2 = valueof(entity->damage2);
                drawStringCenter(c->font_plain11, splatX + 12, c->projectY + 4, damage2, BLACK);
                drawStringCenter(c->font_plain11, splatX + 11, c->projectY + 3, damage2, WHITE);
                free(damage2);
            }
        }
    }

    // TODO
    // if (c->show_debug) {
    // 	for (int i = 0; i < c->userTileMarkers.length; i++) {
    // 		if (c->userTileMarkers[i] == null || c->userTileMarkers[i].level != c->currentLevel || c->userTileMarkers[i].x < 0 || c->userTileMarkers[i].z < 0 || c->userTileMarkers[i].x >= 104 || c->userTileMarkers[i].z >= 104) {
    // 			continue;
    // 		}

    // 		c->drawTileOverlay(c->userTileMarkers[i].x * 128 + 64, c->userTileMarkers[i].z * 128 + 64, c->userTileMarkers[i].level, 1, 0xffff00, false);
    // 	}
    // }

    for (int i = 0; i < c->chatCount; i++) {
        int x = c->chatX[i];
        int y = c->chatY[i];
        int padding = c->chatWidth[i];
        int height = c->chatHeight[i];
        bool sorting = true;
        while (sorting) {
            sorting = false;
            for (int j = 0; j < i; j++) {
                if (y + 2 > c->chatY[j] - c->chatHeight[j] && y - height < c->chatY[j] + 2 && x - padding < c->chatX[j] + c->chatWidth[j] && x + padding > c->chatX[j] - c->chatWidth[j] && c->chatY[j] - c->chatHeight[j] < y) {
                    y = c->chatY[j] - c->chatHeight[j];
                    sorting = true;
                }
            }
        }
        c->projectX = c->chatX[i];
        c->projectY = c->chatY[i] = y;
        const char *message = c->chats[i];
        if (c->chatEffects == 0) {
            int color = YELLOW;
            if (c->chatColors[i] < 6) {
                color = CHAT_COLORS[c->chatColors[i]];
            }
            if (c->chatColors[i] == 6) {
                color = c->scene_cycle % 20 < 10 ? RED : YELLOW;
            }
            if (c->chatColors[i] == 7) {
                color = c->scene_cycle % 20 < 10 ? BLUE : CYAN;
            }
            if (c->chatColors[i] == 8) {
                color = c->scene_cycle % 20 < 10 ? 0xb000 : 0x80ff80;
            }
            if (c->chatColors[i] == 9) {
                int delta = 150 - c->chatTimers[i];
                if (delta < 50) {
                    color = delta * 1280 + RED;
                } else if (delta < 100) {
                    color = YELLOW - (delta - 50) * 327680;
                } else if (delta < 150) {
                    color = (delta - 100) * 5 + GREEN;
                }
            }
            if (c->chatColors[i] == 10) {
                int delta = 150 - c->chatTimers[i];
                if (delta < 50) {
                    color = delta * 5 + RED;
                } else if (delta < 100) {
                    color = MAGENTA - (delta - 50) * 327680;
                } else if (delta < 150) {
                    color = (delta - 100) * 327680 + BLUE - (delta - 100) * 5;
                }
            }
            if (c->chatColors[i] == 11) {
                int delta = 150 - c->chatTimers[i];
                if (delta < 50) {
                    color = WHITE - delta * 327685;
                } else if (delta < 100) {
                    color = (delta - 50) * 327685 + GREEN;
                } else if (delta < 150) {
                    color = WHITE - (delta - 100) * 327680;
                }
            }
            if (c->chatStyles[i] == 0) {
                drawStringCenter(c->font_bold12, c->projectX, c->projectY + 1, message, BLACK);
                drawStringCenter(c->font_bold12, c->projectX, c->projectY, message, color);
            }
            if (c->chatStyles[i] == 1) {
                drawCenteredWave(c->font_bold12, c->projectX, c->projectY + 1, message, BLACK, c->scene_cycle);
                drawCenteredWave(c->font_bold12, c->projectX, c->projectY, message, color, c->scene_cycle);
            }
            if (c->chatStyles[i] == 2) {
                int w = stringWidth(c->font_bold12, message);
                int offsetX = (150 - c->chatTimers[i]) * (w + 100) / 150;
                pix2d_set_clipping(334, c->projectX + 50, 0, c->projectX - 50);
                drawString(c->font_bold12, c->projectX + 50 - offsetX, c->projectY + 1, message, BLACK);
                drawString(c->font_bold12, c->projectX + 50 - offsetX, c->projectY, message, color);
                pix2d_reset_clipping();
            }
        } else {
            drawStringCenter(c->font_bold12, c->projectX, c->projectY + 1, message, BLACK);
            drawStringCenter(c->font_bold12, c->projectX, c->projectY, message, YELLOW);
        }
    }
}

static void drawTileHint(Client *c) {
    if (c->hint_type != 2) {
        return;
    }

    projectFromGround2(c, ((c->hint_tile_x - c->sceneBaseTileX) << 7) + c->hint_offset_x, c->hint_height * 2, ((c->hint_tile_z - c->sceneBaseTileZ) << 7) + c->hint_offset_z);

    if (c->projectX > -1 && _Client.loop_cycle % 20 < 10) {
        pix24_draw(c->image_headicons[2], c->projectX - 12, c->projectY - 28);
    }
}

static void updateTextures(Client *c, int cycle) {
    if (!_Client.lowmem) {
        if (_Pix3D.textureCycle[17] >= cycle) {
            Pix8 *texture = _Pix3D.textures[17];
            int bottom = texture->width * texture->height - 1;
            int adjustment = texture->width * c->scene_delta * 2;

            int8_t *src = texture->pixels;
            int8_t *dst = c->textureBuffer;
            for (int i = 0; i <= bottom; i++) {
                dst[i] = src[i - adjustment & bottom];
            }

            texture->pixels = dst;
            c->textureBuffer = src;
            pix3d_push_texture(17);
        }

        if (_Pix3D.textureCycle[24] >= cycle) {
            Pix8 *texture = _Pix3D.textures[24];
            int bottom = texture->width * texture->height - 1;
            int adjustment = texture->width * c->scene_delta * 2;

            int8_t *src = texture->pixels;
            int8_t *dst = c->textureBuffer;
            for (int i = 0; i <= bottom; i++) {
                dst[i] = src[i - adjustment & bottom];
            }

            texture->pixels = dst;
            c->textureBuffer = src;
            pix3d_push_texture(24);
        }
    }
}

static void drawPrivateMessages(Client *c) {
    if (c->split_private_chat == 0) {
        return;
    }

    PixFont *font = c->font_plain12;
    int lineOffset = 0;
    if (c->system_update_timer != 0) {
        lineOffset = 1;
    }

    for (int i = 0; i < 100; i++) {
        if (c->message_text[i][0] == '\0') {
            continue;
        }

        int type = c->message_type[i];
        int y;
        char buf[USERNAME_LENGTH + CHAT_LENGTH + 8];
        if ((type == 3 || type == 7) && (type == 7 || c->private_chat_setting == 0 || (c->private_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
            y = 329 - lineOffset * 13;
            sprintf(buf, "From %s: %s", c->message_sender[i], c->message_text[i]);
            drawString(font, 4, y, buf, BLACK);
            sprintf(buf, "From %s: %s", c->message_sender[i], c->message_text[i]);
            drawString(font, 4, y - 1, buf, CYAN);

            lineOffset++;
            if (lineOffset >= 5) {
                return;
            }
        }

        if (type == 5 && c->private_chat_setting < 2) {
            y = 329 - lineOffset * 13;
            drawString(font, 4, y, c->message_text[i], BLACK);
            drawString(font, 4, y - 1, c->message_text[i], CYAN);

            lineOffset++;
            if (lineOffset >= 5) {
                return;
            }
        }

        if (type == 6 && c->private_chat_setting < 2) {
            y = 329 - lineOffset * 13;
            sprintf(buf, "To %s: %s", c->message_sender[i], c->message_text[i]);
            drawString(font, 4, y, buf, BLACK);
            sprintf(buf, "To %s: %s", c->message_sender[i], c->message_text[i]);
            drawString(font, 4, y - 1, buf, CYAN);

            lineOffset++;
            if (lineOffset >= 5) {
                return;
            }
        }
    }
}

static void drawWildyLevel(Client *c) {
    int x = (c->local_player->pathing_entity.x >> 7) + c->sceneBaseTileX;
    int z = (c->local_player->pathing_entity.z >> 7) + c->sceneBaseTileZ;

    if (x >= 2944 && x < 3392 && z >= 3520 && z < 6400) {
        c->wildernessLevel = (z - 3520) / 8 + 1;
    } else if (x >= 2944 && x < 3392 && z >= 9920 && z < 12800) {
        c->wildernessLevel = (z - 9920) / 8 + 1;
    } else {
        c->wildernessLevel = 0;
    }

    c->worldLocationState = 0;
    if (x >= 3328 && x < 3392 && z >= 3200 && z < 3264) {
        int localX = x & 63;
        int localZ = z & 63;

        if (localX >= 4 && localX <= 29 && localZ >= 44 && localZ <= 58) {
            c->worldLocationState = 1;
        } else if (localX >= 36 && localX <= 61 && localZ >= 44 && localZ <= 58) {
            c->worldLocationState = 1;
        } else if (localX >= 4 && localX <= 29 && localZ >= 25 && localZ <= 39) {
            c->worldLocationState = 1;
        } else if (localX >= 36 && localX <= 61 && localZ >= 25 && localZ <= 39) {
            c->worldLocationState = 1;
        } else if (localX >= 4 && localX <= 29 && localZ >= 6 && localZ <= 20) {
            c->worldLocationState = 1;
        } else if (localX >= 36 && localX <= 61 && localZ >= 6 && localZ <= 20) {
            c->worldLocationState = 1;
        }
    }

    if (c->worldLocationState == 0 && x >= 3328 && x <= 3393 && z >= 3203 && z <= 3325) {
        c->worldLocationState = 2;
    }

    c->overrideChat = 0;
    if (x >= 3053 && x <= 3156 && z >= 3056 && z <= 3136) {
        c->overrideChat = 1;
    } else if (x >= 3072 && x <= 3118 && z >= 9492 && z <= 9535) {
        c->overrideChat = 1;
    }

    if (c->overrideChat == 1 && x >= 3139 && x <= 3199 && z >= 3008 && z <= 3062) {
        c->overrideChat = 0;
    }
}

static void drawTooltip(Client *c) {
    if (c->menu_size < 2 && c->obj_selected == 0 && c->spell_selected == 0) {
        return;
    }

    char tooltip[DOUBLE_STR];
    if (c->obj_selected == 1 && c->menu_size < 2) {
        sprintf(tooltip, "Use %s with...", c->objSelectedName);
    } else if (c->spell_selected == 1 && c->menu_size < 2) {
        sprintf(tooltip, "%s...", c->spellCaption);
    } else {
        strcpy(tooltip, c->menu_option[c->menu_size - 1]);
    }

    if (c->menu_size > 2) {
        char tmp[MAX_STR];
        strcpy(tmp, tooltip);
        sprintf(tooltip, "%s@whi@ / %d more options", tmp, (c->menu_size - 2));
    }

    drawStringTooltip(c->font_bold12, 4, 15, tooltip, WHITE, true, _Client.loop_cycle / 1000);
}

static void draw3DEntityElements(Client *c) {
    drawPrivateMessages(c);
    if (c->cross_mode == 1) {
        pix24_draw(c->image_crosses[c->cross_cycle / 100], c->crossX - 8 - 8, c->crossY - 8 - 11);
    }

    if (c->cross_mode == 2) {
        pix24_draw(c->image_crosses[c->cross_cycle / 100 + 4], c->crossX - 8 - 8, c->crossY - 8 - 11);
    }

    if (c->viewport_interface_id != -1) {
        client_update_interface_animation(c, c->viewport_interface_id, c->scene_delta);
        client_draw_interface(c, component_get(c->viewport_interface_id), 0, 0, 0);
    }

    drawWildyLevel(c);

    if (!c->menu_visible) {
        client_handle_input(c);
        drawTooltip(c);
    } else if (c->menu_area == 0) {
        client_draw_menu(c);
    }

    if (c->in_multizone == 1) {
        if (c->wildernessLevel > 0 || c->worldLocationState == 1) {
            pix24_draw(c->image_headicons[1], 472, 258);
        } else {
            pix24_draw(c->image_headicons[1], 472, 296);
        }
    }

    char buf[HALF_STR];
    if (c->wildernessLevel > 0) {
        pix24_draw(c->image_headicons[0], 472, 296);
        sprintf(buf, "Level: %d", c->wildernessLevel);
        drawStringCenter(c->font_plain12, 484, 329, buf, YELLOW);
    }

    if (c->worldLocationState == 1) {
        pix24_draw(c->image_headicons[6], 472, 296);
        drawStringCenter(c->font_plain12, 484, 329, "Arena", YELLOW);
    }

    if (c->system_update_timer != 0) {
        int seconds = c->system_update_timer / 50;
        int minutes = seconds / 60;
        seconds %= 60;

        if (seconds < 10) {
            sprintf(buf, "System update in: %d:0%d", minutes, seconds);
            drawString(c->font_plain12, 4, 329, buf, YELLOW);
        } else {
            sprintf(buf, "System update in: %d:%d", minutes, seconds);
            drawString(c->font_plain12, 4, 329, buf, YELLOW);
        }
    }

    draw_info_overlay(c);
}

#ifdef __PS2__
static void ps2_draw_local_player(Client *c, int loopCycle) {
    PlayerEntity *player = c->local_player;
    if (!player || !playerentity_is_visible(player)) {
        return;
    }

    // The player can sit well outside the tiny camera-centred terrain window.  Submit it directly
    // instead of enlarging World3D's entire traversal.  Lowmem avoids the per-frame animated-model
    // clone; the cached pose remains adequate for this hardware-first profile.
    player->lowmem = true;
    player->y = getHeightmapY(c, c->currentLevel, player->pathing_entity.x, player->pathing_entity.z);
    Model *model = entity_draw(&player->pathing_entity.entity, loopCycle);
    if (!model) {
        return;
    }
    model_draw(model, player->pathing_entity.yaw, _World3D.sinEyePitch, _World3D.cosEyePitch,
               _World3D.sinEyeYaw, _World3D.cosEyeYaw,
               player->pathing_entity.x - _World3D.eyeX, player->y - _World3D.eyeY,
               player->pathing_entity.z - _World3D.eyeZ, LOCAL_PLAYER_INDEX << 14);
}

// The 3D renderer uses a deliberately smaller target while the UI continues to
// use the readable 512x334 surface.  Nearest-neighbour expansion keeps fixed UI
// coordinates and input paths untouched without assuming an integer scale factor.
static void ps2_upscale_viewport_3d(Client *c) {
    const int *src = c->area_viewport_3d->pixels;
    int *dst = c->area_viewport->pixels;
    for (int y = 0; y < 334; y++) {
        const int *src_row = src + (y * PS2_3D_RENDER_HEIGHT / 334) * PS2_3D_RENDER_WIDTH;
        int *dst_row = dst + y * 512;
        for (int x = 0; x < 512; x++) {
            dst_row[x] = src_row[x * PS2_3D_RENDER_WIDTH / 512];
        }
    }
}

// The normal performance strings are rendered at the deliberately low 3D
// resolution and are too small to diagnose a hardware freeze from a TV.  Copy
// a tiny text strip with nearest-neighbour scaling into the full-resolution
// viewport.  This is diagnostic-only and costs a fixed 6,480 pixel copies.
static void ps2_draw_large_status(Client *c, const char *status) {
    const int source_x = 4;
    const int source_y = 2;
    const int source_w = 180;
    const int source_h = 18;
    const int dest_x = 4;
    const int dest_y = 26;
    pix2d_fill_rect(source_x, source_y, BLACK, source_w, source_h);
    drawString(c->font_bold12, source_x + 2, source_y + 13, status, YELLOW);

    int *pixels = c->area_viewport->pixels;
    for (int y = 0; y < source_h; y++) {
        const int *src = pixels + (source_y + y) * 512 + source_x;
        int *dst0 = pixels + (dest_y + y * 2) * 512 + dest_x;
        int *dst1 = dst0 + 512;
        for (int x = 0; x < source_w; x++) {
            const int pixel = src[x];
            const int dx = x * 2;
            dst0[dx] = pixel;
            dst0[dx + 1] = pixel;
            dst1[dx] = pixel;
            dst1[dx + 1] = pixel;
        }
    }
}

static void ps2_draw_large_world_time(Client *c, uint64_t world_ms) {
    char status[128];
    // P/U have repeatedly matched B in the stable live loop.  Keep the compact
    // form within the magnified 180-pixel strip so the renderer/presenter
    // boundary is visible instead of being clipped off-screen.
    sprintf(status, "T%lu H%d D%d G%d", ps2_live_update_count,
            ps2_heap_tick_begin_kb, ps2_heap_after_draw_kb, ps2_heap_after_present_kb);
    // Keep this packet/send trace legible in the fixed 180-pixel magnified strip.
    (void)world_ms;
    ps2_draw_large_status(c, status);
}

static void ps2_runtime_checkpoint(Client *c, const char *status) {
    if (!c->area_viewport) {
        return;
    }
    pixmap_bind(c->area_viewport);
    ps2_draw_large_status(c, status);
    pixmap_draw(c->area_viewport, 4, 4);
    platform_update_surface();
}
#endif

void client_draw_scene(Client *c) {
    c->scene_cycle++;
    pushPlayers(c);
    pushNpcs(c);
    pushProjectiles(c);
    pushSpotanims(c);
    // TODO see if defines are needed
    // 2026-09-14: __PS2__ added per the original C client author's suggestion. pushLocs() doesn't just
    // advance animation timers - on every frame an animated loc's seqFrame rolls over, it re-enters
    // loctype_get_model() -> model_calculate_normals() (see the "append" block below) to regenerate
    // that loc's model geometry. That's the exact function this session's real-hardware checkpoint
    // bisection isolated the scene-build hang to, and the exact function that had a real unguarded-OOM
    // NULL-deref hazard (see model_calculate_normals()'s vertex_normal allocation, fixed this session).
    // Skipping loc animation entirely on PS2 removes both the extra memory churn AND the repeated,
    // ongoing (not just first-load) exposure to that code path during live gameplay.
#if !defined(_arch_dreamcast) && !defined(__NDS__) && !defined(__PS2__)
    pushLocs(c);
#endif

    if (!c->cutscene) {
        int pitch = c->orbit_camera_pitch;

        if (c->cameraPitchClamp / 256 > pitch) {
            pitch = c->cameraPitchClamp / 256;
        }

        if (c->cameraModifierEnabled[4] && c->cameraModifierWobbleScale[4] + 128 > pitch) {
            pitch = c->cameraModifierWobbleScale[4] + 128;
        }

        int yaw = c->orbit_camera_yaw + c->camera_anticheat_angle & 0x7ff;
        int camera_distance = pitch * 3 + 600 + c->controller_camera_zoom;
        if (camera_distance < 500) camera_distance = 500;
        if (camera_distance > 2350) camera_distance = 2350;
        orbitCamera(c, c->orbitCameraX, getHeightmapY(c, c->currentLevel, c->local_player->pathing_entity.x, c->local_player->pathing_entity.z) - 50, c->orbitCameraZ, yaw, pitch, camera_distance);

        _Client.cyclelogic2++;
        if (_Client.cyclelogic2 > 1802) {
            _Client.cyclelogic2 = 0;
            // ANTICHEAT_CYCLELOGIC2
            p1isaac(c->out, 225); // ANTICHEAT_CYCLELOGIC2
            p1(c->out, 0);
            int start = c->out->pos;
            p2(c->out, 29711);
            p1(c->out, 70);
            p1(c->out, (int)(jrand() * 256.0));
            p1(c->out, 242);
            p1(c->out, 186);
            p1(c->out, 39);
            p1(c->out, 61);
            if ((int)(jrand() * 2.0) == 0) {
                p1(c->out, 13);
            }
            if ((int)(jrand() * 2.0) == 0) {
                p2(c->out, 57856);
            }
            p2(c->out, (int)(jrand() * 65536.0));
            psize1(c->out, c->out->pos - start);
        }
    }

    int level;
    if (c->cutscene) {
        level = getTopLevelCutscene(c);
    } else {
        level = getTopLevel(c);
    }

    int cameraX = c->cameraX;
    int cameraY = c->cameraY;
    int cameraZ = c->cameraZ;
    int cameraPitch = c->cameraPitch;
    int cameraYaw = c->cameraYaw;
    int jitter;
    for (int type = 0; type < 5; type++) {
        if (c->cameraModifierEnabled[type]) {
            jitter = (int)(jrand() * (double)(c->cameraModifierJitter[type] * 2 + 1) - (double)c->cameraModifierJitter[type] + sin((double)c->cameraModifierCycle[type] * ((double)c->cameraModifierWobbleSpeed[type] / 100.0)) * (double)c->cameraModifierWobbleScale[type]);
            if (type == 0) {
                c->cameraX += jitter;
            }
            if (type == 1) {
                c->cameraY += jitter;
            }
            if (type == 2) {
                c->cameraZ += jitter;
            }
            if (type == 3) {
                c->cameraYaw = c->cameraYaw + jitter & 0x7ff;
            }
            if (type == 4) {
                c->cameraPitch += jitter;
                if (c->cameraPitch < 128) {
                    c->cameraPitch = 128;
                }
                if (c->cameraPitch > 383) {
                    c->cameraPitch = 383;
                }
            }
        }
    }
    jitter = _Pix3D.cycle;
    _Model.check_hover = true;
    _Model.picked_count = 0;
#ifdef __PS2__
    pixmap_bind(c->area_viewport_3d);
    _Pix3D.line_offset = c->area_viewport_3d_offsets;
    _Pix3D.center_x = PS2_3D_RENDER_WIDTH / 2;
    _Pix3D.center_y = PS2_3D_RENDER_HEIGHT / 2;
    _Model.mouse_x = (c->shell->mouse_x - 4) / 2;
    _Model.mouse_y = (c->shell->mouse_y - 4) / 2;
#else
    _Model.mouse_x = c->shell->mouse_x - 4;
    _Model.mouse_y = c->shell->mouse_y - 4;
#endif
#ifdef __PS2__
    // Use the viewport clear as a zero-memory sky: terrain/models simply paint over this,
    // so pixels beyond the world geometry show light blue instead of the old black void.
    // This replaces (rather than follows) pix2d_clear(), keeping the clear to one framebuffer pass.
    pix2d_fill_rect(0, 0, 0x87ceeb, PS2_3D_RENDER_WIDTH, PS2_3D_RENDER_HEIGHT);
#else
    pix2d_clear();
#endif
#if defined(__PS2__) && PS2_FLAT_TERRAIN
    // The normal terrain pass is omitted below; give entities a stable, grass-like background
    // without spending time transforming/rasterising hundreds of terrain triangles.
    pix2d_fill_rect(0, 0, 0x496d3a, PS2_3D_RENDER_WIDTH, PS2_3D_RENDER_HEIGHT);
#endif

    gl_start_drawscene();

    // NOTE rm these
    char buf[MAX_STR];
    uint64_t last = rs2_now();
    world3d_draw(c->scene, c->cameraX, c->cameraY, c->cameraZ, level, c->cameraYaw, c->cameraPitch, _Client.loop_cycle);
#ifdef __PS2__
#if PS2_RENDER_LOCAL_PLAYER
    ps2_draw_local_player(c, _Client.loop_cycle);
#endif
#endif
    uint64_t world_ms = rs2_now() - last;
    if (_Custom.show_performance) {
        sprintf(buf, "World3D: %lu ms", world_ms);
        drawStringRight(c->font_plain11, 507, 200, buf, YELLOW, true);
    }

    gl_end_drawscene(c);

    world3d_clear_temporarylocs(c->scene);
    draw2DEntityElements(c);
    drawTileHint(c);
    updateTextures(c, jitter);
    draw3DEntityElements(c);

#ifdef __PS2__
    ps2_upscale_viewport_3d(c);
    pixmap_bind(c->area_viewport);
    _Pix3D.line_offset = c->area_viewport_offsets;
    _Pix3D.center_x = 256;
    _Pix3D.center_y = 167;
    if (_Custom.show_performance) {
    ps2_draw_large_world_time(c, world_ms);
}
#endif

    static uint64_t pixmap_now;
    static uint64_t pixmap_last;
    if (_Custom.show_performance) {
        sprintf(buf, "Viewport pixmap: %lu ms", pixmap_now - pixmap_last);
#ifdef GL11
        extern bool use_opengl11; // use global use_opengl11 as gl_end_drawscene force disables it
        if (use_opengl11) {
            extern int pixcount;
            sprintf(buf, "%s, pixcount %d", buf, pixcount);
        }
#endif
        drawStringRight(c->font_plain11, 507, 213, buf, YELLOW, true);
    }
    pixmap_last = rs2_now();
    pixmap_draw(c->area_viewport, 4, 4);
    pixmap_now = rs2_now();

    c->cameraX = cameraX;
    c->cameraY = cameraY;
    c->cameraZ = cameraZ;
    c->cameraPitch = cameraPitch;
    c->cameraYaw = cameraYaw;
}

int getTopLevel(Client *c) {
    int top = 3;
    if (c->cameraPitch < 310) {
        int cameraLocalTileX = c->cameraX >> 7;
        int cameraLocalTileZ = c->cameraZ >> 7;
        int playerLocalTileX = c->local_player->pathing_entity.x >> 7;
        int playerLocalTileZ = c->local_player->pathing_entity.z >> 7;
        if ((c->levelTileFlags[c->currentLevel][cameraLocalTileX][cameraLocalTileZ] & 0x4) != 0) {
            top = c->currentLevel;
        }
        int tileDeltaX;
        if (playerLocalTileX > cameraLocalTileX) {
            tileDeltaX = playerLocalTileX - cameraLocalTileX;
        } else {
            tileDeltaX = cameraLocalTileX - playerLocalTileX;
        }
        int tileDeltaZ;
        if (playerLocalTileZ > cameraLocalTileZ) {
            tileDeltaZ = playerLocalTileZ - cameraLocalTileZ;
        } else {
            tileDeltaZ = cameraLocalTileZ - playerLocalTileZ;
        }
        int delta;
        int accumulator;
        if (tileDeltaX > tileDeltaZ) {
            delta = tileDeltaZ * 65536 / tileDeltaX;
            accumulator = 32768;
            while (cameraLocalTileX != playerLocalTileX) {
                if (cameraLocalTileX < playerLocalTileX) {
                    cameraLocalTileX++;
                } else if (cameraLocalTileX > playerLocalTileX) {
                    cameraLocalTileX--;
                }
                if ((c->levelTileFlags[c->currentLevel][cameraLocalTileX][cameraLocalTileZ] & 0x4) != 0) {
                    top = c->currentLevel;
                }
                accumulator += delta;
                if (accumulator >= 65536) {
                    accumulator -= 65536;
                    if (cameraLocalTileZ < playerLocalTileZ) {
                        cameraLocalTileZ++;
                    } else if (cameraLocalTileZ > playerLocalTileZ) {
                        cameraLocalTileZ--;
                    }
                    if ((c->levelTileFlags[c->currentLevel][cameraLocalTileX][cameraLocalTileZ] & 0x4) != 0) {
                        top = c->currentLevel;
                    }
                }
            }
        } else {
            delta = tileDeltaX * 65536 / tileDeltaZ;
            accumulator = 32768;
            while (cameraLocalTileZ != playerLocalTileZ) {
                if (cameraLocalTileZ < playerLocalTileZ) {
                    cameraLocalTileZ++;
                } else if (cameraLocalTileZ > playerLocalTileZ) {
                    cameraLocalTileZ--;
                }
                if ((c->levelTileFlags[c->currentLevel][cameraLocalTileX][cameraLocalTileZ] & 0x4) != 0) {
                    top = c->currentLevel;
                }
                accumulator += delta;
                if (accumulator >= 65536) {
                    accumulator -= 65536;
                    if (cameraLocalTileX < playerLocalTileX) {
                        cameraLocalTileX++;
                    } else if (cameraLocalTileX > playerLocalTileX) {
                        cameraLocalTileX--;
                    }
                    if ((c->levelTileFlags[c->currentLevel][cameraLocalTileX][cameraLocalTileZ] & 0x4) != 0) {
                        top = c->currentLevel;
                    }
                }
            }
        }
    }
    if ((c->levelTileFlags[c->currentLevel][c->local_player->pathing_entity.x >> 7][c->local_player->pathing_entity.z >> 7] & 0x4) != 0) {
        top = c->currentLevel;
    }
    return top;
}

int getTopLevelCutscene(Client *c) {
    int y = getHeightmapY(c, c->currentLevel, c->cameraX, c->cameraZ);
    return y - c->cameraY >= 800 || (c->levelTileFlags[c->currentLevel][c->cameraX >> 7][c->cameraZ >> 7] & 0x4) == 0 ? 3 : c->currentLevel;
}

int getHeightmapY(Client *c, int level, int sceneX, int sceneZ) {
    int tileX = MIN(sceneX >> 7, 104 - 1);
    int tileZ = MIN(sceneZ >> 7, 104 - 1);
    int realLevel = level;
    if (level < 3 && (c->levelTileFlags[1][tileX][tileZ] & 0x2) == 2) {
        realLevel = level + 1;
    }

    int tileLocalX = sceneX & 0x7f;
    int tileLocalZ = sceneZ & 0x7f;
    int y00 = (c->levelHeightmap[realLevel][tileX][tileZ] * (128 - tileLocalX) + c->levelHeightmap[realLevel][tileX + 1][tileZ] * tileLocalX) >> 7;
    int y11 = (c->levelHeightmap[realLevel][tileX][tileZ + 1] * (128 - tileLocalX) + c->levelHeightmap[realLevel][tileX + 1][tileZ + 1] * tileLocalX) >> 7;
    return (y00 * (128 - tileLocalZ) + y11 * tileLocalZ) >> 7;
}

void orbitCamera(Client *c, int targetX, int targetY, int targetZ, int yaw, int pitch, int distance) {
    int invPitch = 2048 - pitch & 0x7ff;
    int invYaw = 2048 - yaw & 0x7ff;
    int x = 0;
    int z = 0;
    int y = distance;
    int sin;
    int cos;
    int tmp;

    if (invPitch != 0) {
        sin = _Pix3D.sin_table[invPitch];
        cos = _Pix3D.cos_table[invPitch];
        tmp = (z * cos - distance * sin) >> 16;
        y = (z * sin + distance * cos) >> 16;
        z = tmp;
    }

    if (invYaw != 0) {
        sin = _Pix3D.sin_table[invYaw];
        cos = _Pix3D.cos_table[invYaw];
        tmp = (y * sin + x * cos) >> 16;
        y = (y * cos - x * sin) >> 16;
        x = tmp;
    }

    c->cameraX = targetX - x;
    c->cameraY = targetY - z;
    c->cameraZ = targetZ - y;
    c->cameraPitch = pitch;
    c->cameraYaw = yaw;
}

void pushLocs(Client *c) {
    for (LocEntity *loc = (LocEntity *)linklist_head(c->locList); loc; loc = (LocEntity *)linklist_next(c->locList)) {
        bool append = false;
        loc->seqCycle += c->scene_delta;
        if (loc->seqFrame == -1) {
            loc->seqFrame = 0;
            append = true;
        }

        while (loc->seqCycle > seqtype_get_duration(loc->seq, loc->seqFrame)) {
            loc->seqCycle -= seqtype_get_duration(loc->seq, loc->seqFrame) + 1;
            loc->seqFrame++;

            append = true;

            if (loc->seqFrame >= loc->seq->frameCount) {
                loc->seqFrame -= loc->seq->replayoff;

                if (loc->seqFrame < 0 || loc->seqFrame >= loc->seq->frameCount) {
                    linkable_unlink(&loc->link);
                    free(loc);
                    append = false;
                    break;
                }
            }
        }

        if (append) {
            int level = loc->level;
            int x = loc->x;
            int z = loc->z;

            int bitset = 0;
            if (loc->type == 0) {
                bitset = world3d_get_wallbitset(c->scene, level, x, z);
            }

            if (loc->type == 1) {
                bitset = world3d_get_walldecorationbitset(c->scene, level, z, x);
            }

            if (loc->type == 2) {
                bitset = world3d_get_locbitset(c->scene, level, x, z);
            }

            if (loc->type == 3) {
                bitset = world3d_get_grounddecorationbitset(c->scene, level, x, z);
            }

            if (bitset != 0 && (bitset >> 14 & 0x7fff) == loc->index) {
                int heightmapSW = c->levelHeightmap[level][x][z];
                int heightmapSE = c->levelHeightmap[level][x + 1][z];
                int heightmapNE = c->levelHeightmap[level][x + 1][z + 1];
                int heightmapNW = c->levelHeightmap[level][x][z + 1];

                LocType *type = loctype_get(loc->index);
                int seqId = -1;
                if (loc->seqFrame != -1) {
                    seqId = loc->seq->frames[loc->seqFrame];
                }

                if (loc->type == 2) {
                    int info = world3d_get_info(c->scene, level, x, z, bitset);
                    int shape = info & 0x1f;
                    int rotation = info >> 6;

                    if (shape == CENTREPIECE_DIAGONAL) {
                        shape = CENTREPIECE_STRAIGHT;
                    }

                    Model *model = loctype_get_model(type, shape, rotation, heightmapSW, heightmapSE, heightmapNE, heightmapNW, seqId);
                    world3d_set_locmodel(c->scene, level, x, z, model);
                } else if (loc->type == 1) {
                    Model *model = loctype_get_model(type, WALLDECOR_STRAIGHT_NOOFFSET, 0, heightmapSW, heightmapSE, heightmapNE, heightmapNW, seqId);
                    world3d_set_walldecorationmodel(c->scene, level, x, z, model);
                } else if (loc->type == 0) {
                    int info = world3d_get_info(c->scene, level, x, z, bitset);
                    int shape = info & 0x1f;
                    int rotation = info >> 6;

                    if (shape == WALL_L) {
                        int nextRotation = rotation + 1 & 0x3;
                        Model *model1 = loctype_get_model(type, WALL_L, rotation + 4, heightmapSW, heightmapSE, heightmapNE, heightmapNW, seqId);
                        Model *model2 = loctype_get_model(type, WALL_L, nextRotation, heightmapSW, heightmapSE, heightmapNE, heightmapNW, seqId);
                        world3d_set_wallmodels(c->scene, x, z, level, model1, model2);
                    } else {
                        Model *model = loctype_get_model(type, shape, rotation, heightmapSW, heightmapSE, heightmapNE, heightmapNW, seqId);
                        world3d_set_wallmodel(c->scene, level, x, z, model);
                    }
                } else if (loc->type == 3) {
                    int info = world3d_get_info(c->scene, level, x, z, bitset);
                    int rotation = info >> 6;
                    Model *model = loctype_get_model(type, GROUNDDECOR, rotation, heightmapSW, heightmapSE, heightmapNE, heightmapNW, seqId);
                    world3d_set_grounddecorationmodel(c->scene, level, x, z, model);
                }
            } else {
                linkable_unlink(&loc->link);
                free(loc);
            }
        }
    }
}

void pushSpotanims(Client *c) {
    for (SpotAnimEntity *entity = (SpotAnimEntity *)linklist_head(c->spotanims); entity; entity = (SpotAnimEntity *)linklist_next(c->spotanims)) {
        if (entity->level != c->currentLevel || entity->seqComplete) {
            linkable_unlink(&entity->entity.link);
            free(entity);
        } else if (_Client.loop_cycle >= entity->startCycle) {
            spotanimentity_update(entity, c->scene_delta);
            if (entity->seqComplete) {
                linkable_unlink(&entity->entity.link);
                free(entity);
            } else {
                world3d_add_temporary(c->scene, entity->level, entity->x, entity->y, entity->z, NULL, &entity->entity, -1, 0, 60, false);
            }
        }
    }
}

void pushProjectiles(Client *c) {
    for (ProjectileEntity *proj = (ProjectileEntity *)linklist_head(c->projectiles); proj; proj = (ProjectileEntity *)linklist_next(c->projectiles)) {
        if (proj->level != c->currentLevel || _Client.loop_cycle > proj->lastCycle) {
            linkable_unlink(&proj->entity.link);
            free(proj);
        } else if (_Client.loop_cycle >= proj->startCycle) {
            if (proj->target > 0) {
                NpcEntity *npc = c->npcs[proj->target - 1];
                if (npc) {
                    projectileentity_update_velocity(proj, npc->pathing_entity.x, getHeightmapY(c, proj->level, npc->pathing_entity.x, npc->pathing_entity.z) - proj->offsetY, npc->pathing_entity.z, _Client.loop_cycle);
                }
            }

            if (proj->target < 0) {
                int index = -proj->target - 1;
                PlayerEntity *player;
                if (index == c->local_pid) {
                    player = c->local_player;
                } else {
                    player = c->players[index];
                }
                if (player) {
                    projectileentity_update_velocity(proj, player->pathing_entity.x, getHeightmapY(c, proj->level, player->pathing_entity.x, player->pathing_entity.z) - proj->offsetY, player->pathing_entity.z, _Client.loop_cycle);
                }
            }

            projectileentity_update(proj, c->scene_delta);
            world3d_add_temporary(c->scene, c->currentLevel, (int)proj->x, (int)proj->y, (int)proj->z, NULL, &proj->entity, -1, proj->yaw, 60, false);
        }
    }
}

void pushNpcs(Client *c) {
    for (int i = 0; i < c->npc_count; i++) {
        NpcEntity *npc = c->npcs[c->npc_ids[i]];
        int bitset = (c->npc_ids[i] << 14) + 0x20000000;

        if (!npc || !npcentity_is_visible(npc)) {
            continue;
        }

        int x = npc->pathing_entity.x >> 7;
        int z = npc->pathing_entity.z >> 7;

        if (x < 0 || x >= 104 || z < 0 || z >= 104) {
            continue;
        }

        if (npc->pathing_entity.size == 1 && (npc->pathing_entity.x & 0x7f) == 64 && (npc->pathing_entity.z & 0x7f) == 64) {
            if (c->tileLastOccupiedCycle[x][z] == c->scene_cycle) {
                continue;
            }

            c->tileLastOccupiedCycle[x][z] = c->scene_cycle;
        }

        world3d_add_temporary(c->scene, c->currentLevel, npc->pathing_entity.x, getHeightmapY(c, c->currentLevel, npc->pathing_entity.x, npc->pathing_entity.z), npc->pathing_entity.z, NULL, &npc->pathing_entity.entity, bitset, npc->pathing_entity.yaw, (npc->pathing_entity.size - 1) * 64 + 60, npc->pathing_entity.seqStretches);
    }
}

void pushPlayers(Client *c) {
    if (c->local_player->pathing_entity.x >> 7 == c->flagSceneTileX && c->local_player->pathing_entity.z >> 7 == c->flagSceneTileZ) {
        c->flagSceneTileX = 0;
    }

    for (int i = -1; i < c->player_count; i++) {
        PlayerEntity *player;
        int id;
        if (i == -1) {
            player = c->local_player;
            id = LOCAL_PLAYER_INDEX << 14;
#ifdef __PS2__
            // See ps2_draw_local_player(): do not make the camera-centred tile traversal chase
            // the local player across a large radius just to draw this one dynamic model.
            continue;
#endif
        } else {
            player = c->players[c->player_ids[i]];
            id = c->player_ids[i] << 14;
        }

        if (!player || !playerentity_is_visible(player)) {
            continue;
        }

        int stx = player->pathing_entity.x >> 7;
        int stz = player->pathing_entity.z >> 7;

        if (stx < 0 || stx >= 104 || stz < 0 || stz >= 104) {
            continue;
        }

#ifdef __PS2__
        // Crowd LOD: player state continues to update normally, but only nearby remote players are
        // submitted to the expensive software 3D path. Mid-distance players reuse the cached,
        // unanimated appearance model through PlayerEntity::lowmem. Preserve full detail for combat/
        // interaction-relevant players and temporary loc-model transformations.
        int localStx = c->local_player->pathing_entity.x >> 7;
        int localStz = c->local_player->pathing_entity.z >> 7;
        int dx = stx - localStx;
        int dz = stz - localStz;
        if (dx < 0) dx = -dx;
        if (dz < 0) dz = -dz;
        int playerDistance = dx > dz ? dx : dz;

        int playerIndex = c->player_ids[i];
        int localTarget = c->local_player->pathing_entity.targetId;
        bool interactionImportant =
            localTarget == playerIndex + 32768 ||
            player->pathing_entity.targetId == LOCAL_PLAYER_INDEX + 32768 ||
            player->locModel ||
            (player->pathing_entity.spotanimId != -1 && player->pathing_entity.spotanimFrame != -1);

        if (!interactionImportant && playerDistance > PS2_PLAYER_RENDER_RADIUS) {
            continue;
        }

        player->lowmem =
            !interactionImportant &&
            playerDistance > PS2_PLAYER_FULL_DETAIL_RADIUS;
#else
        player->lowmem = ((_Client.lowmem && c->player_count > 50) || c->player_count > 200) && i != -1 && player->pathing_entity.secondarySeqId == player->pathing_entity.seqStandId;
#endif

        if (!player->locModel || _Client.loop_cycle < player->locStartCycle || _Client.loop_cycle >= player->locStopCycle) {
            if ((player->pathing_entity.x & 0x7f) == 64 && (player->pathing_entity.z & 0x7f) == 64) {
                if (c->tileLastOccupiedCycle[stx][stz] == c->scene_cycle) {
                    continue;
                }

                c->tileLastOccupiedCycle[stx][stz] = c->scene_cycle;
            }

            player->y = getHeightmapY(c, c->currentLevel, player->pathing_entity.x, player->pathing_entity.z);
            world3d_add_temporary(c->scene, c->currentLevel, player->pathing_entity.x, player->y, player->pathing_entity.z, NULL, &player->pathing_entity.entity, id, player->pathing_entity.yaw, 60, player->pathing_entity.seqStretches);
        } else {
            player->lowmem = false;
            player->y = getHeightmapY(c, c->currentLevel, player->pathing_entity.x, player->pathing_entity.z);
            world3d_add_temporary2(c->scene, c->currentLevel, player->pathing_entity.x, player->y, player->pathing_entity.z, player->minTileX, player->minTileZ, player->maxTileX, player->maxTileZ, NULL, &player->pathing_entity.entity, id, player->pathing_entity.yaw);
        }
    }
}

void client_draw_minimap(Client *c) {
#if defined(__PS2__) && PS2_DISABLE_MINIMAP
    pixmap_bind(c->area_viewport);
    return;
#endif
    pixmap_bind(c->area_mapback);
    int angle = c->orbit_camera_yaw + c->minimap_anticheat_angle & 0x7ff;
    int anchorX = c->local_player->pathing_entity.x / 32 + 48;
    int anchorY = 464 - c->local_player->pathing_entity.z / 32;

    // (25, 5) matches the mask-building scan's own offset base (see client_load()) - the previous
    // (21, 9) predates that fix and disagreed with it by (4,-4), part of the minimap corruption.
    pix24_draw_rotated_masked(c->image_minimap, 25, 5, 146, 151, c->minimap_mask_line_offsets, c->minimap_mask_line_lengths, anchorX, anchorY, angle, c->minimap_zoom + 256);
    pix24_draw_rotated_masked(c->image_compass, 0, 0, 33, 33, c->compass_mask_line_offsets, c->compass_mask_line_lengths, 25, 25, c->orbit_camera_yaw, 256);
    for (int i = 0; i < c->activeMapFunctionCount; i++) {
        anchorX = c->activeMapFunctionX[i] * 4 + 2 - c->local_player->pathing_entity.x / 32;
        anchorY = c->activeMapFunctionZ[i] * 4 + 2 - c->local_player->pathing_entity.z / 32;
        client_draw_on_minimap(c, anchorY, c->activeMapFunctions[i], anchorX);
    }

    for (int ltx = 0; ltx < 104; ltx++) {
        for (int ltz = 0; ltz < 104; ltz++) {
            LinkList *stack = c->level_obj_stacks[c->currentLevel][ltx][ltz];
            if (stack) {
                anchorX = ltx * 4 + 2 - c->local_player->pathing_entity.x / 32;
                anchorY = ltz * 4 + 2 - c->local_player->pathing_entity.z / 32;
                client_draw_on_minimap(c, anchorY, c->image_mapdot0, anchorX);
            }
        }
    }

    for (int i = 0; i < c->npc_count; i++) {
        NpcEntity *npc = c->npcs[c->npc_ids[i]];
        if (npc && npcentity_is_visible(npc) && npc->type->minimap) {
            anchorX = npc->pathing_entity.x / 32 - c->local_player->pathing_entity.x / 32;
            anchorY = npc->pathing_entity.z / 32 - c->local_player->pathing_entity.z / 32;
            client_draw_on_minimap(c, anchorY, c->image_mapdot1, anchorX);
        }
    }

    for (int i = 0; i < c->player_count; i++) {
        PlayerEntity *player = c->players[c->player_ids[i]];
        if (player && playerentity_is_visible(player)) {
            anchorX = player->pathing_entity.x / 32 - c->local_player->pathing_entity.x / 32;
            anchorY = player->pathing_entity.z / 32 - c->local_player->pathing_entity.z / 32;

            bool friend = false;
            int64_t name37 = jstring_to_base37(player->name);
            for (int j = 0; j < c->friend_count; j++) {
                if (name37 == c->friendName37[j] && c->friendWorld[j] != 0) {
                    friend = true;
                    break;
                }
            }

            if (friend) {
                client_draw_on_minimap(c, anchorY, c->image_mapdot3, anchorX);
            } else {
                client_draw_on_minimap(c, anchorY, c->image_mapdot2, anchorX);
            }
        }
    }

    if (c->flagSceneTileX != 0) {
        anchorX = c->flagSceneTileX * 4 + 2 - c->local_player->pathing_entity.x / 32;
        anchorY = c->flagSceneTileZ * 4 + 2 - c->local_player->pathing_entity.z / 32;
        client_draw_on_minimap(c, anchorY, c->image_mapflag, anchorX);
    }

    pix2d_fill_rect(97, 78, WHITE, 3, 3);
    pixmap_bind(c->area_viewport);
}

void client_draw_on_minimap(Client *c, int dy, Pix24 *image, int dx) {
    // same class of bug as pixmap_draw()'s NULL guard - a minimap icon (flag/dot/map-function sprite)
    // can be missing from the real rev254 media archive, and this dereferenced image->crop_w
    // unconditionally. Confirmed real crash: the map-flag icon is only drawn once flagSceneTileX is
    // set (not on every tick), so this went unnoticed until well after login/scene rendering worked.
    if (!image) {
        return;
    }
    int angle = c->orbit_camera_yaw + c->minimap_anticheat_angle & 0x7ff;
    int distance = dx * dx + dy * dy;
    if (distance > 6400) {
        return;
    }

    int sinAngle = _Pix3D.sin_table[angle];
    int cosAngle = _Pix3D.cos_table[angle];

    sinAngle = sinAngle * 256 / (c->minimap_zoom + 256);
    cosAngle = cosAngle * 256 / (c->minimap_zoom + 256);

    int x = (dy * sinAngle + dx * cosAngle) >> 16;
    int y = (dy * cosAngle - dx * sinAngle) >> 16;

    if (distance > 2500) {
        pix24_draw_masked(image, x + 94 - image->crop_w / 2, 83 - y - image->crop_h / 2, c->image_mapback);
    } else {
        pix24_draw(image, x + 94 - image->crop_w / 2, 83 - y - image->crop_h / 2);
    }
}

#ifdef __PS2__
static void ps2_draw_chat_string(PixFont *font, int x, int y, const char *text, int color) {
    // The complete 765x503 software UI is scaled down for the PS2 output, so the stock p12
    // strokes become difficult to read on a real 480i/480p display. Reuse the already-loaded
    // bold font and add a one-pixel shadow instead of allocating another font/texture.
    if (color != BLACK) {
        drawString(font, x + 1, y + 1, text, BLACK);
    }
    drawString(font, x, y, text, color);
}
#endif

void client_draw_chatback(Client *c) {
    pixmap_bind(c->area_chatback);
    _Pix3D.line_offset = c->area_chatback_offsets;
#if defined(__PS2__) && PS2_SIMPLE_UI
    pix2d_fill_rect(0, 0, 0xe5e5e5, 479, 96);
#else
    pix8_draw(c->image_chatback, 0, 0);
#endif
    if (c->show_social_input) {
        char buf[CHAT_LENGTH + 2];
        sprintf(buf, "%s*", c->social_input);
        drawStringCenter(c->font_bold12, 239, 40, c->social_message, BLACK);
        drawStringCenter(c->font_bold12, 239, 60, buf, DARKBLUE);
    } else if (c->chatback_input_open) {
        char buf[CHATBACK_LENGTH + 2];
        sprintf(buf, "%s*", c->chatback_input);
        drawStringCenter(c->font_bold12, 239, 40, "Enter amount:", BLACK);
        drawStringCenter(c->font_bold12, 239, 60, buf, DARKBLUE);
    } else if (c->modal_message[0]) {
        drawStringCenter(c->font_bold12, 239, 40, c->modal_message, BLACK);
        drawStringCenter(c->font_bold12, 239, 60, "Click to continue", DARKBLUE);
    } else if (c->chat_interface_id != -1) {
        client_draw_interface(c, component_get(c->chat_interface_id), 0, 0, 0);
    } else if (c->sticky_chat_interface_id == -1) {
#ifdef __PS2__
        // Keep the same 479x96 chatbox and the same loaded font set: bold12 is clearer after
        // the PS2's final framebuffer scale and costs no additional persistent memory.
        PixFont *font = c->font_bold12;
        const int chatLineHeight = 15;
        const int chatBaseY = 74;
        const int chatInputY = 92;
#define CHAT_DRAW(px, py, str, rgb) ps2_draw_chat_string(font, (px), (py), (str), (rgb))
#else
        PixFont *font = c->font_plain12;
        if (_Custom.chat_era == 0) {
            font = c->font_quill8;
        }
        const int chatLineHeight = 14;
        const int chatBaseY = 70;
        const int chatInputY = 90;
#define CHAT_DRAW(px, py, str, rgb) drawString(font, (px), (py), (str), (rgb))
#endif
        int line = 0;
        pix2d_set_clipping(77, 463, 0, 0);
        for (int i = 0; i < 100; i++) {
            if (c->message_text[i][0]) {
                int type = c->message_type[i];
                int offset = c->chat_scroll_offset + chatBaseY - line * chatLineHeight;
                if (type == 0) {
                    if (offset > 0 && offset < 110) {
                        CHAT_DRAW(4, offset, c->message_text[i], BLACK);
                    }
                    line++;
                }
                if (type == 1) {
                    if (offset > 0 && offset < 110) {
                        char buf[USERNAME_LENGTH + 2];
                        sprintf(buf, "%s:", c->message_sender[i]);
                        CHAT_DRAW(4, offset, buf, WHITE);
                        CHAT_DRAW(stringWidth(font, c->message_sender[i]) + 12, offset, c->message_text[i], BLUE);
                    }
                    line++;
                }
                if (type == 2 && (c->public_chat_setting == 0 || (c->public_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
                    if (offset > 0 && offset < 110) {
                        char buf[USERNAME_LENGTH + 2];
                        sprintf(buf, "%s:", c->message_sender[i]);
                        CHAT_DRAW(4, offset, buf, BLACK);
                        CHAT_DRAW(stringWidth(font, c->message_sender[i]) + 12, offset, c->message_text[i], BLUE);
                    }
                    line++;
                }
                if ((type == 3 || type == 7) && c->split_private_chat == 0 && (type == 7 || c->private_chat_setting == 0 || (c->private_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
                    if (offset > 0 && offset < 110) {
                        char buf[USERNAME_LENGTH + 7];
                        sprintf(buf, "From %s:", c->message_sender[i]);
                        CHAT_DRAW(4, offset, buf, BLACK);
                        sprintf(buf, "From %s", c->message_sender[i]);
                        CHAT_DRAW(stringWidth(font, buf) + 12, offset, c->message_text[i], DARKRED);
                    }
                    line++;
                }
                if (type == 4 && (c->trade_chat_setting == 0 || (c->trade_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
                    if (offset > 0 && offset < 110) {
                        char buf[USERNAME_LENGTH + CHAT_LENGTH + 2];
                        sprintf(buf, "%s %s", c->message_sender[i], c->message_text[i]);
                        CHAT_DRAW(4, offset, buf, TRADE_MESSAGE);
                    }
                    line++;
                }
                if (type == 5 && c->split_private_chat == 0 && c->private_chat_setting < 2) {
                    if (offset > 0 && offset < 110) {
                        CHAT_DRAW(4, offset, c->message_text[i], DARKRED);
                    }
                    line++;
                }
                if (type == 6 && c->split_private_chat == 0 && c->private_chat_setting < 2) {
                    if (offset > 0 && offset < 110) {
                        char buf[USERNAME_LENGTH + 6];
                        sprintf(buf, "To %s:", c->message_sender[i]);
                        CHAT_DRAW(4, offset, buf, BLACK);
                        sprintf(buf, "To %s", c->message_sender[i]);
                        CHAT_DRAW(stringWidth(font, buf) + 12, offset, c->message_text[i], DARKRED);
                    }
                    line++;
                }
                if (type == 8 && (c->trade_chat_setting == 0 || (c->trade_chat_setting == 1 && client_is_friend(c, c->message_sender[i])))) {
                    if (offset > 0 && offset < 110) {
                        char buf[USERNAME_LENGTH + CHAT_LENGTH + 2];
                        sprintf(buf, "%s %s", c->message_sender[i], c->message_text[i]);
                        CHAT_DRAW(4, offset, buf, DUEL_MESSAGE);
                    }
                    line++;
                }
            }
        }
        pix2d_reset_clipping();
        c->chat_scroll_height = line * chatLineHeight + 7;
        if (c->chat_scroll_height < 78) {
            c->chat_scroll_height = 78;
        }
        client_draw_scrollbar(c, 463, 0, c->chat_scroll_height - c->chat_scroll_offset - 77, c->chat_scroll_height, 77);

        if (_Custom.chat_era == 0) {
            // 186-194?
            char buf2[CHAT_LENGTH + 2];
            sprintf(buf2, "%s*", c->chat_typed);
            CHAT_DRAW(3, chatInputY, buf2, BLACK);
        } else if (_Custom.chat_era == 1) {
            // <204
            char buf2[CHAT_LENGTH + 2];
            sprintf(buf2, "%s*", c->chat_typed);
            CHAT_DRAW(3, chatInputY, buf2, BLUE);
        } else if (_Custom.chat_era == 2) {
            // 204+
            char buf[USERNAME_LENGTH + 3];
            sprintf(buf, "%s:", jstring_format_name(c->username));
            CHAT_DRAW(4, chatInputY, buf, BLACK);
            sprintf(buf, "%s: ", c->username);

            char buf2[CHAT_LENGTH + 2];
            sprintf(buf2, "%s*", c->chat_typed);
            CHAT_DRAW(stringWidth(font, buf) + 6, chatInputY, buf2, BLUE);
        }

        pix2d_hline(0, 77, BLACK, 479);
    } else {
        client_draw_interface(c, component_get(c->sticky_chat_interface_id), 0, 0, 0);
    }
    if (c->menu_visible && c->menu_area == 2) {
        client_draw_menu(c);
    }
    pixmap_draw(c->area_chatback, 17, 357);
    pixmap_bind(c->area_viewport);
    _Pix3D.line_offset = c->area_viewport_offsets;
#undef CHAT_DRAW
}

bool client_is_friend(Client *c, const char *username) {
    if (!username) {
        return false;
    }

    for (int i = 0; i < c->friend_count; i++) {
        if (platform_strcasecmp(username, c->friendName[i]) == 0) {
            return true;
        }
    }

    return (platform_strcasecmp(username, c->local_player->name) == 0);
}

void client_draw_scrollbar(Client *c, int x, int y, int scrollY, int scrollHeight, int height) {
    pix8_draw(c->image_scrollbar0, x, y);
    pix8_draw(c->image_scrollbar1, x, y + height - 16);
    pix2d_fill_rect(x, y + 16, SCROLLBAR_TRACK, 16, height - 32);

    int gripSize = (height - 32) * height / scrollHeight;
    if (gripSize < 8) {
        gripSize = 8;
    }

    int gripY = (height - gripSize - 32) * scrollY / (scrollHeight - height);
    pix2d_fill_rect(x, y + gripY + 16, SCROLLBAR_GRIP_FOREGROUND, 16, gripSize);

    pix2d_vline(x, y + gripY + 16, SCROLLBAR_GRIP_HIGHLIGHT, gripSize);
    pix2d_vline(x + 1, y + gripY + 16, SCROLLBAR_GRIP_HIGHLIGHT, gripSize);

    pix2d_hline(x, y + gripY + 16, SCROLLBAR_GRIP_HIGHLIGHT, 16);
    pix2d_hline(x, y + gripY + 17, SCROLLBAR_GRIP_HIGHLIGHT, 16);

    pix2d_vline(x + 15, y + gripY + 16, SCROLLBAR_GRIP_LOWLIGHT, gripSize);
    pix2d_vline(x + 14, y + gripY + 17, SCROLLBAR_GRIP_LOWLIGHT, gripSize - 1);

    pix2d_hline(x, y + gripY + gripSize + 15, SCROLLBAR_GRIP_LOWLIGHT, 16);
    pix2d_hline(x + 1, y + gripY + gripSize + 14, SCROLLBAR_GRIP_LOWLIGHT, 15);
}

void client_draw_sidebar(Client *c) {
    pixmap_bind(c->area_sidebar);
    _Pix3D.line_offset = c->area_sidebar_offsets;
#if defined(__PS2__) && PS2_SIMPLE_UI
    pix2d_fill_rect(0, 0, 0x252a33, 190, 261);
#else
    pix8_draw(c->image_invback, 0, 0);
#endif
    if (c->sidebar_interface_id != -1) {
        client_draw_interface(c, component_get(c->sidebar_interface_id), 0, 0, 0);
    } else if (c->tab_interface_id[c->selected_tab] != -1) {
        client_draw_interface(c, component_get(c->tab_interface_id[c->selected_tab]), 0, 0, 0);
    }
    if (c->menu_visible && c->menu_area == 1) {
        client_draw_menu(c);
    }
    pixmap_draw(c->area_sidebar, 553, 205);
    pixmap_bind(c->area_viewport);
    _Pix3D.line_offset = c->area_viewport_offsets;
}

void client_update_interface_content(Client *c, Component *component) {
    // Every branch below writes component->text directly (strcpy/sprintf/strcat) - now that it's
    // allocated lazily rather than a fixed inline array, guarantee it exists once here instead of
    // touching every one of those call sites individually.
    if (!component->text) {
        component->text = malloc(DOUBLE_STR);
        component->text[0] = '\0';
    }

    int clientCode = component->clientCode;

    if (clientCode >= 1 && clientCode <= 100) {
        clientCode--;
        if (clientCode >= c->friend_count) {
            strcpy(component->text, "");
            component->buttonType = 0;
        } else {
            strcpy(component->text, c->friendName[clientCode]);
            component->buttonType = 1;
        }
    } else if (clientCode >= 101 && clientCode <= 200) {
        clientCode -= 101;
        if (clientCode >= c->friend_count) {
            strcpy(component->text, "");
            component->buttonType = 0;
        } else {
            if (c->friendWorld[clientCode] == 0) {
                strcpy(component->text, "@red@Offline");
            } else if (c->friendWorld[clientCode] == _Client.nodeid) {
                sprintf(component->text, "@gre@World-%d", c->friendWorld[clientCode] - 9);
            } else {
                sprintf(component->text, "@yel@World-%d", c->friendWorld[clientCode] - 9);
            }
            component->buttonType = 1;
        }
    } else if (clientCode == 203) {
        component->scroll = c->friend_count * 15 + 20;
        if (component->scroll <= component->height) {
            component->scroll = component->height + 1;
        }
    } else if (clientCode >= 401 && clientCode <= 500) {
        clientCode -= 401;
        if (clientCode >= c->ignoreCount) {
            strcpy(component->text, "");
            component->buttonType = 0;
        } else {
            char *ignore_name = jstring_format_name(jstring_from_base37(c->ignoreName37[clientCode]));
            strcpy(component->text, ignore_name);
            free(ignore_name);
            component->buttonType = 1;
        }
    } else if (clientCode == 503) {
        component->scroll = c->ignoreCount * 15 + 20;
        if (component->scroll <= component->height) {
            component->scroll = component->height + 1;
        }
    } else if (clientCode == 327) {
        component->xan = 150;
        component->yan = (int)(sin((double)_Client.loop_cycle / 40.0) * 256.0) & 0x7ff;
        if (c->update_design_model) {
            c->update_design_model = false;

            Model **models = calloc(7, sizeof(Model *));
            int modelCount = 0;
            for (int part = 0; part < 7; part++) {
                int kit = c->designIdentikits[part];
                if (kit >= 0) {
                    models[modelCount++] = idktype_get_model(_IdkType.instances[kit]);
                }
            }

            Model *model = model_from_models(models, modelCount, false);
            for (int part = 0; part < modelCount; part++) {
                model_free(models[part]);
            }
            free(models);
            for (int part = 0; part < 5; part++) {
                if (c->design_colors[part] != 0) {
                    model_recolor(model, DESIGN_BODY_COLOR[part][0], DESIGN_BODY_COLOR[part][c->design_colors[part]]);
                    if (part == 1) {
                        model_recolor(model, DESIGN_HAIR_COLOR[0], DESIGN_HAIR_COLOR[c->design_colors[part]]);
                    }
                }
            }

            model_create_label_references(model, false);
            model_apply_transform(model, _SeqType.instances[c->local_player->pathing_entity.seqStandId]->frames[0]);
            model_calculate_normals(model, 64, 850, -30, -50, -30, true, false);
            component_set_dynamic_model(component, model);
        }
    } else if (clientCode == 324) {
#ifdef __PS2__
        component_ensure_graphic(component);
#endif
        if (!c->genderButtonImage0) {
            c->genderButtonImage0 = component->graphic;
            c->genderButtonImage1 = component->activeGraphic;
        }
        if (c->design_gender_male) {
            component->graphic = c->genderButtonImage1;
        } else {
            component->graphic = c->genderButtonImage0;
        }
    } else if (clientCode == 325) {
#ifdef __PS2__
        component_ensure_graphic(component);
#endif
        if (!c->genderButtonImage0) {
            c->genderButtonImage0 = component->graphic;
            c->genderButtonImage1 = component->activeGraphic;
        }
        if (c->design_gender_male) {
            component->graphic = c->genderButtonImage0;
        } else {
            component->graphic = c->genderButtonImage1;
        }
    } else if (clientCode == 600) {
        strcpy(component->text, c->reportAbuseInput);
        if (_Client.loop_cycle % 20 < 10) {
            strcat(component->text, "|");
        } else {
            strcat(component->text, " ");
        }
    } else if (clientCode == 613) {
        if (!c->rights) {
            strcpy(component->text, "");
        } else if (c->reportAbuseMuteOption) {
            component->colour = RED;
            strcpy(component->text, "Moderator option: Mute player for 48 hours: <ON>");
        } else {
            component->colour = WHITE;
            strcpy(component->text, "Moderator option: Mute player for 48 hours: <OFF>");
        }
    } else if (clientCode == 650 || clientCode == 655) {
        if (c->lastAddress == 0) {
            strcpy(component->text, "");
        } else {
            char text[HALF_STR];
            if (c->daysSinceLastLogin == 0) {
                strcpy(text, "earlier today");
            } else if (c->daysSinceLastLogin == 1) {
                strcpy(text, "yesterday");
            } else {
                sprintf(text, "%d days ago", c->daysSinceLastLogin);
            }
            sprintf(component->text, "You last logged in %s from: %s", text, _Client.dns);
        }
    } else if (clientCode == 651) {
        if (c->unreadMessages == 0) {
            strcpy(component->text, "0 unread messages");
            component->colour = YELLOW;
        }
        if (c->unreadMessages == 1) {
            strcpy(component->text, "1 unread message");
            component->colour = GREEN;
        }
        if (c->unreadMessages > 1) {
            sprintf(component->text, "%d unread messages", c->unreadMessages);
            component->colour = GREEN;
        }
    } else if (clientCode == 652) {
        if (c->daysSinceRecoveriesChanged == 201) {
            strcpy(component->text, "");
        } else if (c->daysSinceRecoveriesChanged == 200) {
            strcpy(component->text, "You have not yet set any password recovery questions.");
        } else {
            char text[HALF_STR];
            if (c->daysSinceRecoveriesChanged == 0) {
                strcpy(text, "Earlier today");
            } else if (c->daysSinceRecoveriesChanged == 1) {
                strcpy(text, "Yesterday");
            } else {
                sprintf(text, "%d days ago", c->daysSinceRecoveriesChanged);
            }
            sprintf(component->text, "%s you changed your recovery questions", text);
        }
    } else if (clientCode == 653) {
        if (c->daysSinceRecoveriesChanged == 201) {
            strcpy(component->text, "");
        } else if (c->daysSinceRecoveriesChanged == 200) {
            strcpy(component->text, "We strongly recommend you do so now to secure your account.");
        } else {
            strcpy(component->text, "If you do not remember making this change then cancel it immediately");
        }
    } else if (clientCode == 654) {
        if (c->daysSinceRecoveriesChanged == 201) {
            strcpy(component->text, "");
        } else if (c->daysSinceRecoveriesChanged == 200) {
            strcpy(component->text, "Do this from the 'account management' area on our front webpage");
        } else {
            strcpy(component->text, "Do this from the 'account management' area on our front webpage");
        }
    }
}

void client_set_lowmem(void) {
    _World3D.lowMemory = true;
    _Pix3D.lowMemory = true;
    _Client.lowmem = true;
    _World.lowMemory = true;
}

void client_set_highmem(void) {
    _World3D.lowMemory = false;
    _Pix3D.lowMemory = false;
    _Client.lowmem = false;
    _World.lowMemory = false;
}

static const char *formatObjCountTagged(int amount) {
    static char s[SIXTY_STR];

    char tmp[14];
    sprintf(tmp, "%d", amount);

    int len = (int)strlen(tmp);
    for (int i = len - 3; i > 0; i -= 3) {
        for (int k = len; k >= i; k--) {
            tmp[k + 1] = tmp[k];
        }
        tmp[i] = ',';
        len++;
    }

    if (len > 8) {
        sprintf(s, " @gre@%.*s million @whi@(%s)", len - 8, tmp, tmp);
    } else if (len > 4) {
        sprintf(s, " @cya@%.*sK @whi@(%s)", len - 4, tmp, tmp);
    } else {
        sprintf(s, " %s", tmp);
    }

    return s;
}

static const char *formatObjCount(int amount) {
    static char s[12];
    if (amount < 100000) {
        sprintf(s, "%d", amount);
    } else if (amount < 10000000) {
        sprintf(s, "%dK", amount / 1000);
    } else {
        sprintf(s, "%dM", amount / 1000000);
    }
    return s;
}

static char *getIntString(int value) {
    return value < 999999999 ? valueof(value) : platform_strndup("*", 1);
}

static void client_draw_interface(Client *c, Component *com, int x, int y, int scrollY) {
    if (!com) {
        return;
    }
    if (com->type != 0 || !com->childId || (com->hide && c->viewportHoveredInterfaceIndex != com->id && c->sidebarHoveredInterfaceIndex != com->id && c->chatHoveredInterfaceIndex != com->id)) {
        return;
    }

    int left = _Pix2D.left;
    int top = _Pix2D.top;
    int right = _Pix2D.right;
    int bottom = _Pix2D.bottom;

    pix2d_set_clipping(y + com->height, x + com->width, y, x);
    int children = com->childCount;

    for (int i = 0; i < children; i++) {
        int childX = com->childX[i] + x;
        int childY = com->childY[i] + y - scrollY;

        Component *child = component_get(com->childId[i]);
        childX += child->x;
        childY += child->y;

        if (child->clientCode > 0) {
            client_update_interface_content(c, child);
        }

        if (child->type == 0) {
            if (child->scrollPosition > child->scroll - child->height) {
                child->scrollPosition = child->scroll - child->height;
            }

            if (child->scrollPosition < 0) {
                child->scrollPosition = 0;
            }

            client_draw_interface(c, child, childX, childY, child->scrollPosition);
            if (child->scroll > child->height) {
                client_draw_scrollbar(c, childX + child->width, childY, child->scrollPosition, child->scroll, child->height);
            }
        } else if (child->type == 2) {
#if defined(__PS2__) && PS2_SAFE_INTERFACE
            // Inventory icons are generated by rendering object models into a
            // Pix24.  Do not enter that renderer in the hardware-safe UI pass.
            continue;
#endif
            int slot = 0;

            for (int row = 0; row < child->height; row++) {
                for (int col = 0; col < child->width; col++) {
                    int slotX = childX + col * (child->marginX + 32);
                    int slotY = childY + row * (child->marginY + 32);

                    if (slot < 20) {
                        slotX += child->invSlotOffsetX[slot];
                        slotY += child->invSlotOffsetY[slot];
                    }

                    if (child->invSlotObjId[slot] > 0) {
                        int dx = 0;
                        int dy = 0;
                        int id = child->invSlotObjId[slot] - 1;

                        if ((slotX >= -32 && slotX <= 512 && slotY >= -32 && slotY <= 334) || (c->obj_drag_area != 0 && c->objDragSlot == slot)) {
                            Pix24 *icon = NULL;
                            if (_Custom.item_outlines) {
                                int outline_color = 0;
                                if (c->obj_selected == 1 && c->objSelectedSlot == slot && c->objSelectedInterface == child->id) {
                                    outline_color = WHITE;
                                }
                                icon = objtype_get_icon_outline(id, child->invSlotObjCount[slot], outline_color);
                            } else {
                                icon = objtype_get_icon(id, child->invSlotObjCount[slot]);
                            }
                            if (c->obj_drag_area != 0 && c->objDragSlot == slot && c->objDragInterfaceId == child->id) {
                                dx = c->shell->mouse_x - c->objGrabX;
                                dy = c->shell->mouse_y - c->objGrabY;

                                if (dx < 5 && dx > -5) {
                                    dx = 0;
                                }

                                if (dy < 5 && dy > -5) {
                                    dy = 0;
                                }

                                if (c->obj_drag_cycles < 5) {
                                    dx = 0;
                                    dy = 0;
                                }

                                pix24_draw_alpha(icon, 128, slotX + dx, slotY + dy);
                            } else if (c->selected_area != 0 && c->selectedItem == slot && c->selectedInterface == child->id) {
                                pix24_draw_alpha(icon, 128, slotX, slotY);
                            } else {
                                pix24_draw(icon, slotX, slotY);
                            }

                            if (icon->crop_w == 33 || child->invSlotObjCount[slot] != 1) {
                                int count = child->invSlotObjCount[slot];
                                drawString(c->font_plain11, slotX + dx + 1, slotY + 10 + dy, formatObjCount(count), BLACK);
                                drawString(c->font_plain11, slotX + dx, slotY + 9 + dy, formatObjCount(count), YELLOW);
                            }

                            if (_Custom.item_outlines) {
                                if (c->obj_selected == 1 && c->objSelectedSlot == slot && c->objSelectedInterface == child->id) {
                                    pix24_free(icon);
                                }
                            }
                        }
                    } else if (child->invSlotSprite && slot < 20) {
#ifdef __PS2__
                        component_ensure_invslot_sprite(child, slot);
#endif
                        Pix24 *image = child->invSlotSprite[slot];

                        if (image) {
                            pix24_draw(image, slotX, slotY);
                        }
                    }

#ifdef __PS2__
                    // Controller grid focus is drawn by the same interface pass that owns the slot,
                    // so there is no ambiguity about whether snapping is active. Black outer edge +
                    // yellow inner edge remains legible over both bright item icons and dark panels.
                    if (!c->controller_grid_analog_override &&
                        c->controller_grid_component == child->id && c->controller_grid_slot == slot) {
                        pix2d_draw_rect(slotX - 2, slotY - 2, BLACK, 36, 36);
                        pix2d_draw_rect(slotX - 1, slotY - 1, YELLOW, 34, 34);

                        // Publish the final displayed slot center from the exact render pass. This
                        // eliminates coordinate drift between local interface PixMaps and the global
                        // controller cursor/click coordinate system.
                        int origin_x = 0;
                        int origin_y = 0;
                        if (c->area_sidebar && _Pix2D.pixels == c->area_sidebar->pixels) {
                            origin_x = 553;
                            origin_y = 205;
                        } else if (c->area_chatback && _Pix2D.pixels == c->area_chatback->pixels) {
                            origin_x = 17;
                            origin_y = 357;
                        } else if (c->area_viewport && _Pix2D.pixels == c->area_viewport->pixels) {
                            origin_x = 4;
                            origin_y = 4;
                        }
                        c->controller_grid_screen_x = origin_x + slotX + 16;
                        c->controller_grid_screen_y = origin_y + slotY + 16;
                        c->controller_grid_screen_valid = true;
                    }
#endif

                    slot++;
                }
            }
        } else if (child->type == 3) {
            if (child->fill) {
                pix2d_fill_rect(childX, childY, child->colour, child->width, child->height);
            } else {
                pix2d_draw_rect(childX, childY, child->colour, child->width, child->height);
            }
        } else if (child->type == 4) {
            PixFont *font = child->font;
#ifdef __PS2__
            // Dialogue/tutorial/bank text is rendered through interface components, not the normal
            // chat-history path. When that interface is being drawn into the chatback, use the
            // already-resident bold12 font as well. This improves real-TV readability without
            // introducing a larger font asset or retaining any extra chat surfaces.
            const bool ps2ChatText = c->area_chatback && _Pix2D.pixels == c->area_chatback->pixels;
            if (ps2ChatText) {
                font = c->font_bold12;
            }
#endif
            int color = child->colour;
            char text[DOUBLE_STR];
            strcpy(text, child->text);

            if ((c->chatHoveredInterfaceIndex == child->id || c->sidebarHoveredInterfaceIndex == child->id || c->viewportHoveredInterfaceIndex == child->id) && child->overColour != 0) {
                color = child->overColour;
            }

            if (client_execute_interface_script(c, child)) {
                color = child->activeColour;

                if (strlen(child->activeText) > 0) {
                    strcpy(text, child->activeText);
                }
            }

            if (child->buttonType == BUTTON_CONTINUE && c->pressed_continue_option) {
                strcpy(text, "Please wait...");
                color = child->colour;
            }

            for (int lineY = childY + font->height; strlen(text) > 0; lineY += font->height) {
                if (indexof_chr(text, '%') != -1) {
                    do {
                        int index = indexof(text, "%1");
                        if (index == -1) {
                            break;
                        }

                        char *sub = substring(text, 0, index);
                        char *value = getIntString(client_execute_clientscript1(c, child, 0));
                        char *sub1 = substring(text, index + 2, strlen(text));
                        sprintf(text, "%s%s%s", sub, value, sub1);
                        free(sub);
                        free(value);
                        free(sub1);
                    } while (true);

                    do {
                        int index = indexof(text, "%2");
                        if (index == -1) {
                            break;
                        }

                        char *sub = substring(text, 0, index);
                        char *value = getIntString(client_execute_clientscript1(c, child, 1));
                        char *sub1 = substring(text, index + 2, strlen(text));
                        sprintf(text, "%s%s%s", sub, value, sub1);
                        free(sub);
                        free(value);
                        free(sub1);
                    } while (true);

                    do {
                        int index = indexof(text, "%3");
                        if (index == -1) {
                            break;
                        }

                        char *sub = substring(text, 0, index);
                        char *value = getIntString(client_execute_clientscript1(c, child, 2));
                        char *sub1 = substring(text, index + 2, strlen(text));
                        sprintf(text, "%s%s%s", sub, value, sub1);
                        free(sub);
                        free(value);
                        free(sub1);
                    } while (true);

                    do {
                        int index = indexof(text, "%4");
                        if (index == -1) {
                            break;
                        }

                        char *sub = substring(text, 0, index);
                        char *value = getIntString(client_execute_clientscript1(c, child, 3));
                        char *sub1 = substring(text, index + 2, strlen(text));
                        sprintf(text, "%s%s%s", sub, value, sub1);
                        free(sub);
                        free(value);
                        free(sub1);
                    } while (true);

                    do {
                        int index = indexof(text, "%5");
                        if (index == -1) {
                            break;
                        }

                        char *sub = substring(text, 0, index);
                        char *value = getIntString(client_execute_clientscript1(c, child, 4));
                        char *sub1 = substring(text, index + 2, strlen(text));
                        sprintf(text, "%s%s%s", sub, value, sub1);
                        free(sub);
                        free(value);
                        free(sub1);
                    } while (true);
                }

                int newline = indexof(text, "\\n");
                char split[DOUBLE_STR];
                if (newline != -1) {
                    char *sub = substring(text, 0, newline);
                    strcpy(split, sub);
                    free(sub);
                    char *sub1 = substring(text, newline + 2, strlen(text));
                    strcpy(text, sub1);
                    free(sub1);
                } else {
                    strcpy(split, text);
                    strcpy(text, "");
                }

#ifdef __PS2__
                // Prefer bold12 for chatbox readability, but never make a cache-authored dialogue
                // line wider than its component. Long lines fall back to their original font so
                // the PS2 accessibility tweak cannot clip text that previously fit.
                PixFont *drawFont = font;
                if (ps2ChatText && child->font && child->width > 0 &&
                    stringWidth(drawFont, split) > child->width) {
                    drawFont = child->font;
                }
#else
                PixFont *drawFont = font;
#endif
                if (child->center) {
#ifdef __PS2__
                    drawStringTaggableCenter(drawFont, split, childX + child->width / 2, lineY, color, child->shadowed || ps2ChatText);
#else
                    drawStringTaggableCenter(drawFont, split, childX + child->width / 2, lineY, color, child->shadowed);
#endif
                } else {
#ifdef __PS2__
                    drawStringTaggable(drawFont, childX, lineY, split, color, child->shadowed || ps2ChatText);
#else
                    drawStringTaggable(drawFont, childX, lineY, split, color, child->shadowed);
#endif
                }
            }
        } else if (child->type == 5) {
#ifdef __PS2__
            component_ensure_graphic(child);
#endif
            Pix24 *image;
            if (client_execute_interface_script(c, child)) {
                image = child->activeGraphic;
            } else {
                image = child->graphic;
            }

            if (image) {
                pix24_draw(image, childX, childY);
            }
        } else if (child->type == 6) {
#if defined(__PS2__) && PS2_SAFE_INTERFACE
            // Component type 6 is a live 3D model (equipment preview, prayer
            // tab artwork, etc.).  It shares the unsafe software model path.
            continue;
#endif
            int tmpX = _Pix3D.center_x;
            int tmpY = _Pix3D.center_y;

            _Pix3D.center_x = childX + child->width / 2;
            _Pix3D.center_y = childY + child->height / 2;

            int eyeY = _Pix3D.sin_table[child->xan] * child->zoom >> 16;
            int eyeZ = _Pix3D.cos_table[child->xan] * child->zoom >> 16;

            bool active = client_execute_interface_script(c, child);
            int seqId;
            if (active) {
                seqId = child->activeAnim;
            } else {
                seqId = child->anim;
            }
            // child->anim/activeAnim can come from either a live IF_SETANIM packet or the local
            // interface archive's own decode (component_unpack) - guard here too as a second layer,
            // since _SeqType.instances[seqId] below is otherwise indexed with zero bounds checking.
            if (seqId < -1 || seqId >= _SeqType.count) {
                seqId = -1;
            }

            Model *model;
            bool _free = false;
            if (seqId == -1) {
                model = component_get_model2(child, -1, -1, active, &_free);
            } else {
                SeqType *seq = _SeqType.instances[seqId];
                model = component_get_model2(child, seq->frames[child->seqFrame], seq->iframes[child->seqFrame], active, &_free);
            }

            if (model) {
                model_draw_simple(model, 0, child->yan, 0, child->xan, 0, eyeY, eyeZ);
                if (_free) {
                    model_free_label_references(model);
                    model_free_calculate_normals(model);
                    model_free_share_colored(model, true, true, false);
                }
            }

            _Pix3D.center_x = tmpX;
            _Pix3D.center_y = tmpY;
        } else if (child->type == 7) {
            PixFont *font = child->font;
            int slot = 0;
            for (int row = 0; row < child->height; row++) {
                for (int col = 0; col < child->width; col++) {
                    if (child->invSlotObjId[slot] > 0) {
                        ObjType *obj = objtype_get(child->invSlotObjId[slot] - 1);
                        char text[MAX_STR];
                        strcpy(text, obj->name);
                        if (obj->stackable || child->invSlotObjCount[slot] != 1) {
                            char tmp[HALF_STR];
                            strcpy(tmp, text);
                            sprintf(text, "%s x%s", tmp, formatObjCountTagged(child->invSlotObjCount[slot]));
                        }

                        int textX = childX + col * (child->marginX + 115);
                        int textY = childY + row * (child->marginY + 12);

                        if (child->center) {
                            drawStringTaggableCenter(font, text, textX + child->width / 2, textY, child->colour, child->shadowed);
                        } else {
                            drawStringTaggable(font, textX, textY, text, child->colour, child->shadowed);
                        }
                    }

                    slot++;
                }
            }
        }
    }

    pix2d_set_clipping(bottom, right, top, left);
}

void client_draw_menu(Client *c) {
    int x = c->menu_x;
    int y = c->menu_y;
    int w = c->menu_width;
    int h = c->menu_height;
    int background = OPTIONS_MENU;
    pix2d_fill_rect(x, y, background, w, h);
    pix2d_fill_rect(x + 1, y + 1, BLACK, w - 2, 16);
    pix2d_draw_rect(x + 1, y + 18, BLACK, w - 2, h - 19);

    drawString(c->font_bold12, x + 3, y + 14, "Choose Option", background);
    int mouseX = c->shell->mouse_x;
    int mouseY = c->shell->mouse_y;
    if (c->menu_area == 0) {
        mouseX -= 4;
        mouseY -= 4;
    }
    if (c->menu_area == 1) {
        mouseX -= 553;
        mouseY -= 205;
    }
    if (c->menu_area == 2) {
        mouseX -= 17;
        mouseY -= 357;
    }

    for (int i = 0; i < c->menu_size; i++) {
        int optionY = y + (c->menu_size - 1 - i) * 15 + 31;
        int rgb = WHITE;
        if (c->controller_menu_index == i ||
            (c->controller_menu_index < 0 && mouseX > x && mouseX < x + w &&
             mouseY > optionY - 13 && mouseY < optionY + 3)) {
            rgb = YELLOW;
        }
        drawStringTaggable(c->font_bold12, x + 3, optionY, c->menu_option[i], rgb, true);
    }
}

void client_draw_error(Client *c) {
    platform_set_color(BLACK);
    platform_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    gameshell_set_framerate(c->shell, 1);

    if (c->error_loading) {
        c->flame_active = false;
        int y = 35;

        platform_set_font("Helvetica", true, 16);
        platform_set_color(YELLOW);
        platform_draw_string("Sorry, an error has occured whilst loading RuneScape", 30, y);
        y += 50;

        platform_set_color(WHITE);
        platform_draw_string("To fix this try the following (in order):", 30, y);
        y += 50;

        platform_set_color(WHITE);
        platform_set_font("Helvetica", true, 12);
        platform_draw_string("1: Try closing ALL open web-browser windows, and reloading", 30, y);
        y += 30;

        platform_draw_string("2: Try clearing your web-browsers cache from tools->internet options", 30, y);
        y += 30;

        platform_draw_string("3: Try using a different game-world", 30, y);
        y += 30;

        platform_draw_string("4: Try rebooting your computer", 30, y);
        y += 30;

        platform_draw_string("5: Try selecting a different version of Java from the play-game menu", 30, y);
    }

    if (c->error_host) {
        c->flame_active = false;
        platform_set_font("Helvetica", true, 20);
        platform_set_color(WHITE);
        platform_draw_string("Error - unable to load game!", 50, 50);
        platform_draw_string("To play RuneScape make sure you play from", 50, 100);
        platform_draw_string("http://www.runescape.com", 50, 150);
    }

    if (c->error_started) {
        c->flame_active = false;
        int y = 35;

        platform_set_color(YELLOW);
        platform_draw_string("Error a copy of RuneScape already appears to be loaded", 30, y);
        y += 50;

        platform_set_color(WHITE);
        platform_draw_string("To fix this try the following (in order):", 30, y);
        y += 50;

        platform_set_color(WHITE);
        platform_set_font("Helvetica", true, 12);
        platform_draw_string("1: Try closing ALL open web-browser windows, and reloading", 30, y);
        y += 30;

        platform_draw_string("2: Try rebooting your computer, and reloading", 30, y);
        y += 30;
    }
}

void client_draw_title_screen(Client *c) {
    client_load_title(c);
    pixmap_bind(c->image_title4);
    pix8_draw(c->image_titlebox, 0, 0);

    int w = 360;
    int h = 200;

    if (c->title_screen_state == 0) {
        int y = h / 2 - 20;
        drawStringTaggableCenter(c->font_bold12, "Welcome to RuneScape", w / 2, y, YELLOW, true);

        int x = w / 2 - 80;
        y = h / 2 + 20;

        pix8_draw(c->image_titlebutton, x - 73, y - 20);
        drawStringTaggableCenter(c->font_bold12, "New user", x, y + 5, WHITE, true);

        x = w / 2 + 80;
        pix8_draw(c->image_titlebutton, x - 73, y - 20);
        drawStringTaggableCenter(c->font_bold12, "Existing User", x, y + 5, WHITE, true);
    } else if (c->title_screen_state == 2) {
        int y = h / 2 - 40;
        if (strlen(c->login_message0) > 0) {
            drawStringTaggableCenter(c->font_bold12, c->login_message0, w / 2, y - 15, YELLOW, true);
            drawStringTaggableCenter(c->font_bold12, c->login_message1, w / 2, y, YELLOW, true);
            y += 30;
        } else {
            drawStringTaggableCenter(c->font_bold12, c->login_message1, w / 2, y - 7, YELLOW, true);
            y += 30;
        }

        char username[MAX_STR];
        sprintf(username, "Username: %s%s", c->username, c->title_login_field == 0 && _Client.loop_cycle % 40 < 20 ? "@yel@|" : "");
        drawStringTaggable(c->font_bold12, w / 2 - 90, y, username, WHITE, true);
        y += 15;

        char password[MAX_STR];
        char *censored = jstring_to_asterisk(c->password);
        sprintf(password, "Password: %s%s", censored, c->title_login_field == 1 && _Client.loop_cycle % 40 < 20 ? "@yel@|" : "");
        free(censored);
        drawStringTaggable(c->font_bold12, w / 2 - 88, y, password, WHITE, true);
        y += 15;

        int x = w / 2 - 80;
        y = h / 2 + 50;
        pix8_draw(c->image_titlebutton, x - 73, y - 20);
        drawStringTaggableCenter(c->font_bold12, "Login", x, y + 5, WHITE, true);

        x = w / 2 + 80;
        pix8_draw(c->image_titlebutton, x - 73, y - 20);
        drawStringTaggableCenter(c->font_bold12, "Cancel", x, y + 5, WHITE, true);
    } else if (c->title_screen_state == 3) {
        drawStringTaggableCenter(c->font_bold12, "Create a free account", w / 2, h / 2 - 60, YELLOW, true);

        int y = h / 2 - 35;
        drawStringTaggableCenter(c->font_bold12, "To create a new account you need to", w / 2, y, WHITE, true);
        y += 15;

        drawStringTaggableCenter(c->font_bold12, "go back to the main RuneScape webpage", w / 2, y, WHITE, true);
        y += 15;

        drawStringTaggableCenter(c->font_bold12, "and choose the red 'create account'", w / 2, y, WHITE, true);
        y += 15;

        drawStringTaggableCenter(c->font_bold12, "button at the top right of that page.", w / 2, y, WHITE, true);
        y += 15;

        int x = w / 2;
        y = h / 2 + 50;
        pix8_draw(c->image_titlebutton, x - 73, y - 20);
        drawStringTaggableCenter(c->font_bold12, "Cancel", x, y + 5, WHITE, true);
    }

    pixmap_draw(c->image_title4, 202, 171);
#ifdef GL11
    c->redraw_background = true;

    c->image_title0->dirty = true;
    c->image_title1->dirty = true;
    pixmap_draw(c->image_title0, 0, 0);
    pixmap_draw(c->image_title1, 637, 0);
#endif
    if (c->redraw_background) {
        c->redraw_background = false;
        pixmap_draw(c->image_title2, 128, 0);
        pixmap_draw(c->image_title3, 202, 371);
        pixmap_draw(c->image_title5, 0, 265);
        pixmap_draw(c->image_title6, 562, 265);
        pixmap_draw(c->image_title7, 128, 171);
        pixmap_draw(c->image_title8, 562, 171);
    }

    client_run_flames(c); // NOTE: random placement of run_flames
}

void client_unload(Client *c) {
    bump_allocator_free();

    model_free_global();
    animbase_free_global();
    animframe_free_global();
    component_free_global();
    pix3d_free_global();
    tone_free_global();
    wave_free_global();
    objtype_free_global();
    loctype_free_global();
    npctype_free_global();
    seqtype_free_global();
    varptype_free_global();
    varbittype_free_global();
    playerentity_free_global();
    spotanimtype_free_global();
    idktype_free_global();
    flotype_free_global();
    packet_free_global();
    world3d_free_global();
    wordfilter_free_global();

    client_free(c);
}

#ifdef __EMSCRIPTEN__
#include "emscripten.h"

EM_JS(bool, get_host_js, (char *socketip, size_t len, int *http_port), {
    const url = new URL(window.location.href);
    stringToUTF8(url.hostname, socketip, len);
    if (url.port && url.hostname != 'localhost' && url.hostname != '127.0.0.1') {
        HEAP32[http_port >> 2] = parseInt(url.port, 10);
    }
    const secured = url.protocol == 'https';
    const protocol = secured ? 'wss' : 'ws';
    // TODO: check https://github.com/emscripten-core/emscripten/issues/22969
    SOCKFS.websocketArgs = {'url' : protocol + '://'};
    return secured;
})

static bool secured = false;
#endif

int main(int argc, char **argv) {
#ifdef __PS2__
    // Preserve the launcher-supplied ELF path before platform/USB initialization. The PS2 asset
    // resolver later validates this directory and uses it for config.ini + the complete rom tree.
    ps2_set_launch_path((argc > 0 && argv) ? argv[0] : NULL);
#endif
    // init screen before logging is required for some platforms
    if (!platform_init()) {
        rs2_error("Failed to init platform!\n");
        rs2_sleep(5000);
        return 1;
    }
    // to print argv on emscripten you need to print index to flush instead of just \n?
    rs2_log("RS2 user client - release #%d\n", _Client.clientversion);

#ifdef __EMSCRIPTEN__
    // we fetch instead of preload config.ini to avoid leaking account details
    emscripten_wget("config.ini", "config.ini");

    if (argc != 5) {
        if (load_ini_args()) {
            _Client.lowmem ? client_set_lowmem() : client_set_highmem();
            goto init;
        }
        rs2_error("Usage: node-id, port-offset, [lowmem/highmem], [free/members]\n");
        return 0;
    }

    secured = get_host_js(_Client.socketip, MAX_STR - 1, &_Custom.http_port);
    _Client.nodeid = atoi(argv[1]);
    _Client.portoff = atoi(argv[2]);

    const char *lowmem = argv[3];
    if (lowmem && strcmp(lowmem, "1") == 0) {
        client_set_lowmem();
    } else {
        client_set_highmem();
    }

    const char *_free = argv[4];
    _Client.members = !_free || strcmp(_free, "1") != 0;
#else
#ifdef __TINYC__ // tcc -run passes many args
    if (load_ini_args()) {
#else
    // some console sdks (nxdk) have argc set to 0 with empty argv
    if (argc <= 1 && load_ini_args()) {
#endif
        _Client.lowmem ? client_set_lowmem() : client_set_highmem();
        goto init;
    }

    if (argc != 5) {
        rs2_error("Usage: node-id, port-offset, [lowmem/highmem], [free/members]\n");
        return 0;
    } else {
        _Client.nodeid = atoi(argv[1]);
        _Client.portoff = atoi(argv[2]);
        if (strcmp(argv[3], "lowmem") == 0) {
            client_set_lowmem();
        } else {
            if (strcmp(argv[3], "highmem") != 0) {
                rs2_error("Usage: node-id, port-offset, [lowmem/highmem], [free/members]\n");
                return 0;
            }
            client_set_highmem();
        }
        if (strcmp(argv[4], "free") == 0) {
            _Client.members = false;
        } else {
            if (strcmp(argv[4], "members") != 0) {
                rs2_error("Usage: node-id, port-offset, [lowmem/highmem], [free/members]\n");
                return 0;
            }
            _Client.members = true;
        }
    }
#endif

init:
    srand(0);
    client_init_global();
    model_init_global();
    packet_init_global();
    pix3d_init_global();
    pixfont_init_global();
    playerentity_init_global();
    world_init_global();
    world3d_init_global();

    Client *c = client_new();
    load_ini_config(c);
    gameshell_init_application(c, SCREEN_WIDTH, SCREEN_HEIGHT);
    return 0;
}

void client_free(Client *c) {
    free(c->stream);
    client_scenemap_free(c);
    gameshell_free(c->shell);
    pixfont_free(c->font_plain11);
    pixfont_free(c->font_plain12);
    pixfont_free(c->font_bold12);
    pixfont_free(c->font_quill8);
    jagfile_free(c->archive_title);

    if (c->area_chatback) {
        // other images are allocated at same time
        pixmap_free(c->area_chatback);
        pixmap_free(c->area_mapback);
        pixmap_free(c->area_sidebar);
        pixmap_free(c->area_viewport);
#ifdef __PS2__
        pixmap_free(c->area_viewport_3d);
#endif
        pixmap_free(c->area_backbase1);
        pixmap_free(c->area_backbase2);
        pixmap_free(c->area_backhmid1);
    }

    if (c->image_titlebox) {
        // other images are allocated at same time
        pix8_free(c->image_titlebox);
        pix8_free(c->image_titlebutton);
#ifndef DISABLE_FLAMES
        for (int i = 0; i < 12; i++) {
            pix8_free(c->image_runes[i]);
        }
        free(c->image_runes);
        free(c->flame_gradient0);
        free(c->flame_gradient1);
        free(c->flame_gradient2);
        free(c->flame_gradient);
        free(c->flame_buffer0);
        free(c->flame_buffer1);
        free(c->flame_buffer3);
        free(c->flame_buffer2);
        pix24_free(c->image_flames_left);
        pix24_free(c->image_flames_right);
#endif
        pixmap_free(c->image_title0);
        pixmap_free(c->image_title1);
        pixmap_free(c->image_title2);
        pixmap_free(c->image_title3);
        pixmap_free(c->image_title4);
        pixmap_free(c->image_title5);
        pixmap_free(c->image_title6);
        pixmap_free(c->image_title7);
        pixmap_free(c->image_title8);
    }

    pix24_free(c->image_minimap);
    pix8_free(c->image_invback);
    pix8_free(c->image_chatback);
    pix8_free(c->image_mapback);
    pix8_free(c->image_backbase1);
    pix8_free(c->image_backbase2);
    pix8_free(c->image_backhmid1);
    for (int i = 0; i < 13; i++) {
        pix8_free(c->image_sideicons[i]);
    }
    free(c->image_sideicons);
    pix24_free(c->image_compass);
    for (int i = 0; i < 50; i++) {
        if (c->image_mapscene[i]) {
            pix8_free(c->image_mapscene[i]);
        }
    }
    free(c->image_mapscene);
    for (int i = 0; i < 50; i++) {
        if (c->image_mapfunction[i]) {
            pix24_free(c->image_mapfunction[i]);
        }
    }
    free(c->image_mapfunction);
    for (int i = 0; i < 20; i++) {
        if (c->image_hitmarks[i]) {
            pix24_free(c->image_hitmarks[i]);
        }
    }
    free(c->image_hitmarks);
    for (int i = 0; i < 20; i++) {
        if (c->image_headicons[i]) {
            pix24_free(c->image_headicons[i]);
        }
    }
    free(c->image_headicons);
    pix24_free(c->image_mapflag);
    for (int i = 0; i < 8; i++) {
        pix24_free(c->image_crosses[i]);
    }
    free(c->image_crosses);
    pix24_free(c->image_mapdot0);
    pix24_free(c->image_mapdot1);
    pix24_free(c->image_mapdot2);
    pix24_free(c->image_mapdot3);
    pix8_free(c->image_scrollbar0);
    pix8_free(c->image_scrollbar1);
    pix8_free(c->image_redstone1);
    pix8_free(c->image_redstone2);
    pix8_free(c->image_redstone3);
    pix8_free(c->image_redstone1h);
    pix8_free(c->image_redstone2h);
    pix8_free(c->image_redstone1v);
    pix8_free(c->image_redstone2v);
    pix8_free(c->image_redstone3v);
    pix8_free(c->image_redstone1hv);
    pix8_free(c->image_redstone2hv);
    pixmap_free(c->area_backleft1);
    pixmap_free(c->area_backleft2);
    pixmap_free(c->area_backright1);
    pixmap_free(c->area_backright2);
    pixmap_free(c->area_backtop1);
    pixmap_free(c->area_backvmid1);
    pixmap_free(c->area_backvmid2);
    pixmap_free(c->area_backvmid3);
    pixmap_free(c->area_backhmid2);
    free(c->compass_mask_line_offsets);
    free(c->compass_mask_line_lengths);
    free(c->minimap_mask_line_offsets);
    free(c->minimap_mask_line_lengths);
    packet_free(c->out);
    packet_free(c->in);
    packet_free(c->login);
    for (int i = 0; i < MAX_PLAYER_COUNT; i++) {
        free(c->players[i]);
        if (c->player_appearance_buffer[i]) {
            packet_free(c->player_appearance_buffer[i]);
        }
    }
    for (int i = 0; i < MAX_NPC_COUNT; i++) {
        free(c->npcs[i]);
    }

    free(c->design_colors);

    linklist_free(c->projectiles);
    linklist_free(c->spotanims);
    linklist_free(c->merged_locations);
    linklist_free(c->spawned_locations);
    for (int level = 0; level < 4; level++) {
        for (int x = 0; x < 104; x++) {
            for (int z = 0; z < 104; z++) {
                if (c->level_obj_stacks[level][x][z]) {
                    linklist_free(c->level_obj_stacks[level][x][z]);
                }
            }
            free(c->level_obj_stacks[level][x]);
        }
        free(c->level_obj_stacks[level]);
    }
    free(c->level_obj_stacks);
    linklist_free(c->locList);
    free(c->levelTileFlags);
    free(c->levelHeightmap);
    world3d_free(c->scene, 104, 4, 104);
    for (int level = 0; level < 4; level++) {
        collisionmap_free(c->levelCollisionMap[level]);
    }
    free(c->chat_interface);
    free(c->textureBuffer);
    free(c->area_chatback_offsets);
    free(c->area_sidebar_offsets);
    free(c->area_viewport_offsets);
#ifdef __PS2__
    free(c->area_viewport_3d_offsets);
#endif

    free(c);
}

#ifdef __PS2__
Client *ps2_crash_client = NULL;
#endif

Client *client_new(void) {
    Client *c = calloc(1, sizeof(Client));
#ifdef __PS2__
    ps2_crash_client = c;
#endif

    c->shell = gameshell_new();
    c->image_sideicons = calloc(13, sizeof(Pix8 *));
    c->image_mapscene = calloc(50, sizeof(Pix8 *));
    c->image_mapfunction = calloc(50, sizeof(Pix24 *));
    c->image_hitmarks = calloc(20, sizeof(Pix24 *));
    c->image_headicons = calloc(20, sizeof(Pix24 *));
    c->image_crosses = calloc(8, sizeof(Pix24 *));
    c->compass_mask_line_offsets = calloc(33, sizeof(int));
    c->compass_mask_line_lengths = calloc(33, sizeof(int));
    c->minimap_mask_line_offsets = calloc(151, sizeof(int));
    c->minimap_mask_line_lengths = calloc(151, sizeof(int));

    c->out = packet_alloc(1);
    c->in = packet_alloc(1);
    c->login = packet_alloc(1);
    c->orbit_camera_pitch = 128;
    c->controller_menu_index = -1;
    c->controller_cursor_deadzone = 20;
    c->controller_cursor_speed = 5;
    c->controller_camera_deadzone = 40;
    c->controller_grid_cancel_pressed = false;
    c->controller_grid_component = -1;
    c->controller_grid_slot = -1;
    c->controller_grid_screen_valid = false;
    c->controller_grid_analog_override = true;
    c->controller_free_cursor_valid = false;
    c->controller_camera_zoom = 0;

    c->minimap_level = -1;
    c->sticky_chat_interface_id = -1;
    c->chat_interface_id = -1;
    c->viewport_interface_id = -1;
    c->sidebar_interface_id = -1;
    c->selected_tab = 3;
    c->flashing_tab = -1;
    c->design_gender_male = true;
    c->design_colors = malloc(5 * sizeof(int));
    memset(c->tab_interface_id, -1, sizeof(c->tab_interface_id));
    c->projectiles = linklist_new();
    c->spotanims = linklist_new();
    c->merged_locations = linklist_new();
    c->spawned_locations = linklist_new();
    c->level_obj_stacks = calloc(4, sizeof(LinkList ***));
    for (int level = 0; level < 4; level++) {
        c->level_obj_stacks[level] = calloc(104, sizeof(LinkList **));
        for (int x = 0; x < 104; x++) {
            c->level_obj_stacks[level][x] = calloc(104, sizeof(LinkList *));
            for (int z = 0; z < 104; z++) {
                c->level_obj_stacks[level][x][z] = linklist_new();
            }
        }
    }
    c->chat_interface = calloc(1, sizeof(Component));
    c->locList = linklist_new();
    c->local_pid = -1;
    c->chat_scroll_height = 78;
    c->cameraOffsetXModifier = 2;
    c->cameraOffsetZModifier = 2;
    c->cameraOffsetYawModifier = 1;
    c->minimapAngleModifier = 2;
    c->minimapZoomModifier = 1;
    c->reportAbuseInterfaceID = -1;
    c->projectX = -1;
    c->projectY = -1;
    c->textureBuffer = calloc(16384, sizeof(int8_t));
    c->wave_enabled = true;
    c->midiActive = true;
    return c;
}

#ifdef __wasm
#ifdef __EMSCRIPTEN__
void *client_openurl(const char *name, int *size) {
    void *buffer = NULL;
    int error = 0;
    char url[PATH_MAX];
    sprintf(url, "%s://%s:%d/%s", secured ? "https" : "http", _Client.socketip, _Custom.http_port, name);
    emscripten_wget_data(url, &buffer, size, &error);
    if (error) {
        rs2_error("Error downloading %s: %d\n", url, error);
        return NULL;
    }
    return buffer;
}
#else
void *client_openurl(const char *name, int *size) {
    char url[PATH_MAX];
    bool secured = false;
    sprintf(url, "%s://%s:%d/%s", secured ? "https" : "http", _Client.socketip, _Custom.http_port, name);

    FILE *file = fopen(url, "rb");
    if (!file) {
        rs2_error("Error downloading %s: %d\n", url);
        return NULL;
    }
    // TODO this will break on newer caches
    #define MAX_FETCH 1 << 20
    uint8_t *buffer = malloc(MAX_FETCH);
    *size = fread(buffer, 1, MAX_FETCH, file);
    fclose(file);

    return buffer;
}
#endif

Jagfile *load_archive(Client *c, const char *name, int crc, const char *display_name, int progress) {
    int retry = 5;
    // int8_t *data = signlink.cacheload(name);
    int8_t *data = NULL; // TODO cacheload
    int size = 0;
    if (data) {
        int crc_value = rs_crc32(data, size);
        if (crc_value != crc) {
            rs2_log("%s archive CRC check failed\n", display_name);
            free(data);
            data = NULL;
            size = 0;
        }
    }

    if (data) {
        return jagfile_new(data, size);
    }

    while (!data) {
        char message[PATH_MAX];
        snprintf(message, sizeof(message), "Requesting %s", display_name);
        client_draw_progress(c, message, progress);

        snprintf(message, sizeof(message), "%s%d", name, crc);
        data = client_openurl(message, &size);
        if (!data) {
            for (int i = retry; i > 0; i--) {
                snprintf(message, sizeof(message), "Error loading - Will retry in %d secs.", i);
                client_draw_progress(c, message, progress);
                rs2_sleep(1000);
            }

            retry *= 2;
            if (retry > 60) {
                retry = 60;
            }
        }
    }

    // signlink.cachesave(name, data);
    return jagfile_new(data, size);
}
#else
void *client_openurl(const char *name, int *size) {
    (void)name, (void)size;
    return NULL;
}
Jagfile *load_archive(Client *c, const char *name, int crc, const char *display_name, int progress) {
    // TODO
    (void)display_name, (void)progress;
    int8_t *data;
    int8_t *header = malloc(6);
    char filename[PATH_MAX];
#ifdef _arch_dreamcast
    snprintf(filename, sizeof(filename), "cache/client/%s.", name);
#elif defined(NXDK)
    snprintf(filename, sizeof(filename), "D:\\cache\\client\\%s", name);
#elif defined(__PS2__)
    snprintf(filename, sizeof(filename), "%srom/cache/client/%s", ps2_cache_prefix(), name);
#else
    snprintf(filename, sizeof(filename), "rom/cache/client/%s", name);
#endif
    // rs2_log("Loading %s\n", filename);
    // TODO: add load messages?
    (void)c;

#ifdef ANDROID
    SDL_RWops *file = SDL_RWFromFile(filename, "rb");
#else
    FILE *file = fopen(filename, "rb");
#endif
    if (!file) {
        rs2_error("Failed to open file %s. %s\n", filename, strerror(errno));
        free(header);
        return NULL;
    }

#ifdef ANDROID
    if (SDL_RWread(file, header, 1, 6) != 6) {
#else
    if (fread(header, 1, 6, file) != 6) {
#endif
        rs2_error("Failed to read header\n", strerror(errno));
    }
    Packet *packet = packet_new(header, 6);
    packet->pos = 3;
    int file_size = g3(packet) + 6;
    int total_read = 6;
    data = malloc(file_size);
    memcpy(data, header, total_read); // or packet->data instead of header
    size_t remaining = file_size - total_read;
#ifdef ANDROID
    if (SDL_RWread(file, data + total_read, 1, remaining) != remaining) {
#else
    if (fread(data + total_read, 1, remaining, file) != remaining) {
#endif
        rs2_error("Failed to read file %s. %s\n", filename, strerror(errno));
    }
#ifdef ANDROID
    SDL_RWclose(file);
#else
    fclose(file);
#endif
    packet_free(packet);

    int crc_value = rs_crc32(data, file_size);
    if (crc_value != crc) {
        rs2_log("%s archive CRC check failed (update archive_checksums if login says RuneScape has been updated) TODO downloading\n", display_name);
        // free(data);
        // data = NULL;
    }

    return jagfile_new(data, file_size);
}
#endif

void client_load_title(Client *c) {
    if (c->image_title2) {
        return;
    }

    if (c->shell->draw_area) {
        pixmap_free(c->shell->draw_area);
        c->shell->draw_area = NULL;
    }
    if (c->area_chatback) {
        // other images are allocated at same time
        pixmap_free(c->area_chatback);
        pixmap_free(c->area_mapback);
        pixmap_free(c->area_sidebar);
        pixmap_free(c->area_viewport);
#ifdef __PS2__
        pixmap_free(c->area_viewport_3d);
#endif
        pixmap_free(c->area_backbase1);
        pixmap_free(c->area_backbase2);
        pixmap_free(c->area_backhmid1);
        c->area_chatback = NULL;
        c->area_mapback = NULL;
        c->area_sidebar = NULL;
        c->area_viewport = NULL;
#ifdef __PS2__
        c->area_viewport_3d = NULL;
#endif
        c->area_backbase1 = NULL;
        c->area_backbase2 = NULL;
        c->area_backhmid1 = NULL;
    }

    // sizes/positions corrected to the real rev254 fixed-mode (765x503) title-screen tiling grid -
    // Client3's values were calibrated for the wrong 789x532 layout (see client_load_title_background
    // and client_draw_title_screen for the corresponding blit/draw position fixes). The mirror pass's
    // split point alone (394 -> 382) explains the black vertical seam: title.dat is 383px wide, so
    // 382+383=765 tiles exactly, while 394 left an 11px uncovered gap down the middle.
    c->image_title0 = pixmap_new(128, 265);
    pix2d_clear();

    c->image_title1 = pixmap_new(128, 265);
    pix2d_clear();

    c->image_title2 = pixmap_new(509, 171);
    pix2d_clear();

    c->image_title3 = pixmap_new(360, 132);
    pix2d_clear();

    c->image_title4 = pixmap_new(360, 200);
    pix2d_clear();

    c->image_title5 = pixmap_new(202, 238);
    pix2d_clear();

    c->image_title6 = pixmap_new(203, 238);
    pix2d_clear();

    c->image_title7 = pixmap_new(74, 94);
    pix2d_clear();

    c->image_title8 = pixmap_new(75, 94);
    pix2d_clear();

    if (c->archive_title) {
        client_load_title_background(c);
        client_load_title_images(c);
    }

    c->redraw_background = true;
}

void client_draw_progress(Client *c, const char *message, int progress) {
    client_load_title(c);
    if (!c->archive_title) {
        gameshell_draw_progress(c->shell, message, progress);
    } else {
        pixmap_bind(c->image_title4);
        int x = 360;
        int y = 200;
        int offsetY = 20;
        drawStringCenter(c->font_bold12, x / 2, y / 2 - offsetY - 26, "RuneScape is loading - please wait...", WHITE);
        int midY = y / 2 - offsetY - 18;
        pix2d_draw_rect(x / 2 - 152, midY, PROGRESS_RED, 304, 34);
        pix2d_draw_rect(x / 2 - 151, midY + 1, BLACK, 302, 32);
        pix2d_fill_rect(x / 2 - 150, midY + 2, PROGRESS_RED, progress * 3, 30);
        pix2d_fill_rect(x / 2 - 150 + progress * 3, midY + 2, BLACK, 300 - progress * 3, 30);
        drawStringCenter(c->font_bold12, x / 2, y / 2 + 5 - offsetY, message, WHITE);
        pixmap_draw(c->image_title4, 202, 171);
#ifdef GL11
        c->redraw_background = true;
        if (c->flame_active) {
            pixmap_draw(c->image_title0, 0, 0);
            pixmap_draw(c->image_title1, 637, 0);
        }
#endif
        if (c->redraw_background) {
            c->redraw_background = false;
            if (!c->flame_active) {
                pixmap_draw(c->image_title0, 0, 0);
                pixmap_draw(c->image_title1, 637, 0);
            }
            pixmap_draw(c->image_title2, 128, 0);
            pixmap_draw(c->image_title3, 202, 371);
            pixmap_draw(c->image_title5, 0, 265);
            pixmap_draw(c->image_title6, 562, 265);
            pixmap_draw(c->image_title7, 128, 171);
            pixmap_draw(c->image_title8, 562, 171);
        }

        platform_update_surface();
    }
}
#endif
