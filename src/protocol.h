#pragma once

typedef struct {
    int SERVERPROT_SIZES[257];
} Protocol;

// Revision 254 server->client opcode length table (opcode id -> length).
// 0 = fixed empty, N = fixed size in bytes, -1 = 1-byte length prefix follows, -2 = 2-byte length prefix follows.
// Source: 2004sp-progressive engine/src/network/game/server/ServerGameProt.ts, cross-checked against
// 2004sp-client's ServerProt.ts. Zone sub-opcodes (LOC_*/OBJ_*/MAP_*/P_LOCMERGE) share this same opcode
// space and are dispatched through readZonePacket() in entry/client.c; their sizes were verified directly
// against Client3's own existing (rev225) readZonePacket() field-read code, which is unchanged by revision.
Protocol _Protocol = {
    .SERVERPROT_SIZES = {
        [0] = 6,    // CAM_LOOKAT
        [2] = 0,    // P_NAMEDIALOG
        [3] = 4,    // IF_SETNPCHEAD
        [5] = 0,    // P_COUNTDIALOG
        [8] = 7,    // OBJ_REVEAL (zone sub-opcode)
        [14] = 4,   // IF_SETSCROLLPOS
        [21] = 0,   // LOGOUT
        [24] = 3,   // CHAT_FILTER_SETTINGS
        [25] = 5,   // SYNTH_SOUND
        [27] = 6,   // IF_SETPOSITION
        [28] = -2,  // UPDATE_INV_FULL
        [29] = 0,   // FINISH_TRACKING
        [30] = 4,   // LOC_ANIM (zone sub-opcode)
        [37] = 15,  // MAP_PROJANIM (zone sub-opcode)
        [38] = 4,   // IF_SETCOLOUR
        [41] = -2,  // IF_SETTEXT
        [55] = 6,   // CAM_MOVETO
        [58] = 1,   // TUT_FLASH
        [60] = -1,  // MESSAGE_PRIVATE
        [61] = -2,  // UPDATE_ZONE_PARTIAL_ENCLOSED
        [63] = -2,  // UPDATE_IGNORELIST
        [64] = 6,   // HINT_ARROW
        [70] = 4,   // LOC_ADD_CHANGE (zone sub-opcode)
        [73] = -1,  // MESSAGE_GAME
        [75] = 1,   // SET_MULTIWAY
        [85] = 2,   // IF_OPENOVERLAY
        [87] = -2,  // PLAYER_INFO
        [88] = 2,   // LOC_DEL (zone sub-opcode)
        [91] = 3,   // IF_SETTAB
        [94] = 1,   // UPDATE_RUNENERGY
        [95] = 4,   // IF_SETANIM
        [98] = 7,   // OBJ_COUNT (zone sub-opcode)
        [108] = 0,  // UNSET_MAP_FLAG
        [111] = 9,  // UPDATE_FRIENDLIST
        [114] = 6,  // MAP_ANIM (zone sub-opcode)
        [115] = 3,  // OBJ_DEL (zone sub-opcode)
        [120] = 5,  // OBJ_ADD (zone sub-opcode)
        [123] = -2, // NPC_INFO
        [136] = 6,  // UPDATE_STAT
        [138] = 1,  // IF_SETTAB_ACTIVE
        [140] = 0,  // RESET_CLIENT_VARCACHE
        [141] = 2,  // IF_OPENCHAT
        [143] = 2,  // UPDATE_REBOOT_TIMER
        [146] = 10, // LAST_LOGIN_INFO
        [159] = 2,  // UPDATE_ZONE_FULL_FOLLOWS
        [161] = 2,  // IF_SETPLAYERHEAD
        [163] = 2,  // MIDI_SONG
        [164] = 2,  // UPDATE_RUNWEIGHT
        [167] = 0,  // CAM_RESET
        [168] = 2,  // UPDATE_INV_STOP_TRANSMIT
        [170] = -2, // UPDATE_INV_PARTIAL
        [173] = 2,  // UPDATE_ZONE_PARTIAL_FOLLOWS
        [174] = 0,  // IF_CLOSE
        [186] = 3,  // VARP_SMALL
        [187] = 2,  // IF_OPENSIDE
        [196] = 6,  // VARP_LARGE
        [197] = 2,  // IF_OPENMAIN
        [203] = 0,  // RESET_ANIMS
        [204] = -1, // SET_PLAYER_OP
        [209] = 4,  // REBUILD_NORMAL
        [211] = 4,  // IF_SETMODEL
        [213] = 3,  // UPDATE_PID
        [218] = 14, // P_LOCMERGE (zone sub-opcode)
        [222] = 6,  // IF_SETOBJECT
        [225] = 4,  // CAM_SHAKE
        [227] = 3,  // IF_SETHIDE
        [239] = 2,  // TUT_OPEN
        [242] = 4,  // MIDI_JINGLE
        [249] = 4,  // IF_OPENMAIN_SIDE
        [251] = 0,  // ENABLE_TRACKING
        [255] = 1,  // FRIENDLIST_LOADED
    },
};
