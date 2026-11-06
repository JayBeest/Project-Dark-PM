/*
 * Boot flags - implementation. See boot_flags.h for the matching rules and why
 * they are tested rather than assumed.
 */

#include <stddef.h>

#include "boot_flags.h"

/* Case-insensitive, with '-' and '_' treated as the same character. Written
 * out rather than calling strcasecmp so this file needs neither <string.h>'s
 * locale-sensitive behaviour nor SDL. */
static char lowerDash(char c)
{
    if (c == '-') {
        return '_';
    }
    if (c >= 'A' && c <= 'Z') {
        return (char) (c - 'A' + 'a');
    }
    return c;
}

int portBootFlagIs(const char *arg, const char *name)
{
    if (arg == NULL || name == NULL) {
        return 0;
    }
    /* At least one dash, or this is not a flag. A bare first argument is the
     * drag-and-drop ROM path; nothing may consume it. */
    if (arg[0] != '-') {
        return 0;
    }
    while (*arg == '-') {
        arg++;
    }
    while (*arg != '\0' && *name != '\0') {
        if (lowerDash(*arg) != lowerDash(*name)) {
            return 0;
        }
        arg++;
        name++;
    }
    /* Both must end together: "-audio" must not match "audio_pump". */
    return (*arg == '\0' && *name == '\0') ? 1 : 0;
}

/* Defined below, beside the other =value matcher: --backend=caca and
 * --backend=terminal are -terminal. Forward-declared because the parse above
 * is where it is used. */
static int flagIsTerminal(const char *arg);
static int flagIsTerminalGlitch(const char *arg);
static int terminalDriverFor(const char *arg);

/* A whole decimal argument, or -1. Used for -skip's count: "3" is a count,
 * "dam" and "-audio_pump" are not, and neither is anything too long to be
 * one. Zero counts: -skip 0 is the same as not passing -skip. */
static int argIsCount(const char *a)
{
    int n = 0;
    int i;

    if (a == NULL || a[0] == '\0') {
        return -1;
    }
    for (i = 0; a[i] != '\0'; i++) {
        if (a[i] < '0' || a[i] > '9' || i >= 3) {
            return -1;   /* not a count (or absurd: the chain is 5 long) */
        }
        n = n * 10 + (a[i] - '0');
    }
    return n;
}

void portBootFlagsParse(int argc, char **argv, PortBootFlags *out)
{
    int i;
    int skip_all = 0;   /* a bare -skip: the whole chain */
    int skip_n = 0;     /* -skip N: that many */

    if (out == NULL) {
        return;
    }
    out->classic = 0;
    out->audio_pump = 0;
    out->audio_log = 0;
    out->skip_screens = 0;
    out->stage = NULL;
    out->terminal = 0;
    out->terminal_glitch = 0;
    out->terminal_driver = 0;
    out->terminal_spelling = NULL;
    out->classic_spelling = NULL;

    if (argv == NULL) {
        return;
    }
    for (i = 1; i < argc; i++) {
        if (portBootFlagIs(argv[i], "classic") || portBootFlagIs(argv[i], "n64_aa")) {
            out->classic = 1;
            out->classic_spelling = argv[i];
        } else if (portBootFlagIs(argv[i], "audio_pump")) {
            out->audio_pump = 1;
        } else if (portBootFlagIs(argv[i], "audio_log")) {
            out->audio_log = 1;
        } else if (portBootFlagIs(argv[i], "skip")) {
            /* The count is the NEXT argument when it is a number, which is how
             * --rom-file and --lang already take their values. A non-number is
             * left alone for whatever else wanted it, and the flag keeps its
             * bare meaning: skip the whole boot chain. */
            const int n = (i + 1 < argc) ? argIsCount(argv[i + 1]) : -1;

            if (n >= 0) {
                skip_n = n;
                i++;             /* consumed */
            } else {
                skip_all = 1;
            }
        } else if (portBootFlagIs(argv[i], "stage")) {
            /* The next argument, unless it is another flag: -stage with nothing
             * after it is a mistake, not a request to load a stage called
             * "-audio_pump". */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                out->stage = argv[++i];
            }
        } else if (flagIsTerminalGlitch(argv[i])) {
            out->terminal = 1;
            out->terminal_glitch = 1;
            out->terminal_spelling = argv[i];
        } else if (flagIsTerminal(argv[i])) {
            int drv;

            out->terminal = 1;
            out->terminal_spelling = argv[i];
            /* First driver request wins, so `-terminal --backend=ansi` means
             * auto: the bare flag stated no preference and a later one does.
             * A SECOND --backend=ansi after --backend=ffmpeg overwrites, which
             * is last-one-wins like the rest of this command line. */
            drv = terminalDriverFor(argv[i]);
            if (drv > 0) {
                out->terminal_driver = drv;
            }
        }
    }
    /* A bare -skip outranks a count: it is the more aggressive request, and
     * "-skip 2 -skip" should not quietly skip two. */
    out->skip_screens = skip_all ? -1 : skip_n;
}

/* 1 when `arg` matches `name` followed by '=' and anything: the `--flag=value`
 * form. Same dash and separator rules as portBootFlagIs, then a strict '='. */
static const char *flagAssignmentValue(const char *arg, const char *name)
{
    if (arg == NULL || name == NULL || arg[0] != '-') {
        return NULL;
    }
    while (*arg == '-') {
        arg++;
    }
    while (*arg != '\0' && *name != '\0') {
        if (lowerDash(*arg) != lowerDash(*name)) {
            return NULL;
        }
        arg++;
        name++;
    }
    return (*name == '\0' && *arg == '=') ? arg + 1 : NULL;
}

static int flagIsAssignment(const char *arg, const char *name)
{
    return flagAssignmentValue(arg, name) != NULL;
}

/* Case-insensitive equality for a flag's value (which has no dashes to fold). */
static int valueIs(const char *value, const char *want)
{
    if (value == NULL) {
        return 0;
    }
    while (*value != '\0' && *want != '\0') {
        char a = *value;
        char b = *want;
        if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
        if (a != b) {
            return 0;
        }
        value++;
        want++;
    }
    return (*value == '\0' && *want == '\0') ? 1 : 0;
}

/* --backend=caca / --backend=terminal: the same request as -terminal.
 *
 * Only the implemented values count. `--backend=sdl` would otherwise be a no-op
 * that looks like it did something, so it is NOT a known flag:
 * videoParseBootArgs' existing "ignoring argument" report names it instead. */
static int flagIsTerminalBackend(const char *arg)
{
    const char *v = flagAssignmentValue(arg, "backend");

    return v != NULL && (valueIs(v, "caca") || valueIs(v, "terminal")
                         || valueIs(v, "ffmpeg") || valueIs(v, "ansi"));
}

static int flagIsTerminal(const char *arg)
{
    return portBootFlagIs(arg, "terminal") || flagIsTerminalBackend(arg);
}

/* The driver a -terminal spelling asks for, or -1 when it only asks for the
 * mode: 1 = ffmpeg caca, 2 = the built-in ANSI renderer. These numbers are the
 * PortTermDriver enum in term_backend.h (kept numeric to avoid a header
 * dependency from this pure parse). */
static int terminalDriverFor(const char *arg)
{
    const char *v = flagAssignmentValue(arg, "backend");

    if (v != NULL)
    {
        if (valueIs(v, "ffmpeg")) return 1;
        if (valueIs(v, "ansi")) return 2;
    }
    return -1;
}

/* -terminal_glitch: the terminal-mode log toggle. Implies -terminal, because
 * "show me the log on the terminal" is meaningless without the terminal mode
 * that owns the screen. Named for what it looks like, not for what it does:
 * the game's `ge:` lines stamp themselves over the art at whatever column the
 * cursor reached, leaving a staircase. MIDAS_TERM_NOLOG=1 is the same switch
 * from the environment. */
static int flagIsTerminalGlitch(const char *arg)
{
    return portBootFlagIs(arg, "terminal_glitch") || portBootFlagIs(arg, "glitch");
}

int portBootFlagKnown(const char *arg)
{
    if (arg == NULL) {
        return 0;
    }

    /* The three this file parses. -classic and -n64_aa are one flag, so both
     * spellings must be known: reporting the documented one as ignored is the
     * defect portBootFlagKnown exists to prevent. -skip and -stage take their
     * value from the NEXT argument, so only the bare flag is a flag here. */
    if (portBootFlagIs(arg, "classic") || portBootFlagIs(arg, "n64_aa")
        || portBootFlagIs(arg, "audio_pump") || portBootFlagIs(arg, "audio_log")
        || portBootFlagIs(arg, "skip") || portBootFlagIs(arg, "stage")
        || flagIsTerminal(arg) || flagIsTerminalGlitch(arg)) {
        return 1;
    }

    /* The two romdata.c parses: the bare flag (its value is the NEXT argument)
     * and the `=value` form. Both are documented, and both must not be reported
     * as unknown. */
    if (portBootFlagIs(arg, "rom-file") || portBootFlagIs(arg, "lang")
        || flagIsAssignment(arg, "rom-file") || flagIsAssignment(arg, "lang")) {
        return 1;
    }

    return 0;
}
