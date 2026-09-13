#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ondemand.h"
#include "platform.h"
#include "thirdparty/miniz.h"

typedef struct {
    bool loaded;
    mz_zip_archive zip;
} OnDemandData;

static OnDemandData _OnDemand = {0};

bool ondemand_load(int8_t *zip_data, int zip_size) {
    memset(&_OnDemand.zip, 0, sizeof(_OnDemand.zip));
    if (!mz_zip_reader_init_mem(&_OnDemand.zip, zip_data, (size_t)zip_size, 0)) {
        rs2_error("ondemand: failed to open ondemand.zip (%s)\n", mz_zip_get_error_string(mz_zip_get_last_error(&_OnDemand.zip)));
        return false;
    }
    _OnDemand.loaded = true;
    return true;
}

bool ondemand_load_file(const char *path) {
    memset(&_OnDemand.zip, 0, sizeof(_OnDemand.zip));
    if (!mz_zip_reader_init_file(&_OnDemand.zip, path, 0)) {
        rs2_error("ondemand: failed to open ondemand.zip file (%s)\n", mz_zip_get_error_string(mz_zip_get_last_error(&_OnDemand.zip)));
        return false;
    }
    _OnDemand.loaded = true;
    return true;
}

bool ondemand_is_loaded(void) {
    return _OnDemand.loaded;
}

// Parses an RFC 1952 gzip header and returns the byte offset where the raw deflate stream begins,
// or -1 if this isn't a valid gzip stream. fflate's gzipSync (used server-side to build
// ondemand.zip's per-entry payloads) writes a minimal 10-byte header in practice, but this handles
// the optional FEXTRA/FNAME/FCOMMENT/FHCRC fields correctly rather than assuming that.
static int gzip_header_length(const uint8_t *data, int size) {
    if (size < 10 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8) {
        return -1;
    }
    int pos = 10;
    uint8_t flg = data[3];
    if (flg & 0x04) { // FEXTRA
        if (pos + 2 > size) {
            return -1;
        }
        int xlen = data[pos] | (data[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) { // FNAME
        while (pos < size && data[pos] != 0) {
            pos++;
        }
        pos++;
    }
    if (flg & 0x10) { // FCOMMENT
        while (pos < size && data[pos] != 0) {
            pos++;
        }
        pos++;
    }
    if (flg & 0x02) { // FHCRC
        pos += 2;
    }
    if (pos > size) {
        return -1;
    }
    return pos;
}

// Shared second-layer unwrap: takes ownership of (frees) `outer` regardless of outcome. Every
// ondemand.zip entry, across every archive category, uses this same gzip(payload)+2-byte-version
// wrapping (confirmed against the reference client) - only the outer zip entry's own compression
// (handled by miniz before this is called) differs per-entry.
static int8_t *unwrap_entry(void *outer, size_t outer_size, const char *name_for_log, int *out_size) {
    if (!outer) {
        return NULL;
    }
    if (outer_size < 2) {
        free(outer);
        return NULL;
    }
    int gz_size = (int)(outer_size - 2);
    int hdr_len = gzip_header_length((const uint8_t *)outer, gz_size);
    if (hdr_len < 0 || gz_size - hdr_len < 8) {
        rs2_error("ondemand: bad gzip payload for %s\n", name_for_log);
        free(outer);
        return NULL;
    }
    int deflate_len = gz_size - hdr_len - 8; // trailing 8 bytes = gzip CRC32 + ISIZE, unused here

    size_t final_size = 0;
    void *final_data = tinfl_decompress_mem_to_heap((const uint8_t *)outer + hdr_len, (size_t)deflate_len, &final_size, 0);
    free(outer);
    if (!final_data) {
        rs2_error("ondemand: inflate failed for %s\n", name_for_log);
        return NULL;
    }

    *out_size = (int)final_size;
    return (int8_t *)final_data;
}

int8_t *ondemand_get(int archive, int id, int *out_size) {
    if (!_OnDemand.loaded) {
        return NULL;
    }

    char name[32];
    snprintf(name, sizeof(name), "%d.%d", archive + 1, id);

    size_t outer_size = 0;
    void *outer = mz_zip_reader_extract_file_to_heap(&_OnDemand.zip, name, &outer_size, 0);
    return unwrap_entry(outer, outer_size, name, out_size);
}

int ondemand_file_count(void) {
    if (!_OnDemand.loaded) {
        return 0;
    }
    return (int)mz_zip_reader_get_num_files(&_OnDemand.zip);
}

bool ondemand_get_entry_info(int index, int *out_archive, int *out_id) {
    if (!_OnDemand.loaded) {
        return false;
    }
    char name[64];
    if (!mz_zip_reader_get_filename(&_OnDemand.zip, (mz_uint)index, name, sizeof(name))) {
        return false;
    }
    int archive1based;
    int id;
    if (sscanf(name, "%d.%d", &archive1based, &id) != 2) {
        return false;
    }
    *out_archive = archive1based - 1;
    *out_id = id;
    return true;
}

int8_t *ondemand_get_by_index(int index, int *out_size) {
    if (!_OnDemand.loaded) {
        return NULL;
    }
    size_t outer_size = 0;
    void *outer = mz_zip_reader_extract_to_heap(&_OnDemand.zip, (mz_uint)index, &outer_size, 0);
    char label[32];
    snprintf(label, sizeof(label), "index %d", index);
    return unwrap_entry(outer, outer_size, label, out_size);
}

void ondemand_free_global(void) {
    if (_OnDemand.loaded) {
        mz_zip_reader_end(&_OnDemand.zip);
        _OnDemand.loaded = false;
    }
}
