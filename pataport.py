#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pataport: build and play your own copy of Patapon 3 as a native program.

    python pataport.py build <your-patapon3.iso>   # once: recompiles the game from your disc
    python pataport.py play                         # plays it

Everything built from the disc (decrypted code, generated C, the executable) and your saves
stay in the data folder on this computer (--data, default: the per-user app data folder).
Never share that folder: it contains the game's code. Built on Nakagawa Recomp (see README).
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent


def default_data_dir():
    if sys.platform == "win32":
        return Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Pataport"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "Pataport"
    return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share")) / "pataport"


def find_package(data):
    """The newest built package: (package dir, executable, base, entry)."""
    found = sorted((data / "user-data" / "packages").glob("*/package.json"),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    if not found:
        sys.exit(f"No built game in {data}. Run: python pataport.py build <your-patapon3.iso>")
    pkg = found[0].parent
    meta = json.loads(found[0].read_text(encoding="utf-8"))
    exe = pkg / meta["executable"]["path"]
    if not exe.exists() and exe.with_suffix(".exe").exists():
        exe = exe.with_suffix(".exe")
    base = meta["cache"]["codegen_options"]["base"]
    entry = meta["runtime"]["run_entry"]
    return pkg, exe, base, entry


def build(args, data):
    iso = Path(args.iso).resolve()
    if not iso.is_file():
        sys.exit(f"ISO not found: {iso}")
    data.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("PYTHON", sys.executable)   # the build's tools run on this same interpreter
    if sys.platform == "darwin":
        env.setdefault("CC", "clang")
    # The bring-up ends by launching the game for a few seconds; a time-out there is fine.
    subprocess.call([sys.executable, str(REPO / "tools" / "nk_cli.py"), "bringup", str(iso),
                     "--work-dir", str(data / "work"), "--report", str(data / "build-report.json"),
                     "--user-data-root", str(data / "user-data"), "--launch-timeout", "10"],
                    cwd=REPO, env=env)
    _pkg, exe, _base, _entry = find_package(data)
    if not exe.exists():
        sys.exit(f"Build failed: see {data / 'build-report.json'}")
    (data / "pataport.json").write_text(json.dumps({"iso": str(iso)}), encoding="utf-8")
    print(f"Built {exe.name}. Play with: python pataport.py play")


def play(args, data):
    pkg, exe, base, entry = find_package(data)
    iso = args.iso or json.loads((data / "pataport.json").read_text(encoding="utf-8"))["iso"]
    if not Path(iso).is_file():
        sys.exit(f"ISO not found: {iso} (pass it: python pataport.py play <iso>)")
    env = {k: v for k, v in os.environ.items() if k not in (
        "SR_DATAROOT", "SR_VIDEO", "SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER",
        "SDL_AUDIODRIVER", "SDL_AUDIO_DRIVER")}
    memstick = Path(env.get("SR_MEMSTICK") or data / "memstick")   # saves survive rebuilds
    memstick.mkdir(parents=True, exist_ok=True)
    env.update(SR_USER_DATA_ROOT=str(data / "user-data"), SR_MODULE_DIR=str(pkg / "modules"),
               PSP_ISO=str(Path(iso).resolve()), SR_MEMSTICK=str(memstick),
               SR_BOOT_PATH="disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", SR_LOOSE_CONTENT_ROOTS="")
    image = exe.with_suffix("").name + "_image.bin"
    cmd = [str(exe), "--image", image, base, entry, "none", "none", "--sched", "--gui"]
    sys.exit(subprocess.call(cmd + args.extra, cwd=pkg, env=env))


def selftest():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        pkg = Path(tmp) / "user-data" / "packages" / "UCES01421"
        pkg.mkdir(parents=True)
        (pkg / "package.json").write_text(json.dumps({
            "executable": {"path": "experimental-uces01421"},
            "cache": {"codegen_options": {"base": "0x08804000"}},
            "runtime": {"run_entry": "0x08869238"}}), encoding="utf-8")
        _pkg, exe, base, entry = find_package(Path(tmp))
        assert (exe.name, base, entry) == ("experimental-uces01421", "0x08804000", "0x08869238")
    print("pataport selftest: OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, default=default_data_dir(), help="data folder (default: %(default)s)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="recompile the game from your own ISO")
    b.add_argument("iso")
    p = sub.add_parser("play", help="play the built game")
    p.add_argument("iso", nargs="?", help="ISO path (default: the one used to build)")
    p.add_argument("extra", nargs=argparse.REMAINDER, help="extra runtime arguments")
    sub.add_parser("selftest", help="check the launcher logic (no game needed)")
    args = ap.parse_args()
    if args.cmd == "selftest":
        return selftest()
    (build if args.cmd == "build" else play)(args, args.data.expanduser())


if __name__ == "__main__":
    main()
