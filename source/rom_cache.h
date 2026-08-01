#pragma once
#include <stdio.h>
#include "gb_operator.h"

// Checks whether a cached ROM file exists for this cart and its size is correct.
// Fills path_out with the file path. Returns 1 if valid cache hit, 0 otherwise.
int rom_cache_exists(const CartInfo *info, char *path_out, int path_size);

// Dumps the ROM from the cart via GB Operator and saves it to SD/USB.
// Fills path_out with the saved file path on success. Returns 0 on success.
int rom_cache_dump(GBOperatorHandle handle, const CartInfo *info,
                   char *path_out, int path_size);

// Streams up to total_size bytes from the device into f (already open for
// writing; caller owns closing it), DUMP_CHUNK_SIZE bytes at a time, with
// the same PAD X+Y abort check rom_cache_dump() has always had.
// *out_written is always set to however many bytes were actually streamed
// and fwritten, even on failure/abort — every caller can rely on this being
// a clean, uncorrupted prefix of real data (gbop_dump_rom either fully
// succeeds for a chunk or fails; there is no partial-chunk success).
// Returns 0 = fully clean (*out_written == total_size), -1 = read/write
// failure, -2 = user aborted with X+Y.
int rom_cache_stream_chunks(GBOperatorHandle handle, const CartInfo *info,
                             FILE *f, uint32_t total_size, uint32_t *out_written);
