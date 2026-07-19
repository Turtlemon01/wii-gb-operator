#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <gccore.h>
#include <time.h>
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include "log.h"
#include "gb_operator.h"
#include "rom_cache.h"
#include "mgba_frontend.h"
#include "settings.h"
#include "cartindex.h"
#include "gametitles.h"

void *xfb   = NULL;
GXRModeObj *rmode = NULL;

FILE *g_log = NULL;
char  g_log_path[64] = {0};
int   g_log_suppress_console = 0; /* set to 1 during GX emulation */

/* Set by the physical Wii Reset button; checked in all blocking loops. */
volatile int g_reset_pressed = 0;
static void main_on_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; g_reset_pressed = 1; }
static void main_on_power(void) {
    /* Commit log to SD before powering off — libfat may hold dirty sectors in RAM
     * after fflush; fclose forces a physical write. Safe here: no concurrent thread
     * holds g_log outside of an mGBA session, and we're powering off immediately. */
    if (g_log) { fclose(g_log); g_log = NULL; }
    SYS_ResetSystem(SYS_POWEROFF_STANDBY, 0, 0);
}


#define DEV_MENU_ITEMS 8

#define SAVES_DIR    "sd:/apps/wii-gb-operator/saves"
#define BACKUPS_DIR  "sd:/apps/wii-gb-operator/saves/backups"
#define ROMS_DIR     ROM_CACHE_DIR_SD
#define STICK_THRESH 40

// Extracts the full title from a ROM header buffer.
// GB/GBC: title at 0x0134, length 11 for CGB carts (CGB flag 0x80/0xC0 at 0x0143)
//         or 15 for DMG-only carts. Stops at first non-printable byte.
// GBA:    title at 0xA0, up to 12 bytes.
// Returns the number of chars written (0 if header too short or title empty).
static int extract_rom_title(CartType type, const uint8_t *hdr, size_t hdr_len,
                              char *out, size_t out_max) {
    if (!hdr || !out || out_max < 2) return 0;
    int start, maxlen;
    if (type == CART_TYPE_GBA) {
        if (hdr_len < 0xAC) return 0;
        start = 0xA0; maxlen = 12;
    } else {
        if (hdr_len < 0x0144) return 0;
        uint8_t cgb = hdr[0x0143];
        start = 0x0134;
        maxlen = (cgb == 0x80 || cgb == 0xC0) ? 11 : 15;
    }
    int len = 0;
    for (int i = 0; i < maxlen && (size_t)(len + 1) < out_max; i++) {
        char c = (char)hdr[start + i];
        if (c < 0x20 || c > 0x7E) break;
        out[len++] = c;
    }
    out[len] = '\0';
    return len;
}

// Reads full title and game code from a ROM file header, updates info in-place.
// Returns 1 if any field changed, 0 if info was already correct or file unreadable.
static int enrich_info_from_rom(const char *path, CartInfo *info) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[0x0150];
    memset(hdr, 0, sizeof(hdr));
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);

    int changed = 0;

    char title[17] = {0};
    if (extract_rom_title(info->type, hdr, n, title, sizeof(title)) > 0 &&
        strcmp(title, info->title) != 0) {
        strncpy(info->title, title, sizeof(info->title) - 1);
        info->title[sizeof(info->title) - 1] = '\0';
        changed = 1;
    }

    // GBA: game code is 4 ASCII bytes at ROM header offset 0xAC
    if (info->type == CART_TYPE_GBA && n >= 0xB0 && info->game_code[0] == '\0') {
        char code[5] = {0};
        int len = 0;
        for (int i = 0; i < 4 && len < 4; i++) {
            char c = (char)hdr[0xAC + i];
            if (c >= 0x20 && c <= 0x7E) code[len++] = c;
            else break;
        }
        if (len > 0) {
            strncpy(info->game_code, code, sizeof(info->game_code) - 1);
            info->game_code[sizeof(info->game_code) - 1] = '\0';
            changed = 1;
        }
    }

    if (changed)
        lprintf("[info] Enriched: title=\"%s\" code=\"%s\"\n", info->title, info->game_code);
    return changed;
}

// Enriches CartInfo from a 512-byte ROM header read by gbop_read_rom_header.
// Updates title (full ROM header title), game_code (GBA/CGB 4-char code), and
// type/type_str (CART_TYPE_GBC when CGB flag at hdr[0x143] is 0x80 or 0xC0).
// Call before try_enrich_info so the correct title is used for path lookup.
static void enrich_info_from_buf(CartInfo *info, const uint8_t *hdr) {
    if (!info || !hdr) return;

    char title[17] = {0};
    if (extract_rom_title(info->type, hdr, 512, title, sizeof(title)) > 0) {
        strncpy(info->title, title, sizeof(info->title) - 1);
        info->title[sizeof(info->title) - 1] = '\0';
    }

    if (info->type == CART_TYPE_GBA) {
        /* GBA: 4-char game code at ROM header offset 0xAC */
        char code[5] = {0};
        int len = 0;
        for (int i = 0; i < 4; i++) {
            char c = (char)hdr[0xAC + i];
            if (c >= 0x20 && c <= 0x7E) code[len++] = c;
            else break;
        }
        if (len == 4) {
            strncpy(info->game_code, code, sizeof(info->game_code) - 1);
            info->game_code[sizeof(info->game_code) - 1] = '\0';
        }
    } else {
        /* GB/GBC: detect Color flag at 0x143 and extract CGB code at 0x13F */
        uint8_t cgb = hdr[0x143];
        if (cgb == 0x80 || cgb == 0xC0) {
            info->type = CART_TYPE_GBC;
            strncpy(info->type_str, "GBC", sizeof(info->type_str));
        }
        if (info->type == CART_TYPE_GBC) {
            char code[5] = {0};
            int ok = 1;
            for (int i = 0; i < 4; i++) {
                char c = (char)hdr[0x13F + i];
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) code[i] = c;
                else { ok = 0; break; }
            }
            if (ok) {
                strncpy(info->game_code, code, sizeof(info->game_code) - 1);
                info->game_code[sizeof(info->game_code) - 1] = '\0';
            }
        }
    }
    lprintf("[hdr] Enriched: type=%s title=\"%s\" code=\"%s\"\n",
            info->type_str, info->title, info->game_code);
}

// Mirrors rom_cache.c's static build_path for SD (duplicated since it's static there).
// Used when we need the canonical ROM path after enriching info.title/game_code.
static void build_rom_path_sd(const CartInfo *info, char *out, size_t size) {
    char safe[32] = {0};
    int j = 0;
    for (int i = 0; info->title[i] && j < 31; i++) {
        char c = info->title[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            safe[j++] = c;
        else if (c == ' ' && j > 0)
            safe[j++] = '_';
    }
    const char *ext = (info->type == CART_TYPE_GBA) ? "gba"
                    : (info->type == CART_TYPE_GBC) ? "gbc" : "gb";
    snprintf(out, size, "%s/%s_%s.%s", ROM_CACHE_DIR_SD, safe, info->game_code, ext);
}

// Enriches info with full title/code from a cached ROM file at the standard path.
// Returns 1 if a matching ROM was found at the expected path, 0 otherwise.
//
// Size-scan fallback is intentionally absent: the GB Operator device response
// contains too little data to distinguish hardware-identical carts (e.g. Pokémon
// Red vs Blue — same MBC, ROM size, RAM size, and partial title "P").  A size
// scan would corrupt info with another game's title/code and cause rom_cache_exists
// to report the wrong game as cached, blocking the dump of the intended cart.
// When a cart is not found at the standard path, the user must dump it fresh; the
// mGBA ROM browser still allows manual selection of any previously-cached file.
static int try_enrich_info(CartInfo *info) {
    char rom_path[256] = {0};

    if (rom_cache_exists(info, rom_path, sizeof(rom_path))) {
        enrich_info_from_rom(rom_path, info);
        return 1;
    }

    return 0;
}

/* Presents a scrollable list of ambiguous ROM matches (hardware-identical carts).
 * Pre-reads each candidate's full title from its ROM file header.
 * Returns the selected index [0..n-1] or -1 if the user pressed B to skip. */
static int select_from_matches(CartType type, CartIndexEntry *entries,
                               char paths[][256], int n) {
    char titles[8][32];
    for (int i = 0; i < n; i++) {
        CartInfo tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.type = type;
        if (enrich_info_from_rom(paths[i], &tmp) && tmp.title[0])
            strncpy(titles[i], tmp.title, 31);
        else
            strncpy(titles[i], entries[i].rom_basename, 31);
        titles[i][31] = '\0';
    }

    int sel = 0;
    while (1) {
        printf("\x1b[2J\x1b[H");
        printf("Multiple ROMs match this cart.\n");
        printf("Which game is this?\n\n");
        for (int i = 0; i < n; i++)
            printf("%s%s\n", (i == sel) ? "> " : "  ", titles[i]);
        printf("\n[Up/Down] Select   [A] Confirm   [B] Skip\n");

        VIDEO_WaitVSync();
        PAD_ScanPads();
        WPAD_ScanPads();
        u16 btn = PAD_ButtonsDown(0);
        if (btn & PAD_BUTTON_UP)   sel = (sel + n - 1) % n;
        if (btn & PAD_BUTTON_DOWN) sel = (sel + 1) % n;
        if (btn & PAD_BUTTON_A)    return sel;
        if (btn & PAD_BUTTON_B)    return -1;
    }
}

/* Check cart index for a previously-dumped ROM whose device fingerprint matches
 * the current cart.  Enriches info from the matched ROM file and returns 1.
 * If multiple entries match (hardware-identical twins), shows a selection list.
 * Returns 0 if nothing was found or the user cancelled disambiguation. */
static int try_enrich_from_index(CartInfo *info) {
    CartIndexEntry entries[8];
    int n = cartindex_lookup(info, entries, 8);
    if (n == 0) return 0;

    /* Validate: file exists and size matches */
    CartIndexEntry valid[8];
    char valid_paths[8][256];
    int valid_n = 0;
    for (int i = 0; i < n && valid_n < 8; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", ROM_CACHE_DIR_SD, entries[i].rom_basename);
        FILE *f = fopen(path, "rb");
        if (!f) {
            snprintf(path, sizeof(path), "%s/%s", ROM_CACHE_DIR_USB, entries[i].rom_basename);
            f = fopen(path, "rb");
        }
        if (!f) { lprintf("[index] Entry missing: %s\n", entries[i].rom_basename); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if ((uint32_t)sz != info->rom_size_kb * 1024) {
            lprintf("[index] Size mismatch: %s (%ld vs %u)\n",
                    entries[i].rom_basename, sz, info->rom_size_kb * 1024);
            continue;
        }
        valid[valid_n] = entries[i];
        strncpy(valid_paths[valid_n], path, sizeof(valid_paths[0]) - 1);
        valid_paths[valid_n][sizeof(valid_paths[0]) - 1] = '\0';
        valid_n++;
    }
    if (valid_n == 0) return 0;

    int sel = 0;
    if (valid_n > 1)
        sel = select_from_matches(info->type, valid, valid_paths, valid_n);
    if (sel < 0) return 0;

    enrich_info_from_rom(valid_paths[sel], info);
    lprintf("[index] Enriched from index: title=\"%s\" code=\"%s\"\n",
            info->title, info->game_code);
    return 1;
}

// Build save path from cart info — mirrors the ROM naming convention so each
// game's save file pairs unambiguously with its ROM.
// GB/GBC (empty game_code): saves/POKEMONGOLD.sav
// GBA  (4-char game_code):  saves/POKEMONFIRE_BPRE.sav
static void build_save_path(const CartInfo *info, char *path, size_t path_size) {
    char safe[32] = {0};
    int j = 0;
    for (int i = 0; info->title[i] && j < 31; i++) {
        char c = info->title[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            safe[j++] = c;
        else if (c == ' ' && j > 0)
            safe[j++] = '_';
    }
    if (info->game_code[0])
        snprintf(path, path_size, "sd:/apps/wii-gb-operator/saves/%s_%s.sav",
                 safe, info->game_code);
    else
        snprintf(path, path_size, "sd:/apps/wii-gb-operator/saves/%s.sav", safe);
}

/* -----------------------------------------------------------------------
 * ROM / save file browsers for the mGBA launch flow.
 * ---------------------------------------------------------------------- */

#define BROWSER_MAX 48

/* Fills names[]/paths[] with files matching ext inside dir_path.
 * Returns count. names/paths are static so callers must copy what they need. */
static int scan_dir(const char *dir_path, const char *ext,
                    char names[BROWSER_MAX][64], char paths[BROWSER_MAX][256]) {
    DIR *d = opendir(dir_path);
    if (!d) return 0;
    int cnt = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && cnt < BROWSER_MAX) {
        size_t nlen = strlen(ent->d_name);
        size_t elen = strlen(ext);
        if (nlen <= elen) continue;
        if (strcasecmp(ent->d_name + nlen - elen, ext) != 0) continue;
        strncpy(names[cnt], ent->d_name, 63);
        names[cnt][63] = '\0';
        snprintf(paths[cnt], 255, "%s/%s", dir_path, ent->d_name);
        cnt++;
    }
    closedir(d);
    return cnt;
}

static void draw_browser(const char *title, const char *items[], int count, int sel) {
    printf("\x1b[2J\x1b[H");
    printf("%s\n", title);
    for (int i = 0; i < (int)strlen(title); i++) printf("=");
    printf("\n\n");
    if (count == 0) {
        printf("  (no files found)\n");
    } else {
        int top = sel - 8;
        if (top < 0) top = 0;
        for (int i = top; i < count && i < top + 18; i++)
            printf("  %s %s\n", i == sel ? ">" : " ", items[i]);
    }
    printf("\nD-Pad: navigate   A: select   B: cancel\n");
}

/* Show a file list browser.  Returns selected index or -1 on cancel. */
static int run_browser(const char *title, const char *items[], int count) {
    if (count == 0) {
        printf("\x1b[2J\x1b[H%s\n\n(no files found)\n\nB/1: back\n", title);
        while (!g_reset_pressed) {
            VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
            if ((PAD_ButtonsDown(0) & PAD_BUTTON_B) || (WPAD_ButtonsDown(0) & WPAD_BUTTON_B)) return -1;
        }
        return -1;
    }
    int sel = 0;
    draw_browser(title, items, count, sel);
    s8 psy = 0, pcy = 0;
    while (!g_reset_pressed) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        WPAD_ScanPads();
        u16 pressed  = PAD_ButtonsDown(0);
        u32 wpressed = WPAD_ButtonsDown(0);
        s8 sy = PAD_StickY(0), cy = PAD_SubStickY(0);
        bool sup = ((sy > STICK_THRESH || cy > STICK_THRESH) && !(psy > STICK_THRESH || pcy > STICK_THRESH));
        bool sdn = ((sy < -STICK_THRESH || cy < -STICK_THRESH) && !(psy < -STICK_THRESH || pcy < -STICK_THRESH));
        psy = sy; pcy = cy;
        if (((pressed & PAD_BUTTON_UP)   || (wpressed & WPAD_BUTTON_UP)   || sup) && sel > 0)         { sel--; draw_browser(title, items, count, sel); }
        if (((pressed & PAD_BUTTON_DOWN) || (wpressed & WPAD_BUTTON_DOWN) || sdn) && sel < count - 1) { sel++; draw_browser(title, items, count, sel); }
        if ((pressed & PAD_BUTTON_A) || (wpressed & WPAD_BUTTON_A)) return sel;
        if ((pressed & PAD_BUTTON_B) || (wpressed & WPAD_BUTTON_B)) return -1;
    }
    return -1;
}

/* Launch mGBA: browse for a ROM, then a save, then run.
 * Called after gbop_close() — USB is not held while browsing. */
static void launch_mgba(const CartInfo *info) {
    static char rom_names[BROWSER_MAX][64];
    static char rom_paths[BROWSER_MAX][256];
    static const char *rom_items[BROWSER_MAX];

    /* Collect ROMs — scan for .gb, .gbc, .gba */
    int rom_cnt = 0;
    {
        static char rn_gb[BROWSER_MAX][64], rp_gb[BROWSER_MAX][256];
        static char rn_gbc[BROWSER_MAX][64], rp_gbc[BROWSER_MAX][256];
        static char rn_gba[BROWSER_MAX][64], rp_gba[BROWSER_MAX][256];
        int n_gb  = scan_dir(ROMS_DIR, ".gb",  rn_gb,  rp_gb);
        int n_gbc = scan_dir(ROMS_DIR, ".gbc", rn_gbc, rp_gbc);
        int n_gba = scan_dir(ROMS_DIR, ".gba", rn_gba, rp_gba);
        for (int i = 0; i < n_gb  && rom_cnt < BROWSER_MAX; i++, rom_cnt++) {
            strncpy(rom_names[rom_cnt], rn_gb[i],  63);
            strncpy(rom_paths[rom_cnt], rp_gb[i], 255);
        }
        for (int i = 0; i < n_gbc && rom_cnt < BROWSER_MAX; i++, rom_cnt++) {
            strncpy(rom_names[rom_cnt], rn_gbc[i],  63);
            strncpy(rom_paths[rom_cnt], rp_gbc[i], 255);
        }
        for (int i = 0; i < n_gba && rom_cnt < BROWSER_MAX; i++, rom_cnt++) {
            strncpy(rom_names[rom_cnt], rn_gba[i],  63);
            strncpy(rom_paths[rom_cnt], rp_gba[i], 255);
        }
    }
    for (int i = 0; i < rom_cnt; i++) rom_items[i] = rom_names[i];
    lprintf("[mgba] Found %d ROMs in %s\n", rom_cnt, ROMS_DIR);

    int rom_sel = run_browser("Select ROM", (const char **)rom_items, rom_cnt);
    if (rom_sel < 0) { lprintf("[mgba] ROM selection cancelled\n"); return; }
    lprintf("[mgba] ROM selected: %s\n", rom_paths[rom_sel]);

    /* Collect saves */
    static char sav_names[BROWSER_MAX + 1][64];
    static char sav_paths[BROWSER_MAX + 1][256];
    static const char *sav_items[BROWSER_MAX + 1];
    int sav_cnt = 0;

    /* "No save" always first */
    strncpy(sav_names[0], "(No save / new game)", 63);
    sav_paths[0][0] = '\0';
    sav_cnt = 1;
    {
        static char sn[BROWSER_MAX][64], sp[BROWSER_MAX][256];
        int n = scan_dir(SAVES_DIR, ".sav", sn, sp);
        for (int i = 0; i < n && sav_cnt <= BROWSER_MAX; i++, sav_cnt++) {
            strncpy(sav_names[sav_cnt], sn[i], 63);
            strncpy(sav_paths[sav_cnt], sp[i], 255);
        }
    }
    for (int i = 0; i < sav_cnt; i++) sav_items[i] = sav_names[i];
    lprintf("[mgba] Found %d saves in %s\n", sav_cnt - 1, SAVES_DIR);

    int sav_sel = run_browser("Select Save", (const char **)sav_items, sav_cnt);
    if (sav_sel < 0) { lprintf("[mgba] Save selection cancelled\n"); return; }

    const char *chosen_save = (sav_paths[sav_sel][0] != '\0') ? sav_paths[sav_sel] : NULL;
    lprintf("[mgba] Save selected: %s\n", chosen_save ? chosen_save : "(none)");

    /* Determine save size from CartInfo or from file size */
    uint32_t save_kb = info->ram_size_kb;
    if (chosen_save && save_kb == 0) {
        FILE *f = fopen(chosen_save, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            save_kb = (uint32_t)(sz / 1024);
        }
    }

    lprintf("[mgba] Launching mGBA: ROM=%s save=%s save_kb=%u\n",
            rom_paths[rom_sel], chosen_save ? chosen_save : "none", save_kb);

    mgba_run(info, rom_paths[rom_sel], chosen_save, save_kb);
}

/* Builds the P1/P2/P3 priority display title into out[out_size]:
 *   P1: canonical name from gametitles table (region-free) + derived region suffix
 *   P2: raw ROM header title (info->title) + derived region suffix
 *   P3: raw ROM header title only (region suffix is "" — e.g. non-Japan DMG)
 * Falls back to "(unknown)" only if all sources are empty. */
static void make_display_title(const CartInfo *info, const uint8_t *hdr,
                                char *out, size_t out_size) {
    const char *base   = gametitles_lookup(info->type, hdr);
    const char *region = gametitles_region_suffix(info->type, hdr);
    if (base) {
        snprintf(out, out_size, "%s%s", base, region);
    } else if (info->title[0]) {
        snprintf(out, out_size, "%s%s", info->title, region);
    } else {
        strncpy(out, "(unknown)", out_size - 1);
        out[out_size - 1] = '\0';
    }
}

// Core cart detection: retry cart_info (up to 3 attempts), then run mini-dump
// if the cart changed. No UI — prompts/waits are the caller's responsibility.
// Updates *info, rom_hdr[512], display_title on success. Returns 1 on success, 0 on failure.
static int run_detect_cart_inner(CartInfo *info, uint8_t *rom_hdr,
                                  char *display_title, size_t dsize) {
    GBOperatorHandle detect_op = gbop_reopen();
    CartInfo new_info = {0};
    int cart_ok = (detect_op && gbop_read_cart_info(detect_op, &new_info) == 0);
    for (int retry = 0; !cart_ok && retry < 2; retry++) {
        if (detect_op) { gbop_close(detect_op); detect_op = NULL; }
        lprintf("[detect] cart_info retry %d\n", retry + 1);
        usleep(500000);
        detect_op = gbop_reopen();
        if (!detect_op) continue;
        memset(&new_info, 0, sizeof(new_info));
        cart_ok = (gbop_read_cart_info(detect_op, &new_info) == 0);
    }
    if (!cart_ok) {
        if (detect_op) { gbop_close(detect_op); }
        lprintf("[detect] No cart found\n");
        return 0;
    }
    int same_cart = (memcmp(new_info.raw_resp, info->raw_resp, 60) == 0 && rom_hdr[0] != 0);
    gbop_close(detect_op); detect_op = NULL;
    uint8_t dc_hdr[512];
    bool dc_hdr_loaded = false;
    if (same_cart) {
        memcpy(dc_hdr, rom_hdr, 512);
        enrich_info_from_buf(&new_info, dc_hdr);
        dc_hdr_loaded = true;
        lprintf("[detect] Same cart — reusing cached rom_hdr\n");
    } else {
        memset(dc_hdr, 0, 512);
    }
    *info = new_info;
    // Enrich game_code/title from cartindex before looking up the ROM file on SD.
    // try_enrich_from_index matches by raw_resp fingerprint, not game_code, so it
    // works even when game_code is empty.  The result populates game_code so that
    // the rom_cache_exists call below can build the correct filename.
    if (!try_enrich_info(info))
        try_enrich_from_index(info);
    // After enrichment, load ROM header bytes from SD for make_display_title.
    // Skipped for same_cart (dc_hdr already set above).
    if (!dc_hdr_loaded) {
        char cached_path[256] = {0};
        if (rom_cache_exists(info, cached_path, sizeof(cached_path))) {
            FILE *f = fopen(cached_path, "rb");
            if (f) {
                fread(dc_hdr, 1, sizeof(dc_hdr), f);
                fclose(f);
                enrich_info_from_buf(info, dc_hdr);
                lprintf("[detect] ROM header from SD: %s\n", cached_path);
            }
        } else {
            /* Both cartindex and filename-based lookups failed.
             * Try a mini ROM header read (512 bytes) to get the real game
             * code, then retry the SD lookup.  Costs ~1s but runs only once;
             * on success cartindex_update ensures future swaps are instant. */
            lprintf("[detect] cartindex miss — reading ROM header for game code\n");
            GBOperatorHandle hdr_op = gbop_reopen();
            if (hdr_op) {
                uint8_t hdr_buf[512];
                memset(hdr_buf, 0, sizeof(hdr_buf));
                int hdr_rc = gbop_read_rom_header(hdr_op, hdr_buf);
                if (hdr_rc != 0) {
                    /* ROM header cmd stalled (-7005): fd was spent by prior polls/detect.
                     * The stall triggers IOS re-enumeration; wait for fresh fd and retry.
                     * Same recovery pattern as play_game's fresh-fd wait. */
                    lprintf("[detect] ROM hdr read failed — waiting for fresh fd to retry\n");
                    int32_t hdr_old_fd = gbop_get_fd(hdr_op);
                    gbop_close(hdr_op); hdr_op = NULL;
                    for (int i = 0; i < 50 && !hdr_op; i++) {
                        usleep(60000);
                        hdr_op = gbop_reopen();
                        if (!hdr_op) continue;
                        if (gbop_get_fd(hdr_op) == hdr_old_fd) {
                            gbop_close(hdr_op); hdr_op = NULL;
                        }
                    }
                    if (hdr_op) {
                        lprintf("[detect] hdr retry on fresh fd=%d\n",
                                (int)gbop_get_fd(hdr_op));
                        memset(hdr_buf, 0, 512);
                        hdr_rc = gbop_read_rom_header(hdr_op, hdr_buf);
                    }
                }
                if (hdr_rc == 0) {
                    enrich_info_from_buf(info, hdr_buf);
                    lprintf("[detect] ROM hdr: code=%s title=%s\n",
                            info->game_code, info->title);
                    if (rom_cache_exists(info, cached_path, sizeof(cached_path))) {
                        FILE *hf = fopen(cached_path, "rb");
                        if (hf) {
                            fread(dc_hdr, 1, sizeof(dc_hdr), hf);
                            fclose(hf);
                            enrich_info_from_buf(info, dc_hdr);
                            lprintf("[detect] ROM header from SD: %s\n", cached_path);
                            const char *bn = strrchr(cached_path, '/');
                            cartindex_update(info, bn ? bn + 1 : cached_path);
                        }
                    } else {
                        lprintf("[detect] ROM not cached — dump needed\n");
                        /* Use the live ROM header bytes for display (gametitles_lookup
                         * needs the CGB flag at hdr[0x143] to pick the right table). */
                        memcpy(dc_hdr, hdr_buf, 512);
                    }
                } else {
                    lprintf("[detect] ROM hdr read failed (no fresh fd)\n");
                }
                if (hdr_op) gbop_close(hdr_op);
            }
        }
    }
    memcpy(rom_hdr, dc_hdr, 512);
    make_display_title(info, rom_hdr, display_title, dsize);
    lprintf("[detect] %s (%s) ROM=%uKB RAM=%uKB\n",
            display_title, info->type_str, info->rom_size_kb, info->ram_size_kb);
    return 1;
}

// Quick cart poll (~1 s interval from menu loop). Returns 1 if detect should run.
// Sets *was_absent only after TWO consecutive "no cart / no device" reads (retry
// absorbs the alternating IOS USB failure pattern seen in family-B sessions where
// every other cart_info returns "Cart not detected" even with a cart inserted).
static int poll_cart(const CartInfo *cur_info, bool *was_absent) {
    GBOperatorHandle probe = gbop_reopen();
    if (!probe) {
        /* Retry once before declaring device lost */
        probe = gbop_reopen();
        if (!probe) {
            if (!*was_absent) { lprintf("[poll] device not found\n"); }
            *was_absent = true;
            return 0;
        }
    }
    CartInfo probe_info = {0};
    int rc = gbop_read_cart_info(probe, &probe_info);
    gbop_close(probe);

    if (rc != GBOP_OK || probe_info.rom_size_kb == 0) {
        /* GBOP_USB (-2): EP OUT stall or recv failure — device-side transient,
         *   NOT cart removal.  Device recovers after IOS cycles to a new fd.
         * GBOP_NOCART (-1): device responded with resp[3:5]==0 — potential removal;
         *   retry before declaring absent.
         * The alternate "no cart" protocol (response bytes in the ACK slot, data
         * chunks = zeros) is still GBOP_NOCART because gbop_bulk_recv returned 0. */
        if (rc == GBOP_USB) {
            lprintf("[poll] command stall — transient, skipping\n");
            return 0;
        }
        lprintf("[poll] cart_info fail — retrying\n");
        probe = gbop_reopen();
        int rc2 = GBOP_USB;
        if (probe) {
            memset(&probe_info, 0, sizeof(probe_info));
            rc2 = gbop_read_cart_info(probe, &probe_info);
            gbop_close(probe);
        }
        if (rc2 != GBOP_OK || probe_info.rom_size_kb == 0) {
            if (rc2 == GBOP_USB) {
                /* Retry stalled — couldn't confirm; wait for next fd cycle */
                lprintf("[poll] retry stalled — skipping\n");
                return 0;
            }
            if (!*was_absent) { lprintf("[poll] no cart (confirmed after retry)\n"); }
            *was_absent = true;
            return 0;
        }
        lprintf("[poll] retry ok — transient failure ignored\n");
    }

    /* Cart is present */
    if (*was_absent || memcmp(probe_info.raw_resp, cur_info->raw_resp, 60) != 0)
        return 1;  /* cart appeared or changed */
    return 0;
}

static void draw_dev_menu(const CartInfo *info, const char *title, int sel) {
    printf("\x1b[2J\x1b[H");
    printf("Wii GB Operator — Developer Menu\n");
    printf("=================================\n\n");
    if (info->rom_size_kb == 0 && !info->title[0]) {
        printf("Cart : No cart detected\n\n");
    } else {
        printf("Cart : %s\n", title[0] ? title : "(unknown)");
        printf("Type : %s\n", info->type_str);
        printf("ROM  : %u KB\n", info->rom_size_kb);
        printf("Save : %u KB\n\n", info->ram_size_kb);
    }
    printf("  %s Dump ROM    (%u KB)\n", sel == 0 ? ">" : " ", info->rom_size_kb);
    printf("  %s Dump Save   (%u KB)\n", sel == 1 ? ">" : " ", info->ram_size_kb);
    printf("  %s Upload Save (%u KB)\n", sel == 2 ? ">" : " ", info->ram_size_kb);
    printf("  %s Launch mGBA\n",         sel == 3 ? ">" : " ");
    printf("  %s Detect Cart\n",         sel == 4 ? ">" : " ");
    printf("  %s Commit Log\n",          sel == 5 ? ">" : " ");
    printf("  %s Back to Main Menu\n",   sel == 6 ? ">" : " ");
    printf("  %s Exit to Loader\n",      sel == 7 ? ">" : " ");
    printf("\nD-Pad / Stick: navigate   A: confirm\n");
}

// sel_persist: caller-owned cursor position — preserved across poll-triggered re-entries.
// Returns: >=0 = menu choice; -1 = reset pressed; -2 = poll interval elapsed.
static int run_dev_menu(const CartInfo *info, const char *title, int *sel_persist) {
    int sel = *sel_persist;
    int frame_cnt = 0;
    draw_dev_menu(info, title, sel);
    s8 psy = 0, pcy = 0;
    while (!g_reset_pressed) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        WPAD_ScanPads();
        if (++frame_cnt >= 60) {
            *sel_persist = sel;
            return -2;
        }
        u16 pressed  = PAD_ButtonsDown(0);
        u32 wpressed = WPAD_ButtonsDown(0);
        s8 sy = PAD_StickY(0), cy = PAD_SubStickY(0);
        bool sup = ((sy > STICK_THRESH || cy > STICK_THRESH) && !(psy > STICK_THRESH || pcy > STICK_THRESH));
        bool sdn = ((sy < -STICK_THRESH || cy < -STICK_THRESH) && !(psy < -STICK_THRESH || pcy < -STICK_THRESH));
        psy = sy; pcy = cy;
        if (((pressed & PAD_BUTTON_UP) || (wpressed & WPAD_BUTTON_UP) || sup) && sel > 0) {
            sel--;
            frame_cnt = 0;
            draw_dev_menu(info, title, sel);
        }
        if (((pressed & PAD_BUTTON_DOWN) || (wpressed & WPAD_BUTTON_DOWN) || sdn) && sel < DEV_MENU_ITEMS - 1) {
            sel++;
            frame_cnt = 0;
            draw_dev_menu(info, title, sel);
        }
        if ((pressed & PAD_BUTTON_A) || (wpressed & WPAD_BUTTON_A)) {
            *sel_persist = 0;
            return sel;
        }
        if ((pressed & PAD_BUTTON_B) || (wpressed & WPAD_BUTTON_B)) {
            *sel_persist = 0;
            return 6;  /* B = Back to Main Menu */
        }
    }
    *sel_persist = 0;
    return -1;
}

/* -----------------------------------------------------------------------
 * UI helpers
 * ---------------------------------------------------------------------- */

static void cprint(const char *s) {
    int len = (int)strlen(s);
    int pad = (80 - len) / 2;
    if (pad < 0) pad = 0;
    for (int i = 0; i < pad; i++) printf(" ");
    printf("%s\n", s);
}

static void wait_a(void) {
    while (!g_reset_pressed) {
        VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
        u16 p = PAD_ButtonsDown(0); u32 wp = WPAD_ButtonsDown(0);
        if ((p & PAD_BUTTON_A) || (wp & WPAD_BUTTON_A)) break;
    }
}

static void show_message(const char *msg) {
    printf("\x1b[2J\x1b[H\n\n");
    cprint(msg);
    printf("\n");
    cprint("Press A to confirm.");
    wait_a();
}

/* Returns 1 if user pressed A (yes), 0 if B (no). */
static int prompt_yesno(const char *msg) {
    printf("\x1b[2J\x1b[H\n\n");
    cprint(msg);
    printf("\n");
    cprint("A = Yes    B = No");
    while (!g_reset_pressed) {
        VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
        u16 p = PAD_ButtonsDown(0);
        if (p & PAD_BUTTON_A) return 1;
        if (p & PAD_BUTTON_B) return 0;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Save backup — copies current .sav to backups/TITLE_YYYYMMDD_HHMMSS.sav
 * ---------------------------------------------------------------------- */

static void backup_save(const CartInfo *info, const char *save_path) {
    mkdir(BACKUPS_DIR, 0755);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char safe[20] = {0};
    int j = 0;
    for (int i = 0; info->title[i] && j < 16; i++) {
        char c = info->title[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            safe[j++] = c;
        else if (c == ' ' && j > 0)
            safe[j++] = '_';
    }
    if (!safe[0]) strncpy(safe, "UNKNOWN", 7);

    char dst_path[256];
    snprintf(dst_path, sizeof(dst_path),
             "%s/%s_%04d%02d%02d_%02d%02d%02d.sav", BACKUPS_DIR, safe,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    FILE *src = fopen(save_path, "rb");
    if (!src) { lprintf("[backup] Cannot open %s\n", save_path); return; }
    fseek(src, 0, SEEK_END); long sz = ftell(src); rewind(src);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(src); lprintf("[backup] malloc failed\n"); return; }
    fread(buf, 1, (size_t)sz, src);
    fclose(src);

    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { free(buf); lprintf("[backup] Cannot write %s\n", dst_path); return; }
    fwrite(buf, 1, (size_t)sz, dst);
    fclose(dst);
    free(buf);
    lprintf("[backup] %s\n", dst_path);
}

/* -----------------------------------------------------------------------
 * Full-SD save file browser.
 * Starts in SAVES_DIR; B goes up; directories shown as [name].
 * Returns 0 on success (path_out filled), -1 on cancel.
 * ---------------------------------------------------------------------- */

#define BROWSE_FS_MAX 48

static int browse_save_file(char *path_out, size_t path_size) {
    static char item_names[BROWSE_FS_MAX][64];
    static char item_paths[BROWSE_FS_MAX][256];
    static int  item_isdir[BROWSE_FS_MAX];

    char cur_dir[256];
    strncpy(cur_dir, SAVES_DIR, sizeof(cur_dir) - 1);
    cur_dir[sizeof(cur_dir) - 1] = '\0';
    /* Fall back to SD root if saves dir doesn't exist yet */
    {
        DIR *td = opendir(cur_dir);
        if (!td) strncpy(cur_dir, "sd:/", sizeof(cur_dir) - 1);
        else closedir(td);
    }

    int sel = 0;

    while (!g_reset_pressed) {
        /* Scan directory */
        int cnt = 0;

        /* ".." entry if not at root */
        bool at_root = (strcmp(cur_dir, "sd:/") == 0 || strcmp(cur_dir, "sd:") == 0);
        if (!at_root) {
            strncpy(item_names[cnt], "..", 63);
            item_names[cnt][63] = '\0';
            strncpy(item_paths[cnt], cur_dir, 255);
            char *sl = strrchr(item_paths[cnt], '/');
            if (sl && sl > item_paths[cnt] + 3)
                *sl = '\0';
            else
                strncpy(item_paths[cnt], "sd:/", 255);
            item_isdir[cnt] = 1;
            cnt++;
        }

        DIR *d = opendir(cur_dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && cnt < BROWSE_FS_MAX) {
                if (ent->d_name[0] == '.') continue;
                char full[256];
                snprintf(full, sizeof(full), "%s/%s", cur_dir, ent->d_name);
                struct stat st;
                int isdir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
                if (!isdir) {
                    size_t nl = strlen(ent->d_name);
                    if (nl < 4 || strcasecmp(ent->d_name + nl - 4, ".sav") != 0)
                        continue;
                }
                if (isdir)
                    snprintf(item_names[cnt], 63, "[%s]", ent->d_name);
                else
                    strncpy(item_names[cnt], ent->d_name, 63);
                item_names[cnt][63] = '\0';
                strncpy(item_paths[cnt], full, 255);
                item_paths[cnt][255] = '\0';
                item_isdir[cnt] = isdir;
                cnt++;
            }
            closedir(d);
        }

        if (sel >= cnt && cnt > 0) sel = cnt - 1;
        if (cnt == 0) sel = 0;

        /* Draw */
        static char br_title[320];
        snprintf(br_title, sizeof(br_title), "Select Save File\n%s", cur_dir);
        const char *ptrs[BROWSE_FS_MAX];
        for (int i = 0; i < cnt; i++) ptrs[i] = item_names[i];
        draw_browser(br_title, ptrs, cnt, sel);

        /* Input loop — break on sel change (redraw) or dir change (rescan) */
        s8 psy = 0, pcy = 0;
        bool rescan = false;
        while (!rescan && !g_reset_pressed) {
            VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
            u16 pressed  = PAD_ButtonsDown(0);
            u32 wpressed = WPAD_ButtonsDown(0);
            s8 sy = PAD_StickY(0), cy = PAD_SubStickY(0);
            bool sup = ((sy > STICK_THRESH || cy > STICK_THRESH) && !(psy > STICK_THRESH || pcy > STICK_THRESH));
            bool sdn = ((sy < -STICK_THRESH || cy < -STICK_THRESH) && !(psy < -STICK_THRESH || pcy < -STICK_THRESH));
            psy = sy; pcy = cy;

            if (((pressed & PAD_BUTTON_UP) || (wpressed & WPAD_BUTTON_UP) || sup) && sel > 0) {
                sel--;
                draw_browser(br_title, ptrs, cnt, sel);
            }
            if (((pressed & PAD_BUTTON_DOWN) || (wpressed & WPAD_BUTTON_DOWN) || sdn) && sel < cnt - 1) {
                sel++;
                draw_browser(br_title, ptrs, cnt, sel);
            }
            if ((pressed & PAD_BUTTON_A) || (wpressed & WPAD_BUTTON_A)) {
                if (cnt == 0) return -1;
                if (item_isdir[sel]) {
                    strncpy(cur_dir, item_paths[sel], sizeof(cur_dir) - 1);
                    cur_dir[sizeof(cur_dir) - 1] = '\0';
                    sel = 0; rescan = true;
                } else {
                    strncpy(path_out, item_paths[sel], path_size - 1);
                    path_out[path_size - 1] = '\0';
                    return 0;
                }
            }
            if ((pressed & PAD_BUTTON_B) || (wpressed & WPAD_BUTTON_B)) {
                if (at_root) return -1;
                char *sl = strrchr(cur_dir, '/');
                if (sl && sl > cur_dir + 3)
                    *sl = '\0';
                else
                    strncpy(cur_dir, "sd:/", sizeof(cur_dir) - 1);
                sel = 0; rescan = true;
            }
        }
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * play_game: the primary user-facing launch flow.
 *   1. Check cart detected.
 *   2. Ensure ROM is on SD (offer install if not).
 *   3. Read save from cart; back it up.
 *   4. Boot mGBA.
 * ---------------------------------------------------------------------- */

static void play_game(CartInfo *info, uint8_t *rom_hdr,
                      char *display_title, size_t dsize) {
    if (info->rom_size_kb == 0) {
        show_message("No cart detected.");
        return;
    }

    /* Check ROM on SD */
    char rom_path[256] = {0};
    int rom_exists = rom_cache_exists(info, rom_path, sizeof(rom_path));

    if (!rom_exists) {
        if (!prompt_yesno("ROM not installed on SD card.\nInstall now?"))
            return;

        printf("\x1b[2J\x1b[H\n\n");
        cprint("Installing ROM — please wait...");
        printf("\n");

        GBOperatorHandle op = gbop_reopen();
        if (!op) { show_message("GB Operator not found. Check USB."); return; }

        int32_t dump_old_fd = gbop_get_fd(op);
        if (rom_cache_dump(op, info, rom_path, sizeof(rom_path)) != 0) {
            /* Dump failed — most likely the dump cmd stalled (-7005) because
             * the fd was spent by auto-detect's ROM header read + poll iterations.
             * The stall triggers IOS re-enumeration; wait for fresh fd and retry.
             * Auto-retry is safe: a partial file at offset 0 is just overwritten. */
            gbop_close(op); op = NULL;
            lprintf("[play] ROM install failed — waiting for fresh fd to retry\n");
            log_commit_sd();
            for (int i = 0; i < 50 && !op; i++) {
                usleep(60000);
                op = gbop_reopen();
                if (!op) continue;
                if (gbop_get_fd(op) == dump_old_fd) { gbop_close(op); op = NULL; }
            }
            if (op) {
                lprintf("[play] ROM install retry on fresh fd=%d\n",
                        (int)gbop_get_fd(op));
                gbop_close(op); op = NULL;
                log_commit_sd();
                op = gbop_reopen();
            }
            if (!op || rom_cache_dump(op, info, rom_path, sizeof(rom_path)) != 0) {
                if (op) gbop_close(op);
                show_message("ROM install failed. Returning to menu.");
                return;
            }
        }
        gbop_close(op); op = NULL;

        enrich_info_from_rom(rom_path, info);
        char new_path[256];
        build_rom_path_sd(info, new_path, sizeof(new_path));
        if (strcmp(rom_path, new_path) != 0 && rename(rom_path, new_path) == 0) {
            strncpy(rom_path, new_path, sizeof(rom_path));
            lprintf("[play] ROM renamed: %s\n", new_path);
        }
        const char *bn = strrchr(rom_path, '/');
        cartindex_update(info, bn ? bn + 1 : rom_path);
        make_display_title(info, rom_hdr, display_title, dsize);
        rom_exists = 1;
    }

    /* No save slot */
    if (info->ram_size_kb == 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "No save data for %s.", display_title);
        if (!prompt_yesno(msg))
            return;
        lprintf("[play] Launching (no save): %s\n", rom_path);
        mgba_run(info, rom_path, NULL, 0);
        log_commit_sd();
        return;
    }

    /* Read save from cart */
    printf("\x1b[2J\x1b[H\n\n");
    cprint("Reading save from cart...");
    fflush(stdout);

    GBOperatorHandle op = gbop_reopen();
    if (!op) { show_message("GB Operator not found. Check USB."); return; }

    // Cart_info (cmd 0x04) resets the device to idle state before the save read.
    // Also confirms the cart is still present before committing to the save read.
    // Retry on GBOP_NOCART: the device alternates good/bad responses on the same
    // fd — run_detect_cart_inner consumed the "good" turn immediately before this,
    // so play_game lands on the "bad" turn.  One retry on the same fd gets the
    // next "good" turn (test_113: alternating pattern confirmed deterministic).
    // GBOP_USB (tx=-7005) means the fd is spent from poll iterations — the
    // fresh-fd wait below will get a clean fd; cart presence is rechecked there
    // (test_118: first Play Game attempt stalled at cart_info, user had to retry).
    bool need_cart_recheck = false;
    {
        CartInfo live = {0};
        int ci_rc = gbop_read_cart_info(op, &live);
        if (ci_rc == GBOP_NOCART) {
            gbop_close(op); op = NULL;
            op = gbop_reopen();
            if (op) {
                memset(&live, 0, sizeof(live));
                ci_rc = gbop_read_cart_info(op, &live);
            }
        }
        if (ci_rc == GBOP_USB) {
            lprintf("[play] cart_info stall (fd spent) — retrying on fresh fd\n");
            need_cart_recheck = true;
            /* op still holds the stalled fd — fresh-fd wait uses it for old_fd */
        } else if (!op || ci_rc != 0 || live.rom_size_kb == 0) {
            if (op) { gbop_close(op); op = NULL; }
            show_message("Cart not detected. Check insertion.");
            return;
        }
    }

    // Refresh handle before save read: the current fd has been used for
    // boot probe, ROM header, polls, detect, and the cart_info check above.
    // After ~5 operations the device stalls EP OUT with tx=-7005 (test_114).
    // Standalone "Dump Save" always works because >1s of menu navigation passes
    // before it opens a handle, giving IOS time to allocate a fresh fd.
    // Solution: close now and poll until IOS allocates a new fd (~1s).
    {
        int32_t old_fd = gbop_get_fd(op);
        gbop_close(op); op = NULL;
        // Log BEFORE log_commit_sd so this line is included in the committed
        // FAT directory entry.  fclose takes ~200-500ms (SD sector writes), but
        // USB is closed here so IOS USB is idle — no stall risk.
        lprintf("[play] waiting for fresh fd (old=%d)\n", (int)old_fd);
        log_commit_sd();
        for (int i = 0; i < 50 && !op; i++) {
            usleep(60000);                     // 60ms/try → new fd after ~1-3s (~17-50 tries)
            op = gbop_reopen();
            if (!op) continue;
            if (gbop_get_fd(op) == old_fd) { gbop_close(op); op = NULL; }
        }
        if (!op) { show_message("GB Operator not found."); return; }
        lprintf("[play] fresh fd=%d\n", (int)gbop_get_fd(op));
        // Commit "fresh fd=M" before the save read.  Close→commit→reopen:
        // immediate reopen returns the same fd number at 0 ops (clean state).
        // fclose stalls IOS USB if open; closing first avoids the stall.
        gbop_close(op); op = NULL;
        log_commit_sd();
        op = gbop_reopen();
        if (!op) { show_message("GB Operator not found."); return; }
        lprintf("[play] save fd=%d\n", (int)gbop_get_fd(op));
    }

    if (need_cart_recheck) {
        CartInfo live2 = {0};
        lprintf("[play] cart recheck on fresh fd\n");
        if (gbop_read_cart_info(op, &live2) != 0 || live2.rom_size_kb == 0) {
            gbop_close(op); op = NULL;
            show_message("Cart not detected. Check insertion.");
            return;
        }
        lprintf("[play] cart recheck OK\n");
    }

    uint32_t save_bytes = info->ram_size_kb * 1024;
    uint8_t *savebuf = (uint8_t *)malloc(save_bytes);
    if (!savebuf) { gbop_close(op); show_message("Out of memory."); return; }

    int save_ok = (gbop_read_save(op, info, savebuf, save_bytes) == 0);
    gbop_close(op); op = NULL;

    if (!save_ok) {
        free(savebuf);
        show_message("Save dump failed. Returning to menu.");
        return;
    }

    /* Warn if save appears blank — scan full buffer.
     * GBA Flash saves (Ruby/Sapphire/Emerald) have section footers at 0xFF4
     * (byte 4084) so the first 64 bytes are legitimately zero even on a valid
     * save; only a completely zero buffer means the cart has never been saved. */
    {
        uint32_t chk = save_bytes;
        int blank = 1;
        for (uint32_t i = 0; i < chk; i++) {
            if (savebuf[i]) { blank = 0; break; }
        }
        if (blank) {
            lprintf("[play] Save appears blank (all %u bytes zero)\n", chk);
            if (!prompt_yesno("No save data found on cart.\nCart appears new or save was erased.\n\nStart a new game?")) {
                free(savebuf);
                return;
            }
        }
    }

    /* Write save to SD */
    mkdir(SAVES_DIR, 0755);
    char save_path[256] = {0};
    build_save_path(info, save_path, sizeof(save_path));
    FILE *sf = fopen(save_path, "wb");
    if (sf) {
        fwrite(savebuf, 1, save_bytes, sf);
        fclose(sf);
        lprintf("[play] Save → %s\n", save_path);
        backup_save(info, save_path);
    } else {
        lprintf("[play] WARNING: cannot write save to SD\n");
    }
    free(savebuf);

    lprintf("[play] Launching: rom=%s save=%s\n", rom_path, save_path);
    mgba_run(info, rom_path, sf ? save_path : NULL, info->ram_size_kb);
    log_commit_sd();
}

static int dump_save_to_sd(GBOperatorHandle op, const CartInfo *info,
                            const char *display_title) {
    if (info->ram_size_kb == 0) {
        lprintf("[save] Save size unknown — cannot dump\n");
        return -1;
    }

    uint32_t save_bytes = info->ram_size_kb * 1024;
    uint8_t *buf = (uint8_t *)malloc(save_bytes);
    if (!buf) {
        lprintf("[save] malloc(%u) failed\n", save_bytes);
        return -1;
    }

    printf("\x1b[2J\x1b[H");
    printf("Dumping save: %s (%u KB)\n", display_title, info->ram_size_kb);
    printf("-------------------------------\n\n");
    lprintf("[save] Reading %u KB save from cart...\n", info->ram_size_kb);
    if (gbop_read_save(op, info, buf, save_bytes) != 0) {
        lprintf("[save] Read failed\n");
        free(buf);
        return -1;
    }

    mkdir("sd:/apps/wii-gb-operator/saves", 0755);

    char path[256];
    build_save_path(info, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        lprintf("[save] Cannot open %s for writing\n", path);
        free(buf);
        return -1;
    }

    size_t written = fwrite(buf, 1, save_bytes, f);
    fclose(f);
    free(buf);

    if (written != save_bytes) {
        lprintf("[save] Write incomplete (%zu / %u bytes)\n", written, save_bytes);
        return -1;
    }

    lprintf("[save] Saved to %s\n", path);
    return 0;
}

// Loads a save file from SD into a newly malloc'd buffer.
// Caller must free(*buf_out) on success. Returns 0 on success, -1 on error.
// Handles the confirmation prompt so the user can cancel before USB is touched.
static int load_save_from_sd(const CartInfo *info, const char *display_title,
                              uint8_t **buf_out, uint32_t *size_out, char *path_out) {
    if (info->ram_size_kb == 0) {
        lprintf("[save] Save size unknown — cannot upload\n");
        return -1;
    }

    uint32_t save_bytes = info->ram_size_kb * 1024;
    build_save_path(info, path_out, 256);

    // Confirm before writing to cart (destructive operation)
    printf("\x1b[2J\x1b[H");
    printf("Upload Save to Cart\n");
    printf("-------------------\n\n");
    printf("File : %s\n", path_out);
    printf("Size : %u KB\n\n", info->ram_size_kb);
    printf("This will OVERWRITE the save on the\n");
    printf("cartridge. This cannot be undone.\n\n");
    printf("A = Confirm   B = Cancel\n");

    while (1) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        u16 pressed = PAD_ButtonsDown(0);
        if (pressed & PAD_BUTTON_B) {
            lprintf("[save] Upload cancelled by user\n");
            return -1;
        }
        if (pressed & PAD_BUTTON_A)
            break;
    }

    FILE *f = fopen(path_out, "rb");
    if (!f) {
        lprintf("[save] Cannot open %s — dump save first\n", path_out);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);
    if (file_size != (long)save_bytes) {
        lprintf("[save] File size mismatch: %ld bytes on SD, expected %u\n",
                file_size, save_bytes);
        fclose(f);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc(save_bytes);
    if (!buf) {
        lprintf("[save] malloc(%u) failed\n", save_bytes);
        fclose(f);
        return -1;
    }

    size_t read_bytes = fread(buf, 1, save_bytes, f);
    fclose(f);
    if (read_bytes != save_bytes) {
        lprintf("[save] File read incomplete (%zu / %u bytes)\n", read_bytes, save_bytes);
        free(buf);
        return -1;
    }

    lprintf("[save] Loaded %u bytes from %s\n", save_bytes, path_out);
    lprintf("[save] First 4 bytes: %02X %02X %02X %02X\n",
            buf[0], buf[1], buf[2], buf[3]);

    *buf_out  = buf;
    *size_out = save_bytes;
    return 0;
}

// Writes a pre-loaded save buffer to cart via USB.
static int write_save_to_cart(GBOperatorHandle op, const CartInfo *info,
                               const uint8_t *buf, uint32_t save_bytes,
                               const char *display_title, const char *path) {
    printf("\x1b[2J\x1b[H");
    printf("Uploading save: %s (%u KB)\n", display_title, info->ram_size_kb);
    printf("-------------------------------\n\n");
    lprintf("[save] Writing %u KB save to cart from %s\n", info->ram_size_kb, path);

    if (gbop_write_save(op, info, buf, save_bytes) != 0) {
        lprintf("[save] Write failed\n");
        return -1;
    }

    lprintf("[save] Uploaded from %s\n", path);
    return 0;
}

/* -----------------------------------------------------------------------
 * Frontend: Upload Save with file browser
 * ---------------------------------------------------------------------- */

static void upload_save_frontend(const CartInfo *info, const char *display_title) {
    if (info->ram_size_kb == 0) {
        show_message("This cart has no save slot.");
        return;
    }

    char save_path[256] = {0};
    if (browse_save_file(save_path, sizeof(save_path)) != 0)
        return;  /* cancelled */

    /* Confirmation */
    const char *filename = strrchr(save_path, '/');
    filename = filename ? filename + 1 : save_path;
    char msg[128];
    snprintf(msg, sizeof(msg), "Upload %s to cartridge?", filename);
    if (!prompt_yesno(msg)) return;

    /* Load file */
    uint32_t save_bytes = info->ram_size_kb * 1024;
    FILE *f = fopen(save_path, "rb");
    if (!f) { show_message("Cannot open file."); return; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f); rewind(f);
    if (fsz != (long)save_bytes) {
        fclose(f);
        char err[80];
        snprintf(err, sizeof(err), "File size mismatch (%ld vs %u bytes).", fsz, save_bytes);
        show_message(err);
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(save_bytes);
    if (!buf) { fclose(f); show_message("Out of memory."); return; }
    fread(buf, 1, save_bytes, f);
    fclose(f);

    /* Write */
    printf("\x1b[2J\x1b[H\n\n");
    cprint("Uploading save to cartridge...");
    printf("\n");

    GBOperatorHandle op = gbop_reopen();
    if (!op) { free(buf); show_message("GB Operator not found. Check USB."); return; }
    int ok = (write_save_to_cart(op, info, buf, save_bytes, display_title, save_path) == 0);
    gbop_close(op); op = NULL;
    free(buf);

    if (ok)
        show_message("Save uploaded successfully.");
    else
        show_message("Save upload failed.");
}

/* -----------------------------------------------------------------------
 * Frontend: main player-facing menu
 * ---------------------------------------------------------------------- */

static void draw_frontend(const CartInfo *info, const char *title,
                           int sel, int n_items, const char *items[]) {
    printf("\x1b[2J\x1b[H");
    printf("Wii gb operator\n");
    printf("===============\n\n");

    if (info->rom_size_kb == 0 && !info->title[0]) {
        printf("No cart detected\n");
    } else {
        printf("%s | %s | ROM: %u KB | Save: %u KB\n",
               title[0] ? title : "Unknown", info->type_str,
               info->rom_size_kb, info->ram_size_kb);
    }
    printf("\n");

    for (int i = 0; i < n_items; i++)
        printf("%s  %s\n", i == sel ? ">" : " ", items[i]);

    printf("\nD-Pad / Stick: navigate    A: select\n");
}

/* Returns: >=0 choice, -1 reset, -2 poll interval */
static int run_frontend(const CartInfo *info, const char *title,
                         int n_items, const char *items[], int *sel_persist) {
    int sel = *sel_persist;
    int frame_cnt = 0;
    draw_frontend(info, title, sel, n_items, items);
    s8 psy = 0, pcy = 0;
    while (!g_reset_pressed) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        WPAD_ScanPads();
        if (++frame_cnt >= 60) {
            *sel_persist = sel;
            return -2;
        }
        u16 pressed  = PAD_ButtonsDown(0);
        u32 wpressed = WPAD_ButtonsDown(0);
        s8 sy = PAD_StickY(0), cy = PAD_SubStickY(0);
        bool sup = ((sy > STICK_THRESH || cy > STICK_THRESH) && !(psy > STICK_THRESH || pcy > STICK_THRESH));
        bool sdn = ((sy < -STICK_THRESH || cy < -STICK_THRESH) && !(psy < -STICK_THRESH || pcy < -STICK_THRESH));
        psy = sy; pcy = cy;
        if (((pressed & PAD_BUTTON_UP)   || (wpressed & WPAD_BUTTON_UP)   || sup) && sel > 0) {
            sel--; frame_cnt = 0;
            draw_frontend(info, title, sel, n_items, items);
        }
        if (((pressed & PAD_BUTTON_DOWN) || (wpressed & WPAD_BUTTON_DOWN) || sdn) && sel < n_items - 1) {
            sel++; frame_cnt = 0;
            draw_frontend(info, title, sel, n_items, items);
        }
        if ((pressed & PAD_BUTTON_A) || (wpressed & WPAD_BUTTON_A)) {
            *sel_persist = 0;
            return sel;
        }
    }
    *sel_persist = 0;
    return -1;
}

static void init_video(void) {
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb   = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

int main(int argc, char **argv) {
    init_video();
    WPAD_Init();
    PAD_Init();
    SYS_SetResetCallback(main_on_reset);
    SYS_SetPowerCallback(main_on_power);

    printf("Wii GB Operator\n");
    printf("===============\n\n");
    printf("Starting up...\n");

    // Mount SD only — fatInitDefault also starts a USB mass storage driver
    // which can interfere with USB device enumeration for the GB Operator.
    // Retry up to 3 times in case the SD slot needs a moment after boot.
    int sd_ok = 0;
    for (int retry = 0; retry < 3 && !sd_ok; retry++) {
        if (retry > 0) { printf("SD retry %d...\n", retry); usleep(200000); }
        sd_ok = fatMountSimple("sd", &__io_wiisd);
    }
    if (!sd_ok) {
        printf("[WARN] SD mount failed — log and caching disabled\n");
    } else {
        printf("[OK]  SD mounted\n");
        /* Log rotation: find next available log.txt / log1.txt / log2.txt ... */
        char log_path[64];
        {
            FILE *t = fopen("sd:/apps/wii-gb-operator/log.txt", "r");
            if (!t) {
                strcpy(log_path, "sd:/apps/wii-gb-operator/log.txt");
            } else {
                fclose(t);
                int n;
                for (n = 1; n < 200; n++) {
                    snprintf(log_path, sizeof(log_path),
                             "sd:/apps/wii-gb-operator/log%d.txt", n);
                    FILE *t2 = fopen(log_path, "r");
                    if (!t2) break;
                    fclose(t2);
                }
            }
        }
        strncpy(g_log_path, log_path, sizeof(g_log_path) - 1);
        g_log = fopen(g_log_path, "w");
        if (g_log) printf("[OK]  Logging to %s\n", g_log_path);
        else printf("[WARN] Log file open failed\n");
    }

    settings_load();

    lprintf("\n===== SESSION START =====\n\n");
    lprintf("Wii GB Operator Test\n");
    lprintf("====================\n\n");

    bool cart_was_absent = false;
    CartInfo info;
    memset(&info, 0, sizeof(info));
    uint8_t rom_hdr[512];
    memset(rom_hdr, 0, sizeof(rom_hdr));
    char display_title[64];
    strncpy(display_title, "No cart", sizeof(display_title));
    display_title[sizeof(display_title) - 1] = '\0';

    GBOperatorHandle op = gbop_find();
    if (!op) {
        lprintf("[INFO] GB Operator not detected — entering menu (auto-detect will run on insertion)\n");
        cart_was_absent = true;
    } else {
        lprintf("[OK]  GB Operator found\n\n");
        if (gbop_read_cart_info(op, &info) != 0) {
            lprintf("[INFO] No cart at boot — entering menu\n");
            cart_was_absent = true;
            gbop_close(op); op = NULL;
        } else {
            // Mini-dump: close and immediately reopen before the ROM header read.
            // Issuing the ROM read on the warm cart-info handle gave garbage data for
            // GB/GBC carts (test_97). Fresh handle matches what gbop_dump_rom does.
            gbop_close(op); op = NULL;
            op = gbop_reopen();
            if (op) {
                if (gbop_read_rom_header(op, rom_hdr) == 0)
                    enrich_info_from_buf(&info, rom_hdr);
                gbop_close(op); op = NULL;
            }
            if (!try_enrich_info(&info))
                try_enrich_from_index(&info);
            make_display_title(&info, rom_hdr, display_title, sizeof(display_title));
        }
    }

    lprintf("Cart Info\n");
    lprintf("---------\n");
    lprintf("  Title    : %s\n",  display_title);
    lprintf("  Code     : %s\n",  info.game_code);
    lprintf("  Type     : %s\n",  info.type_str);
    lprintf("  ROM      : %u KB\n", info.rom_size_kb);
    lprintf("  RAM/Save : %u KB\n", info.ram_size_kb);
    lprintf("\n");
    log_force_flush(); /* fflush is sufficient; fclose+fopen here sets g_log=NULL on this hardware */

    /* op is NULL here — closed after boot cart_info. Re-opened per-operation in the loop below. */

    static const char *dev_menu_names[] = {
        "Dump ROM", "Dump Save", "Upload Save", "Launch mGBA",
        "Detect Cart", "Commit Log", "Back to Main Menu", "Exit to Loader"
    };
    int frontend_sel = 0;
    int dev_sel      = 0;
    bool in_dev_menu = false;

    while (!g_reset_pressed) {
        /* Re-establish callbacks (mgba_run clears reset callback on teardown) */
        SYS_SetResetCallback(main_on_reset);
        SYS_SetPowerCallback(main_on_power);
        g_reset_pressed = 0;

        /* Build frontend item list (dev_menu item optional) */
        static const char *fe_items[8];
        int n_fe = 0;
        fe_items[n_fe++] = "Play Game";
        fe_items[n_fe++] = "Dump ROM";
        fe_items[n_fe++] = "Dump Save";
        fe_items[n_fe++] = "Upload Save";
        fe_items[n_fe++] = "Detect Cart Swap";
        if (g_settings.dev_menu)
            fe_items[n_fe++] = "Developer Menu";
        fe_items[n_fe++] = "Exit to Loader";

        int choice = in_dev_menu
            ? run_dev_menu(&info, display_title, &dev_sel)
            : run_frontend(&info, display_title, n_fe, fe_items, &frontend_sel);

        if (choice == -1) break;   /* Reset pressed */

        if (choice == -2) {
            /* Poll interval elapsed — check cart presence.
             * All lprintf inside this block go to log only (not TV console). */
            g_log_suppress_console = 1;
            bool was_before = cart_was_absent;
            if (poll_cart(&info, &cart_was_absent)) {
                lprintf("[poll] Cart change — running auto-detect\n");
                if (run_detect_cart_inner(&info, rom_hdr,
                                           display_title, sizeof(display_title))) {
                    cart_was_absent = false;
                } else {
                    cart_was_absent = true;
                    memset(&info, 0, sizeof(info));
                    memset(rom_hdr, 0, sizeof(rom_hdr));
                    strncpy(display_title, "No cart", sizeof(display_title));
                    display_title[sizeof(display_title) - 1] = '\0';
                }
            } else if (!was_before && cart_was_absent) {
                /* Confirmed removal */
                info.rom_size_kb = 0; info.ram_size_kb = 0;
                info.title[0] = '\0'; info.type_str[0] = '\0';
                strncpy(display_title, "No cart", sizeof(display_title));
                display_title[sizeof(display_title) - 1] = '\0';
                lprintf("[poll] Cart removed\n");
            }
            g_log_suppress_console = 0;
            continue;
        }

        /* ================================================================
         * Developer menu choices
         * ================================================================ */
        if (in_dev_menu) {
            lprintf("[dev] %s\n", dev_menu_names[choice]);

            if (choice == 6) { in_dev_menu = false; continue; }  /* Back to Main Menu */

            if (choice == 7) {
                lprintf("[dev] Exiting to HBC\n");
                if (g_log) { fclose(g_log); g_log = NULL; }
                SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
            }

            if (choice == 4) {  /* Detect Cart */
                printf("\x1b[2J\x1b[H");
                printf("Detect Cart\n-----------\n\n");
                printf("Insert cart fully, then press A to scan.\n");
                printf("Allow a few seconds after inserting.\n\nA: Scan   B: Cancel\n");
                bool dc = false;
                while (!g_reset_pressed) {
                    VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                    u16 dp = PAD_ButtonsDown(0); u32 wp = WPAD_ButtonsDown(0);
                    if ((dp & PAD_BUTTON_B) || (wp & WPAD_BUTTON_B)) { dc = true; break; }
                    if ((dp & PAD_BUTTON_A) || (wp & WPAD_BUTTON_A)) break;
                }
                if (!dc) {
                    printf("Scanning...\n");
                    usleep(500000);
                    lprintf("[dev] Manual detect cart\n");
                    if (run_detect_cart_inner(&info, rom_hdr,
                                               display_title, sizeof(display_title))) {
                        cart_was_absent = false;
                        printf("\x1b[2J\x1b[H");
                        printf("Cart: %s\nType: %s  ROM: %u KB  Save: %u KB\n",
                               display_title, info.type_str, info.rom_size_kb, info.ram_size_kb);
                    } else {
                        cart_was_absent = true;
                        memset(&info, 0, sizeof(info)); memset(rom_hdr, 0, sizeof(rom_hdr));
                        strncpy(display_title, "No cart", sizeof(display_title));
                        display_title[sizeof(display_title)-1] = '\0';
                        printf("\x1b[2J\x1b[H");
                        printf("Could not read cart — check insertion.\n");
                    }
                    printf("\nPress A to return.\n");
                    while (!g_reset_pressed) {
                        VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                        if ((PAD_ButtonsDown(0) & PAD_BUTTON_A) ||
                            (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) break;
                    }
                }
                continue;
            }

            if (choice == 3) {  /* Launch mGBA */
                launch_mgba(&info);
                log_commit_sd();
                lprintf("[dev] Session log committed\n");
                continue;
            }

            if (choice == 5) {  /* Commit Log */
                lprintf("[dev] Committing log\n");
                log_commit_sd();
                if (!g_log && g_log_path[0]) {
                    g_log = fopen(g_log_path, "a");
                    if (!g_log) {
                        char alt[64];
                        snprintf(alt, sizeof(alt), "%s.new", g_log_path);
                        g_log = fopen(alt, "w");
                        if (g_log) strncpy(g_log_path, alt, sizeof(g_log_path) - 1);
                    }
                }
                lprintf("[dev] Log committed%s\n", g_log ? "" : " (WARNING: log NULL)");
                printf("\nLog committed to SD. Press A.\n");
                while (!g_reset_pressed) {
                    VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                    if ((PAD_ButtonsDown(0) & PAD_BUTTON_A) ||
                        (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) break;
                }
                continue;
            }

            /* choices 0-2: USB operations */
            uint8_t *upload_buf  = NULL;
            uint32_t upload_size = 0;
            char     upload_path[256] = {0};

            if (choice == 2) {
                if (load_save_from_sd(&info, display_title,
                                       &upload_buf, &upload_size, upload_path) != 0)
                    continue;
            }

            /* Commit log before opening USB — if the operation hangs we keep the
             * "[dev] Dump/Upload" line in the FAT directory entry. USB is closed here. */
            log_commit_sd();

            op = gbop_reopen();
            if (!op) {
                lprintf("[FAIL] GB Operator lost\n");
                if (upload_buf) { free(upload_buf); upload_buf = NULL; }
                printf("\nPress A to return.\n");
                while (!g_reset_pressed) {
                    VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                    if ((PAD_ButtonsDown(0) & PAD_BUTTON_A) ||
                        (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) break;
                }
                continue;
            }

            if (choice == 0) {
                char rom_path[256] = {0};
                if (info.rom_size_kb == 0) {
                    lprintf("[WARN] ROM size unknown\n");
                } else if (rom_cache_exists(&info, rom_path, sizeof(rom_path))) {
                    lprintf("[OK]  ROM cached: %s\n", rom_path);
                } else {
                    lprintf("[INFO] Dumping ROM...\n");
                    if (rom_cache_dump(op, &info, rom_path, sizeof(rom_path)) == 0) {
                        lprintf("[OK]  Saved: %s\n", rom_path);
                        enrich_info_from_rom(rom_path, &info);
                        char new_path[256];
                        build_rom_path_sd(&info, new_path, sizeof(new_path));
                        if (strcmp(rom_path, new_path) != 0 && rename(rom_path, new_path) == 0) {
                            strncpy(rom_path, new_path, sizeof(rom_path));
                            lprintf("[info] ROM renamed: %s\n", new_path);
                        }
                        const char *bn = strrchr(rom_path, '/');
                        cartindex_update(&info, bn ? bn + 1 : rom_path);
                        make_display_title(&info, rom_hdr, display_title, sizeof(display_title));
                    } else {
                        lprintf("[FAIL] ROM dump failed\n");
                    }
                }
            } else if (choice == 1) {
                if (dump_save_to_sd(op, &info, display_title) == 0)
                    lprintf("[OK]  Save dump complete\n");
                else
                    lprintf("[FAIL] Save dump failed\n");
            } else {
                if (write_save_to_cart(op, &info, upload_buf, upload_size,
                                        display_title, upload_path) == 0) {
                    lprintf("[OK]  Save upload complete\n");
                    gbop_close(op); op = NULL;
                    uint32_t vd = (info.type == CART_TYPE_GBA) ? 30000000 : 200000;
                    lprintf("[verify] Waiting %u ms\n", vd / 1000);
                    usleep(vd);
                    op = gbop_reopen();
                    if (op) {
                        uint32_t vs = info.ram_size_kb * 1024;
                        uint8_t *vb = (uint8_t *)malloc(vs);
                        if (vb) {
                            lprintf("[verify] Reading back %u KB\n", info.ram_size_kb);
                            if (gbop_read_save(op, &info, vb, vs) == 0) {
                                lprintf("[verify] Flash[0..15]: "
                                        "%02X %02X %02X %02X %02X %02X %02X %02X "
                                        "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                                        vb[0],vb[1],vb[2],vb[3],vb[4],vb[5],vb[6],vb[7],
                                        vb[8],vb[9],vb[10],vb[11],vb[12],vb[13],vb[14],vb[15]);
                                int match = 1; uint32_t fd2 = 0;
                                for (uint32_t i = 0; i < vs; i++) {
                                    if (vb[i] != upload_buf[i]) { match = 0; fd2 = i; break; }
                                }
                                if (!match)
                                    lprintf("[verify] First diff 0x%04X: Flash=0x%02X src=0x%02X\n",
                                            fd2, vb[fd2], upload_buf[fd2]);
                                lprintf("[verify] %u KB: %s\n",
                                        info.ram_size_kb, match ? "MATCH" : "MISMATCH");
                            } else {
                                lprintf("[verify] Read-back failed\n");
                            }
                            free(vb);
                        }
                    }
                } else {
                    lprintf("[FAIL] Save upload failed\n");
                }
                free(upload_buf); upload_buf = NULL;
            }

            if (op) { gbop_close(op); op = NULL; }
            printf("\nPress A to return.\n");
            while (!g_reset_pressed) {
                VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                if ((PAD_ButtonsDown(0) & PAD_BUTTON_A) ||
                    (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) break;
            }
            continue;
        }

        /* ================================================================
         * Frontend choices
         * ================================================================ */
        lprintf("[fe] %s\n", fe_items[choice]);

        /* Exit to Loader (always last item) */
        if (choice == n_fe - 1) {
            lprintf("[fe] Exiting to HBC\n");
            if (g_log) { fclose(g_log); g_log = NULL; }
            SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
        }

        /* Developer Menu (second-to-last when dev_menu=1) */
        if (g_settings.dev_menu && choice == n_fe - 2) {
            in_dev_menu = true; dev_sel = 0; continue;
        }

        /* Play Game (0) */
        if (choice == 0) {
            play_game(&info, rom_hdr, display_title, sizeof(display_title));
            continue;
        }

        /* Upload Save (3) — file browser version */
        if (choice == 3) {
            upload_save_frontend(&info, display_title);
            continue;
        }

        /* Detect Cart Swap (4) */
        if (choice == 4) {
            printf("\x1b[2J\x1b[H\n\n");
            cprint("Detect Cart Swap");
            printf("\n");
            cprint("Insert cart fully, then press A to scan.");
            printf("\n");
            cprint("A: Scan   B: Cancel");
            bool dc = false;
            while (!g_reset_pressed) {
                VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                u16 dp = PAD_ButtonsDown(0); u32 wp = WPAD_ButtonsDown(0);
                if ((dp & PAD_BUTTON_B) || (wp & WPAD_BUTTON_B)) { dc = true; break; }
                if ((dp & PAD_BUTTON_A) || (wp & WPAD_BUTTON_A)) break;
            }
            if (!dc) {
                cprint("Scanning...");
                usleep(500000);
                lprintf("[fe] Manual detect cart\n");
                if (run_detect_cart_inner(&info, rom_hdr,
                                           display_title, sizeof(display_title))) {
                    cart_was_absent = false;
                    printf("\x1b[2J\x1b[H\n\n");
                    char dl[80];
                    snprintf(dl, sizeof(dl), "Cart detected: %s", display_title);
                    cprint(dl);
                    snprintf(dl, sizeof(dl), "%s  |  ROM: %u KB  |  Save: %u KB",
                             info.type_str, info.rom_size_kb, info.ram_size_kb);
                    cprint(dl);
                } else {
                    cart_was_absent = true;
                    memset(&info, 0, sizeof(info)); memset(rom_hdr, 0, sizeof(rom_hdr));
                    strncpy(display_title, "No cart", sizeof(display_title));
                    display_title[sizeof(display_title)-1] = '\0';
                    printf("\x1b[2J\x1b[H\n\n");
                    cprint("Could not read cart. Check insertion.");
                }
                printf("\n"); cprint("Press A to return.");
                while (!g_reset_pressed) {
                    VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                    if ((PAD_ButtonsDown(0) & PAD_BUTTON_A) ||
                        (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) break;
                }
            }
            continue;
        }

        /* Dump ROM (1) or Dump Save (2) */
        op = gbop_reopen();
        if (!op) {
            printf("\x1b[2J\x1b[H\n\n");
            cprint("GB Operator not found. Check USB connection.");
            printf("\n"); cprint("Press A to return.");
            wait_a();
            continue;
        }

        if (choice == 1) {  /* Dump ROM */
            char rom_path[256] = {0};
            if (info.rom_size_kb == 0) {
                lprintf("[WARN] No cart\n");
                gbop_close(op); op = NULL;
                show_message("No cart detected.");
                continue;
            }
            if (rom_cache_exists(&info, rom_path, sizeof(rom_path))) {
                lprintf("[OK]  ROM cached: %s\n", rom_path);
                gbop_close(op); op = NULL;
                show_message("ROM already on SD card.");
                continue;
            }
            lprintf("[INFO] Dumping ROM...\n");
            if (rom_cache_dump(op, &info, rom_path, sizeof(rom_path)) == 0) {
                lprintf("[OK]  Saved: %s\n", rom_path);
                enrich_info_from_rom(rom_path, &info);
                char new_path[256];
                build_rom_path_sd(&info, new_path, sizeof(new_path));
                if (strcmp(rom_path, new_path) != 0 && rename(rom_path, new_path) == 0) {
                    strncpy(rom_path, new_path, sizeof(rom_path));
                    lprintf("[info] ROM renamed: %s\n", new_path);
                }
                const char *bn = strrchr(rom_path, '/');
                cartindex_update(&info, bn ? bn + 1 : rom_path);
                make_display_title(&info, rom_hdr, display_title, sizeof(display_title));
                gbop_close(op); op = NULL;
                show_message("ROM dump complete.");
            } else {
                lprintf("[FAIL] ROM dump failed\n");
                gbop_close(op); op = NULL;
                show_message("ROM dump failed.");
            }
        } else {  /* Dump Save (choice == 2) */
            if (dump_save_to_sd(op, &info, display_title) == 0) {
                lprintf("[OK]  Save dump complete\n");
                char save_path[256];
                build_save_path(&info, save_path, sizeof(save_path));
                backup_save(&info, save_path);
                gbop_close(op); op = NULL;
                show_message("Save dump complete.");
            } else {
                lprintf("[FAIL] Save dump failed\n");
                gbop_close(op); op = NULL;
                show_message("Save dump failed. Returning to menu.");
            }
        }
        if (op) { gbop_close(op); op = NULL; }
    }

    SYS_SetResetCallback(main_on_reset);
    SYS_SetPowerCallback(main_on_power);
    lprintf("\nPress START (GC) or A (Wiimote) to exit.\n");
    if (g_log) { fclose(g_log); g_log = NULL; }
    while (!g_reset_pressed) {
        PAD_ScanPads();
        WPAD_ScanPads();
        VIDEO_WaitVSync();
        if (PAD_ButtonsHeld(0) & PAD_BUTTON_START) break;
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_A) break;
    }
    return 0;
}
