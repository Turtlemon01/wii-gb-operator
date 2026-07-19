#pragma once
#include "gb_operator.h"
#include <stdint.h>

/* Returns a canonical (region-free) display name for the inserted cart,
 * or NULL if not in the database.  hdr must be the 512-byte buffer filled
 * by gbop_read_rom_header.
 *
 * Lookup keys (read directly from hdr — independent of CartInfo.type):
 *   GBA              : first 3 chars of 4-char game code at hdr[0xAC]
 *   GBC (0x143=0x80/C0): first 3 chars of 4-char CGB code at hdr[0x13F]
 *   GB  (0x143=0x00)  : exact ASCII title string at hdr[0x134], up to 15 chars
 *
 * The returned name has NO region suffix.  Append gametitles_region_suffix()
 * to form the full display string (P1 in the priority chain). */
const char *gametitles_lookup(CartType type, const uint8_t *hdr);

/* Returns a region suffix string derived from the ROM header, e.g. " (USA)",
 * " (Japan)", " (Europe)", or "" when region cannot be determined.
 * For GBA/GBC: reads 4th char of the game/CGB code.
 * For DMG: reads the destination byte at 0x14A (Japan vs. non-Japan only).
 * Safe to call when hdr is all-zeros — returns "". */
const char *gametitles_region_suffix(CartType type, const uint8_t *hdr);
