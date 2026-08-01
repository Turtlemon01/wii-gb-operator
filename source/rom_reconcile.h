#pragma once
#include <stdint.h>
#include "gb_operator.h"

// ROM dump reconciliation — see CLAUDE.md "ROM Dump Reconciliation" and the
// plan this was implemented from for the full design rationale. Short
// version: a rejected dump attempt's captured bytes are always genuine,
// correct ROM content (never garbled) forming one contiguous run starting
// at some true-ROM offset S (usually 0, but can be >0 for a front-shifted
// attempt — see gb_operator.c's newproto_is_header()/test_25 history).
// Two attempts whose gaps land in different places can, together, cover
// the whole ROM without needing a single lucky fully-clean stream — IF we
// can prove where they overlap. This module never guesses: every function
// here either produces a result backed by an exact, validated byte match,
// or fails cleanly with a specific reason.

typedef struct {
    char     path[128];
    uint32_t captured_len;
} ReconcileAttempt;

// slot is just a numbered scratch filename (reconcile/attempt_<slot>.bin) —
// not capped in this API. The dev-menu screen exposes a small fixed number
// of capture buttons; capture itself is cheap (streams straight to an SD
// file via rom_cache_stream_chunks(), no large RAM needed), so there's no
// reason to hard-limit how many attempts get recorded even though alignment
// itself only ever operates on two at a time (see rom_reconcile_align_and_merge).
//
// Returns 0 = full clean capture (captured_len == total, no reconciliation
//             needed at all — just use this file directly),
//         1 = short prefix captured (expected/common case),
//        -1 = nothing usable captured at all (captured_len == 0), or the
//             handle/file could not be used.
int rom_reconcile_capture(GBOperatorHandle handle, const CartInfo *info,
                          int slot, ReconcileAttempt *out);

typedef enum {
    RECONCILE_OK = 0,
    RECONCILE_SIZE_NOT_CONFIRMED,  // info->rom_size_confirmed == 0 — refuse up front
    RECONCILE_NO_ANCHOR,           // neither attempt's own header validates
    RECONCILE_REDUNDANT,           // both attempts agree at shift 0 (same start
                                   // point) — their union adds nothing beyond
                                   // the longer one; need an attempt with a
                                   // genuinely different start offset, not just
                                   // another capture. This is the *expected*
                                   // outcome for the now-confirmed-common case
                                   // of two tail-cuts sharing the same
                                   // dominant, apparently deterministic gap
                                   // (see CLAUDE.md test_25-test_34 tally).
    RECONCILE_NO_ALIGNMENT,        // shift-0 didn't hold; searched for a real
                                   // shift, found no confident unique match
    RECONCILE_GAP,                 // a genuine non-zero shift was confirmed,
                                   // but the union still leaves a hole
    RECONCILE_HEADER_FAILED,       // assembled buffer failed the final header
                                   // gate (defense in depth; should be
                                   // unreachable given the zero-tolerance
                                   // overlap validation, but never skipped)
    RECONCILE_IO_ERROR,           // malloc/file I/O failure
} ReconcileResult;

typedef struct {
    ReconcileResult result;
    uint32_t gap_start, gap_end;   // valid only on RECONCILE_GAP (gap_end exclusive)
    uint32_t other_shift;          // valid only on RECONCILE_OK — the non-anchor
                                    // attempt's true-ROM start offset
    uint32_t confirmed_overlap;    // valid only on RECONCILE_OK — bytes of
                                    // exact agreement the shift was proven on
} ReconcileOutcome;

// Order of a/b does not matter — the anchor is chosen internally by which
// attempt's own header validates (header_checksum_valid()), not by argument
// position. On RECONCILE_OK, the reconciled ROM (info->rom_size_kb * 1024
// bytes) is written to out_path.
ReconcileResult rom_reconcile_align_and_merge(const CartInfo *info,
                                               const ReconcileAttempt *a,
                                               const ReconcileAttempt *b,
                                               const char *out_path,
                                               ReconcileOutcome *outcome);

// Human-readable label for a ReconcileResult, for dev-menu status lines/logs.
const char *rom_reconcile_result_str(ReconcileResult result);
