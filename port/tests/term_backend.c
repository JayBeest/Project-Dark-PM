/*
 * -terminal's pure half: the box downscale and the ANSI half-block packer.
 *
 * Everything here runs without a window, a GPU, an ffmpeg or a terminal. That
 * is deliberate: the interesting failure of this backend is not "the pipe did
 * not open" (loud, and the log says so), it is a picture that is upside down,
 * squashed, or one cell short - and those are all decided by the two functions
 * below. The pipe and the probe need a real run; the arithmetic does not.
 *
 * Build standalone:
 *   cc -std=c99 -Iport/include port/tests/term_backend.c port/src/term_backend.c -o t
 *   ./t
 *
 * portTermBackendShutdown writes SGR/ANSI bytes to stdout and that is fine:
 * nothing in this file ever enables the backend, so the writes are the only
 * output and the assertions still speak on stderr.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "term_backend.h"

/* term_backend.c logs through the engine's console tee. Nothing in these tests
 * reaches a log line (the backend is never enabled), but the symbol must
 * resolve to link the real implementation. */
void portConLogf(const char *fmt, ...)
{
    (void) fmt;
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); } } while (0)

/* ---- downsample ---------------------------------------------------------- */

static void downsample_identity(void)
{
    unsigned char src[4 * 3] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    unsigned char dst[4 * 3];

    CHECK(portTermDownsample(dst, 2, 2, src, 2, 2, 0) == 1);
    CHECK(memcmp(dst, src, sizeof(src)) == 0);
}

/* 2x2 -> 1x1 averages all four pixels, rounding to nearest (127.5 -> 128). */
static void downsample_box_average(void)
{
    unsigned char src[4 * 3] = {
        0, 0, 0,
        255, 255, 255,
        255, 255, 255,
        0, 0, 0,
    };
    unsigned char dst[3];

    CHECK(portTermDownsample(dst, 1, 1, src, 2, 2, 0) == 1);
    CHECK(dst[0] == 128);
    CHECK(dst[1] == 128);
    CHECK(dst[2] == 128);
}

/* glReadPixels hands back row 0 = the BOTTOM of the frame. flip_y=1 must turn
 * that into a top-down image, or the terminal shows the level upside down and
 * the HUD under the floor. Source: 1 wide, 2 tall, bottom row red, top row
 * blue -> top-down output must be blue then red. */
static void downsample_flips_bottom_up(void)
{
    unsigned char src[2 * 3] = {
        255, 0, 0,   /* row 0: bottom of the frame */
        0, 0, 255,   /* row 1: top of the frame */
    };
    unsigned char dst[2 * 3];

    CHECK(portTermDownsample(dst, 1, 2, src, 1, 2, 1) == 1);
    CHECK(dst[0] == 0 && dst[1] == 0 && dst[2] == 255);      /* top row */
    CHECK(dst[3] == 255 && dst[4] == 0 && dst[5] == 0);      /* bottom row */
}

static void downsample_rejects_bad_args(void)
{
    unsigned char px[3];

    CHECK(portTermDownsample(NULL, 1, 1, px, 1, 1, 0) == 0);
    CHECK(portTermDownsample(px, 1, 1, NULL, 1, 1, 0) == 0);
    CHECK(portTermDownsample(px, 0, 1, px, 1, 1, 0) == 0);
}

/* ---- ANSI packer --------------------------------------------------------- */

#define RED_FG_BLUE_BG "\x1b[38;2;255;0;0;48;2;0;0;255m"
#define HALF_BLOCK "\xE2\x96\x80"

/* One cell, two pixels: the top half carries the foreground colour and the
 * bottom half the background. This is the whole encoding. The row is placed
 * absolutely at terminal row 1, and there is no newline. */
static void ansi_single_cell(void)
{
    const unsigned char rgb[2 * 1 * 3] = {
        255, 0, 0,     /* top    -> foreground */
        0, 0, 255,     /* bottom -> background */
    };
    const char *want = "\x1b[1;1H" RED_FG_BLUE_BG HALF_BLOCK "\x1b[K";
    char out[256];
    int n = portTermAnsiFrame(out, (int) sizeof(out), rgb, 1, 2, 1);

    CHECK(n == (int) strlen(want));
    out[n] = '\0';
    CHECK(strcmp(out, want) == 0);
}

/* A frame emits NO newline: that is the whole fix for the picture bobbing up
 * and down, because a newline parked the cursor on the last terminal row and
 * the next stderr line scrolled the screen. */
static void ansi_emits_no_newline(void)
{
    unsigned char rgb[4 * 2 * 3] = { 0 };
    char out[512];
    int n = portTermAnsiFrame(out, (int) sizeof(out), rgb, 4, 2, 1);

    CHECK(n > 0);
    out[n] = '\0';
    CHECK(strchr(out, '\n') == NULL);
    CHECK(strstr(out, "\x1b[1;1H") != NULL);
    CHECK(strstr(out, "\x1b[K") != NULL);
}

/* Rows are placed at row0 upward, so a caller can reserve lines at the top
 * or bottom without the packer needing to know why. */
static void ansi_honours_row0(void)
{
    unsigned char rgb[1 * 4 * 3] = { 0 };
    char out[512];
    int n = portTermAnsiFrame(out, (int) sizeof(out), rgb, 1, 4, 7);

    CHECK(n > 0);
    out[n] = '\0';
    CHECK(strstr(out, "\x1b[7;1H") != NULL);
    CHECK(strstr(out, "\x1b[8;1H") != NULL);
    CHECK(strstr(out, "\x1b[9;1H") == NULL);
}

/* Same colours side by side must NOT re-emit the escape: at 200 cells wide,
 * one SGR per cell is the difference between a smooth picture and a stutter. */
static void ansi_repeats_are_not_re_emitted(void)
{
    const unsigned char rgb[2 * 2 * 3] = {
        255, 0, 0,  255, 0, 0,
        0, 0, 255,  0, 0, 255,
    };
    const char *want = "\x1b[1;1H" RED_FG_BLUE_BG HALF_BLOCK HALF_BLOCK "\x1b[K";
    char out[256];
    int n = portTermAnsiFrame(out, (int) sizeof(out), rgb, 2, 2, 1);

    CHECK(n == (int) strlen(want));
    out[n] = '\0';
    CHECK(strcmp(out, want) == 0);
}

/* An odd height repeats its last row rather than dropping it: half a cell of
 * colour beats losing the bottom pixel row of the frame. */
static void ansi_odd_height_repeats_last_row(void)
{
    const unsigned char rgb[3 * 1 * 3] = {
        255, 0, 0,    /* row 0 -> top    */
        0, 0, 255,    /* row 1 -> bottom */
        0, 255, 0,    /* row 2 -> the repeat */
    };
    const char *want = "\x1b[1;1H" RED_FG_BLUE_BG HALF_BLOCK "\x1b[K"
                       "\x1b[2;1H" "\x1b[38;2;0;255;0;48;2;0;255;0m" HALF_BLOCK "\x1b[K";
    char out[256];
    int n = portTermAnsiFrame(out, (int) sizeof(out), rgb, 1, 3, 1);

    CHECK(n == (int) strlen(want));
    out[n] = '\0';
    CHECK(strcmp(out, want) == 0);
}

/* A buffer one byte short must report failure, not write past the end. */
static void ansi_refuses_short_buffer(void)
{
    const unsigned char rgb[2 * 3] = { 255, 0, 0, 0, 0, 255 };
    const int need = (int) strlen("\x1b[1;1H" RED_FG_BLUE_BG HALF_BLOCK "\x1b[K");
    char out[128];

    CHECK(need < (int) sizeof(out));
    CHECK(portTermAnsiFrame(out, need - 1, rgb, 1, 2, 1) == -1);
    CHECK(portTermAnsiFrame(out, need, rgb, 1, 2, 1) == need);
}

static void ansi_rejects_bad_args(void)
{
    unsigned char px[3];
    char out[64];

    CHECK(portTermAnsiFrame(NULL, 64, px, 1, 1, 1) == -1);
    CHECK(portTermAnsiFrame(out, 64, NULL, 1, 1, 1) == -1);
    CHECK(portTermAnsiFrame(out, 64, px, 0, 1, 1) == -1);
    CHECK(portTermAnsiFrame(out, 64, px, 1, 1, 0) == -1); /* row0 is 1-based */
}

/* ---- backend state ------------------------------------------------------- */

/* Off by default: nothing may capture a frame until the flag asks for one. */
static void backend_is_off_until_enabled(void)
{
    CHECK(portTermBackendActive() == 0);
    CHECK(portTermBackendHidesWindow() == 0);
    CHECK(portTermBackendWantsFrame() == 0);
}

int main(void)
{
    downsample_identity();
    downsample_box_average();
    downsample_flips_bottom_up();
    downsample_rejects_bad_args();

    ansi_single_cell();
    ansi_emits_no_newline();
    ansi_honours_row0();
    ansi_repeats_are_not_re_emitted();
    ansi_odd_height_repeats_last_row();
    ansi_refuses_short_buffer();
    ansi_rejects_bad_args();

    backend_is_off_until_enabled();

    printf("term_backend: all checks passed\n");
    return 0;
}
