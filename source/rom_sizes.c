#include "rom_sizes.h"
#include <string.h>
#include <stddef.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---------------------------------------------------------------------------
 * GBA ROM/RAM size database. Key: first 3 chars of the 4-char game code at
 * ROM header 0xAC (same convention as gametitles.c). See rom_sizes.h for
 * how these values are used and their confidence caveats.
 *
 * GBA has no self-describing size field in its header (unlike GB/GBC, see
 * gb_header_rom_kb()/gb_header_ram_kb() below) — this table plus
 * gbop_detect_gba_mirror_size()'s hardware mirror-check (gb_operator.c) are
 * the only two ways to learn a GBA cart's real size without an external
 * reference. A table hit here is preferred (skips the mirror-check
 * entirely) but is not required for a dump to come out correctly sized.
 *
 * Source: No-Intro (https://www.no-intro.org). CONFIRMED entries were read
 * directly off hardware via Wireshark capture or the device's own (old-
 * firmware) size reporting in this project's testing.
 * --------------------------------------------------------------------------- */
static const struct { char pfx[4]; uint32_t rom_kb; uint32_t ram_kb; } kGBA[] = {
    /* Pokemon Gen 3 — CONFIRMED: Emerald and Sapphire read directly off
     * hardware via Wireshark capture (test v10.0.10, 2026-07-27); FireRed
     * seen with the same 16MB/128KB shape on external hardware. All five
     * share the same cart generation/capacity. */
    { "AXV", 16384, 128 },   /* Pokemon Ruby        CONFIRMED (Sapphire's pair, same gen) */
    { "AXP", 16384, 128 },   /* Pokemon Sapphire     CONFIRMED */
    { "BPR", 16384, 128 },   /* Pokemon FireRed      CONFIRMED */
    { "BPG", 16384, 128 },   /* Pokemon LeafGreen    CONFIRMED (FireRed's pair, same gen) */
    { "BPE", 16384, 128 },   /* Pokemon Emerald      CONFIRMED */
    /* Not independently hardware-verified — No-Intro-sourced only. */
    { "AZL", 8192,  0 },     /* Zelda: A Link to the Past / Four Swords */
    { "BZM", 8192,  0 },     /* Zelda: The Minish Cap */
    { "AGF", 8192,  0 },     /* Golden Sun */
    { "AGS", 8192,  0 },     /* Golden Sun: The Lost Age */
    { "B06", 32768, 0 },     /* Mother 3 (Japan only) — largest common GBA cart size */
    /* Real size given directly by the developer (2026-08-01, in response to
     * Save Read Write Test/test_external_1: an external tester's Mario Golf
     * dump was requested at the 32768KB unrecognized-game fallback — 4x too
     * large — which the ROM-size-doubling investigation elsewhere in this
     * project suggests the device may actually honor/stream up to, rather
     * than reject). Not independently re-verified by this project's own
     * hardware/Wireshark testing. */
    { "BMG", 8192,  0 },     /* Mario Golf: Advance Tour */
};

static int match_gba_prefix(const uint8_t *hdr, char out[4]) {
    for (int i = 0; i < 3; i++) {
        char c = (char)hdr[0xAC + i];
        if (c >= 0x20 && c <= 0x7E) out[i] = c;
        else { out[0] = '\0'; return 0; }
    }
    out[3] = '\0';
    return 1;
}

uint32_t rom_sizes_lookup_rom_kb(CartType type, const uint8_t *hdr) {
    if (!hdr || type != CART_TYPE_GBA) return 0;
    char pfx[4];
    if (!match_gba_prefix(hdr, pfx)) return 0;
    for (size_t i = 0; i < ARRAY_LEN(kGBA); i++)
        if (strcmp(kGBA[i].pfx, pfx) == 0) return kGBA[i].rom_kb;
    return 0;
}

uint32_t rom_sizes_lookup_ram_kb(CartType type, const uint8_t *hdr) {
    if (!hdr || type != CART_TYPE_GBA) return 0;
    char pfx[4];
    if (!match_gba_prefix(hdr, pfx)) return 0;
    for (size_t i = 0; i < ARRAY_LEN(kGBA); i++)
        if (strcmp(kGBA[i].pfx, pfx) == 0) return kGBA[i].ram_kb;
    return 0;
}

/* ---------------------------------------------------------------------------
 * GB/GBC: real ROM/RAM size read directly from the cartridge header, not a
 * curated table. Standard Game Boy header fields (Pan Docs "The Cartridge
 * Header"): ROM size code at 0x148, RAM size code at 0x149. Both offsets
 * fall inside the header checksum range (0x134-0x14C) that
 * header_checksum_valid() (gb_operator.c) already requires to pass before
 * any header buffer is trusted — so these values are exact for every real
 * GB/GBC cart, not a guess limited to whatever games happen to be in a
 * table. Replaces the old kGBC[]/kGBTitle[] curated-table approach entirely
 * (2026-08-01, prompted by an external tester's Pokemon Gold/Super Mario
 * Land reports — a plain DMG cart like Super Mario Land was never going to
 * fit a curated table, and previously fell to the generic 8192KB/128KB
 * fallback: 128x the real ROM size and, critically, a wrongly-assumed save
 * chip on a cart that has none at all).
 * --------------------------------------------------------------------------- */
uint32_t gb_header_rom_kb(const uint8_t *hdr) {
    if (!hdr) return 0;
    switch (hdr[0x148]) {
        case 0x00: return 32;
        case 0x01: return 64;
        case 0x02: return 128;
        case 0x03: return 256;
        case 0x04: return 512;
        case 0x05: return 1024;
        case 0x06: return 2048;
        case 0x07: return 4096;
        case 0x08: return 8192;
        case 0x52: return 1152;
        case 0x53: return 1280;
        case 0x54: return 1536;
        default:   return 0; /* unrecognized code — a real cart is never 0 KB */
    }
}

uint32_t gb_header_ram_kb(const uint8_t *hdr) {
    if (!hdr) return UINT32_MAX;
    switch (hdr[0x149]) {
        case 0x00: return 0;    /* no save RAM — a real, common, correct answer */
        case 0x02: return 8;
        case 0x03: return 32;
        case 0x04: return 128;
        case 0x05: return 64;
        default:   return UINT32_MAX; /* unrecognized — caller treats as unknown */
    }
}
