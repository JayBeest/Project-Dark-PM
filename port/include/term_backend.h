#ifndef PORT_TERM_BACKEND_H
#define PORT_TERM_BACKEND_H

/*
 * -terminal: render the game as coloured ASCII art in the terminal.
 *
 * This is not a renderer. The frame the GL backend has finished drawing is
 * read back with glReadPixels and piped, as raw rgb24, into
 *
 *     ffmpeg -f rawvideo -pixel_format rgb24 -video_size WxH -i - -f caca -
 *
 * and libcaca (via FFmpeg) does the character mapping, the colour and the
 * dithering. FFmpeg's own scaling puts the frame at the terminal's size, so
 * nothing here guesses at cell counts for the caca path.
 *
 * The SDL window still exists - a GL context needs a drawable - but it is
 * never shown (MIDAS_TERM_SHOWWINDOW=1 keeps it visible, for drivers that
 * refuse to render into a hidden window).
 *
 * FFMPEG IS OPTIONAL. Not every ffmpeg is built with --enable-libcaca
 * (Homebrew's, for one, is not), so the driver is probed once:
 *
 *     1. ffmpeg with a caca device -> the pipe above;
 *     2. otherwise (or if the pipe breaks mid-run) -> a built-in ANSI
 *        fallback that writes 24-bit colour half-blocks to stdout.
 *
 * Which one ran is logged, because a mode that silently degrades to the
 * fallback is indistinguishable from the one you meant to test.
 *
 * Knobs (environment, read once):
 *   MIDAS_TERM_DRIVER=ffmpeg|ansi  force a driver, skipping the probe
 *   MIDAS_TERM_FPS=<n>             frame cap, default 20 (caca is CPU-heavy)
 *   MIDAS_TERM_SIZE=WxH            readback size fed to ffmpeg, default
 *                                  ≤320 wide, window aspect kept
 *   MIDAS_TERM_FFMPEG=<path>       the ffmpeg binary, default "ffmpeg"
 *   MIDAS_TERM_SHOWWINDOW=1        do not hide the SDL window
 */

/* Which renderer -terminal uses. AUTO probes for a caca-capable ffmpeg and
 * falls back to the built-in ANSI renderer; the other two force one and are
 * how you compare them, or run a known path when the probe would guess wrong. */
typedef enum {
    PORT_TERM_DRIVER_AUTO = 0,   /* -terminal, --backend=caca (default) */
    PORT_TERM_DRIVER_FFMPEG = 1, /* --backend=ffmpeg: the caca pipe, or ANSI if it fails */
    PORT_TERM_DRIVER_ANSI = 2    /* --backend=ansi: the built-in renderer */
} PortTermDriver;

#ifdef __cplusplus
extern "C" {
#endif

/* Turn -terminal on for this run. Idempotent. */
void portTermBackendSetEnabled(int on);

/* Where to send the game's stderr once the backend is on, or NULL to leave it
 * alone. Call before portTermBackendSetEnabled.
 *
 * Both renderers paint the whole screen, so a `ge:` line printed while they own
 * it lands on top of the picture - and if it lands on the last row, the
 * terminal scrolls and the image jumps. Pointing stderr at a file makes that
 * impossible instead of unlikely. MIDAS_TERM_NOLOG=1 keeps stderr on the
 * terminal (the lines then overwrite the art). */
void portTermBackendSetLogPath(const char *path);

/* Pin which driver -terminal uses (a PortTermDriver). Independent of the order
 * relative to SetEnabled, and it outranks MIDAS_TERM_DRIVER. */
void portTermBackendSetDriver(int driver);

/* 1 while -terminal asked for this run. Safe before any GL exists. */
int portTermBackendActive(void);

/* 1 when the SDL window should stay hidden (active, unless
 * MIDAS_TERM_SHOWWINDOW is set). Called from the window backend's init. */
int portTermBackendHidesWindow(void);

/*
 * The capture hook, called from the renderer at the end of a frame and
 * before the buffer swap:
 *
 *     if (portTermBackendWantsFrame()) {
 *         glReadPixels(..., buf);
 *         portTermBackendSubmitFrame(buf, w, h);
 *     }
 *
 * WantsFrame does the rate limiting, so a frame that will be dropped costs
 * no readback at all - which matters, because a 1080p readback is 8 MB.
 */
int portTermBackendWantsFrame(void);

/*
 * `rgb` is rgb24, w*h*3 bytes, BOTTOM-UP (row 0 is the bottom of the image),
 * exactly as glReadPixels returns it. It is not retained: the frame is
 * downsampled and written before this returns.
 */
void portTermBackendSubmitFrame(const unsigned char *rgb, int w, int h);

/* Close the pipe and restore the terminal (cursor, SGR). Registered with
 * atexit when the backend starts; safe to call twice. */
void portTermBackendShutdown(void);

/* ---------------------------------------------------------------------------
 * Pure helpers, exposed so port/tests/term_backend.c can check them without
 * a window, a GPU, an ffmpeg or a terminal.
 * ------------------------------------------------------------------------- */

/*
 * Box-filter `src` (sw x sh, rgb24) into `dst` (dw x dh, rgb24).
 * `flip_y` reads the source bottom-up (glReadPixels order) and writes the
 * destination top-down. Returns 1 on success, 0 on bad arguments.
 */
int portTermDownsample(unsigned char *dst, int dw, int dh,
                       const unsigned char *src, int sw, int sh, int flip_y);

/*
 * Render `rgb` (w x h, top-down, rgb24) as ANSI half-blocks into `dst`.
 * Each cell is one '▀' (U+2580): the top half in the foreground colour and
 * the bottom half in the background colour, so a cell carries two pixels.
 * h must be even; the last row pair is repeated if it is not.
 *
 * Each output row is placed with an ABSOLUTE cursor escape at terminal row
 * `row0` (1-based) and up, and NO newline is emitted anywhere. That is
 * deliberate: a trailing newline parks the cursor on the terminal's last row,
 * where the next line of game log output scrolls the screen and makes the
 * picture bob. Absolute placement means one frame cannot scroll at all.
 *
 * Returns the number of bytes written (never including a terminator), or -1
 * when `cap` is too small.
 */
int portTermAnsiFrame(char *dst, int cap, const unsigned char *rgb, int w, int h, int row0);

#ifdef __cplusplus
}
#endif

#endif /* PORT_TERM_BACKEND_H */
