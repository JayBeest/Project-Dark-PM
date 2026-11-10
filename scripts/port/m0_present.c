/*
 * m0_present.c — Miyoo Mini present-path probe (milestone M0).
 *
 *   docs/port/miyoo-backend.md §4, M0
 *
 * The whole software-renderer plan rests on two hardware questions that no
 * document in this repository answers, because nobody has run anything on the
 * device yet:
 *
 *   1. Does MI_GFX_BitBlit *scale* when the source and destination rectangles
 *      differ in size? If yes, the present step is free and the rasteriser can
 *      run at 320x240. If no, the rasteriser has to fill 640x480 itself.
 *   2. Does an MI_SYS_MMA_Alloc buffer land *above* 0x10000000? port/src/pcsched.c
 *      seg_addr() treats untagged low words as segment references, so a display
 *      list that lives below that bound is misread.
 *
 * This program answers both without involving SDL, EGL, GLES or the engine. It
 * allocates one surface, paints a gradient on it with the CPU, blits it to
 * /dev/fb0 through the same path the SDL "Mini" video driver uses, flips, and
 * reads the result back to see where the pixels actually landed.
 *
 * It is deliberately standalone: no SDL, no geometry, no renderer. If this
 * fails, the architecture changes; if it passes, the rasteriser is next.
 *
 * Build:   scripts/port/build_m0.sh
 * Run:     ./m0_present [logfile]        (default Data/Logs/m0.log)
 *
 * Every line is `key=value` so the log can be grepped off the SD card.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "mi_sys.h"
#include "mi_gfx.h"

/* Source surface. 320x240 is the N64's own resolution and the size the plan
 * targets; the panel is 640x480, i.e. exactly 2x, which makes any scaling
 * visible as a surviving 2x2 checker rather than an interpolation guess. */
#define SRC_W 320
#define SRC_H 240
#define BPP   4

/* Above this, pcsched.c's seg_addr() is safe. See docs/port/miyoo-backend.md §5. */
#define SEG_ADDR_SAFE_MIN 0x10000000ull

static FILE *g_log;

static void logf_(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

/* The MI error codes are documented for a handful of the cases that matter
 * here; naming them is the difference between "rc=-5" and "the driver refused
 * to stretch", which is the answer to question 1. */
static const char *mi_err_name(MI_S32 rc)
{
    if (rc == 0)
        return "MI_SUCCESS";
    if (rc == MI_ERR_GFX_NOT_INIT)             return "MI_ERR_GFX_NOT_INIT";
    if (rc == MI_ERR_GFX_INVALID_PARAM)        return "MI_ERR_GFX_INVALID_PARAM";
    if (rc == MI_ERR_GFX_INVALID_DEVID)        return "MI_ERR_GFX_INVALID_DEVID";
    if (rc == MI_ERR_GFX_DEV_BUSY)             return "MI_ERR_GFX_DEV_BUSY";
    if (rc == MI_ERR_GFX_DRV_FAIL_FORMAT)      return "MI_ERR_GFX_DRV_FAIL_FORMAT";
    if (rc == MI_ERR_GFX_NON_ALIGN_ADDRESS)    return "MI_ERR_GFX_NON_ALIGN_ADDRESS";
    if (rc == MI_ERR_GFX_NON_ALIGN_PITCH)      return "MI_ERR_GFX_NON_ALIGN_PITCH";
    if (rc == MI_ERR_GFX_DRV_FAIL_STRETCH)     return "MI_ERR_GFX_DRV_FAIL_STRETCH";
    return "unrecognised";
}

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* ---- pixels --------------------------------------------------------------
 * The SDK's MI_GFX_PaletteEntry_t documents memory order as A,R,G,B, so work
 * in bytes throughout and never assume what a uint32_t means on this endianness.
 */
static void put_px(uint8_t *base, int stride, int x, int y,
                   uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = base + (size_t)y * stride + (size_t)x * BPP;
    p[0] = a;
    p[1] = r;
    p[2] = g;
    p[3] = b;
}

static void sample_px(const uint8_t *base, int stride, int x, int y,
                      uint8_t out[4])
{
    const uint8_t *p = base + (size_t)y * stride + (size_t)x * BPP;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
}

/* Non-black = the blitter wrote here. Alpha is set to 255 by the paint, so a
 * stale zeroed buffer reads as empty and a written pixel never does. */
static int is_lit(const uint8_t px[4])
{
    return px[1] || px[2] || px[3];
}

static void fill_rect(uint8_t *base, int stride, int x0, int y0, int w, int h,
                      uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            put_px(base, stride, x, y, a, r, g, b);
}

/*
 * Paint the test pattern:
 *   - an R/G gradient across the frame, over vertical blue stripes so that a
 *     scaled copy is obvious and a 1:1 copy is obviously only a quarter;
 *   - a white border, so the source rect's edges are visible as edges;
 *   - four saturated corner blocks (TL red, TR green, BL blue, BR yellow),
 *     which make rotation and mirroring unambiguous;
 *   - a white crosshair at the centre.
 */
static void paint_pattern(uint8_t *base, int stride)
{
    for (int y = 0; y < SRC_H; y++) {
        for (int x = 0; x < SRC_W; x++) {
            uint8_t r = (uint8_t)(x * 255 / (SRC_W - 1));
            uint8_t g = (uint8_t)(y * 255 / (SRC_H - 1));
            uint8_t b = ((x / 32) & 1) ? 200 : 32;
            /* Keep the extreme corners from being claimed by the blocks. */
            put_px(base, stride, x, y, 255, r, g, b);
        }
    }

    fill_rect(base, stride, 0, 0, SRC_W, 4, 255, 255, 255, 255);            /* top    */
    fill_rect(base, stride, 0, SRC_H - 4, SRC_W, 4, 255, 255, 255, 255);    /* bottom */
    fill_rect(base, stride, 0, 0, 4, SRC_H, 255, 255, 255, 255);            /* left   */
    fill_rect(base, stride, SRC_W - 4, 0, 4, SRC_H, 255, 255, 255, 255);    /* right  */

    fill_rect(base, stride, 6, 6, 28, 28, 255, 255, 0, 0);                  /* TL red    */
    fill_rect(base, stride, SRC_W - 34, 6, 28, 28, 255, 0, 255, 0);         /* TR green  */
    fill_rect(base, stride, 6, SRC_H - 34, 28, 28, 255, 0, 0, 255);         /* BL blue   */
    fill_rect(base, stride, SRC_W - 34, SRC_H - 34, 28, 28,
              255, 255, 255, 0);                                            /* BR yellow */

    fill_rect(base, stride, SRC_W / 2 - 2, 0, 4, SRC_H, 255, 255, 255, 255);
    fill_rect(base, stride, 0, SRC_H / 2 - 2, SRC_W, 4, 255, 255, 255, 255);
}

/* ---- framebuffer --------------------------------------------------------- */

struct fb {
    int          fd;
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    int          w, h, stride, bpp;
    int          double_buffered;
    int          cached;
    size_t       map_len;
    uint8_t     *vir;
    MI_PHY       phy;
};

static int fb_open(struct fb *f)
{
    memset(f, 0, sizeof(*f));
    f->fd = open("/dev/fb0", O_RDWR);
    if (f->fd < 0) {
        logf_("fb.open=FAIL errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    if (ioctl(f->fd, FBIOGET_FSCREENINFO, &f->finfo) < 0 ||
        ioctl(f->fd, FBIOGET_VSCREENINFO, &f->vinfo) < 0) {
        logf_("fb.ioctl=FAIL errno=%d (%s)\n", errno, strerror(errno));
        close(f->fd);
        f->fd = -1;
        return -1;
    }

    f->w    = f->vinfo.xres;
    f->h    = f->vinfo.yres;
    f->bpp  = f->vinfo.bits_per_pixel ? f->vinfo.bits_per_pixel / 8 : 4;
    /* line_length is authoritative; xres*4 is only a fallback. */
    f->stride = f->finfo.line_length ? (int)f->finfo.line_length
                                     : f->w * f->bpp;

    logf_("fb.open=ok\n");
    logf_("fb.geometry=%dx%d bpp=%d stride=%d\n", f->w, f->h, f->bpp, f->stride);
    logf_("fb.phys=0x%llx len=%u line_length=%u\n",
          (unsigned long long)f->finfo.smem_start, f->finfo.smem_len,
          f->finfo.line_length);

    /* Same double-buffer setup the SDL Mini driver uses. Only ask for two
     * buffers if the mapping is actually large enough for them. */
    if ((size_t)f->finfo.smem_len >= (size_t)f->stride * f->h * 2) {
        struct fb_var_screeninfo want = f->vinfo;
        want.yoffset = 0;
        want.yres_virtual = f->h * 2;
        if (ioctl(f->fd, FBIOPUT_VSCREENINFO, &want) == 0 &&
            ioctl(f->fd, FBIOGET_VSCREENINFO, &f->vinfo) == 0 &&
            f->vinfo.yres_virtual >= (unsigned)f->h * 2) {
            f->double_buffered = 1;
        } else {
            logf_("fb.double_buffer=unavailable errno=%d\n", errno);
        }
    }
    logf_("fb.double_buffer=%s yres_virtual=%u yoffset=%u\n",
          f->double_buffered ? "yes" : "no",
          f->vinfo.yres_virtual, f->vinfo.yoffset);

    f->phy = f->finfo.smem_start;
    f->map_len = f->finfo.smem_len;
    /*
     * Uncached first, deliberately. This probe reads the framebuffer back to
     * find out where the blitter put things, and a cached mapping can serve
     * stale lines - which would look exactly like "the blit did not scale".
     * The SDL driver maps cached because it only ever writes.
     */
    if (MI_SYS_Mmap(f->phy, (MI_U32)f->map_len, (void **)&f->vir, FALSE) != 0 ||
        !f->vir) {
        f->cached = 1;
        if (MI_SYS_Mmap(f->phy, (MI_U32)f->map_len, (void **)&f->vir, TRUE) != 0 ||
            !f->vir) {
            logf_("fb.mmap=FAIL\n");
            close(f->fd);
            f->fd = -1;
            return -1;
        }
    }
    logf_("fb.mapped=ok vir=%p cache=%s\n", (void *)f->vir,
          f->cached ? "cached (readback may be stale)" : "uncached");

    /* Junk from MainUI makes the coverage counts meaningless. */
    MI_SYS_MemsetPa(f->phy, 0, (MI_U32)f->map_len);
    return 0;
}

static void fb_close(struct fb *f)
{
    if (f->fd <= 0)
        return;
    if (f->vir && f->map_len)
        MI_SYS_Munmap(f->vir, (MI_U32)f->map_len);
    f->vinfo.yoffset = 0;
    ioctl(f->fd, FBIOPUT_VSCREENINFO, &f->vinfo);
    close(f->fd);
    f->fd = -1;
}

int main(int argc, char **argv)
{
    const char *log_path = argc > 1 ? argv[1] : "Data/Logs/m0.log";
    struct fb fb;
    MI_PHY  src_phy = 0;
    void   *src_vir_raw = NULL;
    uint8_t *src;
    MI_GFX_Surface_t src_surf, dst_surf;
    MI_GFX_Rect_t    src_rect, dst_rect;
    MI_GFX_Opt_t     opt;
    MI_S32 rc;
    int      fail = 0, scaling = -1;   /* -1 unknown, 0 no, 1 yes */
    double   t0, blit_ms = 0.0;

    g_log = fopen(log_path, "w");
    if (!g_log)
        fprintf(stderr, "m0: cannot open %s: %s\n", log_path, strerror(errno));

    logf_("=== M0 present-path probe ===\n");
    logf_("source=%dx%d fmt=ARGB8888\n", SRC_W, SRC_H);

    rc = MI_SYS_Init();
    logf_("sys.init=rc=%d (%s)\n", rc, mi_err_name(rc));
    if (rc != 0) {
        logf_("VERDICT present_path=FAIL reason=MI_SYS_Init\n");
        return 1;
    }

    rc = MI_GFX_Open();
    logf_("gfx.open=rc=%d (%s)\n", rc, mi_err_name(rc));

    if (fb_open(&fb) != 0) {
        logf_("VERDICT present_path=FAIL reason=framebuffer\n");
        MI_GFX_Close();
        MI_SYS_Exit();
        return 1;
    }

    /* ---- allocation ----------------------------------------------------- */
    rc = MI_SYS_MMA_Alloc(NULL, SRC_W * SRC_H * BPP, &src_phy);
    logf_("mma.alloc=rc=%d (%s) phys=0x%llx size=%d\n",
          rc, mi_err_name(rc), (unsigned long long)src_phy, SRC_W * SRC_H * BPP);
    if (rc != 0) {
        logf_("VERDICT present_path=FAIL reason=MI_SYS_MMA_Alloc\n");
        fb_close(&fb);
        MI_GFX_Close();
        MI_SYS_Exit();
        return 1;
    }

    logf_("mma.above_0x10000000=%s\n",
          (src_phy >= SEG_ADDR_SAFE_MIN) ? "yes" : "NO");

    rc = MI_SYS_Mmap(src_phy, SRC_W * SRC_H * BPP, &src_vir_raw, TRUE);
    logf_("mma.mmap=rc=%d (%s) vir=%p\n", rc, mi_err_name(rc), src_vir_raw);
    if (rc != 0 || !src_vir_raw) {
        logf_("VERDICT present_path=FAIL reason=MI_SYS_Mmap\n");
        MI_SYS_MMA_Free(src_phy);
        fb_close(&fb);
        MI_GFX_Close();
        MI_SYS_Exit();
        return 1;
    }
    src = (uint8_t *)src_vir_raw;

    paint_pattern(src, SRC_W * BPP);
    MI_SYS_FlushInvCache(src, SRC_W * SRC_H * BPP);

    /* ---- surface descriptors -------------------------------------------- */
    memset(&opt, 0, sizeof(opt));
    opt.u32GlobalSrcConstColor = 0;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eDstDfbBldOp = 0;
    opt.eDFBBlendFlag = 0;

    src_surf.phyAddr  = src_phy;
    src_surf.eColorFmt = E_MI_GFX_FMT_ARGB8888;
    src_surf.u32Width  = SRC_W;
    src_surf.u32Height = SRC_H;
    src_surf.u32Stride = SRC_W * BPP;

    src_rect.s32Xpos = 0;
    src_rect.s32Ypos = 0;
    src_rect.u32Width = SRC_W;
    src_rect.u32Height = SRC_H;

    /*
     * Three passes, each clearing the panel first:
     *   A  1:1, no rotation  — does anything arrive at all, and where?
     *   B  1:1, rotate 180   — orientation, which the product path needs
     *   C  2x scaled, rotate 180 — the question the plan hangs on
     */
    for (int pass = 0; pass < 3; pass++) {
        int        dst_w = (pass == 2) ? fb.w : SRC_W;
        int        dst_h = (pass == 2) ? fb.h : SRC_H;
        MI_GFX_Rotate_e rot = (pass == 0) ? E_MI_GFX_ROTATE_0
                                          : E_MI_GFX_ROTATE_180;
        MI_U16 fence = 0;
        int    q[4] = {0, 0, 0, 0}, total = 0;
        uint8_t px[4];

        if ((size_t)dst_w * fb.bpp > (size_t)fb.stride) {
            /* The destination cannot be wider than one framebuffer line. */
            dst_w = fb.stride / fb.bpp;
        }

        MI_SYS_MemsetPa(fb.phy + (MI_PHY)fb.vinfo.yoffset * fb.stride, 0,
                        (MI_U32)((size_t)fb.stride * fb.h));

        opt.eRotate = rot;
        dst_surf.phyAddr   = fb.phy + (MI_PHY)fb.vinfo.yoffset * fb.stride;
        dst_surf.eColorFmt = E_MI_GFX_FMT_ARGB8888;
        dst_surf.u32Width  = fb.w;
        dst_surf.u32Height = fb.h;
        dst_surf.u32Stride = fb.stride;

        dst_rect.s32Xpos = 0;
        dst_rect.s32Ypos = 0;
        dst_rect.u32Width = dst_w;
        dst_rect.u32Height = dst_h;

        t0 = now_ms();
        rc = MI_GFX_BitBlit(&src_surf, &src_rect, &dst_surf, &dst_rect, &opt,
                            &fence);
        if (rc == 0)
            MI_GFX_WaitAllDone(TRUE, fence);
        blit_ms = now_ms() - t0;

        logf_("pass%d.blit=%s dst=%dx%d rotate=%s rc=%d (%s) fence=%u ms=%.2f\n",
              'A' + pass, rc == 0 ? "ok" : "FAIL", dst_w, dst_h,
              rot == E_MI_GFX_ROTATE_180 ? "180" : "0",
              rc, mi_err_name(rc), fence, blit_ms);
        if (rc != 0)
            fail = 1;

        if (fb.double_buffered) {
            ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.vinfo);
            fb.vinfo.yoffset ^= fb.h;
        }

        /* Readback. On a cached mapping, invalidate first: the blitter wrote
         * DRAM behind this process's cache. */
        if (fb.cached)
            MI_SYS_FlushInvCache(fb.vir, (MI_U32)((size_t)fb.stride * fb.h));

        if (rc == 0) {
            struct { int x, y; } probe[5] = {
                { 0, 0 },
                { dst_w - 1, 0 },
                { 0, dst_h - 1 },
                { dst_w - 1, dst_h - 1 },
                { dst_w / 2, dst_h / 2 },
            };
            static const char *name[5] = { "tl", "tr", "bl", "br", "mid" };

            for (int i = 0; i < 5; i++) {
                sample_px(fb.vir, fb.stride, probe[i].x, probe[i].y, px);
                logf_("pass%d.%s=(%d,%d) argb=%02x%02x%02x%02x lit=%d\n",
                      'A' + pass, name[i], probe[i].x, probe[i].y,
                      px[0], px[1], px[2], px[3], is_lit(px));
            }

            for (int y = 0; y < dst_h; y++) {
                int qy = (y < dst_h / 2) ? 0 : 2;
                for (int x = 0; x < dst_w; x++) {
                    sample_px(fb.vir, fb.stride, x, y, px);
                    if (is_lit(px))
                        q[qy + (x < dst_w / 2 ? 0 : 1)]++;
                    total++;
                }
            }
            logf_("pass%d.coverage tl=%.1f%% tr=%.1f%% bl=%.1f%% br=%.1f%%\n",
                  'A' + pass,
                  100.0 * q[0] / (total / 4.0), 100.0 * q[1] / (total / 4.0),
                  100.0 * q[2] / (total / 4.0), 100.0 * q[3] / (total / 4.0));

            if (pass == 2) {
                /* The gradient's far corner is the only way to tell a real
                 * 2x scale from a blit that only filled the top-left quarter
                 * with black-ish work: both far quadrants must be lit. */
                if (q[3] > 0 && q[1] > 0)
                    scaling = 1;
                else if (q[0] > 0 && q[3] == 0)
                    scaling = 0;
            }
        }
    }

    /* ---- cost ------------------------------------------------------------ */
    if (!fail) {
        const int   frames = 60;
        double      best = 1e9, worst = 0, sum = 0;
        MI_GFX_Surface_t d = dst_surf;

        d.phyAddr = fb.phy;
        d.u32Width = fb.w;
        d.u32Height = fb.h;
        d.u32Stride = fb.stride;
        opt.eRotate = E_MI_GFX_ROTATE_180;
        dst_rect.u32Width = fb.w;
        dst_rect.u32Height = fb.h;

        for (int i = 0; i < frames; i++) {
            MI_U16   fence = 0;
            double   a, ms;

            d.phyAddr = fb.phy + (MI_PHY)fb.vinfo.yoffset * fb.stride;
            a = now_ms();
            if (MI_GFX_BitBlit(&src_surf, &src_rect, &d, &dst_rect, &opt,
                               &fence) == 0)
                MI_GFX_WaitAllDone(TRUE, fence);
            if (fb.double_buffered) {
                ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.vinfo);
                fb.vinfo.yoffset ^= fb.h;
            }
            ms = now_ms() - a;
            sum += ms;
            if (ms < best) best = ms;
            if (ms > worst) worst = ms;
        }
        logf_("timing.frames=%d avg_ms=%.2f min_ms=%.2f max_ms=%.2f\n",
              frames, sum / frames, best, worst);
    }

    /* Leave something a human can look at, then hand the panel back. */
    logf_("VERDICT present_path=%s\n", fail ? "FAIL" : "ok");
    logf_("VERDICT scaling=%s\n",
          scaling == 1 ? "yes" : scaling == 0 ? "NO" : "unknown");
    logf_("VERDICT alloc_above_0x10000000=%s\n",
          src_phy >= SEG_ADDR_SAFE_MIN ? "yes" : "NO");

    if (!fail) {
        sleep(3);
    }

    MI_SYS_Munmap(src_vir_raw, SRC_W * SRC_H * BPP);
    MI_SYS_MMA_Free(src_phy);
    fb_close(&fb);
    MI_GFX_Close();
    MI_SYS_Exit();
    if (g_log)
        fclose(g_log);
    return fail;
}
