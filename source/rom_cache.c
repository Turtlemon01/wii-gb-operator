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

// Prefer SD; fall back to USB if SD file doesn't exist.
static void resolve_path(const CartInfo *info, char *out, int size) {
    build_path(info, ROM_CACHE_DIR_SD, out, size);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return; }
    build_path(info, ROM_CACHE_DIR_USB, out, size);
}

int rom_cache_exists(const CartInfo *info, char *path_out, int path_size) {
    resolve_path(info, path_out, path_size);

    FILE *f = fopen(path_out, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long actual = ftell(f);
    fclose(f);

    long expected = (long)info->rom_size_kb * 1024;
    return (actual == expected) ? 1 : 0;
}

int rom_cache_dump(GBOperatorHandle handle, const CartInfo *info,
                   char *path_out, int path_size) {
    // Try SD first, fall back to USB
    build_path(info, ROM_CACHE_DIR_SD, path_out, path_size);
    mkdir(ROM_CACHE_DIR_SD, 0755);

    FILE *f = fopen(path_out, "wb");
    if (!f) {
        mkdir(ROM_CACHE_DIR_USB, 0755);
        build_path(info, ROM_CACHE_DIR_USB, path_out, path_size);
        f = fopen(path_out, "wb");
        if (!f) {
            printf("[cache] ERROR: cannot open %s for writing\n", path_out);
            return -1;
        }
    }

    uint32_t total    = info->rom_size_kb * 1024;
    uint8_t *chunk    = malloc(DUMP_CHUNK_SIZE);
    if (!chunk) { fclose(f); return -1; }

    int result   = 0;
    int aborted  = 0;
    uint32_t offset = 0;

    printf("Dumping %u KB... (hold X+Y on GC controller to abort)\n", info->rom_size_kb);
    lprintf("[cache] Dump started: %u KB → %s\n", info->rom_size_kb, path_out);

    while (offset < total) {
        uint32_t to_read = (total - offset < DUMP_CHUNK_SIZE)
                         ? (total - offset)
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
        printf("\r  %u / %u KB", offset / 1024, info->rom_size_kb);
        fflush(stdout);

        // Abort if the user holds X+Y on GameCube controller port 0
        PAD_ScanPads();
        if ((PAD_ButtonsHeld(0) & (PAD_BUTTON_X | PAD_BUTTON_Y)) ==
                                   (PAD_BUTTON_X | PAD_BUTTON_Y)) {
            lprintf("\n[cache] Aborted by user at offset 0x%08X (%u / %u KB)\n",
                    offset, offset / 1024, info->rom_size_kb);
            aborted = 1;
            result  = -1;
            break;
        }
    }

    printf("\n");
    fclose(f);
    free(chunk);

    if (result != 0) {
        // Keep the partial file on disk for inspection.
        // Log its path before clearing it so the user can find it.
        lprintf("[cache] Partial file kept at: %s\n", path_out);
        if (!aborted)
            lprintf("[cache] Dump failed — check log for USB errors above\n");
        path_out[0] = '\0';
    }
    return result;
}
