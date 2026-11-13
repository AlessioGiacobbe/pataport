/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The easter egg's sound unit. The clip is Pataport's own audio, embedded by
 * tools/gen_ui_assets.py from assets/easter_egg (see that folder's
 * PROVENANCE.md); third_party/stb/stb_vorbis.c decodes it through
 * src/rt/vorbis_bridge.c, the one translation unit that compiles the vendored
 * decoder. The decoded PCM sits beside the game's audio as its own stream on
 * the default playback device -- SDL3 mixes the streams, the way the
 * launcher's UI sounds (src/player/ui/player_ui_sound.c) sit beside whatever
 * else is playing -- and the stream opens lazily, on the first play, so a run
 * that never fires never touches a device.
 *
 * The same environment guards as the game's own audio (sr_audio_init) keep it
 * silent where the runtime is silent, and the M-key mute silences it too: the
 * egg is part of the game's sound, not an announcement over it. A device
 * that cannot open, or a clip that does not decode, logs once and the egg
 * stays silent for the rest of the run. */

#include "easter_egg_sound.h"

#include "easter_egg.h"
#include "egg_assets.h"
#include "vorbis_bridge.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The egg's own level: present, not louder than the game. */
#define EASTER_EGG_GAIN 0.6f

#define EASTER_EGG_CLIP "easter-egg/echo_cai.ogg"

static const int16_t *s_pcm;        /* the decoded clip (interleaved) */
static size_t s_frames;             /* per-channel frames */
static int s_channels;
static int s_rate;
static int s_failed;                /* decode or device failed once: stay silent */
static int s_audio_started;         /* this unit started SDL's audio subsystem */
static SDL_AudioStream *s_stream;

/* The game's own mute (audio_sdl3.c; audio_unavailable.c answers 0). */
int sr_audio_muted(void);

static int egg_sound_env_silent(void) {
    const char *env_backend = getenv("SR_AUDIO_BACKEND");
    const char *env_disabled = getenv("SR_AUDIO_DISABLED");
    const char *env_headless = getenv("SR_HEADLESS");
    if (env_backend && (strcmp(env_backend, "null") == 0 || strcmp(env_backend, "none") == 0 ||
                        strcmp(env_backend, "0") == 0 || strcmp(env_backend, "off") == 0)) {
        return 1;
    }
    if (env_disabled && strcmp(env_disabled, "1") == 0) {
        return 1;
    }
    if (env_headless && strcmp(env_headless, "1") == 0 && !env_backend) {
        return 1;
    }
    return 0;
}

static void egg_sound_decode_once(void) {
    const NkUiEmbeddedAsset *asset = egg_embedded_asset(EASTER_EGG_CLIP);
    int16_t *pcm = NULL;
    size_t frames = 0;
    int channels = 0, rate = 0;
    if (asset == NULL || sr_vorbis_decode(asset->bytes, asset->size, &pcm, &frames,
                                          &channels, &rate) != 0) {
        s_failed = 1;
        fprintf(stderr, "easter-egg: %s did not decode; the egg stays silent\n",
                EASTER_EGG_CLIP);
        return;
    }
    s_pcm = pcm;
    s_frames = frames;
    s_channels = channels;
    s_rate = rate;
    fprintf(stderr, "easter-egg: clip decoded (%zu frames, %d ch, %d Hz)\n",
            s_frames, s_channels, s_rate);
}

static int egg_sound_stream_open(void) {
    if (s_stream != NULL) {
        return 1;
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            s_failed = 1;
            fprintf(stderr, "easter-egg: SDL audio could not start; staying silent\n");
            return 0;
        }
        s_audio_started = 1;
    }
    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof spec);
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = (Uint8)s_channels;
    spec.freq = s_rate;
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (s_stream == NULL) {
        s_failed = 1;
        fprintf(stderr, "easter-egg: the playback device could not open; staying silent\n");
        return 0;
    }
    (void)SDL_SetAudioStreamGain(s_stream, EASTER_EGG_GAIN);
    (void)SDL_ResumeAudioStreamDevice(s_stream);
    return 1;
}

static void egg_sound_play(void) {
    if (s_failed || s_pcm == NULL || egg_sound_env_silent() || sr_audio_muted()) {
        return;
    }
    if (!egg_sound_stream_open()) {
        return;
    }
    size_t bytes = s_frames * (size_t)s_channels * sizeof(int16_t);
    if (bytes == 0 || bytes > (size_t)SDL_MAX_SINT32) {
        return;
    }
    (void)SDL_ClearAudioStream(s_stream);
    if (!SDL_PutAudioStreamData(s_stream, s_pcm, (int)bytes)) {
        fprintf(stderr, "easter-egg: the clip could not be queued; staying silent\n");
        s_failed = 1;
    }
}

void sr_easter_egg_sound_register(void) {
    egg_sound_decode_once();
    if (!s_failed) {
        sr_easter_egg_register_player(egg_sound_play);
    }
}

/* The SDL-free tests register their own player; this unit is not linked there
   and holds no state worth resetting beyond what easter_egg.c owns. */
