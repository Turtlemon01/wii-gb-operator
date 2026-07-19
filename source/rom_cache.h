#pragma once
#include "gb_operator.h"

// Where ROM dumps are stored on SD/USB.
// fatInitDefault() mounts SD as "sd:/" and USB as "usb:/"
#define ROM_CACHE_DIR_SD  "sd:/apps/wii-gb-operator/roms"
#define ROM_CACHE_DIR_USB "usb:/apps/wii-gb-operator/roms"

// Checks whether a cached ROM file exists for this cart and its size is correct.
// Fills path_out with the file path. Returns 1 if valid cache hit, 0 otherwise.
int rom_cache_exists(const CartInfo *info, char *path_out, int path_size);

// Dumps the ROM from the cart via GB Operator and saves it to SD/USB.
// Fills path_out with the saved file path on success. Returns 0 on success.
int rom_cache_dump(GBOperatorHandle handle, const CartInfo *info,
                   char *path_out, int path_size);
