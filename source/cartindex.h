#pragma once
#include "gb_operator.h"

/* One entry returned by cartindex_lookup. rom_basename is just the filename
 * (no directory), e.g. "POKEMONRED_.gb" — may not exist yet on SD (see
 * below). Full path is built by the caller.
 *
 * identity_known: 1 if type/game_code/title/rom_size_kb/ram_size_kb below
 * were actually recorded for this entry (new-format line); 0 for an older-
 * format "fingerprint|rom_basename"-only line (pre-2026-07-31), in which
 * case the caller must fall back to reading rom_basename's file to learn
 * anything beyond "this fingerprint has been seen before." */
typedef struct {
    char     rom_basename[64];
    int      identity_known;
    CartType type;
    char     type_str[8];
    char     game_code[5];
    char     title[17];
    uint32_t rom_size_kb;
    uint32_t ram_size_kb;
} CartIndexEntry;

/* Look up a cart by device response fingerprint — whether or not its ROM
 * has ever actually been dumped to SD. Returns number of matches written to
 * out[] (capped at max):
 *   0 = not in index (this cart has never been successfully identified before)
 *   1 = unique match
 *  >1 = multiple entries share this fingerprint (hardware-identical carts)
 * The caller is responsible for verifying rom_basename's file still exists
 * before relying on it — identity_known fields are usable immediately
 * either way. */
int cartindex_lookup(const CartInfo *info, CartIndexEntry *out, int max);

/* Records a cart's full identity (type/game_code/title/rom_size_kb/
 * ram_size_kb, taken from *info) in the index, keyed by device fingerprint —
 * independent of whether rom_basename's file exists yet. rom_basename is
 * the bare filename without directory (e.g. "POKEMONRED_.gb") that this
 * cart's ROM either already occupies or will occupy once dumped (callers
 * predict this deterministically via build_rom_path_sd() so the entry is
 * immediately useful for future lookups regardless of dump status — see
 * main.c call sites). Safe to call repeatedly for the same cart; duplicate
 * (fingerprint, rom_basename) pairs are suppressed, but a repeat call with
 * the same fingerprint and a newer identity (e.g. filled in after a header
 * read that a previous call didn't have) still records fresh info.
 *
 * Added 2026-07-31: previously this only ever ran after a successful ROM
 * dump, meaning identification (game_code/title) would not survive between
 * a first "detect this cart" and a later "actually dump it" — a cart
 * detected-but-never-dumped had to pay for a fresh, slow ROM-header read on
 * every subsequent detect, indistinguishable from a genuinely new cart. Now
 * called immediately after any successful header read, so identity is
 * captured on first successful DETECT, not first successful DUMP. */
void cartindex_update(const CartInfo *info, const char *rom_basename);
