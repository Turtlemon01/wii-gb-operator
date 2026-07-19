#pragma once

/* dol_reload — restart the app in-place after an mGBA session.
 *
 * dol_capture_entry(): call once at startup (before any mGBA session) to
 *   record the stack pointer at that point.
 *
 * dol_reload(): zeros BSS (resetting dlmalloc and all NULL-init statics),
 *   resets the stack pointer to its original height, then branches directly
 *   to main(0, NULL).  Hardware (video, controllers, IOS) stays initialised
 *   from the first run — only the heap and statics are wiped clean.  Never
 *   returns on success.  Falls through silently if dol_capture_entry was
 *   not called (e.g. SD unavailable at startup).
 *
 * Why not jump to the CRT entry point?  The CRT calls SYSTEM_Init() which
 * reinitialises IPC/IOS on hardware that is already live → DSI crash.
 */
void dol_capture_entry(void);
void dol_reload(void);
