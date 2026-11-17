/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef EASTER_EGG_SOUND_H
#define EASTER_EGG_SOUND_H

#ifdef __cplusplus
extern "C" {
#endif

/* The easter egg's sound unit: decodes the clip embedded by
   tools/gen_ui_assets.py (assets/easter_egg/, Pataport's own audio) and plays
   it once as a one-shot on its own stream beside the game's audio, the way
   the launcher's UI sounds do. Decoding happens at registration and fails
   closed: a clip that does not decode logs once and the egg stays silent.
   No audio device is opened until the clip actually plays, and the runtime's
   env guards (SR_AUDIO_BACKEND, SR_AUDIO_DISABLED, SR_HEADLESS) and the M-key
   mute keep it silent like the rest of the audio path. */

/* From the generated sr_register_all(): decode the embedded clip and register
   the player. Refusing anything leaves the egg silent. */
void sr_easter_egg_sound_register(void);

#ifdef __cplusplus
}
#endif

#endif /* EASTER_EGG_SOUND_H */
