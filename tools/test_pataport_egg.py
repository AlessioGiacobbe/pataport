#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The easter egg's wiring: the fact, the generated registration, and the seams.

The egg itself is a bridge-channel feature (docs/GAME_CHANGES.md): the fact it
watches lives in ``tools/pataport_patches.py`` and travels through the
generated ``sr_register_all()``; the runtime side stays SDL-free and carries
no game facts. These checks hold the wiring together: the fact exists, the
generated code registers it beside the pcmenu facts it rides, the runtime's
overlay-bind path reports to the egg, the declarations the generated code
sees exist, and both build inventories carry the new units.
"""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pataport_patches  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def read(*parts: str) -> str:
    return (ROOT.joinpath(*parts)).read_text(encoding="utf-8")


class PataportEggWiringTests(unittest.TestCase):
    def test_the_fact_is_the_title_overlay(self) -> None:
        self.assertEqual(pataport_patches.EGG_TITLE_OVERLAY, "OL_Title.bin")
        # The egg rides the pcmenu set: the same applied gate must cover both.
        self.assertIn("patapon3-pcmenu", pataport_patches.PATCHES)

    def test_generated_code_registers_the_sound_unit_and_the_name(self) -> None:
        codegen = read("tools", "codegen.py")
        self.assertIn("sr_easter_egg_sound_register();", codegen)
        self.assertIn("sr_easter_egg_register_title_overlay(", codegen)
        self.assertIn("pataport_patches.EGG_TITLE_OVERLAY", codegen)
        # The registrations ride the pcmenu gate, not a gate of their own.
        gate = codegen.index('applied_pataport_patches.get("patapon3-pcmenu")')
        self.assertLess(gate, codegen.index("sr_easter_egg_sound_register();"))
        self.assertLess(gate, codegen.index("sr_easter_egg_register_title_overlay("))

    def test_the_runtime_reports_overlay_binds_to_the_egg(self) -> None:
        guest_interp = read("src", "rt", "guest_interp.c")
        self.assertIn("sr_easter_egg_overlay_bound(code->name);", guest_interp)
        self.assertIn('#include "easter_egg.h"', guest_interp)
        egg = read("src", "rt", "easter_egg.c")
        self.assertIn("SR_EASTER_EGG", egg)
        self.assertIn("sr_easter_egg_reset_for_test", egg)

    def test_the_declarations_the_generated_code_sees_exist(self) -> None:
        recomp = read("src", "rt", "recomp.h")
        self.assertIn("void sr_easter_egg_register_title_overlay(const char *name);", recomp)
        self.assertIn("void sr_easter_egg_sound_register(void);", recomp)

    def test_the_sound_unit_reads_the_embedded_clip_and_honors_mute(self) -> None:
        sound = read("src", "rt", "easter_egg_sound.c")
        self.assertIn("egg_embedded_asset(", sound)
        self.assertIn("sr_vorbis_decode(", sound)
        self.assertIn("sr_audio_muted()", sound)

    def test_both_build_inventories_carry_the_new_units(self) -> None:
        makefile = read("Makefile")
        for unit in ("src/rt/easter_egg.c", "src/rt/easter_egg_sound.c",
                     "src/rt/vorbis_bridge.c"):
            self.assertIn(unit, makefile)
        cmake = read("CMakeLists.txt")
        for unit in ("src/rt/easter_egg.c", "src/rt/easter_egg_sound.c",
                     "src/rt/vorbis_bridge.c"):
            self.assertIn(unit, cmake)
        # The sound unit compiles with the UI include path (src/ui/egg_assets.h).
        self.assertIn("$(BUILD_DIR)/easter_egg_sound.o:", makefile)
        self.assertIn("RT_NK_UI_FLAGS", makefile)

    def test_the_selftest_target_exists(self) -> None:
        makefile = read("Makefile")
        self.assertIn("egg-selftest:", makefile)
        self.assertIn("tests/native/easter_egg_selftest.c", makefile)


if __name__ == "__main__":
    unittest.main()
