# assets/easter_egg — provenance

`echo_cai.ogg` is Pataport's own easter-egg clip: authored for this project by
its maintainer in 2026. It is not extracted from any game, console, or
third-party work, and it carries no private or console-derived content.
Licence: GPL-3.0-or-later, like the rest of the project.

- SHA-256: `0cc129529f2fc565a32d9816313ed4e9b12c0864a583a24430c17598d4e115d9`
- Format: Ogg Vorbis, 2 channels, 44100 Hz (decoded by the vendored
  `third_party/stb/stb_vorbis.c`; see that file's PROVENANCE row).

It is embedded verbatim by `tools/gen_ui_assets.py` into `src/ui/egg_assets.c`
and played once per screen entry by `src/rt/easter_egg.c`, when the boot
contract-license screen shows and when the player presses L+R on the game's
OPTION screen.
