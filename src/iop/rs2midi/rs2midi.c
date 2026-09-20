#include <irx_imports.h>
#include <libsd.h>

#define MODNAME "rs2midi"
#define RS2MIDI_RPC_ID 0x5253324d

#define RS2MIDI_RPC_PING       0
#define RS2MIDI_RPC_LOAD       1
#define RS2MIDI_RPC_NOTE_ON    2
#define RS2MIDI_RPC_SET_PITCH  3
#define RS2MIDI_RPC_KEY_OFF    4
#define RS2MIDI_RPC_GET_STATE  5

#define RS2MIDI_PONG 0x52533250u

/*
 * Milestone 4A owns one tiny sample slot near the top of SPU2 RAM and plays
 * only on core 0. The frozen voice-only audsrv backend keeps using core 1 and
 * allocates its ADPCM heap upward from 0x5010.
 *
 * This is deliberately a tiny proof, not the final music allocator.
 */
#define RS2MIDI_SPU_ADDR          0x001e0000u
#define RS2MIDI_MAX_SAMPLE_BYTES  800u
#define RS2MIDI_RPC_HEADER_BYTES  64u
#define RS2MIDI_RPC_BUFFER_BYTES  1024u
#define RS2MIDI_DMA_CHANNEL       1
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

static int rs2midi_sample_loaded;

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

static void *rs2midi_rpc_handler(int function, void *data, int size)
{
    u32 *words = (u32 *)data;
    u8 *payload = ((u8 *)data) + RS2MIDI_RPC_HEADER_BYTES;
    int status = RS2MIDI_OK;

    switch (function) {
    case RS2MIDI_RPC_PING:
        words[0] = RS2MIDI_PONG;
        return data;

    case RS2MIDI_RPC_LOAD: {
        u32 sample_size = words[0];

        if (sample_size == 0 ||
            sample_size > RS2MIDI_MAX_SAMPLE_BYTES ||
            (sample_size & 0x0f) != 0 ||
            size < (int)(RS2MIDI_RPC_HEADER_BYTES + sample_size)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }

        /* Silence our test voice before replacing its sample data. */
        sceSdSetSwitch(RS2MIDI_CORE | SD_SWITCH_KOFF,
                       1u << RS2MIDI_DEFAULT_VOICE);

        int transferred = sceSdVoiceTrans(
            RS2MIDI_DMA_CHANNEL,
            SD_TRANS_WRITE | SD_TRANS_MODE_DMA,
            payload,
            (u32 *)RS2MIDI_SPU_ADDR,
            sample_size);

        if (transferred != (int)sample_size) {
            status = RS2MIDI_ERR_DMA;
            break;
        }

        sceSdVoiceTransStatus(RS2MIDI_DMA_CHANNEL, 1);
        rs2midi_sample_loaded = 1;
        break;
    }

    case RS2MIDI_RPC_NOTE_ON: {
        u32 voice = words[0];
        if (!rs2midi_sample_loaded) {
            status = RS2MIDI_ERR_STATE;
            break;
        }
        if (!rs2midi_valid_voice(voice)) {
            status = RS2MIDI_ERR_ARGS;
            break;
        }

        u16 pitch = rs2midi_clamp_pitch(words[1]);
        u16 voll = rs2midi_clamp_volume(words[2]);
        u16 volr = rs2midi_clamp_volume(words[3]);

        /*
         * LIBSD cold-init leaves both core master volumes at zero. audsrv's
         * frozen voice-only init enables only core 1, so enable core 0 here
         * without touching audsrv's core-1 voices.
         */
        sceSdSetParam(RS2MIDI_CORE | SD_PARAM_MVOLL, 0x3fff);
        sceSdSetParam(RS2MIDI_CORE | SD_PARAM_MVOLR, 0x3fff);

        /*
         * Core 0 reaches the final SPU2 output through core 1's external
         * input path. LIBSD cold-init normally leaves these at 0x7fff, but
         * set them explicitly so the companion does not depend on a prior
         * module preserving that routing state.
         */
        sceSdSetParam(1 | SD_PARAM_AVOLL, 0x7fff);
        sceSdSetParam(1 | SD_PARAM_AVOLR, 0x7fff);

        /*
         * Do NOT KOFF immediately before KON here. LIBSD's own reset path
         * documents that key transitions are asynchronous; an immediate
         * KOFF->KON can race on real hardware. LOAD/explicit KEY_OFF already
         * silence the voice before a later note-on.
         */
        sceSdSetParam(RS2MIDI_CORE | (voice << 1) | SD_VPARAM_VOLL, voll);
        sceSdSetParam(RS2MIDI_CORE | (voice << 1) | SD_VPARAM_VOLR, volr);
        sceSdSetParam(RS2MIDI_CORE | (voice << 1) | SD_VPARAM_PITCH, pitch);
        sceSdSetAddr(RS2MIDI_CORE | (voice << 1) | SD_VADDR_SSA,
                     RS2MIDI_SPU_ADDR);
        sceSdSetSwitch(RS2MIDI_CORE | SD_SWITCH_KON, 1u << voice);
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
        words[1] = (u32)rs2midi_sample_loaded;
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
