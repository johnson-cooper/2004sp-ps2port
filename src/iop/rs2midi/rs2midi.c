#include <irx_imports.h>

#define MODNAME "rs2midi"
#define RS2MIDI_RPC_ID 0x5253324d
#define RS2MIDI_RPC_PING 0
#define RS2MIDI_PONG 0x52533250u

IRX_ID(MODNAME, 1, 0);

static SifRpcDataQueue_t rs2midi_queue;
static SifRpcServerData_t rs2midi_server;
static u32 rs2midi_rpc_buffer[16] __attribute__((aligned(16)));

static void *rs2midi_rpc_handler(int function, void *data, int size) {
    (void)size;

    u32 *words = (u32 *)data;
    if (function == RS2MIDI_RPC_PING) {
        words[0] = RS2MIDI_PONG;
    } else {
        words[0] = 0;
    }
    return data;
}

static void rs2midi_rpc_thread(void *arg) {
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

int _start(int argc, char *argv[]) {
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
