/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_scale.h - edge-preserving texture upscaling for the GPU GE backend.
 *
 * The runtime decodes guest textures to RGBA8 (0xAABBGGRR, alpha in the top byte). When the
 * render scale is above 1 (the Quality option's 2K/4K paths) those texels are stretched
 * across the whole device canvas; tex_scale applies an edge-preserving upscaler (xBRZ,
 * third_party/xbrz, GPL-3.0) so hard edges stay sharp at the higher resolution. The unit is
 * presentation-only: it never reads guest memory, never touches the GE, and its output is
 * deterministic for a given input.
 *
 * Factors are powers of two (2 or 4) so the GPU's mip level of detail for the upscaled
 * chain stays an exact whole-level shift of the guest's own level of detail (the lodcfg
 * bits in ge_gpu.c). tex_scale_rgba is pure C linkage so both the runtime and the C
 * selftests call it without a C++ runtime.
 */
#ifndef SR_TEX_SCALE_H
#define SR_TEX_SCALE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest factor this unit applies. xBRZ itself supports 2..6; the runtime pins the
 * power-of-two subset up to 4 (see the header comment). */
#define TEX_SCALE_MAX 4

/* Scales the w x h source into a caller-provided (factor*w) x (factor*h) destination.
 * Returns 1 on success, 0 on refusal: a NULL pointer, a non-positive size, or a factor
 * outside 2..TEX_SCALE_MAX. The source and destination must not overlap. Deterministic:
 * the same input always produces the same output. */
int tex_scale_rgba(const uint32_t *src, int w, int h, uint32_t *dst, int factor);

#ifdef __cplusplus
}
#endif

#endif /* SR_TEX_SCALE_H */
