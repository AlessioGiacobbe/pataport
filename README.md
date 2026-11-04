<!-- markdownlint-disable-file MD041 MD025 -->
# Pataport

**Play your own copy of Patapon 3 as a native program on macOS (Apple Silicon) and Windows.**

[![Pataport CI](https://github.com/AlessioGiacobbe/pataport/actions/workflows/pataport.yml/badge.svg)](https://github.com/AlessioGiacobbe/pataport/actions/workflows/pataport.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)

Pataport is a fork of [Nakagawa Recomp](https://github.com/Jstar269/nakagawa-recomp), a static recompiler for PSP
games, tuned for Patapon 3. The game's MIPS code is translated to C and compiled into a native executable on your
own machine, from your own disc. There is no emulator in the loop.

> **No game data here.** This repository contains only the recompiler and runtime source. You need your own
> Patapon 3 disc image (tested: European release, UCES01421). The build stays on your computer: never share
> the data folder or the executable it produces, because they contain the game's code.
> Pataport is a fan project, not affiliated with or endorsed by Sony Interactive Entertainment.
> Patapon is a trademark of Sony Interactive Entertainment.

## Status

| | |
|---|---|
| macOS 14+ (Apple Silicon) | Plays: menus, saves, movies with sound, story, hideout and missions at 60 fps |
| Windows 10/11 x64 | Builds and passes the test suite in CI; play-testing on a real PC is pending |
| Gameplay missions | Battles run at 60 fps (M4); full campaign not played through yet |
| Online (ad-hoc multiplayer) | Experimental, see [Online](#online) |

## Install

**macOS**

```sh
xcode-select --install
brew install make pkgconf sdl3 sdl3_ttf vulkan-headers vulkan-loader molten-vk python
git clone https://github.com/AlessioGiacobbe/pataport && cd pataport
python3 pataport.py build "/path/to/Patapon 3.iso"     # once, about 5 minutes
python3 pataport.py play
```

**Windows 10/11 (x64)**

1. Install [Git for Windows](https://git-scm.com/download/win),
   [Python 3.15](https://www.python.org/downloads/windows/) (tick *Add python.exe to PATH*),
   [MSYS2](https://www.msys2.org/) and an up-to-date graphics driver (Pataport draws with Vulkan).
2. Open the **MSYS2 UCRT64** terminal from the Start menu and install the compiler and libraries:

   ```sh
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-sdl3-ttf mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader
   ```

3. Put MSYS2's tools on your `PATH` once, from **PowerShell**, then close PowerShell and open a new one:

   ```powershell
   [Environment]::SetEnvironmentVariable("Path", "C:\msys64\ucrt64\bin;" + [Environment]::GetEnvironmentVariable("Path", "User"), "User")
   ```

4. Build once (about 5 minutes) and play, from PowerShell:

   ```powershell
   git clone https://github.com/AlessioGiacobbe/pataport
   cd pataport
   python pataport.py build "C:\Games\Patapon 3.iso"
   python pataport.py play
   ```

   Next time, `cd pataport` and `python pataport.py play` is all it takes.

Builds and saves go to `~/Library/Application Support/Pataport` (macOS) or `%LOCALAPPDATA%\Pataport`
(Windows); `--data <folder>` picks another place. Saves live in its `memstick` folder and survive rebuilds.

## 60 fps

Builds unlock 60 fps by default. Patapon 3 renders at 30 fps on a PSP; Pataport applies the
community "60 FPS Patch v2" by Madwig ([PPSSPP forum](https://forums.ppsspp.org/showthread.php?pid=132462))
to the game's code before translating it. The game keeps its speed: in a measured battle the
mission timer and the drum combo advance exactly as at 30 fps. Its author notes unit movement is
"a bit off". For the original frame rate, build with `python pataport.py build --30fps <iso>`.

## Controls

| PSP | Keyboard | Patapon |
|---|---|---|
| □ | A | PATA |
| ○ | Z | PON |
| △ | S | CHAKA |
| ✕ | X | DON |
| D-pad | Arrow keys | |
| Analog stick | I / J / K / L | Sign the Hero's Pledge |
| L / R | Q / W | |
| START / SELECT | Return / Shift | |
| Pause menu | Esc | |
| Mute / unmute | M | |
| FPS / performance counter | F3 (fn+F3 on a Mac); `SR_HUD=1` starts with it on | |

A game controller works too (SDL3).

## Changes from upstream Nakagawa Recomp

Modified by the Pataport contributors, November 2026 (GPL-3.0 §5a):

- **macOS arm64 port**: Darwin build flags and BSD APIs, Mach-O coroutine switch, MoltenVK portability,
  `gmake`/`python3`/`-dead_strip`/`-map` in the build, `_NSGetExecutablePath`, `/var` symlink handling.
- **Code overlays**: constructors that a linker places after an overlay's data (all of Patapon 3's overlays)
  are now translated; before, the game stopped right after creating its first save.
- **Pad scripts**: `STICK_LEFT/RIGHT/UP/DOWN` drive the analog stick in automated runs.
- **Performance**: lines and points are drawn on the GPU (`SR_GPU_THIN_PRIMS=0` restores the pixel-exact
  software path), and `pataport.py` selects the GPU renderer: battles went from 12-15 fps to 30, then 60.
- **60 fps**: `tools/pataport_patches.py` changes the frame limiter before translation (see [60 fps](#60-fps)).
- **Pataport**: `pataport.py` launcher, window title, CI for Windows and macOS (upstream workflows are kept in
  `.github/upstream-workflows/`).

## Online

> **Experimental.** The ad hoc libraries Patapon 3 uses are implemented, but a full multiplayer
> session has not been played through yet. Reports are welcome.

```sh
python3 pataport.py server                      # one player (or a VPS) hosts the lobby: TCP 27312
python3 pataport.py play --online LOBBY_ADDRESS # everyone, in the game's ad hoc multiplayer menu
python3 pataport.py play --online 127.0.0.1 --player2   # a second window on the same computer
```

On a PSP, Patapon 3's multiplayer is *ad-hoc*: nearby consoles talk directly over Wi-Fi. The game joins a room
(`sceNetAdhocctl`), finds the other players and lets the host accept them (`sceNetAdhocMatching`), then exchanges
game data over direct connections (`PDP`, like UDP, and `PTP`, like TCP). Pataport runs those same calls over
the internet instead of Wi-Fi:

- **Lobby.** Each player connects to a server that speaks PPSSPP's ad-hoc server protocol. It only introduces
  the players: who is in which room, and at which address. Use a public server or run your own.
- **Gameplay.** Game data then goes straight between the players' computers. Their ports must be reachable: the
  easy way is a virtual LAN such as [Tailscale](https://tailscale.com/) or ZeroTier; otherwise forward the ports
  on the router.
- **Cross-play.** The lobby speaks PPSSPP's protocol, but the matching packets are Pataport's own, so playing
  with PPSSPP users is not expected to work yet.
- **Off by default.** Nothing connects to the network unless you pass `--online`.

The original PSN online mode cannot come back: Sony's servers are shut down.
