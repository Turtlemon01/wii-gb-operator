#pragma once
#include "gb_operator.h"
#include <stdint.h>

/* Best-effort ROM/RAM size lookup, keyed by game code the same way
 * gametitles.c is (first 3 chars of the 4-char GBA code / CGB code,
 * region-agnostic).
 *
 * Why this exists: on GB Operator firmware v10.0.10, no field in the
 * cart-info response (cmd 0x04) has been found to reliably encode ROM or
 * RAM size — see CLAUDE.md "resp[26..29] is a fixed constant" finding.
 * The new-firmware ROM/save read protocol terminates on a device-sent
 * C0 DE 01 ... footer marker rather than a byte count, so an entry here
 * being wrong or absent does not, as currently understood, prevent a dump
 * from completing correctly — it only affects the progress percentage
 * shown while it runs, and the size initially requested in the read
 * command. That second part is a working assumption, not a confirmed
 * fact: it has not been tested what a real device does when asked for a
 * size that doesn't match its actual capacity. Treat entries here as
 * display/estimate hints, not protocol-critical truth.
 *
 * Sources: No-Intro (https://www.no-intro.org) for ROM sizes; entries
 * marked CONFIRMED were read directly off hardware in this project's own
 * testing (see CLAUDE.md dated 2026-07-27 entries). Entries without that
 * marker are No-Intro-sourced but not independently hardware-verified —
 * check before trusting them for anything beyond a progress estimate.
 *
 * Returns 0 if the game is not in the table (caller falls back to
 * whatever its own unknown-size handling is).
 */
uint32_t rom_sizes_lookup_rom_kb(CartType type, const uint8_t *hdr);
uint32_t rom_sizes_lookup_ram_kb(CartType type, const uint8_t *hdr);
