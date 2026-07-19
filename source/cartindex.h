#pragma once
#include "gb_operator.h"

/* One entry returned by cartindex_lookup. rom_basename is just the filename
 * (no directory), e.g. "POKEMONRED_.gb". Full path is built by the caller. */
typedef struct {
    char rom_basename[64];
} CartIndexEntry;

/* Look up previously-dumped ROMs by device response fingerprint.
 * Returns number of matches written to out[] (capped at max):
 *   0 = not in index (cart never dumped)
 *   1 = unique match
 *  >1 = multiple ROMs share this fingerprint (hardware-identical carts)
 * The caller is responsible for verifying that each returned file still exists. */
int cartindex_lookup(const CartInfo *info, CartIndexEntry *out, int max);

/* Record a successfully-dumped ROM in the index.
 * rom_basename is the bare filename without directory (e.g. "POKEMONRED_.gb").
 * Safe to call repeatedly for the same cart; duplicate entries are suppressed. */
void cartindex_update(const CartInfo *info, const char *rom_basename);
