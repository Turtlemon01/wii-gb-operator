#include "gb_operator.h"
#include "log.h"
#include "settings.h"
#include <ogc/usb.h>
#include <ogc/cache.h>
#include <ogc/ios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ogc/pad.h>
#include <ogc/lwp_watchdog.h>

// All packets are 64 bytes: 60 bytes payload + 4 bytes CRC32-MPEG2 (little-endian)
#define GBOP_PKT_SIZE      64
#define GBOP_PAYLOAD_SIZE  60

// On IOS58, USB_WriteBlkMsg naturally takes >=200ms, which incidentally gives
// the GB Operator's own internal cart-probe time to settle before the first
// command after an open is answered. On d2x-cIOS, writes complete in <10ms,
// removing that incidental settle time. Observed symptom on d2x-cIOS setups:
// the cart-info response is self-consistent (title bytes correct) but a size
// field reads a stale/default value (see gbop_verify_gba_rom_size). Adding an
// explicit delay after open — gated to d2x-cIOS only via s_ack_read_size, so
// IOS58 timing is never touched — tests whether that settle time is the cause.
#define GBOP_D2X_SETTLE_US 250000

// A legitimate mid-stream ZLP (batch terminator, ~every 512 bytes on some
// d2x-cIOS setups) is always immediately followed by resumed real data —
// never by another ZLP. More than this many in a row means the device has
// stopped streaming entirely (observed on a marginal external connection);
// treat it as a dead stream rather than spinning on USB_ReadBlkMsg forever.
#define GBOP_MAX_ZLP_STREAK 64

/* Detected once on the first ACK exchange: GBOP_PAYLOAD_SIZE (60) on IOS58,
 * GBOP_PKT_SIZE (64) on d2x-cIOS.  Non-zero init keeps it in .data so it
 * survives a dol_reload() BSS clear and carries forward across sessions. */
static int s_ack_read_size = GBOP_PAYLOAD_SIZE;

typedef struct {
    s32     fd;
    u8      ep_out;
    u8      ep_in;
    uint8_t cached_resp[GBOP_PAYLOAD_SIZE];
    int     has_cached_resp;
    // ROM streaming state (persists across gbop_dump_rom calls)
    int      dump_active;
    uint32_t dump_total;      // total ROM bytes expected
    uint32_t dump_given;      // bytes returned to caller so far
    uint32_t dump_chunk_cnt;    // ROM data packets written to file
    uint32_t dump_iter_cnt;     // ALL USB reads (ROM + response) — drives ACK cycle
    uint32_t dump_rx_bytes;     // ROM bytes written to file (for progress logging)
    uint32_t dump_pending_drain; // drain reads owed after last in-stream ACK (0, 1, or 2)
    uint32_t dump_zlp_streak;   // consecutive mid-stream ZLPs with no real data between them
    uint8_t  dump_spare[64];  // leftover bytes from the last device chunk
    uint32_t dump_spare_len;
    // New-firmware (v10.0.10+) read-streaming state (gbop_dump_rom_new /
    // gbop_read_save_new). Keyed on the device's own C0 DE footer marker
    // rather than a byte count — see CLAUDE.md "NEW FIRMWARE PROTOCOL
    // REBUILT FROM WIRESHARK". np_total is a caller-supplied estimate only;
    // being wrong does not break the stream, it only affects how early
    // np_footer_seen goes true relative to np_given reaching np_total.
    int      np_active;
    int      np_footer_seen;
    uint8_t  np_cmd;          // command byte this stream is for (0x00 or 0x02)
    uint32_t np_total;        // caller's buffer-size estimate
    uint32_t np_given;        // bytes returned to caller so far
    uint32_t np_stall_streak; // consecutive reads with no forward progress (safety cap)
} GBOpDevice;

// USB DMA buffers — 32-byte aligned (Wii IOS DMA requirement)
static uint8_t s_tx[GBOP_PKT_SIZE]          ATTRIBUTE_ALIGN(32);
static uint8_t s_rx[GBOP_PAYLOAD_SIZE + 32] ATTRIBUTE_ALIGN(32);
static uint8_t s_crc[32]                    ATTRIBUTE_ALIGN(32);
// Response buffer: device sends 256 bytes (4 × 64-byte USB packets) after the ACK
// Each 64-byte chunk starts at a 32-byte-aligned offset (64 = 2 × 32)
static uint8_t s_resp[256]                  ATTRIBUTE_ALIGN(32);

// ─── CRC32-MPEG2 ─────────────────────────────────────────────────────────────
// Poly=0x04C11DB7, Init=0xFFFFFFFF, RefIn=false, RefOut=false, XorOut=0x00

static uint32_t crc32_mpeg2(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
    }
    return crc;
}

// ─── Packet helpers ──────────────────────────────────────────────────────────

// Fill s_tx from a 60-byte payload and append CRC32-MPEG2 in bytes 60-63
static void gbop_make_pkt(const uint8_t *payload) {
    memcpy(s_tx, payload, GBOP_PAYLOAD_SIZE);
    uint32_t crc = crc32_mpeg2(s_tx, GBOP_PAYLOAD_SIZE);
    s_tx[60] = (uint8_t)(crc >>  0);
    s_tx[61] = (uint8_t)(crc >>  8);
    s_tx[62] = (uint8_t)(crc >> 16);
    s_tx[63] = (uint8_t)(crc >> 24);
}

// Send a 60-byte payload (CRC appended automatically)
static int gbop_bulk_send(GBOpDevice *dev, const uint8_t *payload) {
    gbop_make_pkt(payload);
    DCFlushRange(s_tx, GBOP_PKT_SIZE);
    s32 r = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
    lprintf("[gbop] cmd=0x%02X tx=%d\n", payload[0], (int)r);
    return (r < 0) ? (int)r : 0;
}

/* Reads one command-response ACK packet using the detected read size.
 * On the first call where the 60-byte read fails (d2x-cIOS), upgrades to
 * 64-byte reads for the rest of the session without changing the protocol. */
static s32 gbop_ack_read(GBOpDevice *dev) {
    DCInvalidateRange(s_rx, sizeof(s_rx));
    s32 ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, s_ack_read_size, s_rx);
    if (ra < 0 && s_ack_read_size == GBOP_PAYLOAD_SIZE) {
        DCInvalidateRange(s_rx, sizeof(s_rx));
        ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        if (ra >= 0) {
            lprintf("[gbop] ACK auto-detect: 60-byte read failed, switching to 64-byte reads\n");
            s_ack_read_size = GBOP_PKT_SIZE;
        }
    }
    return ra;
}

// Full command exchange: send is done by caller; this reads the full response.
//
// Protocol (from gbopyrator/coms_utils.py read_cartridge_info):
//   1. Read 60 bytes — command ACK (always zeros, discard)
//   2. Read  4 bytes — ACK footer (optional; sometimes absent on re-open)
//   3. Read 256 bytes in 4 × 64-byte USB bulk packets — actual response data
//   4. out receives the first 60 bytes of the 256-byte response
//
// The 4-byte ACK footer is device-state-dependent. On fresh power-up it arrives
// as a separate USB short packet; after USB_CloseDevice + USB_OpenDevice the
// device sometimes omits it. Issuing USB_ReadBlkMsg(4) when no 4-byte packet
// is coming returns -7008 but — critically — leaves the IOS endpoint in a bad
// state that causes the subsequent 64-byte data-chunk read to hang indefinitely.
// Instead we absorb the optional 4-byte packet by treating the first 64-byte
// data read as either the footer (rd==4, discard + re-read) or chunk0 directly.
static int gbop_bulk_recv(GBOpDevice *dev, uint8_t *out) {
    // Step 1: discard the ACK (size auto-detected: 60 on IOS58, 64 on d2x-cIOS)
    s32 ra = gbop_ack_read(dev);
    lprintf("[gbop] ACK read%d=%d\n", s_ack_read_size, (int)ra);
    // ZLP (rd=0) can land here on d2x-cIOS as a batch-terminator from a
    // previous gbop_read_rom_header 512-byte mini-dump.  IOS does not flush
    // EP IN on close/reopen, so the ZLP persists.  Re-read to get the real ACK.
    if (ra == 0) {
        lprintf("[gbop] ZLP before ACK — re-reading\n");
        ra = gbop_ack_read(dev);
        lprintf("[gbop] ACK re-read%d=%d\n", s_ack_read_size, (int)ra);
    }
    if (ra < 0) return -1;

    // Step 2+3: read data chunks; the first read may be the optional 4-byte footer.
    // Data chunks are single 64-byte USB packets (60-byte payload + 4-byte CRC).
    for (int i = 0; i < 2; i++) {
        uint8_t *chunk = s_resp + i * 64; // 64-byte stride preserves 32-byte alignment
        DCInvalidateRange(chunk, 64);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
        if (rd < 0) { lprintf("[gbop] resp[%d] error: %d\n", i, (int)rd); return (int)rd; }
        if (rd == 4) {
            // 4-byte ACK footer arrived as a separate USB packet — discard and
            // re-read to get the actual data chunk.  The footer can arrive before
            // chunk[0] (fast device) or before chunk[1] (slow device); catching
            // it only at i==0 left chunk[1] unconsumed, accumulating stale data
            // in the IOS EP IN buffer and causing alternating "no cart" failures.
            lprintf("[gbop] ACK footer (rd=4) consumed before chunk[%d]\n", i);
            DCInvalidateRange(chunk, 64);
            rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
            if (rd < 0) { lprintf("[gbop] resp[%db] error: %d\n", i, (int)rd); return (int)rd; }
        }
    }

    // Cart info lives in the first 60 bytes of the 256-byte response
    memcpy(out, s_resp, GBOP_PAYLOAD_SIZE);
    return 0;
}

// ─── Public API ──────────────────────────────────────────────────────────────

// Shared device-open logic: find the GB Operator in the USB device list,
// open interface [1] (CDC Data), send SET_CONFIGURATION, and return a handle.
// Returns NULL on failure.
static GBOpDevice *gbop_open_hw(void) {
    usb_device_entry devlist[8];
    u8 num_devs = 0;
    s32 ret = USB_GetDeviceList(devlist, 8, 0x00, &num_devs);
    lprintf("[gbop] GetDeviceList ret=%d devices=%u\n", (int)ret, (unsigned)num_devs);
    if (ret < 0) return NULL;

    int first_match = -1;
    for (int i = 0; i < num_devs; i++) {
        lprintf("[gbop]  dev[%d]: vid=%04X pid=%04X id=%d\n", i,
                (unsigned)devlist[i].vid, (unsigned)devlist[i].pid,
                (int)devlist[i].device_id);
        if (devlist[i].vid == GBOP_VID && devlist[i].pid == GBOP_PID && first_match < 0)
            first_match = i;
    }
    if (first_match < 0) { lprintf("[gbop] Not found\n"); return NULL; }

    GBOpDevice *dev = calloc(1, sizeof(GBOpDevice));
    if (!dev) return NULL;
    dev->ep_out = 0x01;
    dev->ep_in  = 0x81;

    // Open interface [1] (CDC Data) for bulk IN/OUT.
    int data_idx = first_match + 1;
    if (data_idx >= num_devs ||
        devlist[data_idx].vid != GBOP_VID || devlist[data_idx].pid != GBOP_PID) {
        lprintf("[gbop] No interface [1] found, falling back to interface [0]\n");
        data_idx = first_match;
    }

    ret = USB_OpenDevice(devlist[data_idx].device_id, GBOP_VID, GBOP_PID, &dev->fd);
    lprintf("[gbop] OpenDevice(iface[%d]) ret=%d fd=%d\n",
            data_idx - first_match, (int)ret, (int)dev->fd);
    if (ret < 0) {
        lprintf("[gbop] OpenDevice failed (ret=%d)\n", (int)ret);
        free(dev);
        return NULL;
    }
    if (dev->fd < 0)
        lprintf("[gbop] fd negative (%d) — d2x-cIOS handle, proceeding\n", (int)dev->fd);
    lprintf("[gbop] Bulk ep_out=%02X ep_in=%02X\n", dev->ep_out, dev->ep_in);

    s32 cfg = USB_WriteCtrlMsg(dev->fd, 0x00, 0x09, 0x0001, 0x0000, 0, NULL);
    if (cfg < 0) lprintf("[gbop] SET_CONFIGURATION ret=%d\n", (int)cfg);
    usleep(100000);

    return dev;
}

GBOperatorHandle gbop_find(void) {
    /* USB host reset is NOT done here. For SD-boot: the host is already in a
     * clean state (HBC didn't use USB). For USB-boot: main() resets it before
     * mounting USB mass storage (Phase 2), and repeating the reset here would
     * close the shared OH0 handle the storage driver depends on. */
    {
        static const uint8_t test[] = "123456789";
        uint32_t chk = crc32_mpeg2(test, 9);
        lprintf("[gbop] CRC self-test (expect 0376E6E7): %08X %s\n",
                chk, (chk == 0x0376E6E7) ? "PASS" : "FAIL");
    }

    // IOS/cIOS identity — logged once at boot so every report carries it without
    // relying on a tester to look it up separately. IOS_GetVersion() reports the
    // base IOS number (e.g. 58, or the number a cIOS was built from); revision
    // major/minor distinguish stock IOS from a cIOS slot built on top of it.
    lprintf("[gbop] IOS: version=%d revision=%d.%d\n",
            (int)IOS_GetVersion(), (int)IOS_GetRevisionMajor(), (int)IOS_GetRevisionMinor());

    lprintf("[gbop] Scanning USB (VID=%04X PID=%04X)...\n", GBOP_VID, GBOP_PID);

    GBOpDevice *dev = gbop_open_hw();
    if (!dev) return NULL;

    // Probe with 0x04 (Read Cart Info) to verify the device is responsive and
    // cache the response for the subsequent gbop_read_cart_info call.
    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x04 };
    uint8_t resp[GBOP_PAYLOAD_SIZE];
    if (gbop_bulk_send(dev, cmd) < 0 || gbop_bulk_recv(dev, resp) < 0) {
        lprintf("[gbop] Probe failed\n");
        USB_CloseDevice(&dev->fd);
        free(dev);
        return NULL;
    }

    memcpy(dev->cached_resp, resp, GBOP_PAYLOAD_SIZE);
    dev->has_cached_resp = 1;
    lprintf("[gbop] GB Operator ready\n");
    return (GBOperatorHandle)dev;
}

// Opens the GB Operator after a previous gbop_close() without re-probing.
// Use when cart info is already known and a fresh USB handle is needed for
// a dump or save command. Skips SET_CONFIGURATION to avoid resetting the
// device state machine that caused probe hangs on re-open.
GBOperatorHandle gbop_reopen(void) {
    usb_device_entry devlist[8];
    u8 num_devs = 0;
    s32 ret = USB_GetDeviceList(devlist, 8, 0x00, &num_devs);
    if (ret < 0 || num_devs == 0) { lprintf("[gbop] reopen: GetDeviceList failed\n"); return NULL; }

    int first_match = -1;
    for (int i = 0; i < num_devs; i++) {
        if (devlist[i].vid == GBOP_VID && devlist[i].pid == GBOP_PID && first_match < 0)
            first_match = i;
    }
    if (first_match < 0) { lprintf("[gbop] reopen: device not found\n"); return NULL; }

    GBOpDevice *dev = calloc(1, sizeof(GBOpDevice));
    if (!dev) return NULL;
    dev->ep_out = 0x01;
    dev->ep_in  = 0x81;

    int data_idx = first_match + 1;
    if (data_idx >= num_devs ||
        devlist[data_idx].vid != GBOP_VID || devlist[data_idx].pid != GBOP_PID)
        data_idx = first_match;

    ret = USB_OpenDevice(devlist[data_idx].device_id, GBOP_VID, GBOP_PID, &dev->fd);
    lprintf("[gbop] reopen: OpenDevice(iface[%d]) ret=%d fd=%d\n",
            data_idx - first_match, (int)ret, (int)dev->fd);
    if (ret < 0) {
        lprintf("[gbop] reopen: OpenDevice failed (ret=%d)\n", (int)ret);
        free(dev);
        return NULL;
    }
    lprintf("[gbop] reopen: ep_out=%02X ep_in=%02X\n", dev->ep_out, dev->ep_in);

    // GBOP_D2X_SETTLE_US delay REMOVED here (2026-07-29) — it fired on every
    // single reopen once s_ack_read_size detected d2x-cIOS-style framing
    // (which happens almost immediately and then sticks for the rest of the
    // session), costing 250ms per reopen with no offsetting benefit ever
    // confirmed for the new-firmware protocol. Its own premise (d2x-cIOS's
    // fast writes removing IOS58's incidental settle time) was already
    // disproven by this project's own research before the new-firmware
    // rebuild even started (test_21: the bug it was built for reproduced
    // under plain IOS58 with no cIOS involved at all). At ~1,150 reopens in
    // a single session (Post Firmware Update Test/test_34), this alone was
    // costing several minutes of pure sleep with no evidence it helped.
    // GBOP_D2X_SETTLE_US/s_ack_read_size are kept for the other things
    // s_ack_read_size still gates (old-firmware ACK framing size).

    return (GBOperatorHandle)dev;
}

// See header comment. Shared by gbop_probe_hardware() and by main.c's ROM
// install / play_game / header-read recovery paths, all of which previously
// hand-rolled this same loop.
// Set once this session's fd has been observed to survive a FULL wait-fresh
// exhaustion without ever changing — i.e. real, hardware-confirmed evidence
// (not a guess) that this connection never cycles the fd at all (some
// d2x-cIOS builds; see CLAUDE.md "IOS/d2x-cIOS compatibility work"). Once
// set, every future wait-fresh request is pointless: it can only burn its
// full retry budget again for the same non-result. Rom Stitching Test/
// test_6 confirmed this directly — every single reopen across an entire
// session showed the identical fd, and two separate ~2-6s bursts (one from
// this function, one from dump_rom_with_retry()'s own near-identical inline
// loop in main.c) each ran their full budget before falling back anyway.
static int s_fd_known_unstable = 0;

int gbop_fd_known_unstable(void) { return s_fd_known_unstable; }
void gbop_mark_fd_unstable(void) { s_fd_known_unstable = 1; }

GBOperatorHandle gbop_reopen_wait_fresh(int32_t old_fd, int *out_same_fd_fallback) {
    if (s_fd_known_unstable) {
        // Already confirmed this session — one fast reopen instead of
        // burning the retry budget again for the same non-result.
        GBOperatorHandle op = gbop_reopen();
        if (out_same_fd_fallback) *out_same_fd_fallback = (op != NULL);
        return op;
    }
    GBOperatorHandle op = NULL;
    int same_fd_count = 0;
    for (int i = 0; i < 75 && !op; i++) {
        usleep(60000);
        op = gbop_reopen();
        if (!op) continue;
        if (gbop_get_fd(op) != old_fd) break;
        same_fd_count++;
        if (same_fd_count < 75) { gbop_close(op); op = NULL; }
    }
    if (same_fd_count >= 75) s_fd_known_unstable = 1;
    if (out_same_fd_fallback) *out_same_fd_fallback = (same_fd_count >= 75);
    return op;
}

// Old-firmware (v9.2.0 and earlier) cart-info implementation. See the
// dispatcher gbop_read_cart_info() below — this is only called when
// g_settings.use_old_firmware is set. Do not modify based on new-firmware
// findings; see the CLAUDE.md disclaimer at the top of "## Do Not".
static int gbop_read_cart_info_legacy(GBOperatorHandle handle, CartInfo *out) {
    if (!handle || !out) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    memset(out, 0, sizeof(CartInfo));

    uint8_t resp[GBOP_PAYLOAD_SIZE];

    if (dev->has_cached_resp) {
        memcpy(resp, dev->cached_resp, GBOP_PAYLOAD_SIZE);
        dev->has_cached_resp = 0;
    } else {
        uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x04 };
        if (gbop_bulk_send(dev, cmd) < 0) return GBOP_USB;   // EP stall or IOS error
        if (gbop_bulk_recv(dev, resp) < 0) return GBOP_USB;  // recv failed
    }

    // Response format (from gbopyrator/coms_utils.py):
    //   resp[2]      == 0x20  → GB/GBC cart
    //   resp[3:5]    != 0x00  → cart present
    //   resp[5:8]    → ROM size in bytes, little-endian (3 bytes)
    //   resp[9:12]   → RAM size in bytes, little-endian (3 bytes)
    //   resp[13...]  → cart title (ASCII)
    //   resp[14]     → MBC type byte
    //   resp[15]     → ROM type byte
    //   resp[16]     → RAM type byte

    // Populate raw_resp before any early return so callers have the raw bytes
    // even when the device says "no cart."
    memcpy(out->raw_resp, resp, GBOP_PAYLOAD_SIZE);

    // Corrupted response from intermittent cart contact on d2x-cIOS.
    // Bytes 0-1 = C0 DE (garbage header); treat as no cart regardless of resp[3:5].
    if (resp[0] == 0xC0 && resp[1] == 0xDE) {
        lprintf("[gbop] C0 DE garbage header — cart contact error, treating as NOCART\n");
        return GBOP_NOCART;
    }

    if (resp[3] == 0x00 && resp[4] == 0x00) {
        lprintf("[gbop] Cart not detected (resp[3:5] == 0) — is cart inserted?\n");
        return GBOP_NOCART;
    }

    if (resp[2] == 0x20) {
        // GB/GBC: size fields are 3-byte LE at resp[5:8] and resp[9:12]
        uint32_t rom_bytes = (uint32_t)resp[5] | ((uint32_t)resp[6] << 8) | ((uint32_t)resp[7] << 16);
        uint32_t ram_bytes = (uint32_t)resp[9] | ((uint32_t)resp[10] << 8) | ((uint32_t)resp[11] << 16);
        out->rom_size_kb = rom_bytes / 1024;
        out->ram_size_kb = ram_bytes / 1024;
        out->type = CART_TYPE_GB;
        strncpy(out->type_str, "GB", sizeof(out->type_str));
    } else {
        // GBA: resp[2]=0x30; ROM size is encoded at resp[26] as a shift count
        // (32KB << n), matching the standard GB Operator size code scheme:
        // 0x07=4MB, 0x08=8MB, 0x09=16MB, 0x0A=32MB
        out->type = CART_TYPE_GBA;
        strncpy(out->type_str, "GBA", sizeof(out->type_str));
        uint8_t sz = resp[26];
        if (sz > 0 && sz <= 12) {
            out->rom_size_kb = 32u << sz;
        } else {
            out->rom_size_kb = 0;
            lprintf("[gbop] GBA ROM size unknown (resp[26]=0x%02X)\n", sz);
        }

        // GBA save size — resp[27] is a save type code.
        // Only one confirmed data point: FireRed (resp[27]=0x02) → Flash 128KB.
        // All other entries are speculative; update as more carts are tested.
        static const uint32_t kSaveSizeKB[] = {
            0,    // 0x00: no save / unknown
            8,    // 0x01: EEPROM 8KB (unconfirmed)
            128,  // 0x02: Flash 128KB (FireRed confirmed)
            32,   // 0x03: SRAM 32KB (unconfirmed)
            64,   // 0x04: Flash 64KB (unconfirmed)
            128,  // 0x05: Flash 128KB (unconfirmed)
        };
        uint8_t sc = resp[27];
        // Sapphire device firmware quirk: resp[27]=0x00 but resp[28]==resp[26]
        // (ROM size code leaked into resp[28]). Confirmed unique to Sapphire/Ruby RSE.
        // Default to sc=2 (128KB Flash) when quirk is detected.
        if (sc == 0 && resp[26] != 0 && resp[28] == resp[26]) {
            lprintf("[gbop] GBA save code resp[27]=0x00, resp[28]=resp[26]=0x%02X — device quirk, defaulting to 128KB Flash\n", resp[26]);
            sc = 2;
        }
        out->ram_size_kb = (sc < 6) ? kSaveSizeKB[sc] : 0;
        lprintf("[gbop] GBA save code resp[27]=0x%02X → %uKB%s\n",
                sc, out->ram_size_kb, (sc == 0x02) ? "" : " (unconfirmed mapping)");
    }

    // Title starts at resp[13]; read until non-printable or end of payload
    int title_len = 0;
    for (int i = 13; i < GBOP_PAYLOAD_SIZE && title_len < 16; i++) {
        char c = (char)resp[i];
        if (c < 0x20 || c > 0x7E) break;
        out->title[title_len++] = c;
    }
    out->title[title_len] = '\0';

    // Log full response for protocol analysis (find CGB flag, full title offset, etc.)
    lprintf("[gbop] resp[0..29] :");
    for (int i = 0; i < 30; i++) lprintf(" %02X", resp[i]);
    lprintf("\n");
    lprintf("[gbop] resp[30..59]:");
    for (int i = 30; i < GBOP_PAYLOAD_SIZE; i++) lprintf(" %02X", resp[i]);
    lprintf("\n");

    lprintf("[gbop] type=%s rom=%uKB ram=%uKB title=\"%s\"\n",
            out->type_str, out->rom_size_kb, out->ram_size_kb, out->title);
    return 0;
}

int32_t gbop_get_fd(GBOperatorHandle handle) {
    if (!handle) return 0;
    return (int32_t)((GBOpDevice *)handle)->fd;
}

void gbop_drain_stray(GBOperatorHandle handle, int max_attempts) {
    if (!handle || max_attempts <= 0) return;
    GBOpDevice *dev = (GBOpDevice *)handle;
    uint8_t buf[GBOP_PKT_SIZE];
    int consecutive_empty = 0;
    lprintf("[gbop] drain: starting (cap=%d attempts)\n", max_attempts);
    for (int i = 0; i < max_attempts; i++) {
        DCInvalidateRange(buf, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, buf);
        lprintf("[gbop] drain[%d] rd=%d [0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                i, (int)rd, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        if (rd < 0) {
            // A real USB error here almost certainly means there was nothing
            // to read, not that something is wrong — stop, don't treat as fatal.
            break;
        }
        if (rd <= 0) {
            if (++consecutive_empty >= 3) {
                lprintf("[gbop] drain: 3 consecutive empty reads — assuming quiet, stopping early\n");
                break;
            }
        } else {
            consecutive_empty = 0;
        }
    }
    lprintf("[gbop] drain: done\n");
}

// A stall this long (consecutive non-progress reads) means the device has
// stopped responding entirely — same bounded-safety reasoning as
// NEWPROTO_MAX_STALL, sized smaller since this is an interactive diagnostic
// tool, not a multi-megabyte transfer.
#define GBOP_RAW_CAPTURE_MAX_STALL 32

// Sends a bare command (cmd in payload byte 0, rest zero, CRC appended as
// usual) and captures whatever comes back into buf_out completely
// unconditionally — no marker parsing, no header/footer interpretation, no
// drain logic. Exists for the dev-menu "Command Test Lab" so individual
// protocol commands — including ones with no existing higher-level
// implementation, like 0x15 "Get Info" (see CLAUDE.md Sources,
// kantosCoder/FreeGbOperatorFirm10Py) — can be exercised in isolation and
// their exact raw response captured for offline byte-level analysis, the
// same way ROM dumps already are. Deliberately does NOT log per-byte content
// (that would mean one lprintf — a printf+fprintf+fflush — per byte for a
// 2048-byte capture); callers should write buf_out to an SD file instead and
// inspect it with the same tooling used for ROM dumps.
// Returns bytes actually captured (may be less than buf_size if the stream
// stalls or a footer/short packet ends it early — neither is treated as an
// error here, since this tool's whole purpose is to see what really
// happens), or -1 if the command itself failed to send.
int gbop_raw_command_capture(GBOperatorHandle handle, uint8_t cmd,
                              uint8_t *buf_out, uint32_t buf_size) {
    if (!handle || !buf_out || buf_size == 0) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;

    uint8_t payload[GBOP_PAYLOAD_SIZE] = {0};
    payload[0] = cmd;
    lprintf("[testlab] cmd=0x%02X raw capture: sending command\n", cmd);
    if (gbop_bulk_send(dev, payload) < 0) {
        lprintf("[testlab] cmd=0x%02X raw capture: command send failed\n", cmd);
        return -1;
    }
    lprintf("[testlab] cmd=0x%02X raw capture: command sent, reading (hold X+Y to abort)\n", cmd);

    uint32_t captured = 0;
    int stall = 0;
    while (captured < buf_size && stall < GBOP_RAW_CAPTURE_MAX_STALL) {
        PAD_ScanPads();
        if ((PAD_ButtonsHeld(0) & (PAD_BUTTON_X | PAD_BUTTON_Y)) == (PAD_BUTTON_X | PAD_BUTTON_Y)) {
            lprintf("[testlab] cmd=0x%02X raw capture: aborted by user at %u/%u bytes\n",
                    cmd, captured, buf_size);
            return -2;
        }
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        if (rd <= 0) {
            stall++;
            continue;
        }
        stall = 0;
        uint32_t copy = (uint32_t)rd;
        if (copy > buf_size - captured) copy = buf_size - captured;
        memcpy(buf_out + captured, s_rx, copy);
        captured += copy;
    }
    lprintf("[testlab] cmd=0x%02X raw capture: %u/%u bytes (stall=%d)\n",
            cmd, captured, buf_size, stall);
    return (int)captured;
}

// Old-firmware (v9.2.0 and earlier) implementation — see gbop_read_rom_header()
// dispatcher below. Do not modify based on new-firmware findings.
static int gbop_read_rom_header_legacy(GBOperatorHandle handle, uint8_t *hdr_out) {
    if (!handle || !hdr_out) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;

    // Request exactly 512 bytes — enough for both GBA header (ends 0xBF) and
    // GB/GBC header (ends 0x14F).  First in-stream ACK fires at 16 KB, so no
    // ACK is needed during this mini-dump.
    const uint32_t probe_size = 512;
    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x00, 0x02 };  /* matches gbop_dump_rom — from gbopyrator */
    cmd[2] = (uint8_t)((probe_size >>  0) & 0xFF);
    cmd[3] = (uint8_t)((probe_size >>  8) & 0xFF);
    cmd[4] = (uint8_t)((probe_size >> 16) & 0xFF);
    cmd[5] = (uint8_t)((probe_size >> 24) & 0xFF);
    if (gbop_bulk_send(dev, cmd) < 0) {
        lprintf("[hdr] cmd send failed\n");
        return -1;
    }

    // Command ACK: read size auto-detected (60 on IOS58, 64 on d2x-cIOS).
    s32 ra = gbop_ack_read(dev);
    DCInvalidateRange(s_crc, sizeof(s_crc));
    s32 rb = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
    lprintf("[hdr] cmd ACK: r%d=%d r4=%d\n", s_ack_read_size, (int)ra, (int)rb);
    if (ra <= 0) return -1;

    // Initial host ACK: 64 zero bytes sent to EP OUT before streaming starts.
    // Without this the device does not stream; same requirement as full dump.
    memset(s_tx, 0, GBOP_PKT_SIZE);
    DCFlushRange(s_tx, GBOP_PKT_SIZE);
    s32 wack = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
    lprintf("[hdr] host ACK: w=%d\n", (int)wack);
    if (wack < 0) return -1;

    // Device ready-to-stream response.
    s32 r1 = gbop_ack_read(dev);
    DCInvalidateRange(s_crc, sizeof(s_crc));
    s32 r2 = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
    lprintf("[hdr] ready resp: r1=%d r2=%d\n", (int)r1, (int)r2);
    if (r1 <= 0) return -1;

    // IOS58 GBA (r1==16): drain the remaining 3 zero packets from the IOS queue.
    // IOS58 GB/GBC (r1==60): ROM data begins immediately — no drain.
    // d2x-cIOS (r1==64): device sends N non-ZLP packets then a ZLP before streaming.
    //   Drain until ZLP (rd=0) — count varies by IOS buffer state (typically 8+ZLP).
    //   Guard on s_ack_read_size==GBOP_PKT_SIZE so this never fires on IOS58 even if
    //   an unexpected r1 value occurs (avoids a hanging 64-byte read on IOS58).
    if (r1 == 16) {
        for (int i = 0; i < 3; i++) {
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 dr = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            lprintf("[hdr] drain[%d]: rd=%d\n", i, (int)dr);
        }
    } else if (r1 != 60 && r1 > 0 && s_ack_read_size == GBOP_PKT_SIZE) {
        for (int i = 0; i < 32; i++) {
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 dr = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            lprintf("[hdr] drain-d2x[%d]: rd=%d\n", i, (int)dr);
            if (dr <= 0) break;
        }
    }

    // Read ROM data chunks until probe_size bytes accumulated.
    // Data arrives as rd=60 packets; every 5th chunk is rd=16 (device behavior).
    // 512 bytes = 10 packets (4×60 + 1×16 + 4×60 + 1×16), no ACK needed.
    uint32_t rx = 0;
    uint32_t chunk_cnt = 0;
    while (rx < probe_size) {
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        if (rd <= 0) {
            lprintf("[hdr] read error rd=%d at rx=%u chunk=%u\n",
                    (int)rd, rx, chunk_cnt);
            break;
        }
        uint32_t copy = (uint32_t)rd;
        if (rx + copy > probe_size) copy = probe_size - rx;
        memcpy(hdr_out + rx, s_rx, copy);
        rx += (uint32_t)rd;
        if (rx > probe_size) rx = probe_size;
        chunk_cnt++;
    }
    lprintf("[hdr] Got %u bytes in %u chunks [0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
            rx, chunk_cnt,
            hdr_out[0], hdr_out[1], hdr_out[2], hdr_out[3],
            hdr_out[4], hdr_out[5], hdr_out[6], hdr_out[7]);

    // No post-read drain. Neither GBA nor GB/GBC sends end-of-stream packets
    // after a 512-byte mini-dump — USB_ReadBlkMsg(60) blocks forever on the
    // empty IOS buffer (asymmetric IOS timeout: only 4-byte reads time out with
    // -7008; 60-byte reads hang). The "r1=60, all-zeros" contamination on a
    // second mini-dump on the same fd is device state machine behaviour, not IOS
    // buffer content. It is handled in main.c via same-cart raw_resp caching:
    // a mini-dump is only attempted when the cart actually changes, at which
    // point a physical reconnect gives a fresh fd and r1=16 (test_100).

    return (rx >= probe_size) ? 0 : -1;
}

// Old-firmware (v9.2.0 and earlier) implementation — see gbop_dump_rom()
// dispatcher below. Do not modify based on new-firmware findings.
static int gbop_dump_rom_legacy(GBOperatorHandle handle, const CartInfo *info,
                  uint8_t *buffer, uint32_t buffer_size) {
    if (!handle || !info || !buffer || buffer_size == 0) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    uint32_t total = info->rom_size_kb * 1024;
    if (total == 0) return -1;

    // ---- First call: send Read ROM command (0x00) and consume the ACK ----
    if (!dev->dump_active) {
        // Command format from gbopyrator _craft_rom_read_trigger:
        //   byte 0 = 0x00 (Read ROM)
        //   byte 1 = 0x02
        //   bytes 2-5 = ROM size in bytes, 4-byte little-endian
        uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x00, 0x02 };
        cmd[2] = (uint8_t)((total >>  0) & 0xFF);
        cmd[3] = (uint8_t)((total >>  8) & 0xFF);
        cmd[4] = (uint8_t)((total >> 16) & 0xFF);
        cmd[5] = (uint8_t)((total >> 24) & 0xFF);

        lprintf("[gbop] ROM dump start: size=%u bytes (%u KB)\n", total, total / 1024);
        if (gbop_bulk_send(dev, cmd) < 0) return -1;

        // Command ACK: read size auto-detected.
        s32 ra = gbop_ack_read(dev);
        // A ZLP (rd=0) here means a batch-terminator from a prior gbop_read_rom_header
        // mini-dump is still sitting in the IOS EP IN buffer (IOS does not flush on
        // close/reopen on d2x-cIOS). Re-read to get the real ACK.
        if (ra == 0) {
            lprintf("[gbop] ZLP before ROM dump cmd ACK — re-reading\n");
            ra = gbop_ack_read(dev);
        }
        DCInvalidateRange(s_crc, sizeof(s_crc));
        s32 rb = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
        lprintf("[gbop] ROM dump cmd ACK: r%d=%d r4=%d [0..3]=%02X %02X %02X %02X\n",
                s_ack_read_size, (int)ra, (int)rb,
                s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
        if (ra <= 0) return -1;

        // Initial host ACK: gbopyrator sends 64 zero bytes to the device before
        // EVERY batch of 320 chunks, starting with batch 0. Without this the device
        // does not start streaming and returns -7005 on the first data read.
        memset(s_tx, 0, GBOP_PKT_SIZE);
        DCFlushRange(s_tx, GBOP_PKT_SIZE);
        s32 wack = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
        lprintf("[gbop] Host ACK write: w=%d\n", (int)wack);
        if (wack < 0) return -1;

        // Device "ready to stream" response to the initial host ACK: 84 bytes total across
        // 4 USB packets (16+4+60+4 bytes, all zeros). Read sequence on Wii:
        //   - USB_ReadBlkMsg(60) → rd=16 (16-byte short packet, IOS terminates early)
        //   - USB_ReadBlkMsg(4)  → -7008 (IOS 4-byte timeout fires before 4-byte packet arrives)
        //   - The remaining 3 USB packets (4+60+4 bytes) sit in the IOS receive queue
        // All 84 bytes must be consumed before ROM data begins, or the streaming loop
        // reads them as the first 68 bytes of the ROM file (corrupting the output).
        s32 r1 = gbop_ack_read(dev);
        DCInvalidateRange(s_crc, sizeof(s_crc));
        s32 r2 = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
        lprintf("[gbop] Host ACK resp: r%d=%d r4=%d [0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                s_ack_read_size, (int)r1, (int)r2,
                s_rx[0], s_rx[1], s_rx[2], s_rx[3],
                s_rx[4], s_rx[5], s_rx[6], s_rx[7]);
        if (r1 <= 0) return -1;

        // IOS58 GBA (r1==16): initial host ACK triggers an extended device "ready" response —
        // 84 bytes across 4 USB packets (16+4+60+4, all zeros). The r60 read above
        // returned rd=16 (short packet); the remaining 3 packets are queued in the
        // IOS receive queue and must be drained before ROM data begins.
        //
        // IOS58 GB/GBC (r1==60): ready response is only the 60+4 bytes already read above.
        // ROM streaming begins immediately with the next USB_ReadBlkMsg — no drain.
        // Draining here for GB/GBC consumes the first 3×60 = 180 bytes of ROM data,
        // causing a 3-chunk offset in dump_chunk_cnt and a stall at chunk 317.
        //
        // d2x-cIOS (r1==64): device sends N non-ZLP packets then a ZLP before streaming.
        // Same ZLP-terminated drain as gbop_read_rom_header.
        if (r1 == 16) {
            for (int i = 0; i < 3; i++) {
                DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
                lprintf("[gbop] drain[%d]: rd=%d [0..3]=%02X %02X %02X %02X\n",
                        i, (int)rd, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
            }
        } else if (r1 != 60 && r1 > 0 && s_ack_read_size == GBOP_PKT_SIZE) {
            for (int i = 0; i < 32; i++) {
                DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                s32 dr = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
                lprintf("[gbop] drain-d2x[%d]: rd=%d [0..3]=%02X %02X %02X %02X\n",
                        i, (int)dr, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
                if (dr <= 0) break;
            }
        }

        dev->dump_total         = total;
        dev->dump_given         = 0;
        dev->dump_chunk_cnt     = 0;
        dev->dump_iter_cnt      = 0;
        dev->dump_rx_bytes      = 0;
        dev->dump_pending_drain = 0;
        dev->dump_zlp_streak    = 0;
        dev->dump_spare_len     = 0;
        dev->dump_active        = 1;
    }

    uint32_t written = 0;

    // Drain any leftover bytes from the previous device chunk read
    if (dev->dump_spare_len > 0) {
        uint32_t copy = dev->dump_spare_len;
        if (copy > buffer_size) copy = buffer_size;
        memcpy(buffer, dev->dump_spare, copy);
        written += copy;
        dev->dump_spare_len -= copy;
        if (dev->dump_spare_len > 0)
            memmove(dev->dump_spare, dev->dump_spare + copy, dev->dump_spare_len);
    }

    // Read ROM data chunks until the caller's buffer is full.
    //
    // ACK protocol: gbopyrator fires ACK every 320 counted reads, then does 2 explicit
    // drain reads (not counted). Total per cycle = 322 USB reads. The device sends its
    // rd=4 "ACK received" response every 322 USB reads. Matching this cycle length keeps
    // the device's pending-ACK buffer from filling up (max depth ~2).
    //
    // drain[0] = rd=60 all-zeros (device ACK) — always overhead, always discarded.
    // drain[1] differs by IOS variant:
    //   IOS58:    rd=4  all-zeros (ACK part 2, overhead) — discard.
    //   d2x-cIOS: rd=16 (first data packet of next batch, real ROM data) — count and copy.
    //             The device starts streaming the next batch before sending its rd=4
    //             confirmation, which lands in the main loop at iter+2 instead.
    // drain[0] injected as ROM data corrupts 60 bytes at every 16KB boundary (61,380
    // bytes in a 16MB dump); the adaptive drain[1] fix prevents 16 bytes/batch drift.

    // Inline helper: copy s_rx (rd bytes) into caller's buffer, spill overflow into spare.
    // Returns 1 if buffer is now full.
#define COPY_TO_BUF(rd_val) do {                                                   \
        uint32_t _remaining = dev->dump_total - (dev->dump_given + written);       \
        if (_remaining > 0) {                                                       \
            uint32_t _cb = ((uint32_t)(rd_val) < _remaining) ?                    \
                           (uint32_t)(rd_val) : _remaining;                        \
            uint32_t _fits = buffer_size - written;                                \
            if (_cb <= _fits) {                                                     \
                memcpy(buffer + written, s_rx, _cb);                               \
                written += _cb;                                                     \
            } else {                                                                \
                memcpy(buffer + written, s_rx, _fits);                             \
                memcpy(dev->dump_spare, s_rx + _fits, _cb - _fits);               \
                dev->dump_spare_len = _cb - _fits;                                 \
                written = buffer_size;                                              \
            }                                                                       \
        }                                                                           \
    } while (0)

    while (written < buffer_size && dev->dump_given + written < dev->dump_total) {
        // Execute drain reads deferred from the previous in-stream ACK.
        // Deferring to the top of the next call (written=0) guarantees both
        // drain reads always happen — the inline approach skipped drain[1]
        // whenever the caller's buffer filled at the ACK boundary, causing
        // ~0.5 read of drift per batch and a stall after ~277 batches.
        while (dev->dump_pending_drain > 0) {
            dev->dump_pending_drain--;
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 dr = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            lprintf("[gbop] drain rd=%d iter=%u [0..3]=%02X %02X %02X %02X\n",
                    (int)dr, dev->dump_iter_cnt,
                    s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
            if (dr < 0) {
                dev->dump_active = 0;
                return -1;
            }
            // drain[0] (rd=60): overhead — discard unconditionally.
            // drain[1] (dump_pending_drain now 0): adaptive per hardware variant —
            //   rd<16  → device ACK response (IOS58: rd=4) — discard.
            //   rd>=16 → first packet of next batch (d2x-cIOS: rd=16) — count as ROM data.
            if (dev->dump_pending_drain == 0 && dr >= 16) {
                dev->dump_chunk_cnt++;
                dev->dump_rx_bytes += (uint32_t)dr;
                COPY_TO_BUF(dr);
            }
        }
        if (written >= buffer_size) break;

        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        if (rd == 0) {
            // ZLP mid-stream: batch-terminator on some d2x-cIOS setups (every 512 bytes),
            // always immediately followed by resumed real data on a healthy connection.
            // On a connection that has gone silent (device stopped streaming entirely),
            // USB_ReadBlkMsg keeps returning rd=0 forever and this would spin without
            // end — observed as an indefinitely repeating "ZLP mid-stream" log on an
            // external Wii with a marginal cart connection. Cap consecutive ZLPs so a
            // dead stream fails cleanly instead of hanging.
            dev->dump_zlp_streak++;
            if (dev->dump_zlp_streak > GBOP_MAX_ZLP_STREAK) {
                lprintf("[gbop] ROM stream dead: %u consecutive ZLPs at iter %u chunk %u — aborting\n",
                        dev->dump_zlp_streak, dev->dump_iter_cnt, dev->dump_chunk_cnt);
                dev->dump_active = 0;
                return -1;
            }
            lprintf("[gbop] ZLP mid-stream iter=%u chunk=%u rx=%u\n",
                    dev->dump_iter_cnt, dev->dump_chunk_cnt, dev->dump_rx_bytes);
            continue;
        }
        if (rd < 0) {
            lprintf("[gbop] ROM read error: %d at iter %u chunk %u\n",
                    (int)rd, dev->dump_iter_cnt, dev->dump_chunk_cnt);
            dev->dump_active = 0;
            return -1;
        }

        dev->dump_zlp_streak = 0;
        dev->dump_iter_cnt++;

        if (rd >= 16) {
            if (dev->dump_chunk_cnt < 5) {
                lprintf("[gbop] chunk[%u] iter=%u rd=%d [0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        dev->dump_chunk_cnt, dev->dump_iter_cnt, (int)rd,
                        s_rx[0], s_rx[1], s_rx[2], s_rx[3],
                        s_rx[4], s_rx[5], s_rx[6], s_rx[7]);
            }
            dev->dump_chunk_cnt++;
            dev->dump_rx_bytes += (uint32_t)rd;
            COPY_TO_BUF(rd);
        } else {
            lprintf("[gbop] resp rd=%d iter=%u [0..3]=%02X %02X %02X %02X\n",
                    (int)rd, dev->dump_iter_cnt,
                    s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
        }

        // Fire in-stream host ACK every 320 iter reads. After sending, defer 2
        // drain reads to the top of the next iteration: drain[0]=rd=60 overhead,
        // drain[1]=adaptive (rd=4 discard on IOS58; rd=16 ROM data counted on d2x-cIOS).
        // The device's rd=4 confirmation lands at iter+2 in the main loop on d2x-cIOS.
        if (dev->dump_iter_cnt % 320 == 0) {
            uint32_t batch = dev->dump_iter_cnt / 320;
            lprintf("[gbop] ACK#%u iter=%u chunk=%u rx=%uKB rd=%d [0..3]=%02X %02X %02X %02X\n",
                    batch, dev->dump_iter_cnt, dev->dump_chunk_cnt,
                    dev->dump_rx_bytes / 1024, (int)rd,
                    s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
            memset(s_tx, 0, GBOP_PKT_SIZE);
            DCFlushRange(s_tx, GBOP_PKT_SIZE);
            s32 wack2 = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
            if (wack2 < 0) {
                lprintf("[gbop] ACK#%u error: w=%d\n", batch, (int)wack2);
                DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                s32 rp = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_rx);
                lprintf("[gbop] post-fail probe: rd=%d [%02X %02X %02X %02X]\n",
                        (int)rp, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
                dev->dump_active = 0;
                return -1;
            }
            dev->dump_pending_drain = 2;
        }

        if (written >= buffer_size) break;
    }

#undef COPY_TO_BUF

    dev->dump_given += written;

    // Log progress every 256 KB — visible on TV and written through to the SD
    // directory entry via lprintf's fflush.
#define DUMP_LOG_INTERVAL (512 * 1024)
    uint32_t prev_mark = (dev->dump_given - written) / DUMP_LOG_INTERVAL;
    uint32_t curr_mark = dev->dump_given / DUMP_LOG_INTERVAL;
    if (curr_mark > prev_mark || dev->dump_given >= dev->dump_total) {
        lprintf("[gbop] ROM dump: %u KB / %u KB (%u%%)\n",
                dev->dump_given / 1024, dev->dump_total / 1024,
                dev->dump_given * 100 / dev->dump_total);
        // Flush to commit buffered data to FAT clusters. Note: libfat only
        // updates the directory entry (file size) on fclose, so a hard power-cut
        // mid-dump may leave a stale size in the directory. fflush is used here
        // instead of close/reopen to prevent g_log from becoming NULL if the
        // reopen fails, which would silently drop all subsequent log output.
        if (g_log) fflush(g_log);
    }

    if (dev->dump_given >= dev->dump_total) {
        /* Drain any deferred in-stream ACK response packets.  The last ACK fires at
         * the exact last iteration, sets dump_pending_drain=2, then the loop breaks
         * immediately (written==buffer_size) — the deferred drains never execute.
         * Without this flush, 2 residual packets (rd=60 + rd=4) sit in the IOS EP IN
         * buffer.  The next gbop_bulk_recv call (cart_info in play_game) reads them as
         * the ACK and 4-byte footer, consuming real response data from the wrong slot
         * → resp all-zeros → NOCART → "Cart not detected" after a successful dump. */
        while (dev->dump_pending_drain > 0) {
            dev->dump_pending_drain--;
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 dr = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            lprintf("[gbop] final drain rd=%d [0..3]=%02X %02X %02X %02X\n",
                    (int)dr, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
        }
        dev->dump_active = 0;
        lprintf("[gbop] ROM dump complete: given=%u total=%u chunks=%u\n",
                dev->dump_given, dev->dump_total, dev->dump_chunk_cnt);
    }

    return 0;
}

// GBA ROM size verification (mirror check) ----------------------------------
//
// On some connections the cart-info command (0x04) reports a GBA ROM size
// that is exactly double the cart's real size. Confirmed on both Sapphire
// (AXPE) and FireRed (BPRE) — two different cartridges with different flash
// chips, on the same external Wii — so this is not a per-cart firmware quirk;
// it tracks the same anomalous byte pattern already used to guess save type
// (resp[27]==0x00 with resp[28] echoing resp[26]).
//
// Rather than trust a hardcoded per-game size table, this asks the cartridge
// itself: real GBA ROM chips mirror when addressed past their physical
// capacity (the address lines wrap back to 0), so if the reported size is
// 2x too large, the "second half" of a read is bit-identical to the first
// half — down to the Nintendo logo and reset vector, an effectively
// impossible coincidence for real game data. This streams up to the reported
// halfway point (the same data volume a corrected dump would need anyway)
// and compares a window there against the same-size window at offset 0.
#define GBOP_VERIFY_CHUNK 4096
static uint8_t s_verify_buf[GBOP_VERIFY_CHUNK];
static uint8_t s_verify_first_window[GBOP_VERIFY_CHUNK];

int gbop_verify_gba_rom_size(GBOperatorHandle handle, CartInfo *info, int *out_did_stream) {
    if (out_did_stream) *out_did_stream = 0;
    if (!handle || !info || info->type != CART_TYPE_GBA) return 0;

    // Gate on the same byte pattern gbop_read_cart_info already flags for the
    // save-size quirk — every confirmed doubled-size case showed it, and
    // skipping the check otherwise avoids doubling transfer time for carts
    // that already report correctly.
    if (!(info->raw_resp[27] == 0 && info->raw_resp[26] != 0 &&
          info->raw_resp[28] == info->raw_resp[26])) {
        return 0;
    }

    uint32_t reported_total = info->rom_size_kb * 1024;
    uint32_t midpoint = reported_total / 2;
    if (reported_total < GBOP_VERIFY_CHUNK * 2 || midpoint % GBOP_VERIFY_CHUNK != 0) {
        lprintf("[gbop] ROM size verification skipped (size %u KB not checkable)\n",
                info->rom_size_kb);
        return 0;
    }

    lprintf("[gbop] Verifying reported ROM size (%u KB) via mirror check before dump\n",
            info->rom_size_kb);
    if (out_did_stream) *out_did_stream = 1;

    uint32_t offset = 0;
    int mirror_detected = 0;
    while (offset < midpoint + GBOP_VERIFY_CHUNK) {
        if (gbop_dump_rom(handle, info, s_verify_buf, GBOP_VERIFY_CHUNK) != 0) {
            lprintf("[gbop] ROM size verification aborted (read error) — using reported size\n");
            return 0;
        }
        if (offset == 0) {
            memcpy(s_verify_first_window, s_verify_buf, GBOP_VERIFY_CHUNK);
        } else if (offset == midpoint) {
            mirror_detected = (memcmp(s_verify_buf, s_verify_first_window, GBOP_VERIFY_CHUNK) == 0);
        }
        offset += GBOP_VERIFY_CHUNK;
    }

    if (mirror_detected) {
        lprintf("[gbop] Mirror detected at offset 0x%08X — correcting ROM size %u KB -> %u KB\n",
                midpoint, info->rom_size_kb, info->rom_size_kb / 2);
        info->rom_size_kb /= 2;
        return 1;
    }

    lprintf("[gbop] No mirroring detected at offset 0x%08X — reported size confirmed\n", midpoint);
    return 0;
}

/* Removed sd_commit (fclose+fopen) — it stalls USB bulk transfers by 200-500ms,
 * causing the GB Operator to stall the IN endpoint and blocking USB_ReadBlkMsg(60)
 * indefinitely.  fflush is used instead: fast (stdio buffer only, no SD sector
 * commit), no IOS contention, and safe to call from a background thread. */
#define log_flush_safe() do { if (g_log) fflush(g_log); } while (0)

// Old-firmware (v9.2.0 and earlier) implementation — see gbop_read_save()
// dispatcher below. Do not modify based on new-firmware findings.
static int gbop_read_save_legacy(GBOperatorHandle handle, const CartInfo *info,
                   uint8_t *buffer, uint32_t buffer_size) {
    if (!handle || !info || !buffer || buffer_size == 0) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    uint32_t save_size = info->ram_size_kb * 1024;
    if (save_size == 0) {
        lprintf("[gbop] gbop_read_save: save size unknown (ram_size_kb=0)\n");
        return -1;
    }
    if (buffer_size < save_size) {
        lprintf("[gbop] gbop_read_save: buffer too small (%u < %u)\n",
                buffer_size, save_size);
        return -1;
    }

    // Command 0x02: Read Save.
    // Bytes[1..5] differ by cart type (confirmed from Epilogue Playback USB captures test_43/test_46):
    //   GBA: cmd[1]=0x03, cmd[4]=0x00, cmd[5]=0x01  (Flash — test_43 confirmed, test_45 bit-perfect)
    //   GBC: cmd[1]=0x00, cmd[4]=0x20, cmd[5]=0x00  (SRAM  — test_46 GBC_SAVE_CART2WIN capture)
    // Size at bytes 6-8 LE for both types.
    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x02 };
    if (info->type == CART_TYPE_GBA) {
        cmd[1] = 0x03; cmd[4] = 0x00; cmd[5] = 0x01;
    } else {
        cmd[1] = 0x00; cmd[4] = 0x20; cmd[5] = 0x00;
    }
    cmd[6] = (uint8_t)((save_size >>  0) & 0xFF);
    cmd[7] = (uint8_t)((save_size >>  8) & 0xFF);
    cmd[8] = (uint8_t)((save_size >> 16) & 0xFF);

    lprintf("[gbop] Save read start: size=%u bytes (%u KB)\n", save_size, save_size / 1024);
    log_flush_safe();  // flush before first blocking USB op so log survives a hang
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    // Command ACK: read size auto-detected (60 on IOS58, 64 on d2x-cIOS).
    s32 ra = gbop_ack_read(dev);
    // A ZLP (rd=0) here means a batch-terminator from a prior gbop_read_rom_header
    // mini-dump is still sitting in the IOS EP IN buffer (IOS does not flush on
    // close/reopen on d2x-cIOS). Re-read to get the real ACK.
    if (ra == 0) {
        lprintf("[gbop] ZLP before save cmd ACK — re-reading\n");
        ra = gbop_ack_read(dev);
    }
    DCInvalidateRange(s_crc, sizeof(s_crc));
    s32 rb = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
    lprintf("[gbop] Save cmd ACK: r%d=%d r4=%d [0..3]=%02X %02X %02X %02X\n",
            s_ack_read_size, (int)ra, (int)rb, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
    if (ra <= 0) return -1;

    // Commit ACK to SD before streaming so the log survives a power-cycle hang.
    log_flush_safe();

    uint32_t received = 0;
    uint32_t pkt_cnt  = 0;
    int      first_nz = -1;

    // Post-ACK drain: device sends extra packets before save data begins.
    //
    // GBA cold (rb<0): the 4-byte ACK footer arrived late and was not caught by
    //   the rb read (IOS timed out).  It now sits in the IOS EP IN buffer, along
    //   with 0–3 "ready-to-stream" packets (rd=16, rd=60, rd=4 — all zeros) that
    //   the device sends before data.  The count varies with device state (idle vs
    //   post-ROM-dump).  Drain with 4-byte non-blocking reads until IOS returns
    //   -7008 (timeout = buffer empty).  4-byte reads also safely truncate any
    //   larger drain packets (rd=16 truncated to 4, rd=60 truncated to 4).
    //
    // GBC after write (rb>=0): 64-byte overhead (rd=60+rd=4, all zeros).
    //   Observed in test_47 verify read; absent in cold reads (test_46 capture).
    //   Peek-2: if (pkt_a=60)+(pkt_b<60) → drain pair; else both are real data.
    if (rb < 0) {
        // GBA save ready-to-stream drain: device sends rd=16 + rd=60 + rd=4
        // (3 fixed packets, all zeros) before streaming begins.
        // Must use GBOP_PKT_SIZE reads — USB_ReadBlkMsg(4) does NOT truncate
        // larger packets; it blocks waiting for a 4-byte packet that never
        // arrives (test_113: hang at save drain[0] with rd=16 present).
        for (int di = 0; di < 3; di++) {
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 dr = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            lprintf("[gbop] save drain[%d]: rd=%d [0..3]=%02X %02X %02X %02X\n",
                    di, (int)dr, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
            if (dr < 0) break;
        }
        log_flush_safe();
    } else if (info->type != CART_TYPE_GBA) {
        // GBC: peek two packets to detect post-write protocol overhead
        uint8_t pa[GBOP_PKT_SIZE], pb[GBOP_PKT_SIZE];
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 pa_rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        if (pa_rd > 0) memcpy(pa, s_rx, (size_t)pa_rd);
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 pb_rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        if (pb_rd > 0) memcpy(pb, s_rx, (size_t)pb_rd);
        lprintf("[gbop] gbc peek: pkt_a=%d pkt_b=%d\n", (int)pa_rd, (int)pb_rd);
        if (pa_rd == 60 && pb_rd > 0 && pb_rd < 60) {
            // protocol overhead after write: 60+(short) → discard
            lprintf("[gbop] gbc drain: 60+%d discarded\n", (int)pb_rd);
        } else if (pa_rd > 0 && pb_rd > 0) {
            // real data — inject both pre-read packets into buffer
            uint8_t *pre[2] = { pa, pb };
            s32 pre_sz[2]   = { pa_rd, pb_rd };
            for (int p = 0; p < 2 && received < save_size; p++) {
                s32 rd = pre_sz[p];
                if (rd <= 0) break;
                pkt_cnt++;
                lprintf("[gbop] pkt=%u rd=%d [pre] [0..3]=%02X %02X %02X %02X rx=%u\n",
                        pkt_cnt, (int)rd, pre[p][0], pre[p][1], pre[p][2], pre[p][3], received);
                uint32_t copy = (uint32_t)rd;
                if (copy > save_size - received) copy = save_size - received;
                memcpy(buffer + received, pre[p], copy);
                if (first_nz < 0) {
                    for (uint32_t i = 0; i < copy; i++) {
                        if (pre[p][i]) {
                            first_nz = (int)(received + i);
                            lprintf("[gbop] FIRST NZ: offset=0x%04X val=0x%02X pkt=%u\n",
                                    (unsigned)first_nz, (unsigned)pre[p][i], pkt_cnt);
                            break;
                        }
                    }
                }
                received += copy;
            }
        } else {
            lprintf("[gbop] gbc peek error: pkt_a=%d pkt_b=%d\n", (int)pa_rd, (int)pb_rd);
            return -1;
        }
        log_flush_safe();
    }

    while (received < save_size) {
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);

        if (rd <= 0) {
            lprintf("[gbop] save rd=%d at pkt=%u rx=%u\n", (int)rd, pkt_cnt, received);
            return -1;
        }

        pkt_cnt++;

        // Discard sub-16-byte packets: deferred 4-byte ACK footer that IOS missed
        // in the r4 read (warm-USB path) arrives here at pkt=2.  Real save data
        // is always rd=60 or rd=16; any smaller packet is protocol overhead.
        if (rd < 16) {
            lprintf("[gbop] pkt=%u rd=%d [proto skip]\n", pkt_cnt, (int)rd);
            if (g_log) fflush(g_log);
            continue;
        }

        // Log first 30 packets and anything unexpected (not rd=60 or rd=16)
        if (pkt_cnt <= 30 || (rd != 60 && rd != 16)) {
            lprintf("[gbop] pkt=%u rd=%d [0..3]=%02X %02X %02X %02X rx=%u\n",
                    pkt_cnt, (int)rd,
                    s_rx[0], s_rx[1], s_rx[2], s_rx[3], received);
        }

        uint32_t copy = (uint32_t)rd;
        if (copy > save_size - received) copy = save_size - received;
        memcpy(buffer + received, s_rx, copy);

        // Log the byte at FireRed's first expected non-zero offset (0xFF4 = 4084)
        if (received <= 0xFF4 && received + copy > 0xFF4) {
            uint32_t off = 0xFF4 - received;
            lprintf("[gbop] offset 0xFF4: 0x%02X (pkt=%u)\n",
                    (unsigned)s_rx[off], pkt_cnt);
            if (g_log) fflush(g_log);
        }

        if (first_nz < 0) {
            for (uint32_t i = 0; i < copy; i++) {
                if (s_rx[i]) {
                    first_nz = (int)(received + i);
                    lprintf("[gbop] FIRST NZ: offset=0x%04X val=0x%02X pkt=%u\n",
                            (unsigned)first_nz, (unsigned)s_rx[i], pkt_cnt);
                    if (g_log) fflush(g_log);
                    break;
                }
            }
        }

        received += copy;

        if (received % (8 * 1024) == 0 || received >= save_size) {
            lprintf("[save] %u / %u KB (%u pkts)\n",
                    received / 1024, save_size / 1024, pkt_cnt);
            if (g_log) fflush(g_log);
        }
    }

    lprintf("[gbop] save complete: %u bytes %u pkts nz=%d\n",
            received, pkt_cnt, first_nz);
    return 0;
}

// Old-firmware (v9.2.0 and earlier) implementation — see gbop_write_save()
// dispatcher below. Do not modify based on new-firmware findings.
static int gbop_write_save_legacy(GBOperatorHandle handle, const CartInfo *info,
                    const uint8_t *buffer, uint32_t buffer_size) {
    if (!handle || !info || !buffer || buffer_size == 0) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    uint32_t save_size = info->ram_size_kb * 1024;
    if (save_size == 0) {
        lprintf("[gbop] gbop_write_save: save size unknown (ram_size_kb=0)\n");
        return -1;
    }
    if (buffer_size < save_size) {
        lprintf("[gbop] gbop_write_save: buffer too small (%u < %u)\n",
                buffer_size, save_size);
        return -1;
    }

    // Command 0x03: Write Save.
    // Bytes[1..5] differ by cart type (confirmed from Epilogue Playback USB captures test_46):
    //   GBA: cmd[1]=0x03, cmd[4]=0x00, cmd[5]=0x01  (GBA_SAVE_WIN2CART capture)
    //   GBC: cmd[1]=0x00, cmd[4]=0x20, cmd[5]=0x00  (GBC_SAVE_WIN2CART capture)
    // Size at bytes 6-8 LE for both types. No initial host ACK; data starts immediately after
    // command ACK. Per-chunk ACK: 60+4 bytes per data packet (same for GBA and GBC).
    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x03 };
    if (info->type == CART_TYPE_GBA) {
        cmd[1] = 0x03; cmd[4] = 0x00; cmd[5] = 0x01;
    } else {
        cmd[1] = 0x00; cmd[4] = 0x20; cmd[5] = 0x00;
    }
    cmd[6] = (uint8_t)((save_size >>  0) & 0xFF);
    cmd[7] = (uint8_t)((save_size >>  8) & 0xFF);
    cmd[8] = (uint8_t)((save_size >> 16) & 0xFF);

    lprintf("[gbop] Save write start: size=%u bytes (%u KB)\n", save_size, save_size / 1024);
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    // Command ACK: read size auto-detected.
    s32 ra = gbop_ack_read(dev);
    DCInvalidateRange(s_crc, sizeof(s_crc));
    s32 rb = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
    lprintf("[gbop] Write cmd ACK: r%d=%d r4=%d [0..3]=%02X %02X %02X %02X\n",
            s_ack_read_size, (int)ra, (int)rb,
            s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
    if (ra <= 0) return -1;

    // NO initial host ACK — gbopyrator (coms_utils.py write_save) sends none.
    // The previous implementation sent 64 zero bytes here, which the device treated
    // as the first 64-byte data chunk, writing zeros to SRAM[0..63] and shifting
    // all subsequent data by one chunk. Data upload begins immediately after the ACK.

    // Stream save data in 64-byte raw data packets.
    // Protocol confirmed from gbopyrator write_bulk_out:
    //   for each 64-byte chunk:
    //     write(OUT, 64 bytes of data)     — raw, no CRC
    //     read(IN, 60)                     — per-chunk ACK (device sends 60 or 4 bytes)
    //     read(IN, 4)                      — ACK footer (may be -7008 if absent)
    //
    // On Wii: USB_ReadBlkMsg(60) blocks until any USB packet arrives — no -7008 timeout
    // for 60-byte reads. Returns actual packet size (may be 4 or 60). This avoids the
    // retry-loop pattern needed for 4-byte reads.
    uint32_t sent = 0;
    uint32_t chunk_cnt = 0;

    while (sent < save_size) {
        uint32_t chunk = save_size - sent;
        if (chunk > GBOP_PKT_SIZE) chunk = GBOP_PKT_SIZE;  // 64 bytes

        memcpy(s_tx, buffer + sent, chunk);
        if (chunk < GBOP_PKT_SIZE) memset(s_tx + chunk, 0, GBOP_PKT_SIZE - chunk);
        DCFlushRange(s_tx, GBOP_PKT_SIZE);

        s32 ww = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);

        if (chunk_cnt < 5 || ww < 0) {
            lprintf("[gbop] write[%u] w=%d [0..3]=%02X %02X %02X %02X\n",
                    chunk_cnt, (int)ww,
                    s_tx[0], s_tx[1], s_tx[2], s_tx[3]);
        }
        if (ww < 0) {
            lprintf("[gbop] write error at chunk %u\n", chunk_cnt);
            return -1;
        }

        sent += chunk;
        chunk_cnt++;

        // Per-chunk ACK: read size auto-detected (handles 4, 16, 60, or 64-byte packets).
        {
            s32 rack60 = gbop_ack_read(dev);
            DCInvalidateRange(s_crc, sizeof(s_crc));
            s32 rack4  = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
            if (chunk_cnt <= 5 || rack60 < 0) {
                lprintf("[gbop] write ACK[%u] r%d=%d r4=%d [0..3]=%02X %02X %02X %02X\n",
                        chunk_cnt - 1, s_ack_read_size, (int)rack60, (int)rack4,
                        s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
            }
            if (rack60 < 0) {
                lprintf("[gbop] write ACK error at chunk %u: r%d=%d\n",
                        chunk_cnt - 1, s_ack_read_size, (int)rack60);
                return -1;
            }
        }

        if (sent % (8 * 1024) == 0 || sent >= save_size) {
            lprintf("[write] %u / %u KB\n", sent / 1024, save_size / 1024);
            log_flush_safe();  /* fflush — ensures log progress is in libfat cache */
        }
    }

    // GBA Flash: device programs asynchronously after the last chunk ACK.
    // The 60-byte completion packet is a notification only — we do not wait
    // for it here because it can block for 10-30s and prevents clean shutdown.
    // Flash programming proceeds on the device regardless of host connection.

    lprintf("[gbop] save write complete: %u bytes, %u chunks\n", sent, chunk_cnt);
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// NEW FIRMWARE PROTOCOL (GB Operator v10.0.10+) — added 2026-07-27
//
// Reverse-engineered from Wireshark captures of Epilogue Playback v1.10.0
// against GB Operator firmware v10.0.10 (logs/Firmware-V10_0_10-WireShark/).
// See CLAUDE.md "NEW FIRMWARE PROTOCOL REBUILT FROM WIRESHARK" for the full
// writeup this implementation is based on. Summary:
//
//   - Every command's response is bracketed by a fixed 512-byte marker
//     block: C0 DE <seq> <cmd echo> <~cmd> + zero padding. seq=0x00 for the
//     header (replaces the old firmware's all-zero ACK), seq=0x01 for the
//     footer (sent once the operation is complete).
//   - Cart-info (cmd 0x04): header marker, one 512-byte data block (real
//     60-byte response in the first 64 bytes, zero-padded), footer marker.
//   - ROM read (cmd 0x00) / save read (cmd 0x02): header marker, then the
//     device pushes real data continuously with NO host ACKs of any kind,
//     terminated by the footer marker. The old "ACK every 320 packets"
//     cadence does not exist on this firmware.
//   - Save write (cmd 0x03): header marker, then the HOST pushes all data
//     as plain 64-byte chunks back-to-back with NO per-chunk ACKs, and the
//     device sends the footer marker once it has consumed everything.
//   - No field in the cart-info response has been found to reliably encode
//     ROM/RAM size (resp[26..29] is a fixed constant across every cart and
//     type tested, not a size code) — see rom_sizes.h for how size is
//     resolved instead.
//
// UNVERIFIED ON REAL WII HARDWARE: the exact 64-byte-packet framing within
// each 512-byte marker block (assumed here to be 8 consecutive full 64-byte
// USB_ReadBlkMsg reads, inferred from Epilogue's PC-side URB always
// completing at exactly 512 bytes with no short packet) has not been
// independently confirmed at the Wii/IOS level — the PC-side capture tool
// abstracts real USB packet boundaries away. Every read here is logged
// verbosely so a first hardware test immediately reveals whether this
// framing guess is right. Likewise unverified: what the device does if the
// requested ROM/save size (resolved from rom_sizes.h) doesn't match the
// cart's real capacity — every capture used to reverse-engineer this had
// Epilogue requesting the objectively correct size.
// ═══════════════════════════════════════════════════════════════════════

#define NEWPROTO_MARKER_PACKETS 8   // 512-byte marker block = 8 x 64-byte packets
#define NEWPROTO_MAX_STALL 4096     // safety cap: reads with no progress before giving up
#define NEWPROTO_LOG_INTERVAL (512 * 1024)

// NEWPROTO_PRESTREAM_SETTLE_US REMOVED (2026-07-29) — a 20ms pause between
// sending a 0x00 command and the first marker read, added one session ago
// to test whether reintroducing some of the old firmware's lost handshake
// pacing (see logs/test_122 vs. the new protocol) would help the marker-read
// success rate. Test_34 data: the persistent-handle attempt (which always
// gets this delay, since it's attempt #1 of every cycle) succeeded 0/8
// times, same as the historical baseline with no delay at all — no
// measurable benefit, and it costs real time on every one of the hundreds
// of attempts in a session. Removed as part of a speed pass; the underlying
// theory (old firmware's host-ACK step gave free settle time) may still be
// correct, but a fixed pre-read sleep isn't the right way to test it —
// worth revisiting with something that actually mimics the handshake
// exchange itself, not just a delay, if this is picked back up.

// Master off-switch for all the diagnostic-only additions from test_16/17/18
// (per-packet verbose logging in newproto_stream_body and the ROM header
// peek, plus the header peek's post-read overrun check). Each lprintf() call
// does a printf + fprintf + fflush to SD, and logging a line per 64-byte
// packet during the exact early-stream window under investigation could
// itself be changing the timing enough to affect what's being observed —
// set to 0 for a "barebones" hardware test that sends the same commands
// with none of this extra instrumentation, as close as reasonably possible
// to what Epilogue Playback's own captured traffic looks like. Flip back to
// 1 to restore full diagnostic visibility for a normal test session.
#define NEWPROTO_DIAGNOSTICS_ENABLED 0

// Logs the full contents of a packet (up to GBOP_PKT_SIZE bytes) as hex,
// instead of just the first few bytes. Requested after several sessions
// (test_10-14) where garbage/unexplained substitute packets kept showing up
// where a header or marker was expected — a 5-byte preview isn't enough to
// tell whether a given "unknown" value has any recognizable structure (e.g.
// matching a different exchange's marker, or looking like plausible mid-ROM
// data at a fixed offset). This is a software-level protocol log, not true
// bus-level sniffing — the Wii's IOS USB stack does not expose a raw/
// promiscuous capture API the way a PC's USB debug hardware or Linux's
// usbmon does, so nothing below the level `USB_ReadBlkMsg` already sees is
// observable from software running on the Wii itself. This is the practical
// software equivalent: dump everything that level *can* see, instead of a
// truncated preview of it.
static void log_full_packet(const char *label, const uint8_t *data, int len) {
    // Gated by the same barebones toggle. Previously still made 2 lprintf()
    // calls (each a printf + fprintf + fflush) even with diagnostics off —
    // a "(N bytes):" label and a bare newline, with nothing in between and
    // zero diagnostic content shown. That's pure overhead on every single
    // marker read in every session (speed pass, 2026-07-29) — skip entirely
    // when disabled instead of emitting an empty-handed pair of log lines.
    if (!NEWPROTO_DIAGNOSTICS_ENABLED) return;
    lprintf("[newproto] %s (%d bytes):", label, len);
    for (int i = 0; i < len; i++) lprintf(" %02X", data[i]);
    lprintf("\n");
}

// Reads one 512-byte marker block and checks it against the expected
// C0 DE <expect_seq> <cmd> <~cmd> shape.
//
// Returns 0 if it matched (marker block drained).
//
// Returns 1 if the first packet is a genuine 64-byte read that is NOT a
// C0DE marker but carries real (non-zero) content — the marker was dropped
// by the connection and this packet is the actual start of the stream
// (confirmed on hardware, Post Firmware Update Test/test_3: a genuine rd==64
// read landed real cart-info bytes where the header was expected). Its bytes
// are copied to *first_out (if non-NULL) so the caller can splice them back
// into the stream instead of losing them.
//
// Returns 2 if no marker was found AND there is nothing safely usable to
// recover: either no genuine 64-byte packet ever arrived (ZLP/short read),
// or the packet that arrived is ALL ZERO. All-zero is deliberately treated
// as a failure, not recoverable data: the marker block is
// `C0 DE <seq><cmd><~cmd>` followed by 507 zero bytes, so if only the
// marker's own first packet is dropped, every packet after it is
// indistinguishable zero padding for up to 7 more reads. Silently accepting
// that as real ROM/save data would inject corrupt zero bytes at the start
// of the caller's buffer — the same failure shape as the old-firmware
// drain[0] corruption bug. Neither GBA/GB ROM data nor save data is ever a
// full 64-byte run of zero at its start, so all-zero here is a reliable
// signal that this is marker padding, not data (confirmed on hardware,
// Post Firmware Update Test/test_8: gbop_read_rom_header_new accepted an
// all-zero substitute packet as "the ROM header," reading exactly 512 bytes
// of zero — the marker block's own size — instead of failing and retrying).
//
// Returns -1 on a USB error.
static int newproto_read_marker(GBOpDevice *dev, uint8_t cmd, uint8_t expect_seq,
                                 uint8_t *first_out) {
    uint8_t first[GBOP_PKT_SIZE] = {0};
    int got_first = 0;
    // A stray ZLP can land here (leftover from a previous exchange — IOS
    // does not flush EP IN on close/reopen, a recurring theme throughout
    // this project's history). Re-read once before giving up, same pattern
    // as gbop_bulk_recv's "ZLP before ACK — re-reading". Only a genuine
    // rd==64 read is ever treated as real content — a shorter read's buffer
    // is stale/undefined and must not be inspected (confirmed on hardware,
    // Post Firmware Update Test/test_2: a stale buffer from an earlier read
    // was previously being validated as if it were this call's response).
    for (int attempt = 0; attempt < 2 && !got_first; attempt++) {
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        lprintf("[newproto] marker(cmd=%02X seq=%d) pkt[0]%s rd=%d [0..4]=%02X %02X %02X %02X %02X\n",
                cmd, expect_seq, attempt ? " (retry after ZLP)" : "", (int)rd,
                s_rx[0], s_rx[1], s_rx[2], s_rx[3], s_rx[4]);
        if (rd == GBOP_PKT_SIZE) log_full_packet("marker pkt[0] full", s_rx, GBOP_PKT_SIZE);
        if (rd < 0) return -1;
        if (rd == 0 && attempt == 0) continue;  // retry once
        if (rd != GBOP_PKT_SIZE) return 2;      // short/second-ZLP — nothing usable
        memcpy(first, s_rx, GBOP_PKT_SIZE);
        got_first = 1;
    }
    if (!got_first) return 2;

    if (first[0] == 0xC0 && first[1] == 0xDE) {
        // A C0DE prefix alone does not mean this is OUR marker — IOS's
        // un-flushed EP IN queue can hand back a stale marker left over from
        // a completely different exchange (confirmed on hardware, Post
        // Firmware Update Test/test_10: a save-read's header check received
        // seq=01 cmd=04 — a residual CART-INFO FOOTER from an earlier,
        // unrelated poll — and the old code accepted it anyway since it only
        // checked the 2-byte prefix, logging a "mismatch" warning but still
        // proceeding to read save data as if that stale marker had been the
        // real thing). Require an exact seq/cmd/~cmd match before treating
        // this as a real, current-exchange marker.
        int matches = (first[2] == expect_seq && first[3] == cmd && first[4] == (uint8_t)(~cmd));
        if (!matches) {
            lprintf("[newproto] marker(cmd=%02X seq=%d): stale/foreign marker (got seq=%02X cmd=%02X ~cmd=%02X) "
                    "— draining and treating as failed read\n",
                    cmd, expect_seq, first[2], first[3], first[4]);
        }

        // Drain the rest of this marker block regardless of match — it's
        // definitely marker-shaped, not real stream data, so consuming it
        // here (rather than leaving it queued) reduces backlog for whatever
        // reads this device next, ours or not. Every clean header/footer
        // seen on hardware so far is full 64-byte packets until one
        // short/ZLP packet ends it — not necessarily always exactly 8, so
        // this stops on the natural short packet rather than assuming a
        // fixed count.
        // Per-packet logging here gated behind the diagnostics flag (speed
        // pass, 2026-07-29) — up to 6 extra lprintf()/fflush() calls per
        // marker match that almost always just confirm "yes, rd=64" with no
        // new information. A single summary line replaces them when
        // diagnostics are off, keeping the "how many packets drained" fact
        // without the per-packet cost.
        int drained = 0;
        for (int i = 1; i < NEWPROTO_MARKER_PACKETS; i++) {
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            if (NEWPROTO_DIAGNOSTICS_ENABLED)
                lprintf("[newproto] marker(cmd=%02X seq=%d) pkt[%d] rd=%d\n", cmd, expect_seq, i, (int)rd);
            if (rd < 0) return -1;
            drained++;
            if (rd < GBOP_PKT_SIZE) break;
        }
        if (!NEWPROTO_DIAGNOSTICS_ENABLED)
            lprintf("[newproto] marker(cmd=%02X seq=%d): drained %d padding packet(s)\n", cmd, expect_seq, drained);
        return matches ? 0 : 2;
    }

    int all_zero = 1;
    for (int i = 0; i < GBOP_PKT_SIZE; i++) if (first[i] != 0) { all_zero = 0; break; }
    if (all_zero) {
        lprintf("[newproto] marker(cmd=%02X seq=%d): missing header, packet is all-zero — "
                "ambiguous (likely marker padding), treating as failed read\n", cmd, expect_seq);
        return 2;
    }
    if (first_out) memcpy(first_out, first, GBOP_PKT_SIZE);
    return 1;
}

// True if pkt (a just-read full 64-byte packet) is the footer marker for
// cmd. Checks the whole packet, not just the C0 DE prefix, so a
// coincidental match inside real ROM/save data (odds ~1 in 65536 per
// packet on the 2-byte prefix alone) cannot false-positive: real data
// would also need to echo cmd, ~cmd, and 59 more zero bytes by chance.
static int newproto_is_footer(const uint8_t *pkt, uint8_t cmd) {
    if (pkt[0] != 0xC0 || pkt[1] != 0xDE || pkt[2] != 0x01 ||
        pkt[3] != cmd || pkt[4] != (uint8_t)(~cmd)) return 0;
    for (int i = 5; i < GBOP_PKT_SIZE; i++) if (pkt[i] != 0) return 0;
    return 1;
}

// Same idea as newproto_is_footer() but for the HEADER shape (seq=0x00).
// Added after Post Firmware Update Test/test_20: a ROM dump's very first
// byte was the literal header marker (C0 DE 00 00 FF 00...) instead of real
// data, permanently shifting every subsequent byte 64 bytes later than it
// should be. That specific failure shape is self-identifying — the full
// 5-byte-prefix-plus-59-zeros match makes a coincidental hit inside real
// ROM/save content astronomically unlikely, the same reasoning already
// relied on for footer detection — so newproto_stream_body() can recognize
// and drop a leaked header packet in real time, without needing a reference
// ROM. This does NOT address every corruption shape seen so far: test_16/18
// and test_20's *second* shift each showed the gap starting right after a
// genuinely-correct packet, with no marker bytes anywhere near the
// transition (confirmed directly against the dumped file for test_20) —
// those are silent, signature-less drops this check cannot see. See
// newproto_confirm_footer_immediate() below for how those get caught
// instead (not corrected, but no longer silently accepted as success).
static int newproto_is_header(const uint8_t *pkt, uint8_t cmd) {
    if (pkt[0] != 0xC0 || pkt[1] != 0xDE || pkt[2] != 0x00 ||
        pkt[3] != cmd || pkt[4] != (uint8_t)(~cmd)) return 0;
    for (int i = 5; i < GBOP_PKT_SIZE; i++) if (pkt[i] != 0) return 0;
    return 1;
}

// ---- Cart info (cmd 0x04) -------------------------------------------------
static int gbop_read_cart_info_new(GBOperatorHandle handle, CartInfo *out) {
    if (!handle || !out) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    memset(out, 0, sizeof(CartInfo));

    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x04 };
    if (gbop_bulk_send(dev, cmd) < 0) return GBOP_USB;

    // The header marker is not always present at all — on a lossy
    // connection it can be dropped outright, with the device's real data
    // arriving as the very first packet instead (confirmed on hardware,
    // Post Firmware Update Test/test_3, line 80: a genuine rd==64 read
    // landed real cart-info bytes where the header was expected, with no
    // header seen that exchange at all — not a stale-buffer artifact, a
    // real received packet). Treat the header as optional: read the first
    // packet, and if it's a C0DE header consume the rest of that marker
    // block and read a following packet as the data; if it's NOT a C0DE
    // header, treat THAT packet as the data directly. Either way, a
    // genuinely-all-zero data packet still correctly resolves to NOCART via
    // the resp[3:5]==0 check below, so this doesn't lose the "no cart"
    // case — it just stops assuming the header must be there to reach it.
    uint8_t resp[GBOP_PAYLOAD_SIZE];
    int got_resp = 0;
    {
        uint8_t first[GBOP_PKT_SIZE] = {0};
        int got_first = 0;
        for (int attempt = 0; attempt < 2 && !got_first; attempt++) {
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            lprintf("[newproto] cart-info pkt[0]%s rd=%d [0..4]=%02X %02X %02X %02X %02X\n",
                    attempt ? " (retry after ZLP)" : "", (int)rd,
                    s_rx[0], s_rx[1], s_rx[2], s_rx[3], s_rx[4]);
            if (rd == GBOP_PKT_SIZE) log_full_packet("cart-info pkt[0] full", s_rx, GBOP_PKT_SIZE);
            if (rd < 0) return GBOP_USB;
            if (rd == 0 && attempt == 0) continue;  // stray leftover ZLP — retry once
            if (rd != GBOP_PKT_SIZE) return GBOP_USB;  // short/second-ZLP — nothing usable
            memcpy(first, s_rx, GBOP_PKT_SIZE);
            got_first = 1;
        }
        if (!got_first) return GBOP_USB;

        if (first[0] == 0xC0 && first[1] == 0xDE && first[2] == 0x00 &&
            first[3] == 0x04 && first[4] == (uint8_t)(~0x04)) {
            // Real header — drain the rest of that marker block (not
            // necessarily a fixed count; stop on the natural short packet).
            for (int i = 1; i < NEWPROTO_MARKER_PACKETS; i++) {
                DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
                lprintf("[newproto] marker(cmd=04 seq=0) pkt[%d] rd=%d\n", i, (int)rd);
                if (rd < 0) return GBOP_USB;
                if (rd < GBOP_PKT_SIZE) break;
            }
        } else {
            // No header this exchange — this packet IS the data.
            memcpy(resp, first, GBOP_PAYLOAD_SIZE);
            got_resp = 1;
        }
    }

    if (!got_resp) {
        // Header was consumed above — now read the real data packet,
        // skipping any ZLP/short noise rather than trusting it blindly.
        for (int i = 0; i < NEWPROTO_MARKER_PACKETS && !got_resp; i++) {
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            lprintf("[newproto] cart-info data pkt[%d] rd=%d [0..4]=%02X %02X %02X %02X %02X\n",
                    i, (int)rd, s_rx[0], s_rx[1], s_rx[2], s_rx[3], s_rx[4]);
            if (rd < 0) return GBOP_USB;
            if (rd == GBOP_PKT_SIZE) {
                log_full_packet("cart-info data pkt full", s_rx, GBOP_PKT_SIZE);
                memcpy(resp, s_rx, GBOP_PAYLOAD_SIZE);
                got_resp = 1;
            }
        }
        if (!got_resp) {
            lprintf("[newproto] cart-info: no genuine data packet found\n");
            return GBOP_USB;
        }
    }

    // Sanity-check resp[] BEFORE attempting any further reads. Confirmed on
    // hardware (Post Firmware Update Test/test_5): a residual footer from a
    // PREVIOUS exchange (IOS does not flush EP IN across close/reopen — a
    // recurring theme in this project) can land as this exchange's "data"
    // packet (`C0 DE 01 04 FB...` — seq=01, a footer, not a seq=00 header,
    // so it fell through to the "treat as data" branch above). The old
    // ordering here ran a 32-iteration footer-drain loop on that garbage
    // BEFORE ever checking it was garbage — each iteration a further
    // blocking USB_ReadBlkMsg call on a connection that may have nothing
    // more to send, which is a real, confirmed hang, not a theoretical one.
    // Check first; only proceed to any further read once resp[] has passed
    // this check.
    memcpy(out->raw_resp, resp, GBOP_PAYLOAD_SIZE);

    if (resp[0] == 0xC0 && resp[1] == 0xDE) {
        lprintf("[newproto] C0 DE where cart-info data was expected — likely a residual "
                "footer from the previous exchange; treating as a failed read\n");
        return GBOP_USB;
    }

    // Best-effort footer drain for THIS exchange's real data, now that resp[]
    // has been confirmed to actually be data. Deliberately small (not the
    // 512-byte/8-packet block size used elsewhere) and tolerant of stopping
    // early — this is pure hygiene (keeps a stray trailing footer from
    // landing as the next exchange's first packet) and was never required
    // for correctness: the check above already handles a stray footer
    // showing up as the next exchange's data just fine. Not worth spending
    // many extra blocking reads on a connection already shown to sometimes
    // stop sending mid-exchange.
#define NEWPROTO_CARTINFO_FOOTER_CAP 4
    for (int i = 0; i < NEWPROTO_CARTINFO_FOOTER_CAP; i++) {
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd2 = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        lprintf("[newproto] cart-info footer-drain[%d] rd=%d\n", i, (int)rd2);
        if (rd2 < 0) break;
        if (rd2 == GBOP_PKT_SIZE && newproto_is_footer(s_rx, 0x04)) {
            lprintf("[newproto] cart-info footer found after %d padding packet(s)\n", i);
            break;
        }
    }
#undef NEWPROTO_CARTINFO_FOOTER_CAP

    if (resp[3] == 0x00 && resp[4] == 0x00) {
        lprintf("[gbop] Cart not detected (resp[3:5] == 0) — is cart inserted?\n");
        return GBOP_NOCART;
    }

    if (resp[2] == 0x20) {
        out->type = CART_TYPE_GB;
        strncpy(out->type_str, "GB", sizeof(out->type_str));
    } else {
        out->type = CART_TYPE_GBA;
        strncpy(out->type_str, "GBA", sizeof(out->type_str));
    }

    // No field here reliably encodes ROM/RAM size on this firmware (see
    // CLAUDE.md "resp[26..29] is a fixed constant" finding) — left at 0.
    // Resolved later (see enrich_info_from_buf in main.c) from rom_sizes.h
    // against the ROM header's game code.
    out->rom_size_kb = 0;
    out->ram_size_kb = 0;

    int title_len = 0;
    for (int i = 13; i < GBOP_PAYLOAD_SIZE && title_len < 16; i++) {
        char c = (char)resp[i];
        if (c < 0x20 || c > 0x7E) break;
        out->title[title_len++] = c;
    }
    out->title[title_len] = '\0';

    // Full 60-byte payload (not just the first 30) — widened 2026-07-31 to
    // capture the same raw data the short-lived "Cart Info Raw Dump" dev-menu
    // tool was built for, via this already-exercised, already-working path
    // (gbop_read_cart_info succeeds dozens of times per session) instead of a
    // separate reopen+capture flow. That tool was removed the same day after
    // proving hang-prone on hardware — see CLAUDE.md test_3 (log_commit_sd
    // hang) and its same-day follow-up (a raw, unbounded USB_ReadBlkMsg call
    // can itself block forever, which no in-loop abort check can rescue).
    lprintf("[gbop] (new-fw) resp[0..59] :");
    for (int i = 0; i < GBOP_PAYLOAD_SIZE; i++) lprintf(" %02X", resp[i]);
    lprintf("\n");
    lprintf("[gbop] (new-fw) type=%s title=\"%s\" (size resolved separately)\n",
            out->type_str, out->title);
    return 0;
}

int gbop_read_cart_info(GBOperatorHandle handle, CartInfo *out) {
    if (g_settings.use_old_firmware) return gbop_read_cart_info_legacy(handle, out);
    return gbop_read_cart_info_new(handle, out);
}

// ---- Fixed Nintendo logo constants ------------------------------------------
// Every licensed GB/GBC or GBA cartridge is hardware-required to carry these
// exact bytes (checked by the real console's own boot ROM before it will run
// anything). Two uses: (1) rom_reconcile.c's anchor validation, for captures
// too short to reach header_checksum_valid()'s full-header offsets; (2) a
// fast front-shift/gap detector inside gbop_dump_rom_new() itself (below) —
// a device-side quirk documented extensively in this project (test_16/18/20,
// Rom Stitching Test test_1/test_3) can deliver a perfectly valid marker
// handshake followed by real ROM content starting from the WRONG address
// (most commonly exactly 512 bytes in) — a stream shaped like that can never
// be useful no matter how long it runs, so checking the very first packet
// against this fixed, universal constant lets that be caught in one read
// instead of after a full ~40s dump.
//
// These bytes are NOT transcribed from memory: extracted directly (2026-07-29)
// from four independently-sourced GBA ROM files already in this repo (Pocket
// Monsters Emerald JP, Pokemon Emerald (J), Pocket Monsters Fire Red JP,
// Pokemon Ruby USA — logs/Firmware-V10_0_10-WireShark/) — all four produced
// byte-identical logo regions, and the CRC32 of this exact sequence
// (0xD0BEB55E) matches vendor/mgba's own LOGO_CRC32 constant (src/gba/core.c)
// exactly, which mGBA itself already relies on to decide whether to run a
// cartridge. The GB/GBC logo was extracted the same way from the Pokemon
// Silver reference file in the same folder, cross-checked by recomputing
// header_checksum_valid()'s own GB/GBC formula against that file's real,
// on-disk checksum byte (both match). See CLAUDE.md Sources (GBATEK
// "Nintendo Logo & Header Checksum", Pan Docs "Header Checksum") for the
// documented offsets/algorithm this cross-check was run against.
static const uint8_t GBA_LOGO[156] = {
    0x24,0xFF,0xAE,0x51,0x69,0x9A,0xA2,0x21,0x3D,0x84,0x82,0x0A,0x84,0xE4,0x09,0xAD,
    0x11,0x24,0x8B,0x98,0xC0,0x81,0x7F,0x21,0xA3,0x52,0xBE,0x19,0x93,0x09,0xCE,0x20,
    0x10,0x46,0x4A,0x4A,0xF8,0x27,0x31,0xEC,0x58,0xC7,0xE8,0x33,0x82,0xE3,0xCE,0xBF,
    0x85,0xF4,0xDF,0x94,0xCE,0x4B,0x09,0xC1,0x94,0x56,0x8A,0xC0,0x13,0x72,0xA7,0xFC,
    0x9F,0x84,0x4D,0x73,0xA3,0xCA,0x9A,0x61,0x58,0x97,0xA3,0x27,0xFC,0x03,0x98,0x76,
    0x23,0x1D,0xC7,0x61,0x03,0x04,0xAE,0x56,0xBF,0x38,0x84,0x00,0x40,0xA7,0x0E,0xFD,
    0xFF,0x52,0xFE,0x03,0x6F,0x95,0x30,0xF1,0x97,0xFB,0xC0,0x85,0x60,0xD6,0x80,0x25,
    0xA9,0x63,0xBE,0x03,0x01,0x4E,0x38,0xE2,0xF9,0xA2,0x34,0xFF,0xBB,0x3E,0x03,0x44,
    0x78,0x00,0x90,0xCB,0x88,0x11,0x3A,0x94,0x65,0xC0,0x7C,0x63,0x87,0xF0,0x3C,0xAF,
    0xD6,0x25,0xE4,0x8B,0x38,0x0A,0xAC,0x72,0x21,0xD4,0xF8,0x07,
};
static const uint8_t GB_LOGO[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
    0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
    0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E,
};

// Compares the fixed logo against `buf` (length `avail`) at the logo's fixed
// header offset for this cart type (GBA: 0x04; GB/GBC: 0x104), and returns
// the LONGEST matching prefix length — not an all-or-nothing full match.
// This matters: an attempt can be genuinely correct for its first N bytes
// and then diverge (an internal gap right past the entry vector, confirmed
// shape in Rom Stitching Test test_1/test_3's attempt_0/attempt_3-style
// captures) — requiring the entire available window to match would give
// that genuinely-good prefix zero credit just because of what comes after
// it. Every credited byte still has to match exactly; this only changes
// how far the comparison is allowed to stop.
uint32_t gbop_logo_prefix_match_len(CartType type, const uint8_t *buf, uint32_t avail) {
    const uint8_t *logo; uint32_t logo_len, offset;
    if (type == CART_TYPE_GBA) { logo = GBA_LOGO; logo_len = sizeof(GBA_LOGO); offset = 0x04; }
    else { logo = GB_LOGO; logo_len = sizeof(GB_LOGO); offset = 0x104; }

    if (avail <= offset) return 0;
    uint32_t check_len = avail - offset;
    if (check_len > logo_len) check_len = logo_len;
    uint32_t matched = 0;
    while (matched < check_len && buf[offset + matched] == logo[matched]) matched++;
    return matched;
}

// True if hdr looks like a genuine ROM header, checked against the same
// hardware-mandated checksum real GBA BIOS / DMG-CGB boot ROMs validate
// before they will boot the cartridge at all — a game-independent,
// well-defined structural signal, unlike raw non-zero content (test_10 and
// test_11 both confirmed a stale, repeatable non-zero garbage value can also
// look "non-zero", so that alone isn't a safe recoverability test). Tries
// both known header layouts and accepts either match, since this function
// serves both GBA and GB/GBC without knowing the cart type in advance; a
// random/stale 512-byte buffer satisfying either 8-bit checksum by chance is
// close to 1/256 per check, acceptably low for a value that is re-validated
// against rom_cache_exists()/cartindex on every use anyway.
int header_checksum_valid(const uint8_t *hdr) {
    // GBA: GBATEK "Nintendo Logo & Header Checksum" — complement of the sum
    // of bytes 0xA0-0xBC, biased by 0x19, stored at 0xBD.
    uint8_t gba_chk = 0;
    for (int i = 0xA0; i <= 0xBC; i++) gba_chk = (uint8_t)(gba_chk - hdr[i]);
    gba_chk = (uint8_t)(gba_chk - 0x19);
    if (gba_chk == hdr[0xBD]) return 1;

    // GB/GBC: Pan Docs "Header Checksum" — running subtraction across bytes
    // 0x134-0x14C, stored at 0x14D. This is the exact check the original
    // DMG boot ROM performs before it will run the game.
    uint8_t gb_chk = 0;
    for (int i = 0x134; i <= 0x14C; i++) gb_chk = (uint8_t)(gb_chk - hdr[i] - 1);
    if (gb_chk == hdr[0x14D]) return 1;

    return 0;
}

// Forward declaration — defined further down (shared streaming primitive
// used by the full dump path); gbop_read_rom_header_new() below now reuses
// it too (2026-07-31 modernization).
static int newproto_stream_body(GBOpDevice *dev, uint8_t cmd,
                                 uint8_t *buffer, uint32_t buffer_size);

// ---- ROM header mini-read (first 512 bytes, cmd 0x00) ---------------------
static int gbop_read_rom_header_new(GBOperatorHandle handle, uint8_t *hdr_out) {
    if (!handle || !hdr_out) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    const uint32_t probe_size = 512;

    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x00, 0x02 };
    cmd[2] = (uint8_t)(probe_size & 0xFF);
    cmd[3] = (uint8_t)((probe_size >> 8) & 0xFF);
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    // Header is optional (test_3) — a dropped header can be followed
    // immediately by genuinely fresh real data (confirmed repeatedly on
    // hardware, e.g. Post Firmware Update Test/test_12 line 847: a rejected
    // "recovered" packet `7F 00 00 EA 24...` was in fact the cart's real,
    // correct header — test_11's fix of refusing ALL non-zero recovery here
    // was too strong and threw away good reads, not just the stale-garbage
    // ones). What test_10/test_11 actually proved is that *raw non-zero
    // content alone* isn't a safe recoverability test, since a stale,
    // repeatable garbage value can look non-zero too. The fix is a real
    // validity check, not an outright ban: recover non-zero data as before,
    // but only trust the assembled 512 bytes if they pass
    // header_checksum_valid() below — the same hardware-mandated check a
    // real console applies before it will even boot the cartridge.
    uint32_t rx = 0;
    uint8_t recovered[GBOP_PKT_SIZE];
    int m = newproto_read_marker(dev, 0x00, 0x00, recovered);
    if (m < 0) return -1;
    if (m == 2) {
        lprintf("[newproto] hdr: header missing and unrecoverable — treating as failed read\n");
        return -1;
    }
    if (m == 1) {
        uint32_t copy = GBOP_PKT_SIZE;
        if (copy > probe_size) copy = probe_size;
        memcpy(hdr_out, recovered, copy);
        rx = copy;
    }

    // Modernized (2026-07-31, Rom Stitching Test/test_10): the rest of the
    // 512 bytes now goes through newproto_stream_body() — the same batched,
    // robust primitive gbop_dump_rom_continuation() already uses — instead
    // of the original one-64-byte-packet-per-USB_ReadBlkMsg-call loop this
    // function had used since before the 2026-07-28 streaming throughput
    // fix. That fix was applied to the dump path only; this header peek was
    // never brought along, so every one of its calls (used constantly
    // during cart detection, see read_rom_header_with_retry()) paid for up
    // to 8 individual IOS round trips instead of one batched read.
    if (rx < probe_size) {
        dev->np_active = 1;
        dev->np_footer_seen = 0;
        dev->np_cmd = 0x00;
        dev->np_total = probe_size;
        dev->np_given = rx;
        dev->np_stall_streak = 0;
        int rc = newproto_stream_body(dev, 0x00, hdr_out + rx, probe_size - rx);
        int footer_seen_early = dev->np_footer_seen;
        dev->np_active = 0;
        if (rc != 0) { lprintf("[newproto] hdr: stream read failed\n"); return -1; }
        // newproto_stream_body() zero-pads and reports success if the
        // device's footer arrives before the requested amount — correct,
        // benign behavior for a full dump against an unconfirmed/estimated
        // size (rom_sizes.h), but never legitimate here: every real ROM is
        // far larger than 512 bytes, so a footer this early always means
        // the stream is misaligned, not that the peek is "done". Missed
        // this when the read loop was modernized to use stream_body
        // (2026-07-31) — confirmed on hardware (Detect Cart Test/test_1):
        // a clean marker match followed by "footer seen at 64 bytes" was
        // silently accepted as a full, valid 512-byte header (zero-padded
        // from byte 64 on), producing a wrong title/code/size that then
        // got cached and reused via the same_cart shortcut on every
        // subsequent detect until something else changed the cart's
        // raw_resp bytes.
        if (footer_seen_early) {
            lprintf("[newproto] hdr: footer arrived early (given=%u of %u requested) — "
                    "stream misaligned, treating as failed read\n", dev->np_given, probe_size);
            return -1;
        }
        rx = probe_size;
    }
    if (m == 1 && !header_checksum_valid(hdr_out)) {
        lprintf("[newproto] hdr: recovered data failed header checksum — treating as failed read\n");
        return -1;
    }
    lprintf("[newproto] hdr Got %u bytes [0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
            rx, hdr_out[0], hdr_out[1], hdr_out[2], hdr_out[3],
            hdr_out[4], hdr_out[5], hdr_out[6], hdr_out[7]);

    // Diagnostic only (2026-07-27, following up a user hypothesis): this
    // function asks the device for only 512 bytes, then has always just
    // closed the handle without reading anything further — never checking
    // whether the device actually stops at 512 bytes, or keeps streaming the
    // rest of the ROM regardless, leaving that undrained tail sitting in the
    // connection. An abandoned tail like that is a strong candidate for
    // explaining why unrelated *later* exchanges keep seeing real-but-
    // misplaced cart data instead of a clean marker (test_15/16/17) — if the
    // device doesn't honour the 512-byte request, every header peek would
    // leave up to a whole ROM's worth of undrained data behind it. Reads a
    // bounded amount more (no new command sent) and logs it; does not affect
    // this function's return value, hdr_out, or the caller's behaviour.
    // Gated off entirely (not just silenced) for a barebones test — these
    // extra reads are real USB traffic the reference Wireshark captures
    // never show Epilogue performing, not just extra logging overhead.
    if (NEWPROTO_DIAGNOSTICS_ENABLED) {
        uint8_t extra[GBOP_PKT_SIZE];
        for (int i = 0; i < 16; i++) {
            DCInvalidateRange(extra, GBOP_PKT_SIZE);
            s32 rd2 = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, extra);
            lprintf("[newproto] hdr overrun-check[%d] rd=%d [0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                    i, (int)rd2, extra[0], extra[1], extra[2], extra[3], extra[4], extra[5], extra[6], extra[7]);
            if (rd2 < 0) break;
        }
    }

    // Deliberately do not read the footer marker — the device is still
    // mid-stream (we only asked for the first 512 bytes of a much longer
    // ROM). Caller closes the handle, matching the old-firmware contract.
    return (rx >= probe_size) ? 0 : -1;
}

int gbop_read_rom_header(GBOperatorHandle handle, uint8_t *hdr_out) {
    if (g_settings.use_old_firmware) return gbop_read_rom_header_legacy(handle, hdr_out);
    return gbop_read_rom_header_new(handle, hdr_out);
}

// ---- Shared streaming read body for ROM (cmd 0x00) and save (cmd 0x02) ---
// Streams bytes into buffer until either buffer_size bytes are written or
// the device's footer marker is seen. No host ACKs are sent during this
// phase on this firmware. Persists across calls via dev->np_* so a
// caller's fixed-size-chunk loop (rom_cache.c) can call this repeatedly,
// same contract as the old gbop_dump_rom/gbop_read_save.
// Verbose per-read logging for the first stretch of a fresh stream only
// (gated on dev->np_given, so a 16MB dump doesn't get 16MB of log). Added
// after Post Firmware Update Test/test_16 found a precise, reproducible
// 448-byte (7-packet) gap between the first and second real data packets of
// an otherwise byte-perfect ROM dump — confirmed by direct comparison
// against the known-good Wireshark reference (rom_dump_GBA.pcapng): packet 0
// matched exactly, and every packet from index 1 onward matched the
// reference shifted forward by exactly 448 bytes, consistently for the rest
// of the 16MB file. This function has no code path that discards a
// successfully-read packet without writing it (verified by inspection), so
// the loss must be either genuinely on the wire/in IOS, or something not yet
// understood — this logging is the direct way to see it, since neither the
// marker-drain phase's nor this function's reads were ever logged
// packet-by-packet with content before now.
#define NEWPROTO_EARLY_LOG_BYTES 4096

// Read buffer for newproto_stream_body(). This used to be a single 64-byte
// packet per USB_ReadBlkMsg() call — correct, but slow: a full 16MB ROM dump
// is 262,144 individual IOS round trips. Wireshark timing analysis (this
// project's own captures) measured Wii-side USB_ReadBlkMsg latency as
// bimodal ~1ms/~11ms per call, versus Epilogue's ~600-700us on Windows — and
// crucially, Windows's own captures show 512-byte "transfers" per USB event,
// not 64-byte ones, meaning its stack is already coalescing multiple packets
// per call while ours wasn't. Measured hardware timing (Post Firmware Update
// Test/test_24's own progress log) confirms this is the dominant cost, not
// retries: ~9.3s per 512KB once a dump is actually streaming, ~16x slower
// than the same 512KB in the Wireshark reference capture. Requesting a
// bigger buffer per call lets IOS do the packet-coalescing internally
// instead of paying one round-trip per single 64-byte packet.
#define NEWPROTO_READ_CHUNK 4096
static uint8_t s_stream_rx[NEWPROTO_READ_CHUNK] ATTRIBUTE_ALIGN(32);

static int newproto_stream_body(GBOpDevice *dev, uint8_t cmd,
                                 uint8_t *buffer, uint32_t buffer_size) {
    uint32_t written = 0;
    while (written < buffer_size) {
        if (dev->np_footer_seen) {
            // Real stream already ended (our size estimate was too big) —
            // pad the rest of the caller's buffer with zeros rather than
            // touching USB again. Safe direction to be wrong in; see
            // rom_sizes.h and the "Do Not: Refactor rom_cache.c" entry.
            uint32_t pad = buffer_size - written;
            memset(buffer + written, 0, pad);
            written += pad;
            break;
        }

        // Never request more than the caller's remaining space, so `rd` can
        // never exceed what's left in `buffer` — no overflow bookkeeping
        // needed below regardless of how much of this read gets dropped as
        // a marker rather than counted as data.
        uint32_t want = buffer_size - written;
        if (want > NEWPROTO_READ_CHUNK) want = NEWPROTO_READ_CHUNK;

        int verbose = NEWPROTO_DIAGNOSTICS_ENABLED && dev->np_given < NEWPROTO_EARLY_LOG_BYTES;
        DCInvalidateRange(s_stream_rx, NEWPROTO_READ_CHUNK);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, want, s_stream_rx);
        if (verbose) {
            lprintf("[newproto] stream(cmd=%02X) early read at given=%u want=%u rd=%d [0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                    cmd, dev->np_given, want, (int)rd,
                    s_stream_rx[0], s_stream_rx[1], s_stream_rx[2], s_stream_rx[3],
                    s_stream_rx[4], s_stream_rx[5], s_stream_rx[6], s_stream_rx[7]);
        }
        if (rd < 0) {
            lprintf("[newproto] stream(cmd=%02X) read error: %d at %u/%u bytes\n",
                    cmd, (int)rd, dev->np_given, dev->np_total);
            dev->np_active = 0;
            return -1;
        }
        if (rd <= 0) {
            dev->np_stall_streak++;
            if (dev->np_stall_streak > NEWPROTO_MAX_STALL) {
                lprintf("[newproto] stream(cmd=%02X) dead: %u reads with no progress — aborting\n",
                        cmd, dev->np_stall_streak);
                dev->np_active = 0;
                return -1;
            }
            continue;
        }
        dev->np_stall_streak = 0;

        // A batched read can contain multiple 64-byte packets at once now,
        // so a marker (always exactly one packet) has to be looked for at
        // every packet-aligned position within what came back, not just
        // checked once against the whole read the way a single-packet read
        // could. A short tail smaller than one full packet can't contain a
        // marker (same as the old rd != GBOP_PKT_SIZE check) and is just
        // copied through as data.
        uint32_t pos = 0;
        uint32_t rd_u = (uint32_t)rd;
        while (pos < rd_u && written < buffer_size && !dev->np_footer_seen) {
            uint32_t remain = rd_u - pos;
            if (remain >= GBOP_PKT_SIZE) {
                uint8_t *pkt = s_stream_rx + pos;
                if (newproto_is_footer(pkt, cmd)) {
                    dev->np_footer_seen = 1;
                    lprintf("[newproto] stream(cmd=%02X) footer seen at %u bytes\n", cmd, dev->np_given);
                    pos += GBOP_PKT_SIZE;
                    continue;
                }
                if (newproto_is_header(pkt, cmd)) {
                    // A header marker leaked into the data stream instead of
                    // being fully consumed before streaming began (see
                    // newproto_is_header() comment). Definitely not real
                    // data — drop it rather than writing it into the
                    // caller's buffer, so the file doesn't end up
                    // permanently shifted by whatever this packet would
                    // otherwise have displaced.
                    lprintf("[newproto] stream(cmd=%02X) header-shaped packet mid-stream at given=%u — dropping\n",
                            cmd, dev->np_given);
                    pos += GBOP_PKT_SIZE;
                    continue;
                }
                memcpy(buffer + written, pkt, GBOP_PKT_SIZE);
                written += GBOP_PKT_SIZE;
                dev->np_given += GBOP_PKT_SIZE;
                pos += GBOP_PKT_SIZE;
            } else {
                memcpy(buffer + written, s_stream_rx + pos, remain);
                written += remain;
                dev->np_given += remain;
                pos += remain;
            }
        }
        if (dev->np_footer_seen) continue;  // re-enter loop: pad-fill branch above handles the rest
    }
    return 0;
}

// Checked immediately after every newproto_stream_body() call, regardless of
// whether np_given has reached np_total. Catches the EARLY-footer case that
// newproto_confirm_footer_immediate() structurally cannot: once stream_body
// sees the footer, it stops advancing np_given forever (its pad-fill branch
// only touches the per-call `written` counter, filling the caller's buffer
// with zeros so the outer chunked-read loop in rom_cache.c thinks each
// remaining chunk completed normally) — so if the footer arrives before
// np_given reaches np_total, np_given can NEVER subsequently reach np_total,
// meaning the `np_given >= np_total` gate that guards the late-footer check
// is never satisfied and that check never runs at all. Confirmed on
// hardware (Post Firmware Update Test/test_25): an Emerald ROM dump lost its
// real first 512 bytes entirely (device sent the footer 512 bytes early);
// the dump still reported "[OK] Saved" with the tail zero-padded, because
// nothing upstream of this ever checked for an early footer specifically.
//
// An early footer is NOT always wrong, though: it's the expected, correct
// outcome when the caller's total was only a generous, deliberately
// oversized guess for a ROM/save not in rom_sizes.c's table (see
// rom_sizes.h) — the device's real, smaller ROM legitimately ends before
// the guess. `size_confirmed` distinguishes the two: only reject when the
// caller had an exact, confirmed size (a real rom_sizes.c table hit, or a
// cart-reported save size) and the device still fell short of it.
static int newproto_check_early_footer(GBOpDevice *dev, uint8_t cmd, int size_confirmed) {
    if (!dev->np_footer_seen || dev->np_given >= dev->np_total) return 0; // not early
    if (!size_confirmed) {
        lprintf("[newproto] stream(cmd=%02X) footer early at %u/%u bytes — expected for an "
                "unresolved/estimated size, accepting\n", cmd, dev->np_given, dev->np_total);
        return 0;
    }
    lprintf("[newproto] stream(cmd=%02X) footer arrived EARLY: only %u of %u bytes received "
            "(%u bytes missing) on a CONFIRMED exact size — stream was misaligned, rejecting\n",
            cmd, dev->np_given, dev->np_total, dev->np_total - dev->np_given);
    return -1;
}

#define NEWPROTO_FOOTER_CONFIRM_MAX 64

// Called once a caller believes it has collected the full expected byte
// count (np_given >= np_total) but np_footer_seen is still false. This gap
// exists because neither gbop_dump_rom_new() nor gbop_read_save_new() ever
// naturally attempts a read past exactly buffer_size bytes — so on a
// perfectly healthy stream nothing has ever actually confirmed the real
// footer shows up where expected; on a corrupted one (any of the shift
// shapes found on hardware: Post Firmware Update Test/test_16, test_18,
// test_20's both shifts), the device's own footer is tied to its true
// internal byte count and doesn't move, so it arrives LATER than our
// nominal completion point by exactly however many bytes were lost or
// displaced upstream, regardless of whether the cause left a signature
// behind or not. This can't recover or locate the missing/misplaced bytes
// (no reference ROM available in the field) but it reliably tells the
// caller the dump is wrong instead of silently accepting "byte count
// reached, footer eventually seen" as success, which is what let three
// separate corrupted-but-reported-successful dumps through before this.
// Only ever invoked when np_given has JUST reached np_total for an exact,
// resolved size (a known game's correct rom_sizes.c entry, or a save size
// from cart-info) — never for the intentionally-oversized unrecognized-game
// ROM fallback, since np_given never reaches that inflated total and the
// footer is instead caught inline, well before this function would run.
static int newproto_confirm_footer_immediate(GBOpDevice *dev, uint8_t cmd) {
    if (dev->np_footer_seen) return 0;  // already seen inline during streaming — clean
    int real_extra = 0; // count only genuine non-ZLP, non-footer packets — see below
    for (int i = 0; i < NEWPROTO_FOOTER_CONFIRM_MAX; i++) {
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        if (rd < 0) {
            lprintf("[newproto] stream(cmd=%02X) footer-confirm: read error %d after %d extra packet(s)\n",
                    cmd, (int)rd, real_extra);
            return -1;
        }
        if (rd == 0) {
            // A ZLP right before the real footer is a benign, already-
            // established USB artifact in this protocol — the same thing
            // routinely shows up (and is already tolerated without
            // complaint) in the cart-info marker/footer drain elsewhere in
            // gb_operator.c. Confirmed on hardware (Post Firmware Update
            // Test/test_28): two "footer arrived 64 bytes LATE" rejections
            // both turned out to be exactly this — rd=0 immediately
            // followed by the real footer — and the preserved partial dump
            // from one of them was byte-for-byte identical to the reference
            // ROM with zero shift anywhere, proving the stream itself was
            // completely clean. Don't count a ZLP as evidence of
            // misalignment; only genuine unexpected data should reject.
            lprintf("[newproto] stream(cmd=%02X) footer-confirm: ZLP (benign) before footer, continuing\n", cmd);
            continue;
        }
        if (rd == GBOP_PKT_SIZE && newproto_is_footer(s_rx, cmd)) {
            dev->np_footer_seen = 1;
            if (real_extra == 0) {
                lprintf("[newproto] stream(cmd=%02X) footer confirmed — clean\n", cmd);
                return 0;
            }
            lprintf("[newproto] stream(cmd=%02X) footer arrived %d bytes LATE (%d extra real packet(s)) "
                    "— stream was misaligned, rejecting dump\n", cmd, real_extra * GBOP_PKT_SIZE, real_extra);
            return -1;
        }
        // Genuine non-footer, non-ZLP content — this is real, unexpected
        // data and IS still proof of misalignment. Full content logged (not
        // just "mismatch") so a rejection leaves something to actually
        // compare against a reference capture, not just a byte count — see
        // the "log the actual bytes" diagnostic added alongside
        // rom_cache.c's rejected-dump preservation (2026-07-28).
        real_extra++;
        if (rd == GBOP_PKT_SIZE) {
            char label[48];
            snprintf(label, sizeof(label), "footer-confirm extra pkt[%d]", real_extra - 1);
            log_full_packet(label, s_rx, GBOP_PKT_SIZE);
        } else {
            lprintf("[newproto] stream(cmd=%02X) footer-confirm extra pkt[%d]: rd=%d (not a full packet)\n",
                    cmd, real_extra - 1, (int)rd);
        }
    }
    lprintf("[newproto] stream(cmd=%02X) footer-confirm: none within %d extra packets — rejecting dump\n",
            cmd, NEWPROTO_FOOTER_CONFIRM_MAX);
    return -1;
}

// ---- ROM dump (cmd 0x00) ---------------------------------------------------
static int gbop_dump_rom_new(GBOperatorHandle handle, const CartInfo *info,
                              uint8_t *buffer, uint32_t buffer_size) {
    if (!handle || !info || !buffer || buffer_size == 0) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    uint32_t total = info->rom_size_kb * 1024;
    if (total == 0) return -1;

    // Adjusted below (skipping the already-consumed first packet) only when
    // the fast front-check reads and validates it; otherwise stream_body
    // runs against the caller's original buffer/buffer_size unchanged, same
    // as before this check existed.
    uint8_t *stream_buf = buffer;
    uint32_t stream_size = buffer_size;

    if (!dev->np_active) {
        uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x00, 0x02 };
        cmd[2] = (uint8_t)((total >>  0) & 0xFF);
        cmd[3] = (uint8_t)((total >>  8) & 0xFF);
        cmd[4] = (uint8_t)((total >> 16) & 0xFF);
        cmd[5] = (uint8_t)((total >> 24) & 0xFF);
        lprintf("[newproto] ROM dump start: requesting %u bytes (%u KB)\n", total, total / 1024);
        if (gbop_bulk_send(dev, cmd) < 0) return -1;

        // Strict marker match required — NOT the checksum-gated recovery
        // path the ROM header peek uses. Tried that here in test_13; test_17
        // caught it accepting outright wrong data: a recovered "header"
        // packet that was really Thumb code from some unrelated ROM offset
        // (not a stale/repeatable value — genuinely different content each
        // time, consistent with the drifting-pointer theory from test_15)
        // passed the GB/GBC header_checksum_valid() check by pure chance,
        // producing a complete, full-size, entirely-wrong 16MB "dump" that
        // reported success. An 8-bit checksum has a real, non-negligible
        // false-accept rate (~1/128 combining both GBA/GB formats) — cheap
        // insurance for the header peek's low-stakes wrong-metadata case
        // (self-correcting on the next detect), but nowhere near strong
        // enough odds to gate an entire ROM's worth of data being reported
        // as successfully dumped, especially checked repeatedly across many
        // retries in one session (11 attempts here — a false accept was
        // close to even odds over that many tries). A missed recovery here
        // just costs one more retry (already budgeted generously via
        // dump_rom_with_retry in main.c); a false accept costs a silently
        // wrong file.
        int m = newproto_read_marker(dev, 0x00, 0x00, NULL);
        if (m != 0) {
            lprintf("[newproto] ROM dump: header marker mismatch/error (m=%d) — aborting\n", m);
            return -1;
        }

        dev->np_active = 1;
        dev->np_footer_seen = 0;
        dev->np_cmd = 0x00;
        dev->np_total = total;
        dev->np_given = 0;
        dev->np_stall_streak = 0;

        // Fast front-shift/gap check (GBA only — the logo lands entirely
        // within this very first 64-byte data packet, right after the
        // 4-byte entry vector; GB/GBC's logo doesn't start until byte
        // 0x104, well past packet 0, so this can't apply there). Read just
        // the first packet and check it now, before committing to a full
        // stream that can never be useful if this fails — see
        // gbop_logo_prefix_match_len()'s comment for why this shape
        // recurs and is worth catching this early.
        if (info->type == CART_TYPE_GBA && buffer_size >= GBOP_PKT_SIZE) {
            DCInvalidateRange(s_stream_rx, GBOP_PKT_SIZE);
            s32 rd0 = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_stream_rx);
            if (rd0 == GBOP_PKT_SIZE && !newproto_is_footer(s_stream_rx, 0x00) &&
                !newproto_is_header(s_stream_rx, 0x00)) {
                uint32_t matched = gbop_logo_prefix_match_len(CART_TYPE_GBA, s_stream_rx, GBOP_PKT_SIZE);
                if (matched < GBOP_LOGO_MIN_BYTES) {
                    lprintf("[newproto] ROM dump: first packet doesn't match known ROM logo "
                            "(matched=%u bytes) — front-shift/gap detected, aborting early\n", matched);
                    dev->np_active = 0;
                    return -1;
                }
                lprintf("[newproto] ROM dump: front check passed (%u logo bytes matched)\n", matched);
                memcpy(buffer, s_stream_rx, GBOP_PKT_SIZE);
                dev->np_given = GBOP_PKT_SIZE;
                stream_buf = buffer + GBOP_PKT_SIZE;
                stream_size = buffer_size - GBOP_PKT_SIZE;
            } else {
                // rd0 <= 0, or the very first packet looked footer/header-
                // shaped rather than data-shaped — genuinely abnormal for a
                // fresh stream's first packet. This branch previously had no
                // logging at all, which made a skipped front-check
                // indistinguishable from a check that never ran (Rom
                // Stitching Test/test_4: reconcile captures showed no front-
                // check log line at all despite a confirmed clean marker
                // match). Logged now so that's diagnosable instead of
                // guessed at. stream_buf/stream_size stay at their defaults
                // (the full original buffer) — falls through to the normal
                // streaming path, which has its own stall/ZLP handling as a
                // more general safety net; this early check is a fast-path
                // optimization, not the only guard.
                lprintf("[newproto] ROM dump: front check skipped — first read after marker was "
                        "rd=%d (not a clean %u-byte data packet), falling through to normal stream\n",
                        (int)rd0, (unsigned)GBOP_PKT_SIZE);
            }
        } else if (info->type == CART_TYPE_GBA) {
            lprintf("[newproto] ROM dump: front check skipped — buffer_size=%u < %u\n",
                    buffer_size, (unsigned)GBOP_PKT_SIZE);
        }
    }

    int rc = newproto_stream_body(dev, 0x00, stream_buf, stream_size);
    if (rc != 0) return rc;

    if (newproto_check_early_footer(dev, 0x00, info->rom_size_confirmed) != 0) {
        dev->np_active = 0;
        return -1;
    }

    uint32_t prev_mark = (dev->np_given > buffer_size) ? (dev->np_given - buffer_size) / NEWPROTO_LOG_INTERVAL : 0;
    uint32_t curr_mark = dev->np_given / NEWPROTO_LOG_INTERVAL;
    if (curr_mark > prev_mark || dev->np_footer_seen) {
        lprintf("[gbop] ROM dump: %u KB / %u KB (estimate)\n", dev->np_given / 1024, dev->np_total / 1024);
        if (g_log) fflush(g_log);
    }

    if (dev->np_given >= dev->np_total) {
        if (newproto_confirm_footer_immediate(dev, 0x00) != 0) {
            dev->np_active = 0;
            return -1;
        }
        lprintf("[newproto] ROM dump complete: given=%u total(est)=%u\n", dev->np_given, dev->np_total);
        dev->np_active = 0;
    }
    return 0;
}

int gbop_dump_rom(GBOperatorHandle handle, const CartInfo *info,
                  uint8_t *buffer, uint32_t buffer_size) {
    if (g_settings.use_old_firmware) return gbop_dump_rom_legacy(handle, info, buffer, buffer_size);
    return gbop_dump_rom_new(handle, info, buffer, buffer_size);
}

// ---- Continuation dump — now the primary new-firmware dump mechanism ------
// (originally a 2026-07-30 experiment testing a user hypothesis; confirmed
// on hardware across 9 successful dumps, 4 different GBA carts, Rom
// Stitching Test/test_8-9 — promoted to the default new-firmware dump path.
// See CLAUDE.md for the full history.)
//
// gbop_dump_rom_new()'s front-check aborts (dev->np_active=0, close+reopen
// required) the instant a bad front is detected. This does something
// different: instead of tearing the connection down, it drains the current
// (bad) stream to its own natural footer, then sends a FRESH dump command
// on the SAME still-open handle — no close, no reopen — and checks its
// front too, repeating up to max_cycles. Confirmed hardware data: 8 of 9
// successful dumps needed only 1 cycle (marker success = front success,
// immediately); the drain-and-retry path fired only once and worked when
// it did.
//
// GBA and GB/GBC (GB/GBC added when this was promoted from experiment to
// the default path — see gbop_logo_prefix_match_len() for why GB/GBC needs
// a larger front probe than GBA does: the logo starts at ROM offset 0x104,
// well past the first 64-byte packet). buffer must be exactly
// info->rom_size_kb*1024 bytes (the whole ROM in one shot, not chunked —
// this function owns the entire capture, unlike gbop_dump_rom() which is
// called once per chunk by rom_cache_stream_chunks()).
//
// The front probe is read via newproto_stream_body() (the same primitive
// the main capture body uses) rather than a single raw USB_ReadBlkMsg call
// — this was a hand-rolled 3-try ZLP-tolerance loop in the original
// experiment; stream_body's own existing stall/ZLP handling is more
// thorough (NEWPROTO_MAX_STALL, not a fixed 3) and this also naturally
// enables a probe size larger than one packet, which GB/GBC's front check
// needs and GBA's didn't.
//
// *out_cycles_used (optional) is set to how many cycles were attempted
// (1 = first try passed, no draining needed). Returns 0 on a full,
// footer-confirmed success; -1 on a hard USB failure or once max_cycles is
// exhausted without ever passing the front check.
static uint8_t s_continuation_scratch[NEWPROTO_READ_CHUNK];

// Chunk size the main transfer is broken into purely so byte_progress_cb
// gets called periodically during a single cycle's streaming phase — 512KB
// matches the interval the (unused-by-this-function) legacy per-chunk dump
// path already logged progress at, giving ~32 updates for a 16MB ROM, a
// reasonable redraw rate for an on-screen progress bar without excessive
// screen writes. Purely a callback-timing granularity; does not change what
// gets validated or how (the front-check/footer-confirm logic is unaffected
// — the same total byte count streams either way, just via N smaller
// newproto_stream_body() calls instead of one big one).
#define GBOP_DUMP_PROGRESS_CHUNK (512 * 1024)

int gbop_dump_rom_continuation(GBOperatorHandle handle, const CartInfo *info,
                                uint8_t *buffer, uint32_t buffer_size,
                                int max_cycles, int *out_cycles_used,
                                GbopProgressCB cycle_cb, void *cycle_ctx,
                                GbopByteProgressCB byte_progress_cb, void *byte_progress_ctx) {
    if (!handle || !info || !buffer || buffer_size == 0) return -1;
    if (info->type != CART_TYPE_GBA && info->type != CART_TYPE_GBC && info->type != CART_TYPE_GB)
        return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    uint32_t total = info->rom_size_kb * 1024;
    if (total == 0 || buffer_size < total) return -1;

    // GBA: the logo lands entirely within packet 0 (offset 0x04-0x9F, well
    // inside the first 64 bytes). GB/GBC: the logo starts at offset 0x104
    // (260) — packet 0 alone can't reach it, so the probe needs to cover
    // through at least 0x134 (the end of the 48-byte GB/GBC logo). 320
    // bytes (5 packets) covers that with margin and stays packet-aligned.
    uint32_t probe_size = (info->type == CART_TYPE_GBA) ? GBOP_PKT_SIZE : 320;
    if (probe_size > buffer_size) return -1;

    for (int cycle = 1; cycle <= max_cycles; cycle++) {
        if (out_cycles_used) *out_cycles_used = cycle;
        // Fires on every cycle ATTEMPT, success or failure — unlike
        // byte_progress_cb below, which only ever fires once a cycle has
        // already passed its marker+front checks and started real
        // streaming. Given most cycles across a session fail at the
        // marker/front-check stage (see the write-stall analysis a few
        // turns earlier), byte_progress_cb alone left the screen showing
        // nothing at all for however long that took — indistinguishable
        // from a hang. Added 2026-07-31 after direct user feedback: "the
        // progressing dots should be before the progress bar appears so
        // the player knows that attempts are being made."
        if (cycle_cb) cycle_cb(cycle_ctx);

        uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x00, 0x02 };
        cmd[2] = (uint8_t)((total >>  0) & 0xFF);
        cmd[3] = (uint8_t)((total >>  8) & 0xFF);
        cmd[4] = (uint8_t)((total >> 16) & 0xFF);
        cmd[5] = (uint8_t)((total >> 24) & 0xFF);
        lprintf("[continuation] cycle %d/%d: requesting %u bytes (same handle, no reopen)\n",
                cycle, max_cycles, total);
        if (gbop_bulk_send(dev, cmd) < 0) {
            lprintf("[continuation] cycle %d: command send failed — hard USB failure\n", cycle);
            return -1;
        }

        int m = newproto_read_marker(dev, 0x00, 0x00, NULL);
        if (m != 0) {
            lprintf("[continuation] cycle %d: header marker mismatch/error (m=%d) — this is the "
                    "ordinary handshake-failure case, aborting (caller retries with a fresh handle)\n",
                    cycle, m);
            return -1;
        }

        dev->np_active = 1;
        dev->np_footer_seen = 0;
        dev->np_cmd = 0x00;
        dev->np_total = total;
        dev->np_given = 0;
        dev->np_stall_streak = 0;

        int probe_rc = newproto_stream_body(dev, 0x00, buffer, probe_size);
        if (probe_rc != 0) { dev->np_active = 0; return -1; }

        int front_ok = 0;
        uint32_t matched = 0;
        if (!dev->np_footer_seen) {
            CartType logo_type = (info->type == CART_TYPE_GBA) ? CART_TYPE_GBA : CART_TYPE_GBC;
            matched = gbop_logo_prefix_match_len(logo_type, buffer, probe_size);
            front_ok = (matched >= GBOP_LOGO_MIN_BYTES);
        }

        if (front_ok) {
            lprintf("[continuation] cycle %d: front check PASSED (%u logo bytes matched) — "
                    "capturing full ROM\n", cycle, matched);
            uint32_t off = probe_size;
            while (off < total) {
                uint32_t remain = total - off;
                uint32_t chunk = (remain < GBOP_DUMP_PROGRESS_CHUNK) ? remain : GBOP_DUMP_PROGRESS_CHUNK;
                int rc = newproto_stream_body(dev, 0x00, buffer + off, chunk);
                if (rc != 0) { dev->np_active = 0; return -1; }
                off += chunk;
                if (byte_progress_cb) byte_progress_cb(dev->np_given, dev->np_total, byte_progress_ctx);
                if (dev->np_footer_seen) break; // stream already ended (unconfirmed-size estimate) — rest is zero-padded
            }
            if (newproto_check_early_footer(dev, 0x00, info->rom_size_confirmed) != 0) {
                dev->np_active = 0;
                return -1;
            }
            if (newproto_confirm_footer_immediate(dev, 0x00) != 0) {
                dev->np_active = 0;
                return -1;
            }
            lprintf("[continuation] cycle %d: ROM dump complete, given=%u — SUCCESS after %d cycle(s)\n",
                    cycle, dev->np_given, cycle);
            dev->np_active = 0;
            return 0;
        }

        // Bad front (or the stream ended within the probe itself) — drain
        // whatever's left to the footer, then loop and try again on this
        // SAME handle (no close/reopen). This is the actual mechanism.
        lprintf("[continuation] cycle %d: front check FAILED (matched=%u, footer_in_probe=%d) — "
                "draining to footer, staying on same handle\n", cycle, matched, dev->np_footer_seen);
        int drain_failed = 0;
        while (!dev->np_footer_seen) {
            int rc = newproto_stream_body(dev, 0x00, s_continuation_scratch, sizeof(s_continuation_scratch));
            if (rc != 0) { drain_failed = 1; break; }
        }
        dev->np_active = 0;
        if (drain_failed) {
            lprintf("[continuation] cycle %d: drain itself failed mid-stream — hard USB failure, "
                    "aborting (caller should close/reopen)\n", cycle);
            return -1;
        }
        lprintf("[continuation] cycle %d: drained cleanly to footer at given=%u\n", cycle, dev->np_given);
    }

    lprintf("[continuation] exhausted %d cycles without ever passing the front check — giving up\n",
            max_cycles);
    return -1;
}

// ---- ROM header mini-read, continuation-style (cmd 0x00, 512 bytes) -------
// Same mechanism as gbop_dump_rom_continuation() above (same-handle drain-
// and-retry instead of close/reopen per attempt), applied to the 512-byte
// header peek used by cart detection — added 2026-07-31 after confirming
// this was the one part of the new-firmware dump path that never got the
// continuation treatment despite sharing the identical protocol shape.
//
// The declared "total" in a ROM-read command is what the device streams
// before emitting its own real footer (this is the whole basis for the full
// dump's continuation mechanism, and independently for
// gbop_verify_gba_rom_size()'s partial-size mirror check) — so requesting
// exactly 512 bytes gets a genuine, device-emitted footer right at byte 512,
// letting the same drain-to-footer retry apply here too, instead of
// read_rom_header_with_retry()'s old close+reopen-every-attempt loop.
//
// This also fixes a real, evidence-backed self-inflicted contamination bug:
// gbop_read_rom_header_new() (the old per-attempt implementation) never
// drains its own footer before the caller closes the handle — "the device is
// still mid-stream ... caller closes the handle" was always true, but IOS
// does not flush EP IN across close/reopen, so that undrained footer sits in
// the queue and lands as a "stale/foreign marker" on the very next attempt.
// Confirmed directly in Detect Cart Test/test_3's log: a `stale/foreign
// marker (got seq=01 cmd=00 ~cmd=FF)` is exactly a leftover header-peek
// footer contaminating the next reopened attempt. Because every retry cycle
// here properly drains to the real footer before resending (or before
// returning success, matching gbop_read_rom_header_new()'s existing
// contract), this class of contamination can't recur between cycles.
//
// type: the cart type already known from a successful cart-info read (used
// to pick the GBA vs GB/GBC front-check probe size — see
// gbop_dump_rom_continuation() for why GB/GBC needs a larger probe).
// hdr_out must be 512 bytes. *out_cycles_used (optional) reports how many
// cycles were needed. Returns 0 on a validated (logo + header checksum) 512-
// byte header, -1 otherwise. Always leaves the device mid-stream on success
// (does not drain the final footer, same as the old implementation) — the
// caller must close the handle before any further command, same contract as
// before.
int gbop_read_rom_header_continuation(GBOperatorHandle handle, CartType type,
                                       uint8_t *hdr_out, int max_cycles,
                                       int *out_cycles_used,
                                       GbopProgressCB progress_cb, void *progress_ctx) {
    if (!handle || !hdr_out) return -1;
    if (type != CART_TYPE_GBA && type != CART_TYPE_GBC && type != CART_TYPE_GB) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    // Shrunk from a flat 512 (2026-07-31) — the only thing this function's
    // own validation (header_checksum_valid()) actually needs is bytes
    // through 0xBD (GBA, need >=190) or 0x14D (GB/GBC, need >=334); nothing
    // downstream (enrich_info_from_buf's title/game-code extraction) reads
    // past those offsets either. Rounded up to a packet-aligned (64-byte)
    // boundary: 192 for GBA (was 512 — well under half), 384 for GB/GBC
    // (a smaller reduction, since GB/GBC's checksum sits much later in the
    // header). GENUINELY UNTESTED HYPOTHESIS, not a confirmed fix: the
    // measured ~75-78% write-stall rate on cmd 0x00 (vs ~22% on cmd 0x04,
    // see CLAUDE.md) was correlated with the command byte, not proven to
    // depend on the declared size field within it — this is the direct,
    // controlled experiment to find out whether a smaller declared size
    // also reduces how often the write itself stalls, since that's the
    // dominant cost in every measurement so far, not the amount of data
    // actually streamed once a cycle succeeds.
    const uint32_t total = (type == CART_TYPE_GBA) ? 192 : 384;

    for (int cycle = 1; cycle <= max_cycles; cycle++) {
        if (out_cycles_used) *out_cycles_used = cycle;
        if (progress_cb) progress_cb(progress_ctx);

        uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x00, 0x02 };
        cmd[2] = (uint8_t)(total & 0xFF);
        cmd[3] = (uint8_t)((total >> 8) & 0xFF);
        lprintf("[hdr-continuation] cycle %d/%d: requesting %u bytes (same handle, no reopen)\n",
                cycle, max_cycles, total);
        if (gbop_bulk_send(dev, cmd) < 0) {
            lprintf("[hdr-continuation] cycle %d: command send failed — hard USB failure\n", cycle);
            return -1;
        }

        int m = newproto_read_marker(dev, 0x00, 0x00, NULL);
        if (m != 0) {
            // No data streamed at all yet for this cycle — nothing to drain,
            // just resend on the same handle.
            lprintf("[hdr-continuation] cycle %d: header marker mismatch/error (m=%d) — "
                    "retrying same handle\n", cycle, m);
            continue;
        }

        dev->np_active = 1;
        dev->np_footer_seen = 0;
        dev->np_cmd = 0x00;
        dev->np_total = total;
        dev->np_given = 0;
        dev->np_stall_streak = 0;

        memset(hdr_out, 0, 512);
        int rc = newproto_stream_body(dev, 0x00, hdr_out, total);
        if (rc != 0) {
            dev->np_active = 0;
            lprintf("[hdr-continuation] cycle %d: stream read failed — hard USB failure\n", cycle);
            return -1;
        }

        if (dev->np_given < total) {
            // Real footer arrived before we got all 512 bytes — always a
            // genuine misalignment here (every real ROM is far larger than
            // 512 bytes, unlike the full-dump case where an early footer can
            // legitimately mean "the size estimate was too big"). The footer
            // was already consumed by stream_body's own detection, so
            // there's nothing left to drain — just retry.
            dev->np_active = 0;
            lprintf("[hdr-continuation] cycle %d: footer arrived early (given=%u of %u) — "
                    "misaligned, retrying same handle\n", cycle, dev->np_given, total);
            continue;
        }

        // Got all 512 bytes; the device's own footer for this exchange
        // hasn't been read yet (stream_body stops once buffer_size is
        // filled). Validate before deciding whether we need it.
        if (header_checksum_valid(hdr_out)) {
            lprintf("[hdr-continuation] cycle %d: 512 bytes, checksum OK — SUCCESS after %d cycle(s)\n",
                    cycle, cycle);
            dev->np_active = 0;
            return 0;
        }

        // Checksum failed — this cycle's data is unusable. Drain to the real
        // footer before resending, same reasoning as the full dump's
        // bad-front drain: without this, the pending footer would land as a
        // stale/foreign marker at the top of the next cycle.
        lprintf("[hdr-continuation] cycle %d: 512 bytes received but failed header checksum — "
                "draining to footer, staying on same handle\n", cycle);
        int drain_failed = 0;
        while (!dev->np_footer_seen) {
            int drc = newproto_stream_body(dev, 0x00, s_continuation_scratch, sizeof(s_continuation_scratch));
            if (drc != 0) { drain_failed = 1; break; }
        }
        dev->np_active = 0;
        if (drain_failed) {
            lprintf("[hdr-continuation] cycle %d: drain itself failed mid-stream — hard USB failure, "
                    "aborting (caller should close/reopen)\n", cycle);
            return -1;
        }
        lprintf("[hdr-continuation] cycle %d: drained cleanly to footer\n", cycle);
    }

    lprintf("[hdr-continuation] exhausted %d cycles without a valid header — giving up\n", max_cycles);
    return -1;
}

// ---- Save read (cmd 0x02) --------------------------------------------------
static int gbop_read_save_new(GBOperatorHandle handle, const CartInfo *info,
                               uint8_t *buffer, uint32_t buffer_size) {
    if (!handle || !info || !buffer || buffer_size == 0) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;
    uint32_t save_size = info->ram_size_kb * 1024;
    if (save_size == 0) { lprintf("[newproto] gbop_read_save: save size unknown\n"); return -1; }
    if (buffer_size < save_size) { lprintf("[newproto] gbop_read_save: buffer too small\n"); return -1; }

    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x02 };
    if (info->type == CART_TYPE_GBA) { cmd[1] = 0x03; cmd[4] = 0x00; cmd[5] = 0x01; }
    else                             { cmd[1] = 0x00; cmd[4] = 0x20; cmd[5] = 0x00; }
    cmd[6] = (uint8_t)((save_size >>  0) & 0xFF);
    cmd[7] = (uint8_t)((save_size >>  8) & 0xFF);
    cmd[8] = (uint8_t)((save_size >> 16) & 0xFF);

    lprintf("[newproto] Save read start: %u bytes (%u KB)\n", save_size, save_size / 1024);
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    // Strict marker required — same reasoning as gbop_dump_rom_new(): a
    // non-zero "recovered" packet here could be stale real data left behind
    // by an earlier aborted read, not genuinely fresh save data, and
    // trusting it risks the same false-early-footer corruption that zeroed
    // out a ROM dump on hardware (test_10). A save file is smaller than a
    // ROM but no less important to get right — failing cleanly costs a
    // retry, not a corrupted save.
    int m = newproto_read_marker(dev, 0x02, 0x00, NULL);
    if (m != 0) { lprintf("[newproto] save read: header marker mismatch/error (m=%d)\n", m); return -1; }

    dev->np_active = 1;
    dev->np_footer_seen = 0;
    dev->np_cmd = 0x02;
    dev->np_total = save_size;
    dev->np_given = 0;
    dev->np_stall_streak = 0;

    int rc = newproto_stream_body(dev, 0x02, buffer, save_size);
    if (rc != 0) { dev->np_active = 0; return rc; }

    if (newproto_check_early_footer(dev, 0x02, info->ram_size_confirmed) != 0) {
        dev->np_active = 0;
        return -1;
    }

    if (newproto_confirm_footer_immediate(dev, 0x02) != 0) {
        dev->np_active = 0;
        return -1;
    }

    dev->np_active = 0;
    lprintf("[newproto] save read complete: %u bytes\n", save_size);
    return 0;
}

int gbop_read_save(GBOperatorHandle handle, const CartInfo *info,
                   uint8_t *buffer, uint32_t buffer_size) {
    if (g_settings.use_old_firmware) return gbop_read_save_legacy(handle, info, buffer, buffer_size);
    return gbop_read_save_new(handle, info, buffer, buffer_size);
}

// ---- Save write (cmd 0x03) -------------------------------------------------
static int gbop_write_save_new(GBOperatorHandle handle, const CartInfo *info,
                                const uint8_t *buffer, uint32_t buffer_size) {
    if (!handle || !info || !buffer || buffer_size == 0) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;

    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x03 };
    if (info->type == CART_TYPE_GBA) { cmd[1] = 0x03; cmd[4] = 0x00; cmd[5] = 0x01; }
    else                             { cmd[1] = 0x00; cmd[4] = 0x20; cmd[5] = 0x00; }
    cmd[6] = (uint8_t)((buffer_size >>  0) & 0xFF);
    cmd[7] = (uint8_t)((buffer_size >>  8) & 0xFF);
    cmd[8] = (uint8_t)((buffer_size >> 16) & 0xFF);

    lprintf("[newproto] Save write start: %u bytes (%u KB)\n", buffer_size, buffer_size / 1024);
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    // No recovery path applies here (unlike the read-side calls above) —
    // the device isn't streaming payload data at this point, just
    // acknowledging the write command, so a non-header packet has nothing
    // useful to recover; treat m==1 the same as m==2.
    int m = newproto_read_marker(dev, 0x03, 0x00, NULL);
    if (m != 0) { lprintf("[newproto] save write: header marker mismatch/error (m=%d)\n", m); return -1; }

    // Host pushes all data as plain 64-byte OUT chunks, no per-chunk ACK —
    // confirmed via capture: zero IN traffic exists between the header
    // marker and the device's completion footer for the entire write.
    uint32_t sent = 0;
    while (sent < buffer_size) {
        uint32_t chunk = buffer_size - sent;
        if (chunk > GBOP_PKT_SIZE) chunk = GBOP_PKT_SIZE;
        memcpy(s_tx, buffer + sent, chunk);
        if (chunk < GBOP_PKT_SIZE) memset(s_tx + chunk, 0, GBOP_PKT_SIZE - chunk);
        DCFlushRange(s_tx, GBOP_PKT_SIZE);
        s32 w = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
        if (w < 0) {
            lprintf("[newproto] save write: chunk write error w=%d at %u/%u bytes\n",
                    (int)w, sent, buffer_size);
            return -1;
        }
        sent += chunk;
        if (sent % (8 * 1024) == 0 || sent >= buffer_size) {
            lprintf("[write] %u / %u KB\n", sent / 1024, buffer_size / 1024);
            log_flush_safe();
        }
    }

    int m2 = newproto_read_marker(dev, 0x03, 0x01, NULL);
    if (m2 != 0) lprintf("[newproto] save write: footer marker mismatch/error (m=%d) — data was sent regardless\n", m2);

    lprintf("[gbop] save write complete: %u bytes\n", sent);
    return 0;
}

int gbop_write_save(GBOperatorHandle handle, const CartInfo *info,
                    const uint8_t *buffer, uint32_t buffer_size) {
    if (g_settings.use_old_firmware) return gbop_write_save_legacy(handle, info, buffer, buffer_size);
    return gbop_write_save_new(handle, info, buffer, buffer_size);
}

// ---- RTC read/write (cmd 0x09 / 0x10) — new-firmware only, GBC (MBC3+RTC) --
// Confirmed via Wireshark (Firmware-V10_0_10-WireShark/
// save_dump2upload_gba_and_gbc.pcapng, 2026-07-31 capture; see CLAUDE.md for
// the full derivation) — same C0DE marker/data/footer framing as every other
// command. The data block is a full 512-byte block (matching the marker-
// block convention used elsewhere, e.g. cart-info), with only the first 16
// bytes meaningful; read the whole block and confirm the footer normally
// rather than requesting a short 16-byte read, to stay consistent with every
// other exchange in this protocol and avoid short-read desync.
//
// GBC (MBC3+RTC, e.g. Pokemon Gold/Silver/Crystal): payload is four little-
// endian 32-bit integers — the cart's OWN raw MBC3 RTC registers (seconds,
// minutes, hours, day_counter_low), NOT BCD, NOT tied to any external clock.
// Confirmed by the seconds field incrementing in exact lockstep with real
// elapsed capture time across 4 independent samples (deltas of 6, 6, and 11
// real seconds all matched exactly), while the other 3 fields stayed
// constant throughout. Epilogue's own write payload mirrored a read taken
// moments earlier with the seconds value correctly interpolated forward for
// elapsed time — consistent with "preserve the cart's own running RTC state
// across a save operation," not syncing to any external time source. This is
// why gbop_write_rtc_gbc() takes a GbcRtcSnapshot the caller must have
// obtained from gbop_read_rtc_gbc() first — there is no independent time
// source involved for GBC, only read-then-write-back.
//
// GBA (Seiko RTC, e.g. Ruby/Sapphire/Emerald): device bytes 0..3 are
// confirmed BCD (year/month/day/weekday), and the write payload's bytes
// 8..11 are a confirmed, verified little-endian Unix timestamp of the PC's
// wall-clock time (matched a 16-second capture-to-capture delta exactly, and
// decoded to within a minute of the capture file's own save time). Bytes
// 4..7 (three genuinely unresolved bytes plus one separately-noted constant
// byte at offset 7) remain unresolved — they did not track elapsed time
// cleanly across samples the way the GBC and Unix-timestamp fields did —
// these are passed through unmodified from a prior read (see
// GbaRtcSnapshot's date_bcd/misc_bytes fields) rather than guessed at. The
// read response has no confirmed timestamp field (only the write payload
// was observed to carry one), so gbop_read_rtc_gba() leaves unix_time at 0.
//
// unix_time is deliberately NOT the Wii's absolute wall-clock date taken at
// write time — many Wiis (including the one this project is developed on)
// have a dead RTC/CMOS backup battery, so the Wii's own idea of "today" can
// be flatly wrong. gbop_write_rtc_gba() instead computes base_unix_time (an
// anchor captured at read time via gbop_wii_unix_time()) plus the real
// elapsed time since the read, measured via gettime()/ticks_to_secs() — the
// Wii's monotonic CPU tick counter, which keeps correct real-time pace
// regardless of the RTC battery. This mirrors the GBC strategy above
// exactly (read, then interpolate forward by elapsed time) rather than
// trusting the Wii's absolute clock a second time.
//
// CORRECTED 2026-07-31 (see CLAUDE.md): an earlier version of this comment
// claimed no libogc API exists on this project's devkitPPC/libogc install to
// read the Wii's own wall-clock time. That was wrong — the user correctly
// pushed back, and __SYS_GetRTC(u32 *gctime) is a real, linkable libogc
// symbol (confirmed via powerpc-eabi-nm/objdump against the compiled
// libogc.a, and cross-checked against libogc's own timesupp.c source and
// WiiBrew/GBAtemp community documentation — see the Sources table). It
// returns seconds since the Wii/GameCube epoch (2000-01-01 UTC); adding the
// fixed offset 946684800 (seconds between 1970-01-01 and 2000-01-01) yields
// a real Unix timestamp. gbop_wii_unix_time() below wraps this.

int gbop_read_rtc_gbc(GBOperatorHandle handle, GbcRtcSnapshot *out) {
    if (!handle || !out) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;

    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x09 };
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    int m = newproto_read_marker(dev, 0x09, 0x00, NULL);
    if (m != 0) { lprintf("[newproto] RTC read: header marker mismatch/error (m=%d)\n", m); return -1; }

    uint8_t data[512];
    dev->np_active = 1;
    dev->np_footer_seen = 0;
    dev->np_cmd = 0x09;
    dev->np_total = sizeof(data);
    dev->np_given = 0;
    dev->np_stall_streak = 0;
    int rc = newproto_stream_body(dev, 0x09, data, sizeof(data));
    if (rc != 0) { dev->np_active = 0; return -1; }
    if (newproto_confirm_footer_immediate(dev, 0x09) != 0) { dev->np_active = 0; return -1; }
    dev->np_active = 0;

    out->seconds = (uint32_t)data[0]  | ((uint32_t)data[1]  << 8) | ((uint32_t)data[2]  << 16) | ((uint32_t)data[3]  << 24);
    out->minutes = (uint32_t)data[4]  | ((uint32_t)data[5]  << 8) | ((uint32_t)data[6]  << 16) | ((uint32_t)data[7]  << 24);
    out->hours   = (uint32_t)data[8]  | ((uint32_t)data[9]  << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
    out->day_low = (uint32_t)data[12] | ((uint32_t)data[13] << 8) | ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24);

    lprintf("[newproto] RTC read (GBC): sec=%u min=%u hour=%u day_low=%u\n",
            out->seconds, out->minutes, out->hours, out->day_low);
    return 0;
}

int gbop_write_rtc_gbc(GBOperatorHandle handle, const GbcRtcSnapshot *snap) {
    if (!handle || !snap) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;

    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x10 };
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    int m = newproto_read_marker(dev, 0x10, 0x00, NULL);
    if (m != 0) { lprintf("[newproto] RTC write: header marker mismatch/error (m=%d)\n", m); return -1; }

    memset(s_tx, 0, GBOP_PKT_SIZE);
    s_tx[0]  = (uint8_t)(snap->seconds);       s_tx[1]  = (uint8_t)(snap->seconds >> 8);
    s_tx[2]  = (uint8_t)(snap->seconds >> 16); s_tx[3]  = (uint8_t)(snap->seconds >> 24);
    s_tx[4]  = (uint8_t)(snap->minutes);       s_tx[5]  = (uint8_t)(snap->minutes >> 8);
    s_tx[6]  = (uint8_t)(snap->minutes >> 16); s_tx[7]  = (uint8_t)(snap->minutes >> 24);
    s_tx[8]  = (uint8_t)(snap->hours);         s_tx[9]  = (uint8_t)(snap->hours >> 8);
    s_tx[10] = (uint8_t)(snap->hours >> 16);   s_tx[11] = (uint8_t)(snap->hours >> 24);
    s_tx[12] = (uint8_t)(snap->day_low);       s_tx[13] = (uint8_t)(snap->day_low >> 8);
    s_tx[14] = (uint8_t)(snap->day_low >> 16); s_tx[15] = (uint8_t)(snap->day_low >> 24);
    DCFlushRange(s_tx, GBOP_PKT_SIZE);
    s32 w = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
    if (w < 0) { lprintf("[newproto] RTC write: payload write error w=%d\n", (int)w); return -1; }

    int m2 = newproto_read_marker(dev, 0x10, 0x01, NULL);
    if (m2 != 0) { lprintf("[newproto] RTC write: footer marker mismatch/error (m=%d)\n", m2); return -1; }

    lprintf("[newproto] RTC write (GBC): sec=%u min=%u hour=%u day_low=%u\n",
            snap->seconds, snap->minutes, snap->hours, snap->day_low);
    return 0;
}

// __SYS_GetRTC is a real, linkable libogc symbol not declared in any public
// header this project includes (ogc/system.h, ogc/stm.h) — confirmed present
// in the compiled libogc.a via powerpc-eabi-nm/objdump, and confirmed by
// cross-referencing libogc's own source (libogc/timesupp.c) plus WiiBrew/
// GBAtemp community documentation (see Sources table in CLAUDE.md). It reads
// the Wii's hardware RTC as seconds since the Wii/GameCube epoch
// (2000-01-01 00:00:00 UTC), not the Unix epoch.
extern int __SYS_GetRTC(u32 *gctime);

// Seconds between the Unix epoch (1970-01-01) and the Wii/GameCube epoch
// (2000-01-01) — a fixed, well-known constant (confirmed against libogc's
// own timesupp.c, which applies the same offset).
#define GBOP_WII_UNIX_EPOCH_OFFSET 946684800u

uint32_t gbop_wii_unix_time(void) {
    u32 gctime = 0;
    if (__SYS_GetRTC(&gctime) == 0) {
        lprintf("[gbop] __SYS_GetRTC failed\n");
        return 0;
    }
    return (uint32_t)gctime + GBOP_WII_UNIX_EPOCH_OFFSET;
}

int gbop_read_rtc_gba(GBOperatorHandle handle, GbaRtcSnapshot *out) {
    if (!handle || !out) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;

    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x09 };
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    int m = newproto_read_marker(dev, 0x09, 0x00, NULL);
    if (m != 0) { lprintf("[newproto] RTC read (GBA): header marker mismatch/error (m=%d)\n", m); return -1; }

    uint8_t data[512];
    dev->np_active = 1;
    dev->np_footer_seen = 0;
    dev->np_cmd = 0x09;
    dev->np_total = sizeof(data);
    dev->np_given = 0;
    dev->np_stall_streak = 0;
    int rc = newproto_stream_body(dev, 0x09, data, sizeof(data));
    if (rc != 0) { dev->np_active = 0; return -1; }
    if (newproto_confirm_footer_immediate(dev, 0x09) != 0) { dev->np_active = 0; return -1; }
    dev->np_active = 0;

    memcpy(out->date_bcd, &data[0], 4);
    memcpy(out->misc_bytes, &data[4], 4);
    out->unix_time = 0; /* no confirmed timestamp field on read — see GbaRtcSnapshot */
    out->base_unix_time = gbop_wii_unix_time(); /* anchor; may be a wrong absolute date on hardware with a dead RTC battery — only its forward progression matters */
    out->read_tick = gettime(); /* monotonic (CPU tick counter), unaffected by a dead RTC battery */

    lprintf("[newproto] RTC read (GBA): date_bcd=%02X %02X %02X %02X misc=%02X %02X %02X %02X base_unix=%u\n",
            out->date_bcd[0], out->date_bcd[1], out->date_bcd[2], out->date_bcd[3],
            out->misc_bytes[0], out->misc_bytes[1], out->misc_bytes[2], out->misc_bytes[3],
            out->base_unix_time);
    return 0;
}

int gbop_write_rtc_gba(GBOperatorHandle handle, GbaRtcSnapshot *snap) {
    if (!handle || !snap) return -1;
    GBOpDevice *dev = (GBOpDevice *)handle;

    /* Compute the value to write as base_unix_time + real elapsed time
     * since the read, measured via the Wii's monotonic CPU tick counter —
     * deliberately NOT by re-querying the Wii's own (possibly wrong, e.g.
     * dead RTC battery) wall clock a second time. This mirrors the GBC
     * "read, then interpolate forward by elapsed time" strategy exactly
     * (see gbop_write_rtc_gbc()'s header comment). */
    uint64_t elapsed_ticks = gettime() - snap->read_tick;
    uint32_t elapsed_sec = (uint32_t)ticks_to_secs(elapsed_ticks);
    snap->unix_time = snap->base_unix_time + elapsed_sec;

    uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x10 };
    if (gbop_bulk_send(dev, cmd) < 0) return -1;

    int m = newproto_read_marker(dev, 0x10, 0x00, NULL);
    if (m != 0) { lprintf("[newproto] RTC write (GBA): header marker mismatch/error (m=%d)\n", m); return -1; }

    memset(s_tx, 0, GBOP_PKT_SIZE);
    memcpy(&s_tx[0], snap->date_bcd, 4);
    memcpy(&s_tx[4], snap->misc_bytes, 4);
    s_tx[8]  = (uint8_t)(snap->unix_time);
    s_tx[9]  = (uint8_t)(snap->unix_time >> 8);
    s_tx[10] = (uint8_t)(snap->unix_time >> 16);
    s_tx[11] = (uint8_t)(snap->unix_time >> 24);
    DCFlushRange(s_tx, GBOP_PKT_SIZE);
    s32 w = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
    if (w < 0) { lprintf("[newproto] RTC write (GBA): payload write error w=%d\n", (int)w); return -1; }

    int m2 = newproto_read_marker(dev, 0x10, 0x01, NULL);
    if (m2 != 0) { lprintf("[newproto] RTC write (GBA): footer marker mismatch/error (m=%d)\n", m2); return -1; }

    lprintf("[newproto] RTC write (GBA): date_bcd=%02X %02X %02X %02X misc=%02X %02X %02X %02X base_unix=%u elapsed_sec=%u unix=%u\n",
            snap->date_bcd[0], snap->date_bcd[1], snap->date_bcd[2], snap->date_bcd[3],
            snap->misc_bytes[0], snap->misc_bytes[1], snap->misc_bytes[2], snap->misc_bytes[3],
            snap->base_unix_time, elapsed_sec, snap->unix_time);
    return 0;
}

void gbop_close(GBOperatorHandle handle) {
    if (!handle) return;
    GBOpDevice *dev = (GBOpDevice *)handle;
    USB_CloseDevice(&dev->fd);
    free(dev);
}

// ─── Hardware probe ──────────────────────────────────────────────────────────
// Runs a series of low-level USB tests to characterise how this specific IOS/
// hardware combination behaves.  Results go to the log (and TV console via
// lprintf) so the user can paste them to us for diagnosis.  Call with a valid
// CartInfo (cart inserted) so ROM/save tests can run.

// Drain helper used by probe: reads up to max_chunks 64-byte packets and logs
// their sizes.  Never calls USB_ReadBlkMsg(4) — that corrupts the IOS endpoint.
// Stops on rd<=0 or when max_chunks reached.  Returns number of chunks drained.
static int probe_drain(GBOpDevice *dev, const char *tag, int max_chunks) {
    int n = 0;
    for (int i = 0; i < max_chunks; i++) {
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        lprintf("[probe] %s drain[%d] rd=%d [0..3]=%02X %02X %02X %02X\n",
                tag, i, (int)rd, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
        if (rd <= 0) break;
        n++;
    }
    return n;
}

// Wraps gbop_reopen() with the same fresh-fd recovery main.c already uses for
// ROM install / play_game / header-read retries. Without this, the probe's own
// dozen-plus reopens in quick succession exhaust the fd (same fd-spend
// condition documented elsewhere) and every test after the first couple just
// logs "open failed" instead of running — the probe never reaches Tests 1-4.
static GBOperatorHandle probe_reopen(int32_t *last_fd) {
    GBOperatorHandle op = gbop_reopen();
    if (!op) {
        lprintf("[probe] reopen failed — waiting for fresh fd\n");
        op = gbop_reopen_wait_fresh(*last_fd, NULL);
    }
    if (op) *last_fd = gbop_get_fd(op);
    return op;
}

void gbop_probe_hardware(const CartInfo *info) {
    GBOpDevice *dev;
    int32_t last_fd = INT32_MAX; // sentinel: cannot match a real fd, so the
                                  // very first successful open is accepted.

    lprintf("[probe] === USB Hardware Probe ===\n");
    // fflush only, not log_commit_sd() (fclose+fopen) — this function makes
    // many commit points across one run, and repeated fclose+fopen cycling
    // within a session is an already-documented heap-corruption source in
    // this codebase (see mgba_run()'s "reduced from ~32 to 1" fix in
    // CLAUDE.md). Every commit point in this function uses log_force_flush()
    // now except the true final one at the end of the probe.
    log_force_flush();

    // ---- Test 0: Timing sweep — cmd 0x04 with increasing delays after open ----
    // On IOS58, USB_WriteBlkMsg itself takes ≥200ms, giving the device implicit
    // settle time before the response is read.  On d2x-cIOS writes are fast
    // (<10ms), so the device may not have finished reading the cart yet.  This
    // test adds an explicit usleep between reopen and command to find the minimum
    // delay that produces valid cart data.  Also logs resp[26..27] so we can tell
    // whether a wrong ROM size correlates with shorter delays.
    lprintf("[probe] --- Test 0: Timing sweep (delay after open, then cmd 0x04) ---\n");
    {
        static const int kT0Delays[] = {0, 50, 100, 200, 500, 1000};
        for (int d = 0; d < 6; d++) {
            int delay_ms = kT0Delays[d];
            GBOperatorHandle th = probe_reopen(&last_fd);
            if (!th) { lprintf("[probe] T0[%dms]: open failed\n", delay_ms); continue; }
            if (delay_ms > 0) usleep((uint32_t)delay_ms * 1000);
            CartInfo ti = {0};
            int rc = gbop_read_cart_info(th, &ti);
            gbop_close(th);
            // rom_size_kb > 0 used to be required too, but on the new-firmware
            // protocol cart-info never populates size at all (resolved later
            // from the ROM header, see rom_sizes.h) — that extra condition
            // made this print "no cart" on every single genuinely successful
            // read this test has ever logged, on this firmware (confirmed on
            // hardware, Post Firmware Update Test/test_15: a real, valid
            // resp[] with the cart's actual data came back at T0[0ms], but
            // was mislabeled "no cart" solely because rom_size_kb was 0, as
            // it always legitimately is at this point on this firmware).
            // rc == GBOP_OK on its own already means resp[3:5] was non-zero —
            // that's the actual "cart present" signal gbop_read_cart_info
            // uses everywhere else.
            if (rc == GBOP_OK) {
                lprintf("[probe] T0[%dms]: VALID type=%s rom=%uKB ram=%uKB (size resolved separately) resp[26]=0x%02X resp[27]=0x%02X\n",
                        delay_ms, ti.type_str, ti.rom_size_kb, ti.ram_size_kb,
                        ti.raw_resp[26], ti.raw_resp[27]);
            } else {
                lprintf("[probe] T0[%dms]: no cart (rc=%d)\n", delay_ms, rc);
            }
            usleep(100000);
        }
    }
    log_force_flush();

    // ---- Test 1: Cart info ACK size — 60-byte read vs 64-byte read ----
    // IMPORTANT: never issue USB_ReadBlkMsg(4) for the footer here.  Doing so
    // when no 4-byte packet is pending (rb=-7008) leaves the IOS endpoint in a
    // broken state where every subsequent 64-byte read hangs indefinitely.
    // Instead we absorb optional footer packets by using 64-byte reads and
    // checking if rd==4 (same pattern as gbop_bulk_recv).
    // IMPORTANT: use gbop_reopen(), NOT gbop_open_hw().  gbop_open_hw() sends
    // USB_WriteCtrlMsg SET_CONFIGURATION which resets d2x-cIOS device state
    // and causes the subsequent USB_ReadBlkMsg to hang.  gbop_reopen() skips
    // SET_CONFIGURATION (same reason it is skipped in the poll loop).
    lprintf("[probe] --- Test 1: ACK size (60 vs 64) ---\n");

    // T1a: read ACK with 60-byte request
    dev = (GBOpDevice *)probe_reopen(&last_fd);
    if (dev) {
        uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x04 };
        gbop_bulk_send(dev, cmd);
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
        lprintf("[probe] T1a: ACK read60=%d\n", (int)ra);
        if (ra >= 0) {
            // Drain exactly 2 data chunks (handles optional 4B footer via rd==4)
            for (int i = 0; i < 2; i++) {
                uint8_t *chunk = s_resp + i * 64;
                DCInvalidateRange(chunk, 64);
                s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                if (rd == 4) {
                    lprintf("[probe] T1a: 4B footer before chunk[%d]\n", i);
                    DCInvalidateRange(chunk, 64);
                    rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                }
                lprintf("[probe] T1a: chunk[%d] rd=%d\n", i, (int)rd);
                if (rd <= 0) break;
            }
        }
        USB_CloseDevice(&dev->fd); free(dev); dev = NULL;
        usleep(300000);
    } else { lprintf("[probe] T1a: open failed\n"); }

    log_force_flush();  // commit T1a results (fflush only — see note at top of this function)

    // T1b: read ACK with 64-byte request
    dev = (GBOpDevice *)probe_reopen(&last_fd);
    if (dev) {
        uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x04 };
        gbop_bulk_send(dev, cmd);
        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        lprintf("[probe] T1b: ACK read64=%d\n", (int)ra);
        if (ra >= 0) {
            for (int i = 0; i < 2; i++) {
                uint8_t *chunk = s_resp + i * 64;
                DCInvalidateRange(chunk, 64);
                s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                if (rd == 4) {
                    lprintf("[probe] T1b: 4B footer before chunk[%d]\n", i);
                    DCInvalidateRange(chunk, 64);
                    rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                }
                lprintf("[probe] T1b: chunk[%d] rd=%d\n", i, (int)rd);
                if (rd <= 0) break;
            }
        }
        USB_CloseDevice(&dev->fd); free(dev); dev = NULL;
        usleep(300000);
    } else { lprintf("[probe] T1b: open failed\n"); }

    log_force_flush();  // commit T1b results

    // ---- Test 2: ROM cmd handshake — r1 value and drain count ----
    // After a 64-zero host ACK the device sends a "ready to stream" response.
    // On the developer's IOS58: r1=16 (short), then 3 more drain packets before ROM data.
    // On d2x-cIOS test_external_6: r1=64, followed by ZLP to signal ready.
    // IMPORTANT: use 64-byte reads throughout; never USB_ReadBlkMsg(4).
    lprintf("[probe] --- Test 2: ROM cmd handshake r1 and chunk sizes ---\n");

    // Runs unconditionally — probe_sz=512 is fixed, cart size not needed.
    // Even when cmd 0x04 (cart info) returns no cart, cmd 0x00 (ROM read) may
    // succeed if the cart is physically inserted but the device needs a different
    // command path.  Valid ROM header bytes here while T0 shows NOCART would
    // confirm cmd 0x04 is the timing-sensitive path.
    {
        dev = (GBOpDevice *)probe_reopen(&last_fd);
        if (dev) {
            uint32_t probe_sz = 512;
            uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x00, 0x02 };
            cmd[2] = probe_sz & 0xFF; cmd[3] = (probe_sz >> 8) & 0xFF;
            gbop_bulk_send(dev, cmd);

            // Read command ACK using the same 64-byte-first pattern as gbop_bulk_recv
            for (int i = 0; i < 2; i++) {
                uint8_t *chunk = s_resp + i * 64;
                DCInvalidateRange(chunk, 64);
                s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                if (rd == 4) {
                    lprintf("[probe] T2: cmd ACK 4B footer before chunk[%d]\n", i);
                    DCInvalidateRange(chunk, 64);
                    rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                }
                lprintf("[probe] T2: cmd ACK chunk[%d] rd=%d [0..3]=%02X %02X %02X %02X\n",
                        i, (int)rd, chunk[0], chunk[1], chunk[2], chunk[3]);
                if (rd <= 0) { USB_CloseDevice(&dev->fd); free(dev); dev = NULL; break; }
            }

            if (dev) {
                memset(s_tx, 0, GBOP_PKT_SIZE);
                DCFlushRange(s_tx, GBOP_PKT_SIZE);
                s32 wack = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
                lprintf("[probe] T2: host ACK w=%d\n", (int)wack);

                if (wack > 0) {
                    // Read "ready to stream" response — log the first 8 reads
                    for (int i = 0; i < 8; i++) {
                        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
                        lprintf("[probe] T2: rsp[%d] rd=%d [0..3]=%02X %02X %02X %02X\n",
                                i, (int)rd, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
                        // rd=0 is a ZLP — device is ready to stream; stop draining
                        if (rd <= 0) break;
                    }
                }

                USB_CloseDevice(&dev->fd); free(dev); dev = NULL;
            }
            usleep(500000);
        } else { lprintf("[probe] T2: open failed\n"); }
    }

    log_force_flush();  // commit T2 results

    // ---- Test 3: Batch-1 ACK drain sequence ----
    // Read exactly 320 ROM iter chunks (one full batch), send an in-stream ACK,
    // then log the next 4 USB reads.  On the developer's hardware: drain[0]=rd60 zeros,
    // drain[1]=rd4 zeros, then ROM resumes.  The extra rd=16 packet at the final
    // batch on Ruby/Emerald caused the original hang — we want to see if this
    // hardware has a different post-ACK sequence.
    lprintf("[probe] --- Test 3: Batch-1 ACK drain sequence ---\n");

    if (info && info->rom_size_kb >= 16) {
        dev = (GBOpDevice *)probe_reopen(&last_fd);
        if (dev) {
            uint32_t total = info->rom_size_kb * 1024;
            uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x00, 0x02 };
            cmd[2] = total & 0xFF; cmd[3] = (total >> 8) & 0xFF;
            cmd[4] = (total >> 16) & 0xFF; cmd[5] = (total >> 24) & 0xFF;
            gbop_bulk_send(dev, cmd);

            // Command ACK — drain using gbop_bulk_recv-style 64-byte reads
            int t3_ok = 1;
            for (int i = 0; i < 2; i++) {
                uint8_t *chunk = s_resp + i * 64;
                DCInvalidateRange(chunk, 64);
                s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                if (rd == 4) {
                    DCInvalidateRange(chunk, 64);
                    rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                }
                if (rd <= 0) { t3_ok = 0; lprintf("[probe] T3: cmd ACK error rd=%d\n", (int)rd); break; }
            }

            if (t3_ok) {
                // Host ACK
                memset(s_tx, 0, GBOP_PKT_SIZE);
                DCFlushRange(s_tx, GBOP_PKT_SIZE);
                s32 wack = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
                lprintf("[probe] T3: host ACK w=%d\n", (int)wack);
                if (wack <= 0) t3_ok = 0;
            }

            if (t3_ok) {
                // "Ready to stream" response — drain until rd==0 (ZLP) or rd=16
                int saw_r1_16 = 0;
                for (int i = 0; i < 8 && t3_ok; i++) {
                    DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                    s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
                    lprintf("[probe] T3: ready rsp[%d] rd=%d\n", i, (int)rd);
                    if (rd <= 0) break;
                    if (rd == 16) { saw_r1_16 = 1; }
                    if (rd == 0) break; // ZLP — done
                }
                // If r1=16 was seen, drain 3 more packets
                if (saw_r1_16) {
                    probe_drain(dev, "T3-hdrdrain", 3);
                }
            }

            if (t3_ok) {
                // Read exactly 320 iter chunks.  Log first 10 and any anomaly
                // (rd != 60 && rd != 16) to reveal the chunk-size pattern.
                uint32_t iter_cnt = 0;
                uint32_t raw_cnt  = 0;  // all USB reads including discarded
                while (iter_cnt < 320) {
                    DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                    s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
                    raw_cnt++;
                    if (rd <= 0) {
                        lprintf("[probe] T3: read error at iter=%u raw=%u rd=%d\n",
                                iter_cnt, raw_cnt, (int)rd);
                        t3_ok = 0; break;
                    }
                    int is_data = (rd >= 16);
                    if (iter_cnt < 10 || !is_data || (rd != 60 && rd != 16)) {
                        lprintf("[probe] T3: raw[%u] rd=%d%s\n",
                                raw_cnt - 1, (int)rd, is_data ? "" : " (discarded)");
                    }
                    if (is_data) iter_cnt++;
                }
                lprintf("[probe] T3: %u iters read (%u raw reads)%s\n",
                        iter_cnt, raw_cnt, t3_ok ? "" : " (error)");

                if (t3_ok) {
                    memset(s_tx, 0, GBOP_PKT_SIZE);
                    DCFlushRange(s_tx, GBOP_PKT_SIZE);
                    s32 wack = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
                    lprintf("[probe] T3: in-stream ACK w=%d\n", (int)wack);

                    // Log next 4 reads — this is the key data we want
                    probe_drain(dev, "T3-post-ACK", 4);
                }
            }

            USB_CloseDevice(&dev->fd); free(dev); dev = NULL;
            usleep(500000);
        } else { lprintf("[probe] T3: open failed\n"); }
    } else { lprintf("[probe] T3: skip (rom_size_kb=%u < 16)\n",
                     info ? info->rom_size_kb : 0); }

    log_force_flush();  // commit T3 results

    // ---- Test 4: Save cmd ACK — observe first chunk arrival ----
    // Issue a save read command and read the first 4 data packets.  Reveals
    // whether the GBA drain is needed (device sends 80-byte handshake before
    // data) and what the first save bytes look like.
    lprintf("[probe] --- Test 4: Save cmd ACK ---\n");

    if (info && info->ram_size_kb > 0 && info->type != CART_TYPE_UNKNOWN) {
        dev = (GBOpDevice *)probe_reopen(&last_fd);
        if (dev) {
            uint32_t save_size = info->ram_size_kb * 1024;
            uint8_t cmd[GBOP_PAYLOAD_SIZE] = { 0x02 };
            if (info->type == CART_TYPE_GBA) {
                cmd[1] = 0x03; cmd[4] = 0x00; cmd[5] = 0x01;
            } else {
                cmd[1] = 0x00; cmd[4] = 0x20; cmd[5] = 0x00;
            }
            cmd[6] = save_size & 0xFF;
            cmd[7] = (save_size >> 8) & 0xFF;
            cmd[8] = (save_size >> 16) & 0xFF;
            gbop_bulk_send(dev, cmd);

            // Read command ACK via gbop_bulk_recv-style 64-byte reads
            int t4_ok = 1;
            for (int i = 0; i < 2; i++) {
                uint8_t *chunk = s_resp + i * 64;
                DCInvalidateRange(chunk, 64);
                s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                if (rd == 4) {
                    lprintf("[probe] T4: 4B footer before chunk[%d]\n", i);
                    DCInvalidateRange(chunk, 64);
                    rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, chunk);
                }
                lprintf("[probe] T4: ACK chunk[%d] rd=%d [0..3]=%02X %02X %02X %02X\n",
                        i, (int)rd, chunk[0], chunk[1], chunk[2], chunk[3]);
                if (rd <= 0) { t4_ok = 0; break; }
            }

            if (t4_ok && info->type == CART_TYPE_GBA) {
                // GBA: may need drain (r1=16 ready-resp sequence)
                DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                s32 r1 = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
                lprintf("[probe] T4: GBA peek r1=%d [0..3]=%02X %02X %02X %02X\n",
                        (int)r1, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
                if (r1 == 16) {
                    probe_drain(dev, "T4-GBAdrain", 3);
                }
            }

            // Read first 4 save data packets
            lprintf("[probe] T4: first 4 save packets:\n");
            probe_drain(dev, "T4-savedata", 4);

            USB_CloseDevice(&dev->fd); free(dev); dev = NULL;
        } else { lprintf("[probe] T4: open failed\n"); }
    } else { lprintf("[probe] T4: skip (no save or unknown type)\n"); }

    // Real commit (fclose+fopen) here, not just fflush — lprintf() already
    // fflushes on every call, so log_force_flush() elsewhere in this function
    // is redundant with that; the thing that was actually missing is a
    // libfat sector commit, which only fclose forces (see log.h). Confirmed
    // on hardware (Post Firmware Update Test/test_5): after reducing this
    // function to one fclose at the very end, a session that ran real
    // activity through T4 and partway into T5 before hanging produced a
    // completely blank log file — everything was fflushed to the stdio
    // buffer but never reached physical SD. A handful of real commits at
    // strategic points (here, and again before T6) is nowhere near the
    // ~32-per-session cycle count that caused the original heap-corruption
    // finding this function's commit-reduction was based on.
    log_commit_sd();  // commit T0-T4 results before starting T5

    // ---- Test 5: Cart-info reliability sweep — statistics across many attempts ----
    // Calls the actual gbop_read_cart_info() the app uses for detection (not
    // a separate low-level probe) back-to-back, tabulating how it resolves
    // each time. Added 2026-07-27 after hardware testing (Post Firmware
    // Update Test/test_3) showed the header marker can be dropped by the
    // connection even when a cart is genuinely present — this quantifies how
    // often that happens, how often a fully clean exchange occurs, and how
    // often nothing usable comes back at all. Runs regardless of cart
    // presence (a real device.use_old_firmware setting is respected, same
    // as normal operation — this sweep is not a separate protocol path).
    // Test 5 did not complete on the first hardware run (Post Firmware
    // Update Test/test_4) — the log ends right after "T4: skip" with no
    // trace of Test 5 at all, twice, in two separate sessions, at the exact
    // same point. Leading hypothesis: a single USB_ReadBlkMsg call inside
    // gbop_read_cart_info_new (or inside probe_reopen's fresh-fd wait) that
    // never returns at all — IOS has documented asymmetric timeout behavior
    // where some read sizes/conditions block indefinitely rather than
    // erroring out, and this is the first place in the new protocol that
    // hammers cmd 0x04 with only a short gap between attempts (every other
    // caller in this app has much more natural spacing from menu
    // navigation, USB open/close overhead, etc.). Cannot be confirmed
    // without hardware; a syscall that never returns can't be recovered
    // from in-process on this platform, so the fix here is diagnostic —
    // flush the log before AND after every risky call so the *next* hang
    // pinpoints exactly which iteration and which specific call (reopen vs.
    // the read itself) it happened on, instead of leaving no trace — plus a
    // longer, more conservative gap between attempts as a first mitigation
    // to test whether pacing is actually the trigger.
#define NEWPROTO_PROBE_SWEEP_N 20
    lprintf("[probe] --- Test 5: Cart-info reliability sweep (%d attempts) ---\n", NEWPROTO_PROBE_SWEEP_N);
    log_force_flush();
    {
        int n_ok = 0, n_nocart = 0, n_usb = 0, n_mismatch = 0, n_other = 0;
        for (int i = 0; i < NEWPROTO_PROBE_SWEEP_N; i++) {
            lprintf("[probe] T5[%d]: reopening...\n", i);
            log_force_flush();
            dev = (GBOpDevice *)probe_reopen(&last_fd);
            if (!dev) {
                lprintf("[probe] T5[%d]: open failed\n", i);
                log_force_flush();
                n_other++;
                continue;
            }
            lprintf("[probe] T5[%d]: reopened, reading cart info...\n", i);
            log_force_flush();
            CartInfo ti = {0};
            int rc = gbop_read_cart_info((GBOperatorHandle)dev, &ti);
            USB_CloseDevice(&dev->fd); free(dev); dev = NULL;
            switch (rc) {
                case GBOP_OK:
                    n_ok++;
                    lprintf("[probe] T5[%d]: OK title=\"%s\" type=%s\n", i, ti.title, ti.type_str);
                    break;
                case GBOP_NOCART:
                    n_nocart++;
                    lprintf("[probe] T5[%d]: NOCART\n", i);
                    break;
                case GBOP_USB:
                    n_usb++;
                    lprintf("[probe] T5[%d]: USB error\n", i);
                    break;
                case GBOP_FIRMWARE_MISMATCH:
                    n_mismatch++;
                    lprintf("[probe] T5[%d]: FIRMWARE_MISMATCH\n", i);
                    break;
                default:
                    n_other++;
                    lprintf("[probe] T5[%d]: rc=%d\n", i, rc);
                    break;
            }
            log_force_flush();
            usleep(300000);  // was 50ms — widened to test whether pacing avoids the hang
        }
        lprintf("[probe] T5 summary: OK=%d NOCART=%d USB=%d MISMATCH=%d OTHER=%d (of %d attempts)\n",
                n_ok, n_nocart, n_usb, n_mismatch, n_other, NEWPROTO_PROBE_SWEEP_N);
    }
#undef NEWPROTO_PROBE_SWEEP_N
    log_commit_sd();  // real commit before T6 — see note above T5 for why

    // ---- Test 6: Persistent-handle cart-info sweep ----
    // Tests whether closing and reopening the USB device before every single
    // command — done throughout this whole app, originally necessitated by
    // the old-firmware-era IOS fd-exhaustion workaround (see "IOS USB fd
    // spend" in CLAUDE.md) — is itself degrading reliability on this
    // firmware. Added 2026-07-27 after the tester confirmed the SAME
    // physical GB Operator + cart used to capture the Wireshark traces this
    // whole protocol rebuild is based on: that capture polled cart-info 94
    // times over ONE never-reopened USB handle with clean results the whole
    // way through (confirmed directly: zero SET_CONFIGURATION/GET_DESCRIPTOR
    // requests anywhere after the initial enumeration), yet the exact same
    // hardware fails almost every attempt from the Wii, where every single
    // attempt closes and reopens first. This isolates that one variable:
    // open ONCE, then poll exactly like Test 5 but on the SAME handle
    // throughout, directly comparable to Test 5's summary line. If T6's
    // success rate is materially better than T5's, that confirms reopen
    // cycling itself is the problem, not framing/parsing. A USB error
    // partway through (the fd being spent) is itself useful data — it would
    // mean the old fd-exhaustion constraint still applies under the new
    // firmware and a persistent-handle approach needs a fresh-fd fallback
    // partway through, not that the theory is wrong.
#define NEWPROTO_PROBE_SWEEP_N2 20
    lprintf("[probe] --- Test 6: Persistent-handle cart-info sweep (%d attempts, one open) ---\n",
            NEWPROTO_PROBE_SWEEP_N2);
    log_force_flush();
    {
        dev = (GBOpDevice *)probe_reopen(&last_fd);
        if (!dev) {
            lprintf("[probe] T6: open failed\n");
        } else {
            int n_ok = 0, n_nocart = 0, n_usb = 0, n_mismatch = 0, n_other = 0;
            for (int i = 0; i < NEWPROTO_PROBE_SWEEP_N2; i++) {
                CartInfo ti = {0};
                int rc = gbop_read_cart_info((GBOperatorHandle)dev, &ti);
                switch (rc) {
                    case GBOP_OK:
                        n_ok++;
                        lprintf("[probe] T6[%d]: OK title=\"%s\" type=%s\n", i, ti.title, ti.type_str);
                        break;
                    case GBOP_NOCART:
                        n_nocart++;
                        lprintf("[probe] T6[%d]: NOCART\n", i);
                        break;
                    case GBOP_USB:
                        n_usb++;
                        lprintf("[probe] T6[%d]: USB error (fd may be spent on this handle — useful data either way)\n", i);
                        break;
                    case GBOP_FIRMWARE_MISMATCH:
                        n_mismatch++;
                        lprintf("[probe] T6[%d]: FIRMWARE_MISMATCH\n", i);
                        break;
                    default:
                        n_other++;
                        lprintf("[probe] T6[%d]: rc=%d\n", i, rc);
                        break;
                }
                log_force_flush();
                usleep(300000);
            }
            USB_CloseDevice(&dev->fd); free(dev); dev = NULL;
            lprintf("[probe] T6 summary: OK=%d NOCART=%d USB=%d MISMATCH=%d OTHER=%d (of %d attempts, single open — compare to T5)\n",
                    n_ok, n_nocart, n_usb, n_mismatch, n_other, NEWPROTO_PROBE_SWEEP_N2);
        }
    }
#undef NEWPROTO_PROBE_SWEEP_N2
    log_force_flush();

    lprintf("[probe] === Probe complete ===\n");
    log_commit_sd();  // the one real commit for this whole function — see note at top
}
