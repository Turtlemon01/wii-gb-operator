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
    int      rom_size_confirmed; // 1 = rom_size_kb came from an exact rom_sizes.c table
                                  // lookup; 0 = generous unrecognized-game fallback guess.
                                  // See gb_operator.c newproto_check_early_footer() — an
                                  // early device footer is expected/normal against a
                                  // fallback guess, but real corruption against a confirmed
                                  // exact size (Post Firmware Update Test/test_25).
    int      ram_size_confirmed; // same, for ram_size_kb
    uint8_t  raw_resp[60];    // raw 60-byte device response from gbop_read_cart_info
} CartInfo;

typedef void *GBOperatorHandle;

// Return codes for gbop_read_cart_info
#define GBOP_OK      0    // success — cart present and parsed
#define GBOP_NOCART  (-1) // device responded, but no cart inserted (resp[3:5]==0)
#define GBOP_USB     (-2) // USB transport failure (send or recv stall)
// Historical: previously returned when the new-firmware path's cart-info
// header didn't look like a C0 DE marker. No longer returned as of the
// "header is optional" rework (Post Firmware Update Test/test_3) — a
// missing/dropped header is now handled by treating that packet as the
// data directly instead of failing, since hardware testing showed the
// header can be dropped by the connection even when a cart is genuinely
// present. Kept defined for API stability / in case a future firmware
// revision needs a real mismatch signal again; not currently produced by
// gb_operator.c. use_old_firmware remains a manual settings.ini / dev-menu-
// toggle choice only — see CLAUDE.md "Old-firmware support decision".
#define GBOP_FIRMWARE_MISMATCH (-3)

// Returns NULL if the GB Operator is not found on USB
GBOperatorHandle gbop_find(void);

// Re-opens after gbop_close() without probing. Use when cart info is already
// known and only a fresh USB handle is needed for a dump or save command.
GBOperatorHandle gbop_reopen(void);

// Repeatedly calls gbop_reopen() (60ms between tries, up to 75 tries — ~1-3s)
// until it returns a handle whose fd differs from old_fd, i.e. IOS has
// re-enumerated and handed out a fresh fd after the previous one was spent.
// If the fd never cycles after all 75 tries (some d2x-cIOS builds never
// cycle it), the last handle obtained is returned anyway so the caller can
// proceed rather than fail outright; *out_same_fd_fallback (if non-NULL) is
// set to 1 in that case. Pass any value that cannot equal a real fd (e.g. a
// value from before the first open) as old_fd to accept the first handle
// obtained. Returns NULL only if gbop_reopen() never succeeds across all tries.
GBOperatorHandle gbop_reopen_wait_fresh(int32_t old_fd, int *out_same_fd_fallback);

// True once this session has directly observed the fd surviving a full
// gbop_reopen_wait_fresh() exhaustion without ever changing — real evidence
// this connection never cycles the fd, not a guess. Other call sites with
// their own separate "wait for a fresh fd" loop (e.g. dump_rom_with_retry()
// in main.c) can check this to skip straight to a single fast reopen
// instead of re-discovering the same non-result on their own budget.
int gbop_fd_known_unstable(void);

// Records the same observation gbop_reopen_wait_fresh() would have recorded
// itself — for call sites with their OWN separate "wait for fresh fd" loop
// (dump_rom_with_retry() in main.c) so their exhaustions also feed the
// shared flag, not just gbop_reopen_wait_fresh()'s own. Without this, a
// session where only the separate loop ever runs would never set the flag
// at all, and would keep re-discovering the same non-result on every call
// (confirmed: Rom Stitching Test/test_7 — every single exhaustion this
// session cost the full ~2.5s, none of them skipped, because nothing had
// ever called gbop_reopen_wait_fresh() itself to set the flag).
void gbop_mark_fd_unstable(void);

// Reads the cart header and populates CartInfo. Returns 0 on success.
int gbop_read_cart_info(GBOperatorHandle handle, CartInfo *out);

// Called once at the start of every cycle inside gbop_read_rom_header_continuation()
// or gbop_dump_rom_continuation() — purely a UI hook so a caller can show
// live "still working" feedback (a dot per cycle, say) instead of leaving
// the screen static for however long it takes to reach a working attempt.
// Never affects retry/return logic.
typedef void (*GbopProgressCB)(void *ctx);

// Called periodically (every 512KB, GBOP_DUMP_PROGRESS_CHUNK) during
// gbop_dump_rom_continuation()'s main transfer — a UI hook for showing live
// dump progress (a progress bar, say). Unlike GbopProgressCB above (which
// fires on every cycle attempt, success or failure), this only fires once a
// cycle has already passed its marker+front checks and started real
// streaming — see gbop_dump_rom_continuation()'s cycle_cb parameter for the
// "attempts are being made" signal that covers the (usually much longer)
// time before that point. given/total are bytes, not KB. Never affects
// retry/return logic.
typedef void (*GbopByteProgressCB)(uint32_t given, uint32_t total, void *ctx);

// Continuation dump — the primary new-firmware ROM dump mechanism (see
// gb_operator.c for the full rationale and hardware-confirmed history).
// Instead of aborting the instant a bad front is detected, drains the
// current stream to its own footer and retries a fresh command on the SAME
// still-open handle (no close/reopen), up to max_cycles times. GBA and
// GB/GBC. buffer must be exactly info->rom_size_kb*1024 bytes (the whole
// ROM in one call, not chunked). *out_cycles_used (optional) reports how
// many cycles were needed. cycle_cb/cycle_ctx: optional, called once per
// cycle attempt regardless of outcome — see GbopProgressCB above.
// byte_progress_cb/byte_progress_ctx: optional, see GbopByteProgressCB
// above. Returns 0 on full success, -1 otherwise.
int gbop_dump_rom_continuation(GBOperatorHandle handle, const CartInfo *info,
                                uint8_t *buffer, uint32_t buffer_size,
                                int max_cycles, int *out_cycles_used,
                                GbopProgressCB cycle_cb, void *cycle_ctx,
                                GbopByteProgressCB byte_progress_cb, void *byte_progress_ctx);


// Reads the full ROM in chunks into buffer. buffer must be rom_size_kb * 1024 bytes.
// Returns 0 on success.
int gbop_dump_rom(GBOperatorHandle handle, const CartInfo *info,
                  uint8_t *buffer, uint32_t buffer_size);

// Verifies a GBA cart's reported ROM size against the cart's own hardware
// mirroring behaviour (reading past a real GBA ROM's physical capacity wraps
// the address lines back to 0), and corrects info->rom_size_kb in place if
// the reported size is found to be exactly double the real size. Only runs
// the (costly, ~half-ROM) check when the cart-info response showed the
// anomalous byte pattern already associated with every confirmed case of
// this bug; otherwise returns 0 immediately without touching the device.
// *out_did_stream (if non-NULL) is set to 1 only when the function actually
// entered the streaming loop (both gates passed), 0 otherwise — callers use
// this to know whether the handle was left mid-stream (see below) or is
// still exactly as it was on entry, since a call that never streamed
// anything has no reason to pay for a close+reopen. Streams via the same
// ROM-read command as gbop_dump_rom() when it does run, so a call that DID
// stream always leaves the device mid-stream: the caller MUST gbop_close()
// the handle and obtain a fresh one (see gbop_reopen_wait_fresh) before any
// further command in that case, regardless of the return value below.
// Returns 1 if info->rom_size_kb was corrected, 0 otherwise (including when
// the check did not need to run, or aborted on a read error).
int gbop_verify_gba_rom_size(GBOperatorHandle handle, CartInfo *info, int *out_did_stream);

// Reads the first 512 bytes of ROM for header identification (title, game code,
// CGB flag). Must be called on a FRESHLY OPENED handle (gbop_reopen() after
// gbop_close() on the cart-info handle). Running it on the warm cart-info handle
// gives garbage data for GB/GBC carts (test_97). hdr_out must be 512 bytes.
// Returns 0 on success. Caller closes the handle after this call.
int gbop_read_rom_header(GBOperatorHandle handle, uint8_t *hdr_out);

// Continuation-style header peek — the gbop_dump_rom_continuation() mechanism
// (same-handle drain-and-retry instead of close/reopen per attempt) applied
// to a small prefix of the header (192 bytes GBA / 384 bytes GB/GBC as of
// 2026-07-31 — shrunk from a flat 512, see gb_operator.c for why and the
// open question this is testing) used for cart detection and region
// confirmation. New-firmware protocol only — callers must gate on
// !g_settings.use_old_firmware themselves (this function has no legacy
// equivalent, same as gbop_dump_rom_continuation()).
// type: the cart type from a prior successful cart-info read (selects the
// GBA vs GB/GBC front-check probe size). hdr_out must be 512 bytes.
// *out_cycles_used (optional) reports how many cycles were needed.
// progress_cb/progress_ctx: optional, called once per cycle — see
// GbopProgressCB above. Returns 0 on a validated header, -1 otherwise.
// Leaves the device mid-stream on success, same contract as
// gbop_read_rom_header() — caller closes the handle before any further
// command.
int gbop_read_rom_header_continuation(GBOperatorHandle handle, CartType type,
                                       uint8_t *hdr_out, int max_cycles,
                                       int *out_cycles_used,
                                       GbopProgressCB progress_cb, void *progress_ctx);

// True if hdr (a 512-byte ROM header buffer) passes the same hardware-mandated
// checksum a real GBA BIOS or DMG/CGB boot ROM validates before it will boot
// the cartridge — see gb_operator.c for the exact GBA/GB layouts checked.
// Exposed for source/rom_reconcile.c to gate a reconciled ROM before it's
// trusted as output.
int header_checksum_valid(const uint8_t *hdr);

// Returns the longest exactly-matching prefix length between `buf` (length
// `avail`) and the fixed, universal Nintendo logo, at the logo's fixed
// header offset for this cart type (GBA: 0x04; GB/GBC: 0x104). See
// gb_operator.c for the full rationale and provenance of the hardcoded
// bytes. Exposed for source/rom_reconcile.c's anchor scoring.
uint32_t gbop_logo_prefix_match_len(CartType type, const uint8_t *buf, uint32_t avail);

// Minimum matched-byte threshold for treating a gbop_logo_prefix_match_len()
// result as a confirmed match rather than coincidence (32 bytes against a
// fixed constant is astronomically unlikely by chance).
#define GBOP_LOGO_MIN_BYTES 32

// Reads SRAM/save data. Returns 0 on success.
int gbop_read_save(GBOperatorHandle handle, const CartInfo *info,
                   uint8_t *buffer, uint32_t buffer_size);

// Writes SRAM/save data back to cart. Returns 0 on success.
int gbop_write_save(GBOperatorHandle handle, const CartInfo *info,
                    const uint8_t *buffer, uint32_t buffer_size);

// GBC (MBC3+RTC) RTC snapshot — the cart's own raw RTC registers (seconds,
// minutes, hours, day_counter_low), NOT BCD, NOT tied to any external clock.
// See gb_operator.c's "RTC read/write" section for the full derivation and
// why there is deliberately no GBA equivalent yet.
typedef struct {
    uint32_t seconds, minutes, hours, day_low;
} GbcRtcSnapshot;

// Reads the cart's current MBC3 RTC registers (cmd 0x09). GBC/MBC3+RTC only
// (e.g. Pokemon Gold/Silver/Crystal) — do not call for GBA or non-RTC carts.
// Returns 0 on success.
int gbop_read_rtc_gbc(GBOperatorHandle handle, GbcRtcSnapshot *out);

// Writes RTC registers back to the cart (cmd 0x10). Intended usage is
// read-then-write-back via gbop_read_rtc_gbc() moments earlier — there is no
// independent time source involved, this only preserves/restores whatever
// the cart itself already reports. Returns 0 on success.
int gbop_write_rtc_gbc(GBOperatorHandle handle, const GbcRtcSnapshot *snap);

// GBA (Seiko RTC, e.g. Ruby/Sapphire/Emerald) RTC snapshot. date_bcd[0..3]
// and misc_bytes[0..3] (device bytes 4..7, one of which — device byte 7 — is
// a separately-noted constant) are opaque, passed-through-unchanged bytes
// read directly from the cart (date/weekday BCD fields plus bytes whose
// exact meaning is still unresolved — see gb_operator.c's "RTC read/write"
// section) — this struct exists to carry them between a read and a write,
// not to interpret them.
//
// unix_time (device bytes 8..11 on write; not meaningfully present on read,
// so gbop_read_rtc_gba() leaves it at 0) is deliberately NOT taken directly
// from the Wii's own absolute wall-clock date — many Wiis (including the
// one this project is developed on) have a dead RTC/CMOS backup battery,
// making the Wii's own idea of "today's date" wrong. Instead,
// gbop_write_rtc_gba() computes it as base_unix_time + (real elapsed time
// since the read, measured via the Wii's monotonic CPU tick counter, which
// keeps running correctly regardless of the dead battery) — the same
// "read, then interpolate forward by elapsed time" strategy already used
// for GBC (see gbop_write_rtc_gbc()). The absolute date written may still
// be wrong (it inherits whatever wrong starting point the Wii's RTC gave at
// read time), but the WRITE is always correctly offset from the READ by the
// real amount of time that actually passed — the part that matters for any
// in-game logic that compares two of the cart's own RTC readings.
typedef struct {
    uint8_t  date_bcd[4];
    uint8_t  misc_bytes[4];
    uint32_t unix_time;       // computed by gbop_write_rtc_gba() — do not set directly
    uint32_t base_unix_time;  // anchor captured at read time (gbop_wii_unix_time()); may be a wrong absolute date, only used as a stable starting point
    uint64_t read_tick;       // gettime() ticks captured at read time — monotonic, battery-independent
} GbaRtcSnapshot;

// Reads the cart's current RTC block (cmd 0x09) and captures a monotonic
// time anchor (base_unix_time/read_tick) for gbop_write_rtc_gba() to
// interpolate forward from. GBA (Seiko RTC) only — do not call for GBC/GB
// carts. Returns 0 on success.
int gbop_read_rtc_gba(GBOperatorHandle handle, GbaRtcSnapshot *out);

// Writes the RTC block back to the cart (cmd 0x10). date_bcd/misc_bytes are
// passed through unchanged from the prior gbop_read_rtc_gba() call (their
// exact meaning is unresolved, so this project never modifies them).
// unix_time is computed here — NOT taken from the Wii's absolute clock a
// second time — as base_unix_time + real elapsed time since read_tick (see
// GbaRtcSnapshot), and written back into snap->unix_time so the caller can
// log the value actually sent. Returns 0 on success.
int gbop_write_rtc_gba(GBOperatorHandle handle, GbaRtcSnapshot *snap);

// Returns the current Unix timestamp (seconds since 1970-01-01 UTC) as the
// Wii's own hardware RTC reports it, via the undocumented-in-headers-but-
// real libogc symbol __SYS_GetRTC(). See gb_operator.c for the sourcing/
// citation of this symbol. Returns 0 if the underlying call fails outright.
//
// CAVEAT: this is only as accurate as the Wii's own battery-backed RTC
// chip. A Wii (or GameCube) with a dead CMOS/RTC backup battery reports a
// wrong absolute date/time from this call — that is a real, common
// hardware condition, not a rare edge case. Callers should use this only as
// a monotonic-forward-progressing anchor (see GbaRtcSnapshot/
// gbop_write_rtc_gba above), never assume the absolute value is correct.
uint32_t gbop_wii_unix_time(void);

void gbop_close(GBOperatorHandle handle);

// Returns the IOS USB fd for an open handle. Used by main.c to detect
// USB re-enumeration (physical cart reinsertion) between mini-dump calls.
// Within one physical USB connection all gbop_reopen() calls return the same
// fd; a new fd only appears after the device resets on USB re-enumeration.
int32_t gbop_get_fd(GBOperatorHandle handle);

// Runs a series of USB probing tests to characterise this hardware's IOS/USB
// behaviour.  Results go to the log file and TV console.  Requires a cart to
// be inserted so ROM/save tests can run.  All USB opens/closes are internal —
// caller does not need an open handle.
void gbop_probe_hardware(const CartInfo *info);

// Attempts to drain any stray packets already sitting in the EP IN queue
// before the caller sends its own command — e.g. leftovers from an earlier,
// unrelated exchange that IOS never flushed on close/reopen (a recurring,
// long-documented source of cross-exchange contamination in this project).
// Reads and discards up to max_attempts packets, logging each one (rd value
// + short content preview); stops early once it sees a few ZLPs/empty reads
// in a row (a best-effort signal the queue has gone quiet), but is always
// capped at max_attempts regardless — this deliberately never tries to loop
// "until truly empty", since some reads on this hardware are documented to
// block indefinitely if nothing is queued at all, and there's no safe way to
// distinguish that from "still draining" without a hard ceiling. Not a
// guarantee of true silence, just a safe, bounded best effort at one.
void gbop_drain_stray(GBOperatorHandle handle, int max_attempts);

// Sends a bare command byte (rest of payload zero, CRC appended as usual)
// and captures raw response bytes into buf_out with zero interpretation —
// no marker/header/footer parsing, no drain logic. For the dev-menu
// "Command Test Lab": exercising individual protocol commands in isolation,
// including ones with no higher-level implementation yet (e.g. 0x15
// "Get Info" — see CLAUDE.md Sources, kantosCoder/FreeGbOperatorFirm10Py).
// Stops at buf_size bytes captured or after a bounded stall (never loops
// forever) — worst case ~25-30s at this connection's typical per-read cost.
// Also checks X+Y (GC controller port 0) between reads and aborts early if
// held, same convention as rom_cache_stream_chunks()/dump_rom_new_protocol().
// Returns bytes captured (may be less than buf_size — not itself an error,
// since seeing exactly what happens is the point), -1 if the command failed
// to send, or -2 if the user aborted.
int gbop_raw_command_capture(GBOperatorHandle handle, uint8_t cmd,
                              uint8_t *buf_out, uint32_t buf_size);
