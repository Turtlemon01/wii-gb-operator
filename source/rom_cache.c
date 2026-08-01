#include "rom_cache.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ogc/pad.h>

#define DUMP_CHUNK_SIZE 4096

// Build a safe filename from the cart title and game code.
static void build_path(const CartInfo *info, const char *base_dir,
                        char *out, int size) {
    char safe_title[32] = {0};
    int j = 0;
    for (int i = 0; info->title[i] && j < 31; i++) {
        char c = info->title[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            safe_title[j++] = c;
        } else if (c == ' ' && j > 0) {
            safe_title[j++] = '_';
        }
    }
    const char *ext = (info->type == CART_TYPE_GBA) ? "gba"
                    : (info->type == CART_TYPE_GBC) ? "gbc" : "gb";
    snprintf(out, size, "%s/%s_%s.%s", base_dir, safe_title, info->game_code, ext);
}

// Try g_app_root/roms first; fall back to the other drive in case ROMs were
// cached in a previous session on a different storage device.
static void resolve_path(const CartInfo *info, char *out, int size) {
    char roms_dir[64];
    snprintf(roms_dir, sizeof(roms_dir), "%s/roms", g_app_root);
    build_path(info, roms_dir, out, size);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return; }
    // Cross-drive fallback
    const char *alt = (g_app_root[0] == 'u')
                    ? "sd:/apps/wii-gb-operator/roms"
                    : "usb:/apps/wii-gb-operator/roms";
    build_path(info, alt, out, size);
}

int rom_cache_exists(const CartInfo *info, char *path_out, int path_size) {
    resolve_path(info, path_out, path_size);

    FILE *f = fopen(path_out, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long actual = ftell(f);
    fclose(f);

    // expected > 0 is required, not just actual == expected: on the
    // new-firmware protocol info->rom_size_kb is legitimately 0 at the point
    // try_enrich_info() calls this (cart-info never populates size — see
    // rom_sizes.h — enrichment happens later), so expected would be 0 too.
    // Without this guard, any leftover 0-byte file from a dump that failed
    // before writing its first byte (confirmed on hardware, Post Firmware
    // Update Test/test_9: a dropped dump-start marker leaves a 0-byte file)
    // reads as a false cache "hit" for every subsequent detect of ANY cart,
    // short-circuiting the real ROM-header enrichment path and permanently
    // reporting the wrong title/code/size from that point on.
    long expected = (long)info->rom_size_kb * 1024;
    return (expected > 0 && actual == expected) ? 1 : 0;
}

int rom_cache_stream_chunks(GBOperatorHandle handle, const CartInfo *info,
                             FILE *f, uint32_t total_size, uint32_t *out_written) {
    uint8_t *chunk = malloc(DUMP_CHUNK_SIZE);
    if (!chunk) { *out_written = 0; return -1; }

    int result = 0;
    int aborted = 0;
    uint32_t offset = 0;

    printf("Dumping %u KB... (hold X+Y on GC controller to abort)\n", total_size / 1024);

    while (offset < total_size) {
        uint32_t to_read = (total_size - offset < DUMP_CHUNK_SIZE)
                         ? (total_size - offset)
                         : DUMP_CHUNK_SIZE;

        if (gbop_dump_rom(handle, info, chunk, to_read) != 0) {
            lprintf("[cache] ERROR: read failed at offset 0x%08X (%u KB)\n",
                    offset, offset / 1024);
            result = -1;
            break;
        }

        if (fwrite(chunk, 1, to_read, f) != to_read) {
            lprintf("[cache] ERROR: write failed at offset 0x%08X (%u KB)\n",
                    offset, offset / 1024);
            result = -1;
            break;
        }

        offset += to_read;
        printf("\r  %u / %u KB", offset / 1024, total_size / 1024);
        fflush(stdout);

        // Abort if the user holds X+Y on GameCube controller port 0
        PAD_ScanPads();
        if ((PAD_ButtonsHeld(0) & (PAD_BUTTON_X | PAD_BUTTON_Y)) ==
                                   (PAD_BUTTON_X | PAD_BUTTON_Y)) {
            lprintf("\n[cache] Aborted by user at offset 0x%08X (%u / %u KB)\n",
                    offset, offset / 1024, total_size / 1024);
            aborted = 1;
            result  = -1;
            break;
        }
    }

    printf("\n");
    free(chunk);
    *out_written = offset;
    return aborted ? -2 : result;
}

int rom_cache_dump(GBOperatorHandle handle, const CartInfo *info,
                   char *path_out, int path_size) {
    char roms_dir[64];
    snprintf(roms_dir, sizeof(roms_dir), "%s/roms", g_app_root);
    mkdir(roms_dir, 0755);
    build_path(info, roms_dir, path_out, path_size);

    FILE *f = fopen(path_out, "wb");
    if (!f) {
        printf("[cache] ERROR: cannot open %s for writing\n", path_out);
        return -1;
    }

    uint32_t total = info->rom_size_kb * 1024;
    lprintf("[cache] Dump started: %u KB → %s\n", info->rom_size_kb, path_out);

    uint32_t offset = 0;
    int result = rom_cache_stream_chunks(handle, info, f, total, &offset);
    int aborted = (result == -2);
    if (result != 0) result = -1;  // -2 (abort) folds into the same "failed" return as before

    fclose(f);

    if (result != 0) {
        // Keep the partial file on disk for inspection.
        // Log its path before clearing it so the user can find it.
        lprintf("[cache] Partial file kept at: %s\n", path_out);
        if (!aborted)
            lprintf("[cache] Dump failed — check log for USB errors above\n");

        // dump_rom_with_retry() (main.c) reopens this same path_out with
        // fopen(..., "wb") on its very next attempt, truncating whatever was
        // just written here. A dump that got rejected near the very end
        // (the early/late-footer corruption checks in gb_operator.c only
        // ever fire once np_given has nearly or fully reached np_total) can
        // represent almost the entire real ROM — exactly the bytes needed
        // to compare against a reference Wireshark capture and pin down
        // *where* a gap actually occurred, e.g. front-of-stream vs mid-file.
        // Without this, that data is silently destroyed the moment the next
        // retry attempt starts, even though this attempt streamed
        // successfully almost all the way through. Marker-mismatch failures
        // (by far the most common failure mode) abort at offset 0 before a
        // single chunk is written, so this never fires for those — only for
        // a rejection that actually got into streaming.
        if (!aborted && offset > 0) {
            char debug_path[80];
            snprintf(debug_path, sizeof(debug_path), "%s/_debug_last_rejected.bin", roms_dir);
            FILE *src = fopen(path_out, "rb");
            FILE *dst = src ? fopen(debug_path, "wb") : NULL;
            if (src && dst) {
                uint8_t copy_buf[4096];
                size_t n;
                while ((n = fread(copy_buf, 1, sizeof(copy_buf), src)) > 0) fwrite(copy_buf, 1, n, dst);
                lprintf("[cache] Preserved %u bytes of rejected dump for diagnostics: %s\n",
                        offset, debug_path);
            } else {
                lprintf("[cache] Could not preserve rejected dump for diagnostics (open failed)\n");
            }
            if (src) fclose(src);
            if (dst) fclose(dst);
        }

        path_out[0] = '\0';
    }
    return result;
}
