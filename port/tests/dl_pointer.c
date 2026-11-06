/*
 * PORT_DL_POINTER_FITS: the rule that keeps a 64-bit host pointer out of a
 * 32-bit Gfx word.
 *
 * This exists because the port shipped the other half of the rule. Callers
 * checked "is this >= 0x10000000?" - true, and correct, on every 32-bit build
 * the port grew up on. It is also true for EVERY allocation on arm64 macOS,
 * where user space starts at 4 GB, so `(u32)(uintptr_t)p` quietly truncated and
 * seg_addr resolved the remnant as a segment reference. The crosshair tint copy
 * was written into the display list that way.
 *
 * The last check is the one that matters: it takes a real allocation and shows
 * it does not fit. On a 32-bit host that assertion is expected to fail
 * differently, so it is guarded by pointer width rather than assumed.
 *
 * Build standalone:
 *   cc -std=c99 -Iport/include port/tests/dl_pointer.c -o t
 *   ./t
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "port_hostptr.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); } } while (0)

/* The two bounds, at their edges. A word below the floor is a segment
 * reference; a word above the ceiling does not exist. */
static void bounds(void)
{
    CHECK(!PORT_DL_POINTER_FITS((void *) (uintptr_t) 0));
    CHECK(!PORT_DL_POINTER_FITS((void *) (uintptr_t) 0x0FFFFFFF));
    CHECK(PORT_DL_POINTER_FITS((void *) (uintptr_t) 0x10000000));
    CHECK(PORT_DL_POINTER_FITS((void *) (uintptr_t) 0x20000000));
    CHECK(PORT_DL_POINTER_FITS((void *) (uintptr_t) 0x7FFFFFFF));
    CHECK(PORT_DL_POINTER_FITS((void *) (uintptr_t) 0xFFFFFFFF));
}

/* The property the macro exists for: anything it accepts round-trips through
 * u32 unchanged. Anything else would be silently corrupted by the cast. */
static void accepted_values_round_trip(void)
{
    const uintptr_t samples[] = { 0x10000000u, 0x12345678u, 0x7FFFFFFFu, 0xFFFFFFFFu };
    size_t i;

    for (i = 0; i < sizeof(samples) / sizeof(*samples); i++) {
        void *p = (void *) samples[i];
        if (PORT_DL_POINTER_FITS(p)) {
            CHECK((uintptr_t) (uint32_t) (uintptr_t) p == (uintptr_t) p);
        }
    }
}

/* A real heap pointer. On LP64 this is the case that bit the port: it passes
 * the >= 0x10000000 test that every caller used to write, and fails this one. */
static void heap_pointer_on_lp64(void)
{
    void *p = malloc(64);

    CHECK(p != NULL);
    printf("  heap pointer      : %p\n", p);
    printf("  >= 0x10000000     : %s\n",
           PORT_IS_HOST_ADDR(p) ? "yes" : "no");
    printf("  fits a Gfx word   : %s\n",
           PORT_DL_POINTER_FITS(p) ? "yes" : "NO");

    if (sizeof(uintptr_t) > 4) {
        /* arm64 macOS: user space starts at 4 GB, so nothing from mmap or the
         * heap can satisfy this. If a future OS change makes one fit, this
         * assertion failing is the signal to revisit - not a bug in the macro. */
        CHECK(!PORT_DL_POINTER_FITS(p));
    }
    free(p);
}

int main(void)
{
    bounds();
    accepted_values_round_trip();
    heap_pointer_on_lp64();

    printf("dl_pointer: all checks passed\n");
    return 0;
}
