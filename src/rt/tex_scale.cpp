/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_scale.cpp - the C-linkage wrapper around xBRZ (third_party/xbrz, GPL-3.0).
 *
 * The runtime's decoded textures are 0xAABBGGRR words: the upload path treats them as
 * VK_FORMAT_R8G8B8A8_UNORM, so on a little-endian host the red channel sits in the lowest
 * byte. xBRZ's argb format is 0xAARRGGBB, and its colour distance is YCbCr-weighted, so
 * feeding the runtime's words straight in would hand the edge rules red and blue swapped.
 * The red and blue bytes are exchanged before and after the scale instead: two linear
 * passes over at most 512x512 words, far cheaper than the scale itself, and upstream stays
 * byte-identical.
 *
 * argbUnbuffered rather than argb: the buffered format builds its lookup table on first use
 * (ca. 100-300 ms per upstream's header), which would stall the first texture upload of a
 * run. The unbuffered form is slightly slower per call and pays nothing up front; a texture
 * is scaled once per content change and cached (ge_gpu.c), so the per-call cost is the
 * right trade.
 */
#include "tex_scale.h"

#include <stdlib.h>

#include "xbrz.h"

namespace {

inline uint32_t swap_rb(uint32_t v) {
    /* 0xAABBGGRR -> 0xAARRGGBB: alpha and green stay, red and blue trade places. */
    return (v & 0xFF00FF00u) | ((v >> 16) & 0x000000FFu) | ((v & 0x000000FFu) << 16);
}

} // namespace

int tex_scale_rgba(const uint32_t *src, int w, int h, uint32_t *dst, int factor) {
    if (!src || !dst || w <= 0 || h <= 0 || w > 512 || h > 512) return 0;
    if (factor < 2 || factor > TEX_SCALE_MAX) return 0;
    const size_t n = (size_t)w * (size_t)h;
    /* The scratch buffers keep the caller's arrays untouched; a failed allocation refuses
     * the scale and the caller uploads the native texture instead (fail closed). */
    uint32_t *swapped_src = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *swapped_dst = (uint32_t *)malloc(n * (size_t)factor * (size_t)factor * sizeof(uint32_t));
    if (!swapped_src || !swapped_dst) {
        free(swapped_src);
        free(swapped_dst);
        return 0;
    }
    for (size_t i = 0; i < n; i++) swapped_src[i] = swap_rb(src[i]);
    xbrz::scale((size_t)factor, swapped_src, swapped_dst, w, h,
                xbrz::ColorFormat::argbUnbuffered);
    const size_t out_n = n * (size_t)factor * (size_t)factor;
    for (size_t i = 0; i < out_n; i++) dst[i] = swap_rb(swapped_dst[i]);
    free(swapped_src);
    free(swapped_dst);
    return 1;
}
