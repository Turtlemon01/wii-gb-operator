#pragma once
#include <stdint.h>

#define GBOP_VID 0x16D0   // Epilogue GB Operator (confirmed from Wii USB enumeration)
#define GBOP_PID 0x123D

typedef enum {
    CART_TYPE_UNKNOWN = 0,
    CART_TYPE_GB,
    CART_TYPE_GBC,
    CART_TYPE_GBA,
} CartType;

typedef struct {
    char     title[17];       // null-terminated, from cart header
    char     game_code[5];    // GBA code or GB old licensee
    CartType type;
    char     type_str[8];     // "GB", "GBC", or "GBA"
    uint32_t rom_size_kb;
    uint32_t ram_size_kb;
    uint8_t  raw_resp[60];    // raw 60-byte device response from gbop_read_cart_info
} CartInfo;

typedef void *GBOperatorHandle;

// Return codes for gbop_read_cart_info
#define GBOP_OK      0    // success — cart present and parsed
#define GBOP_NOCART  (-1) // device responded, but no cart inserted (resp[3:5]==0)
#define GBOP_USB     (-2) // USB transport failure (send or recv stall)

// Returns NULL if the GB Operator is not found on USB
GBOperatorHandle gbop_find(void);

// Re-opens after gbop_close() without probing. Use when cart info is already
// known and only a fresh USB handle is needed for a dump or save command.
GBOperatorHandle gbop_reopen(void);

// Reads the cart header and populates CartInfo. Returns 0 on success.
int gbop_read_cart_info(GBOperatorHandle handle, CartInfo *out);


// Reads the full ROM in chunks into buffer. buffer must be rom_size_kb * 1024 bytes.
// Returns 0 on success.
int gbop_dump_rom(GBOperatorHandle handle, const CartInfo *info,
                  uint8_t *buffer, uint32_t buffer_size);

// Reads the first 512 bytes of ROM for header identification (title, game code,
// CGB flag). Must be called on a FRESHLY OPENED handle (gbop_reopen() after
// gbop_close() on the cart-info handle). Running it on the warm cart-info handle
// gives garbage data for GB/GBC carts (test_97). hdr_out must be 512 bytes.
// Returns 0 on success. Caller closes the handle after this call.
int gbop_read_rom_header(GBOperatorHandle handle, uint8_t *hdr_out);

// Reads SRAM/save data. Returns 0 on success.
int gbop_read_save(GBOperatorHandle handle, const CartInfo *info,
                   uint8_t *buffer, uint32_t buffer_size);

// Writes SRAM/save data back to cart. Returns 0 on success.
int gbop_write_save(GBOperatorHandle handle, const CartInfo *info,
                    const uint8_t *buffer, uint32_t buffer_size);

void gbop_close(GBOperatorHandle handle);

// Returns the IOS USB fd for an open handle. Used by main.c to detect
// USB re-enumeration (physical cart reinsertion) between mini-dump calls.
// Within one physical USB connection all gbop_reopen() calls return the same
// fd; a new fd only appears after the device resets on USB re-enumeration.
int32_t gbop_get_fd(GBOperatorHandle handle);
