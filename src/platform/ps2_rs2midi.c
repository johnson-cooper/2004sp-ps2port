#ifdef __PS2__

#ifdef client
#undef client
#endif

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <delaythread.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../platform.h"
#include "ps2_rs2midi.h"

#define RS2MIDI_RPC_ID 0x5253324d
#define RS2MIDI_RPC_PING 0
#define RS2MIDI_PONG 0x52533250u

extern unsigned char rs2midi_irx[];
extern unsigned int size_rs2midi_irx;

static SifRpcClientData_t rs2midi_client;
static uint32_t rs2midi_send[16] __attribute__((aligned(64)));
static uint32_t rs2midi_recv[16] __attribute__((aligned(64)));

bool ps2_rs2midi_ping_test(void) {
    int modres = -1;
    int load_ret = SifExecModuleBuffer(
        rs2midi_irx, size_rs2midi_irx, 0, NULL, &modres);
    rs2_log("audio: rs2midi load ret=%d modres=%d size=%u\n",
            load_ret, modres, size_rs2midi_irx);
    if (load_ret < 0 || modres < 0) {
        return false;
    }

    memset(&rs2midi_client, 0, sizeof(rs2midi_client));
    int bind_ret = -1;
    for (int attempt = 0; attempt < 64; attempt++) {
        bind_ret = sceSifBindRpc(&rs2midi_client, RS2MIDI_RPC_ID, 0);
        if (bind_ret < 0) {
            rs2_log("audio: rs2midi bind failed rc=%d attempt=%d\n",
                    bind_ret, attempt);
            return false;
        }
        if (rs2midi_client.server != NULL) {
            break;
        }
        DelayThread(1000);
    }

    if (rs2midi_client.server == NULL) {
        rs2_log("audio: rs2midi RPC server did not bind\n");
        return false;
    }

    memset(rs2midi_send, 0, sizeof(rs2midi_send));
    memset(rs2midi_recv, 0, sizeof(rs2midi_recv));
    rs2midi_send[0] = 0x12345678u;

    int call_ret = sceSifCallRpc(
        &rs2midi_client, RS2MIDI_RPC_PING, 0,
        rs2midi_send, sizeof(uint32_t),
        rs2midi_recv, sizeof(uint32_t),
        NULL, NULL);

    rs2_log("audio: rs2midi ping call=%d pong=0x%08x\n",
            call_ret, (unsigned int)rs2midi_recv[0]);

    return call_ret >= 0 && rs2midi_recv[0] == RS2MIDI_PONG;
}

#endif
