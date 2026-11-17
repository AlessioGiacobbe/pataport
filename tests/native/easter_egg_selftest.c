/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The easter egg's trigger and its clip, without SDL and without a game: the
 * overlay names the runtime's own bind reporting hands over decide when the
 * clip plays -- once, and only for the registered name -- SR_EASTER_EGG=0
 * silences the whole egg, a refused registration stays silent, and the
 * committed clip decodes to exactly the PCM the sound unit plays. This is the
 * same decode the runtime ships, read from the embedded asset table. */

#include "easter_egg.h"
#include "egg_assets.h"
#include "vorbis_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_plays;
static void record_play(void) {
    s_plays++;
}

static int s_failures;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

static void set_env_disabled(void) {
#ifdef _WIN32
    _putenv("SR_EASTER_EGG=0");
#else
    setenv("SR_EASTER_EGG", "0", 1);
#endif
}

int main(void) {
    /* An unrelated overlay never fires; the registered one fires exactly
       once; the game's later binds of the same overlay stay silent. */
    sr_easter_egg_register_title_overlay("OL_Title.bin");
    sr_easter_egg_register_player(record_play);
    sr_easter_egg_overlay_bound("OL_Azito.bin");
    CHECK(s_plays == 0);
    sr_easter_egg_overlay_bound("OL_Title.bin");
    CHECK(s_plays == 1);
    sr_easter_egg_overlay_bound("OL_Title.bin");
    sr_easter_egg_overlay_bound("OL_Title.bin");
    CHECK(s_plays == 1);

    /* A refused registration (NULL, empty, oversized) leaves nothing armed. */
    sr_easter_egg_reset_for_test();
    sr_easter_egg_register_player(record_play);
    sr_easter_egg_register_title_overlay(NULL);
    sr_easter_egg_overlay_bound("OL_Title.bin");
    CHECK(s_plays == 1);
    sr_easter_egg_register_title_overlay("");
    sr_easter_egg_overlay_bound("OL_Title.bin");
    CHECK(s_plays == 1);
    char oversized[128];
    memset(oversized, 'x', sizeof oversized - 1u);
    oversized[sizeof oversized - 1u] = '\0';
    sr_easter_egg_register_title_overlay(oversized);
    sr_easter_egg_overlay_bound(oversized);
    CHECK(s_plays == 1);

    /* Facts without a player: the egg reports once and stays silent. */
    sr_easter_egg_reset_for_test();
    sr_easter_egg_register_title_overlay("OL_Title.bin");
    sr_easter_egg_overlay_bound("OL_Title.bin");
    CHECK(s_plays == 1);

    /* SR_EASTER_EGG=0 silences the whole egg. */
    sr_easter_egg_reset_for_test();
    set_env_disabled();
    sr_easter_egg_register_title_overlay("OL_Title.bin");
    sr_easter_egg_register_player(record_play);
    sr_easter_egg_overlay_bound("OL_Title.bin");
    CHECK(s_plays == 1);

    /* The committed clip decodes to what the sound unit plays: stereo 44.1 kHz
       Vorbis, and every sample not silence. */
    const NkUiEmbeddedAsset *asset = egg_embedded_asset("easter-egg/echo_cai.ogg");
    CHECK(asset != NULL);
    CHECK(asset->size == 133688u);
    int16_t *pcm = NULL;
    size_t frames = 0;
    int channels = 0, rate = 0;
    CHECK(sr_vorbis_decode(asset->bytes, asset->size, &pcm, &frames, &channels,
                           &rate) == 0);
    CHECK(pcm != NULL);
    CHECK(channels == 2);
    CHECK(rate == 44100);
    CHECK(frames == 131383u);
    int loud = 0;
    for (size_t i = 0; pcm != NULL && i < frames * (size_t)channels; i += 97) {
        if (pcm[i] != 0) {
            loud = 1;
            break;
        }
    }
    CHECK(loud);
    sr_vorbis_free(pcm);

    if (s_failures != 0) {
        fprintf(stderr, "easter-egg selftest: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("easter-egg selftest: OK\n");
    return 0;
}
