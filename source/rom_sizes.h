#pragma once
#include "gb_operator.h"
#include <stdint.h>

/* GBA-only best-effort ROM/RAM size lookup, keyed by game code (first 3
 * chars of the 4-char GBA code, region-agnostic). GBA has no self-
 * describing size field in its header (unlike GB/GBC — see
 * gb_header_rom_kb()/gb_header_ram_kb() below), so this curated table is
 * still the fast path for known games; gbop_detect_gba_mirror_size()
 * (gb_operator.c) determines the real size directly from the cart's own
 * hardware mirroring behavior for anything not in this table.
 *
 * Returns 0 for a non-GBA type or a game not in the table (caller falls
 * back to whatever its own unknown-size handling is). Confirmed: the
 * device DOES stream up to (at least) whatever size is requested rather
 * than always terminating at the cart's true capacity on its own (Save
 * Read Write Test/test_external_1, 2026-08-01) — so a badly wrong entry
 * here is not "just a progress estimate," it can produce an oversized
 * dump padded with mirrored content past the real end; the post-capture
 * mirror-check in main.c's dump_rom_new_protocol() corrects this whenever
 * the table doesn't have an entry (rom_size_confirmed == 0).
 *
 * Sources: No-Intro (https://www.no-intro.org) for ROM sizes; entries
 * marked CONFIRMED were read directly off hardware in this project's own
 * testing (see CLAUDE.md dated 2026-07-27 entries). Entries without that
 * marker are No-Intro-sourced but not independently hardware-verified.
 */
uint32_t rom_sizes_lookup_rom_kb(CartType type, const uint8_t *hdr);
uint32_t rom_sizes_lookup_ram_kb(CartType type, const uint8_t *hdr);

/* GB/GBC: real ROM size read directly from the cartridge header (standard
 * Game Boy ROM-size code at offset 0x148, Pan Docs "The Cartridge Header").
 * Exact for any real cart, not a table lookup — see rom_sizes.c for why
 * this is trustworthy (the byte falls inside header_checksum_valid()'s
 * already-required checksum range). Returns 0 if the byte doesn't match a
 * known code (should not happen once the header checksum has passed) — a
 * real GB/GBC ROM is never legitimately 0 KB, so 0 unambiguously means
 * "unrecognized, fall back to a generic guess."
 */
uint32_t gb_header_rom_kb(const uint8_t *hdr);

/* GB/GBC: real RAM (save) size read directly from the cartridge header
 * (standard Game Boy RAM-size code at offset 0x149). UNLIKE ROM size, a
 * genuine "no save RAM" (code 0x00) is a real, common, correct answer
 * (e.g. Super Mario Land has no save chip at all) — it must NOT be treated
 * as "unknown" and coerced to a nonzero fallback. Returns UINT32_MAX (not
 * 0) for a code that doesn't match any known value, so the caller can
 * distinguish "confirmed zero" from "unrecognized."
 */
uint32_t gb_header_ram_kb(const uint8_t *hdr);
