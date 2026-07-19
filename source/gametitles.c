#include "gametitles.h"
#include <string.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * GBA game title database.
 * Key: first 3 chars of the 4-char game code at ROM header 0xAC.
 * The 4th char encodes region and is derived separately by gametitles_region_suffix.
 * One entry covers all regional variants — e.g. "BPR" matches BPRE/BPRJ/BPRP/etc.
 *
 * Sources: No-Intro (https://www.no-intro.org),
 *          libretro-database (https://github.com/libretro/libretro-database).
 * Hardware-confirmed entries noted in comments.
 * --------------------------------------------------------------------------- */
static const struct { char pfx[4]; const char *name; } kGBA[] = {
    /* Pokemon main series */
    { "AXV", "Pokemon Ruby" },
    { "AXP", "Pokemon Sapphire" },
    { "BPR", "Pokemon FireRed" },          /* BPRE confirmed on hardware (test_91) */
    { "BPG", "Pokemon LeafGreen" },
    { "BPE", "Pokemon Emerald" },
    /* Pokemon spinoffs */
    { "B24", "Pokemon Mystery Dungeon: Red Rescue Team" },
    { "BPP", "Pokemon Pinball: Ruby & Sapphire" },
    /* Mario Kart */
    { "AMC", "Mario Kart: Super Circuit" },
    /* Super Mario Advance series */
    { "FMA", "Super Mario Advance" },
    { "AA2", "Super Mario Advance 2: Super Mario World" },
    { "AMZ", "Super Mario Advance 3: Yoshi's Island" },
    { "AX4", "Super Mario Advance 4: Super Mario Bros. 3" },
    /* The Legend of Zelda */
    { "AZL", "The Legend of Zelda: A Link to the Past" },
    { "BZM", "The Legend of Zelda: The Minish Cap" },
    /* Metroid */
    { "AMT", "Metroid Fusion" },
    { "BMX", "Metroid: Zero Mission" },
    /* Kirby */
    { "AIK", "Kirby: Nightmare in Dream Land" },
    { "B86", "Kirby & The Amazing Mirror" },
    /* Golden Sun */
    { "AGF", "Golden Sun" },
    { "AGS", "Golden Sun: The Lost Age" },
    /* Fire Emblem */
    { "AFE", "Fire Emblem" },
    { "BE8", "Fire Emblem: The Sacred Stones" },
    /* Advance Wars */
    { "AWR", "Advance Wars" },
    { "AW2", "Advance Wars 2: Black Hole Rising" },
    /* F-Zero */
    { "AFZ", "F-Zero: Maximum Velocity" },
    { "BFT", "F-Zero Climax" },    /* Japan only */
    /* Castlevania */
    { "ACH", "Castlevania: Circle of the Moon" },
    { "AAM", "Castlevania: Harmony of Dissonance" },
    { "BCH", "Castlevania: Aria of Sorrow" },
    /* Sonic Advance series */
    { "ASM", "Sonic Advance" },
    { "A2S", "Sonic Advance 2" },
    { "B3S", "Sonic Advance 3" },
    /* Mega Man Battle Network */
    { "AE4", "Mega Man Battle Network" },
    /* Mega Man Zero series — codes A4ZE/A6ZE/AZEE confirmed via gbhwdb */
    { "AZC", "Mega Man Zero" },
    { "A4Z", "Mega Man Zero 2" },
    { "A6Z", "Mega Man Zero 3" },
    { "AZE", "Mega Man Zero 4" },
    /* Mother 3 (Japan only) */
    { "B06", "Mother 3" },
    /* Wario */
    { "AWL", "Wario Land 4" },
    { "AZW", "WarioWare, Inc.: Mega Microgame$!" },
    /* Donkey Kong Country */
    { "ADM", "Donkey Kong Country" },
    { "AD2", "Donkey Kong Country 2" },
    { "AX2", "Donkey Kong Country 3" },
};

/* ---------------------------------------------------------------------------
 * GBC (Game Boy Color) title database.
 * Key: first 3 chars of the 4-char CGB code at ROM header 0x13F.
 * Only present when hdr[0x143] is 0x80 (GBC-compatible) or 0xC0 (GBC-only).
 * Sources: No-Intro, libretro-database.
 * Hardware-confirmed: AAUE (Gold), AAXE (Silver) — CLAUDE.md border test.
 * --------------------------------------------------------------------------- */
static const struct { char pfx[4]; const char *name; } kGBC[] = {
    /* Pokemon main series */
    { "AAU", "Pokemon Gold" },     /* AAUE/AAUJ confirmed on hardware */
    { "AAX", "Pokemon Silver" },   /* AAXE/AAXJ confirmed on hardware */
    { "BYT", "Pokemon Crystal" },
    /* Pokemon spinoffs */
    { "DMG", "Pokemon Trading Card Game" },
    { "BPP", "Pokemon Pinball" },
    /* The Legend of Zelda: Oracle series */
    { "AZL", "Zelda: Oracle of Ages" },
    { "AZS", "Zelda: Oracle of Seasons" },
    /* Mega Man Xtreme */
    { "AXC", "Mega Man Xtreme" },
    /* Dragon Warrior Monsters */
    { "ADM", "Dragon Warrior Monsters" },
    /* Metal Gear Solid */
    { "AMG", "Metal Gear Solid" },
    /* Shantae */
    { "ASH", "Shantae" },
};

/* ---------------------------------------------------------------------------
 * GB (original Game Boy / DMG) title database.
 * Key: exact ASCII title string at ROM header 0x134, up to 15 chars.
 * Sources: No-Intro, libretro-database.
 * "ZELDA" confirmed as Link's Awakening header title.
 * --------------------------------------------------------------------------- */
static const struct { char title[16]; const char *name; } kGB[] = {
    /* Pokemon */
    { "POKEMON RED",    "Pokemon Red" },
    { "POKEMON BLUE",   "Pokemon Blue" },
    { "POKEMON YELLOW", "Pokemon Yellow" },
    /* Nintendo first-party */
    { "TETRIS",          "Tetris" },
    { "DR.MARIO",        "Dr. Mario" },
    { "SUPER MARIOLAN",  "Super Mario Land" },
    { "SUPER MARIOLAND2","Super Mario Land 2" },
    { "WARIO LAND",      "Wario Land: Super Mario Land 3" },
    { "ZELDA",           "The Legend of Zelda: Link's Awakening" },   /* confirmed */
    { "ZELDA DX",        "The Legend of Zelda: Link's Awakening DX" },
    { "METROID2",        "Metroid II: Return of Samus" },
    { "KIRBYDREAM",      "Kirby's Dream Land" },
    { "KIRBY BLOCK",     "Kirby's Block Ball" },
    { "KIRBY'S PINBALL", "Kirby's Pinball Land" },
    { "DONKEY KONG",     "Donkey Kong" },
    /* Third-party */
    { "CASTLEVANIA",     "Castlevania: The Adventure" },
    { "MEGAMAN",         "Mega Man: Dr. Wily's Revenge" },
    { "MEGAMAN II",      "Mega Man II" },
    { "MEGAMAN III",     "Mega Man III" },
    { "MEGAMAN IV",      "Mega Man IV" },
    { "MEGAMAN V",       "Mega Man V" },
    { "FINAL FANTASY",   "Final Fantasy Adventure" },
    { "GAMEBOY CAMERA",  "Game Boy Camera" },
    /* Japan originals */
    { "POCKET MONSTERS", "Pocket Monsters Red/Green" },
};

/* --------------------------------------------------------------------------- */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

const char *gametitles_lookup(CartType type, const uint8_t *hdr) {
    if (!hdr) return NULL;

    if (type == CART_TYPE_GBA) {
        /* Match on first 3 chars of the 4-char game code at 0xAC */
        char pfx[4] = {0};
        for (int i = 0; i < 3; i++) {
            char c = (char)hdr[0xAC + i];
            if (c >= 0x20 && c <= 0x7E) pfx[i] = c;
            else { pfx[0] = '\0'; break; }
        }
        if (!pfx[0]) return NULL;
        for (size_t i = 0; i < ARRAY_LEN(kGBA); i++) {
            if (strcmp(kGBA[i].pfx, pfx) == 0) return kGBA[i].name;
        }
        return NULL;
    }

    /* GB / GBC: check CGB flag at 0x143 */
    uint8_t cgb = hdr[0x143];
    if (cgb == 0x80 || cgb == 0xC0) {
        /* CGB cart: match on first 3 chars of the 4-char code at 0x13F */
        char pfx[4] = {0};
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            char c = (char)hdr[0x13F + i];
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) pfx[i] = c;
            else { ok = 0; break; }
        }
        if (ok) {
            for (size_t i = 0; i < ARRAY_LEN(kGBC); i++) {
                if (strcmp(kGBC[i].pfx, pfx) == 0) return kGBC[i].name;
            }
        }
    }

    /* DMG or unrecognized CGB: use title string at 0x134 */
    char title[16] = {0};
    int maxlen = (cgb == 0x80 || cgb == 0xC0) ? 11 : 15;
    int len = 0;
    for (int i = 0; i < maxlen; i++) {
        char c = (char)hdr[0x134 + i];
        if (c < 0x20 || c > 0x7E) break;
        title[len++] = c;
    }
    if (!len) return NULL;
    for (size_t i = 0; i < ARRAY_LEN(kGB); i++) {
        if (strcmp(kGB[i].title, title) == 0) return kGB[i].name;
    }
    return NULL;
}

const char *gametitles_region_suffix(CartType type, const uint8_t *hdr) {
    if (!hdr) return "";

    char rc = '\0';
    if (type == CART_TYPE_GBA) {
        /* 4-char game code at 0xAC; region = 4th char (0xAF).
         * Verify first 3 chars are printable before trusting the 4th. */
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            char c = (char)hdr[0xAC + i];
            if (c < 0x20 || c > 0x7E) { ok = 0; break; }
        }
        if (ok) rc = (char)hdr[0xAF];
    } else {
        uint8_t cgb = hdr[0x143];
        if (cgb == 0x80 || cgb == 0xC0) {
            /* CGB code at 0x13F; region = 4th char (0x142) */
            int ok = 1;
            for (int i = 0; i < 4; i++) {
                char c = (char)hdr[0x13F + i];
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) { ok = 0; break; }
            }
            if (ok) rc = (char)hdr[0x142];
        } else {
            /* DMG: destination byte at 0x14A (0x00=Japan, 0x01=Non-Japan).
             * Only trust it if the title field has printable characters. */
            if (hdr[0x134] >= 0x20 && hdr[0x134] <= 0x7E)
                return (hdr[0x14A] == 0x00) ? " (Japan)" : "";
            return "";
        }
    }

    switch (rc) {
        case 'E': return " (USA)";
        case 'J': return " (Japan)";
        case 'P': return " (Europe)";
        case 'F': return " (France)";
        case 'D': return " (Germany)";
        case 'S': return " (Spain)";
        case 'I': return " (Italy)";
        case 'X': return " (Europe)";
        case 'A': return " (Australia)";
        case 'U': return " (Australia)";
        case 'C': return " (China)";
        case 'K': return " (Korea)";
        default:  return "";
    }
}
