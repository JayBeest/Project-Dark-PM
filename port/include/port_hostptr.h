/**
 * PORT_IS_HOST_ADDR(v): is this 32-bit word a real host pointer, as opposed to
 * an N64 segment word (0x0Sxxxxxx) or a file offset?
 *
 * On PC, host pointers always live at or above 0x10000000: the Windows image is
 * rebased there, the Linux image sits at 0x08048000 (segments 8/9 are never
 * installed), and both arenas are pinned at 0x20000000+.
 *
 * The original Xbox can't follow that rule: the XBE base is forced to
 * 0x00010000, so the whole image (assets and static buffers) overlaps the N64
 * segment 0/1 word range. There the test is "above 0x10000000 OR inside the
 * image" (xbox_sys.c reads the image size from the XBE header at boot). Heap
 * allocations never hold display-list data, and every other display-list
 * allocation goes through the arenas. See docs/port/xbox-port.md.
 */
#ifndef PORT_HOSTPTR_H
#define PORT_HOSTPTR_H

#ifdef GE_XBOX
#ifdef __cplusplus
extern "C" {
#endif
int portXboxIsHostAddr(unsigned int v);
#ifdef __cplusplus
}
#endif
#define PORT_IS_HOST_ADDR(v) (portXboxIsHostAddr((unsigned int) (v)))
#else
#include <stdint.h>
#define PORT_IS_HOST_ADDR(v) ((uintptr_t) (v) >= 0x10000000u)
#endif

/*
 * PORT_DL_POINTER_FITS(p): can `p` be baked into a 32-bit Gfx word?
 *
 * A Gfx word is 32 bits, so a host pointer stored in one must survive
 * (u32)(uintptr_t) AND still satisfy PORT_IS_HOST_ADDR - otherwise seg_addr
 * reads the truncated value as a segment reference and resolves it to a wild
 * address. Both halves are required; the >= 0x10000000 test alone is not, and
 * that was the bug: on arm64 macOS the user address space STARTS at
 * 0x100000000 (4 GB), so every mmap result is >= 0x10000000 and fits in 32
 * bits never.
 *
 * Measured on this machine (macOS arm64): mmap ignores the high-arena hint and
 * returns ~0x104afc000 whatever you ask for; MAP_FIXED below 4 GB fails
 * because nothing may live there. So on Apple Silicon this is false for EVERY
 * heap pointer, and a caller that must bake one into a display list has to
 * degrade gracefully rather than truncate.
 *
 * The lower bound is PORT_IS_HOST_ADDR, not a literal 0x10000000: the Xbox
 * build rebases its image to 0x00010000 and legitimately has host pointers
 * below the PC floor. On any 32-bit target the upper bound is vacuous, so this
 * reduces exactly to the old rule there.
 */
#define PORT_DL_POINTER_FITS(p) \
    (PORT_IS_HOST_ADDR(p) && (uintptr_t) (p) <= 0xFFFFFFFFu)

#endif
