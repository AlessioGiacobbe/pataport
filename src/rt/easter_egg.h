/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef EASTER_EGG_H
#define EASTER_EGG_H

#ifdef __cplusplus
extern "C" {
#endif

/* The Pataport easter egg (docs/GAME_CHANGES.md, the bridge channel): one
   sound, played once, when the game's boot reaches its title overlay -- the
   runtime's own observable for the end of the boot's pre-title phase, whose
   warning screens the main module draws before any code overlay binds. The
   generated code registers the overlay's name and the sound unit at start-up;
   this unit carries no game addresses. SR_EASTER_EGG=0 silences it whole. */

/* From the generated sr_register_all(): the overlay whose binding ends the
   boot's pre-title phase. A NULL, empty, or oversized name refuses the whole
   registration and the egg stays silent. */
void sr_easter_egg_register_title_overlay(const char *name);

/* The sound unit (easter_egg_sound.c) registers how to play the clip; the
   SDL-free tests register their own recorder here. Unset, the egg logs once
   and stays silent. */
void sr_easter_egg_register_player(void (*play)(void));

/* The overlay-binding path (src/rt/guest_interp.c) reports every overlay as
   it binds. Any name the egg did not register is ignored; the registered
   name fires the clip once per run. */
void sr_easter_egg_overlay_bound(const char *name);

void sr_easter_egg_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* EASTER_EGG_H */
