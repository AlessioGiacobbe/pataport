/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef VORBIS_BRIDGE_H
#define VORBIS_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a whole Ogg Vorbis stream to interleaved 16-bit PCM. On success
   returns 0 and stores a malloc'd buffer in *pcm (release it with
   sr_vorbis_free), the per-channel frame count in *frames, and the stream's
   own channel count and rate in *channels and *rate. On any failure returns
   -1 and leaves the outs untouched: the caller stays silent rather than
   playing a truncated clip. */
int sr_vorbis_decode(const uint8_t *data, size_t size, int16_t **pcm,
                     size_t *frames, int *channels, int *rate);

/* Release a buffer sr_vorbis_decode produced. NULL is allowed. */
void sr_vorbis_free(int16_t *pcm);

#ifdef __cplusplus
}
#endif

#endif /* VORBIS_BRIDGE_H */
