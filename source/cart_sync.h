#pragma once
#include <stdint.h>
#include "gb_operator.h"

/* States visible to the frontend for the overlay notification. */
typedef enum {
    CART_SYNC_IDLE = 0,
    CART_SYNC_IN_PROGRESS,
    CART_SYNC_SUCCESS,   /* shown for ~2 s then cleared back to IDLE */
    CART_SYNC_FAILED,    /* retrying */
} CartSyncState;

/* Start the background LWP thread. Must be called once before any save event.
 * info must remain valid for the lifetime of the thread (static or long-lived).
 * save_path: SD path for the .sav file; synced to SD after each successful cart
 * write (so the main thread is never blocked by SD I/O during emulation).
 * on_sync_success: optional callback invoked on the sync thread after each
 * successful write (SD and cart both done). May be NULL. */
void cart_sync_init(const CartInfo *info, const char *save_path,
                    void (*on_sync_success)(const void *buf, uint32_t sz));

/* Signal the sync thread with a new save buffer. Copies buf internally so
 * the caller may free it immediately. size must equal info->ram_size_kb*1024. */
void cart_sync_queue(const void *buf, uint32_t size);

/* Return the current sync state (safe to call from the main thread). */
CartSyncState cart_sync_state(void);

/* Human-readable status string for the overlay. Never NULL. */
const char *cart_sync_status_str(void);

/* Stop the thread and release all resources. Call before exiting emulation. */
void cart_sync_shutdown(void);
