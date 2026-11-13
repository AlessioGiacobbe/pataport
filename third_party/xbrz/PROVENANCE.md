# PROVENANCE.md — `third_party/xbrz/`

Zenju's xBRZ image upscaler ("xBRZ: Scale by rules"), vendored for the GPU GE backend's
texture enhancement: `src/rt/tex_scale.cpp` wraps it behind a C entry point and
`src/rt/gpu_sdl3vk/ge_gpu.c` applies it to decoded textures when the render scale is above
1 (the Quality option's 4K path). Nothing game-derived passes through this directory; the
upstream bytes are algorithm code only.

## Upstream source

- Project: xBRZ ("Scale by rules"), version 1.9 (Changelog entry `xBRZ 1.9 [2026-01-24]`)
- Author: Zenju (zenju AT gmx DOT de)
- Upstream distribution: `https://sourceforge.net/projects/xbrz/`, file
  `xBRZ/xBRZ_1.9.zip` (`https://sourceforge.net/projects/xbrz/files/xBRZ/xBRZ_1.9.zip/download`).
  The project publishes releases as zip archives; there is no upstream VCS revision to pin,
  so the pin is the release archive's contents and the SHA-256 values below.
- License: GNU GPL version 3 (`License.txt` in the archive is the verbatim GPL-3.0 text; the
  file headers point at `https://www.gnu.org/licenses/gpl-3.0`). The archive's file headers
  additionally carry the author's linking exception for MAME, FreeFileSync, Snes9x and
  ePSXe, which this project does not rely on. GPL-3.0 is the project's own licence
  (GPL-3.0-or-later), so the import is licence-compatible. `License.txt` is kept unchanged.
- Import method: the files were extracted from the release zip and copied byte-identical,
  with one checked normalization: the archive's `Changelog.txt` and `License.txt` carry CRLF
  endings, and the tracked copies hold the archive's exact bytes with LF endings (the
  repository's vendored-text convention, as `third_party/libchdr/`), so the digests below are
  the tracked bytes'. The code files' bytes are the archive's own, endings included. No local
  delta exists.

## File manifest

| File | Kind | SHA-256 |
| --- | --- | --- |
| `xbrz.cpp` | imported byte-identical | `882346f5f6703f286c55d4089af7a8f63140fe13a133318e4be64891689bee0c` |
| `xbrz.h` | imported byte-identical | `004e4ee01324d7c9c6870a02aab58e5344eb209e8940cb7f3302b9f9547a1197` |
| `xbrz_config.h` | imported byte-identical | `e7301f66ddbe0785b54b9621148d8d4143f33207be91c430a99847f6e9eb5fda` |
| `xbrz_tools.h` | imported byte-identical | `72202a6a259d0f1b6811c7402cea5b0eeec7479c2e8c5d017ec4bcff8eb1296b` |
| `License.txt` | imported byte-identical (GPL-3.0 text, CRLF normalized to LF) | `c61f12da7cdad526bdcbed47a4c0a603e60dbbfdaf8b66933cd088e9132c303f` |
| `Changelog.txt` | imported byte-identical (CRLF normalized to LF) | `8adeb41c68b328d8ff086596cdab637ffd93ea8a79ddc541baeb0f73ee737083` |
| `PROVENANCE.md` | project-authored manifest | n/a |

## Build configuration

`src/rt/tex_scale.cpp` includes `xbrz.h` and calls `xbrz::scale` with
`xbrz::ColorFormat::argbUnbuffered` (the buffered format's one-time 100-300 ms lookup-table
build would stall the first texture upload). The runtime's decoded pixels are 0xAABBGGRR
while xBRZ's `argb` expects 0xAARRGGBB, and the colour distance is YCbCr-weighted, so the
wrapper byte-swaps each pixel before and after scaling instead of modifying upstream.
`xbrz.cpp` compiles as one C++17 translation unit (`-O2`); nothing else in the runtime is
C++. The scale factor is limited to powers of two up to 4 by the caller (`src/rt/tex_scale.h`),
inside xBRZ's own 2..6 range.
