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
#include <ogc/usbstorage.h>
#include <ogc/usb.h>
#include "log.h"
#include "gb_operator.h"
#include "rom_cache.h"
#include "rom_reconcile.h"
#include "mgba_frontend.h"
#include "settings.h"
#include "cartindex.h"
#include "gametitles.h"
#include "rom_sizes.h"

void *xfb   = NULL;
GXRModeObj *rmode = NULL;

FILE *g_log = NULL;
char  g_log_path[64] = {0};
int   g_log_suppress_console = 0; /* set to 1 during GX emulation */
u64   g_log_t0 = 0; /* gettime() at session start — see log.h's lprintf() timestamp prefix */

/* Detected at startup — whichever drive (sd: or usb:) has the app folder. */
char g_app_root[32] = "sd:/apps/wii-gb-operator";

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


#define DEV_MENU_ITEMS 12

#define STICK_THRESH 40

/* Phase 1 drive detection — SD probe only, no USB mount.
 * USB mass storage (fatMountSimple "usb") must NOT be called here because the
 * USB mass storage driver shares the OH0 USB host controller with the GB Operator.
 * Starting it before gbop_find() prevents GB Operator enumeration.
 * Returns true if SD has the app dir; false means Phase 2 in main() will try USB. */
static bool detect_app_drive(void) {
    DIR *d = opendir("sd:/apps/wii-gb-operator");
    if (d) {
        closedir(d);
        snprintf(g_app_root, sizeof(g_app_root), "sd:/apps/wii-gb-operator");
        printf("[OK]  App drive: SD card\n");
        return true;
    }
    /* usb: is not mounted yet; this succeeds only if some earlier layer (HBC in
     * rare configs) already made it accessible. USB mount is deferred to Phase 2. */
    d = opendir("usb:/apps/wii-gb-operator");
    if (d) {
        closedir(d);
        snprintf(g_app_root, sizeof(g_app_root), "usb:/apps/wii-gb-operator");
        printf("[OK]  App drive: USB storage (pre-mounted)\n");
        return false;
    }
    snprintf(g_app_root, sizeof(g_app_root), "sd:/apps/wii-gb-operator");
    printf("[INFO] App dir not on SD — USB will be checked after GB Operator init\n");
    return false;
}

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

    // New-firmware cart-info doesn't carry a usable size field (see CLAUDE.md
    // "resp[26..29] is a fixed constant" finding), so rom_size_kb/ram_size_kb
    // are left at 0 by the new cart-info parse. Fill them in here from the
    // known-game table now that the header (and therefore game code) is
    // available; falls back to a generous safe-max guess for unrecognized
    // games so rom_cache.c's total-based loop doesn't stop before the
    // device's real end-of-stream footer arrives (see rom_sizes.h and the
    // "Do Not: Refactor rom_cache.c" entry for why oversizing is the safe
    // direction to guess wrong in, not undersizing).
    if (!g_settings.use_old_firmware) {
        if (info->rom_size_kb == 0) {
            uint32_t rom_kb = rom_sizes_lookup_rom_kb(info->type, hdr);
            info->rom_size_kb = rom_kb ? rom_kb
                               : (info->type == CART_TYPE_GBA ? 32768 : 8192);
            info->rom_size_confirmed = rom_kb ? 1 : 0;
        }
        if (info->ram_size_kb == 0) {
            uint32_t ram_kb = rom_sizes_lookup_ram_kb(info->type, hdr);
            info->ram_size_kb = ram_kb ? ram_kb : 128;
            info->ram_size_confirmed = ram_kb ? 1 : 0;
        }
    }

    lprintf("[hdr] Enriched: type=%s title=\"%s\" code=\"%s\" rom=%uKB ram=%uKB\n",
            info->type_str, info->title, info->game_code,
            info->rom_size_kb, info->ram_size_kb);
}

// A cart-info read is a failure if the transport/parse itself failed (rc !=
// GBOP_OK — true under either firmware), OR, on the OLD-firmware path only,
// if rom_size_kb came back 0 (that protocol always populates size on a real
// success, so 0 there means something went wrong even though rc looked ok).
// On the NEW-firmware path rom_size_kb is legitimately 0 straight out of
// cart-info (see rom_sizes.h) and is filled in later from the ROM header,
// so checking it here would make poll_cart never recognize a cart as
// present at all.
static bool cart_info_failed(int rc, const CartInfo *info) {
    if (rc != GBOP_OK) return true;
    if (g_settings.use_old_firmware && info->rom_size_kb == 0) return true;
    return false;
}

// A bare gbop_read_cart_info() can only ever report CART_TYPE_GB or
// CART_TYPE_GBA (resp[2]==0x20 vs 0x30) — it never sets CART_TYPE_GBC, since
// that distinction (the CGB flag at ROM header offset 0x143) requires an
// actual header read, which this cheap check deliberately doesn't do. So
// CART_TYPE_GB and CART_TYPE_GBC must be treated as the same broad family
// when comparing a quick cart-info result against a fully-detected info's
// type — comparing them directly would make a bare CART_TYPE_GB reading
// "mismatch" against a properly-detected CART_TYPE_GBC for the exact same
// physical cart, 100% of the time, for every single CGB game. Confirmed as
// a real, reproduced infinite loop on hardware (Save Read Write Test/
// test_7): the play-vs-cart type-mismatch check kept "detecting" a mismatch
// against a Pokemon Gold cart, re-running a full detect (which correctly
// re-confirmed GBC), recursing back into play_game(), hitting the exact same
// bare cart-info check again, and repeating — dozens of times in one session.
static bool cart_type_family_mismatch(CartType quick, CartType full) {
    bool quick_is_gb_family = (quick == CART_TYPE_GB || quick == CART_TYPE_GBC);
    bool full_is_gb_family  = (full  == CART_TYPE_GB || full  == CART_TYPE_GBC);
    return quick_is_gb_family != full_is_gb_family;
}

// Mirrors rom_cache.c's static build_path (duplicated since it's static there).
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
    char roms_dir[64];
    snprintf(roms_dir, sizeof(roms_dir), "%s/roms", g_app_root);
    snprintf(out, size, "%s/%s_%s.%s", roms_dir, safe, info->game_code, ext);
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

    /* Validate: file exists and is non-empty.
     *
     * Previously compared the file's real size against info->rom_size_kb *
     * 1024 — but on a genuinely fresh detect (not the same_cart fast path
     * in run_detect_cart_inner, which already ran enrich_info_from_buf
     * beforehand) info->rom_size_kb is legitimately still 0 at this point:
     * the new-firmware cart-info response never carries a usable size field
     * (see rom_sizes.h), so nothing has set it yet. Comparing against 0
     * meant EVERY entry always looked like a "size mismatch" and this
     * cartindex fast-path could never fire except on the exact same cart as
     * the previous poll — forcing a live, ~5-20%-per-attempt-reliable USB
     * ROM header read on every other fresh detect even when cartindex
     * already had the fingerprint and the ROM was already sitting on SD,
     * correctly identified. Confirmed on hardware (Post Firmware Update
     * Test/test_30): this is the direct cause of "cart detect takes a lot
     * of tries" for anything other than back-to-back polls of the same
     * cart. A real ROM file is never 0 bytes, so requiring only that is
     * still a meaningful sanity check without depending on a value that
     * isn't known yet. */
    CartIndexEntry valid[8];
    char valid_paths[8][256];
    long valid_sizes[8];
    int valid_n = 0;
    for (int i = 0; i < n && valid_n < 8; i++) {
        char path[256];
        char roms_dir[64];
        snprintf(roms_dir, sizeof(roms_dir), "%s/roms", g_app_root);
        snprintf(path, sizeof(path), "%s/%s", roms_dir, entries[i].rom_basename);
        FILE *f = fopen(path, "rb");
        if (!f) {
            const char *alt = (g_app_root[0] == 'u')
                            ? "sd:/apps/wii-gb-operator/roms"
                            : "usb:/apps/wii-gb-operator/roms";
            snprintf(path, sizeof(path), "%s/%s", alt, entries[i].rom_basename);
            f = fopen(path, "rb");
        }
        if (!f) { lprintf("[index] Entry missing: %s\n", entries[i].rom_basename); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz <= 0) {
            lprintf("[index] Entry file is empty: %s\n", entries[i].rom_basename);
            continue;
        }
        valid[valid_n] = entries[i];
        strncpy(valid_paths[valid_n], path, sizeof(valid_paths[0]) - 1);
        valid_paths[valid_n][sizeof(valid_paths[0]) - 1] = '\0';
        valid_sizes[valid_n] = sz;
        valid_n++;
    }
    if (valid_n == 0) {
        // No entry's ROM file exists on SD yet — but per the 2026-07-31
        // change, cartindex now also records identity (type/game_code/
        // title/rom_size_kb/ram_size_kb) at first successful DETECT, not
        // just first successful DUMP, specifically so this case (a cart
        // seen before but never dumped) doesn't have to fall back to a
        // fresh, slow ROM-header read. Use the first identity_known match
        // directly — no file needed. (If multiple never-dumped entries
        // share this exact fingerprint, picking the first is an accepted
        // imperfection: there's no file to preview for a real disambiguation
        // UI in this state, and this is a rare corner of a rare corner.)
        for (int i = 0; i < n; i++) {
            if (!entries[i].identity_known) continue;
            info->type = entries[i].type;
            strncpy(info->type_str, entries[i].type_str, sizeof(info->type_str) - 1);
            strncpy(info->game_code, entries[i].game_code, sizeof(info->game_code) - 1);
            strncpy(info->title, entries[i].title, sizeof(info->title) - 1);
            info->rom_size_kb = entries[i].rom_size_kb;
            info->ram_size_kb = entries[i].ram_size_kb;
            lprintf("[index] Enriched from index (not yet dumped): title=\"%s\" code=\"%s\" "
                    "rom=%uKB ram=%uKB\n", info->title, info->game_code,
                    info->rom_size_kb, info->ram_size_kb);
            return 1;
        }
        return 0;
    }

    int sel = 0;
    if (valid_n > 1)
        sel = select_from_matches(info->type, valid, valid_paths, valid_n);
    if (sel < 0) return 0;

    enrich_info_from_rom(valid_paths[sel], info);

    // enrich_info_from_rom() only fills title/game_code — rom_size_kb/
    // ram_size_kb are still 0 otherwise, which would make the downstream
    // rom_cache_exists() check in run_detect_cart_inner() fail (it requires
    // rom_size_kb > 0), defeating the whole point of this fast path by
    // falling through to a live header read anyway. Fill them the same way
    // a real header read would, from the header bytes of the very file just
    // matched — falling back to the file's own real byte size (ground
    // truth for a ROM we've already dumped) if rom_sizes.c has no table
    // entry for it.
    if (info->rom_size_kb == 0) {
        FILE *f = fopen(valid_paths[sel], "rb");
        if (f) {
            uint8_t hdr[512] = {0};
            fread(hdr, 1, sizeof(hdr), f);
            fclose(f);
            enrich_info_from_buf(info, hdr);
        }
        // Prefer the matched file's own real size over rom_sizes.c's
        // generous unrecognized-game fallback guess: for a ROM we've
        // already dumped and matched by fingerprint, the file's actual
        // byte count is ground truth — strictly better than a guess that
        // only exists to avoid stopping a live USB stream too early.
        if (!info->rom_size_confirmed) {
            info->rom_size_kb = (uint32_t)(valid_sizes[sel] / 1024);
        }
    }

    lprintf("[index] Enriched from index: title=\"%s\" code=\"%s\" rom=%uKB ram=%uKB\n",
            info->title, info->game_code, info->rom_size_kb, info->ram_size_kb);
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
        snprintf(path, path_size, "%s/saves/%s_%s.sav", g_app_root, safe, info->game_code);
    else
        snprintf(path, path_size, "%s/saves/%s.sav", g_app_root, safe);
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
    char roms_dir[64], saves_dir[64];
    snprintf(roms_dir,  sizeof(roms_dir),  "%s/roms",  g_app_root);
    snprintf(saves_dir, sizeof(saves_dir), "%s/saves", g_app_root);
    int rom_cnt = 0;
    {
        static char rn_gb[BROWSER_MAX][64], rp_gb[BROWSER_MAX][256];
        static char rn_gbc[BROWSER_MAX][64], rp_gbc[BROWSER_MAX][256];
        static char rn_gba[BROWSER_MAX][64], rp_gba[BROWSER_MAX][256];
        int n_gb  = scan_dir(roms_dir, ".gb",  rn_gb,  rp_gb);
        int n_gbc = scan_dir(roms_dir, ".gbc", rn_gbc, rp_gbc);
        int n_gba = scan_dir(roms_dir, ".gba", rn_gba, rp_gba);
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
    lprintf("[mgba] Found %d ROMs in %s\n", rom_cnt, roms_dir);

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
        int n = scan_dir(saves_dir, ".sav", sn, sp);
        for (int i = 0; i < n && sav_cnt <= BROWSER_MAX; i++, sav_cnt++) {
            strncpy(sav_names[sav_cnt], sn[i], 63);
            strncpy(sav_paths[sav_cnt], sp[i], 255);
        }
    }
    for (int i = 0; i < sav_cnt; i++) sav_items[i] = sav_names[i];
    lprintf("[mgba] Found %d saves in %s\n", sav_cnt - 1, saves_dir);

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

// Core cart detection: retry cart_info (up to GBOP_DETECT_CART_ATTEMPTS
// attempts), then run mini-dump if the cart changed. No UI — prompts/waits
// are the caller's responsibility. Updates *info, rom_hdr[512], display_title
// on success. Returns 1 on success, 0 on failure.
//
// Attempt count originally sized from the USB probe's cart-info reliability
// sweeps (Post Firmware Update Test/test_6, test_7): measured per-attempt
// success rate on the new-firmware protocol has ranged 5-20% across
// sessions on the same hardware/cart pair that Epilogue Playback reads
// flawlessly, i.e. this is real connection-level unreliability, not a
// parsing bug (see CLAUDE.md). Raised again (Rom Stitching Test/test_8-9,
// 2026-07-31) after directly measuring the same connection's per-attempt
// system-side cost during Continuation Test's automatic retry loop: a
// failed attempt typically resolves in 5-12ms, but ~76% of attempts hit an
// inherent ~800ms IOS/USB-stack latency spike unrelated to anything this
// code does (confirmed: the same spike rate occurs even in a path with zero
// per-attempt SD I/O) — meaning the real, sustainable budget for a
// user-initiated wait is much larger than 20 was ever set to, without the
// per-attempt cost assumption that originally justified stopping there.
// Not raised to Continuation Test's own 200, since this function is also
// used for passive/automatic detection (boot, background) where a cart
// being genuinely absent — a common, ordinary state — must still resolve
// in a reasonable time; X+Y hold aborts early for a user who doesn't want
// to wait out the full budget, same pattern as Continuation Test.
#define GBOP_DETECT_CART_ATTEMPTS 60

// Shared abort check for the retry loops below — same X+Y-hold convention
// already used by rom_cache_stream_chunks() and dump_rom_new_protocol(),
// so a user who doesn't want to wait out a large retry budget always has
// the same way out regardless of which operation is running.
static bool xy_abort_held(void) {
    PAD_ScanPads();
    return (PAD_ButtonsHeld(0) & (PAD_BUTTON_X | PAD_BUTTON_Y)) == (PAD_BUTTON_X | PAD_BUTTON_Y);
}

// UI-only progress tick for the cart-info/header-read retry loops — a plain
// printf (not lprintf), so it stays visible even while g_log_suppress_console
// is set (see run_detect_cart_inner()'s wrapper below). Added 2026-07-31
// after direct hardware feedback (Detect Cart Test/test_4): with console
// output unsuppressed, a slow detect scrolled raw protocol debug text
// ("cmd=0x00 tx=64", marker/checksum lines) across the whole screen instead
// of showing anything a player would recognize as "still working" — and per-
// attempt latency here is dominated by an inherent ~800ms-1.5s IOS/USB round
// trip this project has already measured and cannot reduce from software
// (see CLAUDE.md "Wireshark timing analysis"), so a slow detect is expected
// behavior on a bad connection, not a hang — this just makes that visible
// instead of either silent or spammy.
static void detect_progress_dot(void *ctx) {
    (void)ctx;
    printf(".");
    fflush(stdout);
}

// Live ROM dump progress display — two lines, redrawn in place via absolute
// cursor positioning (not appended, so a multi-minute dump never scrolls the
// screen). row/dots/bar_shown are caller-owned state (DumpProgressState),
// set up once before the dump starts: row = the screen row the ticking
// "in progress" line occupies; the bar itself is drawn on row+1.
//
// Two callbacks share this state, firing at different times — added
// 2026-07-31 after direct user feedback that the original single-callback
// version showed nothing at all (not even a dot) for however long it took
// to reach a cycle that actually passed its marker+front checks, since
// gbop_dump_rom_continuation()'s byte-progress callback only ever fires
// once real streaming has already started. Given most cycles across a
// session fail before that point (see the write-stall analysis a few turns
// earlier), that could leave the screen looking hung for a long time with
// no indication anything was being attempted:
//   - dump_cycle_tick (GbopProgressCB): fires on every cycle ATTEMPT,
//     success or failure — ticks the dots immediately, so "attempts are
//     being made" is visible from the very first try, well before any
//     bytes have ever streamed.
//   - draw_dump_progress (GbopByteProgressCB): fires only once a cycle has
//     started real streaming (every 512KB thereafter) — draws the bar for
//     the first time at that point (bar_shown latches so it doesn't get
//     re-drawn from scratch/flicker on every call), per the explicit
//     request that the bar should only appear once real progress exists,
//     not before.
typedef struct {
    int row;
    int dots;
    int bar_shown;
} DumpProgressState;

#define DUMP_PROGRESS_BAR_WIDTH 20

static void dump_cycle_tick(void *ctx) {
    DumpProgressState *st = (DumpProgressState *)ctx;
    st->dots++;
    if (st->dots > 20) st->dots = 1; // wrap so the ticker line can't grow forever on a very long dump

    char dots_str[DUMP_PROGRESS_BAR_WIDTH * 2 + 1];
    int n = 0;
    for (int i = 0; i < st->dots && n + 2 < (int)sizeof(dots_str); i++) {
        dots_str[n++] = '.';
        dots_str[n++] = ' ';
    }
    dots_str[n] = '\0';
    printf("\x1b[%d;1HRom dump in progress %-42s\n", st->row, dots_str);
    fflush(stdout);
}

static void draw_dump_progress(uint32_t given, uint32_t total, void *ctx) {
    DumpProgressState *st = (DumpProgressState *)ctx;
    st->bar_shown = 1;

    int filled = total > 0 ? (int)((uint64_t)given * DUMP_PROGRESS_BAR_WIDTH / total) : 0;
    if (filled > DUMP_PROGRESS_BAR_WIDTH) filled = DUMP_PROGRESS_BAR_WIDTH;
    if (filled < 0) filled = 0;
    char bar[DUMP_PROGRESS_BAR_WIDTH + 1];
    for (int i = 0; i < DUMP_PROGRESS_BAR_WIDTH; i++) bar[i] = (i < filled) ? '@' : '_';
    bar[DUMP_PROGRESS_BAR_WIDTH] = '\0';

    // Trailing spaces pad over any leftover characters from a longer
    // previous line (the KB numerator only ever grows during a dump, but
    // this is cheap insurance regardless).
    printf("\x1b[%d;1H[%s] %u/%u KB          \n", st->row + 1, bar, given / 1024, total / 1024);
    fflush(stdout);
}

// Same ticking-dots mechanism as dump_cycle_tick, for the save read/write
// retry loop — a save is at most 128KB (GBA Flash) and transfers in well
// under a second once an attempt's marker actually matches, so there's no
// equivalent to draw_dump_progress()'s byte-count bar here, just the "an
// attempt is being made" ticker. label distinguishes "Dumping save" (read)
// from "Uploading save" (write) while reusing one small helper.
typedef struct {
    int row;
    int dots;
    const char *label;
} SaveProgressState;

static void save_cycle_tick(void *ctx) {
    SaveProgressState *st = (SaveProgressState *)ctx;
    st->dots++;
    if (st->dots > 20) st->dots = 1; // wrap so the ticker line can't grow forever on a long retry stretch

    char dots_str[DUMP_PROGRESS_BAR_WIDTH * 2 + 1];
    int n = 0;
    for (int i = 0; i < st->dots && n + 2 < (int)sizeof(dots_str); i++) {
        dots_str[n++] = '.';
        dots_str[n++] = ' ';
    }
    dots_str[n] = '\0';
    printf("\x1b[%d;1H%s %-42s\n", st->row, st->label, dots_str);
    fflush(stdout);
}

// Pacing between retry attempts in the cart-info/header-read/dump-start
// retry loops. Reduced from 300ms to 100ms (2026-07-29, speed pass) — no
// specific evidence the longer value was ever required for correctness
// (unlike the marker/footer protocol logic itself); given that reliability
// here is fundamentally about retrying until a naturally clean exchange
// happens (see CLAUDE.md test_34), a shorter gap means more attempts fit in
// the same wall-clock time without changing per-attempt behavior at all.
#define GBOP_RETRY_PACING_US 100000

// Reads the 512-byte ROM header (the source of the *real* 4-char game code —
// cart-info's own title field is too crude for rom_sizes.c's lookup, see
// enrich_info_from_buf) with retry. Subject to the same per-attempt
// connection reliability as cart-info/dump-start (~5-20%, see CLAUDE.md
// "Hardware test findings" test_6/7/9) — a single failed attempt is the
// statistically expected outcome on this connection, not exceptional. This
// used to be a single inline attempt plus one fresh-fd-wait retry (a tool
// built for "the fd got exhausted," not for per-attempt packet loss); at the
// measured rate that budget still failed most of the time, leaving
// game_code empty and the ROM size stuck on rom_sizes.c's generous
// unrecognized-game fallback instead of the cart's real size (confirmed on
// hardware, Post Firmware Update Test/test_9). Retries in place (close,
// 100ms, reopen) before falling back to the slower fresh-fd wait, same
// structure as dump_rom_with_retry() above. Raised 15->45 alongside
// GBOP_DETECT_CART_ATTEMPTS (Rom Stitching Test/test_8-9) — same measured
// per-attempt cost distribution applies here (5-12ms typical, ~800ms IOS-
// level spike ~76% of the time, unrelated to per-attempt SD I/O).
#define GBOP_HDR_READ_ATTEMPTS 45
// existing_op: an already-open handle to try FIRST, with no close+reopen
// cycle before it — typically the handle a cart-info read just succeeded
// on. Ownership transfers in: this function always closes whatever was
// passed, win or lose, before falling back (if needed) to its normal
// close+reopen retry loop. Pass NULL to skip straight to that loop (the
// previous, unconditional behavior).
//
// Added 2026-07-29 to directly test a specific theory raised by the user:
// if Epilogue Playback gets reliable reads from the same device on the
// same firmware by holding ONE persistent USB handle for its entire
// session (confirmed via the Wireshark captures in CLAUDE.md — 94 cart-info
// polls, zero re-enumerations), while this codebase closes and reopens the
// handle before nearly every command — including, previously, between a
// just-succeeded cart-info read and the very next ROM-header-read attempt
// — then tearing the connection down right before a command that needs
// the device already settled could itself be part of the reliability gap,
// not just generic connection flakiness. (The old "does reopening hurt"
// question was tested once already — USB Probe Test 5 vs 6 — but only for
// repeating the SAME command back to back; never for this specific
// cart-info-succeeds-then-reopens-for-a-different-command transition.)
// The persistent-handle attempt still counts toward GBOP_HDR_READ_ATTEMPTS
// (i.e. it replaces the first close+reopen attempt, not adds to the total).
//
// OLD-FIRMWARE PATH ONLY as of 2026-07-31 — see
// read_rom_header_continuation_protocol() below and read_rom_header_with_retry()'s
// dispatcher further down for the new-firmware path, which replaces this
// close+reopen-per-attempt loop with the same same-handle drain-and-retry
// mechanism already proven for ROM dumps (gbop_dump_rom_continuation).
static int read_rom_header_with_retry_legacy(uint8_t *hdr_out, GBOperatorHandle existing_op) {
    int32_t old_fd = -1;
    int attempt = 0;

    if (existing_op) {
        old_fd = gbop_get_fd(existing_op);
        memset(hdr_out, 0, 512);
        int rc = gbop_read_rom_header(existing_op, hdr_out);
        gbop_close(existing_op);
        if (rc == 0) {
            lprintf("[detect] ROM hdr read succeeded on persistent handle (no reopen)\n");
            return 0;
        }
        lprintf("[detect] ROM hdr read attempt 1/%d failed (persistent handle)\n", GBOP_HDR_READ_ATTEMPTS);
        attempt = 1;
    }

    for (; attempt < GBOP_HDR_READ_ATTEMPTS; attempt++) {
        detect_progress_dot(NULL);
        if (xy_abort_held()) {
            lprintf("[detect] ROM hdr read: aborted by user at attempt %d/%d\n", attempt + 1, GBOP_HDR_READ_ATTEMPTS);
            return -1;
        }
        GBOperatorHandle hdr_op = gbop_reopen();
        if (!hdr_op) { usleep(GBOP_RETRY_PACING_US); continue; }
        old_fd = gbop_get_fd(hdr_op);
        memset(hdr_out, 0, 512);
        int rc = gbop_read_rom_header(hdr_op, hdr_out);
        gbop_close(hdr_op);
        if (rc == 0) return 0;
        lprintf("[detect] ROM hdr read attempt %d/%d failed\n", attempt + 1, GBOP_HDR_READ_ATTEMPTS);
        if (attempt + 1 < GBOP_HDR_READ_ATTEMPTS) usleep(GBOP_RETRY_PACING_US);
    }

    lprintf("[detect] ROM hdr: exhausted %d attempts — waiting for fresh fd\n", GBOP_HDR_READ_ATTEMPTS);
    GBOperatorHandle hdr_op = gbop_reopen_wait_fresh(old_fd, NULL);
    if (!hdr_op) return -1;
    memset(hdr_out, 0, 512);
    int rc = gbop_read_rom_header(hdr_op, hdr_out);
    gbop_close(hdr_op);
    return rc;
}

// New-firmware header-read protocol — gbop_read_rom_header_continuation()
// (gb_operator.c), the same same-handle drain-and-retry mechanism promoted
// to the default ROM dump path (Rom Stitching Test/test_8-9), applied here
// for the first time (2026-07-31). Detect Cart's own header peek was the one
// remaining piece of the new-firmware protocol still using the old
// close+reopen-per-attempt pattern despite sharing the identical marker/
// footer framing gbop_dump_rom_continuation() already handles well — see
// CLAUDE.md for the full reasoning, including a self-inflicted contamination
// bug this also fixes (the old per-attempt implementation never drained its
// own footer before closing, leaving it to land as a stale marker on the
// very next reopened attempt).
//
// Mirrors dump_rom_new_protocol()'s shape: an outer close+reopen loop (much
// smaller than GBOP_CONTINUATION_OUTER_ATTEMPTS's 200 — a header peek is
// only ever needed once per physical cart until cartindex/SD-cache takes
// over, and this is on the interactive Detect Cart / poll_cart path where
// unbounded latency is more costly than for a background dump) wrapping
// GBOP_HDR_CONTINUATION_MAX_CYCLES same-handle inner cycles per attempt.
// existing_op: same contract as read_rom_header_with_retry_legacy() — an
// already-open handle to try first, no reopen; ownership transfers in.
#define GBOP_HDR_CONTINUATION_MAX_CYCLES 20
#define GBOP_HDR_CONTINUATION_OUTER_ATTEMPTS 30
static int read_rom_header_continuation_protocol(uint8_t *hdr_out, GBOperatorHandle existing_op,
                                                   CartType type) {
    GBOperatorHandle hop = existing_op;
    int cycles_used = 0;

    for (int outer = 1; outer <= GBOP_HDR_CONTINUATION_OUTER_ATTEMPTS; outer++) {
        if (xy_abort_held()) {
            lprintf("[detect] ROM hdr (continuation): aborted by user at outer attempt %d/%d\n",
                    outer, GBOP_HDR_CONTINUATION_OUTER_ATTEMPTS);
            if (hop) { gbop_close(hop); hop = NULL; }
            return -1;
        }
        if (!hop) hop = gbop_reopen();
        if (hop) {
            int rc = gbop_read_rom_header_continuation(hop, type, hdr_out,
                                                         GBOP_HDR_CONTINUATION_MAX_CYCLES, &cycles_used,
                                                         detect_progress_dot, NULL);
            gbop_close(hop);
            hop = NULL;
            if (rc == 0) {
                lprintf("[detect] ROM hdr (continuation): success on outer attempt %d/%d, %d cycle(s)\n",
                        outer, GBOP_HDR_CONTINUATION_OUTER_ATTEMPTS, cycles_used);
                return 0;
            }
        }
        if (outer < GBOP_HDR_CONTINUATION_OUTER_ATTEMPTS) usleep(GBOP_RETRY_PACING_US);
    }
    lprintf("[detect] ROM hdr (continuation): exhausted %d outer attempts\n",
            GBOP_HDR_CONTINUATION_OUTER_ATTEMPTS);
    return -1;
}

// Drop-in replacement for the old read_rom_header_with_retry() name —
// dispatches between the old-firmware close+reopen loop (kept fully intact,
// unused by default) and the new continuation-based protocol (default), same
// shape as dump_rom_best(). type must be a valid, already-known cart type
// (from a prior successful cart-info read) for the new-firmware path.
static int read_rom_header_with_retry(uint8_t *hdr_out, GBOperatorHandle existing_op, CartType type) {
    if (g_settings.use_old_firmware) return read_rom_header_with_retry_legacy(hdr_out, existing_op);
    return read_rom_header_continuation_protocol(hdr_out, existing_op, type);
}

static int run_detect_cart_inner_impl(CartInfo *info, uint8_t *rom_hdr,
                                       char *display_title, size_t dsize) {
    GBOperatorHandle detect_op = gbop_reopen();
    CartInfo new_info = {0};
    int cart_ok = (detect_op && gbop_read_cart_info(detect_op, &new_info) == 0);
    bool detect_aborted = false;
    for (int retry = 0; !cart_ok && retry < GBOP_DETECT_CART_ATTEMPTS - 1; retry++) {
        detect_progress_dot(NULL);
        if (xy_abort_held()) {
            lprintf("[detect] cart_info: aborted by user at retry %d/%d\n", retry + 1, GBOP_DETECT_CART_ATTEMPTS - 1);
            detect_aborted = true;
            break;
        }
        if (detect_op) { gbop_close(detect_op); detect_op = NULL; }
        lprintf("[detect] cart_info retry %d/%d\n", retry + 1, GBOP_DETECT_CART_ATTEMPTS - 1);
        usleep(GBOP_RETRY_PACING_US);
        detect_op = gbop_reopen();
        if (!detect_op) continue;
        memset(&new_info, 0, sizeof(new_info));
        cart_ok = (gbop_read_cart_info(detect_op, &new_info) == 0);
    }
    if (!cart_ok) {
        if (detect_op) { gbop_close(detect_op); }
        lprintf(detect_aborted ? "[detect] No cart found (aborted by user)\n" : "[detect] No cart found\n");
        return 0;
    }
    int same_cart = (memcmp(new_info.raw_resp, info->raw_resp, 60) == 0 && rom_hdr[0] != 0);
    // detect_op is intentionally kept OPEN here now (2026-07-29) instead of
    // being closed immediately — see read_rom_header_with_retry()'s comment.
    // A genuinely fresh detect (not same_cart, no SD cache hit) gets to try
    // the ROM header read on this same, already-negotiated handle first,
    // rather than tearing the connection down right before a command that
    // needs the device already settled. Every branch below closes it if it
    // isn't the one that ends up using it.
    uint8_t dc_hdr[512];
    bool dc_hdr_loaded = false;
    if (same_cart) {
        if (detect_op) { gbop_close(detect_op); detect_op = NULL; }
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
            if (detect_op) { gbop_close(detect_op); detect_op = NULL; }
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
             * code, then retry the SD lookup. On success cartindex_update
             * ensures future swaps are instant. */
            lprintf("[detect] cartindex miss — reading ROM header for game code\n");
            uint8_t hdr_buf[512];
            int hdr_rc = read_rom_header_with_retry(hdr_buf, detect_op, info->type);
            detect_op = NULL; // read_rom_header_with_retry() always closes whatever was passed in
            if (hdr_rc == 0) {
                enrich_info_from_buf(info, hdr_buf);
                lprintf("[detect] ROM hdr: code=%s title=%s\n",
                        info->game_code, info->title);
                // Record identity in cartindex right away, on this first
                // successful DETECT — not gated on rom_cache_exists() below,
                // which only reflects whether the ROM has ALSO already been
                // dumped. Previously cartindex_update() only ever ran after
                // a successful dump (see the other 3 call sites), so a cart
                // that was detected but never dumped paid for this same slow
                // header read on every subsequent detect too, indistinguishable
                // from a genuinely new cart. build_rom_path_sd() is a pure
                // function of title/game_code/type, so the predicted filename
                // is exactly the one the ROM will actually occupy once
                // dumped — recording it now doesn't require the file to
                // exist yet (try_enrich_from_index() handles that case).
                {
                    char predicted_path[256];
                    build_rom_path_sd(info, predicted_path, sizeof(predicted_path));
                    const char *pbn = strrchr(predicted_path, '/');
                    cartindex_update(info, pbn ? pbn + 1 : predicted_path);
                }
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
                lprintf("[detect] ROM hdr read failed (exhausted retries)\n");
                // Nothing usable was ever learned about this cart: game_code
                // and title are still empty, and rom_size_kb is still
                // whatever cart-info itself provided (0 on the new-firmware
                // protocol — see rom_sizes.h; the header read is the only
                // thing that ever fills it in). Returning 1 here would
                // report this as a successful detect with an empty title
                // and zero/wrong size displayed as if it were real — the
                // same "false success" shape already fixed for ROM dumps
                // (newproto_check_early_footer, test_25). Fail cleanly
                // instead so the caller retries (next poll cycle, or the
                // user pressing Detect Cart again) rather than showing a
                // cart as "detected" when its actual identity was never
                // established.
                return 0;
            }
        }
    }
    memcpy(rom_hdr, dc_hdr, 512);
    make_display_title(info, rom_hdr, display_title, dsize);
    lprintf("[detect] %s (%s) ROM=%uKB RAM=%uKB\n",
            display_title, info->type_str, info->rom_size_kb, info->ram_size_kb);
    return 1;
}

// Thin wrapper around run_detect_cart_inner_impl() that suppresses console
// output for the duration of the detect (every gbop_*/newproto_* lprintf
// call still writes to the log file — only the TV console output is muted,
// same mechanism mgba_frontend.c already uses around GX emulation for the
// same reason: console and framebuffer sharing). Added 2026-07-31 after
// direct hardware feedback (Detect Cart Test/test_4): with nothing
// suppressed, a slow detect scrolled raw protocol debug text ("cmd=0x00
// tx=64", marker/checksum lines) across the whole screen — this replaces
// that with a single progress-dot line (detect_progress_dot(), one per
// cart-info retry / header-read cycle) that at least looks like visible
// progress instead of either a wall of unreadable text or a blank screen.
// Wrapping at this single call site (rather than each of run_detect_cart_
// inner_impl()'s several early returns) is why this exists as a separate
// function instead of just editing the impl in place.
static int run_detect_cart_inner(CartInfo *info, uint8_t *rom_hdr,
                                  char *display_title, size_t dsize) {
    int saved_suppress = g_log_suppress_console;
    g_log_suppress_console = 1;
    printf("Detecting cart");
    fflush(stdout);
    int rc = run_detect_cart_inner_impl(info, rom_hdr, display_title, dsize);
    g_log_suppress_console = saved_suppress;
    printf("\n");
    return rc;
}

// Quick cart poll (~1 s interval from menu loop). Returns 1 if detect should run.
// Sets *was_absent only after GBOP_POLL_CONFIRM_RETRIES+1 consecutive "no cart /
// no device" reads. Raised from a single retry once the USB probe's reliability
// sweeps (test_6/test_7) showed per-attempt cart-info success as low as 5-20% on
// hardware that reads fine in Epilogue Playback — a single retry confirms "absent"
// on a present cart most of the time at that rate. This only affects how quickly
// poll_cart decides to run/re-run detection; it is not the sole detection budget
// (run_detect_cart_inner has its own, larger retry loop for the same reason).
#define GBOP_POLL_CONFIRM_RETRIES 5
// Minimum continuous real time (not attempt count) of cart-info failure
// before auto_detect_cart=0's fast path concludes "absent" — see the
// debounce at its use below. Switched from a fixed attempt-count streak
// (GBOP_POLL_ABSENT_STREAK, formerly 12) to elapsed time (2026-07-31) after
// direct user reports of the app showing "No cart detected" with a cart
// genuinely inserted: at this connection's own measured worst-case per-
// attempt success rate (as low as ~5%, Rom Stitching Test/test_7), the
// probability that N *consecutive* attempts all fail while a cart is
// present is (1-p)^N — at 12 attempts and p=0.05, that's (0.95)^12 ≈ 54%,
// meaning a false "removed" was the MORE LIKELY outcome, not a rare edge
// case, on a bad-connection session. Reaching a comfortably safe ~1% false-
// positive rate at that same worst-case p needs roughly 90 consecutive
// attempts — but a raw attempt count is also the wrong unit here, since the
// adaptive poll interval (added the same day) means "12 attempts" no longer
// maps to a fixed amount of real time the way it did against the old fixed
// ~1s interval. Time is the more robust, more predictable unit regardless
// of how fast polling happens to be running or how slow an individual read
// happens to be this session.
//
// Given the app's default model is now boot-time detect + an explicit
// user-triggered "Detect Cart Swap" (no continuous automatic re-detection —
// see CLAUDE.md), nothing depends on a real removal being reflected
// promptly: the only cost of taking longer to notice is a stale on-screen
// title, and the user will trigger a fresh manual detect themselves the
// moment they actually swap carts anyway. That asymmetry (false "removed"
// is actively harmful/confusing; slow-to-notice a real removal costs
// nothing) is why this is deliberately generous rather than tuned for
// promptness.
#define GBOP_POLL_ABSENT_SECONDS 20
// Debounce state for poll_cart()'s "cart changed"/"cart absent" signals —
// file-scope (not function-local) statics so poll_cart_reset() below can
// clear them from outside. Persists across ~poll-interval calls the same
// way a function-local static would; the only difference is visibility.
static uint8_t s_pending_change[60];
static bool s_pending_change_valid = false;
static u64 s_absent_since = 0; // 0 = no failure streak currently in progress

// Clears poll_cart()'s own internal debounce state. MUST be called whenever
// a cart is successfully identified via any path OTHER than poll_cart()
// itself (boot detect, manual "Detect Cart", "Detect Cart Swap", or the
// poll-triggered auto-detect call) — run_detect_cart_inner() has no
// visibility into poll_cart()'s statics, so without this, a failure streak
// that had already been accumulating BEFORE a successful detect (e.g. from
// polling while no cart was present) keeps counting from its original start
// time even after the detect succeeds. Confirmed on hardware (Detect Cart
// Test/test_5): a FireRed detect at t=426198ms succeeded cleanly, but the
// very next poll_cart() tick — a single, ordinary transient failure, only
// ~10.5 real seconds later — immediately reported "177299 ms of continuous
// failure" and concluded "Cart removed", because s_absent_since had been
// set roughly 177 real seconds earlier by an unrelated, already-expired
// streak from before the detect ever ran, and nothing had ever cleared it.
static void poll_cart_reset(void) {
    s_absent_since = 0;
    s_pending_change_valid = false;
}

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

    // No firmware-mismatch auto-detection/prompting here (removed — see
    // GBOP_FIRMWARE_MISMATCH in gb_operator.h). use_old_firmware is
    // settings.ini- or dev-menu-toggle-only; rc falls through to the same
    // transient-failure handling as any other failed read below.

    if (cart_info_failed(rc, &probe_info)) {
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
        if (!g_settings.auto_detect_cart) {
            // test_25: with auto_detect_cart=0, nothing expensive ever follows
            // a "removed" conclusion anymore (run_detect_cart_inner is never
            // triggered from here either way) — but the GBOP_POLL_CONFIRM_RETRIES
            // loop below is itself a several-second blocking operation (5
            // retries x reopen+read, ~200ms apart) that still ran unconditionally
            // and was firing constantly on this connection's measured ~5-20%
            // per-attempt reliability, which is exactly the "controls disabled
            // for a good amount of time" the user reported even after gating
            // the auto-detect trigger itself.
            //
            // test_30: a bare single-check conclusion (no debounce at all)
            // over-corrected — main.c's "Confirmed removal" branch wipes
            // info.rom_size_kb/ram_size_kb/title/display_title the instant
            // *was_absent flips false->true, so a single unlucky read (the
            // ordinary outcome on this connection, cart or no cart) was
            // erasing a just-detected cart's info within seconds, over and
            // over. Require GBOP_POLL_ABSENT_STREAK consecutive independent
            // ticks to all fail — spread across real wall-clock time (no
            // blocking, no reopen-loop, no sleep here) rather than compressed
            // into one call — before concluding absence. A genuine removal
            // costs a few extra seconds of stale display; a transient miss no
            // longer wipes anything.
            u64 now = gettime();
            if (s_absent_since == 0) s_absent_since = now;
            u64 elapsed_ms = ticks_to_millisecs(now - s_absent_since);
            if (elapsed_ms < (u64)GBOP_POLL_ABSENT_SECONDS * 1000) {
                return 0; // not yet confident — wait for the next tick
            }
            if (!*was_absent) lprintf("[poll] no cart (%llu ms of continuous failure, auto_detect_cart=0)\n",
                                       (unsigned long long)elapsed_ms);
            *was_absent = true;
            return 0;
        }
        int rc2 = rc;
        for (int retry = 0; retry < GBOP_POLL_CONFIRM_RETRIES; retry++) {
            lprintf("[poll] cart_info fail — retrying (%d/%d)\n", retry + 1, GBOP_POLL_CONFIRM_RETRIES);
            usleep(200000);
            probe = gbop_reopen();
            rc2 = GBOP_USB;
            if (probe) {
                memset(&probe_info, 0, sizeof(probe_info));
                rc2 = gbop_read_cart_info(probe, &probe_info);
                gbop_close(probe);
            }
            if (!cart_info_failed(rc2, &probe_info)) break;
            if (rc2 == GBOP_USB) {
                /* Device-side stall, not a removal signal — wait for next fd cycle */
                lprintf("[poll] retry stalled — skipping\n");
                return 0;
            }
        }
        if (cart_info_failed(rc2, &probe_info)) {
            if (!*was_absent) { lprintf("[poll] no cart (confirmed after %d retries)\n", GBOP_POLL_CONFIRM_RETRIES); }
            *was_absent = true;
            return 0;
        }
        lprintf("[poll] retry ok — transient failure ignored\n");
    }

    /* Cart is present */
    s_absent_since = 0;
    if (*was_absent) {
        s_pending_change_valid = false;
        return 1;  /* confirmed reappearance after a debounced absence — no further wait needed */
    }
    if (memcmp(probe_info.raw_resp, cur_info->raw_resp, 60) != 0) {
        // "Changed" had no debounce at all until now: a single successful-
        // but-noisy read whose bytes merely differ from the last CONFIRMED
        // state was enough to trigger the full, blocking run_detect_cart_inner()
        // — on a connection with a measured ~5-20% per-attempt error rate,
        // that's a real, frequent source of controls freezing for several
        // seconds with no actual cart change involved. Require the SAME
        // differing reading to show up twice in a row (consecutive ~1s polls)
        // before committing to it, mirroring how *was_absent already debounces
        // "gone" via GBOP_POLL_CONFIRM_RETRIES. A genuine swap costs one extra
        // ~1s poll cycle of latency to confirm; a noisy one-off read no longer
        // triggers anything at all.
        if (s_pending_change_valid && memcmp(probe_info.raw_resp, s_pending_change, 60) == 0) {
            s_pending_change_valid = false;
            return 1;  /* same differing reading twice in a row — treat as real */
        }
        memcpy(s_pending_change, probe_info.raw_resp, 60);
        s_pending_change_valid = true;
        lprintf("[poll] cart data changed — awaiting confirmation before re-detect\n");
        return 0;
    }
    s_pending_change_valid = false;
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
    printf("  %s Use Old Firmware: %s\n", sel == 8 ? ">" : " ",
           g_settings.use_old_firmware ? "ON (v9.2.0, unsupported)" : "OFF (v10.0.10, default)");
    printf("  %s ROM Reconcile\n", sel == 9 ? ">" : " ");
    printf("  %s Continuation Test (drain + retry, same handle)\n", sel == 10 ? ">" : " ");
    printf("  %s RTC Sync Test (GBC/GBA, read-then-write-back)\n", sel == 11 ? ">" : " ");
    printf("\nD-Pad / Stick: navigate   A: confirm\n");
}

// Poll interval, in ~60fps frames, used ONLY while a cart is currently
// considered present (see run_dev_menu()/run_frontend() below — polling is
// skipped entirely once a cart is confirmed absent, not just slowed down).
//
// History: this was briefly made adaptive — 30 frames (~500ms) present,
// 6 frames (~100ms) absent, to match Epilogue's own measured "searching"
// cadence (see "Wireshark re-analysis: where cart info actually transfers
// during a physical cart swap") — then reverted after direct hardware
// feedback (Detect Cart Test/test_5): controller input was being dropped
// ("every 3rd press") whenever no cart was detected. Root cause confirmed
// directly from that log's timestamps: poll_cart() itself is a blocking
// call whose own cost (gbop_reopen() + the cmd 0x04 write, which stalls
// ~22% of the time with a slow-tier median ~827ms) already regularly
// exceeds 100ms on its own, so the 6-frame target never achieved a faster
// real poll rate at all (observed inter-poll gaps were still ~500-1000ms,
// dominated by poll_cart()'s own blocking time) — it just left far less
// idle time between calls for PAD_ScanPads() to run inside the menu's own
// per-frame loop, which only gets control between poll_cart() invocations.
//
// Superseded entirely (2026-07-31) by not polling at all in the absent
// state: with manual-only detection (auto_detect_cart=0) as the settled
// design, poll_cart()'s "did something change" signal is inert while no
// cart is known — it never triggers a redetect on its own even when it
// fires, so there was never a reason to poll in that state at any interval.
// Direct user feedback: "if it is a design decision to only have manual
// detection it should just remain not detected."
#define GBOP_POLL_INTERVAL_PRESENT_FRAMES 30

// sel_persist: caller-owned cursor position — preserved across poll-triggered re-entries.
// cart_absent: current cart-presence state (caller's cart_was_absent), selects the
// adaptive poll interval above. Returns: >=0 = menu choice; -1 = reset pressed;
// -2 = poll interval elapsed.
static int run_dev_menu(const CartInfo *info, const char *title, int *sel_persist,
                         bool cart_absent) {
    int sel = *sel_persist;
    int frame_cnt = 0;
    draw_dev_menu(info, title, sel);
    s8 psy = 0, pcy = 0;
    while (!g_reset_pressed) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        WPAD_ScanPads();
        // No background polling AT ALL when auto_detect_cart=0 (the
        // default) — not just while absent (see the history above), but
        // also while a cart is considered present. Direct hardware evidence
        // (Detect Cart Test/test_6) showed poll_cart()'s own time-based
        // absence debounce (however generous) can't be trusted either way:
        // a genuinely inserted, untouched Emerald cart still produced a
        // continuous ~25-second stretch of nothing but NOCART reads,
        // correctly tripping the debounce and wiping the display to "No
        // cart detected" even though the cart was never removed. No
        // threshold fixes this — the connection's own sustained failure
        // streaks are simply longer than any reasonable wait. Per direct
        // user decision: "suppress the 'no cart detected' unless a detect
        // cart itself comes back negative" — i.e. only run_detect_cart_inner()
        // (boot, manual "Detect Cart", "Detect Cart Swap"), with its own far
        // larger and more thorough retry budget, may ever declare a cart
        // absent; the background poll must never drive that conclusion on
        // its own. Since auto_detect_cart=0 already made poll_cart()'s
        // "changed" signal a no-op (see main()'s choice==-2 handling), and
        // now its "absent" signal is untrusted too, there is nothing left
        // for it to usefully do in that mode — so it simply never runs.
        // auto_detect_cart=1 (opt-in, non-default) is unaffected: its whole
        // premise is trusting the background poll to drive behavior
        // automatically, so it keeps the original interval-gated polling.
        if (g_settings.auto_detect_cart && !cart_absent && ++frame_cnt >= GBOP_POLL_INTERVAL_PRESENT_FRAMES) {
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
    char backups_dir[64];
    snprintf(backups_dir, sizeof(backups_dir), "%s/saves/backups", g_app_root);
    mkdir(backups_dir, 0755);

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
             "%s/%s_%04d%02d%02d_%02d%02d%02d.sav", backups_dir, safe,
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
    snprintf(cur_dir, sizeof(cur_dir), "%s/saves", g_app_root);
    /* Fall back to drive root if saves dir doesn't exist yet */
    {
        DIR *td = opendir(cur_dir);
        if (!td) snprintf(cur_dir, sizeof(cur_dir), "%s:/",
                          (g_app_root[0] == 'u') ? "usb" : "sd");
        else closedir(td);
    }

    int sel = 0;

    while (!g_reset_pressed) {
        /* Scan directory */
        int cnt = 0;

        /* ".." entry if not at root */
        bool at_root = (strcmp(cur_dir, "sd:/") == 0 || strcmp(cur_dir, "sd:") == 0 ||
                        strcmp(cur_dir, "usb:/") == 0 || strcmp(cur_dir, "usb:") == 0);
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

/* Runs the GBA ROM-size mirror check (gbop_verify_gba_rom_size) before any
 * ROM dump. That check streams via the ROM-read command whenever it actually
 * runs, which always leaves the device mid-stream — so *op is unconditionally
 * closed and replaced with a fresh handle afterward. Shows a message and
 * returns false if the device can't be reopened. No-op (returns true, *op
 * untouched) for GB/GBC carts. */
static bool verify_rom_size_before_dump(GBOperatorHandle *op, CartInfo *info) {
    if (info->type != CART_TYPE_GBA) return true;
    // Every caller of this function immediately follows it with a "Dump ROM"
    // screen that suppresses console output around the dump itself (a fixed-
    // position progress bar) — but this mirror-check's own marker/protocol
    // logging (gbop_verify_gba_rom_size(), potentially several retried lines
    // when its gate actually fires) ran BEFORE any of those callers set
    // g_log_suppress_console, so it scrolled straight onto the console
    // unsuppressed regardless. Suppressing it here, once, fixes all three
    // call sites (play_game(), dev-menu Dump ROM, frontend Dump ROM) instead
    // of needing the same fix repeated at each one. Log file unaffected;
    // save/restore rather than hardcoding back to 0 in case a future caller
    // ever invokes this from an already-suppressed context.
    int saved_suppress = g_log_suppress_console;
    g_log_suppress_console = 1;
    int32_t old_fd = gbop_get_fd(*op);
    int did_stream = 0;
    int corrected = gbop_verify_gba_rom_size(*op, info, &did_stream);
    if (corrected)
        lprintf("[gbop] ROM size corrected to %u KB before dump\n", info->rom_size_kb);
    // The mirror-check's own gate (an anomalous cart-info byte pattern) very
    // often decides no verification is needed at all — in that case the
    // handle passed in was never touched, so there is no reason to pay for
    // any reopen at all before every single dump attempt.
    if (!did_stream) { g_log_suppress_console = saved_suppress; return true; }
    gbop_close(*op); *op = NULL;
    // Try a fast, plain reopen first — matching dump_rom_with_retry()'s own
    // established pattern of fast retries before ever reaching for the
    // expensive fresh-fd wait. A single failed marker read (the common case,
    // ~97% per-attempt) doesn't meaningfully spend the fd any differently
    // than any other quick failed attempt elsewhere in this codebase; using
    // gbop_reopen_wait_fresh() unconditionally here burned the full 75-try
    // budget (~5-6s) on hardware where the fd never cycles at all (Rom
    // Stitching Test/test_5, confirmed directly: ~75 back-to-back reopens,
    // same fd every time, immediately after a verify-check read failure).
    // gbop_reopen_wait_fresh() is now only reached if the fast path itself
    // fails to produce a working handle at all.
    *op = gbop_reopen();
    if (!*op) *op = gbop_reopen_wait_fresh(old_fd, NULL);
    g_log_suppress_console = saved_suppress;
    if (!*op) {
        show_message("GB Operator not found. Check USB.");
        return false;
    }
    return true;
}

// The ROM dump's opening handshake (send cmd 0x00, read the header marker)
// is subject to the same per-attempt connection reliability already measured
// for cart-info detection (~5-20%, see CLAUDE.md "Hardware test findings"
// test_6/7/9) — a single failed attempt here is the statistically expected
// outcome on a lossy connection, not exceptional. Confirmed on hardware
// (test_8/test_9): every observed dump-start failure happens before a single
// ROM byte is written, so retrying from scratch is safe and cheap —
// rom_cache_dump() always (re)opens its output file in "wb" mode, silently
// overwriting whatever partial (0-byte, in every case seen so far) file the
// previous attempt left. Retried in place first (close+reopen, no
// fd-freshness requirement — the failure mode here is a dropped packet, not
// a spent fd); falls back to the slower fresh-fd wait only if that budget is
// also exhausted, covering the rarer "fd genuinely spent" failure mode that
// the existing play_game retry was originally written for.
#define GBOP_DUMP_START_ATTEMPTS 15
static int dump_rom_with_retry(GBOperatorHandle *op, CartInfo *info,
                                char *rom_path, size_t path_size) {
    int32_t old_fd = gbop_get_fd(*op);
    for (int attempt = 0; attempt < GBOP_DUMP_START_ATTEMPTS && *op; attempt++) {
        if (rom_cache_dump(*op, info, rom_path, path_size) == 0) return 0;
        lprintf("[dump] ROM dump attempt %d/%d failed\n", attempt + 1, GBOP_DUMP_START_ATTEMPTS);
        gbop_close(*op); *op = NULL;
        if (attempt + 1 < GBOP_DUMP_START_ATTEMPTS) {
            usleep(GBOP_RETRY_PACING_US);
            *op = gbop_reopen();
        }
    }

    lprintf("[dump] ROM dump: exhausted %d attempts — waiting for fresh fd\n", GBOP_DUMP_START_ATTEMPTS);
    log_commit_sd();
    // Trimmed from 75 to 30 tries (test_26, 2026-07-28): this loop exists to
    // recover from a genuinely spent/stale fd (tx=-7005), but the 15 fast
    // retries above already fail almost exclusively on header-marker
    // mismatches — a connection-timing issue, not fd exhaustion — and on
    // hardware where the fd is documented to never cycle at all (external
    // tester #2's d2x-cIOS pattern), the full 75 tries (~24.5s at ~330ms/try)
    // was pure dead time before falling back to the same fd anyway. 30 tries
    // (~10s) keeps ample margin above the ~42-try/2.5s fd-cycle timing
    // documented elsewhere for hardware where the fd genuinely does cycle,
    // while cutting the wasted worst case by more than half.
    #define GBOP_DUMP_FRESH_FD_TRIES 30
    int same_fd = 0;
    if (gbop_fd_known_unstable()) {
        // Already directly confirmed elsewhere this session (see
        // gbop_fd_known_unstable()'s doc comment, and Rom Stitching Test/
        // test_6 — a ~2.4s burst here, right after an identical burst from
        // gbop_reopen_wait_fresh() moments earlier, same fd both times) that
        // this connection never cycles the fd at all. Skip straight to one
        // fast reopen instead of re-discovering the same non-result.
        *op = gbop_reopen();
        same_fd = *op ? GBOP_DUMP_FRESH_FD_TRIES : 0;
    } else {
        for (int i = 0; i < GBOP_DUMP_FRESH_FD_TRIES && !*op; i++) {
            usleep(60000);
            *op = gbop_reopen();
            if (!*op) continue;
            if (gbop_get_fd(*op) != old_fd) break;
            same_fd++;
            if (same_fd < GBOP_DUMP_FRESH_FD_TRIES) { gbop_close(*op); *op = NULL; }
        }
        if (same_fd >= GBOP_DUMP_FRESH_FD_TRIES) gbop_mark_fd_unstable();
    }
    if (*op) {
        lprintf("[dump] ROM dump retry on fresh fd=%d%s\n",
                (int)gbop_get_fd(*op), same_fd >= GBOP_DUMP_FRESH_FD_TRIES ? " (same_fd fallback)" : "");
    }
    if (!*op || rom_cache_dump(*op, info, rom_path, path_size) != 0) {
        if (*op) { gbop_close(*op); *op = NULL; }
        return -1;
    }
    return 0;
}

// New-firmware dump protocol — the continuation mechanism
// (gbop_dump_rom_continuation, gb_operator.c), promoted from experiment to
// the default path after Rom Stitching Test/test_8-9 confirmed it on
// hardware: every successful dump across 4 different GBA carts that
// session, 8 of 9 first-try-clean, and a directly measured per-attempt
// system cost of 5-12ms for the common marker-only-failure case (every
// multi-second gap in the earlier manual-press logs was human reaction
// time, not real cost) — justifying a much larger automatic retry budget
// than dump_rom_with_retry()'s old 15+30 design. GBA and GB/GBC.
//
// Deliberately does NOT reuse any of dump_rom_with_retry()'s machinery
// (per-attempt SD fopen via rom_cache_dump(), the old fixed 15/30 split) —
// per direct instruction, none of the old, slower protocol's checks carry
// over. Buffer-based: touches SD exactly once, on final success, not once
// per attempt. Only used when !g_settings.use_old_firmware — see
// dump_rom_best() below, which is the actual drop-in replacement for
// dump_rom_with_retry() at every call site.
#define GBOP_CONTINUATION_MAX_CYCLES 20
#define GBOP_CONTINUATION_OUTER_ATTEMPTS 200
#define GBOP_CONTINUATION_OUTER_PACING_US 100000
static int dump_rom_new_protocol(GBOperatorHandle *op, CartInfo *info,
                                  char *rom_path, size_t path_size,
                                  int *out_outer_attempt, int *out_cycles_used,
                                  GbopProgressCB cycle_cb, void *cycle_ctx,
                                  GbopByteProgressCB progress_cb, void *progress_ctx) {
    uint32_t total = info->rom_size_kb * 1024;
    uint8_t *buf = malloc(total);
    if (!buf) return -1;

    int cycles_used = 0;
    int rc = -1;
    int outer_attempt = 1;
    for (outer_attempt = 1; outer_attempt <= GBOP_CONTINUATION_OUTER_ATTEMPTS; outer_attempt++) {
        if (*op) { gbop_close(*op); *op = NULL; }
        *op = gbop_reopen();
        if (*op) {
            rc = gbop_dump_rom_continuation(*op, info, buf, total,
                                             GBOP_CONTINUATION_MAX_CYCLES, &cycles_used,
                                             cycle_cb, cycle_ctx,
                                             progress_cb, progress_ctx);
            if (rc == 0) break;
        }
        PAD_ScanPads();
        if ((PAD_ButtonsHeld(0) & (PAD_BUTTON_X | PAD_BUTTON_Y)) == (PAD_BUTTON_X | PAD_BUTTON_Y)) {
            lprintf("[dump] new-protocol dump aborted by user after %d outer attempt(s)\n", outer_attempt);
            break;
        }
        usleep(GBOP_CONTINUATION_OUTER_PACING_US);
    }
    if (*op) { gbop_close(*op); *op = NULL; }

    if (out_outer_attempt) *out_outer_attempt = outer_attempt;
    if (out_cycles_used) *out_cycles_used = cycles_used;

    if (rc != 0) { free(buf); return -1; }

    build_rom_path_sd(info, rom_path, path_size);
    FILE *f = fopen(rom_path, "wb");
    if (!f || fwrite(buf, 1, total, f) != total) {
        if (f) fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    free(buf);
    return 0;
}

// Drop-in replacement for every dump_rom_with_retry() call site: dispatches
// between the old-firmware retry mechanism (kept fully intact, unused by
// default — see settings.ini use_old_firmware) and the new continuation-
// based protocol (default). Same signature/contract dump_rom_with_retry()
// alone used to have.
static int dump_rom_best(GBOperatorHandle *op, CartInfo *info,
                          char *rom_path, size_t path_size,
                          GbopProgressCB cycle_cb, void *cycle_ctx,
                          GbopByteProgressCB progress_cb, void *progress_ctx) {
    if (g_settings.use_old_firmware) return dump_rom_with_retry(op, info, rom_path, path_size);
    return dump_rom_new_protocol(op, info, rom_path, path_size, NULL, NULL,
                                  cycle_cb, cycle_ctx, progress_cb, progress_ctx);
}

// Save read/write (cmd 0x02/0x03) had NO retry at all until now — a single
// failed attempt was reported straight to the player as "Save dump failed" /
// "Save upload failed", on a connection whose per-attempt success rate for
// this same C0DE-marker-framed protocol shape has been measured elsewhere in
// this project as low as single digits to ~20% (cart-info) and a ~75-78%
// stall rate for the structurally identical cmd 0x00. No continuation
// mechanism is used here (unlike ROM dump/header read) — a save is at most
// 128KB (GBA Flash) vs a 16MB ROM, so the same plain close+reopen retry loop
// already proven for cart-info/header reads is simple and should be more
// than adequate; nothing rules out a continuation variant later if a
// hardware test shows this isn't enough.
//
// Both wrappers follow dump_rom_new_protocol()'s exact contract: *op is
// always closed and set to NULL before returning, success or failure, so
// every existing call site's own "if (op) gbop_close(op)" cleanup becomes a
// safe no-op afterward, unchanged from how choice==0 (ROM dump) already
// works.
//
// Safe to retry a WRITE from scratch on failure: gbop_write_save() only
// ever returns non-zero for a failure that happened before or during data
// transfer (command send, header marker, or a chunk write itself erroring)
// — a footer-marker mismatch after all data was already sent is treated as
// a soft warning and still returns success (see gbop_write_save_new()'s own
// comment), so a retry never fires after a real write has already gone
// through. Each retry resends the complete, correct buffer from byte 0 via
// a fresh cmd 0x03, which simply overwrites whatever a prior failed attempt
// left behind.
#define GBOP_SAVE_RETRY_ATTEMPTS 60

static int save_read_with_retry(GBOperatorHandle *op, const CartInfo *info,
                                 uint8_t *buf, uint32_t save_bytes,
                                 GbopProgressCB cycle_cb, void *cycle_ctx) {
    int rc = -1;
    for (int attempt = 1; attempt <= GBOP_SAVE_RETRY_ATTEMPTS; attempt++) {
        // Fires on every attempt, success or failure — same "attempts are
        // being made" convention as dump_cycle_tick, so the ticker shows
        // progress from the very first try, not just once something fails.
        if (cycle_cb) cycle_cb(cycle_ctx);
        if (*op) { gbop_close(*op); *op = NULL; }
        *op = gbop_reopen();
        if (*op) {
            rc = gbop_read_save(*op, info, buf, save_bytes);
            if (rc == 0) break;
        }
        if (xy_abort_held()) {
            lprintf("[save] read retry aborted by user after %d attempt(s)\n", attempt);
            break;
        }
        if (attempt < GBOP_SAVE_RETRY_ATTEMPTS) usleep(GBOP_RETRY_PACING_US);
    }
    if (*op) { gbop_close(*op); *op = NULL; }
    return rc;
}

static int save_write_with_retry(GBOperatorHandle *op, const CartInfo *info,
                                  const uint8_t *buf, uint32_t save_bytes,
                                  GbopProgressCB cycle_cb, void *cycle_ctx) {
    int rc = -1;
    for (int attempt = 1; attempt <= GBOP_SAVE_RETRY_ATTEMPTS; attempt++) {
        if (cycle_cb) cycle_cb(cycle_ctx);
        if (*op) { gbop_close(*op); *op = NULL; }
        *op = gbop_reopen();
        if (*op) {
            rc = gbop_write_save(*op, info, buf, save_bytes);
            if (rc == 0) break;
        }
        if (xy_abort_held()) {
            lprintf("[save] write retry aborted by user after %d attempt(s)\n", attempt);
            break;
        }
        if (attempt < GBOP_SAVE_RETRY_ATTEMPTS) usleep(GBOP_RETRY_PACING_US);
    }
    if (*op) { gbop_close(*op); *op = NULL; }
    return rc;
}

// Captures one ROM Reconcile attempt (dev-menu "ROM Reconcile" screen) with
// the standard close+reopen retry idiom used everywhere else in this file,
// updating a caller-owned status buffer for the screen to display. Unlike
// dump_rom_with_retry(), a failed/short capture is not itself a failure —
// that's the expected, useful case this whole feature exists to handle — so
// this never falls back to a fresh-fd wait; GBOP_HDR_READ_ATTEMPTS closely-
// spaced tries is enough to get *a* handle open, and whatever
// rom_reconcile_capture() streams (clean, short, or nothing) is reported as-is.
static void reconcile_capture_slot(CartInfo *info, int slot, ReconcileAttempt *out,
                                    char *status, size_t status_size) {
    if (info->rom_size_kb == 0) {
        snprintf(status, status_size, "No cart detected — detect cart first");
        return;
    }
    g_log_suppress_console = 1;
    GBOperatorHandle op = NULL;
    for (int attempt = 0; attempt < GBOP_HDR_READ_ATTEMPTS && !op; attempt++) {
        usleep(GBOP_RETRY_PACING_US);
        op = gbop_reopen();
    }
    int rc = -1;
    if (op) {
        rc = rom_reconcile_capture(op, info, slot, out);
        gbop_close(op);
    }
    g_log_suppress_console = 0;
    if (!op) {
        snprintf(status, status_size, "Could not open device after %d attempts", GBOP_HDR_READ_ATTEMPTS);
    } else if (rc == 0) {
        snprintf(status, status_size, "Clean (%u KB)", out->captured_len / 1024);
    } else if (rc == 1) {
        snprintf(status, status_size, "Short: %u KB", out->captured_len / 1024);
    } else {
        snprintf(status, status_size, "Nothing captured");
    }
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
    // fd used at the very start of the dump (before any EP OUT ops).
    // Used by the fresh-fd wait below so that a fd cycle that happened
    // *during* the dump is correctly recognised as already-fresh.
    int32_t pre_dump_fd = -1;

    if (!rom_exists) {
        if (!prompt_yesno("ROM not installed on SD card.\nInstall now?"))
            return;

        // Snapshot of what was cached (and what the player confirmed at the
        // "Play <title>?" prompt) before any device I/O — compared against
        // the freshly-dumped ROM's own real title/code below (check #1: ROM
        // dump title mismatch). Catches the case where the cart physically
        // in the slot isn't actually the one the cached info describes.
        char pre_title[17]; strncpy(pre_title, info->title, sizeof(pre_title) - 1); pre_title[sizeof(pre_title) - 1] = '\0';
        char pre_code[5];   strncpy(pre_code, info->game_code, sizeof(pre_code) - 1); pre_code[sizeof(pre_code) - 1] = '\0';

        // Same header text as the dev-menu/frontend "Dump ROM" screens
        // (dump_rom_best()'s callers), so the wording is consistent across
        // every place a ROM gets dumped from the cart.
        printf("\x1b[2J\x1b[H");
        printf("Dump ROM\n--------\n\n");

        GBOperatorHandle op = gbop_reopen();
        if (!op) { show_message("GB Operator not found. Check USB."); return; }

        if (!verify_rom_size_before_dump(&op, info)) return;

        if (rom_cache_exists(info, rom_path, sizeof(rom_path))) {
            // The reported size was corrected above and now matches a file
            // already on SD from an earlier (already-corrected) dump — the
            // "not installed" verdict at the top of this function was based
            // on the uncorrected size. Nothing left to transfer.
            lprintf("[play] ROM already cached under corrected size: %s\n", rom_path);
            gbop_close(op); op = NULL;
        } else {
            pre_dump_fd = gbop_get_fd(op);
            DumpProgressState dps = { .row = 5, .dots = 0, .bar_shown = 0 };
            // Suppress console for the dump itself — same reasoning as the
            // dev-menu/frontend Dump ROM screens: gbop_dump_rom_continuation()
            // logs plenty that would otherwise scroll straight through the
            // fixed-position progress bar above. Log file unaffected.
            g_log_suppress_console = 1;
            int install_rc = dump_rom_best(&op, info, rom_path, sizeof(rom_path),
                                             dump_cycle_tick, &dps,
                                             draw_dump_progress, &dps);
            g_log_suppress_console = 0;
            if (install_rc != 0) {
                show_message("ROM install failed. Returning to menu.");
                return;
            }
            gbop_close(op); op = NULL;
        }

        enrich_info_from_rom(rom_path, info);

        // Check #1: does the ROM we just actually dumped match what was
        // cached/confirmed before we dumped it? enrich_info_from_rom() only
        // ever fixes title/game_code from the real file bytes — if either
        // differs from the pre-dump snapshot, the cart physically in the
        // slot isn't the one "info" described when the player confirmed the
        // prompt. That also means rom_size_kb/ram_size_kb/type/raw_resp
        // (none of which enrich_info_from_rom touches) are still stale and
        // wrong for whatever's actually inserted — needed correctly for the
        // save-dump step that follows. Run a real detect to refresh all of
        // that from the actual cart before proceeding (accepts paying for
        // one redundant device header-read in this — rare, mismatch-only —
        // path, in exchange for reusing the same already-proven detection
        // logic rather than hand-rolling a second one).
        if (strcmp(pre_title, info->title) != 0 ||
            (pre_code[0] && strcmp(pre_code, info->game_code) != 0)) {
            lprintf("[play] ROM dump revealed a different cart than expected "
                    "(was \"%s\"/%s, now \"%s\"/%s) — re-detecting\n",
                    pre_title, pre_code, info->title, info->game_code);
            if (!run_detect_cart_inner(info, rom_hdr, display_title, dsize)) {
                show_message("Cart not detected. Check insertion.");
                return;
            }
        }

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

    /* Read save from cart — same header text as the dev-menu/frontend "Dump
     * Save" screens (dump_save_to_sd()), so the wording is consistent across
     * every place a save gets read from the cart. */
    printf("\x1b[2J\x1b[H");
    printf("Dumping save: %s (%u KB)\n", display_title, info->ram_size_kb);
    printf("-------------------------------\n\n");
    fflush(stdout);

    GBOperatorHandle op = gbop_reopen();
    if (!op) { show_message("GB Operator not found. Check USB."); return; }

    // Cart_info (cmd 0x04) resets the device to idle state before the save read.
    // Also confirms the cart is still present before committing to the save read.
    //
    // Retried up to GBOP_DETECT_CART_ATTEMPTS times, matching every other
    // cart-info check in this codebase — this used to be a single attempt
    // plus one NOCART retry (2 total), which on this connection's own
    // measured per-attempt success rate (as low as single digits to ~20%)
    // meant a real, physically-present cart could easily fail both tries and
    // abort the ENTIRE play_game() flow with "Cart not detected" right after
    // a successful ROM install — confirmed on hardware (Save Read Write Test
    // /test_6: exactly 2 failed attempts here, immediately after "ROM dump
    // complete... SUCCESS", forced the player to press Play Game again from
    // scratch). GBOP_USB (tx=-7005, fd spent) still short-circuits out of
    // this loop immediately — that's not a connection-reliability failure,
    // it's a different, already-understood condition the fresh-fd wait below
    // exists specifically to fix, not something retrying the same fd helps.
    bool need_cart_recheck = false;
    {
        CartInfo live = {0};
        int ci_rc = GBOP_NOCART;
        // Suppress console for the retry loop itself only — same reasoning as
        // every other retry loop in this codebase (gbop_reopen()/cart-info's
        // own logging would otherwise scroll across the "Dumping save..."
        // screen). Confined to just this loop (not the whole function) so
        // whatever happens after — a shown message, a detected mismatch —
        // is never accidentally left suppressed too.
        g_log_suppress_console = 1;
        for (int attempt = 0; attempt < GBOP_DETECT_CART_ATTEMPTS; attempt++) {
            if (!op) op = gbop_reopen();
            if (!op) { usleep(GBOP_RETRY_PACING_US); continue; }
            memset(&live, 0, sizeof(live));
            ci_rc = gbop_read_cart_info(op, &live);
            if (ci_rc == GBOP_OK || ci_rc == GBOP_USB) break;
            if (xy_abort_held()) break;
            lprintf("[play] cart_info recheck retry %d/%d\n", attempt + 1, GBOP_DETECT_CART_ATTEMPTS);
            gbop_close(op); op = NULL;
            usleep(GBOP_RETRY_PACING_US);
        }
        g_log_suppress_console = 0;
        if (ci_rc == GBOP_USB) {
            lprintf("[play] cart_info stall (fd spent) — retrying on fresh fd\n");
            need_cart_recheck = true;
            /* op still holds the stalled fd — fresh-fd wait uses it for old_fd */
        } else if (!op || cart_info_failed(ci_rc, &live)) {
            if (op) { gbop_close(op); op = NULL; }
            show_message("Cart not detected. Check insertion.");
            return;
        } else if (cart_type_family_mismatch(live.type, info->type)) {
            // Check #2: save data mismatch (GB/GBC vs GBA) — a cheap signal
            // already available from the cart-info read just done above, no
            // extra USB cost. Deliberately doesn't catch a same-type
            // mismatch (e.g. Ruby vs Sapphire, both GBA) — per direct
            // discussion, that's accepted residual risk the player's own
            // judgement guards against (see CLAUDE.md). When this DOES fire,
            // every ROM-cache/install decision already made earlier in this
            // call was based on stale info, so rather than continuing inline
            // with a half-corrected state, re-detect for real and restart
            // the whole sequence (install check -> save dump -> play) from
            // the top with corrected info.
            lprintf("[play] cart type mismatch (expected %s, cart reports %s) — "
                    "re-detecting and restarting\n", info->type_str, live.type_str);
            gbop_close(op); op = NULL;
            if (!run_detect_cart_inner(info, rom_hdr, display_title, dsize)) {
                show_message("Cart not detected. Check insertion.");
                return;
            }
            play_game(info, rom_hdr, display_title, dsize);
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
        // Use pre-dump fd when available: if the fd cycled *during* the dump,
        // the reopen above (line 1075) already returned the new fd — comparing
        // against pre_dump_fd lets the loop recognise it as fresh immediately.
        // For the cache-hit path (no dump), pre_dump_fd==-1 → fall back to current fd.
        int32_t old_fd = (pre_dump_fd != -1) ? pre_dump_fd : gbop_get_fd(op);
        gbop_close(op); op = NULL;
        // Log BEFORE log_commit_sd so this line is included in the committed
        // FAT directory entry.  fclose takes ~200-500ms (SD sector writes), but
        // USB is closed here so IOS USB is idle — no stall risk.
        lprintf("[play] waiting for fresh fd (old=%d)\n", (int)old_fd);
        log_commit_sd();
        int same_fd_count = 0;
        g_log_suppress_console = 1; // same reasoning as the cart-info recheck loop above
        for (int i = 0; i < 75 && !op; i++) {
            usleep(60000);                     // 60ms/try → new fd after ~1-3s (~17-50 tries)
            op = gbop_reopen();
            if (!op) continue;
            if (gbop_get_fd(op) != old_fd) break;  // fresh fd found
            same_fd_count++;
            if (same_fd_count < 75) { gbop_close(op); op = NULL; }
            // at same_fd_count==75: d2x-cIOS fd never cycles — proceed with same fd
        }
        g_log_suppress_console = 0;
        if (!op) { show_message("GB Operator not found."); return; }
        if (same_fd_count >= 75)
            lprintf("[play] fresh fd: same_fd fallback (fd never cycled)\n");
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
        int live2_rc = GBOP_NOCART;
        g_log_suppress_console = 1; // same reasoning as the first cart-info recheck loop
        for (int attempt = 0; attempt < GBOP_DETECT_CART_ATTEMPTS; attempt++) {
            if (!op) op = gbop_reopen();
            if (!op) { usleep(GBOP_RETRY_PACING_US); continue; }
            memset(&live2, 0, sizeof(live2));
            live2_rc = gbop_read_cart_info(op, &live2);
            if (!cart_info_failed(live2_rc, &live2)) break;
            if (xy_abort_held()) break;
            lprintf("[play] cart recheck retry %d/%d\n", attempt + 1, GBOP_DETECT_CART_ATTEMPTS);
            gbop_close(op); op = NULL;
            usleep(GBOP_RETRY_PACING_US);
        }
        g_log_suppress_console = 0;
        if (!op || cart_info_failed(live2_rc, &live2)) {
            if (op) { gbop_close(op); op = NULL; }
            show_message("Cart not detected. Check insertion.");
            return;
        }
        lprintf("[play] cart recheck OK\n");
    }

    uint32_t save_bytes = info->ram_size_kb * 1024;
    uint8_t *savebuf = (uint8_t *)malloc(save_bytes);
    if (!savebuf) { gbop_close(op); show_message("Out of memory."); return; }

    SaveProgressState sps = { .row = 5, .dots = 0, .label = "Dumping save" };
    g_log_suppress_console = 1;
    int save_ok = (save_read_with_retry(&op, info, savebuf, save_bytes,
                                         save_cycle_tick, &sps) == 0);
    g_log_suppress_console = 0;
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

    /* Write save to storage */
    char saves_dir_pg[64];
    snprintf(saves_dir_pg, sizeof(saves_dir_pg), "%s/saves", g_app_root);
    mkdir(saves_dir_pg, 0755);
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

static int dump_save_to_sd(GBOperatorHandle *op, const CartInfo *info,
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
    SaveProgressState sps = { .row = 5, .dots = 0, .label = "Dumping save" };
    // Suppress console for the retry loop itself — same reasoning as the ROM
    // dump progress bar: gbop_read_save()'s own marker/protocol logging would
    // otherwise scroll straight through the fixed-position ticker line above.
    // Log file unaffected.
    g_log_suppress_console = 1;
    int read_rc = save_read_with_retry(op, info, buf, save_bytes, save_cycle_tick, &sps);
    g_log_suppress_console = 0;
    if (read_rc != 0) {
        lprintf("[save] Read failed\n");
        free(buf);
        return -1;
    }

    {
        char saves_dir_ds[64];
        snprintf(saves_dir_ds, sizeof(saves_dir_ds), "%s/saves", g_app_root);
        mkdir(saves_dir_ds, 0755);
    }

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
static int write_save_to_cart(GBOperatorHandle *op, const CartInfo *info,
                               const uint8_t *buf, uint32_t save_bytes,
                               const char *display_title, const char *path) {
    printf("\x1b[2J\x1b[H");
    printf("Uploading save: %s (%u KB)\n", display_title, info->ram_size_kb);
    printf("-------------------------------\n\n");
    lprintf("[save] Writing %u KB save to cart from %s\n", info->ram_size_kb, path);

    SaveProgressState sps = { .row = 5, .dots = 0, .label = "Uploading save" };
    g_log_suppress_console = 1;
    int write_rc = save_write_with_retry(op, info, buf, save_bytes, save_cycle_tick, &sps);
    g_log_suppress_console = 0;
    if (write_rc != 0) {
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
    int ok = (write_save_to_cart(&op, info, buf, save_bytes, display_title, save_path) == 0);
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
                         int n_items, const char *items[], int *sel_persist,
                         bool cart_absent) {
    int sel = *sel_persist;
    int frame_cnt = 0;
    draw_frontend(info, title, sel, n_items, items);
    s8 psy = 0, pcy = 0;
    while (!g_reset_pressed) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        WPAD_ScanPads();
        // No background polling at all when auto_detect_cart=0 (default) —
        // see the matching comment in run_dev_menu().
        if (g_settings.auto_detect_cart && !cart_absent && ++frame_cnt >= GBOP_POLL_INTERVAL_PRESENT_FRAMES) {
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
    g_log_t0 = gettime();
    init_video();
    WPAD_Init();
    PAD_Init();
    SYS_SetResetCallback(main_on_reset);
    SYS_SetPowerCallback(main_on_power);

    printf("Wii GB Operator\n");
    printf("===============\n\n");
    printf("Starting up...\n");

    /* Reset the USB host controller unconditionally before any USB operations.
     * On d2x-cIOS Wiis, HBC can leave OH0 in a stale state where writes appear
     * to succeed (tx=64) but reads return -7008.  This affects both SD-boot and
     * USB-boot users, so the reset must run regardless of where the app lives.
     * Safe here — SD uses SDIO, not USB; SD mount happens after this returns. */
    printf("Resetting USB host...\n");
    USB_Deinitialize();
    USB_Initialize();
    usleep(2000000);  /* 2s for GB Operator to re-enumerate on the clean bus */

    // Mount SD only — fatInitDefault also starts a USB mass storage driver
    // which can interfere with USB device enumeration for the GB Operator.
    // Retry up to 3 times in case the SD slot needs a moment after boot.
    int sd_ok = 0;
    for (int retry = 0; retry < 3 && !sd_ok; retry++) {
        if (retry > 0) { printf("SD retry %d...\n", retry); usleep(200000); }
        sd_ok = fatMountSimple("sd", &__io_wiisd);
    }
    if (sd_ok) printf("[OK]  SD mounted\n");
    else       printf("[WARN] SD mount failed\n");

    /* Phase 1: detect app drive (SD probe only — USB deferred to Phase 2). */
    bool sd_has_app_dir = detect_app_drive();

    /* Log rotation: find next available log.txt / log1.txt / log2.txt ... */
    {
        char log_path[64];
        snprintf(log_path, sizeof(log_path), "%s/log.txt", g_app_root);
        FILE *t = fopen(log_path, "r");
        if (t) {
            fclose(t);
            int n;
            for (n = 1; n < 200; n++) {
                snprintf(log_path, sizeof(log_path), "%s/log%d.txt", g_app_root, n);
                FILE *t2 = fopen(log_path, "r");
                if (!t2) break;
                fclose(t2);
            }
        }
        strncpy(g_log_path, log_path, sizeof(g_log_path) - 1);
        g_log = fopen(g_log_path, "w");
        if (g_log) printf("[OK]  Logging to %s\n", g_log_path);
        else       printf("[WARN] Log file open failed\n");
    }

    settings_load();

    /* Phase 2: USB mass storage — only when SD did not have the app dir.
     * USB reset already ran unconditionally above; fatMountSimple runs on a
     * clean bus.  Do NOT reset USB again here — USB_Deinitialize would close
     * the shared OH0 handle the storage driver depends on, breaking file I/O. */
    if (!sd_has_app_dir) {
        if (fatMountSimple("usb", &__io_usbstorage)) {
            printf("[OK]  USB storage mounted\n");
            DIR *_ud = opendir("usb:/apps/wii-gb-operator");
            if (_ud) {
                closedir(_ud);
                snprintf(g_app_root, sizeof(g_app_root), "usb:/apps/wii-gb-operator");
                printf("[OK]  Switched app drive to USB storage\n");
                if (g_log) { fclose(g_log); g_log = NULL; }
                snprintf(g_log_path, sizeof(g_log_path), "%s/log.txt", g_app_root);
                g_log = fopen(g_log_path, "w");
                if (g_log) printf("[OK]  Logging switched to %s\n", g_log_path);
                else       printf("[WARN] Could not open log on USB\n");
                settings_load();
            }
        }
    }

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

    // Boot-time detection used to be a single, non-retried gbop_read_cart_info()
    // call — the only cart-info call site in this codebase with zero retry
    // budget (every other one, poll_cart/run_detect_cart_inner/the USB probe,
    // retries generously given the measured ~5-20% per-attempt success rate).
    // Checked directly across six recent hardware logs (Post Firmware Update
    // Test/test_12 through test_17): the boot-time attempt came back all-zero
    // 6 times out of 6 — a ~1% event if it were just an ordinary unlucky draw
    // from the ~47% all-zero baseline measured across all attempts pooled,
    // suggesting the very first command of a session (which also triggers a
    // one-time ACK-size auto-detection probe not run on any later command) is
    // systematically worse odds than a typical later attempt, not just a
    // random miss. Regardless of the deeper cause, giving this call site the
    // same retry budget every other one already has is a well-justified fix
    // on its own. gbop_find() is kept as a fast up-front check for "is the
    // device on the USB bus at all" (a real device absence, unlike a stale
    // response, doesn't improve with retries); once that's confirmed,
    // detection itself is delegated to run_detect_cart_inner(), the same
    // 20-attempt-budgeted path a manual "Detect Cart" press already uses.
    GBOperatorHandle op = gbop_find();
    if (!op) {
        lprintf("[INFO] GB Operator not detected — entering menu (auto-detect will run on insertion)\n");
        cart_was_absent = true;
    } else {
        lprintf("[OK]  GB Operator found\n\n");
        gbop_close(op); op = NULL;
        // Real commit before a detect that can legitimately run for minutes on a
        // bad connection (multiple 20-cycle header-continuation outer attempts) —
        // without this, a genuine indefinite USB block (no interruptible/timeout
        // read API exists on this platform — see CLAUDE.md) loses everything back
        // to the last commit, which at boot could be the entire session so far.
        // Same reasoning/fix as test_23's Command Test Lab 0-byte-log finding.
        log_commit_sd();
        if (!run_detect_cart_inner(&info, rom_hdr, display_title, sizeof(display_title))) {
            lprintf("[INFO] No cart at boot — entering menu\n");
            cart_was_absent = true;
        } else {
            poll_cart_reset();
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
        "Detect Cart", "Commit Log", "Back to Main Menu", "Exit to Loader",
        "Use Old Firmware toggle", "ROM Reconcile", "Continuation Test",
        "RTC Sync Test (GBC/GBA)"
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
            ? run_dev_menu(&info, display_title, &dev_sel, cart_was_absent)
            : run_frontend(&info, display_title, n_fe, fe_items, &frontend_sel, cart_was_absent);

        if (choice == -1) break;   /* Reset pressed */

        if (choice == -2) {
            /* Poll interval elapsed — check cart presence.
             * All lprintf inside this block go to log only (not TV console). */
            g_log_suppress_console = 1;
            bool was_before = cart_was_absent;
            bool poll_changed = poll_cart(&info, &cart_was_absent);
            if (poll_changed && !g_settings.auto_detect_cart) {
                // auto_detect_cart=0: still track presence/absence above for
                // the UI (removal is handled below regardless), but never run
                // the expensive, blocking run_detect_cart_inner() on our own —
                // the whole point of this setting is that a sensed change
                // shouldn't freeze controls on its own; use manual "Detect
                // Cart" / "Detect Cart Swap" instead.
                lprintf("[poll] Cart change detected — auto-detect disabled (auto_detect_cart=0), use manual Detect Cart\n");
            } else if (poll_changed) {
                lprintf("[poll] Cart change — running auto-detect\n");
                log_commit_sd(); // see boot-time detect's comment — same hang/log-loss risk
                if (run_detect_cart_inner(&info, rom_hdr,
                                           display_title, sizeof(display_title))) {
                    cart_was_absent = false;
                    poll_cart_reset();
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

            if (choice == 8) {  /* Use Old Firmware toggle — session-only, not saved to settings.ini */
                g_settings.use_old_firmware = !g_settings.use_old_firmware;
                lprintf("[dev] use_old_firmware -> %d (session only, not saved to settings.ini)\n",
                        g_settings.use_old_firmware);
                continue;
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
                    log_commit_sd(); // see boot-time detect's comment — same hang/log-loss risk
                    if (run_detect_cart_inner(&info, rom_hdr,
                                               display_title, sizeof(display_title))) {
                        cart_was_absent = false;
                        poll_cart_reset();
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

            if (choice == 9) {  /* ROM Reconcile — see CLAUDE.md "ROM Dump Reconciliation" and
                                   * the plan it was implemented from. Captures up to
                                   * RECONCILE_MAX_SLOTS dump attempts and tries to align pairs of
                                   * them by content (source/rom_reconcile.c), assembling a
                                   * complete, verified ROM from two attempts whose gaps
                                   * complement each other — without needing a single lucky fully-
                                   * clean stream. Replaces "Command Test Lab" (per direct user
                                   * feedback that tool hadn't been a productive line of testing).
                                   * GBA-only for this first implementation; requires
                                   * info.rom_size_confirmed (an exact known size), which
                                   * rom_reconcile_align_and_merge() itself enforces. */
                #define RECONCILE_MAX_SLOTS 4
                ReconcileAttempt recon_slots[RECONCILE_MAX_SLOTS];
                memset(recon_slots, 0, sizeof(recon_slots));
                char st_slot[RECONCILE_MAX_SLOTS][64];
                for (int i = 0; i < RECONCILE_MAX_SLOTS; i++)
                    strncpy(st_slot[i], "Not captured", sizeof(st_slot[i]) - 1);
                char st_align[128] = "Not run yet";

                int recon_sel = 0;
                const int RECON_ITEMS = RECONCILE_MAX_SLOTS + 2;  /* 4 captures + Align + Back */
                static const char *recon_names[] = {
                    "Capture Attempt 1", "Capture Attempt 2", "Capture Attempt 3", "Capture Attempt 4",
                    "Align & Reconcile", "Back"
                };

                char reconcile_dir[64];
                snprintf(reconcile_dir, sizeof(reconcile_dir), "%s/reconcile", g_app_root);
                mkdir(reconcile_dir, 0755);

                // Same heap-safety discipline Command Test Lab learned the hard way
                // (test_23/24): log_commit_sd() (fclose+fopen) at most once on entry
                // and once on exit; every action inside the loop uses
                // log_force_flush() (fflush only) instead.
                log_commit_sd();

                bool recon_exit = false;
                while (!g_reset_pressed && !recon_exit) {
                    printf("\x1b[2J\x1b[H");
                    printf("ROM Reconcile (-> %s/)\n\n", reconcile_dir);
                    for (int i = 0; i < RECONCILE_MAX_SLOTS; i++)
                        printf("Slot %d : %s\n", i + 1, st_slot[i]);
                    printf("Align  : %s\n", st_align);
                    printf("\n");
                    for (int i = 0; i < RECON_ITEMS; i++)
                        printf("  %s %s\n", i == recon_sel ? ">" : " ", recon_names[i]);
                    printf("\nD-Pad: navigate   A: run   B: back\n");

                    int nav_done = 0;
                    while (!g_reset_pressed && !nav_done) {
                        VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                        u16 dp = PAD_ButtonsDown(0); u32 wp = WPAD_ButtonsDown(0);
                        if ((dp & PAD_BUTTON_B) || (wp & WPAD_BUTTON_B)) { recon_exit = true; break; }
                        if (((dp & PAD_BUTTON_UP) || (wp & WPAD_BUTTON_UP)) && recon_sel > 0) { recon_sel--; nav_done = 1; }
                        if (((dp & PAD_BUTTON_DOWN) || (wp & WPAD_BUTTON_DOWN)) && recon_sel < RECON_ITEMS - 1) { recon_sel++; nav_done = 1; }
                        if ((dp & PAD_BUTTON_A) || (wp & WPAD_BUTTON_A)) nav_done = 2;
                    }
                    if (recon_exit || nav_done != 2) continue;

                    int run_sel = recon_sel;
                    if (run_sel == RECON_ITEMS - 1) { recon_exit = true; continue; }  /* Back */
                    printf("\x1b[2J\x1b[H");

                    if (run_sel < RECONCILE_MAX_SLOTS) {
                        printf("Capturing Attempt %d...\n", run_sel + 1);
                        log_force_flush();
                        if (op) { gbop_close(op); op = NULL; }
                        reconcile_capture_slot(&info, run_sel, &recon_slots[run_sel],
                                                st_slot[run_sel], sizeof(st_slot[run_sel]));
                        printf("%s\n", st_slot[run_sel]);
                    } else {
                        /* Align & Reconcile: try every pair among the captured slots,
                         * stopping at the first that fully reconciles. Every pair's
                         * outcome is logged (including RECONCILE_REDUNDANT explicitly —
                         * per CLAUDE.md, that's the expected, useful data point this
                         * tool exists to gather, not a bug) so nothing is silently lost
                         * even when no pair succeeds. */
                        printf("Aligning captured attempts...\n");
                        log_force_flush();
                        int tried = 0, redundant = 0, gap = 0, no_align = 0, no_anchor = 0, other_err = 0;
                        bool solved = false;
                        for (int i = 0; i < RECONCILE_MAX_SLOTS && !solved; i++) {
                            if (recon_slots[i].captured_len == 0) continue;
                            for (int j = i + 1; j < RECONCILE_MAX_SLOTS && !solved; j++) {
                                if (recon_slots[j].captured_len == 0) continue;
                                tried++;
                                char out_path[160];
                                snprintf(out_path, sizeof(out_path), "%s/reconciled_%s_%s.gba",
                                         reconcile_dir, info.title[0] ? info.title : "rom",
                                         info.game_code[0] ? info.game_code : "unk");
                                ReconcileOutcome outcome;
                                ReconcileResult r = rom_reconcile_align_and_merge(
                                    &info, &recon_slots[i], &recon_slots[j], out_path, &outcome);
                                lprintf("[dev] ROM Reconcile: pair (slot %d, slot %d) -> %s\n",
                                        i + 1, j + 1, rom_reconcile_result_str(r));
                                switch (r) {
                                    case RECONCILE_OK:
                                        solved = true;
                                        snprintf(st_align, sizeof(st_align), "OK: slots %d+%d -> %s",
                                                 i + 1, j + 1, out_path);
                                        break;
                                    case RECONCILE_REDUNDANT: redundant++; break;
                                    case RECONCILE_GAP:       gap++;       break;
                                    case RECONCILE_NO_ALIGNMENT: no_align++; break;
                                    case RECONCILE_NO_ANCHOR: no_anchor++; break;
                                    default: other_err++; break;
                                }
                            }
                        }
                        if (!solved) {
                            if (tried == 0) {
                                snprintf(st_align, sizeof(st_align), "No captured attempts yet — capture at least 2");
                            } else {
                                snprintf(st_align, sizeof(st_align),
                                         "%d pair(s): %d redundant, %d gap, %d no-align, %d no-anchor, %d other",
                                         tried, redundant, gap, no_align, no_anchor, other_err);
                            }
                        }
                        printf("%s\n", st_align);
                    }

                    log_force_flush();
                    printf("\nPress A to continue.\n");
                    while (!g_reset_pressed) {
                        VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                        if ((PAD_ButtonsDown(0) & PAD_BUTTON_A) ||
                            (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) break;
                    }
                }
                log_commit_sd();  /* one real commit, only once, on the way out of this screen */
                continue;
            }

            if (choice == 10) {  /* Continuation Test — manual, single-shot exercise of
                                   * dump_rom_new_protocol() (the same function "Dump ROM" now
                                   * uses by default — see dump_rom_best()), kept as a dev-menu
                                   * item for isolated testing/inspection now that it has
                                   * graduated from experiment to the default new-firmware dump
                                   * protocol. No duplicated logic here anymore — this screen is
                                   * a thin reporting wrapper. */
                printf("\x1b[2J\x1b[H");
                if (info.rom_size_kb == 0) {
                    printf("No cart detected — detect cart first.\n");
                } else if (info.type != CART_TYPE_GBA && info.type != CART_TYPE_GBC && info.type != CART_TYPE_GB) {
                    printf("Unknown cart type — detect cart first.\n");
                } else {
                    printf("Continuation Test: up to %d automatic outer attempts (hold X+Y to abort)...\n",
                           GBOP_CONTINUATION_OUTER_ATTEMPTS);
                    log_force_flush();
                    char rom_path[256];
                    int outer_attempt = 0, cycles_used = 0;
                    int rc = dump_rom_new_protocol(&op, &info, rom_path, sizeof(rom_path),
                                                    &outer_attempt, &cycles_used,
                                                    NULL, NULL, NULL, NULL);
                    if (rc == 0) {
                        printf("SUCCESS on outer attempt %d/%d (%d cycle(s)). Saved: %s\n",
                               outer_attempt, GBOP_CONTINUATION_OUTER_ATTEMPTS, cycles_used, rom_path);
                        lprintf("[dev] Continuation Test: SUCCESS on outer attempt %d/%d (%d cycle(s)) -> %s\n",
                                outer_attempt, GBOP_CONTINUATION_OUTER_ATTEMPTS, cycles_used, rom_path);
                    } else if (outer_attempt < GBOP_CONTINUATION_OUTER_ATTEMPTS) {
                        printf("Aborted by user after %d outer attempt(s).\n", outer_attempt);
                        lprintf("[dev] Continuation Test: aborted by user after %d outer attempt(s)\n", outer_attempt);
                    } else {
                        printf("FAILED after %d outer attempt(s) (see log for detail).\n", outer_attempt);
                        lprintf("[dev] Continuation Test: FAILED after %d outer attempts\n", outer_attempt);
                    }
                }
                log_force_flush();
                printf("\nPress A to continue.\n");
                while (!g_reset_pressed) {
                    VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                    if ((PAD_ButtonsDown(0) & PAD_BUTTON_A) ||
                        (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) break;
                }
                log_commit_sd();
                continue;
            }

            if (choice == 11) {  /* RTC Sync Test — isolated, read-then-write-back
                                   * exercise of gbop_read_rtc_gbc()/gbop_write_rtc_gbc()
                                   * (GBC, MBC3+RTC) or gbop_read_rtc_gba()/
                                   * gbop_write_rtc_gba() (GBA, Seiko RTC).
                                   * Deliberately the safest possible test for GBC:
                                   * writes back EXACTLY what was just read, with no
                                   * modification or interpolation. For GBA, only the
                                   * one field this project actively computes (unix_time)
                                   * is changed — every other byte (date BCD + unresolved
                                   * misc bytes) is passed through unmodified from the
                                   * read. unix_time is deliberately NOT the Wii's
                                   * absolute wall-clock date (many Wiis, including the
                                   * dev unit, have a dead RTC/CMOS battery, making that
                                   * date wrong) — gbop_write_rtc_gba() computes it as
                                   * the read-time anchor plus real elapsed time since
                                   * the read, measured via the Wii's monotonic tick
                                   * counter, so the WRITE is always correctly offset
                                   * from the READ regardless of whether the Wii's clock
                                   * itself is right. Kept as an isolated dev-menu action
                                   * (not wired into the main save read/write flow yet)
                                   * until verified on hardware — see CLAUDE.md "Save
                                   * read/write vs. Wireshark". */
                printf("\x1b[2J\x1b[H");
                if (info.type != CART_TYPE_GBC && info.type != CART_TYPE_GBA) {
                    printf("Not a GBC or GBA cart — detect one first.\n");
                } else {
                    printf("RTC Sync Test (%s)\n-------------------\n\n",
                           info.type == CART_TYPE_GBC ? "GBC" : "GBA");
                    log_commit_sd();
                    if (op) { gbop_close(op); op = NULL; }
                    op = gbop_reopen();
                    if (!op) {
                        printf("GB Operator not found.\n");
                    } else if (info.type == CART_TYPE_GBC) {
                        GbcRtcSnapshot snap = {0};
                        int rc = gbop_read_rtc_gbc(op, &snap);
                        if (rc != 0) {
                            printf("RTC read FAILED.\n");
                            lprintf("[dev] RTC Sync Test: read failed\n");
                        } else {
                            printf("Read:  sec=%u min=%u hour=%u day_low=%u\n",
                                   snap.seconds, snap.minutes, snap.hours, snap.day_low);
                            int wrc = gbop_write_rtc_gbc(op, &snap);
                            if (wrc != 0) {
                                printf("RTC write FAILED (read was OK).\n");
                                lprintf("[dev] RTC Sync Test: write failed after successful read\n");
                            } else {
                                printf("Write: OK (wrote back exactly what was read)\n");
                                GbcRtcSnapshot verify = {0};
                                if (gbop_read_rtc_gbc(op, &verify) == 0) {
                                    printf("Verify: sec=%u min=%u hour=%u day_low=%u\n",
                                           verify.seconds, verify.minutes, verify.hours, verify.day_low);
                                    lprintf("[dev] RTC Sync Test: read/write/verify all succeeded\n");
                                } else {
                                    printf("Verify read failed (write itself reported success).\n");
                                    lprintf("[dev] RTC Sync Test: verify read failed after successful write\n");
                                }
                            }
                        }
                        gbop_close(op); op = NULL;
                    } else { /* CART_TYPE_GBA */
                        GbaRtcSnapshot snap = {0};
                        int rc = gbop_read_rtc_gba(op, &snap);
                        if (rc != 0) {
                            printf("RTC read FAILED.\n");
                            lprintf("[dev] RTC Sync Test: read failed\n");
                        } else {
                            printf("Read:  date_bcd=%02X %02X %02X %02X misc=%02X %02X %02X %02X\n",
                                   snap.date_bcd[0], snap.date_bcd[1], snap.date_bcd[2], snap.date_bcd[3],
                                   snap.misc_bytes[0], snap.misc_bytes[1], snap.misc_bytes[2], snap.misc_bytes[3]);
                            printf("Read anchor (Wii clock, may be wrong if RTC battery is dead): %u\n",
                                   (unsigned)snap.base_unix_time);
                            if (snap.base_unix_time == 0) {
                                printf("gbop_wii_unix_time() returned 0 (Wii RTC call itself failed) — not writing.\n");
                                lprintf("[dev] RTC Sync Test: base_unix_time is 0, aborting write\n");
                            } else {
                                /* gbop_write_rtc_gba() computes unix_time itself, as
                                 * base_unix_time + real elapsed time since the read
                                 * (measured via the Wii's monotonic tick counter) —
                                 * never re-queries the Wii's absolute clock. */
                                int wrc = gbop_write_rtc_gba(op, &snap);
                                if (wrc != 0) {
                                    printf("RTC write FAILED (read was OK).\n");
                                    lprintf("[dev] RTC Sync Test: write failed after successful read\n");
                                } else {
                                    printf("Write: OK (date/misc unchanged; unix_time = anchor + elapsed = %u)\n",
                                           (unsigned)snap.unix_time);
                                    GbaRtcSnapshot verify = {0};
                                    if (gbop_read_rtc_gba(op, &verify) == 0) {
                                        printf("Verify: date_bcd=%02X %02X %02X %02X misc=%02X %02X %02X %02X\n",
                                               verify.date_bcd[0], verify.date_bcd[1], verify.date_bcd[2], verify.date_bcd[3],
                                               verify.misc_bytes[0], verify.misc_bytes[1], verify.misc_bytes[2], verify.misc_bytes[3]);
                                        lprintf("[dev] RTC Sync Test: read/write/verify all succeeded\n");
                                    } else {
                                        printf("Verify read failed (write itself reported success).\n");
                                        lprintf("[dev] RTC Sync Test: verify read failed after successful write\n");
                                    }
                                }
                            }
                        }
                        gbop_close(op); op = NULL;
                    }
                }
                printf("\nPress A to continue.\n");
                while (!g_reset_pressed) {
                    VIDEO_WaitVSync(); PAD_ScanPads(); WPAD_ScanPads();
                    if ((PAD_ButtonsDown(0) & PAD_BUTTON_A) ||
                        (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) break;
                }
                log_commit_sd();
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
                } else if (!verify_rom_size_before_dump(&op, &info)) {
                    continue;
                } else if (rom_cache_exists(&info, rom_path, sizeof(rom_path))) {
                    // Reported size was corrected above and now matches a file
                    // already on SD from an earlier (already-corrected) dump.
                    lprintf("[OK]  ROM cached: %s\n", rom_path);
                } else {
                    lprintf("[INFO] Dumping ROM...\n");
                    printf("\x1b[2J\x1b[H");
                    printf("Dump ROM\n--------\n\n");
                    DumpProgressState dps = { .row = 5, .dots = 0, .bar_shown = 0 };
                    // Suppress console for the dump itself — gbop_dump_rom_continuation()
                    // logs plenty (cycle/front-check/marker lines) that would otherwise
                    // scroll straight through the fixed-position progress bar above.
                    // Every line still reaches the log file; only the TV console is
                    // muted, same mechanism run_detect_cart_inner() already uses.
                    g_log_suppress_console = 1;
                    int dump_rc = dump_rom_best(&op, &info, rom_path, sizeof(rom_path),
                                                 dump_cycle_tick, &dps,
                                                 draw_dump_progress, &dps);
                    g_log_suppress_console = 0;
                    if (dump_rc == 0) {
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
                if (dump_save_to_sd(&op, &info, display_title) == 0)
                    lprintf("[OK]  Save dump complete\n");
                else
                    lprintf("[FAIL] Save dump failed\n");
            } else {
                if (write_save_to_cart(&op, &info, upload_buf, upload_size,
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
                            if (save_read_with_retry(&op, &info, vb, vs, NULL, NULL) == 0) {
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

        /* Play Game (0) — confirm the currently-detected title first. Detect
         * is manual-only now (auto_detect_cart defaults off — see CLAUDE.md),
         * so "the detected game" can silently go stale if the player swapped
         * carts without pressing "Detect Cart Swap" again.
         *
         * An earlier version of this also ran a full re-verify detect after
         * every confirmation, before touching any ROM/save data. Reverted
         * (2026-08-01, direct hardware feedback, Save Read Write Test/
         * test_6): even the "same_cart fingerprint fast path" this relied on
         * still needs a real cart-info exchange to succeed at all, and this
         * connection's own per-attempt reliability is often poor enough that
         * a single cart-info check alone can need 10+ retries — meaning
         * "Play Game" visibly ran a full Detect-Cart-length operation on
         * every press, even when the cart hadn't changed at all. Decision:
         * the confirmation prompt IS the guard now — no automatic hardware
         * re-check. Two narrower, opportunistic checks (in play_game()
         * itself) catch the two mismatches that are cheaply detectable from
         * operations that already have to happen anyway (a fresh ROM dump's
         * real title, and the pre-save-read cart-info's real type) and
         * escalate to a real detect only when one of those actually fires —
         * see CLAUDE.md for the full reasoning and the accepted residual
         * risk (a same-type/same-size swap, e.g. Ruby ↔ Sapphire, isn't
         * caught by either check — the player's own judgement, plus GB/C's
         * manual-only sync and GBA's teardown never forcing an extra write
         * on quit, are what limit the blast radius there). */
        if (choice == 0) {
            if (info.rom_size_kb > 0 || info.title[0]) {
                char confirm_msg[128];
                snprintf(confirm_msg, sizeof(confirm_msg), "Play %s?", display_title);
                if (!prompt_yesno(confirm_msg)) continue;
            }
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
                log_commit_sd(); // see boot-time detect's comment — same hang/log-loss risk
                if (run_detect_cart_inner(&info, rom_hdr,
                                           display_title, sizeof(display_title))) {
                    cart_was_absent = false;
                    poll_cart_reset();
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
            if (!verify_rom_size_before_dump(&op, &info)) continue;
            if (rom_cache_exists(&info, rom_path, sizeof(rom_path))) {
                // Reported size was corrected above and now matches a file
                // already on SD from an earlier (already-corrected) dump.
                lprintf("[OK]  ROM cached: %s\n", rom_path);
                gbop_close(op); op = NULL;
                show_message("ROM already on SD card.");
                continue;
            }
            lprintf("[INFO] Dumping ROM...\n");
            printf("\x1b[2J\x1b[H\n\n");
            cprint("Dump ROM");
            printf("\n");
            DumpProgressState dps = { .row = 6, .dots = 0, .bar_shown = 0 };
            // See the matching dev-menu Dump ROM comment: suppress console
            // (log file unaffected) for the duration of the dump so its own
            // diagnostic lines don't scroll through the fixed-position
            // progress bar above.
            g_log_suppress_console = 1;
            int dump_rc = dump_rom_best(&op, &info, rom_path, sizeof(rom_path),
                                         dump_cycle_tick, &dps,
                                         draw_dump_progress, &dps);
            g_log_suppress_console = 0;
            if (dump_rc == 0) {
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
            if (dump_save_to_sd(&op, &info, display_title) == 0) {
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
