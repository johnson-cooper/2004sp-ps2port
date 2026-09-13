#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct ClientStream ClientStream;

struct ClientStream {
    int socket;
    bool closed;
    int8_t buf[5000];
    int bufLen;
    int bufPos;
};

bool clientstream_init(void);
ClientStream *clientstream_new(void);
ClientStream *clientstream_opensocket(int port);
void clientstream_close(ClientStream *stream);
int clientstream_available(ClientStream *stream, int length);
int clientstream_read_byte(ClientStream *stream);
int clientstream_read_bytes(ClientStream *stream, int8_t *dst, int off, int len);
int clientstream_write(ClientStream *stream, const int8_t *src, int len, int off);
const char *dnslookup(const char *hostname);
#ifdef __PS2__
// PHASE 4 audit instrumentation: cumulative real time spent inside clientstream_read_bytes()'s
// recv()-empty retry loop - to check whether "update" phase time (gameshell.c's PS2 PERF report)
// is actually game-logic CPU work or just network wait disguised as it, since the socket is
// non-blocking and this loop is the only thing standing between a slow/laggy link and the rest of
// client_update_game(). Reset once per PS2 PERF window, same as the other perf counters.
int64_t clientstream_net_wait_ms(void);
void clientstream_net_wait_reset(void);
// Cumulative real time spent inside clientstream_available()'s single recv() call - separate from
// the retry-loop wait above since this measures per-syscall overhead even when it doesn't have to
// wait for data. See the comment at its definition in clientstream.c for why this matters.
int64_t clientstream_net_call_ms(void);
void clientstream_net_call_reset(void);
#endif
