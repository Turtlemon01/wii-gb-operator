#include "rom_sizes.h"
#include <string.h>
#include <stddef.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---------------------------------------------------------------------------
 * GBA ROM/RAM size database. Key: first 3 chars of the 4-char game code at
 * ROM header 0xAC (same convention as gametitles.c). See rom_sizes.h for
 * how these values are used and their confidence caveats.
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
};

/* ---------------------------------------------------------------------------
 * GBC ROM/RAM size database. Key: first 3 chars of the 4-char CGB code at
 * ROM header 0x13F.
 * --------------------------------------------------------------------------- */
static const struct { char pfx[4]; uint32_t rom_kb; uint32_t ram_kb; } kGBC[] = {
    /* Pokemon Gold/Silver — CONFIRMED: Silver read directly off hardware
     * via Wireshark capture (test v10.0.10, 2026-07-27); Gold shares the
     * same cart generation/capacity. */
    { "AAU", 2048, 32 },     /* Pokemon Gold    CONFIRMED (Silver's pair, same gen) */
    { "AAX", 2048, 32 },     /* Pokemon Silver  CONFIRMED */
    { "BYT", 2048, 32 },     /* Pokemon Crystal — not independently verified, same gen as G/S */
};

/* ---------------------------------------------------------------------------
 * Pure (non-Color) Game Boy cart database. Unlike GBA/GBC, a plain DMG cart
 * has no reliable machine-readable code — match_gbc_prefix() below requires
 * the CGB compatibility flag at 0x143, which these carts never set (they
 * predate Game Boy Color entirely), so the code-based lookup always misses
 * for them and previously fell straight to the generic unrecognized-game
 * fallback (8192KB — 8x the real size for Gen 1 Pokemon). Matched by the
 * trimmed title string at header offset 0x134 instead (added 2026-07-31,
 * Detect Cart Test/test_4: Pokemon Red reported as 8192KB/128KB against its
 * real 1024KB/32KB).
 *
 * Source: No-Intro / Pan Docs (MBC3, standard Gen 1 Pokemon cart shape) —
 * not independently hardware-verified via this project's own Wireshark
 * captures the way the GBC Gold/Silver entries above are.
 * --------------------------------------------------------------------------- */
static const struct { const char *title; uint32_t rom_kb; uint32_t ram_kb; } kGBTitle[] = {
    { "POKEMON RED",    1024, 32 },
    { "POKEMON BLUE",   1024, 32 },
    { "POKEMON GREEN",  1024, 32 },  /* Japan-only Midori/Green */
    { "POKEMON YELLOW", 1024, 32 },
};

/* Header title field is 16 bytes at 0x134 (this path is only ever reached
 * for a cart WITHOUT the CGB flag, so the older, full-width field applies —
 * newer carts that shrink it to 15 bytes for the CGB flag byte are handled
 * by the GBC code-based path above instead), null- or space-padded. */
static int extract_title_trimmed(const uint8_t *hdr, char out[17]) {
    int len = 0;
    for (int i = 0; i < 16; i++) {
        char c = (char)hdr[0x134 + i];
        if (c == 0x00) break;
        out[len++] = c;
    }
    while (len > 0 && out[len - 1] == ' ') len--;
    out[len] = '\0';
    return len;
}

static int match_gba_prefix(const uint8_t *hdr, char out[4]) {
    for (int i = 0; i < 3; i++) {
        char c = (char)hdr[0xAC + i];
        if (c >= 0x20 && c <= 0x7E) out[i] = c;
        else { out[0] = '\0'; return 0; }
    }
    out[3] = '\0';
    return 1;
}

static int match_gbc_prefix(const uint8_t *hdr, char out[4]) {
    uint8_t cgb = hdr[0x143];
    if (cgb != 0x80 && cgb != 0xC0) return 0;
    for (int i = 0; i < 3; i++) {
        char c = (char)hdr[0x13F + i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[i] = c;
        else { out[0] = '\0'; return 0; }
    }
    out[3] = '\0';
    return 1;
}

uint32_t rom_sizes_lookup_rom_kb(CartType type, const uint8_t *hdr) {
    if (!hdr) return 0;
    char pfx[4];
    if (type == CART_TYPE_GBA) {
        if (!match_gba_prefix(hdr, pfx)) return 0;
        for (size_t i = 0; i < ARRAY_LEN(kGBA); i++)
            if (strcmp(kGBA[i].pfx, pfx) == 0) return kGBA[i].rom_kb;
        return 0;
    }
    if (match_gbc_prefix(hdr, pfx)) {
        for (size_t i = 0; i < ARRAY_LEN(kGBC); i++)
            if (strcmp(kGBC[i].pfx, pfx) == 0) return kGBC[i].rom_kb;
        return 0;
    }
    /* No CGB flag — a plain DMG cart, which never has a code-based match.
     * Fall back to a title match instead of the generic 8192KB guess. */
    char title[17];
    extract_title_trimmed(hdr, title);
    for (size_t i = 0; i < ARRAY_LEN(kGBTitle); i++)
        if (strcmp(kGBTitle[i].title, title) == 0) return kGBTitle[i].rom_kb;
    return 0;
}

uint32_t rom_sizes_lookup_ram_kb(CartType type, const uint8_t *hdr) {
    if (!hdr) return 0;
    char pfx[4];
    if (type == CART_TYPE_GBA) {
        if (!match_gba_prefix(hdr, pfx)) return 0;
        for (size_t i = 0; i < ARRAY_LEN(kGBA); i++)
            if (strcmp(kGBA[i].pfx, pfx) == 0) return kGBA[i].ram_kb;
        return 0;
    }
    if (match_gbc_prefix(hdr, pfx)) {
        for (size_t i = 0; i < ARRAY_LEN(kGBC); i++)
            if (strcmp(kGBC[i].pfx, pfx) == 0) return kGBC[i].ram_kb;
        return 0;
    }
    char title[17];
    extract_title_trimmed(hdr, title);
    for (size_t i = 0; i < ARRAY_LEN(kGBTitle); i++)
        if (strcmp(kGBTitle[i].title, title) == 0) return kGBTitle[i].ram_kb;
    return 0;
}
