#include "cart_sync.h"
#include "gb_operator.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/semaphore.h>
#include <unistd.h>
#include <stdbool.h>

#define RETRY_DELAY_US      3000000  /* 3 s between retries (broken into 10ms slices) */
/* libogc default LWP stack is 8 KB which is too small for gbop_write_save (USB
 * IOS calls consume 1-2 KB each on the call stack).  32 KB gives safe headroom. */
#define SYNC_THREAD_STACK_SIZE  (32 * 1024)
#define SHUTDOWN_TIMEOUT    450      /* max 450 × 10ms = 4.5s wait in cart_sync_shutdown */
/* After a successful write, ignore new requests for this long. Prevents games
 * with a hardware RTC (e.g. Pokemon Silver) from hammering the cartridge every
 * second. The SD save file is already authoritative; cart sync is best-effort. */
#define POST_WRITE_COOLDOWN_US  10000000  /* 10 s cooldown (broken into 10ms slices) */

/* 2-second success display: main loop calls cart_sync_state() at ~60 fps */
#define SUCCESS_DISPLAY_FRAMES 120

static const CartInfo    *s_info       = NULL;
static uint8_t           *s_buf        = NULL;
static uint32_t           s_buf_size   = 0;
static char               s_save_path[256] = {0};
static void (*s_on_sync_success_cb)(const void *buf, uint32_t sz) = NULL;
static volatile int       s_pending    = 0;
static volatile int       s_shutdown   = 0;
static volatile int       s_thread_done = 0; /* set by thread before it returns */

static CartSyncState      s_state      = CART_SYNC_IDLE;
static uint32_t           s_frame_cnt  = 0;

/* Sticky SUCCESS: set when a write completes, cleared after SUCCESS_DISPLAY_FRAMES.
 * Survives even if a new IN_PROGRESS overwrites s_state immediately, ensuring the
 * indicator always shows green for the full display window. */
static volatile int      s_sticky_success       = 0;
static          uint32_t s_sticky_success_frame = 0;

static uint8_t s_thread_stack[SYNC_THREAD_STACK_SIZE] __attribute__((aligned(8)));
static lwp_t   s_thread = LWP_THREAD_NULL;
static mutex_t s_mutex  = LWP_MUTEX_NULL;
static sem_t   s_sem    = LWP_SEM_NULL;

/* Log a state change (call under no lock needed — called only from sync thread). */
static void log_state(CartSyncState prev, CartSyncState next) {
    if (prev == next) return;
    const char *names[] = {"IDLE", "IN_PROGRESS", "SUCCESS", "FAILED"};
    lprintf("[sync] state: %s → %s\n",
            prev < 4 ? names[prev] : "?",
            next < 4 ? names[next] : "?");
}

static void set_state(CartSyncState next) {
    log_state(s_state, next);
    s_state = next;
}

static void *sync_thread_func(void *arg) {
    (void)arg;
    while (!s_shutdown) {
        LWP_SemWait(s_sem);
        if (s_shutdown) break;

        LWP_MutexLock(s_mutex);
        int has_data = s_pending;
        s_pending = 0;
        LWP_MutexUnlock(s_mutex);

        if (!has_data) continue;

        set_state(CART_SYNC_IN_PROGRESS);

        int ok = 0;
        while (!ok && !s_shutdown) {
            GBOperatorHandle op = gbop_reopen();
            if (op) {
                int r = gbop_write_save(op, s_info, s_buf, s_buf_size);
                gbop_close(op);
                if (r == 0) {
                    ok = 1;
                } else {
                    lprintf("[sync] Write failed (r=%d), retrying\n", r);
                    set_state(CART_SYNC_FAILED);
                    /* Interruptible sleep: 10ms slices so s_shutdown is checked promptly */
                    for (int i = 0; i < RETRY_DELAY_US / 10000 && !s_shutdown; i++)
                        usleep(10000);
                }
            } else {
                lprintf("[sync] gbop_reopen failed, retrying\n");
                set_state(CART_SYNC_FAILED);
                for (int i = 0; i < RETRY_DELAY_US / 10000 && !s_shutdown; i++)
                    usleep(10000);
            }
        }

        if (ok) {
            /* Notify frontend of successful write (updates snapshot, sets saved flag).
             * Called even during shutdown so s_player_saved_ingame is set correctly. */
            if (s_on_sync_success_cb)
                s_on_sync_success_cb(s_buf, s_buf_size);

            /* SD write: skip if shutdown is in progress.
             * When the main thread calls cart_sync_shutdown(), it sets s_shutdown=1 then
             * waits up to 4.5s for this thread to exit.  If we call fopen/fwrite/fclose
             * here while the main thread simultaneously calls fclose(g_log) inside
             * log_commit_sd(), both threads hit libfat concurrently → deadlock.
             * Skipping the SD write on shutdown is safe: the main thread's teardown
             * does its own final SD write (guarded by s_player_saved_ingame). */
            if (!s_shutdown && s_save_path[0]) {
                FILE *sf = fopen(s_save_path, "wb");
                if (sf) {
                    size_t written = fwrite(s_buf, 1, s_buf_size, sf);
                    fclose(sf);
                    lprintf("[sync] SD save written: %zu / %u bytes → %s\n",
                            written, s_buf_size, s_save_path);
                } else {
                    lprintf("[sync] SD save write FAILED: cannot open %s\n", s_save_path);
                }
                lprintf("[sync] Cart upload complete\n");
            }
            s_sticky_success_frame = s_frame_cnt;
            __asm__ __volatile__("" ::: "memory");
            s_sticky_success = 1;
            set_state(CART_SYNC_SUCCESS);

            /* Cooldown: drain semaphore posts that accumulated during the write,
             * then ignore further requests for POST_WRITE_COOLDOWN_US.  This
             * suppresses continuous re-syncs from games with a real-time clock
             * (e.g. Pokemon Silver) whose savedataUpdated fires every second.
             * s_shutdown is checked every 10 ms so quit-to-menu remains fast. */
            if (!s_shutdown) {
                LWP_MutexLock(s_mutex);
                s_pending = 0;  /* discard saves queued during the write */
                LWP_MutexUnlock(s_mutex);
                for (int ci = 0; ci < POST_WRITE_COOLDOWN_US / 10000 && !s_shutdown; ci++)
                    usleep(10000);
                /* After cooldown, drain any accumulated semaphore posts without writing. */
                LWP_MutexLock(s_mutex);
                s_pending = 0;
                LWP_MutexUnlock(s_mutex);
            }
        }
    }

    s_thread_done = 1;
    return NULL;
}

void cart_sync_init(const CartInfo *info, const char *save_path,
                    void (*on_sync_success)(const void *buf, uint32_t sz)) {
    s_info        = info;
    s_buf_size    = info->ram_size_kb * 1024;
    s_buf         = (uint8_t *)malloc(s_buf_size);
    s_state       = CART_SYNC_IDLE;
    s_pending     = 0;
    s_shutdown    = 0;
    s_thread_done = 0;
    s_frame_cnt   = 0;
    s_sticky_success = 0;
    s_on_sync_success_cb = on_sync_success;
    if (save_path) {
        strncpy(s_save_path, save_path, sizeof(s_save_path) - 1);
        s_save_path[sizeof(s_save_path) - 1] = '\0';
    } else {
        s_save_path[0] = '\0';
    }

    LWP_MutexInit(&s_mutex, FALSE);
    LWP_SemInit(&s_sem, 0, 32);
    LWP_CreateThread(&s_thread, sync_thread_func, NULL,
                     s_thread_stack, SYNC_THREAD_STACK_SIZE, 40);
    lprintf("[sync] Cart sync thread started\n");
}

void cart_sync_queue(const void *buf, uint32_t size) {
    if (!s_buf || size != s_buf_size) return;
    LWP_MutexLock(s_mutex);
    memcpy(s_buf, buf, size);
    s_pending = 1;
    LWP_MutexUnlock(s_mutex);
    LWP_SemPost(s_sem);
    lprintf("[sync] Save queued\n");
}

CartSyncState cart_sync_state(void) {
    s_frame_cnt++;

    /* Sticky SUCCESS: force-show SUCCESS for the full display window even if
     * the sync thread immediately started a new IN_PROGRESS write. */
    if (s_sticky_success) {
        uint32_t elapsed = s_frame_cnt - s_sticky_success_frame;
        if (elapsed <= SUCCESS_DISPLAY_FRAMES)
            return CART_SYNC_SUCCESS;
        s_sticky_success = 0;  /* window expired */
    }

    /* Normal auto-clear of SUCCESS state */
    if (s_state == CART_SYNC_SUCCESS &&
        (s_frame_cnt - s_sticky_success_frame) > SUCCESS_DISPLAY_FRAMES) {
        set_state(CART_SYNC_IDLE);
    }

    return s_state;
}

const char *cart_sync_status_str(void) {
    switch (s_state) {
        case CART_SYNC_IN_PROGRESS: return "Syncing...";
        case CART_SYNC_SUCCESS:     return "Saved";
        case CART_SYNC_FAILED:      return "Sync failed - retrying";
        default:                    return "Idle";
    }
}

void cart_sync_shutdown(void) {
    if (s_thread == LWP_THREAD_NULL) return;

    s_shutdown = 1;
    LWP_SemPost(s_sem);  /* wake thread if waiting on semaphore */

    /* Poll for thread exit (up to SHUTDOWN_TIMEOUT × 10ms = 3s).
     * USB_ReadBlkMsg(60) inside gbop_write_save can block indefinitely;
     * LWP_JoinThread would hang forever in that case.  We give the thread a
     * reasonable window — if the transfer completes or times out, it exits.
     * If not, we proceed anyway: the thread becomes harmless once emulation
     * stops queuing saves, and will exit when its current USB call finishes. */
    for (int i = 0; i < SHUTDOWN_TIMEOUT && !s_thread_done; i++)
        usleep(10000);

    if (s_thread_done) {
        LWP_JoinThread(s_thread, NULL);
        LWP_SemDestroy(s_sem);
        LWP_MutexDestroy(s_mutex);
        lprintf("[sync] Cart sync thread joined cleanly\n");
        free(s_buf);
        s_buf = NULL;
    } else {
        lprintf("[sync] Cart sync thread did not exit within timeout — abandoning\n");
        /* Do NOT destroy mutex/semaphore or free s_buf: thread is still blocked
         * in USB_ReadBlkMsg and may access s_buf when IOS eventually returns.
         * Freeing s_buf here causes a use-after-free crash when USB unblocks.
         * Intentional memory leak: app exits immediately after. */
        s_buf = NULL;  /* null our pointer so cart_sync_queue is a no-op from now on */
    }

    s_thread = LWP_THREAD_NULL;
}
