#include "cartindex.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Fingerprint: resp[2..59] = 58 bytes = 116 hex chars.
 * resp[0..1] are protocol header bytes (excluded).
 * All remaining bytes are included so that any game-specific field in
 * the device response — known or unknown — contributes to discrimination.
 * If a particular byte turns out to be volatile between insertions of the
 * same cart, narrow the range; for now wider is safer. */
#define FP_START 2
#define FP_END   60   /* exclusive */
#define FP_BYTES (FP_END - FP_START)
#define FP_HEX   (FP_BYTES * 2)   /* 116 chars */

static void make_fp(const CartInfo *info, char fp[FP_HEX + 1]) {
    for (int i = 0; i < FP_BYTES; i++)
        snprintf(fp + i * 2, 3, "%02X", info->raw_resp[FP_START + i]);
    fp[FP_HEX] = '\0';
}

/* Line format (2026-07-31): fingerprint|rom_basename|type_str|game_code|title|rom_size_kb|ram_size_kb
 * Older lines (pre-2026-07-31) are just fingerprint|rom_basename — parsed
 * fine here too, just with identity_known left at 0. Fields after
 * rom_basename may individually be empty (e.g. game_code for a plain GB
 * cart) but the pipe count still distinguishes "recorded, just blank" from
 * "never recorded" (old-format line) via extra_fields_present below. */
static int parse_index_line(char *line, char fp_out[FP_HEX + 1], CartIndexEntry *out) {
    char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
    char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
    if (line[0] == '#' || line[0] == '\0') return 0;

    char *fields[7] = {0};
    int nf = 0;
    char *cursor = line;
    fields[nf++] = cursor;
    while (nf < 7) {
        char *pipe = strchr(cursor, '|');
        if (!pipe) break;
        *pipe = '\0';
        cursor = pipe + 1;
        fields[nf++] = cursor;
    }
    if (nf < 2) return 0; /* need at least fingerprint|rom_basename */

    strncpy(fp_out, fields[0], FP_HEX);
    fp_out[FP_HEX] = '\0';

    memset(out, 0, sizeof(*out));
    strncpy(out->rom_basename, fields[1], sizeof(out->rom_basename) - 1);

    if (nf >= 7) {
        out->identity_known = 1;
        strncpy(out->type_str, fields[2], sizeof(out->type_str) - 1);
        out->type = (strcmp(out->type_str, "GBA") == 0) ? CART_TYPE_GBA
                  : (strcmp(out->type_str, "GBC") == 0) ? CART_TYPE_GBC
                  : (strcmp(out->type_str, "GB") == 0)  ? CART_TYPE_GB
                  : CART_TYPE_UNKNOWN;
        strncpy(out->game_code, fields[3], sizeof(out->game_code) - 1);
        strncpy(out->title, fields[4], sizeof(out->title) - 1);
        out->rom_size_kb = (uint32_t)strtoul(fields[5], NULL, 10);
        out->ram_size_kb = (uint32_t)strtoul(fields[6], NULL, 10);
    }
    return 1;
}

int cartindex_lookup(const CartInfo *info, CartIndexEntry *out, int max) {
    if (!info || !out || max <= 0) return 0;

    char index_path[64];
    snprintf(index_path, sizeof(index_path), "%s/cartindex.ini", g_app_root);
    char fp[FP_HEX + 1];
    make_fp(info, fp);

    FILE *f = fopen(index_path, "r");
    if (!f) return 0;

    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && n < max) {
        char line_fp[FP_HEX + 1];
        CartIndexEntry entry;
        if (!parse_index_line(line, line_fp, &entry)) continue;
        if (strcmp(line_fp, fp) != 0) continue;
        out[n++] = entry;
    }
    fclose(f);
    lprintf("[index] Lookup: %d match(es)\n", n);
    return n;
}

void cartindex_update(const CartInfo *info, const char *rom_basename) {
    if (!info || !rom_basename || !rom_basename[0]) return;

    char index_path[64];
    snprintf(index_path, sizeof(index_path), "%s/cartindex.ini", g_app_root);
    char fp[FP_HEX + 1];
    make_fp(info, fp);

    /* Check for exact duplicate (same fingerprint + same basename + same
     * identity already recorded) before appending — a repeat call with a
     * newly-learned identity (e.g. this cart was indexed filename-only by
     * an older build, and is now being re-recorded with full identity)
     * should still append a fresh, fuller line rather than being treated
     * as a no-op duplicate of the old, thinner one. */
    FILE *f = fopen(index_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char line_fp[FP_HEX + 1];
            CartIndexEntry entry;
            if (!parse_index_line(line, line_fp, &entry)) continue;
            if (strcmp(line_fp, fp) == 0 && strcmp(entry.rom_basename, rom_basename) == 0
                && entry.identity_known) {
                fclose(f);
                lprintf("[index] Already indexed: %s\n", rom_basename);
                return;
            }
        }
        fclose(f);
    }

    /* Append; create file with header comment if it doesn't exist yet */
    FILE *fa = fopen(index_path, "a");
    if (!fa) {
        fa = fopen(index_path, "w");
        if (fa) fprintf(fa, "# wii-gb-operator cart index — built automatically\n"
                            "# fingerprint|rom_basename|type|game_code|title|rom_size_kb|ram_size_kb\n");
    }
    if (fa) {
        fprintf(fa, "%s|%s|%s|%s|%s|%u|%u\n", fp, rom_basename,
                info->type_str, info->game_code, info->title,
                info->rom_size_kb, info->ram_size_kb);
        fflush(fa);
        fclose(fa);
        lprintf("[index] Saved: %s\n", rom_basename);
    } else {
        lprintf("[index] ERROR: could not write index\n");
    }
}
