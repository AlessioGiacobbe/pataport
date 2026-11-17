/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef EGG_ASSETS_H
#define EGG_ASSETS_H

#include "nk_ui_assets.h"

/* Files embedded in the runtime binary by tools/gen_ui_assets.py
   (egg_assets.c): Pataport's own easter-egg clips ("easter-egg/<file>.ogg"),
   committed under assets/easter_egg/. Separate from the launcher's nk_ui_assets
   table so the game executable carries only what it plays. */
extern const NkUiEmbeddedAsset egg_embedded_assets[];
extern const size_t egg_embedded_asset_count;

/* The asset with exactly this name, or NULL. */
const NkUiEmbeddedAsset *egg_embedded_asset(const char *name);

#endif /* EGG_ASSETS_H */
