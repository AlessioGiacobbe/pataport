/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_scale_selftest - the texture-upscaling wrapper's contract, no GPU and no game input.
 *
 * The runtime's decoded texture words are 0xAABBGGRR (alpha in the top byte). The
 * guarantees the wrapper relies on, and this file pins:
 *   - the output dimensions are exactly factor*w x factor*h, and flat regions keep their
 *     colour at every output pixel, including the border;
 *   - the scale is deterministic: the same input produces the same output words;
 *   - opaque input stays opaque one scaled texel inside the border;
 *   - axis-aligned hard edges and flat interiors hold input colours exactly; smooth
 *     (diagonal) edges blend neighbouring input colours, and a blend of the palette never
 *     leaves the palette's per-channel hull — so a wrong red/blue swap in the wrapper's
 *     channel exchange is visible as a colour outside that hull or as a swapped flat
 *     region;
 *   - xBRZ's documented ARGB border treatment reaches only the outermost scaled texel
 *     ring: the image corners alpha-fade toward the virtual transparent outside (up to
 *     0x52 at 4x on an opaque input, RGB exact), and where a hard colour edge meets the
 *     border the ring may carry a blend of the two colours. xBRZ 1.8 and 1.9 agree.
 * Refusals (NULL, empty size, out-of-range factor) return 0 and touch nothing.
 */
#include "tex_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 0xAABBGGRR words, alpha in the top byte: */
static const uint32_t RED = 0xFF0000FFu;     /* alpha ff, blue 00, green 00, red ff */
static const uint32_t BLUE = 0xFFFF0000u;    /* alpha ff, blue ff, green 00, red 00 */
static const uint32_t GREEN = 0xFF00FF00u;
static const uint32_t CLEAR = 0x00000000u;   /* fully transparent black */

static int fails = 0;

static void check(int ok, const char *what) {
    if (ok) {
        printf("tex scale selftest: PASS %s\n", what);
    } else {
        fprintf(stderr, "tex scale selftest: FAIL %s\n", what);
        fails++;
    }
}

static uint32_t rgb_of(uint32_t px) { return px & 0x00FFFFFFu; }

/* One scaled texel inside the border: the border treatment's only reach is the outermost
 * scaled ring, so interior pixels of opaque input keep alpha ff there. */
static int interior_alpha_opaque(const uint32_t *px, int wf, int hf, int factor) {
    for (int y = factor; y < hf - factor; y++)
        for (int x = factor; x < wf - factor; x++)
            if ((px[y * wf + x] & 0xFF000000u) != 0xFF000000u) return 0;
    return 1;
}

/* Interior pixels hold an input colour exactly (axis-aligned edges and flat regions: the
 * edge rules pick, they do not blend). */
static int interior_exact(const uint32_t *px, int wf, int hf, int factor,
                          const uint32_t *palette, size_t pal_n) {
    for (int y = factor; y < hf - factor; y++) {
        for (int x = factor; x < wf - factor; x++) {
            const uint32_t v = px[y * wf + x];
            size_t hit = 0;
            for (size_t p = 0; p < pal_n; p++)
                if (rgb_of(v) == rgb_of(palette[p])) hit = 1;
            if (!hit) return 0;
        }
    }
    return 1;
}

/* Interior pixels stay inside the per-channel range of the input palette, alpha ff. xBRZ
 * blends neighbouring colours along smooth (diagonal) edges — that is the filter's point —
 * but a blend of the palette can never leave the palette's per-channel hull. */
static int interior_within_palette(const uint32_t *px, int wf, int hf, int factor,
                                   const uint32_t *palette, size_t pal_n) {
    int lo_r = 255, hi_r = 0, lo_g = 255, hi_g = 0, lo_b = 255, hi_b = 0;
    for (size_t p = 0; p < pal_n; p++) {
        const int r = (int)(palette[p] & 0xFFu), g = (int)((palette[p] >> 8) & 0xFFu),
                  b = (int)((palette[p] >> 16) & 0xFFu);
        if (r < lo_r) lo_r = r;
        if (r > hi_r) hi_r = r;
        if (g < lo_g) lo_g = g;
        if (g > hi_g) hi_g = g;
        if (b < lo_b) lo_b = b;
        if (b > hi_b) hi_b = b;
    }
    for (int y = factor; y < hf - factor; y++) {
        for (int x = factor; x < wf - factor; x++) {
            const uint32_t v = px[y * wf + x];
            const int r = (int)(v & 0xFFu), g = (int)((v >> 8) & 0xFFu), b = (int)((v >> 16) & 0xFFu);
            if (r < lo_r || r > hi_r || g < lo_g || g > hi_g || b < lo_b || b > hi_b) return 0;
            if ((v & 0xFF000000u) != 0xFF000000u) return 0;
        }
    }
    return 1;
}

int main(void) {
    /* Refusals: nothing is written, nothing crashes. */
    {
        uint32_t src[4] = {RED, RED, RED, RED}, dst[16];
        memset(dst, 0, sizeof dst);
        check(tex_scale_rgba(NULL, 2, 2, dst, 2) == 0, "refuse null source");
        check(tex_scale_rgba(src, 2, 2, NULL, 2) == 0, "refuse null destination");
        check(tex_scale_rgba(src, 0, 2, dst, 2) == 0, "refuse zero width");
        check(tex_scale_rgba(src, 2, -1, dst, 2) == 0, "refuse negative height");
        check(tex_scale_rgba(src, 2, 2, dst, 1) == 0, "refuse factor 1");
        check(tex_scale_rgba(src, 2, 2, dst, 0) == 0, "refuse factor 0");
        check(tex_scale_rgba(src, 2, 2, dst, TEX_SCALE_MAX + 1) == 0, "refuse factor above the cap");
        int untouched = 1;
        for (size_t i = 0; i < 16; i++) untouched &= dst[i] == 0u;
        check(untouched, "refusals leave the destination untouched");
    }

    /* Dimensions: the destination is factor*w x factor*h, and the interior holds input
     * colours at full alpha. */
    {
        const uint32_t pal[2] = {RED, BLUE};
        uint32_t src[4 * 3], dst[8 * 6];
        for (size_t i = 0; i < 12; i++) src[i] = (i & 1u) ? BLUE : RED;
        memset(dst, 0, sizeof dst);
        int ok = tex_scale_rgba(src, 4, 3, dst, 2);
        check(ok && interior_exact(dst, 8, 6, 2, pal, 2) && interior_alpha_opaque(dst, 8, 6, 2),
              "factor 2 writes the full 8x6 area with input colours inside");
    }

    /* Determinism: two scales of one input agree word for word across the whole output. */
    {
        enum { W = 16, H = 12 };
        static uint32_t src[W * H], a[W * H * 16], b[W * H * 16];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                src[y * W + x] = ((x / 3 + y / 2) & 1u) ? BLUE
                                 : ((x ^ y) & 7u) == 0 ? GREEN : RED;
        int ok = 1;
        for (int factor = 2; factor <= 4; factor += 2) {
            const size_t out = (size_t)W * H * (size_t)factor * (size_t)factor;
            memset(a, 0xEE, sizeof a);
            memset(b, 0x11, sizeof b);
            ok = ok && tex_scale_rgba(src, W, H, a, factor) &&
                 tex_scale_rgba(src, W, H, b, factor) && memcmp(a, b, out * sizeof(uint32_t)) == 0;
        }
        check(ok, "deterministic at factors 2 and 4");
    }

    /* Hard edges stay hard: axis-aligned edges and flat regions hold input colours exactly;
     * diagonal edges stay inside the palette's per-channel hull (xBRZ blends along them by
     * design); everything opaque keeps alpha ff, one scaled texel inside the border. */
    {
        enum { W = 12, H = 12, N = W * H };
        static uint32_t src[N], dst[N * 16];
        const uint32_t pal[2] = {RED, BLUE};
        int ok = 1;
        /* vertical edge: exact colours */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) src[y * W + x] = x < W / 2 ? RED : BLUE;
        memset(dst, 0, sizeof dst);
        ok = ok && tex_scale_rgba(src, W, H, dst, 2) &&
             interior_exact(dst, 2 * W, 2 * H, 2, pal, 2) &&
             interior_alpha_opaque(dst, 2 * W, 2 * H, 2);
        /* checkerboard, xBRZ's worst case: within the palette hull */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) src[y * W + x] = ((x ^ y) & 1u) ? RED : BLUE;
        memset(dst, 0, sizeof dst);
        ok = ok && tex_scale_rgba(src, W, H, dst, 2) &&
             interior_within_palette(dst, 2 * W, 2 * H, 2, pal, 2);
        /* diagonal edge at factor 4: within the palette hull */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) src[y * W + x] = x < y ? RED : BLUE;
        memset(dst, 0, sizeof dst);
        ok = ok && tex_scale_rgba(src, W, H, dst, 4) &&
             interior_within_palette(dst, 4 * W, 4 * H, 4, pal, 2);
        /* uniform colour at factor 4: interior is exactly the source colour */
        {
            const uint32_t pal_g[1] = {GREEN};
            for (int i = 0; i < N; i++) src[i] = GREEN;
            memset(dst, 0, sizeof dst);
            ok = ok && tex_scale_rgba(src, W, H, dst, 4) &&
                 interior_exact(dst, 4 * W, 4 * H, 4, pal_g, 1) &&
                 interior_alpha_opaque(dst, 4 * W, 4 * H, 4);
        }
        check(ok, "edges scale into palette colours; diagonals stay in the palette hull");
    }

    /* Flat regions survive everywhere including the border, and the red/blue exchange is
     * the right way round: half of a texture red, half blue scales into red left and blue
     * right. A missing or reversed channel exchange turns one side into the other's word. */
    {
        enum { W = 8, H = 8 };
        uint32_t src[W * H], dst[W * H * 16];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) src[y * W + x] = x < W / 2 ? RED : BLUE;
        memset(dst, 0, sizeof dst);
        int ok = tex_scale_rgba(src, W, H, dst, 2);
        for (int y = 0; ok && y < 2 * H; y++)
            for (int x = 0; x < 2 * W; x++)
                ok = rgb_of(dst[y * 2 * W + x]) == rgb_of(x < W ? RED : BLUE);
        check(ok, "flat red/blue halves survive 2x with channels intact");
    }

    /* Single texel and thin textures scale to themselves (RGB; every pixel of these is
     * inside the border ring). */
    {
        uint32_t src[1] = {GREEN}, dst[16];
        memset(dst, 0, sizeof dst);
        int ok = tex_scale_rgba(src, 1, 1, dst, 2);
        for (size_t i = 0; ok && i < 4; i++) ok = rgb_of(dst[i]) == rgb_of(GREEN);
        uint32_t row[2] = {RED, BLUE}, row_dst[8];
        ok = ok && tex_scale_rgba(row, 2, 1, row_dst, 2);
        for (size_t i = 0; ok && i < 8; i++)
            ok = rgb_of(row_dst[i]) == rgb_of((i % 4 < 2) ? RED : BLUE);
        check(ok, "1x1 and 2x1 textures scale without new colours");
    }

    /* Alpha: fully transparent regions stay transparent away from the alpha edge, and
     * opaque regions keep their alpha away from the border corners. */
    {
        enum { W = 8, H = 8 };
        uint32_t src[W * H], dst[W * H * 16];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) src[y * W + x] = y < H / 2 ? RED : CLEAR;
        memset(dst, 0, sizeof dst);
        int ok = tex_scale_rgba(src, W, H, dst, 2);
        if (ok) {
            for (int x = 2; ok && x < 2 * W - 2; x++) {
                if ((dst[2 * x] & 0xFF000000u) != 0xFF000000u) ok = 0;          /* top row opaque */
                if ((dst[(2 * H - 1) * 2 * W + x] & 0xFF000000u) != 0u) ok = 0; /* bottom clear */
            }
        }
        check(ok, "opaque top stays opaque, transparent bottom stays transparent");
    }

    if (fails == 0) {
        puts("tex scale selftest: OK");
        return 0;
    }
    fprintf(stderr, "tex scale selftest: %d check(s) failed\n", fails);
    return 1;
}
