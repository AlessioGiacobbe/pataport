# SPDX-License-Identifier: GPL-3.0-or-later
"""Pataport's changes to a game's code, made before it is translated (codegen --pataport-patch).

patapon3-60fps: "60 FPS Patch v2" for Patapon 3 (all regions), published by Madwig on the PPSSPP
forum (https://forums.ppsspp.org/showthread.php?pid=132462) as a CWCheat code. It turns six
instructions into nops: the frame limiter's loop, which waits until two vblanks have passed since
the last frame, and branches in the game's timing code so that it follows the shorter frame. In
Pataport on an M4 a battle then runs at 60 fps, with the mission timer and the drum combo
advancing exactly as at 30 fps; its author notes unit movement is "a bit off".

patapon3-pcsettings: the Game System settings page's "Installation data" row becomes Pataport's
PC settings row (the row's strings are relabeled by tools/pataport_data_patches.py, and the
runtime bridges the toggled byte to the host). The byte is the game's persisted install-data
preference: on the next title-flow pass one call hands it to the VFS layer, which then prefers
the installed copy of DATAMS.BND over the disc. The nop detaches that call, so the byte keeps
its row, its save persistence and nothing else. Without the nop the repurposed row would
silently switch where the game reads its install data.

Each word names the kind of instruction it must replace (a jal also names the callee it must
call). If any differs (another build of the game), the patch is skipped whole and the game
translates unchanged.
"""
import struct

NOP = 0x00000000

PATCHES = {
    "patapon3-60fps": (
        (0x0883E754, "bne"),     # frame limiter: repeat the vblank wait (now: wait once)
        (0x0883D5CC, "bnel"),    # timing: frame-step branches
        (0x0883D5D0, "addiu"),   #   its delay slot
        (0x0883D5DC, "beq"),
        (0x0883D66C, "beql"),
        (0x0883D670, "addu"),    #   its delay slot
    ),
    "patapon3-pcsettings": (
        # FUN_08850f28: hand the install-data flag to the VFS (FUN_0884962c writes
        # vfs+0x14b3, the DATAMS-installed-copy preference).
        (0x08850F54, "jal", 0x0884962C),
    ),
}

# The PC settings row's text, patched where the game actually reads it: the
# game's localize text is loaded from the encrypted DATAMS.BND and decrypted
# into guest RAM, so the rewrite happens in RAM after that load (verified
# against these exact stock bytes before each write). The stock bytes are
# facts about the patch; the replacements are this project's own text. Each
# replacement must not be longer than the stock string it replaces (the write
# is in-place and NUL-terminated).
def _u16(text):
    return text.encode("utf-16-le") + b"\x00\x00"

# The Game System page row's two strings as the guest's RAM carries them once
# the game has loaded and decrypted its own text archive (DATA_CMN's twin of
# these strings rides the sr_enhancement_file_patch_register data patch), and
# what the row should say instead. The stock bytes must match exactly: the
# rewrite is fail-closed and writes in place, NUL-terminated, so each
# replacement must be no longer than the stock string it replaces. The game's
# line break in these strings is the two characters backslash and n.
PCSETTINGS_TEXT = (
    (_u16("・Installation data"), _u16("・Quality")),
    (_u16("Choose whether to use\\ninstallation data."),
     _u16("Sharpens the 3D view.\\nUp to four times.")),
)

# Where the PC settings bridge finds its byte: the config-manager pointer's
# address, the System Data record pointer's offset in it, the row byte's offset
# in the record, and the main module's load base (the guest-RAM plausibility
# floor). The generated code hands these to sr_pcsettings_register_chain when
# the patch applies; they live here because they are facts about the patch.
PCSETTINGS_CHAIN = (0x08AABD94, 0x48, 0x20, 0x08804000)


def kind(word):
    """The mnemonic of the few instruction kinds the patches name."""
    op = word >> 26
    if op == 0:
        return "addu" if word & 0x3F == 0x21 else None
    if op == 3:
        return "jal"
    return {4: "beq", 5: "bne", 9: "addiu", 0x14: "beql", 0x15: "bnel"}.get(op)


def _jal_target(addr, word):
    """The callee of a jal at addr (its 26-bit index, shifted, in the fetch page)."""
    return ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def _write_word(elf, addr, word):
    """Write one word where Elf.read_at_vaddr reads it: the relocated image of a PRX, or the
    PT_LOAD bytes of a fixed-address executable (Patapon 3's EBOOT)."""
    data = struct.pack("<I", word)
    if elf.reloc is not None:
        o = addr - elf.reloc.lo
        elf.reloc.mem[o:o + 4] = data
        return
    for seg in elf.segments:
        if seg["type"] == 1 and seg["vaddr"] <= addr < seg["vaddr"] + seg["filesz"]:
            if not isinstance(elf.data, bytearray):
                elf.data = bytearray(elf.data)
            o = seg["off"] + addr - seg["vaddr"]
            elf.data[o:o + 4] = data
            return
    raise ValueError(f"0x{addr:08x} is not in a loaded segment")


def apply(elf, name, log):
    """Apply patch `name` to a loaded Elf (analyze.Elf) in place, before any analysis. Returns the
    number of words changed: all of them, or 0 when the code is not the build the patch was made for."""
    words = PATCHES[name]
    found = []
    for entry in words:
        addr, want, *extra = entry
        raw = elf.read_at_vaddr(addr, 4)
        word = struct.unpack("<I", raw)[0] if raw and len(raw) == 4 else None
        if word is None or kind(word) != want:
            log(f"pataport: {name} not applied: 0x{addr:08x} is not the expected {want}; "
                "this build of the game translates unchanged")
            return 0
        if extra and _jal_target(addr, word) != extra[0]:
            log(f"pataport: {name} not applied: 0x{addr:08x} does not call 0x{extra[0]:08x}; "
                "this build of the game translates unchanged")
            return 0
        found.append(addr)
    for addr in found:
        _write_word(elf, addr, NOP)
    log(f"pataport: {name} applied ({len(found)} instructions)")
    return len(found)


if __name__ == "__main__":
    # Self-check on a synthetic image: a matching build is patched, any other is left alone.
    class Reloc:
        lo = 0x0883D000
        mem = bytearray(0x2000)

    class FakeElf:
        reloc = Reloc()

        def read_at_vaddr(self, a, n):
            o = a - self.reloc.lo
            return bytes(self.reloc.mem[o:o + n])

    elf = FakeElf()
    enc = {"bne": 5 << 26, "bnel": 0x15 << 26, "addiu": 9 << 26, "beq": 4 << 26,
           "beql": 0x14 << 26, "addu": 0x21}
    for a, k in PATCHES["patapon3-60fps"]:
        struct.pack_into("<I", elf.reloc.mem, a - Reloc.lo, enc[k] | 0x1234 if k != "addu" else enc[k])
    other = FakeElf()
    other.reloc = type("R", (), {"lo": Reloc.lo, "mem": bytearray(elf.reloc.mem)})()
    struct.pack_into("<I", other.reloc.mem, 0x0883D5DC - Reloc.lo, 9 << 26)   # not a beq
    before = bytes(other.reloc.mem)
    assert apply(other, "patapon3-60fps", lambda m: None) == 0
    assert bytes(other.reloc.mem) == before                                    # untouched
    assert apply(elf, "patapon3-60fps", lambda m: None) == 6
    assert all(elf.read_at_vaddr(a, 4) == b"\0\0\0\0" for a, _ in PATCHES["patapon3-60fps"])
    # The fixed-address form (no relocation): the words live in the PT_LOAD bytes.
    class FixedElf:
        reloc = None
        segments = [{"type": 1, "vaddr": Reloc.lo, "filesz": 0x2000, "off": 0x100}]

        def __init__(self, image):
            self.data = bytes(0x100) + image

        def read_at_vaddr(self, a, n):
            o = 0x100 + a - Reloc.lo
            return bytes(self.data[o:o + n])

    struct.pack_into("<I", other.reloc.mem, 0x0883D5DC - Reloc.lo, 4 << 26)  # a beq again
    fixed = FixedElf(bytes(other.reloc.mem))
    assert apply(fixed, "patapon3-60fps", lambda m: None) == 6
    assert all(fixed.read_at_vaddr(a, 4) == b"\0\0\0\0" for a, _ in PATCHES["patapon3-60fps"])
    # The pcsettings patch: a jal whose callee is verified before it is nop'd.
    class Window:
        def __init__(self, lo):
            self.lo = lo
            self.mem = bytearray(0x2000)

    def encode_jal(addr, target):
        assert (addr + 4) & 0xF0000000 == target & 0xF0000000
        return (3 << 26) | ((target >> 2) & 0x03FFFFFF)

    pc = Window(0x08850000)
    struct.pack_into("<I", pc.mem, 0x08850F54 - pc.lo, encode_jal(0x08850F54, 0x0884962C))
    pc_elf = FakeElf()
    pc_elf.reloc = pc
    assert apply(pc_elf, "patapon3-pcsettings", lambda m: None) == 1
    assert pc_elf.read_at_vaddr(0x08850F54, 4) == b"\0\0\0\0"
    pc2 = Window(0x08850000)
    struct.pack_into("<I", pc2.mem, 0x08850F54 - pc2.lo, encode_jal(0x08850F54, 0x08849630))
    pc2_elf = FakeElf()
    pc2_elf.reloc = pc2
    before = bytes(pc2.mem)
    assert apply(pc2_elf, "patapon3-pcsettings", lambda m: None) == 0   # wrong callee
    assert bytes(pc2.mem) == before
    print("pataport_patches selftest: OK")
