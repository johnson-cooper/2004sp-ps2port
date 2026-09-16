/*
 * PS2 networking wrapper.
 *
 * The historical implementation is included verbatim below under legacy names
 * for the three receive-buffer entry points.  The PS2 overrides fix two bugs
 * whose manifestation depends on how TCP happens to fragment/coalesce bytes:
 *  - clientstream_available() could recv past the 5000-byte buffer tail.
 *  - clientstream_read_bytes() failed to advance dst+off after consuming
 *    already-buffered bytes, so a follow-up recv overwrote the packet prefix.
 *
 * Keep the rest of clientstream_impl.inc unchanged.  Also force the connect
 * completion wait through lwIP's select on PS2; this SDK does not expose all
 * POSIX socket I/O names as libc aliases.
 */
#ifdef __PS2__
#define select lwip_select
#define clientstream_available clientstream_available_legacy
#define clientstream_read_byte clientstream_read_byte_legacy
#define clientstream_read_bytes clientstream_read_bytes_legacy
#endif

#include "clientstream_impl.inc"

#ifdef __PS2__
#undef clientstream_available
#undef clientstream_read_byte
#undef clientstream_read_bytes
#undef select

int clientstream_available(ClientStream *stream, int len) {
    if (!stream || stream->closed) {
        return 0;
    }
    if (len <= 0) {
        return 1;
    }
    if (len > (int)sizeof(stream->buf)) {
        rs2_error("PS2NET requested buffered packet larger than receive buffer: %d > %d\n",
                  len, (int)sizeof(stream->buf));
        stream->closed = true;
        return 0;
    }
    if (stream->bufLen >= len) {
        return 1;
    }

    /*
     * bufPos advances as callers consume packet prefixes.  Compact before a
     * new recv so the full unused capacity is contiguous at the end.  The old
     * code instead increased `want` to the requested deficit even when the
     * physical tail was smaller, which could write beyond buf[5000].
     */
    if (stream->bufPos > 0 && stream->bufLen > 0) {
        memmove(stream->buf, stream->buf + stream->bufPos, stream->bufLen);
        stream->bufPos = 0;
    } else if (stream->bufLen == 0) {
        stream->bufPos = 0;
    }

    int tail = (int)sizeof(stream->buf) - stream->bufLen;
    if (tail <= 0) {
        return 0;
    }

    int64_t select_t0 = rs2_now();
    int ready = ps2net_poll_readable(stream->socket);
    int64_t select_ms = rs2_now() - select_t0;
    if (ready <= 0) {
        if (ready < 0) {
            rs2_error("PS2NET select() error on fd=%d: errno=%d (%s)\n",
                      stream->socket, errno, strerror(errno));
        }
        return 0;
    }

    errno = 0;
    int64_t recv_t0 = rs2_now();
    int bytes = lwip_recv(stream->socket,
                          (char *)stream->buf + stream->bufLen,
                          tail,
                          MSG_DONTWAIT);
    int64_t recv_ms = rs2_now() - recv_t0;
    int recv_errno = errno;
    _net_call_ms += select_ms + recv_ms;

    if (bytes > 0) {
        stream->bufLen += bytes;
    } else if (bytes == 0) {
        /* Readable + zero-byte TCP receive is an orderly peer shutdown. */
        stream->closed = true;
        return 0;
    } else if (recv_errno != EAGAIN && recv_errno != EWOULDBLOCK) {
        rs2_error("PS2NET recv() error on fd=%d: errno=%d (%s)\n",
                  stream->socket, recv_errno, strerror(recv_errno));
        stream->closed = true;
        return 0;
    }

    return stream->bufLen >= len;
}

int clientstream_read_bytes(ClientStream *stream, int8_t *dst, int off, int len) {
    if (!stream || stream->closed || !dst || off < 0 || len < 0) {
        return -1;
    }

    if (stream->bufLen > 0 && len > 0) {
        int copy_length = len < stream->bufLen ? len : stream->bufLen;
        memcpy(dst + off, stream->buf + stream->bufPos, copy_length);

        /* Critical: advance BOTH source state and destination offset. */
        off += copy_length;
        len -= copy_length;
        stream->bufLen -= copy_length;
        if (stream->bufLen == 0) {
            stream->bufPos = 0;
        } else {
            stream->bufPos += copy_length;
        }
    }

    int read_duration = 0;
    int64_t wait_t0 = rs2_now();
    while (len > 0) {
        errno = 0;
        int bytes = lwip_recv(stream->socket, (char *)dst + off, len, MSG_DONTWAIT);
        int recv_errno = errno;

        if (bytes > 0) {
            off += bytes;
            len -= bytes;
            continue;
        }
        if (bytes == 0) {
            stream->closed = true;
            _net_wait_ms += rs2_now() - wait_t0;
            return -1;
        }
        if (recv_errno != EAGAIN && recv_errno != EWOULDBLOCK) {
            rs2_error("PS2NET recv() error on fd=%d: errno=%d (%s)\n",
                      stream->socket, recv_errno, strerror(recv_errno));
            clientstream_close(stream);
            _net_wait_ms += rs2_now() - wait_t0;
            return -1;
        }

        if (++read_duration >= 5000) {
            clientstream_close(stream);
            _net_wait_ms += rs2_now() - wait_t0;
            return -1;
        }
        rs2_sleep(1);
    }

    _net_wait_ms += rs2_now() - wait_t0;
    return 0;
}

int clientstream_read_byte(ClientStream *stream) {
    if (!stream || stream->closed) {
        return -1;
    }
    int8_t byte = 0;
    if (clientstream_read_bytes(stream, &byte, 0, 1) == 0) {
        return byte & 0xff;
    }
    return -1;
}
#endif
