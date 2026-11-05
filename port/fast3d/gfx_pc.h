#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <list>
#include <cstddef>

#include <PR/gbi.h>

#include "system.h"

#define SCREEN_WIDTH ((int32_t)gfx_current_native_viewport.width)
#define SCREEN_HEIGHT ((int32_t)gfx_current_native_viewport.height)

extern uintptr_t gfxFramebuffer;

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct TextureCacheKey {
    const uint8_t* texture_addr;
    const uint8_t* palette_addrs[2];
    uint8_t fmt, siz;
    uint8_t palette_index;
    /* GEOMETRY IS PART OF THE IDENTITY (triage F2.1, Control mainframe
     * "sometimes corrupted"): keyed on address+format alone, a second
     * texture that lands on the same address - a reused dynAllocate
     * block, an animated screen's next frame buffer, another level of a
     * mip chain addressed by TMEM offset - hit the FIRST one's cached GL
     * texture. The draw then sampled the old image at the new tile's UV
     * scale: the wrong picture, tiled and repeating across the surface,
     * and only when the allocator happened to collide (hence "sometimes"). */
    uint32_t line_size_bytes;
    uint32_t size_bytes;
    /* CI textures: fingerprint of the palette CONTENT, not just its
     * address - GE reloads different TLUTs into the same buffer (see
     * rdp.palette_sum). 0 for non-CI formats. */
    uint32_t palette_sum;
    /* GE: wrap masks are part of the identity - a tile whose mask is
     * NARROWER than its load line is imported cropped to the mask
     * (ge_upload_masked); the same bytes drawn with other masks must not
     * reuse that entry. */
    uint8_t masks, maskt;

    /* PD defaulted this with `= default`, which needs C++20 (the Miyoo cross
     * toolchain is GCC 8.3). Written out: every member, in declaration order,
     * which is exactly what the defaulted operator did - the cache identity is
     * address + palette + format + geometry + wrap masks, and dropping any one
     * term reintroduces the stale-texture bugs the comments above describe.
     * `!=` is spelled out too: in C++20 `a != b` rewrites to `!(a == b)`, and
     * that rewrite does not exist at C++17. */
    bool operator==(const TextureCacheKey& o) const noexcept {
        return texture_addr == o.texture_addr
            && palette_addrs[0] == o.palette_addrs[0]
            && palette_addrs[1] == o.palette_addrs[1]
            && fmt == o.fmt
            && siz == o.siz
            && palette_index == o.palette_index
            && line_size_bytes == o.line_size_bytes
            && size_bytes == o.size_bytes
            && palette_sum == o.palette_sum
            && masks == o.masks
            && maskt == o.maskt;
    }

    bool operator!=(const TextureCacheKey& o) const noexcept {
        return !(*this == o);
    }

    struct Hasher {
        size_t operator()(const TextureCacheKey& key) const noexcept {
            uintptr_t addr = (uintptr_t)key.texture_addr;
            return (size_t)(addr ^ (addr >> 5));
        }
    };
};

typedef std::unordered_map<TextureCacheKey, struct TextureCacheValue, TextureCacheKey::Hasher> TextureCacheMap;
typedef std::pair<const TextureCacheKey, struct TextureCacheValue> TextureCacheNode;

struct TextureCacheValue {
    uint32_t texture_id;
    uint32_t serial;     /* unique per node creation (GE Xbox retained caches validate on it: a
                          * texture_id is recycled by the next import after a delete) */
    uint16_t pins;       /* retained lists holding this node: the LRU evicts unpinned nodes first */
    uint8_t cms, cmt;
    bool linear_filter;
    bool mip_filter; /* GE: minifying through a mip chain (N64 G_TL_LOD draw) */

    std::list<struct TextureCacheMapIter>::iterator lru_location;
};

struct TextureCacheMapIter {
    TextureCacheMap::iterator it;
};

extern "C" {

#include "gfx_api.h"

}

#endif
