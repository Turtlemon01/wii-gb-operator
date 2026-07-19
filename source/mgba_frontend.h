#pragma once
#include <stdint.h>
#include "gb_operator.h"

/* Launch the mGBA emulation loop.
 *
 * info      — cart info from gbop_read_cart_info(); used for save-to-cart sync
 * rom_path  — absolute path to the ROM file on SD (e.g. sd:/apps/.../roms/GAME.gba)
 * save_path — absolute path to the save file, or NULL for "new game / no save"
 * save_kb   — save size in KB (from CartInfo.ram_size_kb); 0 if no save
 *
 * Blocks until the player presses the Wii Reset button (or the ROM crashes).
 * Returns 0 on clean exit, -1 on load failure.
 *
 * The caller must NOT hold the GB Operator handle; the cart sync background
 * thread inside this module will open it when needed.
 */
int mgba_run(const CartInfo *info, const char *rom_path, const char *save_path, uint32_t save_kb);
