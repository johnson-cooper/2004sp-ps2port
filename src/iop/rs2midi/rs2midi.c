#include <irx_imports.h>
#include <libsd.h>

#define MODNAME "rs2midi"
#define RS2MIDI_RPC_ID 0x5253324d

#define RS2MIDI_RPC_PING       0
#define RS2MIDI_RPC_LOAD       1
#define RS2MIDI_RPC_NOTE_ON    2
#define RS2MIDI_RPC_SET_PITCH  3
#define RS2MIDI_RPC_KEY_OFF    4
#define RS2MIDI_RPC_GET_STATE    5
#define RS2MIDI_RPC_LOAD_SLOT    6
#define RS2MIDI_RPC_LOAD_ABS     7
#define RS2MIDI_RPC_NOTE_ON_ADDR 8
#define RS2MIDI_RPC_SET_MIX      9

#define RS2MIDI_PONG 0x52533250u

/*
 * Milestone 4A owns one tiny sample slot near the top of SPU2 RAM and plays
 * only on core 0. The frozen voice-only audsrv backend keeps using core 1 and
 * allocates its ADPCM heap upward from 0x5010.
 *
 * This is deliberately a tiny proof, not the final music allocator.
 */
#define RS2MIDI_PACK_SPU_BASE      0x00100000u
#define RS2MIDI_PACK_SPU_LIMIT     0x001e0000u
#define RS2MIDI_SPU_ADDR           0x001e0000u
#define RS2MIDI_SAMPLE_STRIDE      0x00000400u
#define RS2MIDI_SAMPLE_SLOTS       9u
#define RS2MIDI_PACK_PROBE_SLOT    8u
#define RS2MIDI_MAX_SAMPLE_BYTES   800u
#define RS2MIDI_RPC_HEADER_BYTES  64u
#define RS2MIDI_RPC_BUFFER_BYTES  1024u
#define RS2MIDI_DMA_CHANNEL       0
#define RS2MIDI_CORE              0
#define RS2MIDI_DEFAULT_VOICE     0

#define RS2MIDI_OK          0
#define RS2MIDI_ERR_ARGS   -1
#define RS2MIDI_ERR_STATE  -2
#define RS2MIDI_ERR_DMA    -3

IRX_ID(MODNAME, 1, 1);

static SifRpcDataQueue_t rs2midi_queue;
static SifRpcServerData_t rs2midi_server;
static u8 rs2midi_rpc_buffer[RS2MIDI_RPC_BUFFER_BYTES] __attribute__((aligned(64)));

static u32 rs2midi_sample_loaded_mask;

static u32 rs2midi_sample_addr(u32 slot)
{
    return RS2MIDI_SPU_ADDR + slot * RS2MIDI_SAMPLE_STRIDE;
}

static int rs2midi_valid_sample_slot(u32 slot)
{
    return slot < RS2MIDI_SAMPLE_SLOTS;
}

static int rs2midi_valid_pack_range(u32 addr, u32 size)
{
    if (size == 0 || (addr & 0x0f) != 0 || (size & 0x0f) != 0) {
        return 0;
    }
    if (addr < RS2MIDI_PACK_SPU_BASE || addr >= RS2MIDI_PACK_SPU_LIMIT) {
        return 0;
    }
    if (size > RS2MIDI_PACK_SPU_LIMIT - addr) {
        return 0;
    }
    return 1;
}

static int rs2midi_valid_pack_addr(u32 addr)
{
    return addr >= RS2MIDI_PACK_SPU_BASE &&
           addr < RS2MIDI_PACK_SPU_LIMIT &&
           (addr & 0x0f) == 0;
}

static int rs2midi_valid_voice(u32 voice)
{
    return voice < 24;
}

static u16 rs2midi_clamp_pitch(u32 pitch)
{
    if (pitch < 1) {
        return 1;
    }
    if (pitch > 0x3fff) {
        return 0x3fff;
    }
    return (u16)pitch;
}

static u16 rs2midi_clamp_volume(u32 volume)
{
    if (volume > 0x3fff) {
        return 0x3fff;
    }
    return (u16)volume;
}

static void rs2midi_start_voice(
    u32 voice,
    u16 pitch,
    u16 voll,
    u16 volr,
    u32 sample_addr)
{
    /*
     * Keep this register sequence identical to the hardware-proven slot
     * NOTE_ON path. Core 0 is rs2midi-owned; audsrv remains isolated on core 1.
     */
    sceSdSetParam(RS2MIDI_CORE | SD_PARAM_MVOLL, 0x3fff);
    sceSdSetParam(RS2MIDI_CORE | SD_PARAM_MVOLR, 0x3fff);
    sceSdSetParam(1 | SD_PARAM_AVOLL, 0x7fff);
    sceSdSetParam(1 | SD_PARAM_AVOLR, 0x7fff);

    sceSdSetParam(RS2MIDI_CORE | (voice << 1) | SD_VPARAM_VOLL, voll);
    sceSdSetParam(RS2MIDI_CORE | (voice << 1) | SD_VPARAM_VOLR, volr);
    sceSdSetParam(RS2MIDI_CORE | (voice << 1) | SD_VPARAM_PITCH, pitch);
    sceSdSetAddr(RS2MIDI_CORE | (voice << 1) | SD_VADDR_SSA, sample_addr);
    sceSdSetSwitch(RS2MIDI_CORE | SD_SWITCH_KON, 1u << voice);
}

static void *rs2midi_rpc_handler(int function, void *data, int size)
{
    u32 *words = (u32 *)data;
    u8 *payload = ((u8 *)data) + RS2MIDI_RPC_HEADER_BYTES;
    int status = RS2MIDI_OK;

    switch (function) {
    case RS2MIDI_RPC_PING:
        words[0] = RS2MIDI_PONG;
        return data;

    case RS2MIDI_RPC_LOAD:
    case RS2MIDI_RPC_LOAD_SLOT: {
        u32 slot = function == RS2MIDI_RPC_LOAD_SLOT ? words[0] : 0;
        u32 sample_size =
            function == RS2MIDI_RPC_LOAD_SLOT ? words[1] : words[0];
        int transfer_word = function == RS2MIDI_RPC_LOAD_SLOT ? 2 : 1;

        /*
         * Preserve the raw ROM LIBSD return value for the EE diagnostic.
         * The BIOS implementation is not required to use PS2SDK FreeSD's
         * "return byte count" convention, so any non-negative value is an
         * accepted transfer.
         */
        words[transfer_word] = (u32)-999;

        if (!rs2midi_valid_sample_slot(slot) ||
            sample_size == 0 ||
            sample_size > RS2MIDI_MAX_SAMPLE_BYTES ||
            (sample_size & 0x0f) != 0 ||
            size < (int)(RS2MIDI_RPC_HEADER_BYTES + sample_size)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }

        /*
         * Sample uploads happen only during backend bring-up. Silence core 0
         * while replacing SPU2 sample RAM; audsrv remains isolated on core 1.
         */
        sceSdSetSwitch(RS2MIDI_CORE | SD_SWITCH_KOFF, 0x00ffffffu);

        int transferred = sceSdVoiceTrans(
            RS2MIDI_DMA_CHANNEL,
            SD_TRANS_WRITE | SD_TRANS_MODE_DMA,
            payload,
            (u32 *)rs2midi_sample_addr(slot),
            sample_size);
        words[transfer_word] = (u32)transferred;

        if (transferred < 0) {
            status = RS2MIDI_ERR_DMA;
            break;
        }

        sceSdVoiceTransStatus(RS2MIDI_DMA_CHANNEL, 1);
        rs2midi_sample_loaded_mask |= 1u << slot;
        break;
    }

    case RS2MIDI_RPC_LOAD_ABS: {
        u32 sample_addr = words[0];
        u32 sample_size = words[1];
        words[2] = (u32)-999;

        if (sample_size > RS2MIDI_MAX_SAMPLE_BYTES ||
            !rs2midi_valid_pack_range(sample_addr, sample_size) ||
            size < (int)(RS2MIDI_RPC_HEADER_BYTES + sample_size)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }

        /*
         * Accurate packs are uploaded before their first event is scheduled.
         * Silence core 0 while replacing the dedicated high-SPU2 pack region.
         */
        sceSdSetSwitch(RS2MIDI_CORE | SD_SWITCH_KOFF, 0x00ffffffu);

        int transferred = sceSdVoiceTrans(
            RS2MIDI_DMA_CHANNEL,
            SD_TRANS_WRITE | SD_TRANS_MODE_DMA,
            payload,
            (u32 *)sample_addr,
            sample_size);
        words[2] = (u32)transferred;

        if (transferred < 0) {
            status = RS2MIDI_ERR_DMA;
            break;
        }

        sceSdVoiceTransStatus(RS2MIDI_DMA_CHANNEL, 1);
        break;
    }

    case RS2MIDI_RPC_NOTE_ON: {
        u32 voice = words[0];
        u32 sample_slot =
            size >= 20 ? words[4] : 0;
        if (!rs2midi_valid_voice(voice) ||
            !rs2midi_valid_sample_slot(sample_slot)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }
        if ((rs2midi_sample_loaded_mask & (1u << sample_slot)) == 0) {
            status = RS2MIDI_ERR_STATE;
            break;
        }

        u16 pitch = rs2midi_clamp_pitch(words[1]);
        u16 voll = rs2midi_clamp_volume(words[2]);
        u16 volr = rs2midi_clamp_volume(words[3]);

        /*
         * Do NOT KOFF immediately before KON here. LIBSD key transitions are
         * asynchronous; explicit KEY_OFF/LOAD already silence a reused voice.
         */
        rs2midi_start_voice(
            voice, pitch, voll, volr, rs2midi_sample_addr(sample_slot));
        break;
    }

    case RS2MIDI_RPC_NOTE_ON_ADDR: {
        u32 voice = words[0];
        u32 sample_addr = words[4];

        if (!rs2midi_valid_voice(voice) ||
            !rs2midi_valid_pack_addr(sample_addr)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }

        rs2midi_start_voice(
            voice,
            rs2midi_clamp_pitch(words[1]),
            rs2midi_clamp_volume(words[2]),
            rs2midi_clamp_volume(words[3]),
            sample_addr);
        break;
    }

    case RS2MIDI_RPC_SET_MIX: {
        u32 voice = words[0];
        if (!rs2midi_valid_voice(voice)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }

        sceSdSetParam(
            RS2MIDI_CORE | (voice << 1) | SD_VPARAM_VOLL,
            rs2midi_clamp_volume(words[1]));
        sceSdSetParam(
            RS2MIDI_CORE | (voice << 1) | SD_VPARAM_VOLR,
            rs2midi_clamp_volume(words[2]));
        break;
    }

    case RS2MIDI_RPC_SET_PITCH: {
        u32 voice = words[0];
        if (!rs2midi_valid_voice(voice)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }
        sceSdSetParam(RS2MIDI_CORE | (voice << 1) | SD_VPARAM_PITCH,
                      rs2midi_clamp_pitch(words[1]));
        break;
    }

    case RS2MIDI_RPC_KEY_OFF: {
        u32 voice = words[0];
        if (!rs2midi_valid_voice(voice)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }
        sceSdSetSwitch(RS2MIDI_CORE | SD_SWITCH_KOFF, 1u << voice);
        break;
    }

    case RS2MIDI_RPC_GET_STATE: {
        u32 voice = words[0];
        if (!rs2midi_valid_voice(voice)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }

        /*
         * Return enough live SPU2 state to distinguish RPC/DMA/register
         * success from an output-routing problem on real hardware.
         */
        words[1] = rs2midi_sample_loaded_mask != 0;
        words[2] = (u32)sceSdGetParam(
            RS2MIDI_CORE | (voice << 1) | SD_VPARAM_PITCH);
        words[3] = (u32)sceSdGetParam(
            RS2MIDI_CORE | (voice << 1) | SD_VPARAM_VOLL);
        words[4] = (u32)sceSdGetParam(
            RS2MIDI_CORE | (voice << 1) | SD_VPARAM_VOLR);
        words[5] = sceSdGetAddr(
            RS2MIDI_CORE | (voice << 1) | SD_VADDR_SSA);
        words[6] = sceSdGetSwitch(RS2MIDI_CORE | SD_SWITCH_ENDX);
        words[7] = (u32)sceSdGetParam(RS2MIDI_CORE | SD_PARAM_MVOLL);
        words[8] = (u32)sceSdGetParam(RS2MIDI_CORE | SD_PARAM_MVOLR);
        words[9] = (u32)sceSdGetParam(1 | SD_PARAM_AVOLL);
        words[10] = (u32)sceSdGetParam(1 | SD_PARAM_AVOLR);
        break;
    }

    default:
        status = RS2MIDI_ERR_ARGS;
        break;
    }

    words[0] = (u32)status;
    return data;
}

static void rs2midi_rpc_thread(void *arg)
{
    (void)arg;

    sceSifInitRpc(0);
    sceSifSetRpcQueue(&rs2midi_queue, GetThreadId());
    sceSifRegisterRpc(
        &rs2midi_server,
        RS2MIDI_RPC_ID,
        (SifRpcFunc_t)rs2midi_rpc_handler,
        rs2midi_rpc_buffer,
        NULL,
        NULL,
        &rs2midi_queue);
    sceSifRpcLoop(&rs2midi_queue);
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    iop_thread_t thread;
    thread.attr = TH_C;
    thread.thread = rs2midi_rpc_thread;
    thread.priority = 80;
    thread.stacksize = 0x1000;
    thread.option = 0;

    int thread_id = CreateThread(&thread);
    if (thread_id <= 0) {
        return MODULE_NO_RESIDENT_END;
    }
    if (StartThread(thread_id, NULL) != 0) {
        return MODULE_NO_RESIDENT_END;
    }

    return MODULE_RESIDENT_END;
}
