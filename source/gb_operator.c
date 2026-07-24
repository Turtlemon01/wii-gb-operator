#include "gb_operator.h"
#include "log.h"
#include <ogc/usb.h>
#include <ogc/cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// All packets are 64 bytes: 60 bytes payload + 4 bytes CRC32-MPEG2 (little-endian)
#define GBOP_PKT_SIZE      64
#define GBOP_PAYLOAD_SIZE  60

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
    uint8_t  dump_spare[64];  // leftover bytes from the last device chunk
    uint32_t dump_spare_len;
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
    // Step 1: discard the 60-byte ACK
    DCInvalidateRange(s_rx, sizeof(s_rx));
    s32 ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
    lprintf("[gbop] ACK read60=%d\n", (int)ra);
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
    return (GBOperatorHandle)dev;
}

int gbop_read_cart_info(GBOperatorHandle handle, CartInfo *out) {
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

int gbop_read_rom_header(GBOperatorHandle handle, uint8_t *hdr_out) {
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

    // Command ACK: same 60+4 format as the full ROM dump command.
    DCInvalidateRange(s_rx, GBOP_PAYLOAD_SIZE);
    s32 ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
    DCInvalidateRange(s_crc, sizeof(s_crc));
    s32 rb = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
    lprintf("[hdr] cmd ACK: r60=%d r4=%d\n", (int)ra, (int)rb);
    if (ra <= 0) return -1;

    // Initial host ACK: 64 zero bytes sent to EP OUT before streaming starts.
    // Without this the device does not stream; same requirement as full dump.
    memset(s_tx, 0, GBOP_PKT_SIZE);
    DCFlushRange(s_tx, GBOP_PKT_SIZE);
    s32 wack = USB_WriteBlkMsg(dev->fd, dev->ep_out, GBOP_PKT_SIZE, s_tx);
    lprintf("[hdr] host ACK: w=%d\n", (int)wack);
    if (wack < 0) return -1;

    // Device ready-to-stream response: 60-byte read then 4-byte read.
    DCInvalidateRange(s_rx, GBOP_PAYLOAD_SIZE);
    s32 r1 = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
    DCInvalidateRange(s_crc, sizeof(s_crc));
    s32 r2 = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
    lprintf("[hdr] ready resp: r1=%d r2=%d\n", (int)r1, (int)r2);
    if (r1 <= 0) return -1;

    // GBA-style (r1==16): drain the remaining 3 zero packets from the IOS queue.
    // GB/GBC-style (r1==60): ROM data begins immediately — no drain.
    if (r1 == 16) {
        for (int i = 0; i < 3; i++) {
            DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
            s32 dr = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
            lprintf("[hdr] drain[%d]: rd=%d\n", i, (int)dr);
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

int gbop_dump_rom(GBOperatorHandle handle, const CartInfo *info,
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

        // Command ACK: same 60+4 format as other command ACKs.
        // The 4-byte trailing read may return -7008 if no trailing packet — that's fine.
        DCInvalidateRange(s_rx, GBOP_PAYLOAD_SIZE);
        s32 ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
        DCInvalidateRange(s_crc, sizeof(s_crc));
        s32 rb = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
        lprintf("[gbop] ROM dump cmd ACK: r60=%d r4=%d [0..3]=%02X %02X %02X %02X\n",
                (int)ra, (int)rb,
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
        DCInvalidateRange(s_rx, GBOP_PAYLOAD_SIZE);
        s32 r1 = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
        DCInvalidateRange(s_crc, sizeof(s_crc));
        s32 r2 = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
        lprintf("[gbop] Host ACK resp: r60=%d r4=%d [0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                (int)r1, (int)r2,
                s_rx[0], s_rx[1], s_rx[2], s_rx[3],
                s_rx[4], s_rx[5], s_rx[6], s_rx[7]);
        if (r1 <= 0) return -1;

        // GBA (r1==16): initial host ACK triggers an extended device "ready" response —
        // 84 bytes across 4 USB packets (16+4+60+4, all zeros). The r60 read above
        // returned rd=16 (short packet); the remaining 3 packets are queued in the
        // IOS receive queue and must be drained before ROM data begins.
        //
        // GB/GBC (r1==60): ready response is only the 60+4 bytes already read above.
        // ROM streaming begins immediately with the next USB_ReadBlkMsg — no drain.
        // Draining here for GB/GBC consumes the first 3×60 = 180 bytes of ROM data,
        // causing a 3-chunk offset in dump_chunk_cnt and a stall at chunk 317.
        if (r1 == 16) {
            for (int i = 0; i < 3; i++) {
                DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
                s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
                lprintf("[gbop] drain[%d]: rd=%d [0..3]=%02X %02X %02X %02X\n",
                        i, (int)rd, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
            }
        }

        dev->dump_total         = total;
        dev->dump_given         = 0;
        dev->dump_chunk_cnt     = 0;
        dev->dump_iter_cnt      = 0;
        dev->dump_rx_bytes      = 0;
        dev->dump_pending_drain = 0;
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
    // On Wii: both drain reads are device protocol overhead (ACK handshake), never ROM data.
    // drain[0] = rd=60 all-zeros (device ACK), drain[1] = rd=4 all-zeros (ACK part 2).
    // Both are always discarded. Writing drain[0] to file injects 60 corrupt bytes at every
    // 16KB boundary (1023 times in a 16MB GBA ROM = 61,380 bytes of corruption).

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
            // Always discard: drain[0]=rd=60 zeros, drain[1]=rd=4 zeros — both are
            // device ACK handshake packets, not ROM data.
        }
        if (written >= buffer_size) break;

        DCInvalidateRange(s_rx, GBOP_PKT_SIZE);
        s32 rd = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PKT_SIZE, s_rx);
        if (rd <= 0) {
            lprintf("[gbop] ROM read error: %d at iter %u chunk %u\n",
                    (int)rd, dev->dump_iter_cnt, dev->dump_chunk_cnt);
            dev->dump_active = 0;
            return -1;
        }

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
        // drain reads (drain[0]=ROM chunk, drain[1]=rd=4 device response) to the
        // top of the next iteration. Total per cycle = 320 iter + 2 drain = 322
        // USB reads, matching the device's rd=4 interval exactly.
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

/* Removed sd_commit (fclose+fopen) — it stalls USB bulk transfers by 200-500ms,
 * causing the GB Operator to stall the IN endpoint and blocking USB_ReadBlkMsg(60)
 * indefinitely.  fflush is used instead: fast (stdio buffer only, no SD sector
 * commit), no IOS contention, and safe to call from a background thread. */
#define log_flush_safe() do { if (g_log) fflush(g_log); } while (0)

int gbop_read_save(GBOperatorHandle handle, const CartInfo *info,
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

    // Command ACK — same 60+4 format as all other commands.
    // The 4-byte footer read may return -7008 if no footer packet arrives (OK).
    DCInvalidateRange(s_rx, GBOP_PAYLOAD_SIZE);
    s32 ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
    DCInvalidateRange(s_crc, sizeof(s_crc));
    s32 rb = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
    lprintf("[gbop] Save cmd ACK: r60=%d r4=%d [0..3]=%02X %02X %02X %02X\n",
            (int)ra, (int)rb, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
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

int gbop_write_save(GBOperatorHandle handle, const CartInfo *info,
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

    // Command ACK — same 60+4 format as all other commands
    DCInvalidateRange(s_rx, GBOP_PAYLOAD_SIZE);
    s32 ra = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
    DCInvalidateRange(s_crc, sizeof(s_crc));
    s32 rb = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
    lprintf("[gbop] Write cmd ACK: r60=%d r4=%d [0..3]=%02X %02X %02X %02X\n",
            (int)ra, (int)rb,
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

        // Per-chunk ACK: 60-byte read accepts any short packet (4, 16, or 60 bytes).
        // Followed by a 4-byte gap read that always returns -7008 (device idle).
        // This exact pattern (r60 + r4) is confirmed working for all 2048 chunks
        // in test_48. Do NOT change to USB_ReadBlkMsg(4) for the ACK — packet size
        // varies (chunk 0 = 16 bytes, chunks 1+ = 4 bytes) and the 60-byte read
        // handles all sizes without protocol disruption.
        {
            DCInvalidateRange(s_rx, GBOP_PAYLOAD_SIZE);
            s32 rack60 = USB_ReadBlkMsg(dev->fd, dev->ep_in, GBOP_PAYLOAD_SIZE, s_rx);
            DCInvalidateRange(s_crc, sizeof(s_crc));
            s32 rack4  = USB_ReadBlkMsg(dev->fd, dev->ep_in, 4, s_crc);
            if (chunk_cnt <= 5 || rack60 < 0) {
                lprintf("[gbop] write ACK[%u] r60=%d r4=%d [0..3]=%02X %02X %02X %02X\n",
                        chunk_cnt - 1, (int)rack60, (int)rack4,
                        s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
            }
            if (rack60 < 0) {
                lprintf("[gbop] write ACK error at chunk %u: r60=%d\n",
                        chunk_cnt - 1, (int)rack60);
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

void gbop_close(GBOperatorHandle handle) {
    if (!handle) return;
    GBOpDevice *dev = (GBOpDevice *)handle;
    USB_CloseDevice(&dev->fd);
    free(dev);
}
