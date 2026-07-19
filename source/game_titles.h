/*
 * game_titles.h — display-name lookup tables for GB/GBC/GBA cartridges
 *
 * Three tables, each a flat array of { key, display_name } pairs terminated
 * by a NULL-key sentinel.  Callers iterate with strcmp on the key field.
 *
 * GBA  — key is the 4-char game code from ROM header offset 0xAC (4 bytes).
 * GBC  — key is the 4-char manufacturer code from ROM header offset 0x13F,
 *         used only when ROM header byte 0x143 has bit 7 set (0x80 or 0xC0).
 * GB   — key is the title string from ROM header offset 0x134, read as up to
 *         15 printable bytes (0x134–0x142), null-trimmed.  Used only when
 *         0x143 bit 7 is clear.
 *
 * Hardware-confirmed entries are marked "HW".
 * Entries derived from gbhwdb product codes are marked "DB".
 * Entries inferred from regional suffix patterns are marked "INF".
 *
 * Sources
 * -------
 * [1] Game Boy hardware database — https://gbhwdb.gekkio.fi/
 *     Primary source for GBA/GBC/GB product codes (AGB-XXXX-Y, CGB-XXXX-Y,
 *     DMG-XXXX-Y formats) from physical cartridge inspections.
 * [2] Dhole GBA reversing notes CSV —
 *     https://github.com/Dhole/gba-reversing-notes/blob/master/gbalist.csv
 *     Comprehensive GBA internal-code → ROM-name mapping.
 * [3] Data Crystal (TCRF) — https://datacrystal.tcrf.net/
 *     GB ROM header internal-name documentation.  Link's Awakening title
 *     confirmed as "ZELDA" from the LA (Game Boy) page.
 * [4] archive.org manual scans — product codes visible on cartridge labels
 *     used to cross-reference AX4E (SMA4), AA2E (SMA2), AGSE (Golden Sun).
 * [5] User hardware — AAUE (Pokemon Gold USA) and AAXE (Pokemon Silver USA)
 *     read directly from GBC cart header on Wii hardware (HW-confirmed).
 */

#ifndef GAME_TITLES_H
#define GAME_TITLES_H

#ifdef __cplusplus
extern "C" {
#endif

struct title_entry {
    const char *code;
    const char *name;
};

/* ================================================================
 * GBA — 4-char code at ROM offset 0xAC
 * Region suffix: E=USA  J=JPN  P=EUR  F=FRA  D=DEU  S=ESP  I=ITA
 * ================================================================ */
static const struct title_entry gba_titles[] = {

    /* --- Pokemon Ruby / Sapphire --- */
    { "AXVE", "Pokemon Ruby (USA)"          }, /* DB */
    { "AXVJ", "Pokemon Ruby (JPN)"          }, /* DB */
    { "AXVP", "Pokemon Ruby (EUR)"          }, /* DB */
    { "AXVF", "Pokemon Ruby (France)"       }, /* INF */
    { "AXVD", "Pokemon Ruby (Germany)"      }, /* INF */
    { "AXVS", "Pokemon Ruby (Spain)"        }, /* INF */
    { "AXVI", "Pokemon Ruby (Italy)"        }, /* INF */
    { "AXPE", "Pokemon Sapphire (USA)"      }, /* DB */
    { "AXPJ", "Pokemon Sapphire (JPN)"      }, /* DB */
    { "AXPP", "Pokemon Sapphire (EUR)"      }, /* DB */
    { "AXPF", "Pokemon Sapphire (France)"   }, /* INF */
    { "AXPD", "Pokemon Sapphire (Germany)"  }, /* INF */
    { "AXPS", "Pokemon Sapphire (Spain)"    }, /* INF */
    { "AXPI", "Pokemon Sapphire (Italy)"    }, /* INF */

    /* --- Pokemon FireRed / LeafGreen --- */
    { "BPRE", "Pokemon FireRed (USA)"       }, /* HW */
    { "BPRJ", "Pokemon FireRed (JPN)"       }, /* HW */
    { "BPRP", "Pokemon FireRed (EUR)"       }, /* DB */
    { "BPRF", "Pokemon FireRed (France)"    }, /* INF */
    { "BPRD", "Pokemon FireRed (Germany)"   }, /* INF */
    { "BPRS", "Pokemon FireRed (Spain)"     }, /* INF */
    { "BPRI", "Pokemon FireRed (Italy)"     }, /* INF */
    { "BPGE", "Pokemon LeafGreen (USA)"     }, /* DB */
    { "BPGJ", "Pokemon LeafGreen (JPN)"     }, /* DB */
    { "BPGP", "Pokemon LeafGreen (EUR)"     }, /* DB */
    { "BPGF", "Pokemon LeafGreen (France)"  }, /* INF */
    { "BPGD", "Pokemon LeafGreen (Germany)" }, /* INF */
    { "BPGS", "Pokemon LeafGreen (Spain)"   }, /* INF */
    { "BPGI", "Pokemon LeafGreen (Italy)"   }, /* INF */

    /* --- Pokemon Emerald --- */
    { "BPEE", "Pokemon Emerald (USA)"       }, /* DB */
    { "BPEJ", "Pokemon Emerald (JPN)"       }, /* DB */
    { "BPEP", "Pokemon Emerald (EUR)"       }, /* DB */
    { "BPEF", "Pokemon Emerald (France)"    }, /* INF */
    { "BPED", "Pokemon Emerald (Germany)"   }, /* INF */
    { "BPES", "Pokemon Emerald (Spain)"     }, /* INF */
    { "BPEI", "Pokemon Emerald (Italy)"     }, /* INF */

    /* --- Pokemon Mystery Dungeon: Red Rescue Team --- */
    { "B24E", "Pokemon Mystery Dungeon: Red Rescue Team (USA)" }, /* DB */
    { "B24J", "Pokemon Fushigi no Dungeon: Red (JPN)"          }, /* DB */
    { "B24P", "Pokemon Mystery Dungeon: Red Rescue Team (EUR)" }, /* INF */

    /* --- Mario Kart: Super Circuit --- */
    { "AMKE", "Mario Kart: Super Circuit (USA)" }, /* DB */
    { "AMKJ", "Mario Kart: Super Circuit (JPN)" }, /* DB */
    { "AMKP", "Mario Kart: Super Circuit (EUR)" }, /* DB */

    /* --- Super Mario Advance 1–4 --- */
    { "AMAE", "Super Mario Advance (USA)"                            }, /* DB */
    { "AMAJ", "Super Mario Advance (JPN)"                            }, /* DB */
    { "AMAP", "Super Mario Advance (EUR)"                            }, /* DB */
    { "AA2E", "Super Mario Advance 2: Super Mario World (USA)"       }, /* DB */
    { "AA2J", "Super Mario Advance 2: Super Mario World (JPN)"       }, /* DB */
    { "AA2P", "Super Mario Advance 2: Super Mario World (EUR)"       }, /* DB */
    { "A3AE", "Super Mario Advance 3: Yoshi's Island (USA)"          }, /* DB */
    { "A3AJ", "Super Mario Advance 3: Yoshi's Island (JPN)"          }, /* DB */
    { "A3AP", "Super Mario Advance 3: Yoshi's Island (EUR)"          }, /* DB */
    { "AX4E", "Super Mario Advance 4: Super Mario Bros. 3 (USA)"     }, /* DB */
    { "AX4J", "Super Mario Advance 4: Super Mario Bros. 3 (JPN)"     }, /* DB */
    { "AX4P", "Super Mario Advance 4: Super Mario Bros. 3 (EUR)"     }, /* DB */

    /* --- Wario Land 4 --- */
    { "AWAE", "Wario Land 4 (USA)" }, /* DB */
    { "AWAJ", "Wario Land 4 (JPN)" }, /* DB */
    { "AWAP", "Wario Land 4 (EUR)" }, /* DB */

    /* --- Legend of Zelda --- */
    /* AZLE is also used by Link's Awakening DX on GBC — separate table */
    { "AZLE", "Legend of Zelda: A Link to the Past (USA)" }, /* DB */
    { "AZLJ", "Legend of Zelda: A Link to the Past (JPN)" }, /* DB */
    { "AZLP", "Legend of Zelda: A Link to the Past (EUR)" }, /* DB */
    { "BZME", "Legend of Zelda: The Minish Cap (USA)"     }, /* DB */
    { "BZMJ", "Legend of Zelda: The Minish Cap (JPN)"     }, /* DB */
    { "BZMP", "Legend of Zelda: The Minish Cap (EUR)"     }, /* DB */

    /* --- Metroid --- */
    { "AMFE", "Metroid Fusion (USA)"          }, /* DB */
    { "AMFJ", "Metroid Fusion (JPN)"          }, /* DB */
    { "AMFP", "Metroid Fusion (EUR)"          }, /* DB */
    { "AMZE", "Metroid: Zero Mission (USA)"   }, /* DB */
    { "AMZJ", "Metroid: Zero Mission (JPN)"   }, /* DB */
    { "BMXP", "Metroid: Zero Mission (EUR)"   }, /* DB — unusual B prefix for EUR */

    /* --- Kirby --- */
    { "A7KE", "Kirby: Nightmare in Dream Land (USA)" }, /* DB */
    { "A7KJ", "Kirby: Nightmare in Dream Land (JPN)" }, /* DB */
    { "A7KP", "Kirby: Nightmare in Dream Land (EUR)" }, /* DB */
    { "B8KE", "Kirby and the Amazing Mirror (USA)"   }, /* DB */
    { "B8KJ", "Kirby and the Amazing Mirror (JPN)"   }, /* DB */
    { "B8KP", "Kirby and the Amazing Mirror (EUR)"   }, /* DB */

    /* --- Castlevania --- */
    { "AAME", "Castlevania: Circle of the Moon (USA)" }, /* DB */
    { "AAMJ", "Castlevania: Circle of the Moon (JPN)" }, /* DB */
    { "AAMP", "Castlevania: Circle of the Moon (EUR)" }, /* DB */
    { "ACHE", "Castlevania: Harmony of Dissonance (USA)" }, /* DB */
    { "ACHJ", "Castlevania: Harmony of Dissonance (JPN)" }, /* DB */
    { "ACHP", "Castlevania: Harmony of Dissonance (EUR)" }, /* DB */
    { "A2CE", "Castlevania: Aria of Sorrow (USA)" }, /* DB */
    { "A2CJ", "Castlevania: Aria of Sorrow (JPN)" }, /* DB */
    { "A2CP", "Castlevania: Aria of Sorrow (EUR)" }, /* DB */

    /* --- Fire Emblem --- */
    { "AFEJ", "Fire Emblem: The Binding Blade (JPN)"      }, /* DB — JPN only */
    { "AE7E", "Fire Emblem (USA)"                          }, /* DB */
    { "AE7J", "Fire Emblem: Rekka no Ken (JPN)"            }, /* DB */
    { "AE7P", "Fire Emblem (EUR)"                          }, /* DB */
    { "BE8E", "Fire Emblem: The Sacred Stones (USA)"       }, /* DB */
    { "BE8J", "Fire Emblem: The Sacred Stones (JPN)"       }, /* DB */
    { "BE8P", "Fire Emblem: The Sacred Stones (EUR)"       }, /* DB */

    /* --- Advance Wars --- */
    { "AWRE", "Advance Wars (USA)"                    }, /* DB */
    { "AWRP", "Advance Wars (EUR)"                    }, /* DB */
    { "AW2E", "Advance Wars 2: Black Hole Rising (USA)" }, /* DB */
    { "AW2P", "Advance Wars 2: Black Hole Rising (EUR)" }, /* DB */

    /* --- Golden Sun --- */
    { "AGSE", "Golden Sun (USA)"              }, /* DB */
    { "AGSJ", "Golden Sun (JPN)"              }, /* DB */
    { "AGSP", "Golden Sun (EUR)"              }, /* DB */
    { "AGSD", "Golden Sun (Germany)"          }, /* INF */
    { "AGSF", "Golden Sun (France)"           }, /* INF */
    { "AGSS", "Golden Sun (Spain)"            }, /* INF */
    { "AGSI", "Golden Sun (Italy)"            }, /* INF */
    { "AGFE", "Golden Sun: The Lost Age (USA)"     }, /* DB */
    { "AGFJ", "Golden Sun: The Lost Age (JPN)"     }, /* DB */
    { "AGFP", "Golden Sun: The Lost Age (EUR)"     }, /* DB */
    { "AGFD", "Golden Sun: The Lost Age (Germany)" }, /* INF */
    { "AGFF", "Golden Sun: The Lost Age (France)"  }, /* INF */
    { "AGFS", "Golden Sun: The Lost Age (Spain)"   }, /* INF */
    { "AGFI", "Golden Sun: The Lost Age (Italy)"   }, /* INF */

    /* --- F-Zero --- */
    { "AFZE", "F-Zero: Maximum Velocity (USA)" }, /* DB */
    { "AFZJ", "F-Zero: Maximum Velocity (JPN)" }, /* DB */
    { "AFZP", "F-Zero: Maximum Velocity (EUR)" }, /* DB */
    { "BFZE", "F-Zero: GP Legend (USA)"         }, /* DB */
    { "BFZJ", "F-Zero: GP Legend (JPN)"         }, /* DB */
    { "BFZP", "F-Zero: GP Legend (EUR)"         }, /* DB */
    { "BFTJ", "F-Zero Climax (JPN)"             }, /* DB — JPN only */

    /* --- Final Fantasy Tactics Advance --- */
    { "AFXE", "Final Fantasy Tactics Advance (USA)" }, /* DB */
    { "AFXJ", "Final Fantasy Tactics Advance (JPN)" }, /* DB */
    { "AFXP", "Final Fantasy Tactics Advance (EUR)" }, /* DB */

    /* --- Sonic Advance 1–3 --- */
    { "ASOE", "Sonic Advance (USA)"   }, /* DB */
    { "ASOJ", "Sonic Advance (JPN)"   }, /* DB */
    { "ASOP", "Sonic Advance (EUR)"   }, /* DB */
    { "A2NE", "Sonic Advance 2 (USA)" }, /* DB */
    { "A2NJ", "Sonic Advance 2 (JPN)" }, /* DB */
    { "A2NP", "Sonic Advance 2 (EUR)" }, /* DB */
    { "B3SE", "Sonic Advance 3 (USA)" }, /* DB */
    { "B3SJ", "Sonic Advance 3 (JPN)" }, /* DB */
    { "B3SP", "Sonic Advance 3 (EUR)" }, /* DB */

    /* --- Mega Man Zero 1–4 --- */
    { "AZCE", "Mega Man Zero (USA)"    }, /* DB [2] */
    { "ARZJ", "Rockman Zero (JPN)"     }, /* DB [2] */
    { "AZCP", "Mega Man Zero (EUR)"    }, /* INF */
    { "A4ZE", "Mega Man Zero 2 (USA)"  }, /* DB [1] */
    { "A4ZJ", "Rockman Zero 2 (JPN)"   }, /* DB [1] */
    { "A4ZP", "Mega Man Zero 2 (EUR)"  }, /* DB [1] */
    { "A6ZE", "Mega Man Zero 3 (USA)"  }, /* DB [1] */
    { "A6ZJ", "Rockman Zero 3 (JPN)"   }, /* DB [1] */
    { "A6ZP", "Mega Man Zero 3 (EUR)"  }, /* DB [1] */
    { "AZEE", "Mega Man Zero 4 (USA)"  }, /* DB [1] */
    { "AZEJ", "Rockman Zero 4 (JPN)"   }, /* DB [1] */
    { "AZEP", "Mega Man Zero 4 (EUR)"  }, /* DB [1] */

    /* --- Mega Man Battle Network / Rockman EXE --- */
    { "AREE", "Mega Man Battle Network (USA)"              }, /* DB */
    { "AREJ", "Rockman EXE (JPN)"                          }, /* DB */
    { "AE2E", "Mega Man Battle Network 2 (USA)"            }, /* DB */
    { "AE2J", "Rockman EXE 2 (JPN)"                        }, /* DB */
    { "A6BE", "Mega Man Battle Network 3: White (USA)"     }, /* DB */
    { "A6BJ", "Rockman EXE 3 (JPN)"                        }, /* DB */
    { "A3XE", "Mega Man Battle Network 3: Blue (USA)"      }, /* DB */
    { "A3XJ", "Rockman EXE 3 Black (JPN)"                  }, /* DB */
    { "B4WE", "Mega Man Battle Network 4: Red Sun (USA)"   }, /* DB */
    { "B4WJ", "Rockman EXE 4: Tournament Red Sun (JPN)"    }, /* DB */
    { "B4BE", "Mega Man Battle Network 4: Blue Moon (USA)" }, /* DB */
    { "B4BJ", "Rockman EXE 4: Tournament Blue Moon (JPN)"  }, /* DB */
    { "BRBE", "Mega Man Battle Network 5: Team ProtoMan (USA)" }, /* DB */
    { "BKBE", "Mega Man Battle Network 5: Team Colonel (USA)"  }, /* INF */
    { "BRBJ", "Rockman EXE 5: Team of Blues (JPN)"             }, /* DB */

    /* --- Donkey Kong Country GBA ports --- */
    { "A5NE", "Donkey Kong Country (USA)" }, /* DB */
    { "A5NJ", "Donkey Kong Country (JPN)" }, /* DB */
    { "A5NP", "Donkey Kong Country (EUR)" }, /* DB */
    { "B2DE", "Donkey Kong Country 2 (USA)" }, /* DB */
    { "B2DJ", "Donkey Kong Country 2 (JPN)" }, /* DB */
    { "B2DP", "Donkey Kong Country 2 (EUR)" }, /* DB */
    { "BDQE", "Donkey Kong Country 3 (USA)" }, /* DB */
    { "BDQP", "Donkey Kong Country 3 (EUR)" }, /* DB */

    /* --- Mother 3 --- */
    { "A3UJ", "Mother 3 (JPN)" }, /* DB — JPN only */

    /* --- Classic NES Series (USA/EUR) / Famicom Mini (JPN) ---
     * F prefix = Famicom emulated.  Region suffix as above.
     * USA and EUR used "Classic NES Series" branding;
     * JPN used "Famicom Mini" branding.  Same ROMs, same prefix.       */
    { "FSME", "Classic NES Series: Super Mario Bros. (USA)"         }, /* DB */
    { "FSMP", "Classic NES Series: Super Mario Bros. (EUR)"         }, /* INF */
    { "FSMJ", "Famicom Mini: Super Mario Bros. (JPN)"               }, /* DB */
    { "FZLE", "Classic NES Series: The Legend of Zelda (USA)"       }, /* DB */
    { "FZLP", "Classic NES Series: The Legend of Zelda (EUR)"       }, /* INF */
    { "FZLJ", "Famicom Mini: The Legend of Zelda (JPN)"             }, /* DB */
    { "FLBE", "Classic NES Series: Zelda II - Adv. of Link (USA)"   }, /* DB */
    { "FLBP", "Classic NES Series: Zelda II - Adv. of Link (EUR)"   }, /* INF */
    { "FLBJ", "Famicom Mini: Zelda II - Link no Bouken (JPN)"       }, /* DB */
    { "FMRE", "Classic NES Series: Metroid (USA)"                   }, /* DB */
    { "FMRP", "Classic NES Series: Metroid (EUR)"                   }, /* INF */
    { "FMRJ", "Famicom Mini: Metroid (JPN)"                         }, /* DB */
    { "FDKE", "Classic NES Series: Donkey Kong (USA)"               }, /* DB */
    { "FDKP", "Classic NES Series: Donkey Kong (EUR)"               }, /* INF */
    { "FDKJ", "Famicom Mini: Donkey Kong (JPN)"                     }, /* DB */
    { "FDME", "Classic NES Series: Dr. Mario (USA)"                 }, /* DB */
    { "FDMP", "Classic NES Series: Dr. Mario (EUR)"                 }, /* INF */
    { "FDMJ", "Famicom Mini: Dr. Mario (JPN)"                       }, /* DB */
    { "FICE", "Classic NES Series: Ice Climber (USA)"               }, /* DB */
    { "FICP", "Classic NES Series: Ice Climber (EUR)"               }, /* INF */
    { "FICJ", "Famicom Mini: Ice Climber (JPN)"                     }, /* DB */
    { "FEBE", "Classic NES Series: Excitebike (USA)"                }, /* DB */
    { "FEBP", "Classic NES Series: Excitebike (EUR)"                }, /* INF */
    { "FEBJ", "Famicom Mini: Excitebike (JPN)"                      }, /* DB */
    { "FP7E", "Classic NES Series: Pac-Man (USA)"                   }, /* DB */
    { "FP7J", "Famicom Mini: Pac-Man (JPN)"                         }, /* DB */
    { "FXVE", "Classic NES Series: Xevious (USA)"                   }, /* DB */
    { "FXVJ", "Famicom Mini: Xevious (JPN)"                         }, /* DB */
    { "FADE", "Classic NES Series: Castlevania (USA)"               }, /* DB */
    { "FADJ", "Famicom Mini: Castlevania (JPN)"                     }, /* DB */
    { "FBME", "Classic NES Series: Bomberman (USA)"                 }, /* DB */
    { "FBMJ", "Famicom Mini: Bomberman (JPN)"                       }, /* DB */

    { NULL, NULL }
};

/* ================================================================
 * GBC — 4-char manufacturer code at ROM offset 0x13F
 * Used only when header byte 0x143 has bit 7 set (0x80 or 0xC0).
 * Region suffix: E=USA  J=JPN  P=EUR  D=DEU  F=FRA  S=ESP  I=ITA
 * Note: the suffix is embedded in the manufacturer code (no fixed
 * position), so entries must be listed individually per region.
 * ================================================================ */
static const struct title_entry gbc_titles[] = {

    /* --- Pokemon Gold --- */
    { "AAUE", "Pokemon Gold (USA)"         }, /* HW — hardware confirmed */
    { "AAUJ", "Pokemon Gold (JPN)"         }, /* INF */
    { "AAUP", "Pokemon Gold (EUR)"         }, /* DB */

    /* --- Pokemon Silver --- */
    { "AAXE", "Pokemon Silver (USA)"       }, /* HW — hardware confirmed */
    { "AAXJ", "Pokemon Silver (JPN)"       }, /* INF */
    { "AAXP", "Pokemon Silver (EUR)"       }, /* DB */
    { "AAXD", "Pokemon Silver (Germany)"   }, /* DB — "Silberne Edition" */

    /* --- Pokemon Crystal --- */
    { "BYTE", "Pokemon Crystal (USA/EUR)"  }, /* DB */
    { "BXTJ", "Pokemon Crystal (JPN)"      }, /* DB */

    /* --- Pokemon Yellow: Special Pikachu Edition ---
     * GBC-compatible (0x143=0x80); manufacturer code from DMG-APSE-0 label. */
    { "APSE", "Pokemon Yellow: Pikachu Edition (USA/EUR)" }, /* DB */
    { "APSJ", "Pokemon Yellow (JPN)"                      }, /* INF */

    /* --- Pokemon Trading Card Game --- */
    { "AXQE", "Pokemon Trading Card Game (USA)" }, /* DB */
    { "AXQJ", "Pokemon Card GB (JPN)"            }, /* DB */
    { "AXQP", "Pokemon Trading Card Game (EUR)"  }, /* DB */

    /* --- Pokemon Puzzle Challenge --- */
    { "BPNE", "Pokemon Puzzle Challenge (USA)" }, /* DB */
    { "BPNP", "Pokemon Puzzle Challenge (EUR)" }, /* INF */
    { "BPNJ", "Pokemon de Panepon (JPN)"       }, /* INF */

    /* --- Pokemon Pinball ---
     * V prefix unusual; confirmed from cartridge database entries.         */
    { "VPHE", "Pokemon Pinball (USA)"          }, /* DB */
    { "VPHJ", "Pokemon Pinball (JPN)"           }, /* DB */
    { "VPHP", "Pokemon Pinball (EUR)"           }, /* INF */

    /* --- Legend of Zelda: Link's Awakening DX ---
     * GBC-compatible (0x143=0x80).  Code AZLE also appears in GBA table
     * for A Link to the Past — different platforms, no collision.          */
    { "AZLE", "Legend of Zelda: Link's Awakening DX (USA/EUR)" }, /* DB */
    { "AZLJ", "Legend of Zelda: Link's Awakening DX (JPN)"     }, /* DB */

    /* --- Legend of Zelda: Oracle of Ages --- */
    { "AZ8E", "Legend of Zelda: Oracle of Ages (USA)"   }, /* DB */
    { "AZ8J", "Legend of Zelda: Oracle of Ages (JPN)"   }, /* DB */
    { "AZ8P", "Legend of Zelda: Oracle of Ages (EUR)"   }, /* DB */

    /* --- Legend of Zelda: Oracle of Seasons --- */
    { "AZ7E", "Legend of Zelda: Oracle of Seasons (USA)" }, /* DB */
    { "AZ7J", "Legend of Zelda: Oracle of Seasons (JPN)" }, /* DB */
    { "AZ7P", "Legend of Zelda: Oracle of Seasons (EUR)" }, /* DB */

    /* --- Wario Land 2 (GBC-compatible, also runs on original GB) --- */
    { "AWLE", "Wario Land 2 (USA)"   }, /* DB */
    { "AWLJ", "Wario Land 2 (JPN)"   }, /* DB */
    { "AWLP", "Wario Land 2 (EUR)"   }, /* DB */

    /* --- Wario Land 3 (GBC-only) --- */
    { "AW8E", "Wario Land 3 (USA)" }, /* DB */
    { "AW8J", "Wario Land 3 (JPN)" }, /* DB */
    { "AW8P", "Wario Land 3 (EUR)" }, /* DB */

    /* --- Mario Golf GBC --- */
    { "AWXE", "Mario Golf (GBC, USA)" }, /* DB */
    { "AWXJ", "Mario Golf (GBC, JPN)" }, /* DB */
    { "AWXP", "Mario Golf (GBC, EUR)" }, /* INF */

    /* --- Mario Tennis GBC --- */
    { "BM8E", "Mario Tennis (GBC, USA)" }, /* DB */
    { "BM8J", "Mario Tennis (GBC, JPN)" }, /* DB */
    { "BM8P", "Mario Tennis (GBC, EUR)" }, /* INF */

    /* --- Donkey Kong Country (GBC port of SNES original, 2000) --- */
    { "BDDE", "Donkey Kong Country (GBC, USA)" }, /* DB */
    { "BDDJ", "Donkey Kong Country (GBC, JPN)" }, /* DB */
    { "BDDP", "Donkey Kong Country (GBC, EUR)" }, /* INF */

    /* --- Kirby: Tilt 'n' Tumble (GBC-only, accelerometer) --- */
    { "KTNE", "Kirby: Tilt 'n' Tumble (USA)"    }, /* DB */
    { "KKKJ", "Korokoro Kirby (JPN)"             }, /* DB */

    /* --- Metal Gear Solid (GBC) --- */
    { "BMGE", "Metal Gear Solid (GBC, USA)" }, /* DB */
    { "BMGJ", "Metal Gear Solid (GBC, JPN)" }, /* DB */
    { "BMGP", "Metal Gear Solid (GBC, EUR)" }, /* INF */

    /* --- Tetris DX (GBC-compatible launch title) --- */
    { "ATDE", "Tetris DX (USA/EUR)" }, /* DB */
    { "ATDJ", "Tetris DX (JPN)"     }, /* INF */

    { NULL, NULL }
};

/* ================================================================
 * GB — title string at ROM offset 0x134, 15 bytes max (0x134–0x142),
 * null-trimmed, exactly as stored in the ROM (uppercase ASCII).
 * Used only when ROM header byte 0x143 has bit 7 CLEAR.
 *
 * Note on Kirby's Dream Land: the ROM stores "KIRBY DREAM LAND"
 * across all 16 bytes 0x134–0x143, making 0x143='D' (0x44).
 * Since bit 7 of 0x44 is 0, it is treated as a pure-GB game.
 * Reading 15 bytes (0x134–0x142) yields "KIRBY DREAM LAN".
 * ================================================================ */
static const struct title_entry gb_titles[] = {

    /* --- Pokemon --- */
    { "POKEMON RED",   "Pokemon Red (USA/EUR)"  }, /* DB */
    { "POKEMON BLUE",  "Pokemon Blue (USA/EUR)" }, /* DB */
    /* Pokemon Green was JPN only and shares "POCKET MONSTERS" title;
     * Pokemon Blue (JPN) also uses a different internal title.
     * These JPN variants are omitted as they are extremely rare outside
     * import carts and use different publisher/distributor detection.      */

    /* --- Tetris --- */
    { "TETRIS",        "Tetris (USA/EUR)"       }, /* DB */

    /* --- Super Mario Land series --- */
    { "SUPER MARIOLAND", "Super Mario Land (USA/EUR)"                   }, /* DB */
    { "MARIOLAND2",      "Super Mario Land 2: 6 Golden Coins (USA/EUR)" }, /* DB */

    /* --- Dr. Mario --- */
    { "DR.MARIO",      "Dr. Mario (USA/EUR)"    }, /* DB */

    /* --- Kirby --- */
    /* 15-char read of 16-char ROM title "KIRBY DREAM LAND" */
    { "KIRBY DREAM LAN", "Kirby's Dream Land (USA/EUR)"   }, /* DB */
    { "KIRBY2",          "Kirby's Dream Land 2 (USA/EUR)" }, /* DB */

    /* --- Wario Land --- */
    { "WARIO LAND",    "Wario Land: Super Mario Land 3 (USA/EUR)" }, /* DB */

    /* --- Metroid II --- */
    { "METROID II",    "Metroid II: Return of Samus (USA/EUR)" }, /* DB */

    /* --- Legend of Zelda: Link's Awakening (original GB, 1993) ---
     * Internal title confirmed as "ZELDA" by Data Crystal (TCRF) [3].    */
    { "ZELDA",         "Legend of Zelda: Link's Awakening (USA/EUR)" }, /* DB [3] */

    /* --- Donkey Kong (1994) --- */
    { "DONKEY KONG",   "Donkey Kong (USA/EUR)" }, /* DB */

    /* --- Donkey Kong Land series --- */
    { "DONKEY KONG LAND",  "Donkey Kong Land (USA/EUR)"   }, /* DB */
    { "DONKEY KONG LAND2", "Donkey Kong Land 2 (USA/EUR)" }, /* DB */
    { "DONKEY KONG LAND3", "Donkey Kong Land 3 (USA/EUR)" }, /* INF */

    /* --- Kirby's Pinball Land --- */
    { "KIRBY'S PINBALL", "Kirby's Pinball Land (USA/EUR)" }, /* INF */

    /* --- Game Boy Gallery / Game & Watch Gallery --- */
    { "GAME BOY GALLERY",  "Game Boy Gallery (AUS/EUR)"     }, /* INF */
    { "GAMEBOY GALLERY",   "Game & Watch Gallery (USA)"     }, /* INF */
    { "GAMEBOY GALLERY2",  "Game & Watch Gallery 2 (USA)"   }, /* INF */
    { "GAMEBOY GALLERY3",  "Game & Watch Gallery 3 (USA/EUR)" }, /* INF */

    { NULL, NULL }
};

/*
 * Example lookup function:
 *
 *   const char *gba_lookup(const char code[4]) {
 *       char key[5] = { code[0], code[1], code[2], code[3], '\0' };
 *       for (int i = 0; gba_titles[i].code; i++)
 *           if (strcmp(gba_titles[i].code, key) == 0)
 *               return gba_titles[i].name;
 *       return NULL;
 *   }
 */

#ifdef __cplusplus
}
#endif

#endif /* GAME_TITLES_H */
