#pragma once
#include <stdio.h>
#include <stdarg.h>

/* Current log file path — set by main.c after log rotation.
 * Used by log_commit_sd() to reopen the correct file after fclose. */
extern char g_log_path[64];

// Shared log file handle — set by main.c after SD mount, read by all modules
extern FILE *g_log;

/* Set to 1 during GX emulation to suppress console vprintf output.
 * The console shares xfb with GX; writing to it during emulation corrupts video. */
extern int g_log_suppress_console;

static inline void lprintf(const char *fmt, ...) {
    va_list args;
    if (!g_log_suppress_console) {
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    if (g_log) {
        va_start(args, fmt);
        vfprintf(g_log, fmt, args);
        va_end(args);
        fflush(g_log);
    }
}

/* Flush stdio buffer only — g_log stays valid.
 * libfat may not commit dirty sectors to physical SD until fclose. */
static inline void log_force_flush(void) {
    if (g_log) fflush(g_log);
}

/* Sector-commit: fclose forces libfat to write dirty sectors to physical SD,
 * then fopen reopens for continued logging.  If fopen fails g_log becomes NULL
 * (console-only) but the next log_commit_sd call will attempt recovery.
 *
 * MUST NOT be called while the cart_sync LWP thread is alive — the sync thread
 * calls lprintf (→ vfprintf on g_log) concurrently, and fclose racing with
 * vfprintf causes a heap use-after-free on the FILE's internal buffer, eventually
 * corrupting the malloc free list.  Use log_force_flush() during emulation
 * (safe for concurrent callers because fflush does not free the FILE struct). */
static inline void log_commit_sd(void) {
    if (!g_log_path[0]) return;
    if (g_log) fclose(g_log);
    g_log = fopen(g_log_path, "a");
}
