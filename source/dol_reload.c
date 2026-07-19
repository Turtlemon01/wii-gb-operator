#include "dol_reload.h"
#include "log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ogc/machine/processor.h>

extern char __sbss_start[];  /* start of .sbss (small BSS, r13-relative statics) */
extern char __bss_start[];   /* start of .bss  (large zero-init statics) */
extern char __bss_end[];     /* end   of .bss  */
/* Non-static in mgba_frontend.c so we can save/restore it across the BSS zero.
 * Guards the single-call-per-lifetime GX_Init(); must remain true after reload. */
extern bool g_gx_initialized;

/* NOTE: on Wii (devkitPPC), the initial stack pointer falls *inside* the BSS address
 * range [__bss_start, __bss_end).  The stack and BSS share the same memory region
 * with the stack growing downward from a point inside that range.  This means that
 * local variables in dol_reload() are on the stack, which is inside the BSS region,
 * so memset(BSS) zeroes them too.  Any value that must survive the BSS zero MUST be
 * stored in .data (not BSS, not the stack) before the memset. */

/* These two variables live in .data so the BSS memset cannot reach them.
 * Non-zero initialisers ensure the compiler emits them into .data, not .bss. */
static uint32_t s_initial_sp  __attribute__((section(".data"))) = UINT32_MAX;
static bool     s_preserve_gx __attribute__((section(".data"))) = true;

void dol_capture_entry(void) {
    uint32_t sp;
    __asm__ volatile ("mr %0, 1" : "=r"(sp));
    s_initial_sp = sp;
}

void dol_reload(void) {
    /* TV console is live (mgba_frontend teardown called console_init + VIDEO_Configure).
     * lprintf goes to both TV and SD file (before fclose below). */
    lprintf("[reload] sp=0x%08X sbss=%p bss=[%p,%p)\n",
            (unsigned)s_initial_sp,
            (void *)__sbss_start, (void *)__bss_start, (void *)__bss_end);

    /* fflush commits buffered bytes; do NOT call log_commit_sd (fclose+fopen):
     * core->deinit corrupted dlmalloc's free list — fopen/malloc on that heap
     * crashes with DAR=0x10000000 (test_84/test_85/test_86). */
    log_force_flush();
    /* Do NOT fclose(g_log): fclose calls free() on the FILE's buffer/struct, and
     * free() coalesces with adjacent blocks — if core->deinit corrupted those block
     * headers, free() follows a bad pointer → DSI (confirmed test_88).
     * log_force_flush() already committed all data to SD. Just null the pointer;
     * the BSS zero below wipes dlmalloc state clean so no leak persists. */
    g_log = NULL;
    printf("[reload] pre-zero (sbss+bss)\n");
    fflush(stdout);

    if (s_initial_sp == UINT32_MAX) return;  /* capture was never called */

    /* Save the GX guard to .data BEFORE zeroing .sbss+.bss.
     * The GX CP stays armed to s_gxfifo_buf (.data, fixed address) between
     * sessions; calling GX_Init again on an armed CP causes red-screen crashes.
     * s_preserve_gx is in .data so the memset below cannot zero it. */
    s_preserve_gx = g_gx_initialized;

    u32 level;
    _CPU_ISR_Disable(level);
    (void)level;

    /* Zero BOTH .sbss AND .bss.
     *
     * Previously we only zeroed .bss [__bss_start, __bss_end), leaving .sbss
     * untouched. libogc's s_firstThread (LWP thread-list head) lives in .sbss;
     * after the .bss zero it still pointed into the now-zeroed TCB pool.
     * LWP_CreateThread in session 2 walked that dangling pointer → DSI crash.
     * postRetraceCB (VI retrace callback) is also in .sbss; a stale function
     * pointer there crashes as soon as VIDEO_Init re-enables VI interrupts.
     *
     * Zeroing from __sbss_start clears all of these. The only .sbss value that
     * must survive is g_gx_initialized, which we restore from s_preserve_gx
     * (.data) immediately after the memset. */
    memset(__sbss_start, 0, (size_t)(__bss_end - __sbss_start));

    /* Restore GX guard from .data (survives the memset above). */
    g_gx_initialized = s_preserve_gx;

    /* Branch to main(0, NULL):
     *   1. Re-enable external interrupts (MSR[EE] bit 16 = 0x8000).
     *      .sbss is now zeroed so VI callbacks are NULL — no stale handler.
     *      r11 is used as scratch for the MSR write (caller-saved, safe).
     *   2. Reset stack pointer to initial height (from .data, not zeroed).
     *   Hardware stays initialised; only .sbss+.bss and heap are wiped. */
    register uint32_t sp_val __asm__("r12") = s_initial_sp;
    __asm__ volatile (
        "mfmsr 11\n"
        "ori   11, 11, 0x8000\n"   /* set MSR[EE] — re-enable external interrupts */
        "mtmsr 11\n"
        "isync\n"
        "mr    1, 12\n"            /* reset SP */
        "li    3, 0\n"             /* argc = 0 */
        "li    4, 0\n"             /* argv = NULL */
        "b     main\n"
        :: "r"(sp_val) : "memory", "r11"
    );
    __builtin_unreachable();
}
