/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* vorbis_bridge.c -- the only translation unit that compiles the vendored Ogg
 * Vorbis decoder (third_party/stb/stb_vorbis.c, public domain at upstream's
 * choice, byte-identical; see its PROVENANCE row). Upstream defaults are kept:
 * int16 output through stb_vorbis_decode_memory, no resampling, no seeking --
 * the easter-egg clip decodes whole, once, and its own rate and channel count
 * travel with it. */

#include "../../third_party/stb/stb_vorbis.c"

#include "vorbis_bridge.h"

#include <stdlib.h>

int sr_vorbis_decode(const uint8_t *data, size_t size, int16_t **pcm,
                     size_t *frames, int *channels, int *rate) {
    if (pcm == NULL || frames == NULL || channels == NULL || rate == NULL ||
        data == NULL || size == 0 || size > 0x7fffffffu) {
        return -1;
    }
    int got_channels = 0, got_rate = 0;
    short *output = NULL;
    int decoded = stb_vorbis_decode_memory(data, (int)size, &got_channels,
                                           &got_rate, &output);
    if (decoded <= 0 || output == NULL || got_channels <= 0 || got_rate <= 0) {
        free(output);   /* stb_vorbis_decode_memory frees its own on failure */
        return -1;
    }
    *pcm = output;
    *frames = (size_t)decoded;
    *channels = got_channels;
    *rate = got_rate;
    return 0;
}

void sr_vorbis_free(int16_t *pcm) {
    free(pcm);
}
