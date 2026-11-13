/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The Pataport easter egg's trigger. The boot's pre-title phase -- the game's
 * logo and its warning screens -- is drawn by the main module before any code
 * overlay binds: a boot run with SR_IOLOG=1 opens only DATA_CMN.BND and the
 * DATAMS archives, and no message dialog marks the screens. What the runtime
 * observes on its own is the phase's end: the title overlay binding
 * (CODE_OVERLAY_BOUND, sr_module_code_sync_overlays). The generated code
 * registers that overlay's name here and the sound unit beside it; when the
 * overlay binds, the clip plays once. No game address crosses this unit.
 *
 * The trigger fires once per run and never on any later bind of the same
 * overlay (the game swaps its overlays freely while it runs). SR_EASTER_EGG=0
 * disables the egg whole: the facts stay registered, nothing plays, nothing
 * logs. */

#include "easter_egg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EASTER_EGG_NAME_MAX 64u

static char s_overlay_name[EASTER_EGG_NAME_MAX];
static void (*s_play)(void);
static int s_fired;                 /* the clip played once; later binds ignore */
static int s_logged_no_player;      /* the once-only "registered, but silent" note */
static int s_disabled = -1;         /* SR_EASTER_EGG, read once; -1 = not yet read */

static int easter_egg_disabled(void) {
    if (s_disabled < 0) {
        const char *env = getenv("SR_EASTER_EGG");
        s_disabled = env != NULL && env[0] == '0';
    }
    return s_disabled;
}

void sr_easter_egg_register_title_overlay(const char *name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= sizeof s_overlay_name) {
        fprintf(stderr, "easter-egg: overlay name refused; the egg stays silent\n");
        return;
    }
    snprintf(s_overlay_name, sizeof s_overlay_name, "%s", name);
    fprintf(stderr, "easter-egg: watching for %s to bind\n", name);
}

void sr_easter_egg_register_player(void (*play)(void)) {
    s_play = play;
}

void sr_easter_egg_overlay_bound(const char *name) {
    if (easter_egg_disabled() || s_fired || name == NULL || s_overlay_name[0] == '\0') {
        return;
    }
    if (strcmp(name, s_overlay_name) != 0) {
        return;
    }
    s_fired = 1;
    fprintf(stderr, "easter-egg: %s bound; the boot's pre-title phase is over\n", name);
    if (s_play != NULL) {
        s_play();
    } else {
        /* The facts were registered without a player (a build the sound unit
         * did not reach): say so once instead of failing silently. */
        if (!s_logged_no_player) {
            s_logged_no_player = 1;
            fprintf(stderr, "easter-egg: no player registered; staying silent\n");
        }
    }
}

void sr_easter_egg_reset_for_test(void) {
    memset(s_overlay_name, 0, sizeof s_overlay_name);
    s_play = NULL;
    s_fired = 0;
    s_logged_no_player = 0;
    s_disabled = -1;
}
