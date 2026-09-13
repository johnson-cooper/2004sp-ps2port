#pragma once

#include <stdbool.h>
#include <stdint.h>

// rev254 delivers models/anims/midis/maps individually through a single "ondemand.zip" bundle
// instead of the old monolithic per-category .jag archives. Each entry is named "<archive+1>.<id>"
// (archive 0=model, 1=anim, 2=midi, 3=map) and its payload is gzip-compressed with a trailing
// 2-byte version, on top of the outer zip's own (usually stored/uncompressed) entry compression -
// two independent compression layers to unwrap before reaching the raw data the rest of Client3
// already knows how to decode (see model_from_id()'s on-demand path in model.c).
bool ondemand_load(int8_t *zip_data, int zip_size);
// Opens ondemand.zip directly off disk (stdio fopen/fseek/fread under the hood, via miniz's own
// mz_zip_reader_init_file) instead of reading the whole ~6MB file into a permanently-resident
// heap buffer. Only the zip's central directory (entry names/offsets, tens of KB, not the entry
// payloads) stays resident - individual model/anim/map entries are read from disk on demand, the
// same moment they'd have been decompressed out of the in-memory buffer anyway. See PS2 call site
// in entry/client.c for why this matters on a fixed-32MB target specifically.
bool ondemand_load_file(const char *path);
bool ondemand_is_loaded(void);

// Returns a malloc'd buffer of the fully-decompressed entry for the given archive/id, or NULL if
// ondemand isn't loaded, the entry doesn't exist, or either compression layer fails. Caller frees.
int8_t *ondemand_get(int archive, int id, int *out_size);

// Enumeration by raw zip index, for archives (like "anim") that must be bulk-loaded up front rather
// than looked up by a single known id - see the reference client's own bulk anim-archive fetch.
int ondemand_file_count(void);
bool ondemand_get_entry_info(int index, int *out_archive, int *out_id);
int8_t *ondemand_get_by_index(int index, int *out_size);

void ondemand_free_global(void);
