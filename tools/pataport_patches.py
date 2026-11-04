# SPDX-License-Identifier: GPL-3.0-or-later
"""Pataport's changes to a game's code, made before it is translated (codegen --pataport-patch).

patapon3-60fps: "60 FPS Patch v2" for Patapon 3 (all regions), published by Madwig on the PPSSPP
forum (https://forums.ppsspp.org/showthread.php?pid=132462) as a CWCheat code. It turns six
instructions into nops: the frame limiter's loop, which waits until two vblanks have passed since
the last frame, and branches in the game's timing code so that it follows the shorter frame. In
Pataport on an M4 a battle then runs at 60 fps, with the mission timer and the drum combo
advancing exactly as at 30 fps; its author notes unit movement is "a bit off".

Each word names the kind of instruction it must replace. If any differs (another build of the
game), the patch is skipped whole and the game translates unchanged.
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
}


def kind(word):
    """The mnemonic of the few instruction kinds the patches name."""
    op = word >> 26
    if op == 0:
        return "addu" if word & 0x3F == 0x21 else None
    return {4: "beq", 5: "bne", 9: "addiu", 0x14: "beql", 0x15: "bnel"}.get(op)


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
    for addr, want in words:
        raw = elf.read_at_vaddr(addr, 4)
        word = struct.unpack("<I", raw)[0] if raw and len(raw) == 4 else None
        if word is None or kind(word) != want:
            log(f"pataport: {name} not applied: 0x{addr:08x} is not the expected {want}; "
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
    print("pataport_patches selftest: OK")
