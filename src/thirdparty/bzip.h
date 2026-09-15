#ifndef _H_BZIP
#define _H_BZIP

#include <limits.h>
#if !defined(__wasm) || defined(__EMSCRIPTEN__)
#include <setjmp.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NXDK
#include <fileapi.h>
#elif defined(_WIN32)
#include <io.h>
#else
#if defined(__wasm) && !defined(__EMSCRIPTEN__)
#include <js/syscalls.h>
#else
#include <unistd.h>
#endif
#endif

/* Constants for huffman coding */
#define MAX_GROUPS 6
#define GROUP_SIZE 50       /* 64 would have been more efficient */
#define MAX_HUFCODE_BITS 20 /* Longest huffman code allowed */
#define MAX_SYMBOLS 258     /* 256 literals + RUNA + RUNB */
#define SYMBOL_RUNA 0
#define SYMBOL_RUNB 1

/* Status return values */
#define RETVAL_OK 0
#define RETVAL_LAST_BLOCK (-1)
#define RETVAL_NOT_BZIP_DATA (-2)
#define RETVAL_UNEXPECTED_INPUT_EOF (-3)
#define RETVAL_UNEXPECTED_OUTPUT_EOF (-4)
#define RETVAL_DATA_ERROR (-5)
#define RETVAL_OUT_OF_MEMORY (-6)
#define RETVAL_OBSOLETE_INPUT (-7)

/* Other housekeeping constants */
#define IOBUF_SIZE 4096

/* This is what we know about each huffman coding group */
struct group_data {
    /* We have an extra slot at the end of limit[] for a sentinal value. */
    uint32_t limit[MAX_HUFCODE_BITS + 1], base[MAX_HUFCODE_BITS],
        permute[MAX_SYMBOLS];

    uint32_t minLen, maxLen;
};

/* Structure holding all the housekeeping data, including IO buffers and
   memory that persists between calls to bunzip */
typedef struct {
    /* State for interrupting output loop */
    int writeCopies, writePos, writeRunCountdown, writeCount, writeCurrent;

    /* I/O tracking data (file handles, buffers, positions, etc.) */
    int in_fd, out_fd, inbufCount, inbufPos;
    uint8_t *inbuf;
    uint32_t inbufBitCount, inbufBits;

    /* The CRC values stored in the block header and calculated from the data */
    uint32_t crc32Table[256], headerCRC, totalCRC, writeCRC;

    /* Intermediate buffer and its size (in bytes) */
    uint32_t *dbuf, dbufSize;

#ifdef __PS2__
    // Opaque `Client *` (see bzip_decompress()'s own doc comment) carried through to
    // get_next_block(), the only place besides bzip_decompress() itself that needs it for
    // real-hardware diagnostic checkpoints - NULL unless the caller passed one in.
    void *diagnostic_client;
#endif

    /* These things are a bit too big to go on the stack */
    uint8_t selectors[32768];             /* nSelectors=15 bits */
    struct group_data groups[MAX_GROUPS]; /* huffman coding tables */

    /* For I/O error handling */
#if !defined(__wasm) || defined(__EMSCRIPTEN__)
    jmp_buf jmpbuf;
#endif
} bunzip_data;

extern const char BZIP_HEADER[];
extern const char *bunzip_errors[];

#ifdef __PS2__
// `diagnostic_client` is an opaque `Client *` (typed void* here so this otherwise
// project-independent vendored file doesn't need to know about client.h/the Client struct) - pass
// NULL for calls that don't need real-hardware diagnostics (this file's other callers: jagfile.c's
// startup archive decompression, midi.c) to get plain, on-screen-silent log-only behavior; pass the
// real Client* only from the one call site actually under investigation (client_build_scene()'s
// land/loc decode) to also get readable on-screen checkpoint text via ps2_scene_checkpoint(). This
// keeps the diagnostics scoped to the specific call being debugged instead of adding visible noise
// (a stray progress-bar flicker was seen on the ALREADY-WORKING title-screen archive loads, which
// share this same function, before this was scoped) to every other bzip_decompress() call in the
// program.
// 2026-09-14, later session: `dst_capacity` added. Every caller already separately computes/trusts a
// DECLARED decompressed-size value (a packet header field, a jagfile index entry) to size `file_data`
// before calling this - but bzip_decompress() itself never saw that number, so nothing actually
// enforced the real bzip stream's output matched it. A stream that genuinely decompresses to MORE
// bytes than its own declared-length header claimed (corrupt/truncated transmission, or a real
// encoder/decoder edge case) would previously just keep writing past `file_data`'s real allocation
// with zero warning - exactly the still-open gap flagged in-line at the land/loc call site below, and
// the leading suspect for a real-hardware hang landing right at the next free() after a dense loc
// square's decode. Every current caller already has the right value sitting at the call site (the
// buffer it just allocated for `file_data`), so this is a pure hardening addition, not a caller
// behavior change for any well-formed stream.
void bzip_decompress(int8_t *file_data, int8_t *archive_data, int archive_size, int offset, void *diagnostic_client, int dst_capacity);
#else
void bzip_decompress(int8_t *file_data, int8_t *archive_data, int archive_size,
                     int offset);
#endif

#endif
