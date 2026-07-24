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
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';
        if (strcmp(line, fp) != 0) continue;

        strncpy(out[n].rom_basename, sep + 1, sizeof(out[n].rom_basename) - 1);
        out[n].rom_basename[sizeof(out[n].rom_basename) - 1] = '\0';
        n++;
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

    /* Check for exact duplicate before appending */
    FILE *f = fopen(index_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
            char *sep = strchr(line, '|');
            if (!sep) continue;
            *sep = '\0';
            if (strcmp(line, fp) == 0 && strcmp(sep + 1, rom_basename) == 0) {
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
                            "# fingerprint|rom_basename\n");
    }
    if (fa) {
        fprintf(fa, "%s|%s\n", fp, rom_basename);
        fflush(fa);
        fclose(fa);
        lprintf("[index] Saved: %s\n", rom_basename);
    } else {
        lprintf("[index] ERROR: could not write index\n");
    }
}
