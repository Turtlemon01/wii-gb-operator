#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <ogc/lwp_watchdog.h>

/* Current log file path — set by main.c after log rotation.
 * Used by log_commit_sd() to reopen the correct file after fclose. */
extern char g_log_path[64];

// Shared log file handle — set by main.c after SD mount, read by all modules
extern FILE *g_log;

/* Set to 1 during GX emulation to suppress console vprintf output.
 * The console shares xfb with GX; writing to it during emulation corrupts video. */
extern int g_log_suppress_console;

/* App directory root — set at startup by main() based on which drive has the
 * apps/wii-gb-operator folder: "sd:/apps/wii-gb-operator" or "usb:/apps/wii-gb-operator".
 * All modules derive their file paths from this. */
extern char g_app_root[32];

/* gettime() captured once at the very start of main(), before anything else
 * runs. Every lprintf() line is prefixed with elapsed time since this point
 * (see below) so a future hardware log can be compared directly against the
 * Wireshark captures in CLAUDE.md ("Firmware-V10_0_10-WireShark") — e.g.
 * Epilogue Playback's own cart-info round trip is a consistent ~600-700us
 * there. Without a reference point on the Wii side there was previously no
 * way to tell whether a given attempt's actual USB round-trip time looks
 * anything like that, or is much slower — a real, testable explanation for
 * why the same device/cart that Epilogue reads reliably on nearly every
 * poll fails most attempts here (Post Firmware Update Test/test_6 through
 * test_10 confirmed the device itself is not the problem). */
extern u64 g_log_t0;

static inline void lprintf(const char *fmt, ...) {
    va_list args;
    u64 us = ticks_to_microsecs(gettime() - g_log_t0);
    if (!g_log_suppress_console) {
        printf("[%6llu.%03llums] ", (unsigned long long)(us / 1000), (unsigned long long)(us % 1000));
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    if (g_log) {
        fprintf(g_log, "[%6llu.%03llums] ", (unsigned long long)(us / 1000), (unsigned long long)(us % 1000));
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
