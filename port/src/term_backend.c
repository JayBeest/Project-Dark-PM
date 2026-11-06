/*
 * -terminal: the game drawn as coloured ASCII art in the terminal.
 *
 * See term_backend.h for the shape and the knobs. In one line: the finished
 * GL frame is read back and piped as raw rgb24 into
 *
 *     ffmpeg -f rawvideo -pixel_format rgb24 -video_size WxH -i - -f caca -
 *
 * so libcaca does the character mapping, the colour and the dithering. When
 * that ffmpeg has no caca device (Homebrew's does not - it is not built with
 * --enable-libcaca), the same pixels go out as ANSI 24-bit half-blocks
 * instead. The probe is one `ffmpeg -devices` at the first frame, and the
 * result is logged: a silent fallback would look exactly like the real thing.
 *
 * Deliberately absent: error handling beyond "log it and keep the picture
 * moving". A broken pipe falls back to ANSI rather than killing the process,
 * because a game that dies when its display adapter does is worse than one
 * that degrades. SIGPIPE is ignored for the same reason - without that, a
 * dead ffmpeg would take the whole game down with a signal nobody can read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define termPopen _popen
#define termPclose _pclose
#else
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define termPopen popen
#define termPclose pclose
#endif

#include "term_backend.h"

/* port_console.c: same output as the fprintf(stderr, ...) this replaced,
 * plus a silent copy in the tilde console ring (spec section 7). */
extern void portConLogf(const char *fmt, ...);

/* U+2580 UPPER HALF BLOCK: one cell, two pixels - foreground on top,
 * background below. This is what makes a character cell carry 2:1 the
 * colour resolution of plain text. */
#define TERM_HALF_BLOCK "\xE2\x96\x80"

/* The most bytes one cell can cost: the combined fg/bg SGR escape
 * (\x1b[38;2;r;g;b;48;2;r;g;b m is at most 33 with 3-digit components)
 * plus the 3-byte glyph. Rounded up, because guessing low here would
 * truncate the last cell of the last row of a wide terminal. */
#define TERM_BYTES_PER_CELL 48

/* Per-row overhead on top of the cells: the cursor-position escape that puts
 * the row at an absolute line (\x1b[1234;1H, at most ~10 bytes) and the
 * erase-to-end-of-line that clears a previously wider frame (~3). */
#define TERM_BYTES_PER_ROW 24

/* Rows kept free below the picture. The game logs `ge:` lines to stderr while
 * it runs, and those share this terminal; without a margin they would land on
 * the bottom row of the art. Two is enough for a wrapped line. */
#define TERM_RESERVED_ROWS 2

static int s_Enabled;        /* the boot flag asked for this run */
static int s_Active;         /* ...and the backend is live (same thing today) */
static int s_ForcedDriver = PORT_TERM_DRIVER_AUTO; /* from the command line */
static int s_ShutdownArmed;
static double s_Fps = 20.0;  /* caca is CPU-heavy: 20 is the spec's ceiling */
static double s_NextDue;     /* monotonic time the next frame may be captured */

static int s_Driver = -1;    /* -1 undecided, 0 = ANSI, 1 = ffmpeg pipe */
static FILE *s_Pipe;
static int s_PipeW, s_PipeH;
static unsigned char *s_ScaleBuf;  /* live across frames; grows, never shrinks */
static size_t s_ScaleCap;
static unsigned char *s_AnsiPixels;
static size_t s_AnsiPixelsCap;
static char *s_AnsiText;
static size_t s_AnsiTextCap;
static int s_AnsiCleared;
static char s_LogPath[1024]; /* where stderr goes while the backend is on */

/* Where the PICTURE goes. Not stdout: the port logs through stdout too
 * (sysstubs.c sysLogPrintf, hitchlog.c), so writing the art there means the
 * game's own lines paint over it - and because they land at whatever column the
 * cursor reached, they leave the staircase of indented text that showed up in
 * the first real run. /dev/tty is a separate handle on the same terminal, so
 * the art can keep using it while stdout goes to the log with stderr. */
static FILE *s_Picture;
static int s_PictureIsTty;
static int s_AnsiUsed;      /* the ANSI path drew at least one frame */
static int s_AnsiCols;      /* geometry of the last painted frame, so a resize */
static int s_AnsiRows;      /* can clear what the old size left behind */

static double termNow(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart / (double) f.QuadPart;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
#endif
}

/* Grow-on-demand scratch. Returns NULL only on allocation failure, which a
 * frame later forgets about and tries again. */
static void *termScratch(unsigned char **buf, size_t *cap, size_t want)
{
    if (want <= *cap)
    {
        return *buf;
    }
    {
        unsigned char *n = (unsigned char *) realloc(*buf, want);
        if (n == NULL)
        {
            return NULL;
        }
        *buf = n;
        *cap = want;
    }
    return *buf;
}

/* ==== terminal geometry ==================================================== */

/* The descriptor to ask about terminal size: the picture's own handle
 * (the /dev/tty we opened) when the log was routed away, otherwise stdout.
 *
 * This mattered: routing stdout to the log left fd 1 pointing at a regular
 * file, so ioctl(STDOUT_FILENO, TIOCGWINSZ) failed, termSize fell back to
 * 80x24, and the art was drawn in a small corner of a much larger terminal. */
static int termTtyFd(void)
{
    int fd;

    if (s_Picture != NULL && s_PictureIsTty)
    {
        fd = fileno(s_Picture);
        if (fd >= 0)
        {
            return fd;
        }
    }
    return STDOUT_FILENO;
}

static void termSize(int *cols, int *rows)
{
    *cols = 80;
    *rows = 24;
#ifdef _WIN32
    {
        CONSOLE_SCREEN_BUFFER_INFO ci;

        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci))
        {
            *cols = ci.srWindow.Right - ci.srWindow.Left + 1;
            *rows = ci.srWindow.Bottom - ci.srWindow.Top + 1;
        }
    }
#else
    {
        struct winsize ws;

        if (ioctl(termTtyFd(), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
        {
            *cols = (int) ws.ws_col;
            *rows = (int) ws.ws_row;
        }
    }
#endif
    if (*cols < 8) *cols = 8;
    if (*rows < 2) *rows = 2;
}

/* ==== the ffmpeg side ====================================================== */

/* Resolving WHICH ffmpeg to run, once per process.
 *
 * This exists because the obvious version was wrong: it used `ffmpeg` from
 * PATH, which on macOS is Homebrew's, which is built WITHOUT --enable-libcaca.
 * The backend then reported "no caca device" and fell back to ANSI - while the
 * libcaca build that scripts/port/build_ffmpeg_caca.sh had just produced sat
 * unused in .deps/. Building a thing and then not using it is worse than not
 * building it, so the search prefers our own build.
 *
 * Order:
 *   1. MIDAS_TERM_FFMPEG - explicit, wins outright.
 *   2. .deps/ffmpeg-caca/bin/ffmpeg, from the cwd and from one level up (the
 *      game runs from build_macos/ while .deps/ is at the repo root).
 *   3. `ffmpeg` from PATH.
 * A candidate is only ACCEPTED at steps 2-3 if `-devices` actually lists caca,
 * so a stale or half-built copy falls through instead of failing at the pipe.
 */
static char s_FfmpegBin[1024];
static int s_FfmpegHasCaca;
static int s_FfmpegResolved;

/* Defined below, beside the probe it wraps. */
static int termFfmpegHasCaca(const char *bin);

static int termIsExecutable(const char *path)
{
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, X_OK) == 0;
#endif
}

static void termResolveFfmpeg(void)
{
    static const char *const candidates[] = {
        ".deps/ffmpeg-caca/bin/ffmpeg",
        "../.deps/ffmpeg-caca/bin/ffmpeg",
    };
    const char *e;
    size_t i;

    if (s_FfmpegResolved)
    {
        return;
    }
    s_FfmpegResolved = 1;

    e = getenv("MIDAS_TERM_FFMPEG");
    if (e != NULL && e[0] != '\0')
    {
        snprintf(s_FfmpegBin, sizeof(s_FfmpegBin), "%s", e);
        s_FfmpegHasCaca = termFfmpegHasCaca(s_FfmpegBin);
        return;
    }

    for (i = 0; i < sizeof(candidates) / sizeof(*candidates); i++)
    {
        if (termIsExecutable(candidates[i]) && termFfmpegHasCaca(candidates[i]))
        {
            snprintf(s_FfmpegBin, sizeof(s_FfmpegBin), "%s", candidates[i]);
            s_FfmpegHasCaca = 1;
            return;
        }
    }

    /* Nothing of ours: report honestly against whatever PATH offers, so the log
     * names the binary that lacked caca rather than a bare "ffmpeg". */
    snprintf(s_FfmpegBin, sizeof(s_FfmpegBin), "ffmpeg");
    s_FfmpegHasCaca = termFfmpegHasCaca("ffmpeg");
}

static const char *termFfmpegBin(void)
{
    termResolveFfmpeg();
    return s_FfmpegBin;
}

/* One popen at the first frame. `-devices` is cheap and needs no input; the
 * 2>&1 is inside the child's shell, so this stream sees the list whether
 * ffmpeg prints it to stdout or stderr. */
static int termFfmpegHasCaca(const char *bin)
{
    char cmd[1200];
    char line[512];
    FILE *p;
    int found = 0;

    if (snprintf(cmd, sizeof(cmd), "\"%s\" -hide_banner -devices 2>&1", bin) >= (int) sizeof(cmd))
    {
        return 0;
    }
    p = termPopen(cmd, "r");
    if (p == NULL)
    {
        return 0;
    }
    while (fgets(line, sizeof(line), p) != NULL)
    {
        if (strstr(line, "caca") != NULL)
        {
            found = 1;
            break;
        }
    }
    termPclose(p);
    return found;
}

/* At most `MIDAS_TERM_SIZE`, default 320 wide with the window's aspect kept:
 * the caca device maps whatever it is given onto the terminal, and 320x240
 * keeps the pipe (and the readback) small. Height is forced even so that
 * every consumer of the stream agrees on whole chroma pairs. */
static void termPipeSize(int w, int h, int *pw, int *ph)
{
    const char *e = getenv("MIDAS_TERM_SIZE");
    int tw = 0, th = 0;
    const int kMaxWide = 320;

    *pw = w;
    *ph = h;
    if (e != NULL && sscanf(e, "%dx%d", &tw, &th) == 2 && tw > 0 && th > 0)
    {
        *pw = tw;
        *ph = th;
        return;
    }
    if (w > kMaxWide)
    {
        *pw = kMaxWide;
        *ph = (int) (((long long) h * kMaxWide) / w);
    }
    if (*ph < 2) *ph = 2;
    if ((*ph & 1) != 0) (*ph)++;
}

static int termPipeOpen(const char *bin, int w, int h)
{
    char cmd[1400];
    int cols = 0, rows = 0;

    /* caca draws its canvas into the terminal, sized by the terminal itself.
     * Ask for a canvas two rows short so the art does not occupy the last two
     * lines. Measured caveat: caca's curses driver still emits output on the
     * final row (its own clear/restore), so this is a margin, NOT a guarantee.
     * What actually keeps the picture from jumping is the stderr redirect in
     * portTermBackendSetEnabled - without it, an interleaved `ge:` line scrolls
     * the screen even with the shorter canvas. */
    termSize(&cols, &rows);
    if (rows - TERM_RESERVED_ROWS >= 2)
    {
        rows -= TERM_RESERVED_ROWS;
    }
    if (cols > 400) cols = 400;

    if (snprintf(cmd, sizeof(cmd),
                 "\"%s\" -hide_banner -loglevel error -f rawvideo -pixel_format rgb24 "
                 "-video_size %dx%d -i - -window_size %dx%d -f caca -%s", bin, w, h, cols, rows,
                 s_PictureIsTty ? " > /dev/tty" : "")
        >= (int) sizeof(cmd))
    {
        return 0;
    }
#ifndef _WIN32
    /* A dead ffmpeg hands us EPIPE, which by default is a signal that kills
     * the game. The intent is to notice and fall back, not to die. */
    signal(SIGPIPE, SIG_IGN);
#endif
    s_Pipe = termPopen(cmd, "w");
    if (s_Pipe == NULL)
    {
        return 0;
    }
    s_PipeW = w;
    s_PipeH = h;
    portConLogf("ge: terminal: %s: pipe open: %dx%d rgb24 -> -f caca (%dx%d chars)\n", bin, w, h, cols, rows);
    return 1;
}

static void termPipeClose(void)
{
    if (s_Pipe != NULL)
    {
        termPclose(s_Pipe); /* also reaps the child */
        s_Pipe = NULL;
    }
}

/* ==== frame paths ========================================================== */

/* The stream the picture is drawn on: the terminal's own handle when the log
 * was moved aside, otherwise plain stdout. */
static FILE *termPicture(void)
{
    return (s_Picture != NULL) ? s_Picture : stdout;
}

static void termAnsiFrame(const unsigned char *rgb, int w, int h)
{
    int cols = 0, rows = 0;
    int dw, dh, n;
    size_t pxbytes, textcap;

    termSize(&cols, &rows);
    /* Reserve rows for the game's stderr log lines, which share this terminal. */
    rows -= TERM_RESERVED_ROWS;
    if (rows < 1) rows = 1;
    if (cols > 400) cols = 400; /* a runaway terminal width is not a picture */

    dw = cols;
    dh = rows * 2;
    pxbytes = (size_t) dw * (size_t) dh * 3;
    if (termScratch(&s_AnsiPixels, &s_AnsiPixelsCap, pxbytes) == NULL)
    {
        return;
    }
    if (!portTermDownsample(s_AnsiPixels, dw, dh, rgb, w, h, 1))
    {
        return;
    }

    textcap = (size_t) dw * (size_t) rows * TERM_BYTES_PER_CELL
            + (size_t) rows * TERM_BYTES_PER_ROW + 64;
    if (termScratch((unsigned char **) &s_AnsiText, &s_AnsiTextCap, textcap) == NULL)
    {
        return;
    }
    /* Rows are placed by an absolute cursor escape, NOT by newlines. That is
     * the fix for the picture bobbing up and down: a trailing newline left the
     * cursor on the terminal's LAST row, so the next stderr line from the game
     * scrolled the screen by one and the next repaint pulled it back. With
     * absolute positioning this frame cannot scroll the terminal at all. */
    n = portTermAnsiFrame(s_AnsiText, (int) s_AnsiTextCap, s_AnsiPixels, dw, dh, 1);
    if (n <= 0)
    {
        return;
    }

    if (!s_AnsiCleared || cols != s_AnsiCols || rows != s_AnsiRows)
    {
        /* Alternate screen: the player's scrollback is untouched and comes
         * back on exit. The clear is repeated only when the terminal size
         * changed, so a resize does not leave the old frame's tail rows behind
         * - and at a steady size nothing is cleared, which is what stops the
         * flicker a per-frame clear would give at 20 fps. */
        if (!s_AnsiCleared)
        {
            fputs("\x1b[?1049h", termPicture());
            s_AnsiCleared = 1;
        }
        fputs("\x1b[2J\x1b[?25l", termPicture());
        s_AnsiCols = cols;
        s_AnsiRows = rows;
    }
    s_AnsiUsed = 1;
    fwrite(s_AnsiText, 1, (size_t) n, termPicture());
    /* Park the cursor in the first reserved row, so a `ge:` line lands below
     * the art instead of overwriting its bottom row. */
    fprintf(termPicture(), "\x1b[%d;1H", rows + 1);
    fflush(termPicture());
}

static int termPipeFrame(const unsigned char *rgb, int w, int h)
{
    size_t bytes = (size_t) s_PipeW * (size_t) s_PipeH * 3;

    if (termScratch(&s_ScaleBuf, &s_ScaleCap, bytes) == NULL)
    {
        return 1; /* not the pipe's fault; skip the frame rather than fall back */
    }
    if (!portTermDownsample(s_ScaleBuf, s_PipeW, s_PipeH, rgb, w, h, 1))
    {
        return 1;
    }
    if (fwrite(s_ScaleBuf, 1, bytes, s_Pipe) != bytes || fflush(s_Pipe) != 0)
    {
        return 0; /* broken */
    }
    return 1;
}

/* ==== the decision ========================================================= */

static void termPrepare(int w, int h)
{
    const char *force;
    const char *bin = termFfmpegBin();
    int pw = 0, ph = 0;

    if (s_Driver != -1)
    {
        return;
    }
    termPipeSize(w, h, &pw, &ph);

    /* The command line outranks the environment, the port's usual precedence:
     * --backend=ansi must beat a stale MIDAS_TERM_DRIVER=ffmpeg. */
    if (s_ForcedDriver == PORT_TERM_DRIVER_ANSI)
    {
        s_Driver = 0;
        portConLogf("ge: terminal: ANSI driver requested -> half-block fallback\n");
        return;
    }
    if (s_ForcedDriver == PORT_TERM_DRIVER_FFMPEG)
    {
        if (termPipeOpen(bin, pw, ph))
        {
            s_Driver = 1;
        }
        else
        {
            s_Driver = 0;
            portConLogf("ge: terminal: ffmpeg driver requested but the pipe did not open -> ANSI\n");
        }
        return;
    }

    force = getenv("MIDAS_TERM_DRIVER");
    if (force != NULL && strcmp(force, "ansi") == 0)
    {
        s_Driver = 0;
        portConLogf("ge: terminal: MIDAS_TERM_DRIVER=ansi -> ANSI half-block fallback\n");
        return;
    }
    if (force != NULL && strcmp(force, "ffmpeg") == 0)
    {
        if (termPipeOpen(bin, pw, ph))
        {
            s_Driver = 1;
        }
        else
        {
            s_Driver = 0;
            portConLogf("ge: terminal: MIDAS_TERM_DRIVER=ffmpeg but the pipe did not open -> ANSI\n");
        }
        return;
    }

    if (s_FfmpegHasCaca)
    {
        if (termPipeOpen(bin, pw, ph))
        {
            s_Driver = 1;
            return;
        }
        portConLogf("ge: terminal: %s has a caca device but the pipe did not open -> ANSI\n", bin);
    }
    else
    {
        portConLogf("ge: terminal: %s has no caca device -> ANSI 24-bit fallback\n", bin);
        portConLogf("ge: terminal: run scripts/port/build_ffmpeg_caca.sh to get the real "
                    "caca renderer, or point MIDAS_TERM_FFMPEG at one\n");
    }
    s_Driver = 0;
}

/* ==== public API =========================================================== */

void portTermBackendSetLogPath(const char *path)
{
    if (path != NULL && path[0] != '\0')
    {
        snprintf(s_LogPath, sizeof(s_LogPath), "%s", path);
    }
}

void portTermBackendSetDriver(int driver)
{
    if (driver >= PORT_TERM_DRIVER_AUTO && driver <= PORT_TERM_DRIVER_ANSI)
    {
        s_ForcedDriver = driver;
    }
}

void portTermBackendSetEnabled(int on)
{
    const char *e;

    if (!on)
    {
        s_Enabled = 0;
        s_Active = 0;
        return;
    }
    if (s_Enabled)
    {
        return;
    }
    s_Enabled = 1;
    s_Active = 1;

    e = getenv("MIDAS_TERM_FPS");
    if (e != NULL)
    {
        double fps = atof(e);
        if (fps >= 1.0 && fps <= 240.0)
        {
            s_Fps = fps;
        }
    }
    if (!s_ShutdownArmed)
    {
        s_ShutdownArmed = 1;
        atexit(portTermBackendShutdown);
    }
    portConLogf("ge: terminal: -terminal -> %.0f fps cap, driver decided at the first frame\n", s_Fps);

    /* Move the game's log off the terminal before the first frame. Both
     * renderers paint the whole screen, so a `ge:` line would otherwise land
     * on the art - and on the last row it scrolls the terminal, which is what
     * made the picture bob. Done AFTER the line above so the player still sees
     * the mode and the rate; everything after this point goes to the file. */
    if (s_LogPath[0] != '\0' && getenv("MIDAS_TERM_NOLOG") == NULL)
    {
        FILE *tty;

        fflush(NULL);
        /* The picture's own handle first, while stdout is still the terminal. */
#ifdef _WIN32
        tty = fopen("CONOUT$", "w");
#else
        tty = fopen("/dev/tty", "w");
#endif
        if (tty != NULL)
        {
            s_Picture = tty;
            s_PictureIsTty = 1;
        }
        else
        {
            /* No controlling terminal (output piped or redirected): the art
             * and the log have to share stdout. Say so once - a picture with
             * log lines through it otherwise looks like a rendering bug. */
            s_Picture = stdout;
            s_PictureIsTty = 0;
        }

        /* ONE stream, duplicated onto both descriptors.
         *
         * freopen(s_LogPath, "w", stderr) followed by freopen(s_LogPath, "a",
         * stdout) looks equivalent and is not: it creates TWO file
         * descriptions with independent offsets, so whichever stream flushes
         * second lands at its own idea of "append" and the other's lines are
         * lost or overwritten. That is why the `SDL:`/`GL:` lines (printf,
         * i.e. stdout) vanished from the log while portConLogf's (stderr)
         * survived. dup2 gives both fds the same description, hence one offset
         * and one ordering. */
        if (s_PictureIsTty)
        {
            FILE *log = fopen(s_LogPath, "w");

            if (log != NULL)
            {
#ifdef _WIN32
                _dup2(_fileno(log), _fileno(stdout));
                _dup2(_fileno(log), _fileno(stderr));
#else
                dup2(fileno(log), STDOUT_FILENO);
                dup2(fileno(log), STDERR_FILENO);
#endif
                fclose(log); /* the descriptors keep the file open */
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0);
            }
        }
        if (s_PictureIsTty)
        {
            fprintf(s_Picture, "ge: terminal: game log -> %s (art on /dev/tty)\n", s_LogPath);
            fflush(s_Picture);
        }
        else
        {
            portConLogf("ge: terminal: no controlling terminal - the log and the art "
                        "share stdout, so log lines will overwrite the picture\n");
        }
    }
}

int portTermBackendActive(void)
{
    return s_Active;
}

int portTermBackendHidesWindow(void)
{
    return s_Active && getenv("MIDAS_TERM_SHOWWINDOW") == NULL;
}

int portTermBackendWantsFrame(void)
{
    double now;

    if (!s_Active)
    {
        return 0;
    }
    now = termNow();
    if (now < s_NextDue)
    {
        return 0;
    }
    /* Reschedule from NOW, not from the previous due time: a machine that
     * cannot hit the cap should drop frames, not build up a backlog of
     * captures that would then run back to back. */
    s_NextDue = now + 1.0 / s_Fps;
    return 1;
}

void portTermBackendSubmitFrame(const unsigned char *rgb, int w, int h)
{
    if (!s_Active || rgb == NULL || w <= 0 || h <= 0)
    {
        return;
    }
    termPrepare(w, h);

    if (s_Driver == 1 && s_Pipe != NULL)
    {
        if (termPipeFrame(rgb, w, h))
        {
            return;
        }
        portConLogf("ge: terminal: the ffmpeg pipe broke - continuing on the ANSI fallback\n");
        termPipeClose();
        s_Driver = 0;
    }
    termAnsiFrame(rgb, w, h);
}

void portTermBackendShutdown(void)
{
    termPipeClose();
    if (s_AnsiUsed)
    {
        /* Reset SGR, give the cursor back, and leave the alternate screen so
         * the player's own scrollback is restored. Only when this backend DREW
         * - under the ffmpeg driver, libcaca restores the terminal itself. */
        fputs("\x1b[0m\x1b[?25h\x1b[?1049l", termPicture());
        fflush(termPicture());
    }
    s_Active = 0;
    /* The scratch buffers are deliberately NOT freed here. This runs from
     * atexit on the main thread, and the port's render thread (video.c
     * portRenderThreadProc) may be mid-frame on the same pointers - freeing
     * them would be a use-after-free on the way out. The process is ending, so
     * the OS reclaims them. */
}

/* ==== pure helpers (port/tests/term_backend.c) ============================= */

int portTermDownsample(unsigned char *dst, int dw, int dh,
                       const unsigned char *src, int sw, int sh, int flip_y)
{
    int dy, dx;

    if (dst == NULL || src == NULL || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
    {
        return 0;
    }

    for (dy = 0; dy < dh; dy++)
    {
        int y0 = (int) (((long long) dy * sh) / dh);
        int y1 = (int) (((long long) (dy + 1) * sh) / dh);

        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        if (flip_y)
        {
            /* source row 0 is the bottom; destination row 0 is the top */
            const int a = sh - y1;
            const int b = sh - y0;
            y0 = a;
            y1 = b;
        }

        for (dx = 0; dx < dw; dx++)
        {
            int x0 = (int) (((long long) dx * sw) / dw);
            int x1 = (int) (((long long) (dx + 1) * sw) / dw);
            unsigned int r = 0, g = 0, b = 0, n = 0;
            int y, x;

            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            for (y = y0; y < y1; y++)
            {
                const unsigned char *row = src + ((size_t) y * (size_t) sw + (size_t) x0) * 3;

                for (x = 0; x < x1 - x0; x++)
                {
                    r += row[x * 3 + 0];
                    g += row[x * 3 + 1];
                    b += row[x * 3 + 2];
                }
                n += (unsigned int) (x1 - x0);
            }
            if (n == 0)
            {
                n = 1;
            }
            dst[((size_t) dy * (size_t) dw + (size_t) dx) * 3 + 0] = (unsigned char) ((r + n / 2) / n);
            dst[((size_t) dy * (size_t) dw + (size_t) dx) * 3 + 1] = (unsigned char) ((g + n / 2) / n);
            dst[((size_t) dy * (size_t) dw + (size_t) dx) * 3 + 2] = (unsigned char) ((b + n / 2) / n);
        }
    }
    return 1;
}

int portTermAnsiFrame(char *dst, int cap, const unsigned char *rgb, int w, int h, int row0)
{
    int at = 0;
    int rows;
    int x, y;
    int fg[3] = { -1, -1, -1 };
    int bg[3] = { -1, -1, -1 };

    if (dst == NULL || rgb == NULL || w <= 0 || h <= 0 || cap <= 0 || row0 < 1)
    {
        return -1;
    }
    rows = (h + 1) / 2;

    for (y = 0; y < rows; y++)
    {
        /* An odd height repeats the last row: half a cell of colour is a
         * better answer than dropping the bottom pixel row entirely. */
        const int topRow = y * 2;
        const int botRow = (y * 2 + 1 < h) ? (y * 2 + 1) : (h - 1);
        const unsigned char *top = rgb + (size_t) topRow * (size_t) w * 3;
        const unsigned char *bot = rgb + (size_t) botRow * (size_t) w * 3;
        int n;

        /* Absolute row placement. No '\n' is emitted anywhere in this
         * function, so a frame can never advance past the last row and scroll
         * the terminal - which is what made the picture bob up and down. */
        n = snprintf(dst + at, (size_t) (cap - at), "\x1b[%d;1H", row0 + y);
        if (n < 0 || n >= cap - at)
        {
            return -1;
        }
        at += n;

        for (x = 0; x < w; x++)
        {
            const unsigned char *t = top + (size_t) x * 3;
            const unsigned char *b = bot + (size_t) x * 3;

            if (t[0] != fg[0] || t[1] != fg[1] || t[2] != fg[2] ||
                b[0] != bg[0] || b[1] != bg[1] || b[2] != bg[2])
            {
                n = snprintf(dst + at, (size_t) (cap - at),
                             "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm",
                             (int) t[0], (int) t[1], (int) t[2],
                             (int) b[0], (int) b[1], (int) b[2]);
                if (n < 0 || n >= cap - at)
                {
                    return -1;
                }
                at += n;
                fg[0] = t[0]; fg[1] = t[1]; fg[2] = t[2];
                bg[0] = b[0]; bg[1] = b[1]; bg[2] = b[2];
            }
            if (cap - at < 3)
            {
                return -1;
            }
            memcpy(dst + at, TERM_HALF_BLOCK, 3);
            at += 3;
        }
        /* Erase to end of line: a frame narrower than the previous one (after
         * a resize) would otherwise leave its tail behind. */
        if (cap - at < 3)
        {
            return -1;
        }
        memcpy(dst + at, "\x1b[K", 3);
        at += 3;
    }
    return at;
}
