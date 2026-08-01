#include "rom_reconcile.h"
#include "rom_cache.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---- Shift/sign convention -------------------------------------------------
// Let S = an attempt's true-ROM start offset (0 normally, >0 for a
// front-shifted attempt), so buf[i] == trueROM[S+i] for all valid i.
// The anchor attempt always has S_anchor = 0 (verified via
// header_checksum_valid() before anything else runs, never assumed from
// capture order — see test_25 in CLAUDE.md for why capture order alone is
// not trustworthy). For the "other" attempt, a sample taken at anchor
// offset a0 (so sample == trueROM[a0..a0+W)) that is found at unique
// position j0 in other's buffer means other[j0..j0+W) == trueROM[a0..a0+W),
// and since other[j] == trueROM[S_other+j], S_other + j0 == a0, i.e.:
//     d = a0 - j0     (NOT j0 - a0)
// other then covers true-ROM range [d, d+L_other). d is always computed in
// a signed 64-bit type and validated (d >= 0, d + L_other <= total) BEFORE
// ever being used as a buffer offset — a sign error or spurious match
// producing a "negative" shift must never silently wrap into a huge
// unsigned value and turn into an out-of-bounds memcpy.

#define RECONCILE_SAMPLE_WINDOW   256
#define RECONCILE_SAMPLE_TRIES    8
#define RECONCILE_MIN_OVERLAP     4096  // matches DUMP_CHUNK_SIZE in rom_cache.c

// Logo tables and gbop_logo_prefix_match_len() now live in gb_operator.c/.h
// (shared with the fast front-check inside gbop_dump_rom_new()) — see that
// file for the full provenance/verification comment. GBOP_LOGO_MIN_BYTES is
// defined there too.

// 2 = full header checksum passed (strongest -- the complete header region
//     is present and internally self-consistent per the hardware-mandated
//     checksum, exactly what header_checksum_valid() has always checked).
// 1 = partial Nintendo-logo-prefix match passed (not enough captured data
//     for the full checksum, but enough of the fixed, universal logo bytes
//     are present and match exactly -- see GBOP_LOGO_MIN_BYTES). Uses the
//     longest-matching-prefix semantics of gbop_logo_prefix_match_len(), not
//     an all-or-nothing full-window match -- an attempt genuinely correct
//     for its first 64 bytes and then diverging (an internal gap right past
//     the entry vector, confirmed shape in Rom Stitching Test test_1/test_3)
//     still gets credited for those 64 bytes instead of scoring 0.
// 0 = neither check could confirm this attempt as a valid true-offset-0
//     anchor from the bytes available.
// header_checksum_valid() is only invoked once `hdr_len` covers its highest
// index (0x14D, the GB/GBC offset -- larger than GBA's 0xBD) -- hdr is
// always a fixed-size, zero-initialized stack buffer in the caller, so this
// guard is about not scoring on uninitialized/never-actually-captured
// bytes, not about an out-of-bounds read.
static int anchor_score(CartType type, const uint8_t *hdr, uint32_t hdr_len) {
    if (hdr_len >= 0x14E && header_checksum_valid(hdr)) return 2;
    if (gbop_logo_prefix_match_len(type, hdr, hdr_len) >= GBOP_LOGO_MIN_BYTES) return 1;
    return 0;
}

// ---- "Marked jump points" (session-local hint list) ------------------------
// The dominant real-world failure is a deterministic, address-tied gap that
// recurs at the same position for a given cart (CLAUDE.md: the PC-side
// 4x-repeat Wireshark capture found the identical gap in all 4 repeats of
// the same ROM; the test_25-34 tally found 9 of 10 real gap events shared
// the identical shortfall). Once this process has seen a gap boundary (from
// a RECONCILE_GAP result) or the boundary where a shift-0 redundant pair's
// data runs out (RECONCILE_REDUNDANT), later alignment calls in the same
// session try those remembered offsets first, before falling back to blind
// even-spacing. This never changes correctness -- every candidate is still
// subject to the same uniqueness check and zero-tolerance full-overlap
// validation as any other -- it only changes which offsets get tried
// first, so a familiar gap position resolves faster instead of waiting on
// even-spaced luck to land near it.
#define RECONCILE_KNOWN_GAPS_MAX 8
static uint32_t s_known_gaps[RECONCILE_KNOWN_GAPS_MAX];
static int s_known_gaps_count = 0;

static void record_known_gap(uint32_t off) {
    for (int i = 0; i < s_known_gaps_count; i++) {
        if (s_known_gaps[i] == off) return;
    }
    if (s_known_gaps_count < RECONCILE_KNOWN_GAPS_MAX) {
        s_known_gaps[s_known_gaps_count++] = off;
    } else {
        memmove(&s_known_gaps[0], &s_known_gaps[1], (RECONCILE_KNOWN_GAPS_MAX - 1) * sizeof(uint32_t));
        s_known_gaps[RECONCILE_KNOWN_GAPS_MAX - 1] = off;
    }
}

const char *rom_reconcile_result_str(ReconcileResult result) {
    switch (result) {
        case RECONCILE_OK:                 return "OK";
        case RECONCILE_SIZE_NOT_CONFIRMED: return "Size not confirmed for this game";
        case RECONCILE_NO_ANCHOR:          return "Neither attempt's header validates";
        case RECONCILE_REDUNDANT:          return "Both attempts share the same start (redundant)";
        case RECONCILE_NO_ALIGNMENT:       return "No confident alignment found";
        case RECONCILE_GAP:                return "Aligned, but a gap remains";
        case RECONCILE_HEADER_FAILED:      return "Assembled ROM failed final header check";
        case RECONCILE_IO_ERROR:           return "I/O error";
        default:                           return "Unknown";
    }
}

int rom_reconcile_capture(GBOperatorHandle handle, const CartInfo *info,
                          int slot, ReconcileAttempt *out) {
    if (!handle || !info || !out) return -1;

    char reconcile_dir[64];
    snprintf(reconcile_dir, sizeof(reconcile_dir), "%s/reconcile", g_app_root);
    mkdir(reconcile_dir, 0755);

    snprintf(out->path, sizeof(out->path), "%s/attempt_%d.bin", reconcile_dir, slot);
    out->captured_len = 0;

    FILE *f = fopen(out->path, "wb");
    if (!f) {
        lprintf("[reconcile] slot %d: cannot open %s for writing\n", slot, out->path);
        return -1;
    }

    uint32_t total = info->rom_size_kb * 1024;
    uint32_t written = 0;
    int rc = rom_cache_stream_chunks(handle, info, f, total, &written);
    fclose(f);

    out->captured_len = written;
    lprintf("[reconcile] slot %d captured %u / %u bytes (rc=%d) -> %s\n",
            slot, written, total, rc, out->path);

    if (written == 0) return -1;
    if (rc == 0 && written == total) return 0;
    return 1;
}

// ---- Internal helpers -------------------------------------------------------

// Cheap O(W) check: is this window "degenerate" (all-identical-byte, or a
// short repeating period <= 8 bytes)? A degenerate window can't be uniquely
// matched in real ROM data (padding/tile regions are exactly this shape) and
// would waste a full scan attempting to find a "unique" occurrence that
// isn't meaningfully unique at all.
static int is_degenerate_window(const uint8_t *w, uint32_t len) {
    int all_same = 1;
    for (uint32_t i = 1; i < len; i++) {
        if (w[i] != w[0]) { all_same = 0; break; }
    }
    if (all_same) return 1;

    for (uint32_t period = 1; period <= 8; period++) {
        int matches = 1;
        for (uint32_t i = period; i < len; i++) {
            if (w[i] != w[i - period]) { matches = 0; break; }
        }
        if (matches) return 1;
    }
    return 0;
}

// Scans haystack (length hay_len) for an exact match of needle (length
// needle_len). Returns the unique match position (>= 0) if found exactly
// once, -1 if not found at all, or -2 if found more than once (ambiguous —
// the candidate window isn't usable, try a different one).
static int64_t find_unique_occurrence(const uint8_t *haystack, uint32_t hay_len,
                                       const uint8_t *needle, uint32_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return -1;
    int64_t found_at = -1;
    for (uint32_t pos = 0; pos + needle_len <= hay_len; pos++) {
        if (memcmp(haystack + pos, needle, needle_len) == 0) {
            if (found_at >= 0) return -2;
            found_at = (int64_t)pos;
        }
    }
    return found_at;
}

static int verify_exact_range(const uint8_t *x, const uint8_t *y, uint32_t len) {
    return memcmp(x, y, len) == 0;
}

// Copies src_path's full contents to dst_path. Returns 0 on success.
static int copy_file(const char *src_path, const char *dst_path) {
    FILE *src = fopen(src_path, "rb");
    FILE *dst = src ? fopen(dst_path, "wb") : NULL;
    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        return -1;
    }
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { fclose(src); fclose(dst); return -1; }
    }
    fclose(src);
    fclose(dst);
    return 0;
}

ReconcileResult rom_reconcile_align_and_merge(const CartInfo *info,
                                               const ReconcileAttempt *a,
                                               const ReconcileAttempt *b,
                                               const char *out_path,
                                               ReconcileOutcome *outcome) {
    ReconcileOutcome local;
    if (!outcome) outcome = &local;
    memset(outcome, 0, sizeof(*outcome));

    // Step 1: precondition — need a real, trustworthy total to check
    // coverage against. Never guess a size for this.
    if (!info->rom_size_confirmed) {
        outcome->result = RECONCILE_SIZE_NOT_CONFIRMED;
        return outcome->result;
    }
    uint32_t total = info->rom_size_kb * 1024;
    if (total == 0 || a->captured_len == 0 || b->captured_len == 0 ||
        a->captured_len > total || b->captured_len > total) {
        outcome->result = RECONCILE_IO_ERROR;
        return outcome->result;
    }

    lprintf("[reconcile] align: a=%s (%u/%u) b=%s (%u/%u)\n",
            a->path, a->captured_len, total, b->path, b->captured_len, total);

    // Step 2: free fast path — either attempt is already a complete, valid
    // capture, no reconciliation needed at all.
    for (int which = 0; which < 2; which++) {
        const ReconcileAttempt *att = which == 0 ? a : b;
        if (att->captured_len != total) continue;
        FILE *f = fopen(att->path, "rb");
        if (!f) continue;
        uint8_t hdr[512];
        size_t n = fread(hdr, 1, sizeof(hdr), f);
        fclose(f);
        if (n != sizeof(hdr) || !header_checksum_valid(hdr)) continue;

        if (copy_file(att->path, out_path) != 0) {
            outcome->result = RECONCILE_IO_ERROR;
            return outcome->result;
        }
        lprintf("[reconcile] free fast path: %s already complete and valid\n", att->path);
        outcome->result = RECONCILE_OK;
        outcome->confirmed_overlap = total;
        return outcome->result;
    }

    // Step 2.5: cheap anchor pre-check — read only a small header-sized
    // prefix from each file (not the full total-sized buffer) and score
    // both. If neither can serve as an anchor, return immediately without
    // ever allocating or reading the full (up to total-byte) buffers —
    // this is the dominant real-world case (Rom Stitching Test/test_1: a
    // pair's anchor check previously cost ~3.5s purely from loading two
    // 16MB files just to conclude "no anchor"; this precheck costs a couple
    // KB of I/O instead).
    uint8_t hdr_a[512], hdr_b[512];
    uint32_t hdr_a_len = 0, hdr_b_len = 0;
    memset(hdr_a, 0, sizeof(hdr_a));
    memset(hdr_b, 0, sizeof(hdr_b));
    {
        FILE *fa = fopen(a->path, "rb");
        if (fa) { hdr_a_len = (uint32_t)fread(hdr_a, 1, sizeof(hdr_a), fa); fclose(fa); }
        FILE *fb = fopen(b->path, "rb");
        if (fb) { hdr_b_len = (uint32_t)fread(hdr_b, 1, sizeof(hdr_b), fb); fclose(fb); }
    }
    int score_a = anchor_score(info->type, hdr_a, hdr_a_len);
    int score_b = anchor_score(info->type, hdr_b, hdr_b_len);
    if (score_a == 0 && score_b == 0) {
        lprintf("[reconcile] neither attempt's header/logo validates (cheap precheck, %u/%u bytes checked) — no anchor, need a clean-start recapture\n",
                hdr_a_len, hdr_b_len);
        outcome->result = RECONCILE_NO_ANCHOR;
        return outcome->result;
    }

    // Step 3: anchor selection — never assume shift-0 from capture order
    // (see test_25 in CLAUDE.md: a rejected attempt is not always shift-0).
    // Prefer the stronger-scoring attempt; a tie (including "both full
    // checksum" or "both partial logo") falls back to the longer capture.
    int a_is_anchor;
    if (score_a != score_b) a_is_anchor = (score_a > score_b);
    else a_is_anchor = (a->captured_len >= b->captured_len);
    const ReconcileAttempt *anchor_att = a_is_anchor ? a : b;
    const ReconcileAttempt *other_att  = a_is_anchor ? b : a;
    int anchor_score_val = a_is_anchor ? score_a : score_b;
    const char *anchor_path = anchor_att->path;
    const char *other_path  = other_att->path;
    uint32_t anchor_len = anchor_att->captured_len;
    uint32_t other_len  = other_att->captured_len;
    lprintf("[reconcile] anchor=%s (%u bytes, score=%d [%s]), other=%s (%u bytes)\n",
            anchor_path, anchor_len, anchor_score_val,
            anchor_score_val == 2 ? "full header checksum" : "partial logo prefix",
            other_path, other_len);

    // Now that an anchor is confirmed, load both attempts into total-sized,
    // zero-initialized buffers. Both are allocated at `total` (not just
    // their own captured_len) so that the anchor is already the right size
    // to receive the other's non-overlapping tail directly in step 8 — no
    // separate output buffer needed, peak RAM stays at 2x`total` for the
    // whole align/merge phase.
    uint8_t *anchor_buf = calloc(total, 1);
    uint8_t *other_buf  = calloc(total, 1);
    if (!anchor_buf || !other_buf) {
        free(anchor_buf); free(other_buf);
        lprintf("[reconcile] malloc failed for two %u-byte buffers\n", total);
        outcome->result = RECONCILE_IO_ERROR;
        return outcome->result;
    }
    {
        FILE *fA = fopen(anchor_path, "rb");
        FILE *fO = fopen(other_path, "rb");
        size_t rA = fA ? fread(anchor_buf, 1, anchor_len, fA) : 0;
        size_t rO = fO ? fread(other_buf, 1, other_len, fO) : 0;
        if (fA) fclose(fA);
        if (fO) fclose(fO);
        if (rA != anchor_len || rO != other_len) {
            free(anchor_buf); free(other_buf);
            lprintf("[reconcile] short read loading attempt buffers (rA=%zu rO=%zu)\n", rA, rO);
            outcome->result = RECONCILE_IO_ERROR;
            return outcome->result;
        }
    }

    // Step 4: cheap direct shift-0 check before any search. If this holds,
    // it's definitive (a multi-KB/MB agreement is stronger proof than the
    // sample search would produce anyway) — no need to search further.
    uint32_t direct_overlap = anchor_len < other_len ? anchor_len : other_len;
    int64_t shift = 0;
    int shift_found = 0;
    uint32_t confirmed_overlap = 0;

    if (verify_exact_range(anchor_buf, other_buf, direct_overlap)) {
        shift = 0;
        shift_found = 1;
        confirmed_overlap = direct_overlap;
        lprintf("[reconcile] direct shift-0 check: %u bytes agree\n", direct_overlap);
    } else {
        // Step 5: sample search — shift-0 does not hold, so a real
        // (non-zero) shift is being searched for. Only meaningful here;
        // if shift-0 already held there would be nothing else to find.
        lprintf("[reconcile] direct shift-0 check failed — searching for alignment\n");

        // Build the candidate list: known jump points from earlier
        // alignment results this session first (a hint only — every
        // candidate below is still subject to the identical uniqueness +
        // zero-tolerance checks regardless of where it came from), then
        // even-spaced fallback candidates.
        uint32_t candidates[RECONCILE_KNOWN_GAPS_MAX + RECONCILE_SAMPLE_TRIES];
        int n_candidates = 0;
        for (int i = 0; i < s_known_gaps_count; i++) {
            uint32_t off = s_known_gaps[i];
            if (off + RECONCILE_SAMPLE_WINDOW > anchor_len) continue;
            candidates[n_candidates++] = off;
        }
        int n_known = n_candidates;
        for (int i = 0; i < RECONCILE_SAMPLE_TRIES; i++) {
            uint64_t a0_64 = ((uint64_t)anchor_len * (uint32_t)(i + 1)) / (RECONCILE_SAMPLE_TRIES + 1);
            if (a0_64 + RECONCILE_SAMPLE_WINDOW > anchor_len) continue;
            uint32_t a0 = (uint32_t)a0_64;
            int dup = 0;
            for (int k = 0; k < n_candidates; k++) if (candidates[k] == a0) { dup = 1; break; }
            if (!dup && n_candidates < (int)(sizeof(candidates) / sizeof(candidates[0])))
                candidates[n_candidates++] = a0;
        }

        u64 t0 = gettime();
        for (int i = 0; i < n_candidates && !shift_found; i++) {
            uint32_t a0 = candidates[i];
            const char *origin = i < n_known ? "known-gap" : "even-spaced";
            const uint8_t *sample = anchor_buf + a0;

            if (is_degenerate_window(sample, RECONCILE_SAMPLE_WINDOW)) {
                lprintf("[reconcile] candidate %d (%s, anchor offset %u): degenerate window, skipping\n", i, origin, a0);
                continue;
            }

            int64_t j0 = find_unique_occurrence(other_buf, other_len, sample, RECONCILE_SAMPLE_WINDOW);
            if (j0 == -1) {
                lprintf("[reconcile] candidate %d (%s, anchor offset %u): no match in other\n", i, origin, a0);
                continue;
            }
            if (j0 == -2) {
                lprintf("[reconcile] candidate %d (%s, anchor offset %u): ambiguous, multiple matches\n", i, origin, a0);
                continue;
            }

            int64_t d = (int64_t)a0 - j0;
            if (d < 0 || (uint64_t)d + other_len > total) {
                lprintf("[reconcile] candidate %d (%s, anchor offset %u): implied shift %lld out of range, rejecting\n",
                        i, origin, a0, (long long)d);
                continue;
            }

            // Step 6: full-overlap validation — zero tolerance. Overlap in
            // anchor-index terms is [d, min(anchor_len, d+other_len)); the
            // corresponding other-index range is [0, that length), since
            // other[j] == trueROM[d+j] means other-index = anchor-index - d.
            uint32_t ov_start = (uint32_t)d;
            uint32_t ov_hi_other = (uint32_t)((uint64_t)d + other_len);
            uint32_t ov_end = anchor_len < ov_hi_other ? anchor_len : ov_hi_other;
            if (ov_end <= ov_start) {
                lprintf("[reconcile] candidate %d: empty overlap at shift %lld, rejecting\n", i, (long long)d);
                continue;
            }
            uint32_t ov_len = ov_end - ov_start;
            if (ov_len < RECONCILE_MIN_OVERLAP) {
                lprintf("[reconcile] candidate %d: overlap %u bytes < minimum %d, rejecting\n",
                        i, ov_len, RECONCILE_MIN_OVERLAP);
                continue;
            }
            if (!verify_exact_range(anchor_buf + ov_start, other_buf + (ov_start - (uint32_t)d), ov_len)) {
                lprintf("[reconcile] candidate %d: full-overlap validation FAILED at shift %lld — rejecting\n",
                        i, (long long)d);
                continue;
            }

            shift = d;
            shift_found = 1;
            confirmed_overlap = ov_len;
            lprintf("[reconcile] candidate %d (%s): CONFIRMED shift=%lld overlap=%u bytes\n",
                    i, origin, (long long)d, ov_len);
        }
        u64 t1 = gettime();
        lprintf("[reconcile] sample search took %llu us (%d candidates, %d from known-gap hints)\n",
                (unsigned long long)ticks_to_microsecs(t1 - t0), n_candidates, n_known);
    }

    if (!shift_found) {
        free(anchor_buf); free(other_buf);
        outcome->result = RECONCILE_NO_ALIGNMENT;
        return outcome->result;
    }

    // Step 7: coverage check. Anchor always covers [0, anchor_len); other
    // covers [shift, shift+other_len).
    uint32_t cov_end = (uint32_t)((uint64_t)shift + other_len);
    uint32_t union_end = anchor_len > cov_end ? anchor_len : cov_end;
    if (union_end < total) {
        free(anchor_buf); free(other_buf);
        if (shift == 0) {
            // Both attempts start at true offset 0 — their union can never
            // exceed whichever is longer. This is the expected, named
            // outcome for the now-confirmed-common recurring gap, not a
            // generic failure — see CLAUDE.md test_25-test_34 tally.
            outcome->result = RECONCILE_REDUNDANT;
            record_known_gap(union_end);
            lprintf("[reconcile] REDUNDANT: both start at 0, union covers [0,%u), total=%u\n",
                    union_end, total);
        } else {
            outcome->result = RECONCILE_GAP;
            outcome->gap_start = union_end;
            outcome->gap_end = total;
            record_known_gap(union_end);
            lprintf("[reconcile] GAP: union covers [0,%u), total=%u, missing [%u,%u)\n",
                    union_end, total, union_end, total);
        }
        return outcome->result;
    }

    // Step 8: merge — copy only the non-overlapping tail of `other` into
    // the anchor's own (already total-sized) buffer.
    if ((uint64_t)shift + other_len > anchor_len) {
        uint32_t copy_start_final = anchor_len > (uint32_t)shift ? anchor_len : (uint32_t)shift;
        uint32_t copy_start_other = copy_start_final - (uint32_t)shift;
        uint32_t copy_len = other_len - copy_start_other;
        memcpy(anchor_buf + copy_start_final, other_buf + copy_start_other, copy_len);
    }

    // Step 9: final gate — defense in depth, should be unreachable given
    // step 6's zero-tolerance validation, but never skipped.
    if (!header_checksum_valid(anchor_buf)) {
        lprintf("[reconcile] assembled buffer FAILED final header gate — this should be unreachable\n");
        free(anchor_buf); free(other_buf);
        outcome->result = RECONCILE_HEADER_FAILED;
        return outcome->result;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out || fwrite(anchor_buf, 1, total, out) != total) {
        if (out) fclose(out);
        free(anchor_buf); free(other_buf);
        outcome->result = RECONCILE_IO_ERROR;
        return outcome->result;
    }
    fclose(out);

    // Step 10: free both buffers on every exit path (done here for the
    // success path; every earlier return above also frees before returning).
    free(anchor_buf);
    free(other_buf);

    outcome->result = RECONCILE_OK;
    outcome->other_shift = (uint32_t)shift;
    outcome->confirmed_overlap = confirmed_overlap;
    lprintf("[reconcile] OK: wrote %u bytes to %s (other_shift=%u, confirmed_overlap=%u)\n",
            total, out_path, outcome->other_shift, outcome->confirmed_overlap);
    return outcome->result;
}
