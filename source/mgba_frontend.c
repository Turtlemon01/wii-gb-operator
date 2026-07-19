#include "mgba_frontend.h"
#include "cart_sync.h"
#include "gb_operator.h"
#include "log.h"
#include "settings.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>

/* libogc */
#include <gccore.h>
#include <ogc/pad.h>
#include <ogc/audio.h>
#include <ogc/machine/processor.h>

/* mGBA internal headers for GBAAudioCalculateRatio */
#include <mgba/internal/gba/audio.h>
/* GB model enum (GB_MODEL_SGB etc.) and GBValidModels */
#include <mgba/gb/interface.h>
/* mCoreConfigSetValue */
#include <mgba/core/config.h>

/* mGBA core API */
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

/* -----------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------- */
#define GBA_W           240
#define GBA_H           160
#define GB_W            160
#define GB_H            144
/* SGB / custom GBC border frame: 256×224 */
#define SGB_W           256
#define SGB_H           224
/* Offset of the 160×144 game area within the 256×224 GBC border frame */
#define BORDER_GAME_X    48
#define BORDER_GAME_Y    40

/* Custom GBA border frame: 320×240 (40px margin around the 240×160 game) */
#define GBA_BORDER_W        320
#define GBA_BORDER_H        240
#define GBA_BORDER_GAME_X    40
#define GBA_BORDER_GAME_Y    40

/* Video buffer must be large enough for the largest possible output.
 * GBA: 240×160=38400  SGB: 256×224=57344  ← SGB is the max. */
#define MAX_GAME_W      256
#define MAX_GAME_H      224

/* TEX_W must be a power of 2 and ≥ the widest content rendered (GBA border: 320).
 * 512 covers GBA-border (320), SGB-border (256), and bare GBA/GB frames (≤256). */
#define TEX_W           512
#define TEX_H           256

#define AUDIO_RATE      48000
#define AUDIO_SAMPLES   512   /* samples per DSP DMA transfer (≈10.7ms) */
#define AUDIO_BUFFERS   8     /* number of DMA double-buffers */

/* Joystick dead-zone */
#define STICK_THRESHOLD 32

/* Default output scale (1.0 = fill screen, <1.0 = smaller centered image) */
#define DEFAULT_SCALE   0.8f

/* Frames to show a green "saved" indicator after cart sync success */
#define SAVE_INDICATOR_FRAMES 180

#define AUDIO_LOG_PATH "sd:/apps/wii-gb-operator/audio_debug.txt"

/* -----------------------------------------------------------------------
 * Globals — video
 * --------------------------------------------------------------------- */
extern void       *xfb;
extern GXRModeObj *rmode;
static GXRModeObj *s_rmode    = NULL;

static void       *s_xfb_alt  = NULL;  /* second XFB for double-buffering */
static int         s_xfb_cur  = 0;     /* index into s_xfb_bufs[] of displayed buffer */

static GXTexObj    s_texobj;
static u8          s_texbuf[TEX_W * TEX_H * 2] ATTRIBUTE_ALIGN(32);
static Mtx         s_view;
static Mtx44       s_proj;
#define GX_FIFO_SIZE (256 * 1024)
/* Fixed-address static buffer for the GX CP FIFO.  Must NOT be a heap allocation:
 * the CP stays armed to this address between sessions, so if it were freed and
 * reallocated the CP would read heap data as GX commands → machine check exception.
 * section(".data") gives it a stable address independent of heap state. */
static uint8_t     s_gxfifo_buf[GX_FIFO_SIZE] __attribute__((aligned(32), section(".data")));
/* Guards the single-call-per-lifetime GX_Init(). Non-static for visibility if needed. */
bool               g_gx_initialized = false;

static color_t     s_vbuf[MAX_GAME_W * MAX_GAME_H] ATTRIBUTE_ALIGN(32);

static float       s_scale    = DEFAULT_SCALE;

/* Custom GBC border: 256×224 RGB565 image loaded from SD.
 * When active, the 160×144 CGB game is composited into the center
 * (offset BORDER_GAME_X, BORDER_GAME_Y) of the border frame each frame.
 * File: sd:/apps/wii-gb-operator/borders/gbc/border_CODE.bmp (24/32-bit BMP) */
static uint16_t   *s_border_buf    = NULL;
static bool        s_border_active = false;

/* Custom GBA border: 320×240 RGB565 image loaded from SD.
 * When active, the 240×160 GBA game is composited at offset (40,40).
 * File: sd:/apps/wii-gb-operator/borders/gba/border_CODE.bmp (24/32-bit BMP) */
static uint16_t   *s_gba_border_buf    = NULL;
static bool        s_gba_border_active = false;

/* GX render dimensions — equal to s_game_w/h normally, or SGB_W/H when
 * a custom border is composited over a 160×144 CGB game. */
static unsigned    s_render_w = GBA_W;
static unsigned    s_render_h = GBA_H;

/* -----------------------------------------------------------------------
 * Globals — retrace counter for VSync throttle (matches official Wii port)
 * --------------------------------------------------------------------- */
static volatile u32 s_retrace_count     = 0;
static          u32 s_reference_retrace = 0;

static void retrace_callback(u32 count) {
    u32 level = 0;
    _CPU_ISR_Disable(level);
    s_retrace_count = count;
    _CPU_ISR_Restore(level);
}

/* -----------------------------------------------------------------------
 * Globals — audio (raw DSP DMA, matches official Wii port)
 * --------------------------------------------------------------------- */
struct AudioBuf {
    struct mStereoSample samples[AUDIO_SAMPLES] __attribute__((aligned(32)));
    volatile size_t size;
};
static struct AudioBuf   s_audiobuf[AUDIO_BUFFERS];
static volatile int      s_audio_cur  = 0;  /* buffer DMA is currently playing */
static volatile int      s_audio_next = 0;  /* buffer the producer is filling */

static FILE            *s_audio_log     = NULL;
static uint64_t         s_frame_count   = 0;

/* -----------------------------------------------------------------------
 * Globals — emulation
 * --------------------------------------------------------------------- */
static struct mCore  *s_core      = NULL;
static volatile int   s_exit_flag = 0;
static unsigned       s_game_w    = GBA_W;
static unsigned       s_game_h    = GBA_H;
static uint32_t       s_save_size = 0;

/* Grace period: suppress cart sync for this many frames after emulation starts.
 * Prevents the initial savedataUpdated (fired during core reset / save load) from
 * triggering a cart write before the player has done anything.
 * Set to 120 (~2 s at 60 fps) before s_core->reset(). */
static int            s_save_grace_frames = 0;

/* Save indicator: local timer so the green dot shows for the full window
 * even if cart_sync immediately starts another write. */
static int            s_save_ok_timer = 0;

/* Snapshot of save data at last confirmed player save (or at load time).
 * Used to distinguish real saves from background SRAM writes (RTC ticks,
 * game-state flags, etc.) — compare diff byte count against a threshold. */
static void          *s_sync_snapshot    = NULL;
static size_t         s_sync_snapshot_sz = 0;

/* Set when a real in-game save is detected via snapshot comparison.
 * Controls: SD write on teardown (only if true), and quit-without-save warning. */
static volatile int   s_player_saved_ingame = 0;

/* Rate-limit snapshot comparisons to once per 30 frames (0.5 s) to avoid
 * frequent malloc/memcpy pairs from causing audio jitter. */
static uint64_t       s_last_save_check_frame = 0;

/* True when the current cart is CGB (Game Boy Color).
 * Used to select the save-diff threshold: CGB games have RTC background writes
 * so we filter at 64 bytes; DMG GB and GBA only write SRAM on explicit save. */
static bool           s_is_cgb = false;

/* CartInfo for the current session — used to check cart type in indicator/OSD. */
static const CartInfo *s_info = NULL;

/* 1 = at least one successful cart sync completed this session. */
static volatile int   s_synced_this_session = 0;

/* Frames remaining to suppress mGBA's default SGB border art.
 * mGBA shows its own "Game Boy" graphic until the game uploads its custom
 * border via SGB commands (~1-2 s into boot). We black-out the outer ring
 * so only the game area is visible until the game's border arrives.
 * Set to 300 (~5 s at 60 fps) for pure-SGB games; 0 otherwise. */
static int            s_sgb_suppress_border = 0;

/* -----------------------------------------------------------------------
 * Crash-safe checkpoint (used only during mGBA reset, cleared after)
 * --------------------------------------------------------------------- */
void (*g_mgba_checkpoint)(const char *msg) = NULL;
static void do_mgba_checkpoint(const char *msg) {
    lprintf("%s\n", msg);
    log_force_flush();
}

/* -----------------------------------------------------------------------
 * mGBA internal logger — redirect away from stdout
 * --------------------------------------------------------------------- */
static void wii_mlog(struct mLogger *logger, int category, enum mLogLevel level,
                     const char *format, va_list args) {
    (void)logger; (void)category;
    if (!(level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN))) return;
    if (g_log) {
        fprintf(g_log, "[mGBA] ");
        vfprintf(g_log, format, args);
        fputc('\n', g_log);
    }
}
static struct mLogger s_wii_logger = { .log = wii_mlog, .filter = NULL };

/* -----------------------------------------------------------------------
 * Reset / Power buttons
 * --------------------------------------------------------------------- */
static void on_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; s_exit_flag = 1; }
static void on_power(void) {
    lprintf("[sys] Power button callback fired\n");
    if (g_log) fclose(g_log);  /* force SD commit before poweroff */
    SYS_ResetSystem(SYS_POWEROFF_STANDBY, 0, 0);
}

/* -----------------------------------------------------------------------
 * DSP DMA callback — fires when current DMA transfer completes.
 * Advances to the next filled buffer and starts a new DMA transfer.
 * --------------------------------------------------------------------- */
static void audio_dma_callback(void) {
    struct AudioBuf *buf = &s_audiobuf[s_audio_cur];
    if (buf->size != AUDIO_SAMPLES) return;
    DCFlushRange(buf->samples, AUDIO_SAMPLES * sizeof(struct mStereoSample));
    AUDIO_InitDMA((u32)buf->samples, AUDIO_SAMPLES * sizeof(struct mStereoSample));
    buf->size = 0;
    s_audio_cur = (s_audio_cur + 1) % AUDIO_BUFFERS;
}

/* mAVStream hook — called by mGBA core after each runFrame.
 * Drains blip_buf output into DSP DMA buffers (L/R reversed — matches official port). */
static void post_audio_buffer(struct mAVStream *stream, struct blip_t *left, struct blip_t *right) {
    (void)stream;
    u32 level = 0;
    _CPU_ISR_Disable(level);
    struct AudioBuf *buf = &s_audiobuf[s_audio_next];
    int avail = blip_samples_avail(left);
    if ((int)buf->size + avail > AUDIO_SAMPLES)
        avail = AUDIO_SAMPLES - (int)buf->size;
    if (avail > 0) {
        blip_read_samples(left,  &buf->samples[buf->size].right, avail, true);
        blip_read_samples(right, &buf->samples[buf->size].left,  avail, true);
        buf->size += avail;
    }
    if ((int)buf->size == AUDIO_SAMPLES) {
        int next = (s_audio_next + 1) % AUDIO_BUFFERS;
        if ((s_audio_cur + AUDIO_BUFFERS - next) % AUDIO_BUFFERS != 1)
            s_audio_next = next;
        if (!AUDIO_GetDMAEnableFlag()) {
            audio_dma_callback();
            AUDIO_StartDMA();
        }
    }
    _CPU_ISR_Restore(level);
}

static struct mAVStream s_av_stream = {
    .videoDimensionsChanged = NULL,
    .postVideoFrame         = NULL,
    .postAudioFrame         = NULL,
    .postAudioBuffer        = post_audio_buffer,
};

/* Write audio diagnostic stats every 60 frames. */
static void audio_log_commit(struct mCore *core) {
    if (!s_audio_log) return;
    struct blip_t *left = core->getAudioChannel(core, 0);
    int avail = left ? blip_samples_avail(left) : -1;
    fprintf(s_audio_log, "%llu,%d,%d,%d\n",
            (unsigned long long)s_frame_count,
            avail, (int)s_audiobuf[s_audio_next].size,
            (int)AUDIO_GetDMAEnableFlag());
    fflush(s_audio_log);
}

/* -----------------------------------------------------------------------
 * Sync-success callback — called on the sync thread after cart+SD write.
 * Updates snapshot so future diffs are relative to the last committed state.
 * Safe to call from sync thread: on single-core Wii, context switches to
 * main thread only happen at blocking syscalls, not during runFrame.
 * --------------------------------------------------------------------- */
static void on_cart_sync_success(const void *buf, uint32_t sz) {
    if (s_sync_snapshot && s_sync_snapshot_sz == sz)
        memcpy(s_sync_snapshot, buf, sz);
    s_synced_this_session = 1;
    s_player_saved_ingame = 1;
    lprintf("[mgba] Snapshot updated after cart sync\n");
}

/* -----------------------------------------------------------------------
 * Save callback — fires on any save-memory write inside runFrame.
 * --------------------------------------------------------------------- */
static void on_save_data_updated(void *ctx) {
    (void)ctx;
    if (!s_core || s_save_size == 0) return;
    if (s_save_grace_frames > 0) return;
    if (s_frame_count - s_last_save_check_frame < 30) return;
    s_last_save_check_frame = s_frame_count;

    void *ptr = NULL;
    size_t sz = s_core->savedataClone(s_core, &ptr);
    if (ptr && sz > 0) {
        uint32_t upload_sz = (sz < s_save_size) ? (uint32_t)sz : s_save_size;
        bool is_gba = (s_core->platform(s_core) == mPLATFORM_GBA);

        uint32_t diff = 0;
        if (s_sync_snapshot && s_sync_snapshot_sz > 0) {
            uint32_t cmp_sz = upload_sz < (uint32_t)s_sync_snapshot_sz
                              ? upload_sz : (uint32_t)s_sync_snapshot_sz;
            const uint8_t *a = (const uint8_t *)ptr;
            const uint8_t *b = (const uint8_t *)s_sync_snapshot;
            for (uint32_t i = 0; i < cmp_sz; i++)
                if (a[i] != b[i]) diff++;
        } else {
            diff = upload_sz;
        }

        /* GBA: any byte changed = explicit Flash write = real save.
         * GB/GBC: RTC and counter writes are ~16–200 bytes per 30-frame window;
         * a real in-game save rewrites entire save sections (≥3584 bytes for Pokémon). */
        uint32_t threshold = is_gba ? 1 : 512;
        bool is_real_save = (diff >= threshold);
        lprintf("[mgba] savedataUpdated: diff=%u/%u (thr=%u) → %s\n",
                diff, upload_sz, threshold, is_real_save ? "SAVE" : "skip");

        if (is_real_save) {
            s_player_saved_ingame = 1;
            if (is_gba) {
                cart_sync_queue(ptr, upload_sz);
                lprintf("[mgba] savedataUpdated: GBA auto-sync queued %u bytes\n", upload_sz);
            }
        }
        /* Rolling snapshot: advance after every check so RTC bytes don't accumulate
         * past the threshold and cause false positives over long sessions. */
        if (s_sync_snapshot && s_sync_snapshot_sz == upload_sz)
            memcpy(s_sync_snapshot, ptr, upload_sz);
    }
    if (ptr) free(ptr);
}

/* -----------------------------------------------------------------------
 * GX video
 * --------------------------------------------------------------------- */
static void gx_alloc(GXRModeObj *rm) {
    if (!s_xfb_alt)
        s_xfb_alt = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rm));
}

static void gx_setup(GXRModeObj *rm) {
    s_rmode = rm;
    /* GX_Init reinitialises the CP hardware and must only be called once.
     * On successive sessions the CP is still armed to s_gxfifo_buf; calling
     * GX_Init again on an armed CP causes intermittent crashes / red-screen. */
    if (!g_gx_initialized) {
        GX_Init(s_gxfifo_buf, GX_FIFO_SIZE);
        g_gx_initialized = true;
    }

    GXColor bg = {0, 0, 0, 0xff};
    GX_SetCopyClear(bg, 0x00ffffff);
    GX_SetCopyFilter(rm->aa, rm->sample_pattern, GX_TRUE, rm->vfilter);

    GX_SetViewport(0, 0, rm->fbWidth, rm->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rm->fbWidth, rm->efbHeight);
    f32 yscale = GX_GetYScaleFactor(rm->efbHeight, rm->xfbHeight);
    u32 xfbH   = GX_SetDispCopyYScale(yscale);
    GX_SetDispCopySrc(0, 0, rm->fbWidth, rm->efbHeight);
    GX_SetDispCopyDst(rm->fbWidth, xfbH);
    GX_SetFieldMode(rm->field_rendering,
                    ((rm->viHeight == 2 * rm->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetNumTevStages(1);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

    guOrtho(s_proj, 1, -1, -1, 1, 0, 100);
    GX_LoadProjectionMtx(s_proj, GX_ORTHOGRAPHIC);
    guMtxIdentity(s_view);
    GX_LoadPosMtxImm(s_view, GX_PNMTX0);

    GX_InitTexObj(&s_texobj, s_texbuf, TEX_W, TEX_H,
                  GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    /* Nearest-neighbor when a pixel-art border is composited; bilinear otherwise */
    u8 filter = (s_border_active || s_gba_border_active) ? GX_NEAR : GX_LINEAR;
    GX_InitTexObjFilterMode(&s_texobj, filter, filter);
}

static void gx_init(GXRModeObj *rm) { gx_alloc(rm); gx_setup(rm); }

/* Load a BMP border image (must be exactly exp_w × exp_h).
 * Accepts 8bpp indexed BI_RGB, 24bpp BI_RGB, and 32bpp BI_BITFIELDS/BI_RGB.
 * Returns a malloc'd RGB565 buffer (exp_w × exp_h × 2 bytes) or NULL. */
static uint16_t *load_border_bmp(const char *path, int exp_w, int exp_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f); return NULL;
    }

    /* Parse header with byte reads to avoid alignment issues */
    uint32_t data_off = (uint32_t)hdr[10] | ((uint32_t)hdr[11]<<8) |
                        ((uint32_t)hdr[12]<<16) | ((uint32_t)hdr[13]<<24);
    int32_t  bmp_w    = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19]<<8) |
                                  ((uint32_t)hdr[20]<<16) | ((uint32_t)hdr[21]<<24));
    int32_t  bmp_h    = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23]<<8) |
                                  ((uint32_t)hdr[24]<<16) | ((uint32_t)hdr[25]<<24));
    uint16_t bpp      = (uint16_t)(hdr[28] | ((uint16_t)hdr[29]<<8));
    uint32_t comp     = (uint32_t)hdr[30] | ((uint32_t)hdr[31]<<8) |
                        ((uint32_t)hdr[32]<<16) | ((uint32_t)hdr[33]<<24);

    bool    flip  = (bmp_h > 0); /* positive h = bottom-up storage (standard BMP) */
    int32_t abs_h = (bmp_h < 0) ? -bmp_h : bmp_h;

    /* 8bpp: indexed color with 256-entry RGBQUAD palette (Aseprite default export) */
    bool ok8  = (bpp == 8  && comp == 0);
    bool ok24 = (bpp == 24 && comp == 0);
    /* 32bpp: BI_RGB(0), BI_BITFIELDS(3), BI_ALPHABITFIELDS(6) — all store BGRA/BGRX */
    bool ok32 = (bpp == 32 && (comp == 0 || comp == 3 || comp == 6));
    if (bmp_w != exp_w || abs_h != exp_h || (!ok8 && !ok24 && !ok32)) {
        lprintf("[border] %s: need %dx%d 8/24/32bpp; got %dx%d bpp=%u comp=%u\n",
                path, exp_w, exp_h, bmp_w, abs_h, bpp, comp);
        fclose(f); return NULL;
    }

    /* For 8bpp: read the 256-entry color table that follows the DIB header.
     * Standard BITMAPINFOHEADER is 40 bytes; palette starts at offset 14+40=54.
     * Each RGBQUAD entry is 4 bytes: Blue, Green, Red, Reserved. */
    uint8_t palette[256 * 4];
    if (ok8) {
        fseek(f, 54, SEEK_SET);
        fread(palette, 1, sizeof(palette), f);
    }

    uint16_t *out = (uint16_t *)malloc(exp_w * exp_h * sizeof(uint16_t));
    if (!out) { fclose(f); return NULL; }

    int bytes_pp  = ok32 ? 4 : ok8 ? 1 : 3;
    int row_bytes = (exp_w * bytes_pp + 3) & ~3;
    uint8_t *row_buf = (uint8_t *)malloc(row_bytes);
    if (!row_buf) { free(out); fclose(f); return NULL; }

    fseek(f, data_off, SEEK_SET);
    for (int y = 0; y < exp_h; y++) {
        if ((int)fread(row_buf, 1, row_bytes, f) < row_bytes) break;
        int dst_y = flip ? (exp_h - 1 - y) : y;
        for (int x = 0; x < exp_w; x++) {
            uint8_t b, g, r;
            if (ok8) {
                uint8_t idx = row_buf[x];
                b = palette[idx * 4 + 0]; /* RGBQUAD stores BGR */
                g = palette[idx * 4 + 1];
                r = palette[idx * 4 + 2];
            } else {
                b = row_buf[x * bytes_pp + 0]; /* 24/32bpp: BGR */
                g = row_buf[x * bytes_pp + 1];
                r = row_buf[x * bytes_pp + 2]; /* byte 3 = alpha/pad, ignored */
            }
            out[dst_y * exp_w + x] = (uint16_t)(((r >> 3) << 11) |
                                                 ((g >> 2) <<  5) |
                                                  (b >> 3));
        }
    }

    free(row_buf);
    fclose(f);
    lprintf("[border] Loaded: %s (%dbpp)\n", path, bpp);
    return out;
}

/* Tile-swizzle frame into GX texture.
 * When s_border_active: composites the 160×144 CGB game (in s_vbuf at
 * stride s_game_w) into the center of the 256×224 border frame.
 * w/h are the render dimensions passed by the main loop (s_render_w/h). */
static void gx_upload_frame(unsigned w, unsigned h) {
    const uint16_t *src = (const uint16_t *)s_vbuf;
    uint16_t *dst = (uint16_t *)s_texbuf;
    for (unsigned ty = 0; ty < TEX_H; ty += 4) {
        for (unsigned tx = 0; tx < TEX_W; tx += 4) {
            for (unsigned row = 0; row < 4; row++) {
                unsigned y = ty + row;
                for (unsigned col = 0; col < 4; col++) {
                    unsigned x = tx + col;
                    uint16_t px;
                    if (s_gba_border_active) {
                        int gx = (int)x - GBA_BORDER_GAME_X;
                        int gy = (int)y - GBA_BORDER_GAME_Y;
                        if (gx >= 0 && gx < GBA_W && gy >= 0 && gy < GBA_H) {
                            px = src[gy * GBA_W + gx]; /* game pixel */
                        } else if (x < (unsigned)GBA_BORDER_W && y < (unsigned)GBA_BORDER_H) {
                            px = s_gba_border_buf[y * GBA_BORDER_W + x]; /* border pixel */
                        } else {
                            px = 0;
                        }
                    } else if (s_border_active) {
                        int gx = (int)x - BORDER_GAME_X;
                        int gy = (int)y - BORDER_GAME_Y;
                        if (gx >= 0 && gx < (int)s_game_w &&
                            gy >= 0 && gy < (int)s_game_h) {
                            px = src[gy * (int)s_game_w + gx]; /* game pixel */
                        } else if (x < (unsigned)SGB_W && y < (unsigned)SGB_H) {
                            px = s_border_buf[y * SGB_W + x]; /* border pixel */
                        } else {
                            px = 0;
                        }
                    } else {
                        px = (x < w && y < h) ? src[y * w + x] : 0;
                    }
                    *dst++ = px;
                }
            }
        }
    }
    DCFlushRange(s_texbuf, sizeof(s_texbuf));
    GX_InvalidateTexAll();
}

/* Draw textured quad; s_scale==0 fills screen, else multiplies fill size. */
static void gx_draw_frame(unsigned w, unsigned h) {
    float us = (float)w / TEX_W;
    float vs = (float)h / TEX_H;

    float game_ar = (float)w / (float)h;
    /* xfbHeight (480 for NTSC 480i) is the physical TV height after Y-scale;
     * efbHeight (240) is only one EFB field and gives wrong AR 640/240=2.67. */
    float tv_ar   = (float)s_rmode->fbWidth / (float)s_rmode->xfbHeight;
    float sx, sy;
    if (game_ar > tv_ar) { sx = 1.0f; sy = tv_ar / game_ar; }
    else                  { sy = 1.0f; sx = game_ar / tv_ar; }

    if (s_scale != 0.0f) { sx *= s_scale; sy *= s_scale; }

    GX_LoadTexObj(&s_texobj, GX_TEXMAP0);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(-sx,  sy); GX_TexCoord2f32(0,  0);
        GX_Position2f32( sx,  sy); GX_TexCoord2f32(us, 0);
        GX_Position2f32( sx, -sy); GX_TexCoord2f32(us, vs);
        GX_Position2f32(-sx, -sy); GX_TexCoord2f32(0,  vs);
    GX_End();
}

/* Draw 8×8 coloured dot in top-right corner of s_vbuf (before tiling).
 * Maintains an independent local timer so SUCCESS is visible for the full
 * SAVE_INDICATOR_FRAMES window even if cart_sync immediately starts a new write. */
static void draw_save_indicator(void) {
    /* Only show the indicator for GBA (auto-sync). GB/GBC use manual OSD sync. */
    if (!s_info || s_info->type != CART_TYPE_GBA) { cart_sync_state(); return; }
    CartSyncState st = cart_sync_state();
    uint16_t color = 0;

    if (st == CART_SYNC_SUCCESS) {
        /* Latch green timer; don't let a racing IN_PROGRESS clobber it */
        s_save_ok_timer = SAVE_INDICATOR_FRAMES;
    }

    if (s_save_ok_timer > 0) {
        /* Green takes priority: show success even if another write started */
        color = 0x07E0; /* green */
        s_save_ok_timer--;
    } else if (st == CART_SYNC_IN_PROGRESS) {
        color = 0xFFE0; /* yellow */
    } else if (st == CART_SYNC_FAILED) {
        color = 0xF800; /* red */
    } else {
        return; /* IDLE, no dot */
    }

    /* In pure SGB mode, mGBA renders the 160×144 game area at offset
     * (BORDER_GAME_X, BORDER_GAME_Y) inside the 256×224 frame.  Drawing at
     * (s_game_w - 8, 0) would land in the SGB border region, which mGBA
     * redraws every frame — the dot would be invisible.  Place it in the
     * top-right corner of the actual game area instead. */
    int dot_x, dot_y;
    if (!s_border_active && s_game_w == (unsigned)SGB_W) {
        dot_x = BORDER_GAME_X + (int)GB_W - 8;
        dot_y = BORDER_GAME_Y;
    } else {
        dot_x = (int)s_game_w - 8;
        dot_y = 0;
    }
    for (int y = dot_y; y < dot_y + 8; y++)
        for (int x = dot_x; x < dot_x + 8; x++)
            s_vbuf[y * (int)s_game_w + x] = (color_t)color;
}

/* -----------------------------------------------------------------------
 * Input — GC controller → GBA keys
 * Z is reserved for OSD. X and Y both map to Select.
 * D-pad, left joystick, and C-stick all drive the GBA d-pad.
 * --------------------------------------------------------------------- */
static uint32_t read_gc_keys(void) {
    u16 held = PAD_ButtonsHeld(0);
    s8 ax    = PAD_StickX(0),    ay = PAD_StickY(0);
    s8 cx    = PAD_SubStickX(0), cy = PAD_SubStickY(0);

    uint32_t keys = 0;
    if (held & PAD_BUTTON_A)     keys |= (1 << 0); /* GBA A      */
    if (held & PAD_BUTTON_B)     keys |= (1 << 1); /* GBA B      */
    if ((held & PAD_BUTTON_X) || (held & PAD_BUTTON_Y)) keys |= (1 << 2); /* Select */
    if (held & PAD_BUTTON_START) keys |= (1 << 3); /* GBA Start  */

    if ((held & PAD_BUTTON_RIGHT) || ax >  STICK_THRESHOLD || cx >  STICK_THRESHOLD) keys |= (1 << 4);
    if ((held & PAD_BUTTON_LEFT)  || ax < -STICK_THRESHOLD || cx < -STICK_THRESHOLD) keys |= (1 << 5);
    if ((held & PAD_BUTTON_UP)    || ay >  STICK_THRESHOLD || cy >  STICK_THRESHOLD) keys |= (1 << 6);
    if ((held & PAD_BUTTON_DOWN)  || ay < -STICK_THRESHOLD || cy < -STICK_THRESHOLD) keys |= (1 << 7);

    if (held & PAD_TRIGGER_R)    keys |= (1 << 8); /* GBA R */
    if (held & PAD_TRIGGER_L)    keys |= (1 << 9); /* GBA L */
    return keys;
}

/* -----------------------------------------------------------------------
 * OSD — console-mode pause menu shown when Z is pressed
 * Pauses emulation and audio while open.
 * --------------------------------------------------------------------- */
static void osd_draw(int item) {
    printf("\x1b[2J\x1b[1;1H");
    printf("====  OPTIONS  ====\n\n");

    CartSyncState st = cart_sync_state();
    const char *save_str =
        (st == CART_SYNC_IN_PROGRESS)                   ? "Syncing..." :
        (st == CART_SYNC_FAILED)                         ? "FAILED (retrying)" :
        (st == CART_SYNC_SUCCESS || s_save_ok_timer > 0) ? "Complete" : "Idle";
    const char *session_str =
        !s_player_saved_ingame ? "Not saved yet" :
        s_synced_this_session  ? "Saved + Synced" : "Saved (not synced)";
    printf("Cart Save: %s  |  Session: %s\n\n", save_str, session_str);

    printf("%sSync to Cart\n", (item == 0) ? "> " : "  ");
    if (item == 0) printf("      Manually write save to cartridge now\n\n");
    else           printf("\n");

    printf("%sScale: ", (item == 1) ? "> " : "  ");
    if (s_scale == 0.0f) printf("Fill (aspect-correct)\n");
    else                 printf("%.1fx\n", (double)s_scale);
    if (item == 1) printf("      [Left] smaller  [Right] larger  [A] reset to Fill\n\n");
    else           printf("\n");

    printf("%sQuit to Menu\n\n", (item == 2) ? "> " : "  ");

    printf("[B] Resume  [Up/Down] Navigate\n");
}

static void osd_draw_exit_menu(int item) {
    printf("\x1b[2J\x1b[1;1H");
    printf("====  Exit  ====\n\n");
    printf("%sSync & Exit\n", (item == 0) ? "> " : "  ");
    if (item == 0) printf("      Write save to cart, then return to menu\n\n");
    else           printf("\n");
    printf("%sExit without sync\n", (item == 1) ? "> " : "  ");
    if (item == 1) printf("      Return to menu — save is backed up on SD\n\n");
    else           printf("\n");
    printf("%sCancel\n\n", (item == 2) ? "> " : "  ");
    printf("[A] Confirm  [Up/Down] Navigate  [B] Cancel\n");
}

static void osd_draw_syncing_exit(CartSyncState st) {
    printf("\x1b[2J\x1b[1;1H");
    printf("====  Syncing  ====\n\n");
    if (st == CART_SYNC_IN_PROGRESS)
        printf("Writing save to cart...\n\n");
    else if (st == CART_SYNC_SUCCESS)
        printf("Sync complete!\n\n");
    else
        printf("Sync failed — save is on SD.\n\n");
    printf("Please wait...\n");
}

static void osd_show(void) {
    AUDIO_StopDMA();

    GX_DrawDone();
    VIDEO_WaitVSync();
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    /* Wait for Z release to avoid immediate re-trigger */
    while (PAD_ButtonsHeld(0) & PAD_TRIGGER_Z) {
        PAD_ScanPads();
        VIDEO_WaitVSync();
    }

    int  item         = 0;
    bool running      = true;
    bool do_quit      = false;
    bool in_exit_menu = false;   /* GB/C 3-item exit sub-menu */
    int  exit_item    = 0;
    int  redraw_timer = 0;
    bool gba_mode     = (s_info && s_info->type == CART_TYPE_GBA);

    osd_draw(item);

    while (running) {
        PAD_ScanPads();
        u16 pressed = PAD_ButtonsDown(0);
        bool need_redraw = false;

        if (in_exit_menu) {
            /* 3-item scrollable exit menu (GB/C only) */
            if (pressed & PAD_BUTTON_UP)   { exit_item = (exit_item + 2) % 3; need_redraw = true; }
            if (pressed & PAD_BUTTON_DOWN) { exit_item = (exit_item + 1) % 3; need_redraw = true; }
            if ((pressed & PAD_BUTTON_B) || (exit_item == 2 && (pressed & PAD_BUTTON_A))) {
                /* Cancel — back to OSD */
                in_exit_menu = false;
                need_redraw  = true;
            } else if (pressed & PAD_BUTTON_A) {
                if (exit_item == 0) {
                    /* Sync & Exit: queue a sync, wait up to ~10 s, then quit */
                    if (s_core && s_save_size > 0) {
                        void *rawsave = NULL;
                        size_t rawsz = s_core->savedataClone(s_core, &rawsave);
                        if (rawsave && rawsz > 0) {
                            uint32_t upload_sz = (uint32_t)(rawsz < s_save_size
                                                            ? rawsz : s_save_size);
                            cart_sync_queue(rawsave, upload_sz);
                            s_player_saved_ingame = 1;
                            lprintf("[osd] Sync & Exit queued: %u bytes\n", upload_sz);
                        }
                        if (rawsave) free(rawsave);
                    }
                    /* Poll sync state with timeout */
                    int timeout = 600;  /* ~10 s at 60 fps */
                    CartSyncState st;
                    do {
                        st = cart_sync_state();
                        osd_draw_syncing_exit(st);
                        VIDEO_Flush();
                        VIDEO_WaitVSync();
                    } while (st == CART_SYNC_IN_PROGRESS && --timeout > 0);
                    do_quit = true;
                    running = false;
                } else if (exit_item == 1) {
                    /* Exit without sync */
                    do_quit = true;
                    running = false;
                }
            }
        } else {
            /* Main OSD */
            if (pressed & PAD_BUTTON_UP)   { item = (item + 2) % 3; need_redraw = true; }
            if (pressed & PAD_BUTTON_DOWN) { item = (item + 1) % 3; need_redraw = true; }

            if (item == 0 && (pressed & PAD_BUTTON_A)) {
                /* Sync to Cart — always available */
                if (s_core && s_save_size > 0) {
                    void *rawsave = NULL;
                    size_t rawsz = s_core->savedataClone(s_core, &rawsave);
                    if (rawsave && rawsz > 0) {
                        uint32_t upload_sz = (uint32_t)(rawsz < s_save_size
                                                        ? rawsz : s_save_size);
                        cart_sync_queue(rawsave, upload_sz);
                        s_player_saved_ingame = 1;
                        lprintf("[osd] Manual sync queued: %u bytes\n", upload_sz);
                    }
                    if (rawsave) free(rawsave);
                    need_redraw = true;
                }
            } else if (item == 1) {
                if (pressed & PAD_BUTTON_LEFT) {
                    if (s_scale > 0.1f + 0.001f)
                        s_scale = roundf((s_scale - 0.1f) * 10.0f) / 10.0f;
                    else
                        s_scale = 0.0f;
                    need_redraw = true;
                }
                if (pressed & PAD_BUTTON_RIGHT) {
                    if (s_scale == 0.0f) s_scale = 0.1f;
                    else if (s_scale < 3.0f - 0.001f)
                        s_scale = roundf((s_scale + 0.1f) * 10.0f) / 10.0f;
                    need_redraw = true;
                }
                if (pressed & PAD_BUTTON_A) { s_scale = 0.0f; need_redraw = true; }
            } else if (item == 2 && (pressed & PAD_BUTTON_A)) {
                if (!gba_mode) {
                    /* GB/C: show 3-item exit menu */
                    in_exit_menu = true;
                    exit_item    = 0;
                    need_redraw  = true;
                } else {
                    /* GBA: auto-sync handles saves — quit directly */
                    do_quit = true;
                    running = false;
                }
            }

            if (pressed & PAD_BUTTON_B) running = false;
        }

        /* Periodic redraw for sync-status updates */
        if (!in_exit_menu && ++redraw_timer >= 60) { redraw_timer = 0; need_redraw = true; }

        if (need_redraw && running) {
            if (in_exit_menu) osd_draw_exit_menu(exit_item);
            else              osd_draw(item);
        }

        VIDEO_WaitVSync();
    }

    if (do_quit) {
        printf("\x1b[2J\x1b[1;1H");
        printf("Returning to menu...\n");
        VIDEO_Flush();
        s_exit_flag = 1;
        return;
    }

    /* Resume: reinit GX and reset audio */
    gx_setup(rmode);
    memset(s_audiobuf, 0, sizeof(s_audiobuf));
    s_audio_cur = s_audio_next = 0;
    {
        u32 level = 0;
        _CPU_ISR_Disable(level);
        s_reference_retrace = s_retrace_count;
        _CPU_ISR_Restore(level);
    }

    PAD_ScanPads();
}

/* -----------------------------------------------------------------------
 * Public entry point
 * --------------------------------------------------------------------- */
int mgba_run(const CartInfo *info, const char *rom_path, const char *save_path,
             uint32_t save_kb) {
    s_info = info;
    lprintf("[mgba] ROM: %s\n", rom_path);
    if (save_path) lprintf("[mgba] Save: %s (%u KB)\n", save_path, save_kb);
    log_force_flush();

    /* Audio debug log — separate file, force-committed to SD every 60 frames */
    s_audio_log = fopen(AUDIO_LOG_PATH, "w");
    if (s_audio_log) {
        fprintf(s_audio_log, "frame,blip_avail,next_buf_fill,dma_running\n");
        fclose(s_audio_log);
        s_audio_log = fopen(AUDIO_LOG_PATH, "a");
    }
    lprintf("[mgba] Audio log: %s\n", s_audio_log ? "open" : "FAILED"); log_force_flush();

    /* Load ROM */
    FILE *rf = fopen(rom_path, "rb");
    if (!rf) { lprintf("[mgba] Cannot open ROM\n"); log_force_flush(); return -1; }
    fseek(rf, 0, SEEK_END); long rom_len = ftell(rf); rewind(rf);
    lprintf("[mgba] ROM malloc: %ld bytes\n", rom_len); log_force_flush();
    uint8_t *rom_data = malloc(rom_len);
    if (!rom_data) { fclose(rf); lprintf("[mgba] ROM malloc failed\n"); log_force_flush(); return -1; }
    fread(rom_data, 1, rom_len, rf);
    fclose(rf);
    lprintf("[mgba] ROM loaded: %ld bytes\n", rom_len); log_force_flush();

    /* Load save */
    s_save_size = save_kb * 1024;
    uint8_t *save_data = NULL;
    long save_len = 0;
    if (save_path && save_kb > 0) {
        FILE *sf = fopen(save_path, "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END); save_len = ftell(sf); rewind(sf);
            save_data = malloc(save_len);
            if (save_data) fread(save_data, 1, save_len, sf);
            fclose(sf);
            lprintf("[mgba] Save loaded: %ld bytes\n", save_len);
        } else {
            lprintf("[mgba] No save file, new game\n");
        }
    }
    log_force_flush();

    /* Create mGBA core */
    struct VFile *rom_vf = VFileFromMemory(rom_data, rom_len);
    if (!rom_vf) { free(rom_data); free(save_data); lprintf("[mgba] VFileFromMemory failed\n"); return -1; }

    s_core = mCoreFindVF(rom_vf);
    if (!s_core) {
        lprintf("[mgba] mCoreFindVF: unrecognised ROM\n");
        rom_vf->close(rom_vf); free(rom_data); free(save_data); return -1;
    }
    if (!s_core->init(s_core)) {
        lprintf("[mgba] core->init failed\n");
        /* Skip core->deinit: corrupts dlmalloc free list (tests 84-89). Leak is
         * acceptable; heap must stay intact for the next session. */
        s_core = NULL; free(rom_data); free(save_data); return -1;
    }
    mCoreInitConfig(s_core, "wii-gb-operator");
    mLogSetDefaultLogger(&s_wii_logger);
    lprintf("[mgba] Core initialised\n"); log_force_flush();

    /* Pre-reset: set a preliminary video buffer so _GBCoreReset can associate
     * the renderer.  Dimensions may be wrong here (model not yet detected);
     * we re-query after reset below and update the stride if needed. */
    s_core->desiredVideoDimensions(s_core, &s_game_w, &s_game_h);
    lprintf("[mgba] Video (pre-reset): %ux%u\n", s_game_w, s_game_h); log_force_flush();
    memset(s_vbuf, 0, sizeof(s_vbuf));
    s_core->setVideoBuffer(s_core, s_vbuf, s_game_w);

    s_core->setAudioBufferSize(s_core, AUDIO_SAMPLES);
    {
        /* Adjust blip output rate for actual NTSC timing — matches official Wii port */
        double ratio = GBAAudioCalculateRatio(1, 60.0 / 1.001, 1);
        struct blip_t *bl = s_core->getAudioChannel(s_core, 0);
        struct blip_t *br = s_core->getAudioChannel(s_core, 1);
        if (bl) blip_set_rates(bl, s_core->frequency(s_core), AUDIO_RATE * ratio);
        if (br) blip_set_rates(br, s_core->frequency(s_core), AUDIO_RATE * ratio);
    }
    s_core->setAVStream(s_core, &s_av_stream);

    rom_vf->seek(rom_vf, 0, SEEK_SET);
    if (!s_core->loadROM(s_core, rom_vf)) {
        lprintf("[mgba] loadROM failed\n");
        /* Skip core->deinit: corrupts dlmalloc free list. */
        s_core = NULL;
        free(rom_data); free(save_data); return -1;
    }

    if (save_data && save_len > 0) {
        struct VFile *sv = VFileFromMemory(save_data, save_len);
        if (sv) { s_core->loadSave(s_core, sv); lprintf("[mgba] Save loaded into core\n"); log_force_flush(); }
    }

    /* SGB / CGB detection — kept at function scope so the border loader below
     * can use has_sgb + is_cgb after the post-reset dimension query. */
    bool has_sgb = false, is_cgb = false;
    if (s_core->platform(s_core) == mPLATFORM_GB && rom_len >= 0x148) {
        uint8_t cgb_flag = rom_data[0x143];
        uint8_t sgb_flag = rom_data[0x146];
        has_sgb = (sgb_flag == 0x03);
        is_cgb  = (cgb_flag == 0x80 || cgb_flag == 0xC0);
        s_is_cgb = is_cgb;
        /* Pure SGB games (Blue): run in SGB mode — mGBA handles border rendering.
         * CGB+SGB games (Silver, Gold): run in CGB mode for full colour; a custom
         * static border can be composited from a BMP file on SD (see below). */
        if (has_sgb && !is_cgb) {
            mCoreConfigSetValue(&s_core->config, "sgb.model", "SGB");
            lprintf("[mgba] SGB mode enabled (cgb=0x%02X sgb=0x%02X)\n",
                    cgb_flag, sgb_flag);
        } else {
            lprintf("[mgba] GB model: cgb=0x%02X sgb=0x%02X → %s\n",
                    cgb_flag, sgb_flag,
                    is_cgb ? "CGB" : "DMG");
        }
    }

    struct mCoreCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.savedataUpdated = on_save_data_updated;
    s_core->addCoreCallbacks(s_core, &cbs);

    s_exit_flag              = 0;
    s_frame_count            = 0;
    s_save_ok_timer          = 0;
    s_save_grace_frames      = 120; /* suppress cart sync for first ~2 s */
    s_player_saved_ingame    = 0;
    s_last_save_check_frame  = 0;
    s_synced_this_session    = 0;
    s_scale = (info && info->type == CART_TYPE_GBA) ? g_settings.scale_gba : g_settings.scale_gb;

    /* Initialise snapshot from loaded save so the first diff is against the
     * actual on-disk state, not an empty buffer. */
    if (s_sync_snapshot) { free(s_sync_snapshot); s_sync_snapshot = NULL; }
    s_sync_snapshot_sz = 0;
    if (save_data && save_len > 0 && s_save_size > 0) {
        uint32_t copy_sz = (uint32_t)save_len < s_save_size
                           ? (uint32_t)save_len : s_save_size;
        s_sync_snapshot = malloc(s_save_size);
        if (s_sync_snapshot) {
            s_sync_snapshot_sz = s_save_size;
            memcpy(s_sync_snapshot, save_data, copy_sz);
            if (copy_sz < s_save_size)
                memset((uint8_t *)s_sync_snapshot + copy_sz, 0xFF, s_save_size - copy_sz);
        }
    }

    /* Suppress console vprintf during GX rendering: lprintf writes shared xfb and corrupts video */
    g_log_suppress_console = 1;
    memset(s_audiobuf, 0, sizeof(s_audiobuf));
    s_audio_cur = s_audio_next = 0;
    SYS_SetResetCallback(on_reset);
    SYS_SetPowerCallback(on_power);

    if (info && info->ram_size_kb > 0)
        cart_sync_init(info, save_path, on_cart_sync_success);
    log_force_flush();

    lprintf("[mgba] pre-reset\n"); log_force_flush();
    g_mgba_checkpoint = do_mgba_checkpoint;
    s_core->reset(s_core);
    g_mgba_checkpoint = NULL;
    lprintf("[mgba] post-reset\n"); log_force_flush();

    /* Post-reset: model is now detected.  Re-query dimensions and update the
     * video buffer stride in case they changed (e.g. SGB border mode). */
    {
        unsigned post_w = s_game_w, post_h = s_game_h;
        s_core->desiredVideoDimensions(s_core, &post_w, &post_h);
        lprintf("[mgba] Video (post-reset): %ux%u\n", post_w, post_h);
        if (post_w != s_game_w || post_h != s_game_h) {
            s_game_w = post_w;
            s_game_h = post_h;
            s_core->setVideoBuffer(s_core, s_vbuf, s_game_w);
        }
    }

    /* Clear video buffer to black: hides the mGBA default SGB border until
     * the game uploads its own border via SGB commands (~1-2 s into boot). */
    memset(s_vbuf, 0, sizeof(s_vbuf));

    /* For pure-SGB games (Blue), mGBA renders its default "Game Boy" border art
     * into s_vbuf on every runFrame until the game uploads its own border.
     * Counter s_sgb_suppress_border makes the main loop black-out the border ring
     * for the first 300 frames (~5 s) so only the game area is visible. */
    s_sgb_suppress_border = (has_sgb && !is_cgb) ? 300 : 0;

    /* Custom border for CGB+SGB games (Silver, Gold, Crystal, etc.).
     * File: sd:/apps/wii-gb-operator/borders/gbc/border_CODE.bmp
     * Must be 256×224 pixels, 24 or 32-bit BMP.
     * When loaded, the 160×144 CGB game is composited into the centre each frame. */
    if (s_border_buf) { free(s_border_buf); s_border_buf = NULL; }
    s_border_active = false;
    if (has_sgb && is_cgb && rom_data && rom_len >= 0x144) {
        /* Game code: ROM header bytes 0x13F–0x142 (4 uppercase ASCII chars).
         * Border file: sd:/apps/wii-gb-operator/borders/gbc/border_AAXE.bmp */
        char game_code[5] = {0};
        bool code_valid = true;
        for (int i = 0; i < 4; i++) {
            uint8_t c = rom_data[0x13F + i];
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                game_code[i] = (char)c;
            } else {
                code_valid = false;
                break;
            }
        }
        char border_path[256];
        if (code_valid && game_code[0]) {
            snprintf(border_path, sizeof(border_path),
                     "sd:/apps/wii-gb-operator/borders/gbc/border_%s.bmp", game_code);
        } else {
            /* Fallback: sanitised 11-byte ROM title */
            char rom_title[12] = {0};
            for (int i = 0; i < 11; i++) {
                uint8_t c = rom_data[0x134 + i];
                if (c < 0x20 || c > 0x7E) break;
                rom_title[i] = (char)c;
            }
            char safe[32] = {0};
            int j = 0;
            for (int i = 0; rom_title[i] && j < 31; i++) {
                char c = rom_title[i];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_')
                    safe[j++] = c;
                else if (c == ' ' && j > 0)
                    safe[j++] = '_';
            }
            snprintf(border_path, sizeof(border_path),
                     "sd:/apps/wii-gb-operator/borders/gbc/%s.bmp", safe);
        }
        lprintf("[border] GBC: Looking for: %s (code=\"%s\")\n", border_path, game_code);
        s_border_buf = load_border_bmp(border_path, SGB_W, SGB_H);
        if (s_border_buf) {
            s_border_active = true;
            lprintf("[border] GBC border active — rendering at %dx%d\n", SGB_W, SGB_H);
        }
    }

    /* Custom border for GBA games.
     * File: sd:/apps/wii-gb-operator/borders/gba/border_CODE.bmp
     * Must be 320×240 pixels, 24 or 32-bit BMP.
     * When loaded, the 240×160 GBA game is composited at offset (40,40). */
    if (s_gba_border_buf) { free(s_gba_border_buf); s_gba_border_buf = NULL; }
    s_gba_border_active = false;
    if (s_core->platform(s_core) == mPLATFORM_GBA && rom_data && rom_len >= 0xB0) {
        char gba_code[5] = {0};
        bool code_valid = true;
        for (int i = 0; i < 4; i++) {
            uint8_t c = rom_data[0xAC + i];
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                gba_code[i] = (char)c;
            } else {
                code_valid = false;
                break;
            }
        }
        if (code_valid && gba_code[0]) {
            char gba_border_path[256];
            snprintf(gba_border_path, sizeof(gba_border_path),
                     "sd:/apps/wii-gb-operator/borders/gba/border_%s.bmp", gba_code);
            lprintf("[border] GBA: Looking for: %s\n", gba_border_path);
            s_gba_border_buf = load_border_bmp(gba_border_path, GBA_BORDER_W, GBA_BORDER_H);
            if (s_gba_border_buf) {
                s_gba_border_active = true;
                lprintf("[border] GBA border active — rendering at %dx%d\n",
                        GBA_BORDER_W, GBA_BORDER_H);
            }
        }
    }

    /* If no file snapshot was taken (no save file on SD), take one now from the
     * core's post-reset save state. This prevents the "no snapshot" branch in
     * on_save_data_updated from always treating every write as a real save,
     * which would fire cart sync immediately after the grace period ends. */
    if (s_save_size > 0 && !s_sync_snapshot) {
        void *init_ptr = NULL;
        size_t init_sz = s_core->savedataClone(s_core, &init_ptr);
        if (init_ptr && init_sz > 0) {
            s_sync_snapshot    = init_ptr;   /* adopt the malloc'd buffer */
            s_sync_snapshot_sz = init_sz;
            lprintf("[mgba] Snapshot from core (new game baseline): %zu bytes\n", init_sz);
        }
    }
    if (s_gba_border_active) {
        s_render_w = (unsigned)GBA_BORDER_W;
        s_render_h = (unsigned)GBA_BORDER_H;
        s_scale    = g_settings.scale_gba_border;
    } else if (s_border_active) {
        s_render_w = (unsigned)SGB_W;
        s_render_h = (unsigned)SGB_H;
    } else {
        s_render_w = s_game_w;
        s_render_h = s_game_h;
    }
    lprintf("[mgba] Render: %ux%u  Game: %ux%u  GBC-border: %s  GBA-border: %s\n",
            s_render_w, s_render_h, s_game_w, s_game_h,
            s_border_active ? "YES" : "no",
            s_gba_border_active ? "YES" : "no");

    gx_init(rmode);
    lprintf("[mgba] GX init OK\n"); log_force_flush();

    AUDIO_Init(0);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    AUDIO_RegisterDMACallback(audio_dma_callback);
    lprintf("[mgba] AUDIO init OK\n"); log_force_flush();

    VIDEO_SetPostRetraceCallback(retrace_callback);
    {
        u32 level = 0;
        _CPU_ISR_Disable(level);
        s_reference_retrace = s_retrace_count;
        _CPU_ISR_Restore(level);
    }

    lprintf("[mgba] Emulation started\n"); log_force_flush();

    /* ----------------------------------------------------------------
     * Main emulation loop — double-buffered XFB for tear-free display
     * s_xfb_cur: which XFB is currently on screen (displayed by VI).
     * We render to the OTHER one, then swap after VSync.
     * -------------------------------------------------------------- */
    void *xfb_bufs[2] = { xfb, s_xfb_alt ? s_xfb_alt : xfb };
    s_xfb_cur = 0;

    while (!s_exit_flag) {
        /* Throttle to VSync rate — like _drawStart in the official Wii port.
         * If we finished faster than VSync, wait; if we're behind, catch up. */
        {
            u32 level = 0;
            _CPU_ISR_Disable(level);
            if (s_reference_retrace > s_retrace_count) {
                _CPU_ISR_Restore(level);
                VIDEO_WaitVSync();
                _CPU_ISR_Disable(level);
                s_reference_retrace = s_retrace_count;
            } else if (s_reference_retrace < s_retrace_count - 1) {
                s_reference_retrace = s_retrace_count - 1;
            }
            _CPU_ISR_Restore(level);
        }

        s_core->runFrame(s_core);  /* audio delivered via mAVStream hook inside here */
        s_frame_count++;

        /* Grace period: count down each frame; no cart sync while > 0. */
        if (s_save_grace_frames > 0) s_save_grace_frames--;

        /* Suppress mGBA default SGB border: black-out the ring outside the 160×144
         * game area for the first 300 frames (~5 s). The game uploads its own custom
         * border via SGB commands during boot; until then we show black instead of
         * mGBA's built-in "Game Boy" graphic. */
        if (s_sgb_suppress_border > 0) {
            uint16_t *vb = (uint16_t *)s_vbuf;
            for (int vy = 0; vy < (int)SGB_H; vy++) {
                for (int vx = 0; vx < (int)SGB_W; vx++) {
                    if (vx < BORDER_GAME_X || vx >= BORDER_GAME_X + GB_W ||
                        vy < BORDER_GAME_Y || vy >= BORDER_GAME_Y + GB_H)
                        vb[vy * SGB_W + vx] = 0;
                }
            }
            s_sgb_suppress_border--;
        }

        /* Input — scan once; Z opens OSD */
        PAD_ScanPads();
        if (PAD_ButtonsDown(0) & PAD_TRIGGER_Z) {
            osd_show();  /* stops DMA, resets retrace ref on return */
            if (s_exit_flag) break;
            PAD_ScanPads();
        }
        s_core->setKeys(s_core, read_gc_keys());

        /* Save indicator drawn into game framebuffer before tiling */
        draw_save_indicator();

        /* Render — copy immediately after GPU draw, no WaitVSync here. */
        void *render_buf = xfb_bufs[s_xfb_cur ^ 1];
        gx_upload_frame(s_render_w, s_render_h);
        GX_SetViewport(0, 0, s_rmode->fbWidth, s_rmode->efbHeight, 0, 1);
        GX_InvVtxCache();
        gx_draw_frame(s_render_w, s_render_h);
        GX_CopyDisp(render_buf, GX_TRUE);
        GX_DrawDone();
        VIDEO_SetNextFramebuffer(render_buf);
        VIDEO_Flush();
        s_xfb_cur ^= 1;

        {
            u32 level = 0;
            _CPU_ISR_Disable(level);
            ++s_reference_retrace;
            _CPU_ISR_Restore(level);
        }

        if (s_frame_count % 60 == 0) audio_log_commit(s_core);
        /* Flush log buffer every ~5 s.  log_force_flush (fflush only) is safe
         * for concurrent callers: the sync thread calls lprintf at the same time
         * and fflush does not free the FILE struct.  log_commit_sd (fclose+fopen)
         * must NOT be called here — it would race with the sync thread's vfprintf
         * on the same FILE, causing a heap use-after-free that corrupts malloc. */
        if (s_frame_count % 300 == 0) log_force_flush();
    }

    lprintf("[mgba] Exiting emulation\n"); log_force_flush();

    /* ----------------------------------------------------------------
     * Teardown — log_force_flush() (fflush only) after each step writes
     * buffered log data to libfat's sector cache, which commits to SD.
     * log_commit_sd() (fclose+fopen) is NOT used here: repeated FILE
     * struct malloc/free cycles across sessions corrupt the dlmalloc
     * free list, causing a DSI crash on the 3rd session's fclose.
     * Console-suppress is cleared first so teardown messages appear on
     * TV even if SD fails mid-teardown.
     * -------------------------------------------------------------- */
    g_log_suppress_console = 0;  /* restore console output before anything can hang */

    /* Do NOT call log_commit_sd() (fclose) before cart_sync_shutdown().
     * The sync thread calls lprintf() inside gbop_write_save() while the USB
     * transfer is in progress.  If the main thread calls fclose(g_log) at the
     * same time, both threads hit libfat concurrently → deadlock → hang. */
    lprintf("[mgba] teardown: stopping audio DMA\n");
    AUDIO_StopDMA();
    AUDIO_RegisterDMACallback(NULL);  /* deregister before sync thread exits */
    VIDEO_SetPostRetraceCallback(NULL);
    /* Reset audio index so any stray AI interrupt fires audio_dma_callback's
     * early-return guard (buf->size == 0 != AUDIO_SAMPLES). */
    s_audio_cur = s_audio_next = 0;
    lprintf("[mgba] teardown: audio DMA stopped\n");

    if (s_audio_log) { fclose(s_audio_log); s_audio_log = NULL; }
    lprintf("[mgba] teardown: audio log closed\n");

    lprintf("[mgba] teardown: shutting down cart_sync\n");
    cart_sync_shutdown();
    lprintf("[mgba] teardown: cart_sync stopped\n"); log_force_flush();

    /* Final save: only write to SD if a confirmed in-game save occurred this
     * session.  The sync thread already wrote after each confirmed save; this
     * is a fallback in case that SD write failed. */
    lprintf("[mgba] teardown: player_saved=%d save_size=%u\n",
            s_player_saved_ingame, s_save_size); log_force_flush();
    if (save_path && s_save_size > 0 && s_player_saved_ingame) {
        void *rawsave = NULL;
        size_t rawsz = s_core->savedataClone(s_core, &rawsave);
        if (rawsave && rawsz > 0) {
            FILE *sf = fopen(save_path, "wb");
            if (sf) { fwrite(rawsave, 1, rawsz, sf); fclose(sf); }
            lprintf("[mgba] teardown: final save written (%zu bytes)\n", rawsz);
            free(rawsave);
        }
    } else if (!s_player_saved_ingame) {
        lprintf("[mgba] teardown: no player save this session — SD unchanged\n");
    }
    log_force_flush();

    /* Skip core->deinit: it corrupts dlmalloc's free list, causing DAR=0x10000000 DSI
     * on the next malloc() call (confirmed tests 84-89). ~640KB of mGBA internals
     * (WRAM 288KB + VRAM 96KB + audio ~130KB + misc) leak per session but are
     * reclaimed on power cycle. The heap MUST stay intact for the next session's
     * while(1) iteration in main(). Null s_core so stale savedataUpdated callbacks
     * during any residual core activity return early without accessing torn-down state. */
    lprintf("[mgba] teardown: skipping core deinit (heap safety)\n"); log_force_flush();
    s_core     = NULL;
    s_save_size = 0;

    lprintf("[mgba] teardown: freeing rom\n"); log_force_flush();
    free(rom_data); rom_data = NULL;
    lprintf("[mgba] teardown: freeing save\n"); log_force_flush();
    free(save_data); save_data = NULL;
    lprintf("[mgba] teardown: memory freed\n"); log_force_flush();

    /* s_gxfifo_buf is a static .data array, not heap-allocated.  It is never freed.
     * The GX CP stays armed to its fixed address between sessions. */
    lprintf("[mgba] teardown: freeing snapshot\n"); log_force_flush();
    if (s_sync_snapshot) { free(s_sync_snapshot); s_sync_snapshot = NULL; }
    s_sync_snapshot_sz = 0;
    lprintf("[mgba] teardown: freeing border\n"); log_force_flush();
    if (s_border_buf) { free(s_border_buf); s_border_buf = NULL; }
    s_border_active = false;
    if (s_gba_border_buf) { free(s_gba_border_buf); s_gba_border_buf = NULL; }
    s_gba_border_active = false;
    lprintf("[mgba] teardown: resetting callbacks\n"); log_force_flush();
    SYS_SetResetCallback(NULL);
    SYS_SetPowerCallback(on_power);
    lprintf("[mgba] teardown: callbacks reset\n"); log_force_flush();
    s_save_grace_frames     = 0;
    s_player_saved_ingame   = 0;
    s_last_save_check_frame = 0;
    s_sgb_suppress_border   = 0;
    s_is_cgb                = false;
    s_synced_this_session   = 0;
    s_info                  = NULL;
    s_render_w = GBA_W;
    s_render_h = GBA_H;
    lprintf("[mgba] teardown: state reset\n"); log_force_flush();

    lprintf("[mgba] teardown: console_init\n"); log_force_flush();
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    lprintf("[mgba] teardown: VIDEO_Configure\n"); log_force_flush();
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_Flush();
    lprintf("[mgba] teardown: VIDEO_WaitVSync\n"); log_force_flush();
    VIDEO_WaitVSync();
    lprintf("[mgba] teardown: console restored\n"); log_force_flush();

    /* log_commit_sd (fclose+fopen) is NOT called here. core->deinit is skipped so
     * the heap is clean, but log_force_flush (fflush) is sufficient — libfat commits
     * buffered sectors on fflush, so no data is lost. g_log remains open for the
     * next session's lprintf calls. */
    lprintf("[mgba] Session ended\n"); log_force_flush();
    return 0;
}
